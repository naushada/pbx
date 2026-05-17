#include "sip_bridge.hpp"
#include "sip_frame.hpp"
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

namespace {

// In-memory tunnel that records every frame the bridge sends downstream.
class FakeTunnel : public TunnelSink {
public:
    struct Sent {
        SipFrame::Op   op;
        std::uint32_t  stream_id;
        std::string    payload;
    };
    std::vector<Sent> sent;

    void send_frame(SipFrame::Op op, std::uint32_t sid,
                    const std::string &payload) override {
        sent.push_back({op, sid, payload});
    }
};

// In-memory browser that records bytes pushed at it and whether it was closed.
class FakeBrowser : public BrowserSink {
public:
    std::vector<std::string> got;
    bool closed = false;
    std::string close_reason;

    void send_bytes(const std::string &b) override { got.push_back(b); }
    void close(const std::string &reason) override {
        closed = true;
        close_reason = reason;
    }
};

} // namespace

// ── on_browser_upgrade ────────────────────────────────────────────────────────

TEST(SipBridge, OnBrowserUpgrade_AssignsStreamId)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b1, b2, b3;

    const auto id1 = bridge.on_browser_upgrade(&b1, R"({"sip":"u_a"})");
    const auto id2 = bridge.on_browser_upgrade(&b2, R"({"sip":"u_b"})");
    const auto id3 = bridge.on_browser_upgrade(&b3, R"({"sip":"u_c"})");

    // Monotonic, non-zero, unique.
    EXPECT_LT(0u, id1);
    EXPECT_LT(id1, id2);
    EXPECT_LT(id2, id3);

    // 3 OPEN frames went down the tunnel with matching stream-ids + payloads.
    ASSERT_EQ(3u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::OPEN, tun.sent[0].op);
    EXPECT_EQ(id1, tun.sent[0].stream_id);
    EXPECT_EQ(R"({"sip":"u_a"})", tun.sent[0].payload);
    EXPECT_EQ(SipFrame::Op::OPEN, tun.sent[2].op);
    EXPECT_EQ(id3, tun.sent[2].stream_id);

    EXPECT_EQ(3u, bridge.active_streams());
}

TEST(SipBridge, OnBrowserUpgrade_UniqueIdsUnderManyUpgrades)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);
    std::vector<FakeBrowser> browsers(1000);

    std::set<std::uint32_t> ids;
    for (auto &b : browsers) {
        const auto sid = bridge.on_browser_upgrade(&b, "{}");
        ASSERT_TRUE(ids.insert(sid).second) << "duplicate stream-id " << sid;
    }
    EXPECT_EQ(1000u, ids.size());
}

// ── on_browser_data ───────────────────────────────────────────────────────────

TEST(SipBridge, OnBrowserData_FramesAndForwards)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");
    tun.sent.clear();  // discard OPEN frame from the upgrade

    const std::string sip_bytes =
        "INVITE sip:b-204@society SIP/2.0\r\nVia: SIP/2.0/WSS x\r\n\r\n";
    bridge.on_browser_data(sid, sip_bytes);

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::DATA, tun.sent[0].op);
    EXPECT_EQ(sid,                tun.sent[0].stream_id);
    EXPECT_EQ(sip_bytes,          tun.sent[0].payload);
}

TEST(SipBridge, OnBrowserData_UnknownStreamId_DroppedSilently)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);
    bridge.on_browser_data(42, "ghost");
    EXPECT_TRUE(tun.sent.empty());
}

// ── on_tunnel_bytes (DATA demux) ──────────────────────────────────────────────

TEST(SipBridge, OnTunnelData_DemuxesByStreamId)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b1, b2;
    const auto s1 = bridge.on_browser_upgrade(&b1, "{}");
    const auto s2 = bridge.on_browser_upgrade(&b2, "{}");

    // Build a tunnel stream: two DATA frames, one for each browser.
    const std::string frame1 = SipFrame::encode(SipFrame::Op::DATA, s1, "for-b1");
    const std::string frame2 = SipFrame::encode(SipFrame::Op::DATA, s2, "for-b2");
    EXPECT_TRUE(bridge.on_tunnel_bytes(frame1 + frame2));

    ASSERT_EQ(1u, b1.got.size());
    EXPECT_EQ("for-b1", b1.got[0]);
    ASSERT_EQ(1u, b2.got.size());
    EXPECT_EQ("for-b2", b2.got[0]);
}

TEST(SipBridge, OnTunnelData_PartialFrameAcrossReads)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");

    const std::string frame = SipFrame::encode(SipFrame::Op::DATA, sid, "hello");
    // Deliver in two halves.
    EXPECT_TRUE(bridge.on_tunnel_bytes(frame.substr(0, 6)));
    EXPECT_TRUE(b.got.empty());
    EXPECT_TRUE(bridge.on_tunnel_bytes(frame.substr(6)));

    ASSERT_EQ(1u, b.got.size());
    EXPECT_EQ("hello", b.got[0]);
}

TEST(SipBridge, OnTunnelData_PingTriggersPong)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    const std::string ping = SipFrame::encode(SipFrame::Op::PING, 0, "");
    EXPECT_TRUE(bridge.on_tunnel_bytes(ping));

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::PONG, tun.sent[0].op);
    EXPECT_EQ(0u, tun.sent[0].stream_id);
}

TEST(SipBridge, OnTunnelData_InvalidFrameReturnsFalse)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    std::string bad = SipFrame::encode(SipFrame::Op::DATA, 1, "x");
    bad[0] = 0x77;  // bogus version
    EXPECT_FALSE(bridge.on_tunnel_bytes(bad));
}

// ── on_browser_close / agent-initiated CLOSE ──────────────────────────────────

TEST(SipBridge, OnBrowserClose_SendsCloseFrame)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");
    tun.sent.clear();

    bridge.on_browser_close(sid, "user_hangup");

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::CLOSE, tun.sent[0].op);
    EXPECT_EQ(sid,                  tun.sent[0].stream_id);
    EXPECT_EQ("user_hangup",        tun.sent[0].payload);
    EXPECT_EQ(0u, bridge.active_streams());
    // Bridge does NOT call close() on the browser sink when the close was
    // browser-initiated — the caller owns that.
    EXPECT_FALSE(b.closed);
}

TEST(SipBridge, OnTunnelCloseFrame_ClosesBrowserSink)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");

    const std::string close = SipFrame::encode(SipFrame::Op::CLOSE, sid, "agent_bye");
    EXPECT_TRUE(bridge.on_tunnel_bytes(close));

    EXPECT_TRUE(b.closed);
    EXPECT_EQ("agent_bye", b.close_reason);
    EXPECT_EQ(0u, bridge.active_streams());
}

// ── on_tunnel_disconnect ──────────────────────────────────────────────────────

TEST(SipBridge, OnTunnelDisconnect_ClosesAllBrowserConns)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);
    FakeBrowser b1, b2, b3;
    bridge.on_browser_upgrade(&b1, "{}");
    bridge.on_browser_upgrade(&b2, "{}");
    bridge.on_browser_upgrade(&b3, "{}");

    bridge.on_tunnel_disconnect();

    EXPECT_TRUE(b1.closed);
    EXPECT_TRUE(b2.closed);
    EXPECT_TRUE(b3.closed);
    EXPECT_EQ("tunnel_lost", b1.close_reason);
    EXPECT_EQ(0u, bridge.active_streams());
}

// ── reconnect ─────────────────────────────────────────────────────────────────

// ── PUSH_NOTIFY / CDR_PUSH handlers ──────────────────────────────────────────

TEST(SipBridge, OnTunnelPushNotify_InvokesPushHandler)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    std::vector<std::string> got;
    bridge.set_push_notify_handler(
        [&got](const std::string &payload) { got.push_back(payload); });

    const std::string payload = R"({"subscriberId":"u1","callerFlat":"A-101","callId":"abc"})";
    EXPECT_TRUE(bridge.on_tunnel_bytes(
        SipFrame::encode(SipFrame::Op::PUSH_NOTIFY, 0, payload)));

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(payload, got[0]);
}

TEST(SipBridge, OnTunnelCdrPush_InvokesCdrHandler)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    std::vector<std::string> got;
    bridge.set_cdr_push_handler(
        [&got](const std::string &payload) { got.push_back(payload); });

    const std::string payload = R"({"_id":"c1","societyId":"s1","durationSec":42})";
    EXPECT_TRUE(bridge.on_tunnel_bytes(
        SipFrame::encode(SipFrame::Op::CDR_PUSH, 0, payload)));

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(payload, got[0]);
}

TEST(SipBridge, OnTunnelRegisterState_InvokesRegisterStateHandler)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    std::vector<std::string> got;
    bridge.set_register_state_handler(
        [&got](const std::string &payload) { got.push_back(payload); });

    const std::string payload =
        R"({"societyId":"s1","sipUsername":"u_abc","online":true})";
    EXPECT_TRUE(bridge.on_tunnel_bytes(
        SipFrame::encode(SipFrame::Op::REGISTER_STATE, 0, payload)));

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(payload, got[0]);
}

TEST(SipBridge, OnTunnelRegisterState_NoHandler_IsSilentlyIgnored)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);  // no handler installed
    EXPECT_TRUE(bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::REGISTER_STATE, 0,
        R"({"societyId":"s1","sipUsername":"u_x","online":true})")))
        << "an un-handled control frame must not be a protocol error";
}

TEST(SipBridge, OnAgentReconnect_NewStreamIdsOnly)
{
    FakeTunnel  tun1;
    SipBridge   bridge(&tun1);
    FakeBrowser old_b1, old_b2;
    const auto old_id1 = bridge.on_browser_upgrade(&old_b1, "{}");
    const auto old_id2 = bridge.on_browser_upgrade(&old_b2, "{}");

    // Tunnel goes away: old browsers are closed, ids are dead.
    bridge.on_tunnel_disconnect();
    EXPECT_TRUE(old_b1.closed);
    EXPECT_TRUE(old_b2.closed);

    // New tunnel installed.
    FakeTunnel tun2;
    bridge.set_tunnel(&tun2);

    // New browsers registered → must get FRESH (different) stream-ids.
    FakeBrowser new_b;
    const auto new_id = bridge.on_browser_upgrade(&new_b, "{}");
    EXPECT_NE(old_id1, new_id);
    EXPECT_NE(old_id2, new_id);
    EXPECT_LT(old_id2, new_id);  // monotonic — counter did not reset

    // OPEN went to the NEW tunnel, not the old one.
    EXPECT_TRUE(tun1.sent.size() >= 2);  // only the two original OPENs
    ASSERT_EQ(1u, tun2.sent.size());
    EXPECT_EQ(SipFrame::Op::OPEN, tun2.sent[0].op);
    EXPECT_EQ(new_id,             tun2.sent[0].stream_id);
}

// ── IRevocationSink: revoke() emits SUBSCRIBER_REVOKED ────────────────────────

TEST(SipBridge, Revoke_EmitsSubscriberRevokedFrame)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    bridge.revoke("soc1", "u_abc123");

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::SUBSCRIBER_REVOKED, tun.sent[0].op);
    EXPECT_EQ(0u, tun.sent[0].stream_id) << "control frame — stream-id unused";
    // Payload carries the subscriber identity the agent tears down.
    EXPECT_NE(std::string::npos, tun.sent[0].payload.find(R"("societyId":"soc1")"));
    EXPECT_NE(std::string::npos,
              tun.sent[0].payload.find(R"("sipUsername":"u_abc123")"));
}

TEST(SipBridge, Revoke_NoTunnel_IsSilentNoOp)
{
    SipBridge bridge(nullptr);  // agent disconnected
    bridge.revoke("soc1", "u_abc123");  // must not crash
    // Nothing to assert beyond "didn't blow up" — the cloud-side
    // session/`/sip-ws` guards are the durable defence when the agent is down.
}

TEST(SipBridge, Revoke_AfterReconnect_GoesToNewTunnel)
{
    FakeTunnel tun1;
    SipBridge  bridge(&tun1);
    bridge.on_tunnel_disconnect();      // agent drops — tunnel detached

    FakeTunnel tun2;
    bridge.set_tunnel(&tun2);           // agent reconnects
    bridge.revoke("soc1", "u_x");

    EXPECT_TRUE(tun1.sent.empty());
    ASSERT_EQ(1u, tun2.sent.size());
    EXPECT_EQ(SipFrame::Op::SUBSCRIBER_REVOKED, tun2.sent[0].op);
}

// ── AGENT_HELLO / SOCIETY_BOOTSTRAP — realm bootstrap path ────────────────

TEST(SipBridge, OnTunnel_AgentHello_FiresHandlerWithPayload)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    std::string captured;
    bridge.set_agent_hello_handler(
        [&captured](const std::string &p) { captured = p; });

    // Inject an AGENT_HELLO frame as if it arrived from the agent.
    const std::string frame = SipFrame::encode(
        SipFrame::Op::AGENT_HELLO, 0, R"({"societyId":"soc1"})");
    EXPECT_TRUE(bridge.on_tunnel_bytes(frame));
    EXPECT_EQ(R"({"societyId":"soc1"})", captured);
}

// ── ASTERISK_STATUS — admin connectivity chip ─────────────────────────────

TEST(SipBridge, OnTunnel_AsteriskStatus_UpdatesCachedFlag)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    // Default is false (cold start).
    EXPECT_FALSE(bridge.asterisk_connected());

    // Agent reports Asterisk is up.
    bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::ASTERISK_STATUS, 0, R"({"connected":true})"));
    EXPECT_TRUE(bridge.asterisk_connected());

    // Agent reports Asterisk went down.
    bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::ASTERISK_STATUS, 0, R"({"connected":false})"));
    EXPECT_FALSE(bridge.asterisk_connected());
}

TEST(SipBridge, OnTunnel_AsteriskStatus_FiresOptionalHandlerWithPayload)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    std::string captured;
    bridge.set_asterisk_status_handler(
        [&captured](const std::string &p) { captured = p; });

    bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::ASTERISK_STATUS, 0, R"({"connected":true})"));
    EXPECT_EQ(R"({"connected":true})", captured);
}

TEST(SipBridge, OnTunnel_AsteriskStatus_BadJsonLeavesFlagUntouched)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    // Set the flag true via a valid frame …
    bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::ASTERISK_STATUS, 0, R"({"connected":true})"));
    ASSERT_TRUE(bridge.asterisk_connected());

    // … then send garbage. The bridge must NOT flip the flag based on
    // an unparseable payload — last good signal stands.
    bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::ASTERISK_STATUS, 0, "not-json"));
    EXPECT_TRUE(bridge.asterisk_connected());
}

TEST(SipBridge, OnTunnelDisconnect_ClearsAsteriskFlag)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    bridge.on_tunnel_bytes(SipFrame::encode(
        SipFrame::Op::ASTERISK_STATUS, 0, R"({"connected":true})"));
    ASSERT_TRUE(bridge.asterisk_connected());

    // Agent disconnects → the cached "true" no longer reflects reality.
    bridge.on_tunnel_disconnect();
    EXPECT_FALSE(bridge.asterisk_connected());
}

TEST(SipBridge, BootstrapSociety_EmitsSocietyBootstrapFrame)
{
    FakeTunnel tun;
    SipBridge  bridge(&tun);

    bridge.bootstrap_society("soc1", "acme.pbx.local");

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::SOCIETY_BOOTSTRAP, tun.sent[0].op);
    EXPECT_EQ(0u, tun.sent[0].stream_id);
    EXPECT_NE(std::string::npos, tun.sent[0].payload.find("acme.pbx.local"));
    EXPECT_NE(std::string::npos, tun.sent[0].payload.find("soc1"));
}

TEST(SipBridge, BootstrapSociety_NoTunnel_NoOp)
{
    SipBridge bridge(nullptr);
    bridge.bootstrap_society("soc1", "acme.pbx.local");
    // Must not crash; no tunnel to write to.
}

TEST(SipBridge, OnTunnel_SocietyBootstrap_IsDropped_EvenIfHandlerInstalled)
{
    // SOCIETY_BOOTSTRAP is cloud→agent only — if the agent ever echoes
    // one back the bridge must NOT route it through any handler (there
    // is no such handler on the cloud side, but we still want a tidy
    // drop, not a crash).
    FakeTunnel tun;
    SipBridge  bridge(&tun);
    bridge.set_agent_hello_handler(
        [](const std::string &) { FAIL() << "wrong handler fired"; });
    const std::string frame = SipFrame::encode(
        SipFrame::Op::SOCIETY_BOOTSTRAP, 0,
        R"({"societyId":"soc1","sipRealm":"acme.pbx.local"})");
    EXPECT_TRUE(bridge.on_tunnel_bytes(frame));
    EXPECT_TRUE(tun.sent.empty());
}
