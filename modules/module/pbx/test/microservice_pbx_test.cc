#include "microservice_pbx.hpp"
#include "mongodbc.hpp"
#include "json.hpp"
#include <gtest/gtest.h>
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

// ── SIP-WS upgrade auth gate ──────────────────────────────────────────────────

TEST(MicroServicePbx, Auth_RejectsAnonymousSipWsUpgrade)
{
    const std::string req = make_ws_upgrade("/sip-ws");  // no Cookie
    std::string rsp = MicroServicePbx::handle_sipws_upgrade(req);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

TEST(MicroServicePbx, Auth_AllowsSipWsUpgrade_WithSessionCookie)
{
    const std::string req = make_ws_upgrade("/sip-ws", "other=1; session=abc123");
    std::string rsp = MicroServicePbx::handle_sipws_upgrade(req);

    EXPECT_TRUE(rsp.empty()) << "Empty response means 'upgrade may proceed'";
}
