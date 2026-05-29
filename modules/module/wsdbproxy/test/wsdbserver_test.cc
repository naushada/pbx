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
    // Production behaviour (`WsDbServer::on_agent_connected`):
    // when a second agent arrives while one is already connected,
    // the server treats the existing one as STALE — calls
    // `::shutdown(SHUT_RDWR)` on its socket to unwedge any blocked
    // recv — and tells the second agent to back off and retry via
    //   HTTP/1.1 503 Service Unavailable
    //   Retry-After: 2
    // (NOT 409 Conflict — that was the older behaviour, still used
    // by the parallel TLS-accept loop's early-reject in svc(), but
    // not by on_agent_connected itself). This test asserts both
    // halves of the new contract.
    int sv1[2], sv2[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv1), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv2), 0);

    WsDbServer server(1);
    ASSERT_EQ(server.open(), 0);

    EXPECT_TRUE(server.on_agent_connected(sv1[0]));
    EXPECT_FALSE(server.on_agent_connected(sv2[0]));

    // (a) Second agent gets the retry-after instruction.
    char buf[256] = {};
    const ssize_t n = ::recv(sv2[1], buf, sizeof(buf) - 1, MSG_DONTWAIT);
    EXPECT_GT(n, 0) << "second agent socket should carry the 503 response";
    const std::string body(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
    EXPECT_NE(body.find("503"), std::string::npos)
        << "expected 503 Service Unavailable, got: " << body;
    EXPECT_NE(body.find("Retry-After: 2"), std::string::npos)
        << "expected Retry-After: 2 header, got: " << body;

    // (b) First (stale) agent's socket has been shut down — its peer
    // sees EOF on read instead of hanging.
    char eof_buf[1] = {};
    const ssize_t eof_n = ::recv(sv1[1], eof_buf, 1, MSG_DONTWAIT);
    EXPECT_EQ(eof_n, 0)
        << "first agent's socket should be SHUT_RDWR'd by the eviction";

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
