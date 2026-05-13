#include "tunnel_harness.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace tunnel_e2e;

// ─────────────────────────────────────────────────────────────────────────────
// 1. End-to-end browser ↔ "Asterisk" round-trip through cloud + agent.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, BrowserBytesReachAsteriskFake_AndBack)
{
    TunnelHarness h;
    h.connect();
    ASSERT_TRUE(h.cloud_endpoint.has_agent());
    ASSERT_TRUE(h.agent_connector.connected());

    // A browser appears on the cloud side and starts a SIP REGISTER.
    FakeBrowser b;
    const auto sid = h.cloud_bridge.on_browser_upgrade(
        &b, R"({"sip":"u_alice"})");

    const std::string reg =
        "REGISTER sip:society SIP/2.0\r\n"
        "Via: SIP/2.0/WSS x;branch=z9hG4bK1\r\n"
        "Call-ID: 1@society\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Content-Length: 0\r\n\r\n";
    h.cloud_bridge.on_browser_data(sid, reg);

    // Agent's Asterisk fake received the OPEN context AND the REGISTER bytes.
    auto *ast = h.ast_factory.state_for(sid);
    ASSERT_NE(nullptr, ast);
    ASSERT_EQ(1u, ast->received.size());
    EXPECT_EQ(reg, ast->received[0]);

    // Asterisk responds via the demux's Asterisk-side hook.
    const std::string resp = "SIP/2.0 401 Unauthorized\r\n\r\n";
    h.agent_demux.on_asterisk_data(sid, resp);

    // Cloud-side browser saw the response.
    ASSERT_EQ(1u, b.got.size());
    EXPECT_EQ(resp, b.got[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Two browser streams interleave through the single tunnel without
//    cross-talk.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, TwoBrowsersMultiplexedOverOneTunnel)
{
    TunnelHarness h;
    h.connect();

    FakeBrowser ba, bb;
    const auto sa = h.cloud_bridge.on_browser_upgrade(&ba, R"({"sip":"u_a"})");
    const auto sb = h.cloud_bridge.on_browser_upgrade(&bb, R"({"sip":"u_b"})");
    EXPECT_NE(sa, sb);

    h.cloud_bridge.on_browser_data(sa, "for-a");
    h.cloud_bridge.on_browser_data(sb, "for-b");

    auto *ast_a = h.ast_factory.state_for(sa);
    auto *ast_b = h.ast_factory.state_for(sb);
    ASSERT_NE(nullptr, ast_a);
    ASSERT_NE(nullptr, ast_b);
    ASSERT_EQ(1u, ast_a->received.size());
    EXPECT_EQ("for-a", ast_a->received[0]);
    ASSERT_EQ(1u, ast_b->received.size());
    EXPECT_EQ("for-b", ast_b->received[0]);

    // Asterisk replies interleaved.
    h.agent_demux.on_asterisk_data(sb, "reply-b");
    h.agent_demux.on_asterisk_data(sa, "reply-a");

    ASSERT_EQ(1u, ba.got.size()) << "browser-a got only its own reply";
    EXPECT_EQ("reply-a", ba.got[0]);
    ASSERT_EQ(1u, bb.got.size()) << "browser-b got only its own reply";
    EXPECT_EQ("reply-b", bb.got[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Agent restart — browsers get a clean reset, not partial garbage.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, AgentRestart_BrowsersGetCleanReset)
{
    TunnelHarness h;
    h.connect();

    FakeBrowser b1, b2;
    h.cloud_bridge.on_browser_upgrade(&b1, "{}");
    h.cloud_bridge.on_browser_upgrade(&b2, "{}");
    EXPECT_EQ(2u, h.cloud_bridge.active_streams());

    // Agent process dies — its end of the transport closes.
    h.agent_side_drop();

    // Browsers see a clean close with the documented reason; no partial
    // bytes arrived at any sink during the drop.
    EXPECT_TRUE(b1.closed);
    EXPECT_TRUE(b2.closed);
    EXPECT_EQ("tunnel_lost", b1.close_reason);
    EXPECT_EQ("tunnel_lost", b2.close_reason);
    EXPECT_EQ(0u, h.cloud_bridge.active_streams())
        << "bridge cleared its stream map on disconnect";
    EXPECT_FALSE(h.cloud_endpoint.has_agent());
    EXPECT_FALSE(h.agent_connector.connected());
}

TEST(TunnelE2E, AgentReconnectAfterRestart_StreamCounterDoesNotReset)
{
    TunnelHarness h;
    h.connect();
    FakeBrowser b1;
    const auto sid1 = h.cloud_bridge.on_browser_upgrade(&b1, "{}");

    h.agent_side_drop();

    // Driver: tick (with no backoff elapsed) the connector to reconnect.
    h.clock.advance(2);
    h.connect();
    EXPECT_TRUE(h.cloud_endpoint.has_agent());

    FakeBrowser b2;
    const auto sid2 = h.cloud_bridge.on_browser_upgrade(&b2, "{}");
    EXPECT_GT(sid2, sid1) << "stream-id counter survives across reconnect";
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. PUSH_NOTIFY from the agent triggers the cloud's Web Push hook.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, PushNotifyFromAgentToCloud_TriggersWebPush)
{
    TunnelHarness h;
    h.connect();

    // The agent's AriClient (real production) would emit PUSH_NOTIFY when
    // it sees a StasisStart whose target is offline. Simulate that here by
    // writing the frame straight onto the agent's TunnelSink (the
    // CloudConnector implements it).
    const std::string payload =
        R"({"subscriberId":"u1","callerFlat":"A-101","callId":"abc"})";
    h.agent_connector.send_frame(SipFrame::Op::PUSH_NOTIFY, 0, payload);

    ASSERT_EQ(1u, h.push_payloads.size())
        << "cloud's PushSender hook fires once on PUSH_NOTIFY";
    EXPECT_EQ(payload, h.push_payloads[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. CDR_PUSH from the agent persists on the cloud Mongo path.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, CdrPushFromAgentToCloud_PersistsInMongo)
{
    TunnelHarness h;
    h.connect();

    const std::string cdr_doc =
        R"({"_id":"c1","societyId":"s1","fromFlat":"A-101","toFlat":"B-204")"
        R"(,"durationSec":42,"type":"p2p","hangupCause":"normal"})";
    h.agent_connector.send_frame(SipFrame::Op::CDR_PUSH, 0, cdr_doc);

    ASSERT_EQ(1u, h.cdr_payloads.size())
        << "cloud's CDR writer hook fires once on CDR_PUSH";
    EXPECT_EQ(cdr_doc, h.cdr_payloads[0]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Extra: browser-initiated close on cloud side reaches the agent's demux
// and tears down the matching Asterisk-bound stream.
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, BrowserCloseOnCloud_ReachesAgentAsCloseFrame)
{
    TunnelHarness h;
    h.connect();

    FakeBrowser b;
    const auto sid = h.cloud_bridge.on_browser_upgrade(&b, "{}");
    auto *ast = h.ast_factory.state_for(sid);
    ASSERT_NE(nullptr, ast);
    EXPECT_FALSE(ast->closed);

    h.cloud_bridge.on_browser_close(sid, "user_hangup");

    EXPECT_TRUE(ast->closed);
    EXPECT_EQ("user_hangup", ast->close_reason);
    EXPECT_EQ(0u, h.cloud_bridge.active_streams());
    EXPECT_EQ(0u, h.agent_demux.active_streams());
}

// ─────────────────────────────────────────────────────────────────────────────
// Extra: bytes produced during the outage are buffered and flushed after
// reconnect (combines CloudConnector outbound-buffer + CloudTunnelEndpoint
// inbound-resume).
// ─────────────────────────────────────────────────────────────────────────────

TEST(TunnelE2E, OutboundFromAgentDuringDrop_FlushesOnReconnect)
{
    TunnelHarness h;
    h.connect();

    h.agent_side_drop();
    EXPECT_FALSE(h.cloud_endpoint.has_agent());

    // Agent emits PUSH_NOTIFY during the outage — gets buffered.
    h.agent_connector.send_frame(SipFrame::Op::PUSH_NOTIFY, 0, "u1-payload");
    EXPECT_EQ(0u, h.push_payloads.size())
        << "while disconnected, cloud sees nothing yet";

    // Reconnect.
    h.clock.advance(2);
    h.connect();

    ASSERT_EQ(1u, h.push_payloads.size())
        << "PUSH_NOTIFY buffered during outage flushed on reconnect";
    EXPECT_EQ("u1-payload", h.push_payloads[0]);
}
