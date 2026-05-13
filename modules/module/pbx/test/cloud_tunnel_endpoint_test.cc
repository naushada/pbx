#include "cloud_tunnel_endpoint.hpp"
#include "sip_bridge.hpp"
#include "sip_frame.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

// State outlives the IAgentTransport instance — same pattern as the agent's
// CloudConnector tests.
struct AgentTransportState {
  std::vector<std::string> sent;
  bool closed = false;
  bool fail_next_send = false;
};

class FakeAgentTransport : public IAgentTransport {
public:
  explicit FakeAgentTransport(AgentTransportState *state) : m_state(state) {}

  bool send(const std::string &bytes) override {
    if (m_state->fail_next_send) return false;
    m_state->sent.push_back(bytes);
    return true;
  }
  void close() override { m_state->closed = true; }

private:
  AgentTransportState *m_state;
};

/// Per-test factory keeps fakes' state vectors alive after the
/// `unique_ptr` is destroyed during disconnect.
class AgentTransportFactory {
public:
  std::vector<std::unique_ptr<AgentTransportState>> states;
  AgentTransportState &last() { return *states.back(); }

  std::unique_ptr<IAgentTransport> make() {
    auto s = std::make_unique<AgentTransportState>();
    AgentTransportState *raw = s.get();
    states.push_back(std::move(s));
    return std::make_unique<FakeAgentTransport>(raw);
  }
};

// Minimal BrowserSink — only needed to keep SipBridge happy in the
// tests that exercise the inbound DATA forwarding path.
class FakeBrowser : public BrowserSink {
public:
  std::vector<std::string> got;
  bool closed = false;
  void send_bytes(const std::string &b) override { got.push_back(b); }
  void close(const std::string &) override { closed = true; }
};

} // namespace

// ── Connection lifecycle ─────────────────────────────────────────────────────

TEST(CloudTunnelEndpoint, NoAgent_BeforeAttach)
{
    CloudTunnelEndpoint cte;
    EXPECT_FALSE(cte.has_agent());
    EXPECT_EQ(0u, cte.buffered_frame_count());
}

TEST(CloudTunnelEndpoint, AcceptsAgentAndExposesHasAgent)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;

    cte.on_agent_connected(fac.make());
    EXPECT_TRUE(cte.has_agent());
}

TEST(CloudTunnelEndpoint, OnAgentDisconnected_TellsBridge)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);

    cte.on_agent_connected(fac.make());

    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");
    EXPECT_EQ(1u, bridge.active_streams());

    cte.on_agent_disconnected();

    EXPECT_FALSE(cte.has_agent());
    EXPECT_TRUE(fac.last().closed) << "transport.close() called on disconnect";
    EXPECT_TRUE(b.closed)
        << "bridge.on_tunnel_disconnect() ripples through to browser sinks";
    EXPECT_EQ(0u, bridge.active_streams());

    // The stream-id counter does NOT reset (DESIGN.md §7). Verify by
    // registering a fresh browser after a reconnect.
    cte.on_agent_connected(fac.make());
    FakeBrowser b2;
    const auto new_sid = bridge.on_browser_upgrade(&b2, "{}");
    EXPECT_GT(new_sid, sid);
}

// ── TunnelSink semantics: connected ──────────────────────────────────────────

TEST(CloudTunnelEndpoint, SendFrameWhenConnected_WritesEncodedBytes)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    cte.on_agent_connected(fac.make());

    cte.send_frame(SipFrame::Op::DATA, 42, "to-agent");

    auto &ts = fac.last();
    ASSERT_EQ(1u, ts.sent.size());
    const auto r = SipFrame::decode(ts.sent[0]);
    ASSERT_EQ(SipFrame::Status::Ok, r.status);
    EXPECT_EQ(SipFrame::Op::DATA, r.frame.op);
    EXPECT_EQ(42u,                 r.frame.stream_id);
    EXPECT_EQ("to-agent",          r.frame.payload);
}

// ── TunnelSink semantics: disconnected → buffer ──────────────────────────────

TEST(CloudTunnelEndpoint, SendFrameWhenDisconnected_Buffers)
{
    CloudTunnelEndpoint cte;
    cte.send_frame(SipFrame::Op::CDR_PUSH, 0, "while-no-agent");
    cte.send_frame(SipFrame::Op::PUSH_NOTIFY, 0, "still-no-agent");

    EXPECT_EQ(2u, cte.buffered_frame_count());
}

TEST(CloudTunnelEndpoint, ReconnectAgentFlushesBuffer)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;

    cte.send_frame(SipFrame::Op::CDR_PUSH, 0, "first");
    cte.send_frame(SipFrame::Op::PUSH_NOTIFY, 0, "second");
    EXPECT_EQ(2u, cte.buffered_frame_count());

    cte.on_agent_connected(fac.make());

    EXPECT_EQ(0u, cte.buffered_frame_count())
        << "buffered frames must flush on agent attach";
    auto &ts = fac.last();
    ASSERT_EQ(2u, ts.sent.size());
    EXPECT_EQ(SipFrame::Op::CDR_PUSH,
              SipFrame::decode(ts.sent[0]).frame.op);
    EXPECT_EQ("first",
              SipFrame::decode(ts.sent[0]).frame.payload);
    EXPECT_EQ(SipFrame::Op::PUSH_NOTIFY,
              SipFrame::decode(ts.sent[1]).frame.op);
    EXPECT_EQ("second",
              SipFrame::decode(ts.sent[1]).frame.payload);
}

TEST(CloudTunnelEndpoint, AgentDisconnect_BufferSurvivesForNextAgent)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    cte.on_agent_connected(fac.make());

    // First send goes through.
    cte.send_frame(SipFrame::Op::CDR_PUSH, 0, "delivered");
    EXPECT_EQ(1u, fac.last().sent.size());

    cte.on_agent_disconnected();
    EXPECT_FALSE(cte.has_agent());

    // Frames produced during the outage are buffered for the next agent.
    cte.send_frame(SipFrame::Op::CDR_PUSH, 0, "during_outage");
    EXPECT_EQ(1u, cte.buffered_frame_count());

    // New agent attaches: the buffered frame is flushed.
    cte.on_agent_connected(fac.make());
    EXPECT_EQ(0u, cte.buffered_frame_count());
    ASSERT_EQ(1u, fac.last().sent.size());
    EXPECT_EQ("during_outage",
              SipFrame::decode(fac.last().sent[0]).frame.payload);
}

TEST(CloudTunnelEndpoint, SendMidFlightFailure_MarksDisconnectedAndBuffers)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    cte.on_agent_connected(fac.make());
    fac.last().fail_next_send = true;

    cte.send_frame(SipFrame::Op::CDR_PUSH, 0, "doomed");

    EXPECT_FALSE(cte.has_agent());
    EXPECT_EQ(1u, cte.buffered_frame_count())
        << "the frame that triggered the failure is re-queued";
}

// ── Inbound bytes from agent → bridge ────────────────────────────────────────

TEST(CloudTunnelEndpoint, OnBytesReceived_ForwardsIntoBridge)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);
    cte.on_agent_connected(fac.make());

    // Agent PINGs us — bridge should respond with PONG via the
    // endpoint's TunnelSink, which writes to the transport.
    const std::string ping = SipFrame::encode(SipFrame::Op::PING, 0, "");
    cte.on_bytes_received(ping);

    auto &ts = fac.last();
    ASSERT_EQ(1u, ts.sent.size());
    const auto r = SipFrame::decode(ts.sent[0]);
    EXPECT_EQ(SipFrame::Op::PONG, r.frame.op);
}

TEST(CloudTunnelEndpoint, OnBytesReceived_InvalidFrame_DropsTunnel)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);
    cte.on_agent_connected(fac.make());

    std::string bad = SipFrame::encode(SipFrame::Op::DATA, 1, "x");
    bad[0] = 0x77;  // bogus version → bridge.on_tunnel_bytes returns false
    cte.on_bytes_received(bad);

    EXPECT_FALSE(cte.has_agent());
    EXPECT_TRUE(fac.last().closed);
}

TEST(CloudTunnelEndpoint, OnBytesReceived_NoBridge_NoOp)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;  // no bridge attached
    cte.on_agent_connected(fac.make());

    cte.on_bytes_received(SipFrame::encode(SipFrame::Op::PING, 0, ""));
    // No bridge means no PONG (no one to compute the response).
    EXPECT_EQ(0u, fac.last().sent.size());
    EXPECT_TRUE(cte.has_agent()) << "missing bridge isn't a tunnel violation";
}

// ── Browser DATA round-trips through both halves ─────────────────────────────

TEST(CloudTunnelEndpoint, BrowserDataRoundTripsThroughBridgeAndAgent)
{
    AgentTransportFactory fac;
    CloudTunnelEndpoint cte;
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);
    cte.on_agent_connected(fac.make());

    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, R"({"sip":"u_x"})");
    auto &ts = fac.last();
    // First frame the agent saw is the OPEN.
    ASSERT_EQ(1u, ts.sent.size());
    EXPECT_EQ(SipFrame::Op::OPEN, SipFrame::decode(ts.sent[0]).frame.op);

    bridge.on_browser_data(sid, "REGISTER sip:society SIP/2.0\r\n\r\n");
    ASSERT_EQ(2u, ts.sent.size());
    const auto data_frame = SipFrame::decode(ts.sent[1]);
    EXPECT_EQ(SipFrame::Op::DATA, data_frame.frame.op);
    EXPECT_EQ(sid,                data_frame.frame.stream_id);
    EXPECT_EQ("REGISTER sip:society SIP/2.0\r\n\r\n",
              data_frame.frame.payload);

    // Agent replies via the tunnel; bridge demuxes back to browser.
    const std::string reply = SipFrame::encode(
        SipFrame::Op::DATA, sid, "SIP/2.0 401 Unauthorized\r\n\r\n");
    cte.on_bytes_received(reply);

    ASSERT_EQ(1u, b.got.size());
    EXPECT_EQ("SIP/2.0 401 Unauthorized\r\n\r\n", b.got[0]);
}
