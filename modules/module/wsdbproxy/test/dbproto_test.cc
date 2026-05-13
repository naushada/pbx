#include "dbproto_test.hpp"

// ── build_request ─────────────────────────────────────────────────────────────

TEST(DbProto, BuildRequest_CreateDocument_HasRequiredFields)
{
    auto doc = json_to_bson_bytes(R"({"name":"test"})");
    auto [reqid, bson] = dbproto::build_request(
        DbOp::CREATE_DOCUMENT, "xpmile", "shipping", doc);

    dbproto::DbRequest req;
    ASSERT_TRUE(dbproto::parse_request(bson, req));
    EXPECT_EQ(req.reqid,  reqid);
    EXPECT_EQ(req.op,     DbOp::CREATE_DOCUMENT);
    EXPECT_EQ(req.db,     "xpmile");
    EXPECT_EQ(req.coll,   "shipping");
    EXPECT_EQ(req.doc,    doc);
    EXPECT_TRUE(req.doc2.empty());
    EXPECT_TRUE(req.sval.empty());
}

TEST(DbProto, BuildRequest_GetDocument_HasDocAndDoc2)
{
    auto q = json_to_bson_bytes(R"({"awbno":"AWB001"})");
    auto p = json_to_bson_bytes(R"({"_id":false})");
    auto [reqid, bson] = dbproto::build_request(
        DbOp::GET_DOCUMENT, {}, "shipping", q, p);

    dbproto::DbRequest req;
    ASSERT_TRUE(dbproto::parse_request(bson, req));
    EXPECT_EQ(req.op,   DbOp::GET_DOCUMENT);
    EXPECT_EQ(req.doc,  q);
    EXPECT_EQ(req.doc2, p);
}

TEST(DbProto, BuildRequest_NextAwbno_HasSval)
{
    auto [reqid, bson] = dbproto::build_request(
        DbOp::NEXT_AWBNO, {}, {}, {}, {}, "AWB");

    dbproto::DbRequest req;
    ASSERT_TRUE(dbproto::parse_request(bson, req));
    EXPECT_EQ(req.op,   DbOp::NEXT_AWBNO);
    EXPECT_EQ(req.sval, "AWB");
    EXPECT_TRUE(req.doc.empty());
    EXPECT_TRUE(req.doc2.empty());
}

TEST(DbProto, BuildRequest_StoreFile_HasSvalAndDoc)
{
    std::vector<std::uint8_t> file_bytes = {0x25, 0x50, 0x44, 0x46};  // %PDF
    auto [reqid, bson] = dbproto::build_request(
        DbOp::STORE_FILE, {}, {}, file_bytes, {}, "report.pdf|application/pdf");

    dbproto::DbRequest req;
    ASSERT_TRUE(dbproto::parse_request(bson, req));
    EXPECT_EQ(req.op,   DbOp::STORE_FILE);
    EXPECT_EQ(req.sval, "report.pdf|application/pdf");
    EXPECT_EQ(req.doc,  file_bytes);
}

TEST(DbProto, BuildRequest_ReqIdIncrements)
{
    auto [id1, bson1] = dbproto::build_request(DbOp::NEXT_AWBNO, {}, {}, {}, {}, "A");
    auto [id2, bson2] = dbproto::build_request(DbOp::NEXT_AWBNO, {}, {}, {}, {}, "A");
    EXPECT_EQ(id2, id1 + 1);
}

// ── parse_response ────────────────────────────────────────────────────────────

TEST(DbProto, ParseResponse_OkWithSval)
{
    auto bytes = make_ok_sval_response(7, "507f1f77bcf86cd799439011");
    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(bytes, rsp));
    EXPECT_EQ(rsp.reqid, 7);
    EXPECT_TRUE(rsp.ok);
    EXPECT_EQ(rsp.sval, "507f1f77bcf86cd799439011");
}

TEST(DbProto, ParseResponse_OkWithIval)
{
    auto bytes = make_ok_ival_response(2, 5);
    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(bytes, rsp));
    EXPECT_TRUE(rsp.ok);
    EXPECT_EQ(rsp.ival, 5);
}

TEST(DbProto, ParseResponse_OkWithBval)
{
    auto bytes = make_ok_bval_response(3, true);
    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(bytes, rsp));
    EXPECT_TRUE(rsp.ok);
    EXPECT_TRUE(rsp.bval);
}

TEST(DbProto, ParseResponse_OkWithData)
{
    dbproto::DbResponse in;
    in.reqid = 4;
    in.ok    = true;
    in.data  = {0x01, 0x02, 0x03, 0x04};
    auto bytes = dbproto::build_response(in);

    dbproto::DbResponse out;
    ASSERT_TRUE(dbproto::parse_response(bytes, out));
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.data, in.data);
}

TEST(DbProto, ParseResponse_NotOk_HasErrMsg)
{
    dbproto::DbResponse in;
    in.reqid  = 5;
    in.ok     = false;
    in.errmsg = "collection not found";
    auto bytes = dbproto::build_response(in);

    dbproto::DbResponse out;
    ASSERT_TRUE(dbproto::parse_response(bytes, out));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.errmsg, "collection not found");
}

TEST(DbProto, ParseResponse_EmptyBson_ReturnsNotOk)
{
    dbproto::DbResponse rsp;
    EXPECT_FALSE(dbproto::parse_response({}, rsp));
}

TEST(DbProto, MakeErrorResponse_ReqidAndMsg)
{
    auto bytes = dbproto::make_error_response(42, "agent disconnected");
    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(bytes, rsp));
    EXPECT_EQ(rsp.reqid, 42);
    EXPECT_FALSE(rsp.ok);
    EXPECT_EQ(rsp.errmsg, "agent disconnected");
}

// ── round-trip ────────────────────────────────────────────────────────────────

TEST(DbProto, RoundTrip_ParseRequest_AfterBuild)
{
    auto doc  = json_to_bson_bytes(R"({"k":"v"})");
    auto doc2 = json_to_bson_bytes(R"({"_id":false})");
    auto [reqid, bson] = dbproto::build_request(
        DbOp::GET_DOCUMENTS_QUERIED, "xpmile", "account", doc, doc2, "extra");

    dbproto::DbRequest req;
    ASSERT_TRUE(dbproto::parse_request(bson, req));
    EXPECT_EQ(req.reqid,  reqid);
    EXPECT_EQ(req.op,     DbOp::GET_DOCUMENTS_QUERIED);
    EXPECT_EQ(req.db,     "xpmile");
    EXPECT_EQ(req.coll,   "account");
    EXPECT_EQ(req.doc,    doc);
    EXPECT_EQ(req.doc2,   doc2);
    EXPECT_EQ(req.sval,   "extra");
}
