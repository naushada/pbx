#include "microservice_pbx.hpp"

#include "http_parser.hpp"
#include "json.hpp"
#include "mongodbc.hpp"
#include "webservice.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {

// ── Small response helpers ────────────────────────────────────────────────────

// `extra_headers`, when non-empty, must be complete CRLF-terminated header
// line(s) — they are emitted verbatim after Content-Type (e.g. `Set-Cookie`).
std::string http_response(int code, const std::string &reason,
                          const std::string &body,
                          const std::string &content_type = "application/json",
                          const std::string &extra_headers = {}) {
  std::ostringstream os;
  os << "HTTP/1.1 " << code << " " << reason << "\r\n"
     << "Connection: keep-alive\r\n"
     << "Access-Control-Allow-Origin: *\r\n"
     << "Content-Length: " << body.size() << "\r\n"
     << "Content-Type: " << content_type << "\r\n"
     << extra_headers
     << "\r\n"
     << body;
  return os.str();
}

std::string response_error(int code, const std::string &reason,
                            const std::string &cause) {
  const std::string body =
      R"({"status":"failure","error":)" + std::to_string(code) +
      R"(,"cause":")" + cause + R"("})";
  return http_response(code, reason, body);
}

// ── Cryptographic helpers ─────────────────────────────────────────────────────

std::string hex_encode(const unsigned char *bytes, std::size_t n) {
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < n; ++i)
    os << std::setw(2) << static_cast<unsigned>(bytes[i]);
  return os.str();
}

std::string md5_hex(const std::string &in) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;

  EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
  EVP_DigestUpdate(ctx, in.data(), in.size());
  EVP_DigestFinal_ex(ctx, digest, &digest_len);
  EVP_MD_CTX_free(ctx);

  return hex_encode(digest, digest_len);
}

std::string random_alnum(std::size_t n) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string out;
  out.reserve(n);
  std::vector<unsigned char> buf(n);
  if (RAND_bytes(buf.data(), static_cast<int>(n)) != 1) {
    // Extremely unlikely; fall back to std::random_device for resilience.
    std::random_device rd;
    for (auto &b : buf) b = static_cast<unsigned char>(rd());
  }
  for (std::size_t i = 0; i < n; ++i)
    out.push_back(alphabet[buf[i] % (sizeof(alphabet) - 1)]);
  return out;
}

std::string compute_sip_ha1(const std::string &username,
                             const std::string &realm,
                             const std::string &password) {
  return md5_hex(username + ":" + realm + ":" + password);
}

// ── Query-string + body helpers ───────────────────────────────────────────────

// Pull a query-string param from an HTTP URI part like "/api/v1/cdr?societyId=X".
// Returns empty string if absent.
std::string query_param(const std::string &uri_with_query, const std::string &key) {
  const auto q = uri_with_query.find('?');
  if (q == std::string::npos) return {};
  std::string qs = uri_with_query.substr(q + 1);

  std::size_t start = 0;
  while (start < qs.size()) {
    const auto amp = qs.find('&', start);
    const auto pair = qs.substr(start, (amp == std::string::npos)
                                            ? std::string::npos
                                            : amp - start);
    const auto eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key)
      return pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    start = amp + 1;
  }
  return {};
}

// Reconstruct the request line's URI (including query string), which `Http`'s
// `uri()` strips. We re-derive it by scanning the raw request's first line.
std::string raw_uri_with_query(const std::string &req) {
  const auto end = req.find("\r\n");
  if (end == std::string::npos) return {};
  const std::string line = req.substr(0, end);
  const auto sp1 = line.find(' ');
  if (sp1 == std::string::npos) return {};
  const auto sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return {};
  return line.substr(sp1 + 1, sp2 - sp1 - 1);
}

// Trailing path segment after a fixed prefix, query string stripped.
// e.g. ("PUT /api/v1/subscriber/u_abc?societyId=s1 …", "/api/v1/subscriber/")
//      → "u_abc". Empty if the prefix doesn't match or nothing follows it.
std::string path_suffix(const std::string &req, const std::string &prefix) {
  std::string uri = raw_uri_with_query(req);
  const auto q = uri.find('?');
  if (q != std::string::npos) uri.resize(q);
  if (uri.size() <= prefix.size() ||
      uri.compare(0, prefix.size(), prefix) != 0)
    return {};
  return uri.substr(prefix.size());
}

// Minimal CSV row split. Trims trailing \r. Does not handle quoted fields —
// not needed for the import format (`flat_number,name,email,phone,role`).
std::vector<std::string> split_csv_row(std::string line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const auto comma = line.find(',', start);
    fields.push_back(line.substr(start, comma == std::string::npos
                                              ? std::string::npos
                                              : comma - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return fields;
}

// CSV-escape a single field (quote if it contains comma, quote, or newline).
std::string csv_escape(const std::string &field) {
  if (field.find_first_of(",\"\r\n") == std::string::npos) return field;
  std::string out = "\"";
  for (char c : field) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += "\"";
  return out;
}

// Cookie header parser: pull session=<value>. Returns empty string if missing.
std::string cookie_value(const std::string &cookie_header,
                         const std::string &key) {
  std::size_t pos = 0;
  while (pos < cookie_header.size()) {
    const auto semi = cookie_header.find(';', pos);
    std::string pair = cookie_header.substr(pos, (semi == std::string::npos)
                                                      ? std::string::npos
                                                      : semi - pos);
    // Trim leading whitespace.
    auto first = pair.find_first_not_of(" \t");
    if (first != std::string::npos) pair = pair.substr(first);
    const auto eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key)
      return pair.substr(eq + 1);
    if (semi == std::string::npos) break;
    pos = semi + 1;
  }
  return {};
}

// Pull the portal session bearer out of a request — UI prefers the
// `?token=` query param (browsers can't set headers on `new WebSocket`),
// but the `session=` cookie also works for plain HTTP. Same precedence
// as handle_sipws_upgrade.
std::string extract_session_token(const std::string &req) {
  std::string tok = query_param(raw_uri_with_query(req), "token");
  if (!tok.empty()) return tok;
  Http parsed(req);
  return cookie_value(parsed.get_element("Cookie"), "session");
}

// ── env-var helpers (shared by every handler) ─────────────────────────────────

std::string env_or(const char *name, const std::string &fallback = {}) {
  const char *v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

// `db.get_documents()` blocks forever waiting for a Mongo connection
// when DB_URI / REMOTE_DB are both unset (Heroku H12 timeout). Skip
// the DB call entirely in that case and return an empty result.
bool db_available() {
  return !env_or("DB_URI").empty() || !env_or("REMOTE_DB").empty();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// MicroServicePbx — public handlers
// ═══════════════════════════════════════════════════════════════════════════════

namespace MicroServicePbx {

std::string handle_society_POST(const std::string &req, IMongodbClient &db) {
  Http parsed(req);

  json body;
  try {
    body = json::parse(parsed.body());
  } catch (...) {
    return response_error(400, "Bad Request", "Invalid JSON body");
  }

  for (const char *required : {"name", "code"}) {
    if (!body.contains(required) || !body[required].is_string() ||
        body[required].get<std::string>().empty())
      return response_error(400, "Bad Request",
                            std::string("Missing field: ") + required);
  }

  const std::string code = body["code"].get<std::string>();

  // Duplicate-code check.
  const std::string existing =
      db.get_document("societies",
                       R"({"code":")" + code + R"("})",
                       "{}");
  if (!existing.empty())
    return response_error(409, "Conflict", "Society code already exists");

  // Build the canonical society document. The cloud derives sipRealm +
  // turnSharedSecret here so admins never have to enter them; both are
  // returned in the response so the installer can copy them into the agent
  // pjsip.conf / turnserver.conf on the on-prem box.
  json doc = body;
  doc["sipRealm"]            = code + ".pbx.local";
  doc["turnSharedSecret"]    = random_alnum(32);
  doc["maxConcurrentCalls"]  = 5;
  doc["ringTimeoutSec"]      = 30;

  const std::string inserted =
      db.create_document(db.get_database(), "societies", doc.dump());

  // Surface either the DB-returned doc (if non-empty) or our prepared doc.
  return http_response(201, "Created",
                        inserted.empty() ? doc.dump() : inserted);
}

std::string handle_subscriber_import_POST(const std::string &req,
                                          IMongodbClient &db) {
  Http parsed(req);

  const std::string society_id = query_param(raw_uri_with_query(req), "societyId");
  if (society_id.empty())
    return response_error(400, "Bad Request",
                          "Missing required query param: societyId");

  // Look up the society to obtain its SIP realm (needed for HA1 derivation).
  const std::string society_doc =
      db.get_document("societies",
                       R"({"_id":")" + society_id + R"("})",
                       "{}");
  if (society_doc.empty())
    return response_error(404, "Not Found", "Unknown societyId");

  std::string realm;
  try {
    realm = json::parse(society_doc).at("sipRealm").get<std::string>();
  } catch (...) {
    return response_error(500, "Internal Server Error",
                          "Society document missing sipRealm");
  }

  // Parse the CSV body.
  const std::string &csv = parsed.body();
  std::vector<std::vector<std::string>> rows;
  std::istringstream iss(csv);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty() || (line.size() == 1 && line[0] == '\r')) continue;
    rows.push_back(split_csv_row(line));
  }
  if (rows.size() < 2) // header + at least one row
    return response_error(400, "Bad Request",
                          "CSV must contain a header row and at least one data row");

  // Expect header: flat_number,name,email,phone,role.
  const std::vector<std::string> &header = rows[0];
  auto index_of = [&](const std::string &col) -> int {
    for (std::size_t i = 0; i < header.size(); ++i)
      if (header[i] == col) return static_cast<int>(i);
    return -1;
  };
  const int i_flat = index_of("flat_number");
  const int i_name = index_of("name");
  const int i_email = index_of("email");
  const int i_phone = index_of("phone");
  const int i_role = index_of("role");
  if (i_flat < 0 || i_name < 0 || i_email < 0)
    return response_error(400, "Bad Request",
                          "CSV header must include flat_number, name, email");

  std::ostringstream out_csv;
  out_csv << "flat_number,name,email,sipUsername,sipPassword,portalPassword,status\r\n";

  int created = 0, skipped = 0;
  for (std::size_t r = 1; r < rows.size(); ++r) {
    const auto &row = rows[r];
    auto get = [&](int idx) -> std::string {
      return (idx >= 0 && idx < static_cast<int>(row.size())) ? row[idx] : std::string{};
    };
    const std::string flat_no = get(i_flat);
    const std::string name    = get(i_name);
    const std::string email   = get(i_email);
    const std::string phone   = get(i_phone);
    const std::string role    = get(i_role).empty() ? "resident" : get(i_role);

    if (flat_no.empty() || name.empty() || email.empty())
      return response_error(400, "Bad Request",
                            "Row " + std::to_string(r) + ": missing required field");

    // Validate the flat exists (only required for residents). Guards/admins
    // are allowed without a flat (`role=guard` / `role=admin`).
    std::string flat_id;
    if (role == "resident") {
      const std::string flat_doc =
          db.get_document("flats",
                           R"({"societyId":")" + society_id +
                               R"(","number":")" + flat_no + R"("})",
                           "{}");
      if (flat_doc.empty())
        return response_error(400, "Bad Request",
                              "Row " + std::to_string(r) +
                                  ": unknown flat_number '" + flat_no + "'");
      try {
        flat_id = json::parse(flat_doc).at("_id").get<std::string>();
      } catch (...) { /* leave flat_id empty if shape is unexpected */ }
    }

    // Idempotency: skip if a subscriber with this (societyId, email) exists.
    const std::string existing =
        db.get_document("subscribers",
                         R"({"societyId":")" + society_id +
                             R"(","email":")" + email + R"("})",
                         "{}");
    if (!existing.empty()) {
      out_csv << csv_escape(flat_no) << ',' << csv_escape(name) << ','
              << csv_escape(email) << ",,,,skipped\r\n";
      ++skipped;
      continue;
    }

    const std::string sip_username = "u_" + random_alnum(10);
    const std::string sip_password = random_alnum(16);
    const std::string portal_password = random_alnum(16);
    const std::string sip_ha1 = compute_sip_ha1(sip_username, realm, sip_password);
    const std::string portal_hash =
        MongodbClient::hash_password(portal_password);

    json subscriber = {
        {"societyId",          society_id},
        {"flatId",             flat_id},
        // Denormalized human flat string ("A-204") alongside the `flatId`
        // foreign key — the directory filters/displays on it and the UI
        // dials by it. Same human-string-copy pattern as `cdr.fromFlat`
        // (DESIGN.md §4). `flat_no` is a required CSV column so this is
        // always set; only `flatId` is empty for non-residents (the flat
        // is validated + resolved to an id for residents only).
        {"flatNumber",         flat_no},
        {"name",               name},
        {"email",              email},
        {"phone",              phone},
        {"role",               role},
        {"status",             "active"},
        {"autoAnswer",         false},
        {"sipUsername",        sip_username},
        {"sipHa1",             sip_ha1},
        {"portalPasswordHash", portal_hash},
    };
    db.create_document(db.get_database(), "subscribers", subscriber.dump());

    out_csv << csv_escape(flat_no) << ',' << csv_escape(name) << ','
            << csv_escape(email) << ',' << sip_username << ',' << sip_password
            << ',' << portal_password << ",created\r\n";
    ++created;
  }

  std::string summary_header =
      "X-Import-Created: " + std::to_string(created) + "\r\n" +
      "X-Import-Skipped: " + std::to_string(skipped) + "\r\n";

  const std::string body = out_csv.str();
  std::ostringstream os;
  os << "HTTP/1.1 200 OK\r\n"
     << "Connection: keep-alive\r\n"
     << "Access-Control-Allow-Origin: *\r\n"
     << summary_header
     << "Content-Length: " << body.size() << "\r\n"
     << "Content-Type: text/csv\r\n"
     << "Content-Disposition: attachment; filename=\"subscriber_credentials.csv\"\r\n"
     << "\r\n"
     << body;
  return os.str();
}

std::string handle_cdr_GET(const std::string &req, IMongodbClient &db) {
  const std::string uri = raw_uri_with_query(req);
  const std::string society_id = query_param(uri, "societyId");
  if (society_id.empty())
    return response_error(400, "Bad Request",
                          "Missing required query param: societyId");

  if (!db_available()) return http_response(200, "OK", "[]");

  std::string result;
  try {
    result = db.get_documents("cdr",
                              R"({"societyId":")" + society_id + R"("})",
                              "{}");
  } catch (...) {
    return http_response(200, "OK", "[]");
  }

  return http_response(200, "OK", result.empty() ? "[]" : result);
}

std::string handle_push_subscribe_POST(const std::string &req,
                                       IMongodbClient &db) {
  Http parsed(req);

  json body;
  try {
    body = json::parse(parsed.body());
  } catch (...) {
    return response_error(400, "Bad Request", "Invalid JSON body");
  }

  for (const char *required : {"subscriberId", "endpoint", "p256dh", "auth"}) {
    if (!body.contains(required) || !body[required].is_string() ||
        body[required].get<std::string>().empty())
      return response_error(400, "Bad Request",
                            std::string("Missing field: ") + required);
  }

  if (!db_available())
    return response_error(503, "Service Unavailable",
                          "Push subscriptions store unavailable (no DB configured)");

  try {
    db.create_document(db.get_database(), "push_subscriptions", body.dump());
  } catch (...) {
    return response_error(503, "Service Unavailable",
                          "Push subscriptions store unavailable");
  }
  return http_response(201, "Created", body.dump());
}

// ── Slice (REST-routing fix): login, directory, vapid, turn-creds ────────────
//
// These four endpoints unblock the UI's end-to-end flow. Each ships in a
// minimal-but-correct form so the live deployment behaves as advertised in
// /webui/. Production-grade auth + DB-backed directory come next.

namespace {

// env_or() + db_available() live in the file-level anonymous namespace
// above so they're visible to every handler (including the legacy ones).

std::string base64_encode(const std::string &raw) {
  // OpenSSL EVP_EncodeBlock writes exactly 4 * ceil(n/3) bytes plus a NUL.
  const std::size_t out_len = 4 * ((raw.size() + 2) / 3);
  std::vector<unsigned char> out(out_len + 1, 0);
  const int n = ::EVP_EncodeBlock(
      out.data(),
      reinterpret_cast<const unsigned char *>(raw.data()),
      static_cast<int>(raw.size()));
  return std::string(reinterpret_cast<char *>(out.data()),
                     static_cast<std::size_t>(n));
}

std::string hmac_sha1(const std::string &key, const std::string &msg) {
  unsigned int len = 0;
  unsigned char mac[EVP_MAX_MD_SIZE];
  ::HMAC(::EVP_sha1(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(msg.data()),
         msg.size(), mac, &len);
  return std::string(reinterpret_cast<char *>(mac), len);
}

} // namespace

namespace {

// Portal session lifetime. The /sip-ws upgrade rejects a session whose
// `expiresAt` is in the past (see handle_sipws_upgrade). For active
// sessions, /api/v1/ping and /sip-ws upgrade slide `expiresAt` forward
// — these constants are the floor when there's no activity (e.g. a
// kiosk that's been powered off).
constexpr long kSessionTtlSec      = 24 * 60 * 60;        // residents: 1 day
constexpr long kSessionTtlGuardSec = 7 * 24 * 60 * 60;    // guards: 1 week

// Guard kiosks can sit idle through long weekends without a person
// touching the UI; resident tabs that close are expected to log in
// fresh. Both classes still expand by sliding refresh while active.
long ttl_sec_for_role(const std::string &role) {
  return (role == "guard") ? kSessionTtlGuardSec : kSessionTtlSec;
}

// Best-effort sliding refresh: bump `expiresAt` on the session matching
// @p token, using the role-appropriate TTL. Silent on every failure —
// the caller (handle_ping_GET / handle_sipws_upgrade) MUST NOT fail its
// response just because a refresh write didn't land. Skips:
//   - empty token (anonymous request)
//   - unknown token (no row)
//   - already-expired session (don't resurrect — the TTL index will
//     evict it; a new login is the right path)
void refresh_session_expiry(IMongodbClient &db, const std::string &token) {
  if (token.empty()) return;
  const json filter = {{"token", token}};
  std::string record;
  try { record = db.get_document("sessions", filter.dump(), "{}"); }
  catch (...) { return; }
  if (record.empty()) return;

  json session;
  try { session = json::parse(record); } catch (...) { return; }

  const long now = static_cast<long>(::time(nullptr));
  if (!session.contains("expiresAt") || !session["expiresAt"].is_number() ||
      session["expiresAt"].get<long>() <= now)
    return; // don't resurrect

  const long ttl =
      ttl_sec_for_role(session.value("role", std::string{"resident"}));
  const json update = {{"$set", {{"expiresAt", now + ttl}}}};
  try { db.update_collection("sessions", filter.dump(), update.dump()); }
  catch (...) { /* swallowed — best effort */ }
}

// Mint a 16-byte random bearer token, hex-encoded → 32 chars. Returns
// empty on RAND_bytes failure (caller turns that into a 500).
std::string mint_bearer_token() {
  unsigned char token_bytes[16];
  if (RAND_bytes(token_bytes, sizeof(token_bytes)) != 1) return {};
  return hex_encode(token_bytes, sizeof(token_bytes));
}

// Persist a portal session and build the 200 login response. The token is
// returned three ways: as the `sessions` doc key, as a `Set-Cookie` header,
// and in the JSON body — the UI needs the raw value for the /sip-ws
// `?token=` query param (browsers can't set headers on `new WebSocket`).
std::string finish_login(IMongodbClient &db, const json &subscriber,
                         const std::string &email,
                         const std::string &society_id,
                         const std::string &sip_username,
                         const std::string &role) {
  const std::string token = mint_bearer_token();
  if (token.empty())
    return response_error(500, "Internal Server Error", "RAND_bytes failed");

  const long now = static_cast<long>(::time(nullptr));
  const long ttl = ttl_sec_for_role(role);
  // `token` is a plain field, not `_id`: MongodbClient::create_document
  // assumes an ObjectId `_id` in its return path. A unique index on
  // `sessions.token` is expected (same convention as the other collections).
  const json session = {
      {"token",       token},
      {"email",       email},
      {"societyId",   society_id},
      {"sipUsername", sip_username},
      {"role",        role},
      {"createdAt",   now},
      {"expiresAt",   now + ttl},
  };
  db.create_document(db.get_database(), "sessions", session.dump());

  const std::string set_cookie =
      "Set-Cookie: session=" + token +
      "; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=" +
      std::to_string(ttl) + "\r\n";
  const json rsp = {{"token", token}, {"subscriber", subscriber}};
  return http_response(200, "OK", rsp.dump(), "application/json", set_cookie);
}

} // namespace

std::string handle_subscriber_login_POST(const std::string &req,
                                         IMongodbClient &db) {
  Http parsed(req);

  json body;
  try { body = json::parse(parsed.body()); }
  catch (...) {
    return response_error(400, "Bad Request", "Invalid JSON body");
  }

  for (const char *required : {"email", "password"}) {
    if (!body.contains(required) || !body[required].is_string() ||
        body[required].get<std::string>().empty()) {
      return response_error(400, "Bad Request",
                            std::string("Missing field: ") + required);
    }
  }

  const std::string email    = body["email"].get<std::string>();
  const std::string password = body["password"].get<std::string>();

  // Strict mode: look the subscriber up by email (globally unique —
  // DESIGN.md §5) and verify the password against the stored bcrypt hash.
  // Dev mode (default) keeps the seed-free deploy working by synthesising
  // a profile from the email — set PBX_AUTH_STRICT=1 once the CSV-import
  // flow has populated the `subscribers` collection.
  if (env_or("PBX_AUTH_STRICT") == "1") {
    const json query = {{"email", email}};
    const std::string record =
        db.get_document("subscribers", query.dump(), "{}");
    if (record.empty())
      return response_error(401, "Unauthorized", "Invalid credentials");

    json subscriber;
    try { subscriber = json::parse(record); }
    catch (...) {
      return response_error(500, "Internal Server Error",
                            "subscriber record is not valid JSON");
    }

    // Verify against the bcrypt portalPasswordHash. A missing/!string hash
    // fails closed — we never authenticate without a hash to check.
    if (!subscriber.contains("portalPasswordHash") ||
        !subscriber["portalPasswordHash"].is_string() ||
        !MongodbClient::verify_password(
            password, subscriber["portalPasswordHash"].get<std::string>())) {
      return response_error(401, "Unauthorized", "Invalid credentials");
    }

    // An explicitly disabled subscriber can authenticate but not log in.
    if (subscriber.contains("status") && subscriber["status"].is_string() &&
        subscriber["status"].get<std::string>() != "active") {
      return response_error(403, "Forbidden", "Account disabled");
    }

    // Capture the identity fields for the session BEFORE stripping secrets.
    const std::string society_id =
        subscriber.value("societyId", std::string{});
    const std::string sip_username =
        subscriber.value("sipUsername", std::string{});
    const std::string role =
        subscriber.value("role", std::string{"resident"});

    // Never return the secrets: the bcrypt portal hash and the SIP digest
    // credential (sipHa1) must not leave the server.
    subscriber.erase("portalPasswordHash");
    subscriber.erase("sipHa1");

    return finish_login(db, subscriber, email, society_id, sip_username, role);
  }

  // Dev mode: accept any non-empty {email, password}, synthesise a profile.
  // The session is persisted just like strict mode so the /sip-ws upgrade
  // path stays mode-agnostic — it always resolves the token via `sessions`.
  const json subscriber = {
      {"email",       email},
      {"displayName", email},
      {"role",        "resident"},
  };
  return finish_login(db, subscriber, email, /*society_id=*/"dev",
                      /*sip_username=*/email, /*role=*/"resident");
}

std::string handle_directory_GET(const std::string &req, IMongodbClient &db) {
  const std::string uri          = raw_uri_with_query(req);
  const std::string society_id   = query_param(uri, "societyId");
  const std::string flat_prefix  = query_param(uri, "flatPrefix");
  if (society_id.empty()) {
    return response_error(400, "Bad Request",
                          "Missing required query param: societyId");
  }

  if (!db_available()) return http_response(200, "OK", "[]");

  // Mongo regex would be ideal but get_documents takes a plain JSON filter
  // here; do a society-scoped fetch, then filter by prefix client-side.
  std::string result;
  try {
    result = db.get_documents(
        "subscribers",
        R"({"societyId":")" + society_id + R"("})",
        "{}");
  } catch (...) {
    return http_response(200, "OK", "[]");
  }

  // No rows → return [] rather than the Mongo "" so the UI's typed
  // response shape is honoured.
  if (result.empty()) return http_response(200, "OK", "[]");

  json arr;
  try { arr = json::parse(result); } catch (...) { arr = json::array(); }
  if (!arr.is_array()) arr = json::array();

  // Project every row to the UI's `DirectoryEntry` shape
  // (ui/src/common/app-globals.ts) — `{flatNumber, displayName, sipUri,
  // online}`. The persisted `subscribers` doc uses different field names
  // (`name` vs `displayName`, `sipUsername` vs `sipUri`) and carries
  // server-only secrets (`sipHa1`, `portalPasswordHash`) the directory
  // must never leak; building a fresh object with only the four
  // DirectoryEntry fields renames AND strips in one pass.
  //
  // The SIP URI form matches what `SipService.placeCall` uses on the UI
  // (`sip:<flatNumber>@pbx.<societyId>`); `online` (last-known REGISTER
  // state) isn't tracked cloud-side yet — emitted as `false`, a future
  // item populates it from agent-reported REGISTER/UNREGISTER frames.
  const auto matches_prefix = [&](const std::string &flat) -> bool {
    if (flat_prefix.empty()) return true;
    if (flat.size() < flat_prefix.size()) return false;
    return std::equal(flat_prefix.begin(), flat_prefix.end(), flat.begin(),
                      [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                      });
  };

  json out = json::array();
  for (const auto &row : arr) {
    if (!row.is_object()) continue;
    if (!row.contains("flatNumber") || !row["flatNumber"].is_string())
      continue;
    const std::string flat = row["flatNumber"].get<std::string>();
    if (!matches_prefix(flat)) continue;

    out.push_back({
        {"flatNumber",  flat},
        {"displayName", row.value("name", std::string{})},
        {"sipUri",      "sip:" + flat + "@pbx." + society_id},
        {"online",      false},
    });
  }
  return http_response(200, "OK", out.dump());
}

namespace {

// Both lifecycle handlers address a subscriber by sipUsername-in-path +
// societyId-in-query, and both must confirm the row exists first. This
// pulls (sipUsername, societyId) out of @p req and the existence check
// into one place. On any problem it fills @p err with the ready HTTP
// error response and returns false.
bool resolve_lifecycle_target(const std::string &req, IMongodbClient &db,
                              std::string &sip_username,
                              std::string &society_id, std::string &filter,
                              std::string &err) {
  sip_username = path_suffix(req, "/api/v1/subscriber/");
  if (sip_username.empty()) {
    err = response_error(400, "Bad Request",
                         "Missing subscriber sipUsername in path");
    return false;
  }
  society_id = query_param(raw_uri_with_query(req), "societyId");
  if (society_id.empty()) {
    err = response_error(400, "Bad Request",
                         "Missing required query param: societyId");
    return false;
  }
  // Society-scoped: an admin of one society can't touch another's rows.
  filter = json{{"societyId", society_id},
                {"sipUsername", sip_username}}.dump();
  if (db.get_document("subscribers", filter, "{}").empty()) {
    err = response_error(404, "Not Found", "Unknown subscriber");
    return false;
  }
  return true;
}

} // namespace

std::string handle_subscriber_status_PUT(const std::string &req,
                                         IMongodbClient &db,
                                         IRevocationSink *revoke) {
  std::string sip_username, society_id, filter, err;
  if (!resolve_lifecycle_target(req, db, sip_username, society_id, filter, err))
    return err;

  Http parsed(req);
  json body;
  try { body = json::parse(parsed.body()); }
  catch (...) {
    return response_error(400, "Bad Request", "Invalid JSON body");
  }
  if (!body.contains("status") || !body["status"].is_string())
    return response_error(400, "Bad Request", "Missing field: status");
  const std::string status = body["status"].get<std::string>();
  if (status != "active" && status != "disabled")
    return response_error(400, "Bad Request",
                          R"(status must be "active" or "disabled")");

  // Flip subscribers.status for this (societyId, sipUsername) row.
  const json update = {{"$set", {{"status", status}}}};
  db.update_collection("subscribers", filter, update.dump());

  // Disabling revokes; re-enabling ("active") is a plain status flip.
  // Revocation is three guards: the status flip above (caught by the
  // strict-mode /sip-ws gate + login gate), deleting every `sessions`
  // row (so the bearer can't be re-resolved), and signalling the agent
  // to hang up a call that is live right now.
  if (status == "disabled") {
    db.delete_document("sessions", filter);
    if (revoke) revoke->revoke(society_id, sip_username);
  }

  const json rsp = {{"status", "success"},
                    {"sipUsername", sip_username},
                    {"subscriberStatus", status}};
  return http_response(200, "OK", rsp.dump());
}

std::string handle_subscriber_DELETE(const std::string &req,
                                     IMongodbClient &db,
                                     IRevocationSink *revoke) {
  std::string sip_username, society_id, filter, err;
  if (!resolve_lifecycle_target(req, db, sip_username, society_id, filter, err))
    return err;

  // Remove the subscriber doc first, so a /sip-ws upgrade racing this
  // delete re-checks `subscribers` and sees it gone. Then revoke exactly
  // as the disable path does: drop the sessions, cut the live call.
  db.delete_document("subscribers", filter);
  db.delete_document("sessions", filter);
  if (revoke) revoke->revoke(society_id, sip_username);

  const json rsp = {{"status", "success"},
                    {"sipUsername", sip_username},
                    {"removed", true}};
  return http_response(200, "OK", rsp.dump());
}

std::string handle_push_vapid_key_GET(const std::string & /*req*/,
                                      IMongodbClient & /*db*/) {
  const std::string key = env_or("VAPID_PUBLIC_KEY");
  json rsp = {{"key", key}};
  return http_response(200, "OK", rsp.dump());
}

// Lightweight health/keep-alive. The UI hits this every 30s while
// logged in (see ui/src/common/keepalive.service.ts) so the browser can
// detect cloud reachability and so the cloud sees fresh traffic from
// each tab. Doubles as the **sliding-refresh** signal: a best-effort
// `expiresAt` bump on the calling session — keeps an always-on guard
// kiosk from re-logging-in every 24 h. The refresh is fire-and-forget:
// any DB error is swallowed, the ping always returns 200 (a tunnel
// hiccup must not drop a user's UI session — the original "no DB"
// invariant for ping survives in *failure semantics*).
std::string handle_ping_GET(const std::string &req, IMongodbClient &db) {
  refresh_session_expiry(db, extract_session_token(req));

  const long ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  json rsp = {{"ok", true}, {"ts", ts_ms}};
  return http_response(200, "OK", rsp.dump());
}

std::string handle_turn_credentials_GET(const std::string &req,
                                        IMongodbClient & /*db*/) {
  Http parsed(req);
  // The caller's bearer should identify the sip-user but we don't yet
  // validate the bearer end-to-end. For MVP, derive sip-user from the
  // X-PBX-Flat custom header if present, else fall back to "anon".
  const std::string sip_user =
      parsed.get_element("X-PBX-Flat").empty() ? "anon"
                                               : parsed.get_element("X-PBX-Flat");

  const std::string secret  = env_or("TURN_SHARED_SECRET",
                                     "dev-only-shared-secret-override-in-prod");
  const std::string turn_url = env_or("TURN_URL",
                                      "turn:turn.pbx.local:3478?transport=udp");
  const long ttl_sec = 300;
  const long expires =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count() + ttl_sec;

  const std::string username = std::to_string(expires) + ":" + sip_user;
  const std::string credential = base64_encode(hmac_sha1(secret, username));

  json rsp = {
      {"urls",       json::array({turn_url, "stun:" + turn_url.substr(5)})},
      {"username",   username},
      {"credential", credential},
      {"ttlSec",     ttl_sec},
  };
  return http_response(200, "OK", rsp.dump());
}

SipWsUpgrade handle_sipws_upgrade(const std::string &req, IMongodbClient &db) {
  // Browsers can't set arbitrary headers on `new WebSocket(url)`, so the
  // UI passes its bearer via `?token=<value>` (see
  // ui/src/common/sip.service.ts and the comment in sip-ua-sipjs.ts).
  // Accept either the query-string token or a `session=` cookie — both
  // resolve to the same `sessions.token`.
  Http parsed(req);
  std::string token = query_param(raw_uri_with_query(req), "token");
  if (token.empty())
    token = cookie_value(parsed.get_element("Cookie"), "session");

  if (token.empty()) {
    return {response_error(401, "Unauthorized",
                           "SIP-WS upgrade requires ?token=<bearer> or a "
                           "session= cookie tied to a portal login"),
            {}};
  }

  // Resolve the token against the portal `sessions` collection (written by
  // handle_subscriber_login_POST → finish_login).
  const json query = {{"token", token}};
  const std::string record = db.get_document("sessions", query.dump(), "{}");
  if (record.empty()) {
    return {response_error(401, "Unauthorized",
                           "unknown or expired portal session"),
            {}};
  }

  json session;
  try { session = json::parse(record); }
  catch (...) {
    return {response_error(500, "Internal Server Error",
                           "session record is not valid JSON"),
            {}};
  }

  // Reject an expired session. `expiresAt` is the unix timestamp stamped at
  // login; a missing / non-numeric field fails closed.
  const long now = static_cast<long>(::time(nullptr));
  if (!session.contains("expiresAt") || !session["expiresAt"].is_number() ||
      session["expiresAt"].get<long>() <= now) {
    return {response_error(401, "Unauthorized",
                           "unknown or expired portal session"),
            {}};
  }

  // Subscriber-lifecycle gate: a subscriber disabled or removed AFTER this
  // session was minted must not keep /sip-ws for the rest of the 24h TTL.
  // The session row may still be valid (admin disable also deletes it, but
  // a still-open tab can be mid-upgrade) — so re-check `subscribers` here.
  // Strict mode only: dev-mode sessions ("societyId":"dev") have no backing
  // `subscribers` row. Mirrors the status gate in handle_subscriber_login_POST.
  if (env_or("PBX_AUTH_STRICT") == "1") {
    const json sub_query = {
        {"societyId",   session.value("societyId", std::string{})},
        {"sipUsername", session.value("sipUsername", std::string{})}};
    const std::string sub_record =
        db.get_document("subscribers", sub_query.dump(), "{}");
    if (sub_record.empty()) {
      // Subscriber removed — the session is orphaned.
      return {response_error(403, "Forbidden",
                             "subscriber no longer exists"),
              {}};
    }
    json subscriber;
    try { subscriber = json::parse(sub_record); }
    catch (...) { subscriber = json::object(); }
    if (subscriber.contains("status") && subscriber["status"].is_string() &&
        subscriber["status"].get<std::string>() != "active") {
      return {response_error(403, "Forbidden",
                             "subscriber account is disabled"),
              {}};
    }
  }

  // Sliding refresh: every successful upgrade pushes expiresAt forward by
  // the role-appropriate TTL — a /sip-ws reconnect counts as activity.
  // Best-effort like the ping path: an upgrade still succeeds if the
  // write doesn't land.
  {
    const long ttl =
        ttl_sec_for_role(session.value("role", std::string{"resident"}));
    const json upd_filter = {{"token", token}};
    const json upd_doc    = {{"$set", {{"expiresAt", now + ttl}}}};
    try { db.update_collection("sessions", upd_filter.dump(), upd_doc.dump()); }
    catch (...) { /* swallowed */ }
  }

  // Identity resolved — build the OPEN-frame metadata (DESIGN.md §7). The
  // bridge ships this byte-faithfully to the agent in the OPEN frame.
  const json open_meta = {
      {"societyId",   session.value("societyId", std::string{})},
      {"sipUsername", session.value("sipUsername", std::string{})},
      {"clientUA",    parsed.get_element("User-Agent")},
  };
  return {/*error=*/std::string{}, open_meta.dump()};
}

} // namespace MicroServicePbx
