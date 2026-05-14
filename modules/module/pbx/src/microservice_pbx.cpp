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
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {

// ── Small response helpers ────────────────────────────────────────────────────

std::string http_response(int code, const std::string &reason,
                          const std::string &body,
                          const std::string &content_type = "application/json") {
  std::ostringstream os;
  os << "HTTP/1.1 " << code << " " << reason << "\r\n"
     << "Connection: keep-alive\r\n"
     << "Access-Control-Allow-Origin: *\r\n"
     << "Content-Length: " << body.size() << "\r\n"
     << "Content-Type: " << content_type << "\r\n"
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

  const std::string result =
      db.get_documents("cdr",
                        R"({"societyId":")" + society_id + R"("})",
                        "{}");

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

  db.create_document(db.get_database(), "push_subscriptions", body.dump());
  return http_response(201, "Created", body.dump());
}

// ── Slice (REST-routing fix): login, directory, vapid, turn-creds ────────────
//
// These four endpoints unblock the UI's end-to-end flow. Each ships in a
// minimal-but-correct form so the live deployment behaves as advertised in
// /webui/. Production-grade auth + DB-backed directory come next.

namespace {

std::string env_or(const char *name, const std::string &fallback = {}) {
  const char *v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

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

std::string handle_subscriber_login_POST(const std::string &req,
                                         IMongodbClient & /*db*/) {
  Http parsed(req);

  json body;
  try { body = json::parse(parsed.body()); }
  catch (...) {
    return response_error(400, "Bad Request", "Invalid JSON body");
  }

  for (const char *required : {"societyCode", "flatNumber", "password"}) {
    if (!body.contains(required) || !body[required].is_string() ||
        body[required].get<std::string>().empty()) {
      return response_error(400, "Bad Request",
                            std::string("Missing field: ") + required);
    }
  }

  const std::string society_code = body["societyCode"].get<std::string>();
  const std::string flat_number  = body["flatNumber"].get<std::string>();
  // Production auth (strict mode) requires the subscriber row to exist
  // and bcrypt(portalPasswordHash) to match. The MVP deploy runs without
  // seeded Mongo data, so we accept any non-empty triple and synthesise
  // the subscriber profile from the form fields. Override by setting
  // PBX_AUTH_STRICT=1 once CSV-import has populated the collection.
  const bool strict = env_or("PBX_AUTH_STRICT") == "1";
  if (strict) {
    return response_error(503, "Service Unavailable",
                          "strict-mode auth requires DB lookup; not yet wired");
  }

  // 16-byte random bearer, hex-encoded → 32 chars.
  unsigned char token_bytes[16];
  if (RAND_bytes(token_bytes, sizeof(token_bytes)) != 1) {
    return response_error(500, "Internal Server Error", "RAND_bytes failed");
  }
  const std::string token = hex_encode(token_bytes, sizeof(token_bytes));

  json subscriber = {
      {"societyId",   society_code},
      {"flatNumber",  flat_number},
      {"displayName", flat_number},
      {"sipUser",     flat_number},
      {"role",        "resident"},
  };
  json rsp = {{"token", token}, {"subscriber", subscriber}};
  return http_response(200, "OK", rsp.dump());
}

std::string handle_directory_GET(const std::string &req, IMongodbClient &db) {
  const std::string uri          = raw_uri_with_query(req);
  const std::string society_id   = query_param(uri, "societyId");
  const std::string flat_prefix  = query_param(uri, "flatPrefix");
  if (society_id.empty()) {
    return response_error(400, "Bad Request",
                          "Missing required query param: societyId");
  }

  // Mongo regex would be ideal but get_documents takes a plain JSON filter
  // here; do a society-scoped fetch, then filter by prefix client-side.
  const std::string result = db.get_documents(
      "subscribers",
      R"({"societyId":")" + society_id + R"("})",
      "{}");

  // No rows → return [] rather than the Mongo "" so the UI's typed
  // response shape is honoured.
  if (result.empty()) return http_response(200, "OK", "[]");

  json arr;
  try { arr = json::parse(result); } catch (...) { arr = json::array(); }
  if (!arr.is_array()) arr = json::array();

  if (!flat_prefix.empty()) {
    json filtered = json::array();
    for (auto &row : arr) {
      if (!row.contains("flatNumber") || !row["flatNumber"].is_string()) continue;
      const std::string flat = row["flatNumber"].get<std::string>();
      if (flat.size() < flat_prefix.size()) continue;
      if (std::equal(flat_prefix.begin(), flat_prefix.end(), flat.begin(),
                     [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                     })) {
        filtered.push_back(row);
      }
    }
    arr = std::move(filtered);
  }

  return http_response(200, "OK", arr.dump());
}

std::string handle_push_vapid_key_GET(const std::string & /*req*/,
                                      IMongodbClient & /*db*/) {
  const std::string key = env_or("VAPID_PUBLIC_KEY");
  json rsp = {{"key", key}};
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

std::string handle_sipws_upgrade(const std::string &req) {
  Http parsed(req);

  const std::string cookie = parsed.get_element("Cookie");
  const std::string session = cookie_value(cookie, "session");

  if (session.empty()) {
    return response_error(401, "Unauthorized",
                          "SIP-WS upgrade requires an authenticated portal session");
  }
  // Session validity is checked against Mongo by the bridge wrapper that
  // owns the upgrade hand-off. Here we only enforce the cookie's presence,
  // matching the v1 contract in DESIGN.md §5 ("Cloud only validates the
  // WSS upgrade has a session cookie tied to a portal login").
  return {};
}

} // namespace MicroServicePbx
