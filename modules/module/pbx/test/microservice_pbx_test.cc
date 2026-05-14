#include "microservice_pbx.hpp"
#include "mongodbc.hpp"
#include "json.hpp"
#include "webservice.hpp"   // for MicroService::dispatch_pbx_routes routing tests
#include <gtest/gtest.h>
#include <cstdlib>
#include <regex>
#include <string>

using json = nlohmann::json;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Programmable in-memory IMongodbClient. The xpmile MockMongodbClient (in
// webservice_test.cc) is single-result-per-method; MicroServicePbx needs a
// few queries per call, sometimes against different collections, so we use
// a tiny query-pattern → response map keyed by the (collection, query) pair.
// ─────────────────────────────────────────────────────────────────────────────

class TestDb : public IMongodbClient {
public:
  // collection → ordered list of (queryFragment, response) rules; first
  // rule whose fragment is a substring of the actual query wins.
  std::unordered_map<std::string,
                     std::vector<std::pair<std::string, std::string>>>
      getDoc;
  std::unordered_map<std::string,
                     std::vector<std::pair<std::string, std::string>>>
      getDocs;

  // Captured writes for assertions.
  struct Insert { std::string coll; std::string doc; };
  std::vector<Insert> inserts;

  const std::string &get_database() const override { return m_db; }

  std::string get_document(const std::string &coll, const std::string &query,
                           const std::string & /*proj*/) override {
    auto it = getDoc.find(coll);
    if (it == getDoc.end()) return {};
    for (const auto &rule : it->second)
      if (query.find(rule.first) != std::string::npos) return rule.second;
    return {};
  }

  std::string create_document(const std::string & /*db*/,
                              const std::string &coll,
                              const std::string &doc) override {
    inserts.push_back({coll, doc});
    return {};
  }

  std::int32_t create_bulk_document(const std::string &, const std::string &,
                                    const std::string &) override { return 0; }
  bool update_collection(const std::string &, const std::string &,
                         const std::string &) override { return false; }
  std::int32_t update_bulk_document(const std::string &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &) override {
    return 0;
  }
  bool delete_document(const std::string &, const std::string &) override {
    return false;
  }
  std::string get_documents(const std::string &coll, const std::string &query,
                            const std::string & /*proj*/) override {
    auto it = getDocs.find(coll);
    if (it == getDocs.end()) return {};
    for (const auto &rule : it->second)
      if (query.find(rule.first) != std::string::npos) return rule.second;
    return {};
  }
  std::string get_documents(const std::string &coll,
                            const std::string & /*proj*/) override {
    auto it = getDocs.find(coll);
    if (it == getDocs.end()) return {};
    return it->second.empty() ? std::string{} : it->second.front().second;
  }
  std::string next_awbno(const std::string &) override { return {}; }
  std::string store_file(const std::string &, const std::string &,
                         const std::vector<std::uint8_t> &) override {
    return {};
  }
  std::vector<std::uint8_t> fetch_file(const std::string &) override { return {}; }
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &) override {
    return {};
  }
  bool delete_file(const std::string &) override { return false; }

private:
  std::string m_db = "pbx";
};

std::string make_post(const std::string &uri, const std::string &body,
                       const std::string &content_type = "application/json",
                       const std::string &extra_headers = {}) {
  std::ostringstream os;
  os << "POST " << uri << " HTTP/1.1\r\n"
     << "Host: test\r\n"
     << "Content-Type: " << content_type << "\r\n"
     << "Content-Length: " << body.size() << "\r\n"
     << extra_headers
     << "\r\n"
     << body;
  return os.str();
}

std::string make_get(const std::string &uri,
                      const std::string &extra_headers = {}) {
  std::ostringstream os;
  os << "GET " << uri << " HTTP/1.1\r\n"
     << "Host: test\r\n"
     << extra_headers
     << "Content-Length: 0\r\n"
     << "\r\n";
  return os.str();
}

std::string make_ws_upgrade(const std::string &uri,
                             const std::string &cookie_header = {}) {
  std::ostringstream os;
  os << "GET " << uri << " HTTP/1.1\r\n"
     << "Host: test\r\n"
     << "Upgrade: websocket\r\n"
     << "Connection: Upgrade\r\n"
     << "Sec-WebSocket-Version: 13\r\n"
     << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  if (!cookie_header.empty())
    os << "Cookie: " << cookie_header << "\r\n";
  os << "Content-Length: 0\r\n\r\n";
  return os.str();
}

} // namespace

// ── Society create ────────────────────────────────────────────────────────────

TEST(MicroServicePbx, SocietyCreate_201)
{
    TestDb db;
    const std::string body = R"({"name":"Sunset Towers","code":"SUNSET",)"
                             R"("address":"Block A, MG Road"})";
    const std::string req = make_post("/api/v1/society", body);

    std::string rsp = MicroServicePbx::handle_society_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 201 Created"));

    // Body of the response carries the canonical society doc with our
    // generated SIP realm + TURN secret.
    const auto body_start = rsp.find("\r\n\r\n");
    ASSERT_NE(std::string::npos, body_start);
    const std::string body_str = rsp.substr(body_start + 4);
    json doc = json::parse(body_str);
    EXPECT_EQ("Sunset Towers",        doc["name"]);
    EXPECT_EQ("SUNSET",               doc["code"]);
    EXPECT_EQ("SUNSET.pbx.local",     doc["sipRealm"]);
    EXPECT_EQ(5,                       doc["maxConcurrentCalls"]);
    EXPECT_EQ(30,                      doc["ringTimeoutSec"]);
    EXPECT_FALSE(doc["turnSharedSecret"].get<std::string>().empty());

    // Exactly one Mongo insert into `societies`.
    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("societies", db.inserts[0].coll);
}

TEST(MicroServicePbx, SocietyCreate_DuplicateCode_409)
{
    TestDb db;
    // Pre-seed: a society with code SUNSET already exists.
    db.getDoc["societies"].push_back({R"("code":"SUNSET")",
                                       R"({"_id":"s1","code":"SUNSET"})"});

    const std::string body = R"({"name":"Sunset Towers","code":"SUNSET"})";
    const std::string req = make_post("/api/v1/society", body);

    std::string rsp = MicroServicePbx::handle_society_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 409 Conflict"));
    EXPECT_EQ(0u, db.inserts.size()) << "must not insert when duplicate";
}

// ── Subscriber import ─────────────────────────────────────────────────────────

namespace {
void seed_society_and_flats(TestDb &db) {
    db.getDoc["societies"].push_back({R"("_id":"s1")",
                                       R"({"_id":"s1","sipRealm":"SUNSET.pbx.local"})"});
    db.getDoc["flats"].push_back({R"("number":"A-101")",
                                   R"({"_id":"f1","number":"A-101"})"});
    db.getDoc["flats"].push_back({R"("number":"B-204")",
                                   R"({"_id":"f2","number":"B-204"})"});
}
}

TEST(MicroServicePbx, SubscriberImport_GeneratesCreds)
{
    TestDb db;
    seed_society_and_flats(db);

    const std::string csv =
        "flat_number,name,email,phone,role\r\n"
        "A-101,Asha,asha@x,9999,resident\r\n"
        "B-204,Bob,bob@x,8888,resident\r\n";
    const std::string req =
        make_post("/api/v1/subscriber/import?societyId=s1", csv, "text/csv");

    std::string rsp = MicroServicePbx::handle_subscriber_import_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Type: text/csv"));
    EXPECT_NE(std::string::npos, rsp.find("X-Import-Created: 2"));
    EXPECT_NE(std::string::npos, rsp.find("X-Import-Skipped: 0"));

    // Two subscriber inserts.
    int subscriber_inserts = 0;
    for (const auto &ins : db.inserts)
        if (ins.coll == "subscribers") ++subscriber_inserts;
    EXPECT_EQ(2, subscriber_inserts);

    // sipHa1 / portalPasswordHash present, plaintext sipPassword NOT in DB doc.
    for (const auto &ins : db.inserts) {
        if (ins.coll != "subscribers") continue;
        json d = json::parse(ins.doc);
        EXPECT_TRUE(d.contains("sipHa1"));
        EXPECT_TRUE(d.contains("portalPasswordHash"));
        EXPECT_FALSE(d.contains("sipPassword"));
        EXPECT_FALSE(d.contains("portalPassword"));
    }

    // Response CSV carries the plaintexts ONCE.
    const auto body_start = rsp.find("\r\n\r\n");
    ASSERT_NE(std::string::npos, body_start);
    const std::string out_csv = rsp.substr(body_start + 4);
    EXPECT_NE(std::string::npos, out_csv.find("A-101,Asha,asha@x,u_"));
    EXPECT_NE(std::string::npos, out_csv.find("B-204,Bob,bob@x,u_"));
}

TEST(MicroServicePbx, SubscriberImport_RejectsBadFlat)
{
    TestDb db;
    seed_society_and_flats(db);  // only A-101 and B-204 exist

    const std::string csv =
        "flat_number,name,email,phone,role\r\n"
        "A-101,Asha,asha@x,9999,resident\r\n"
        "Z-999,Ghost,ghost@x,7777,resident\r\n";
    const std::string req =
        make_post("/api/v1/subscriber/import?societyId=s1", csv, "text/csv");

    std::string rsp = MicroServicePbx::handle_subscriber_import_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
    // Error body names the offending row index (row 2 = the second data row).
    EXPECT_NE(std::string::npos, rsp.find("Row 2"));
    EXPECT_NE(std::string::npos, rsp.find("Z-999"));
}

TEST(MicroServicePbx, SubscriberImport_Idempotent)
{
    TestDb db;
    seed_society_and_flats(db);
    // First row's email is already registered.
    db.getDoc["subscribers"].push_back(
        {R"("email":"asha@x")", R"({"_id":"sub1","email":"asha@x"})"});

    const std::string csv =
        "flat_number,name,email,phone,role\r\n"
        "A-101,Asha,asha@x,9999,resident\r\n"
        "B-204,Bob,bob@x,8888,resident\r\n";
    const std::string req =
        make_post("/api/v1/subscriber/import?societyId=s1", csv, "text/csv");

    std::string rsp = MicroServicePbx::handle_subscriber_import_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("X-Import-Created: 1"));
    EXPECT_NE(std::string::npos, rsp.find("X-Import-Skipped: 1"));

    int subscriber_inserts = 0;
    for (const auto &ins : db.inserts)
        if (ins.coll == "subscribers") ++subscriber_inserts;
    EXPECT_EQ(1, subscriber_inserts) << "must not double-create asha@x";

    // Response CSV marks the skipped row.
    const auto body_start = rsp.find("\r\n\r\n");
    const std::string out_csv = rsp.substr(body_start + 4);
    EXPECT_NE(std::string::npos, out_csv.find("asha@x,,,,skipped"));
}

// ── CDR list ──────────────────────────────────────────────────────────────────

TEST(MicroServicePbx, CdrList_FiltersBySociety)
{
    TestDb db;
    db.getDocs["cdr"].push_back(
        {R"("societyId":"s1")",
         R"([{"_id":"c1","societyId":"s1","fromFlat":"A-101","toFlat":"B-204"}])"});

    const std::string req = make_get("/api/v1/cdr?societyId=s1");
    std::string rsp = MicroServicePbx::handle_cdr_GET(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find(R"("societyId":"s1")"));
    EXPECT_NE(std::string::npos, rsp.find(R"("fromFlat":"A-101")"));
}

TEST(MicroServicePbx, CdrList_MissingSocietyId_400)
{
    TestDb db;
    const std::string req = make_get("/api/v1/cdr");
    std::string rsp = MicroServicePbx::handle_cdr_GET(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
}

// ── Push subscribe ────────────────────────────────────────────────────────────

TEST(MicroServicePbx, PushSubscribe_PersistsEndpoint)
{
    TestDb db;
    const std::string body =
        R"({"subscriberId":"sub1","endpoint":"https://push.example/abc",)"
        R"("p256dh":"BNc...","auth":"xyz"})";
    const std::string req = make_post("/api/v1/push/subscribe", body);

    std::string rsp = MicroServicePbx::handle_push_subscribe_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 201 Created"));
    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("push_subscriptions", db.inserts[0].coll);
    EXPECT_NE(std::string::npos, db.inserts[0].doc.find("https://push.example/abc"));
}

TEST(MicroServicePbx, PushSubscribe_RejectsMissingFields)
{
    TestDb db;
    const std::string body = R"({"subscriberId":"sub1"})";
    const std::string req = make_post("/api/v1/push/subscribe", body);

    std::string rsp = MicroServicePbx::handle_push_subscribe_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
    EXPECT_TRUE(db.inserts.empty());
}

// ── Subscriber portal login ───────────────────────────────────────────────────

namespace {
// RAII: flip PBX_AUTH_STRICT on for a strict-mode test, restore on scope exit.
// GTest ASSERT_* just returns from the test body, so the destructor still runs.
struct StrictAuthEnv {
  StrictAuthEnv()  { ::setenv("PBX_AUTH_STRICT", "1", 1); }
  ~StrictAuthEnv() { ::unsetenv("PBX_AUTH_STRICT"); }
};

// A `subscribers` doc whose bcrypt portalPasswordHash matches `password`,
// shaped like the rows handle_subscriber_import_POST writes.
std::string subscriber_doc(const std::string &email, const std::string &password,
                           const std::string &status = "active") {
  json doc = {
      {"_id",                "sub_" + email},
      {"societyId",          "soc1"},
      {"flatId",             "flatA101"},
      {"name",               "Asha Resident"},
      {"email",              email},
      {"role",               "resident"},
      {"status",             status},
      {"sipUsername",        "u_abc123"},
      {"sipHa1",             "deadbeefdeadbeefdeadbeefdeadbeef"},
      {"portalPasswordHash", MongodbClient::hash_password(password)},
  };
  return doc.dump();
}
} // namespace

TEST(MicroServicePbx, SubscriberLogin_DevMode_AcceptsAnyCredentials)
{
    TestDb db;  // PBX_AUTH_STRICT unset → dev mode
    const std::string req = make_post(
        "/api/v1/subscriber/login",
        R"({"email":"anyone@example.com","password":"whatever"})");

    std::string rsp = MicroServicePbx::handle_subscriber_login_POST(req, db);
    ASSERT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    // The portal session is persisted and pinned with an HttpOnly cookie.
    EXPECT_NE(std::string::npos, rsp.find("Set-Cookie: session="));
    EXPECT_NE(std::string::npos, rsp.find("HttpOnly; Secure; SameSite=Strict"));
    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("sessions", db.inserts[0].coll);

    const std::string body = rsp.substr(rsp.find("\r\n\r\n") + 4);
    json j = json::parse(body);
    EXPECT_FALSE(j["token"].get<std::string>().empty());
    EXPECT_EQ("anyone@example.com", j["subscriber"]["email"]);
    // The token in the body matches the persisted session row.
    EXPECT_NE(std::string::npos,
              db.inserts[0].doc.find(j["token"].get<std::string>()));
}

TEST(MicroServicePbx, SubscriberLogin_MissingField_400)
{
    TestDb db;
    const std::string req = make_post("/api/v1/subscriber/login",
                                      R"({"email":"a@example.com"})");
    std::string rsp = MicroServicePbx::handle_subscriber_login_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
}

TEST(MicroServicePbx, SubscriberLogin_Strict_ValidCredentials_200)
{
    StrictAuthEnv strict;
    TestDb db;
    db.getDoc["subscribers"].push_back(
        {R"("email":"asha@example.com")",
         subscriber_doc("asha@example.com", "s3cret-pass")});

    const std::string req = make_post(
        "/api/v1/subscriber/login",
        R"({"email":"asha@example.com","password":"s3cret-pass"})");
    std::string rsp = MicroServicePbx::handle_subscriber_login_POST(req, db);
    ASSERT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("Set-Cookie: session="));
    // A `sessions` row is persisted, carrying the real subscriber identity.
    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("sessions", db.inserts[0].coll);
    EXPECT_NE(std::string::npos, db.inserts[0].doc.find(R"("sipUsername":"u_abc123")"));

    const std::string body = rsp.substr(rsp.find("\r\n\r\n") + 4);
    json j = json::parse(body);
    EXPECT_FALSE(j["token"].get<std::string>().empty());
    EXPECT_EQ("asha@example.com", j["subscriber"]["email"]);
    EXPECT_EQ("u_abc123",         j["subscriber"]["sipUsername"]);
    // Secrets must never leave the server.
    EXPECT_FALSE(j["subscriber"].contains("portalPasswordHash"));
    EXPECT_FALSE(j["subscriber"].contains("sipHa1"));
}

TEST(MicroServicePbx, SubscriberLogin_Strict_WrongPassword_401)
{
    StrictAuthEnv strict;
    TestDb db;
    db.getDoc["subscribers"].push_back(
        {R"("email":"asha@example.com")",
         subscriber_doc("asha@example.com", "s3cret-pass")});

    const std::string req = make_post(
        "/api/v1/subscriber/login",
        R"({"email":"asha@example.com","password":"wrong-pass"})");
    std::string rsp = MicroServicePbx::handle_subscriber_login_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

TEST(MicroServicePbx, SubscriberLogin_Strict_UnknownEmail_401)
{
    StrictAuthEnv strict;
    TestDb db;  // no subscribers seeded
    const std::string req = make_post(
        "/api/v1/subscriber/login",
        R"({"email":"ghost@example.com","password":"whatever"})");
    std::string rsp = MicroServicePbx::handle_subscriber_login_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

TEST(MicroServicePbx, SubscriberLogin_Strict_DisabledAccount_403)
{
    StrictAuthEnv strict;
    TestDb db;
    db.getDoc["subscribers"].push_back(
        {R"("email":"asha@example.com")",
         subscriber_doc("asha@example.com", "s3cret-pass", "disabled")});

    const std::string req = make_post(
        "/api/v1/subscriber/login",
        R"({"email":"asha@example.com","password":"s3cret-pass"})");
    std::string rsp = MicroServicePbx::handle_subscriber_login_POST(req, db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 403 Forbidden"));
}

// ── SIP-WS upgrade: session validation + identity resolution ──────────────────

namespace {
// A `sessions` doc shaped like the one finish_login writes.
std::string session_doc(const std::string &token, long expires_at,
                         const std::string &society_id = "soc1",
                         const std::string &sip_username = "u_abc123") {
  json doc = {
      {"token",       token},
      {"email",       "asha@example.com"},
      {"societyId",   society_id},
      {"sipUsername", sip_username},
      {"role",        "resident"},
      {"createdAt",   100},
      {"expiresAt",   expires_at},
  };
  return doc.dump();
}
constexpr long kFarFuture = 4102444800L;  // year 2100
} // namespace

TEST(MicroServicePbx, Auth_RejectsAnonymousSipWsUpgrade)
{
    TestDb db;
    const std::string req = make_ws_upgrade("/sip-ws");  // no token, no cookie
    auto up = MicroServicePbx::handle_sipws_upgrade(req, db);

    EXPECT_NE(std::string::npos, up.error.find("HTTP/1.1 401 Unauthorized"));
    EXPECT_TRUE(up.open_meta.empty());
}

TEST(MicroServicePbx, Auth_AllowsSipWsUpgrade_WithSessionCookie)
{
    TestDb db;
    db.getDoc["sessions"].push_back(
        {R"("token":"abc123")", session_doc("abc123", kFarFuture)});

    const std::string req = make_ws_upgrade("/sip-ws", "other=1; session=abc123");
    auto up = MicroServicePbx::handle_sipws_upgrade(req, db);

    EXPECT_TRUE(up.error.empty()) << "valid session → upgrade may proceed";
    ASSERT_FALSE(up.open_meta.empty());
    json meta = json::parse(up.open_meta);
    EXPECT_EQ("soc1",     meta["societyId"]);
    EXPECT_EQ("u_abc123", meta["sipUsername"]);
}

TEST(MicroServicePbx, SipWsUpgrade_ResolvesSubscriberMeta_FromQueryToken)
{
    TestDb db;
    db.getDoc["sessions"].push_back(
        {R"("token":"tok-query")",
         session_doc("tok-query", kFarFuture, "soc9", "u_zzz")});

    // UI delivery path: ?token= in the WebSocket URL, no cookie.
    const std::string req = make_ws_upgrade("/sip-ws?token=tok-query");
    auto up = MicroServicePbx::handle_sipws_upgrade(req, db);

    ASSERT_TRUE(up.error.empty());
    json meta = json::parse(up.open_meta);
    EXPECT_EQ("soc9",  meta["societyId"]);
    EXPECT_EQ("u_zzz", meta["sipUsername"]);
}

TEST(MicroServicePbx, SipWsUpgrade_RejectsUnknownToken)
{
    TestDb db;  // no sessions seeded
    const std::string req = make_ws_upgrade("/sip-ws?token=ghost");
    auto up = MicroServicePbx::handle_sipws_upgrade(req, db);

    EXPECT_NE(std::string::npos, up.error.find("HTTP/1.1 401 Unauthorized"));
    EXPECT_TRUE(up.open_meta.empty());
}

TEST(MicroServicePbx, SipWsUpgrade_RejectsExpiredSession)
{
    TestDb db;
    db.getDoc["sessions"].push_back(
        {R"("token":"stale")", session_doc("stale", /*expires_at=*/1)});

    const std::string req = make_ws_upgrade("/sip-ws?token=stale");
    auto up = MicroServicePbx::handle_sipws_upgrade(req, db);

    EXPECT_NE(std::string::npos, up.error.find("HTTP/1.1 401 Unauthorized"));
    EXPECT_TRUE(up.open_meta.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Route-wiring: MicroService::dispatch_pbx_routes() picks the right handler.
// Asserts the dispatch table only — handler correctness is owned by the
// MicroServicePbx.* suite above. xpmile URIs (e.g. /api/v1/account/login) must
// NOT be intercepted here; dispatch_pbx_routes returns empty on those so the
// xpmile route chain runs.
// ─────────────────────────────────────────────────────────────────────────────

TEST(MicroServiceRouting, RoutesSocietyPost)
{
    TestDb db;
    MicroService e;
    const std::string req = make_post("/api/v1/society",
                                       R"({"name":"X","code":"X1"})");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 201 Created"));
    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("societies", db.inserts[0].coll);
}

TEST(MicroServiceRouting, RoutesSubscriberImportPost)
{
    TestDb db;
    seed_society_and_flats(db);
    MicroService e;
    const std::string csv =
        "flat_number,name,email,phone,role\r\n"
        "A-101,Asha,asha@x,9999,resident\r\n";
    const std::string req = make_post(
        "/api/v1/subscriber/import?societyId=s1", csv, "text/csv");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Type: text/csv"));
}

TEST(MicroServiceRouting, RoutesCdrGet)
{
    TestDb db;
    db.getDocs["cdr"].push_back(
        {R"("societyId":"s1")", R"([{"_id":"c1","societyId":"s1"}])"});
    MicroService e;
    const std::string req = make_get("/api/v1/cdr?societyId=s1");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find(R"("societyId":"s1")"));
}

TEST(MicroServiceRouting, RoutesPushSubscribePost)
{
    TestDb db;
    MicroService e;
    const std::string req = make_post(
        "/api/v1/push/subscribe",
        R"({"subscriberId":"u1","endpoint":"https://x/y","p256dh":"k","auth":"a"})");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 201 Created"));
    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("push_subscriptions", db.inserts[0].coll);
}

TEST(MicroServiceRouting, RoutesSubscriberLoginPost)
{
    TestDb db;  // dev mode (PBX_AUTH_STRICT unset) — routing check only
    MicroService e;
    const std::string req = make_post(
        "/api/v1/subscriber/login",
        R"({"email":"a@example.com","password":"p"})");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find(R"("token")"));
}

TEST(MicroServiceRouting, FallsThroughOnXpmileUri)
{
    TestDb db;
    MicroService e;
    const std::string req = make_post("/api/v1/account/login",
                                       R"({"userId":"x","password":"y"})");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_TRUE(rsp.empty())
        << "xpmile URIs must NOT be intercepted by the PBX dispatch";
}

TEST(MicroServiceRouting, FallsThroughOnUnknownUri)
{
    TestDb db;
    MicroService e;
    const std::string req = make_get("/something/random");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_TRUE(rsp.empty());
}

TEST(MicroServiceRouting, FallsThroughOnWrongMethod)
{
    TestDb db;
    MicroService e;
    // /api/v1/society is POST-only; GET must fall through.
    const std::string req = make_get("/api/v1/society");
    std::string rsp = e.dispatch_pbx_routes(const_cast<std::string &>(req), db);
    EXPECT_TRUE(rsp.empty());
}
