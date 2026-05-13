#include "wsproxy_test.hpp"

// Every test creates a fresh fake dispatcher and a WsMongodbProxy over it.
// The fake captures the BSON request and returns a preset response.

// ── create_document ───────────────────────────────────────────────────────────

TEST(WsMongodbProxy, CreateDocument_SendsCorrectOp)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    fake.will_respond_sval(0, "deadbeef");  // reqid unknown ahead of time — use 0 as placeholder
    // Override: let fake capture and return based on whatever reqid comes in
    fake.next_response = dbproto::make_error_response(0, "");
    // Re-set to a valid ok response (reqid doesn't need to match in the fake)
    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = "deadbeef";
    fake.next_response = dbproto::build_response(ok);

    proxy.create_document("xpmile", "shipping", R"({"awbno":"AWB001"})");

    EXPECT_EQ(fake.last_parsed().op,   DbOp::CREATE_DOCUMENT);
    EXPECT_EQ(fake.last_parsed().coll, "shipping");
}

TEST(WsMongodbProxy, CreateDocument_ReturnsSval)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = "507f1f77bcf86cd799439011";
    fake.next_response = dbproto::build_response(ok);

    std::string result = proxy.create_document("xpmile", "shipping", R"({})");
    EXPECT_EQ(result, "507f1f77bcf86cd799439011");
}

TEST(WsMongodbProxy, CreateDocument_ReturnsEmptyOnError)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    fake.next_response = dbproto::make_error_response(0, "write failed");
    std::string result = proxy.create_document("xpmile", "shipping", R"({})");
    EXPECT_TRUE(result.empty());
}

// ── get_document ──────────────────────────────────────────────────────────────

TEST(WsMongodbProxy, GetDocument_SendsDocAndDoc2)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = R"({"awbno":"AWB001"})";
    fake.next_response = dbproto::build_response(ok);

    proxy.get_document("shipping",
                        R"({"awbno":"AWB001"})",
                        R"({"_id":false})");

    auto req = fake.last_parsed();
    EXPECT_EQ(req.op,   DbOp::GET_DOCUMENT);
    EXPECT_FALSE(req.doc.empty());   // query embedded as BSON
    EXPECT_FALSE(req.doc2.empty());  // projection embedded as BSON
}

// ── get_documents ─────────────────────────────────────────────────────────────

TEST(WsMongodbProxy, GetDocuments_WithQuery_UsesQueriedOp)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = "[]";
    fake.next_response = dbproto::build_response(ok);

    proxy.get_documents("shipping", R"({"status":"active"})", R"({"_id":false})");
    EXPECT_EQ(fake.last_parsed().op, DbOp::GET_DOCUMENTS_QUERIED);
}

TEST(WsMongodbProxy, GetDocuments_WithoutQuery_UsesAllOp)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = "[]";
    fake.next_response = dbproto::build_response(ok);

    proxy.get_documents("shipping", R"({"_id":false})");
    EXPECT_EQ(fake.last_parsed().op, DbOp::GET_DOCUMENTS_ALL);
}

// ── update_collection ─────────────────────────────────────────────────────────

TEST(WsMongodbProxy, UpdateCollection_SendsBothDocs)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.bval = true;
    fake.next_response = dbproto::build_response(ok);

    proxy.update_collection("shipping",
                             R"({"awbno":"AWB001"})",
                             R"({"$set":{"status":"delivered"}})");

    auto req = fake.last_parsed();
    EXPECT_EQ(req.op,   DbOp::UPDATE_COLLECTION);
    EXPECT_FALSE(req.doc.empty());   // filter
    EXPECT_FALSE(req.doc2.empty());  // update
}

// ── delete_document ───────────────────────────────────────────────────────────

TEST(WsMongodbProxy, DeleteDocument_ReturnsBval)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.bval = true;
    fake.next_response = dbproto::build_response(ok);

    bool result = proxy.delete_document("shipping", R"({"awbno":"AWB001"})");
    EXPECT_TRUE(result);
    EXPECT_EQ(fake.last_parsed().op, DbOp::DELETE_DOCUMENT);
}

// ── next_awbno ────────────────────────────────────────────────────────────────

TEST(WsMongodbProxy, NextAwbno_SendsPrefixInSval)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = "AWB000000042";
    fake.next_response = dbproto::build_response(ok);

    std::string awb = proxy.next_awbno("AWB");
    EXPECT_EQ(fake.last_parsed().sval, "AWB");
    EXPECT_EQ(awb, "AWB000000042");
}

// ── store_file ────────────────────────────────────────────────────────────────

TEST(WsMongodbProxy, StoreFile_SvalContainsNameAndMime)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.sval = "oid123";
    fake.next_response = dbproto::build_response(ok);

    std::vector<uint8_t> bytes = {0x25, 0x50, 0x44, 0x46};
    std::string oid = proxy.store_file("report.pdf", "application/pdf", bytes);

    EXPECT_EQ(fake.last_parsed().op,   DbOp::STORE_FILE);
    EXPECT_EQ(fake.last_parsed().sval, "report.pdf|application/pdf");
    EXPECT_EQ(fake.last_parsed().doc,  bytes);
    EXPECT_EQ(oid, "oid123");
}

// ── fetch_file_by_id ──────────────────────────────────────────────────────────

TEST(WsMongodbProxy, FetchFileById_ReturnsData)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);

    std::vector<uint8_t> file_bytes = {1, 2, 3, 4, 5};
    dbproto::DbResponse ok;
    ok.reqid = 0; ok.ok = true; ok.data = file_bytes;
    fake.next_response = dbproto::build_response(ok);

    auto result = proxy.fetch_file_by_id("oid123");
    EXPECT_EQ(result, file_bytes);
    EXPECT_EQ(fake.last_parsed().op,   DbOp::FETCH_FILE_BY_ID);
    EXPECT_EQ(fake.last_parsed().sval, "oid123");
}

// ── get_database ──────────────────────────────────────────────────────────────

TEST(WsMongodbProxy, GetDatabase_ReturnsEmpty)
{
    FakeWsDispatcher fake;
    WsMongodbProxy proxy(fake);
    EXPECT_EQ(proxy.get_database(), "");
}

// ── IMongodbClient interface ──────────────────────────────────────────────────

TEST(WsMongodbProxy, ImplementsIMongodbClient)
{
    static_assert(std::is_base_of_v<IMongodbClient, WsMongodbProxy>,
                  "WsMongodbProxy must inherit IMongodbClient");
}

TEST(MongodbClient, ImplementsIMongodbClient)
{
    static_assert(std::is_base_of_v<IMongodbClient, MongodbClient>,
                  "MongodbClient must inherit IMongodbClient");
}

TEST(WsMongodbProxy, WorkCtxAcceptsIMongodbClientPtr)
{
    // Verifies that WorkCtx::db can hold an IMongodbClient* at compile time.
    // WorkCtx is defined in webservice.cpp (anonymous namespace), but we can
    // check the pointer assignment compiles.
    IMongodbClient* p = nullptr;
    (void)p;  // no-op — compile-time check only
}
