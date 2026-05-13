#include "wsdbserver_test.hpp"

#include <atomic>
#include <chrono>
#include <thread>

// ── no-agent ──────────────────────────────────────────────────────────────────

TEST(WsDbServer, DispatchWithNoAgent_ReturnsError)
{
    WsDbServer server(1);  // 1s timeout

    auto [reqid, bson] = dbproto::build_request(DbOp::NEXT_AWBNO, {}, {}, {}, {}, "AWB");
    auto rsp_bson = server.dispatch(bson);

    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(rsp_bson, rsp));
    EXPECT_FALSE(rsp.ok);
    EXPECT_FALSE(rsp.errmsg.empty());
}

// ── on_agent_connected ────────────────────────────────────────────────────────

TEST(WsDbServer, OnAgentConnected_SetsConnectedFlag)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    WsDbServer server(1);
    ASSERT_EQ(server.open(), 0);

    EXPECT_FALSE(server.is_connected());
    EXPECT_TRUE(server.on_agent_connected(sv[0]));
    EXPECT_TRUE(server.is_connected());

    server.close(0);
    ::close(sv[1]);
}

// ── dispatch sends frame ──────────────────────────────────────────────────────

TEST(WsDbServer, Dispatch_SendsWsBinaryFrameToAgent)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    WsDbServer server(2);
    ASSERT_EQ(server.open(), 0);
    ASSERT_TRUE(server.on_agent_connected(sv[0]));

    auto [reqid, req_bson] = dbproto::build_request(
        DbOp::NEXT_AWBNO, {}, {}, {}, {}, "AWB");

    // dispatch() blocks — run in background thread
    std::vector<uint8_t> received;
    std::thread agent([&] {
        // Read what the server sent us
        received = recv_ws_binary(sv[1]);

        // Send back a valid response so dispatch() unblocks
        dbproto::DbResponse rsp;
        rsp.reqid = reqid;
        rsp.ok    = true;
        rsp.sval  = "AWB000000001";
        send_ws_binary(sv[1], dbproto::build_response(rsp));
    });

    server.dispatch(req_bson);
    agent.join();

    // The received bytes should decode as the request BSON
    dbproto::DbRequest parsed;
    ASSERT_TRUE(dbproto::parse_request(received, parsed));
    EXPECT_EQ(parsed.op,   DbOp::NEXT_AWBNO);
    EXPECT_EQ(parsed.sval, "AWB");

    server.close(0);
    ::close(sv[1]);
}

// ── dispatch matches response by reqid ───────────────────────────────────────

TEST(WsDbServer, Dispatch_MatchesResponseByReqId)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    WsDbServer server(2);
    ASSERT_EQ(server.open(), 0);
    ASSERT_TRUE(server.on_agent_connected(sv[0]));

    auto [reqid, req_bson] = dbproto::build_request(
        DbOp::NEXT_AWBNO, {}, {}, {}, {}, "AWB");

    std::string received_awb;
    std::thread agent([&] {
        recv_ws_binary(sv[1]);  // consume request

        dbproto::DbResponse rsp;
        rsp.reqid = reqid;
        rsp.ok    = true;
        rsp.sval  = "AWB000000042";
        send_ws_binary(sv[1], dbproto::build_response(rsp));
    });

    auto rsp_bson = server.dispatch(req_bson);
    agent.join();

    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(rsp_bson, rsp));
    EXPECT_TRUE(rsp.ok);
    EXPECT_EQ(rsp.sval, "AWB000000042");

    server.close(0);
    ::close(sv[1]);
}

// ── timeout ───────────────────────────────────────────────────────────────────

TEST(WsDbServer, Dispatch_TimesOutWhenAgentSilent)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    WsDbServer server(1);  // 1s timeout
    ASSERT_EQ(server.open(), 0);
    ASSERT_TRUE(server.on_agent_connected(sv[0]));

    auto [reqid, req_bson] = dbproto::build_request(
        DbOp::NEXT_AWBNO, {}, {}, {}, {}, "AWB");

    // Agent reads the request but never responds
    std::thread agent([&] { recv_ws_binary(sv[1]); });

    auto start    = std::chrono::steady_clock::now();
    auto rsp_bson = server.dispatch(req_bson);
    auto elapsed  = std::chrono::steady_clock::now() - start;

    agent.join();

    dbproto::DbResponse rsp;
    ASSERT_TRUE(dbproto::parse_response(rsp_bson, rsp));
    EXPECT_FALSE(rsp.ok);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 900);

    server.close(0);
    ::close(sv[1]);
}

// ── disconnect wakes all pending ──────────────────────────────────────────────

TEST(WsDbServer, Disconnect_WakesAllPendingDispatchers)
{
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    WsDbServer server(5);
    ASSERT_EQ(server.open(), 0);
    ASSERT_TRUE(server.on_agent_connected(sv[0]));

    std::atomic<int> failed_count{0};

    auto [rid1, bson1] = dbproto::build_request(DbOp::NEXT_AWBNO, {}, {}, {}, {}, "A");
    auto [rid2, bson2] = dbproto::build_request(DbOp::NEXT_AWBNO, {}, {}, {}, {}, "B");

    std::thread t1([&] {
        auto rsp_bson = server.dispatch(bson1);
        dbproto::DbResponse r;
        dbproto::parse_response(rsp_bson, r);
        if (!r.ok) ++failed_count;
    });
    std::thread t2([&] {
        auto rsp_bson = server.dispatch(bson2);
        dbproto::DbResponse r;
        dbproto::parse_response(rsp_bson, r);
        if (!r.ok) ++failed_count;
    });

    // Give both threads time to block in dispatch()
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Close the agent side — simulates disconnect
    ::close(sv[1]);

    t1.join();
    t2.join();

    EXPECT_EQ(failed_count.load(), 2);
    EXPECT_FALSE(server.is_connected());

    server.close(0);
}

// ── second agent rejected ─────────────────────────────────────────────────────

TEST(WsDbServer, SecondAgentRejected_When_FirstAlive)
{
    int sv1[2], sv2[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv1), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv2), 0);

    WsDbServer server(1);
    ASSERT_EQ(server.open(), 0);

    EXPECT_TRUE(server.on_agent_connected(sv1[0]));
    EXPECT_FALSE(server.on_agent_connected(sv2[0]));  // rejected

    // sv2[1] should have received 409
    char buf[256] = {};
    ::recv(sv2[1], buf, sizeof(buf) - 1, MSG_DONTWAIT);
    EXPECT_NE(std::string(buf).find("409"), std::string::npos);

    server.close(0);
    ::close(sv1[1]);
    ::close(sv2[1]);
}

// ── unique reqids ─────────────────────────────────────────────────────────────

TEST(WsDbServer, ConcurrentDispatches_UniqueReqIds)
{
    constexpr int N = 20;
    std::vector<std::int32_t> ids;
    ids.reserve(N);
    std::mutex mu;

    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            auto [id, bson] = dbproto::build_request(
                DbOp::NEXT_AWBNO, {}, {}, {}, {}, "X");
            std::lock_guard<std::mutex> lock(mu);
            ids.push_back(id);
        });
    }
    for (auto& t : threads) t.join();

    // All reqids must be distinct
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}
