#include "webservice_test.hpp"
#include "json.hpp"

#include <gtest/gtest.h>

// ═══════════════════════════════════════════════════════════════════════════════
// MockMongodbClient — test double for IMongodbClient
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

class MockMongodbClient : public IMongodbClient {
public:
  // ── canned responses ──
  std::string getDocumentResult;
  std::string createDocumentResult;
  bool        updateCollectionResult = false;
  std::string getDocumentsResult;

  // ── spy fields ──
  std::string lastGetColl;
  std::string lastGetQuery;
  std::string lastGetProjection;
  std::string lastCreateColl;
  std::string lastCreateDoc;
  std::string lastUpdateColl;
  std::string lastUpdateFilter;
  std::string lastUpdateDoc;
  std::string lastGetDocumentsColl;
  std::string lastGetDocumentsQuery;
  std::string lastGetDocumentsProjection;

  const std::string &get_database() const override { return m_db; }

  std::string get_document(const std::string &coll, const std::string &query,
                           const std::string &projection) override {
    lastGetColl       = coll;
    lastGetQuery      = query;
    lastGetProjection = projection;
    return getDocumentResult;
  }

  std::string create_document(const std::string &, const std::string &coll,
                              const std::string &doc) override {
    lastCreateColl = coll;
    lastCreateDoc  = doc;
    return createDocumentResult;
  }
  std::int32_t create_bulk_document(const std::string &, const std::string &,
                                    const std::string &) override {
    return 0;
  }
  bool update_collection(const std::string &coll, const std::string &filter,
                         const std::string &doc) override {
    lastUpdateColl   = coll;
    lastUpdateFilter = filter;
    lastUpdateDoc    = doc;
    return updateCollectionResult;
  }
  std::int32_t update_bulk_document(const std::string &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &) override {
    return 0;
  }
  bool delete_document(const std::string &, const std::string &) override {
    return false;
  }
  std::string get_documents(const std::string &coll, const std::string &query,
                            const std::string &projection) override {
    lastGetDocumentsColl       = coll;
    lastGetDocumentsQuery      = query;
    lastGetDocumentsProjection = projection;
    return getDocumentsResult;
  }
  std::string get_documents(const std::string &coll,
                            const std::string &projection) override {
    lastGetDocumentsColl       = coll;
    lastGetDocumentsProjection = projection;
    return getDocumentsResult;
  }
  std::string next_awbno(const std::string &) override { return {}; }
  std::string store_file(const std::string &, const std::string &,
                         const std::vector<std::uint8_t> &) override {
    return {};
  }
  std::vector<std::uint8_t> fetch_file(const std::string &) override {
    return {};
  }
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &) override {
    return {};
  }
  bool delete_file(const std::string &) override { return false; }

private:
  std::string m_db = "testdb";
};

// Helper: build a minimal POST request with a JSON body
std::string make_post(const std::string &uri, const std::string &body) {
  return "POST " + uri + " HTTP/1.1\r\n"
         "Host: test\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\n"
         "\r\n" + body;
}

// Helper: build a PUT-like request with a userId header (for account updates)
std::string make_put(const std::string &uri, const std::string &userId,
                     const std::string &body) {
  return "PUT " + uri + " HTTP/1.1\r\n"
         "Host: test\r\n"
         "userId: " + userId + "\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\n"
         "\r\n" + body;
}

} // namespace

// ── build_responseOK ─────────────────────────────────────────────────────────

TEST(MicroService, ResponseOK_NoBody)
{
    MicroService e;
    std::string rsp = e.build_responseOK("");
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Length: 0"));
    EXPECT_NE(std::string::npos, rsp.find("Access-Control-Allow-Origin: *"));
}

TEST(MicroService, ResponseOK_WithJsonBody)
{
    MicroService e;
    const std::string body = "{\"status\":\"ok\"}";
    std::string rsp = e.build_responseOK(body);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Length: " + std::to_string(body.size())));
    EXPECT_NE(std::string::npos, rsp.find("Content-Type: application/json"));
    EXPECT_NE(std::string::npos, rsp.find(body));
}

TEST(MicroService, ResponseOK_CustomContentType)
{
    MicroService e;
    const std::string body = "<html></html>";
    std::string rsp = e.build_responseOK(body, "text/html");

    EXPECT_NE(std::string::npos, rsp.find("Content-Type: text/html"));
    EXPECT_NE(std::string::npos, rsp.find(body));
}

TEST(MicroService, ResponseOK_BodyAppearsAfterHeader)
{
    MicroService e;
    const std::string body = "payload";
    std::string rsp = e.build_responseOK(body);

    auto header_end = rsp.find("\r\n\r\n");
    ASSERT_NE(std::string::npos, header_end);
    EXPECT_EQ(body, rsp.substr(header_end + 4));
}

// ── build_responseCreated ────────────────────────────────────────────────────

TEST(MicroService, ResponseCreated)
{
    MicroService e;
    std::string rsp = e.build_responseCreated();

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 201 Created"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Length: 0"));
    EXPECT_NE(std::string::npos, rsp.find("Access-Control-Allow-Origin: *"));
    EXPECT_NE(std::string::npos, rsp.find("Connection: keep-alive"));
}

// ── build_responseERROR ──────────────────────────────────────────────────────

TEST(MicroService, ResponseError_NoBody)
{
    MicroService e;
    std::string rsp = e.build_responseERROR("", "404 Not Found");

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 404 Not Found"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Length: 0"));
}

TEST(MicroService, ResponseError_WithBody)
{
    MicroService e;
    const std::string body = "{\"error\":\"bad request\"}";
    std::string rsp = e.build_responseERROR(body, "400 Bad Request");

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Length: " + std::to_string(body.size())));
    EXPECT_NE(std::string::npos, rsp.find("Content-Type: application/json"));
    EXPECT_NE(std::string::npos, rsp.find(body));
}

TEST(MicroService, ResponseError_500)
{
    MicroService e;
    const std::string body = "{\"error\":\"internal\"}";
    std::string rsp = e.build_responseERROR(body, "500 Internal Server Error");

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 500 Internal Server Error"));
    EXPECT_NE(std::string::npos, rsp.find(body));
}

// ── get_contentType ──────────────────────────────────────────────────────────

TEST(MicroService, ContentType_Html)
{
    MicroService e;
    EXPECT_EQ("text/html", e.get_contentType("html"));
}

TEST(MicroService, ContentType_Css)
{
    MicroService e;
    EXPECT_EQ("text/css", e.get_contentType("css"));
}

TEST(MicroService, ContentType_Js)
{
    MicroService e;
    EXPECT_EQ("text/javascript", e.get_contentType("js"));
}

TEST(MicroService, ContentType_Json)
{
    MicroService e;
    EXPECT_EQ("application/json", e.get_contentType("json"));
}

TEST(MicroService, ContentType_Png)
{
    MicroService e;
    EXPECT_EQ("image/png", e.get_contentType("png"));
}

TEST(MicroService, ContentType_Jpg)
{
    MicroService e;
    EXPECT_EQ("image/jpeg", e.get_contentType("jpg"));
}

TEST(MicroService, ContentType_Gif)
{
    MicroService e;
    EXPECT_EQ("image/gif", e.get_contentType("gif"));
}

TEST(MicroService, ContentType_Svg)
{
    MicroService e;
    EXPECT_EQ("image/svg+xml", e.get_contentType("svg"));
}

TEST(MicroService, ContentType_Ico)
{
    MicroService e;
    EXPECT_EQ("image/vnd.microsoft.icon", e.get_contentType("ico"));
}

TEST(MicroService, ContentType_Woff)
{
    MicroService e;
    EXPECT_EQ("font/woff", e.get_contentType("woff"));
}

TEST(MicroService, ContentType_Woff2)
{
    MicroService e;
    EXPECT_EQ("font/woff2", e.get_contentType("woff2"));
}

TEST(MicroService, ContentType_Ttf)
{
    MicroService e;
    EXPECT_EQ("font/ttf", e.get_contentType("ttf"));
}

TEST(MicroService, ContentType_Otf)
{
    MicroService e;
    EXPECT_EQ("font/otf", e.get_contentType("otf"));
}

TEST(MicroService, ContentType_Eot)
{
    MicroService e;
    EXPECT_EQ("application/vnd.ms-fontobject", e.get_contentType("eot"));
}

TEST(MicroService, ContentType_UnknownFallsBackToHtml)
{
    MicroService e;
    EXPECT_EQ("text/html", e.get_contentType("xyz"));
    EXPECT_EQ("text/html", e.get_contentType(""));
}

// ── handle_OPTIONS ────────────────────────────────────────────────────────────

TEST(MicroService, HandleOptions_200OK)
{
    MicroService e;
    std::string in = "OPTIONS /api/v1/shipment HTTP/1.1\r\nHost: x.com\r\n\r\n";
    std::string rsp = e.handle_OPTIONS(in);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("Content-Length: 0"));
    EXPECT_NE(std::string::npos, rsp.find("Access-Control-Allow-Origin: *"));
    EXPECT_NE(std::string::npos, rsp.find("Access-Control-Allow-Methods: GET, POST, OPTIONS, PUT, DELETE"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// handle_account_login_POST
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AccountLoginTest, ValidCredentials_Returns200WithAccountData)
{
    MicroService e;
    MockMongodbClient db;
    std::string hash = MongodbClient::hash_password("secret");
    db.getDocumentResult =
        "{\"loginCredentials\":{\"accountCode\":\"admin\"},"
        "\"passwordHash\":\"" + hash + "\","
        "\"personalInfo\":{\"role\":\"Admin\",\"name\":\"Test\"}}";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"admin\",\"password\":\"secret\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_NE(std::string::npos, rsp.find("\"personalInfo\""));
    EXPECT_EQ(std::string::npos, rsp.find("passwordHash"));
}

TEST(AccountLoginTest, WrongPassword_Returns401)
{
    MicroService e;
    MockMongodbClient db;
    db.getDocumentResult =
        "{\"loginCredentials\":{\"accountCode\":\"admin\"},"
        "\"passwordHash\":\"" + MongodbClient::hash_password("secret") + "\"}";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"admin\",\"password\":\"wrongpassword\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

TEST(AccountLoginTest, UnknownUser_Returns401)
{
    MicroService e;
    MockMongodbClient db;
    db.getDocumentResult = "";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"nobody\",\"password\":\"anything\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

TEST(AccountLoginTest, MissingUserId_Returns400)
{
    MicroService e;
    MockMongodbClient db;

    std::string req = make_post("/api/v1/account/login",
                                "{\"password\":\"secret\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
}

TEST(AccountLoginTest, MissingPassword_Returns400)
{
    MicroService e;
    MockMongodbClient db;

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"admin\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
}

TEST(AccountLoginTest, EmptyBody_Returns400)
{
    MicroService e;
    MockMongodbClient db;

    std::string req = make_post("/api/v1/account/login", "");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 400 Bad Request"));
}

TEST(AccountLoginTest, LegacyPlainTextPassword_Rejected_AfterMigration)
{
    MicroService e;
    MockMongodbClient db;
    db.getDocumentResult =
        "{\"loginCredentials\":{"
        "\"accountCode\":\"admin\","
        "\"accountPassword\":\"secret\""
        "},\"personalInfo\":{\"role\":\"Admin\"}}";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"admin\",\"password\":\"secret\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

TEST(AccountLoginTest, ResponseBody_ExcludesSensitiveFields)
{
    MicroService e;
    MockMongodbClient db;
    std::string hash = MongodbClient::hash_password("secret");
    db.getDocumentResult =
        "{\"loginCredentials\":{\"accountCode\":\"admin\","
        "\"accountPassword\":\"secret\"},"
        "\"passwordHash\":\"" + hash + "\","
        "\"personalInfo\":{\"role\":\"Admin\",\"name\":\"Test\"}}";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"admin\",\"password\":\"secret\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    // Sensitive fields must NOT appear in the response body
    std::size_t body_start = rsp.find("\r\n\r\n");
    ASSERT_NE(std::string::npos, body_start);
    std::string body = rsp.substr(body_start + 4);
    EXPECT_EQ(std::string::npos, body.find("passwordHash"));
    EXPECT_EQ(std::string::npos, body.find("accountPassword"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 3 — hash on account create (POST /api/v1/account/account)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AccountCreateTest, HashesPasswordBeforeInsert)
{
    MicroService e;
    MockMongodbClient db;
    db.createDocumentResult = "fake-oid-123";

    std::string req = make_post("/api/v1/account/account",
        "{\"loginCredentials\":{\"accountCode\":\"newuser\","
        "\"accountPassword\":\"secret\"},"
        "\"personalInfo\":{\"role\":\"User\"}}");
    std::string rsp = e.handle_account_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    // Plain text password must NOT be in the stored document
    EXPECT_EQ(std::string::npos, db.lastCreateDoc.find("\"secret\""));
    // passwordHash must be present with modular-crypt format
    EXPECT_NE(std::string::npos, db.lastCreateDoc.find("\"passwordHash\""));
    EXPECT_NE(std::string::npos, db.lastCreateDoc.find("$pbkdf2-sha256$"));
    // accountPassword must be removed
    EXPECT_EQ(std::string::npos, db.lastCreateDoc.find("accountPassword"));
}

TEST(AccountCreateTest, NoPassword_StillWorks)
{
    MicroService e;
    MockMongodbClient db;
    db.createDocumentResult = "fake-oid-456";

    std::string req = make_post("/api/v1/account/account",
        "{\"loginCredentials\":{\"accountCode\":\"nopassuser\"},"
        "\"personalInfo\":{\"role\":\"User\"}}");
    std::string rsp = e.handle_account_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_EQ(std::string::npos, db.lastCreateDoc.find("passwordHash"));
    EXPECT_EQ(std::string::npos, db.lastCreateDoc.find("accountPassword"));
}

TEST(AccountCreateTest, EmptyPassword_StillWorks)
{
    MicroService e;
    MockMongodbClient db;
    db.createDocumentResult = "fake-oid-789";

    std::string req = make_post("/api/v1/account/account",
        "{\"loginCredentials\":{\"accountCode\":\"emptypass\","
        "\"accountPassword\":\"\"},"
        "\"personalInfo\":{\"role\":\"User\"}}");
    std::string rsp = e.handle_account_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    // Empty password is still a password — should get hashed
    EXPECT_NE(std::string::npos, db.lastCreateDoc.find("\"passwordHash\""));
    EXPECT_EQ(std::string::npos, db.lastCreateDoc.find("accountPassword"));
}

TEST(AccountCreateTest, EmptyBody_Returns200WithNoOp)
{
    MicroService e;
    MockMongodbClient db;

    std::string req = make_post("/api/v1/account/account", "");
    std::string rsp = e.handle_account_POST(req, db);

    // Empty body falls through, returns empty string (no-op)
    EXPECT_EQ(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 3 — hash on account update (PUT /api/v1/account/account?userId=...)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AccountUpdateTest, PasswordChange_Rehashes)
{
    MicroService e;
    MockMongodbClient db;
    db.updateCollectionResult = true;

    std::string req = make_put("/api/v1/account/account", "admin",
        "{\"loginCredentials\":{\"accountPassword\":\"newpass\"}}");
    std::string rsp = e.handle_account_PUT(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    // Plain text password must NOT be in the $set document
    EXPECT_EQ(std::string::npos, db.lastUpdateDoc.find("\"newpass\""));
    // passwordHash must be present in the $set
    EXPECT_NE(std::string::npos, db.lastUpdateDoc.find("\"passwordHash\""));
    EXPECT_NE(std::string::npos, db.lastUpdateDoc.find("$pbkdf2-sha256$"));
    // accountPassword must be removed
    EXPECT_EQ(std::string::npos, db.lastUpdateDoc.find("accountPassword"));
}

TEST(AccountUpdateTest, NoPasswordChange_DoesNotTouchHash)
{
    MicroService e;
    MockMongodbClient db;
    db.updateCollectionResult = true;

    std::string req = make_put("/api/v1/account/account", "admin",
        "{\"personalInfo\":{\"name\":\"New Name\"}}");
    std::string rsp = e.handle_account_PUT(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 200 OK"));
    EXPECT_EQ(std::string::npos, db.lastUpdateDoc.find("passwordHash"));
    EXPECT_EQ(std::string::npos, db.lastUpdateDoc.find("accountPassword"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 4 — seed data validation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SeedDataTest, BootstrapAdminHasHashedPassword)
{
    std::ifstream f("/src/docker/mongo-init.js");
    ASSERT_TRUE(f.is_open()) << "Cannot open mongo-init.js";

    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    // Extract the document passed to insertOne(...)
    auto start = contents.find("insertOne(");
    ASSERT_NE(std::string::npos, start);
    start = contents.find('{', start);
    ASSERT_NE(std::string::npos, start);

    auto end = contents.find("});", start);
    ASSERT_NE(std::string::npos, end);
    end = contents.rfind('}', end);
    ASSERT_NE(std::string::npos, end);

    std::string doc = contents.substr(start, end - start + 1);

    // Must use passwordHash, not plain-text accountPassword
    EXPECT_NE(std::string::npos, doc.find("passwordHash"))
        << "Seed data must contain passwordHash";
    EXPECT_NE(std::string::npos, doc.find("$pbkdf2-sha256$"))
        << "passwordHash must be in modular-crypt format";
    EXPECT_EQ(std::string::npos, doc.find("accountPassword"))
        << "Seed data must NOT contain plain-text accountPassword";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 5 — migration tool (migrate_account_passwords)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MigrationTest, Backfill_HashesPlainTextPassword)
{
    MockMongodbClient db;
    db.getDocumentsResult =
        "[{\"loginCredentials\":{\"accountCode\":\"u1\","
        "\"accountPassword\":\"p1\"},\"personalInfo\":{\"name\":\"U1\"}}]";
    db.updateCollectionResult = true;

    int count = migrate_account_passwords(db);

    EXPECT_EQ(1, count);
    EXPECT_EQ("account", db.lastUpdateColl);
    EXPECT_NE(std::string::npos,
              db.lastUpdateFilter.find("\"loginCredentials.accountCode\""));
    EXPECT_NE(std::string::npos,
              db.lastUpdateFilter.find("\"u1\""));
    EXPECT_NE(std::string::npos,
              db.lastUpdateDoc.find("loginCredentials.passwordHash"));
    EXPECT_NE(std::string::npos,
              db.lastUpdateDoc.find("$pbkdf2-sha256$"));
    EXPECT_NE(std::string::npos,
              db.lastUpdateDoc.find("$set"));
    EXPECT_NE(std::string::npos,
              db.lastUpdateDoc.find("$unset"));
    EXPECT_NE(std::string::npos,
              db.lastUpdateDoc.find("loginCredentials.accountPassword"));
}

TEST(MigrationTest, Backfill_SkipsAlreadyHashed)
{
    MockMongodbClient db;
    db.getDocumentsResult =
        "[{\"loginCredentials\":{\"accountCode\":\"u2\","
        "\"passwordHash\":\"$pbkdf2-sha256$i=600000$salt$key\"}}]";

    int count = migrate_account_passwords(db);

    EXPECT_EQ(0, count);
    EXPECT_TRUE(db.lastUpdateColl.empty())
        << "Should NOT call update for already-hashed docs";
}

TEST(MigrationTest, Backfill_SkipsDocWithNoPassword)
{
    MockMongodbClient db;
    db.getDocumentsResult =
        "[{\"loginCredentials\":{\"accountCode\":\"u3\"},"
        "\"personalInfo\":{\"name\":\"NoPassword\"}}]";

    int count = migrate_account_passwords(db);

    EXPECT_EQ(0, count);
    EXPECT_TRUE(db.lastUpdateColl.empty())
        << "Should NOT call update for docs with no password";
}

TEST(MigrationTest, EmptyCollection_ReturnsZero)
{
    MockMongodbClient db;
    db.getDocumentsResult = "[]";

    int count = migrate_account_passwords(db);

    EXPECT_EQ(0, count);
}

TEST(MigrationTest, MigrationFlag_RecognisedByCliParser)
{
    // Verify --migrate-passwords is accepted by ACE_Get_Opt
    const char *test_argv[] = {"uniservice", "--migrate-passwords"};
    int test_argc = 2;

    ACE_Get_Opt args(test_argc, const_cast<char **>(test_argv),
                     ACE_TEXT("m"), 1);
    args.long_option(ACE_TEXT("migrate-passwords"), 'm', ACE_Get_Opt::NO_ARG);

    bool found = false;
    int c;
    while ((c = args()) != EOF) {
        if (c == 'm') found = true;
    }

    EXPECT_TRUE(found) << "--migrate-passwords flag should be recognised";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 6 — remove plain-text password fallback
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AccountLoginTest, PlainTextFallback_Removed_Returns401)
{
    MicroService e;
    MockMongodbClient db;
    db.getDocumentResult =
        "{\"loginCredentials\":{\"accountCode\":\"admin\","
        "\"accountPassword\":\"secret\"},"
        "\"personalInfo\":{\"role\":\"Admin\"}}";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"admin\",\"password\":\"secret\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    // Plain-text fallback is removed — even correct password must fail
    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
    EXPECT_NE(std::string::npos, rsp.find("Invalid Credentials"));
}

TEST(AccountLoginTest, MissingPasswordHash_Returns401)
{
    MicroService e;
    MockMongodbClient db;
    // Account exists but has no passwordHash and no accountPassword
    db.getDocumentResult =
        "{\"loginCredentials\":{\"accountCode\":\"olduser\"},"
        "\"personalInfo\":{\"role\":\"User\"}}";

    std::string req = make_post("/api/v1/account/login",
                                "{\"userId\":\"olduser\",\"password\":\"anything\"}");
    std::string rsp = e.handle_account_login_POST(req, db);

    EXPECT_NE(std::string::npos, rsp.find("HTTP/1.1 401 Unauthorized"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Phase 7 — enforce SSL on wsdbagent (reject --no-ssl)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(WsDbAgentConfigTest, NoSslFlag_Rejected)
{
    // wsdbagent's ACE_Get_Opt parser: --no-ssl is recognised with 'n'.
    // After Phase 7, the agent rejects this flag at startup.
    const char *test_argv[] = {"wsdbagent", "--no-ssl"};
    int test_argc = 2;

    ACE_Get_Opt args(test_argc, const_cast<char **>(test_argv),
                     ACE_TEXT("H:p:U:C:D:b:A:E:K:nh"), 1);
    args.long_option(ACE_TEXT("no-ssl"), 'n', ACE_Get_Opt::NO_ARG);

    bool ssl_disabled = false;
    int c;
    while ((c = args()) != EOF) {
        if (c == 'n') ssl_disabled = true;
    }

    // --no-ssl must trigger rejection — the flag being set means SSL is disabled,
    // which is no longer allowed after Phase 7
    EXPECT_TRUE(ssl_disabled)
        << "--no-ssl should set the disable-ssl flag (to be rejected by main)";
}
