#include "cloud_connector.hpp"
#include "sip_frame.hpp"
#include "sip_frame_demux.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// FakeTransport — records sends, surfaces injected disconnects.
// Owned via unique_ptr by CloudConnector, but its state lives in a separate
// struct so the test can keep reading it after the transport is destroyed
// (same pattern as the SipFrameDemux suite's AsteriskState).
// ─────────────────────────────────────────────────────────────────────────────

struct TransportState {
  std::vector<std::string> sent;
  bool                     closed = false;
  bool                     fail_next_send = false;
};

class FakeTransport : public ITransport {
public:
  explicit FakeTransport(TransportState *state) : m_state(state) {}

  bool send(const std::string &bytes) override {
    if (m_state->fail_next_send) return false;
    m_state->sent.push_back(bytes);
    return true;
  }
  void close() override { m_state->closed = true; }

private:
  TransportState *m_state;
};

/// Scripted factory: returns either a fresh FakeTransport (whose state is
/// recorded in `states`) or nullptr to simulate a connect failure.
class FakeFactory : public ITransportFactory {
public:
  struct Attempt {
    std::string   host;
    std::uint16_t port;
    std::string   cert_path;
    std::string   key_path;
    std::string   ca_path;
  };
  std::vector<Attempt>                        attempts;
  std::vector<std::unique_ptr<TransportState>> states;
  std::deque<bool>                            outcomes; // true = succeed

  std::unique_ptr<ITransport>
  create_connected(const std::string &host, std::uint16_t port,
                   const std::string &cert, const std::string &key,
                   const std::string &ca) override {
    attempts.push_back({host, port, cert, key, ca});
    bool ok = true;
    if (!outcomes.empty()) {
      ok = outcomes.front();
      outcomes.pop_front();
    }
    if (!ok) return nullptr;

    auto state = std::make_unique<TransportState>();
    TransportState *raw = state.get();
    states.push_back(std::move(state));
    return std::make_unique<FakeTransport>(raw);
  }

  TransportState &last_state() { return *states.back(); }
};

class ManualClock : public IClock {
public:
  std::int64_t t = 0;
  std::int64_t now_unix() const override { return t; }
  void advance(std::int64_t sec) { t += sec; }
};

CloudConnector::Config default_cfg() {
  CloudConnector::Config c;
  c.host             = "agent.heroku.example";
  c.port             = 443;
  c.client_cert_path = "/etc/onprem-pbx/agent.crt";
  c.client_key_path  = "/etc/onprem-pbx/agent.key";
  c.server_ca_path   = "/etc/onprem-pbx/cloud-ca.pem";
  c.initial_backoff_sec = 1;
  c.max_backoff_sec     = 30;
  return c;
}

} // namespace

// ── Connect: mTLS material is forwarded to the factory ───────────────────────

TEST(CloudConnector, Connects_PresentsClientCert)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);

    cc.tick();  // first tick triggers the initial connect

    ASSERT_EQ(1u, fac.attempts.size());
    const auto &a = fac.attempts[0];
    EXPECT_EQ("agent.heroku.example",           a.host);
    EXPECT_EQ(443,                              a.port);
    EXPECT_EQ("/etc/onprem-pbx/agent.crt",      a.cert_path);
    EXPECT_EQ("/etc/onprem-pbx/agent.key",      a.key_path);
    EXPECT_EQ("/etc/onprem-pbx/cloud-ca.pem",   a.ca_path);
    EXPECT_TRUE(cc.connected());
}

TEST(CloudConnector, ConnectFailure_StaysDisconnected)
{
    FakeFactory fac;
    fac.outcomes.push_back(false);
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);

    cc.tick();
    EXPECT_FALSE(cc.connected());
    EXPECT_EQ(1, cc.reconnect_attempts());
}

// ── send_frame writes encoded SipFrame bytes ─────────────────────────────────

TEST(CloudConnector, SendsOpenFramesForNewStreams)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);
    cc.tick();  // connect
    ASSERT_TRUE(cc.connected());

    cc.send_frame(SipFrame::Op::OPEN, 7, R"({"sip":"u_x"})");

    auto &ts = fac.last_state();
    ASSERT_EQ(1u, ts.sent.size());

    // Decode what was sent and confirm it round-trips as OPEN(7).
    const auto r = SipFrame::decode(ts.sent[0]);
    ASSERT_EQ(SipFrame::Status::Ok, r.status);
    EXPECT_EQ(SipFrame::Op::OPEN,   r.frame.op);
    EXPECT_EQ(7u,                   r.frame.stream_id);
    EXPECT_EQ(R"({"sip":"u_x"})",   r.frame.payload);
}

TEST(CloudConnector, SendsPushNotifyAndCdrPushUpstream)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);
    cc.tick();

    cc.send_frame(SipFrame::Op::PUSH_NOTIFY, 0, R"({"subscriberId":"u1"})");
    cc.send_frame(SipFrame::Op::CDR_PUSH,    0, "{cdr}");

    auto &ts = fac.last_state();
    ASSERT_EQ(2u, ts.sent.size());
    EXPECT_EQ(SipFrame::Op::PUSH_NOTIFY,
              SipFrame::decode(ts.sent[0]).frame.op);
    EXPECT_EQ(SipFrame::Op::CDR_PUSH,
              SipFrame::decode(ts.sent[1]).frame.op);
}

// ── Reconnect backoff: 1s → 2s → 4s → 8s → 16s → 30s (capped) ────────────────

TEST(CloudConnector, AutoReconnectsWithBackoff)
{
    FakeFactory fac;
    // All connect attempts in this test fail.
    for (int i = 0; i < 10; ++i) fac.outcomes.push_back(false);

    ManualClock clk;
    auto cfg = default_cfg();
    cfg.initial_backoff_sec = 1;
    cfg.max_backoff_sec     = 30;
    CloudConnector cc(cfg, fac, clk);

    // 1st attempt at t=0.
    cc.tick();
    EXPECT_EQ(1, cc.reconnect_attempts());
    EXPECT_EQ(1, cc.current_backoff_sec());

    // Series: 1s → 2s → 4s → 8s → 16s → 30s (cap) → 30s
    const std::vector<int> expected = {1, 2, 4, 8, 16, 30, 30, 30};
    for (std::size_t i = 1; i < expected.size(); ++i) {
        // Advance just under the backoff: tick should NOT attempt yet.
        const int prev = expected[i - 1];
        clk.advance(prev - 1);
        cc.tick();
        EXPECT_EQ(static_cast<int>(i),     cc.reconnect_attempts())
            << "must not retry before backoff elapses (i=" << i << ")";
        // Now advance the remaining 1s; tick should attempt and bump backoff.
        clk.advance(1);
        cc.tick();
        EXPECT_EQ(static_cast<int>(i + 1), cc.reconnect_attempts());
        EXPECT_EQ(expected[i],              cc.current_backoff_sec());
    }
}

TEST(CloudConnector, SuccessfulReconnectResetsBackoff)
{
    FakeFactory fac;
    fac.outcomes = {false, false, true};  // fail, fail, succeed
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);

    cc.tick();                        // attempt 1: fail → 1s backoff
    EXPECT_EQ(1, cc.current_backoff_sec());

    clk.advance(1);
    cc.tick();                        // attempt 2: fail → 2s backoff
    EXPECT_EQ(2, cc.current_backoff_sec());

    clk.advance(2);
    cc.tick();                        // attempt 3: SUCCESS
    EXPECT_TRUE(cc.connected());
    EXPECT_EQ(0, cc.current_backoff_sec());
    EXPECT_EQ(0, cc.reconnect_attempts());
}

// ── Frame buffering across an intermittent tunnel drop ───────────────────────

TEST(CloudConnector, SurvivesIntermittentTunnelDrop)
{
    FakeFactory fac;
    fac.outcomes = {true, true};  // initial connect + reconnect both succeed
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);

    cc.tick();                            // connect
    EXPECT_TRUE(cc.connected());

    cc.send_frame(SipFrame::Op::CDR_PUSH, 0, "first");
    EXPECT_EQ(1u, fac.last_state().sent.size());

    // Simulate the cloud dropping the tunnel.
    cc.on_transport_lost();
    EXPECT_FALSE(cc.connected());
    EXPECT_EQ(0u, cc.buffered_frame_count());

    // Frames sent while disconnected are buffered.
    cc.send_frame(SipFrame::Op::CDR_PUSH, 0, "buffered_1");
    cc.send_frame(SipFrame::Op::PUSH_NOTIFY, 0, "buffered_2");
    EXPECT_EQ(2u, cc.buffered_frame_count());

    // Reconnect on next tick (next_reconnect_at == 0 after transport_lost).
    cc.tick();
    EXPECT_TRUE(cc.connected());
    EXPECT_EQ(0u, cc.buffered_frame_count())
        << "buffered frames must flush on successful reconnect";

    // Flushed frames arrived in order on the NEW transport.
    auto &new_ts = fac.last_state();
    ASSERT_EQ(2u, new_ts.sent.size());
    EXPECT_EQ(SipFrame::Op::CDR_PUSH,
              SipFrame::decode(new_ts.sent[0]).frame.op);
    EXPECT_EQ("buffered_1",
              SipFrame::decode(new_ts.sent[0]).frame.payload);
    EXPECT_EQ(SipFrame::Op::PUSH_NOTIFY,
              SipFrame::decode(new_ts.sent[1]).frame.op);
    EXPECT_EQ("buffered_2",
              SipFrame::decode(new_ts.sent[1]).frame.payload);
}

TEST(CloudConnector, SendDuringDisconnect_BuffersUntilReconnect)
{
    FakeFactory fac;
    fac.outcomes = {false, true};  // first attempt fails, second succeeds
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);

    cc.tick();  // fails → disconnected, 1s backoff
    EXPECT_FALSE(cc.connected());

    // While disconnected, frames get buffered.
    cc.send_frame(SipFrame::Op::CDR_PUSH, 0, "queue_me");
    EXPECT_EQ(1u, cc.buffered_frame_count());

    clk.advance(1);
    cc.tick();  // succeeds
    ASSERT_TRUE(cc.connected());
    EXPECT_EQ(0u, cc.buffered_frame_count());
    EXPECT_EQ(1u, fac.last_state().sent.size());
}

// ── on_bytes_received pipes into the demux ───────────────────────────────────

namespace {
class CountingDemux {  // a non-owning facade — we wrap real SipFrameDemux below
public:
};

class NopFactory : public IAsteriskFactory {
public:
  std::unique_ptr<IAsteriskStream> open(std::uint32_t, const std::string&) override {
    return nullptr;  // we don't care about open behaviour here
  }
};
}

TEST(CloudConnector, OnBytesReceived_ForwardsIntoDemux)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);
    cc.tick();

    NopFactory   ast_fac;
    SipFrameDemux demux(&cc, ast_fac);
    cc.attach_demux(&demux);

    // Send a PING frame inbound; demux should reply PONG via cc (which writes
    // to the FakeTransport).
    const std::string ping = SipFrame::encode(SipFrame::Op::PING, 0, "");
    cc.on_bytes_received(ping);

    auto &ts = fac.last_state();
    ASSERT_EQ(1u, ts.sent.size());
    const auto r = SipFrame::decode(ts.sent[0]);
    EXPECT_EQ(SipFrame::Op::PONG, r.frame.op);
}

TEST(CloudConnector, OnBytesReceived_InvalidFrame_DropsTunnel)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);
    cc.tick();
    ASSERT_TRUE(cc.connected());

    NopFactory   ast_fac;
    SipFrameDemux demux(&cc, ast_fac);
    cc.attach_demux(&demux);

    std::string bad = SipFrame::encode(SipFrame::Op::DATA, 1, "x");
    bad[0] = 0x77;  // bogus version
    cc.on_bytes_received(bad);

    EXPECT_FALSE(cc.connected());
}

// ── Send mid-flight failure marks disconnect ─────────────────────────────────

TEST(CloudConnector, SendMidFlightFailure_MarksDisconnectedAndBuffers)
{
    FakeFactory fac;
    fac.outcomes = {true};  // initial connect succeeds
    ManualClock clk;
    CloudConnector cc(default_cfg(), fac, clk);
    cc.tick();
    ASSERT_TRUE(cc.connected());

    fac.last_state().fail_next_send = true;
    cc.send_frame(SipFrame::Op::CDR_PUSH, 0, "doomed");

    EXPECT_FALSE(cc.connected());
    EXPECT_EQ(1u, cc.buffered_frame_count())
        << "the frame that triggered the failure should be buffered for retry";
}

// ── SipFrame-level heartbeat (DESIGN.md §7) ──────────────────────────────────

namespace {
// default_cfg() leaves the heartbeat at its 15s/3-miss production defaults;
// the heartbeat tests want a tighter, explicit interval to read cleanly.
CloudConnector::Config heartbeat_cfg(int interval_sec, int max_missed) {
    auto c = default_cfg();
    c.heartbeat_interval_sec = interval_sec;
    c.heartbeat_max_missed   = max_missed;
    return c;
}
} // namespace

TEST(CloudConnector, Heartbeat_SendsPingAfterIdleInterval)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(heartbeat_cfg(/*interval=*/10, /*max_missed=*/3), fac, clk);

    cc.tick();  // connect at t=0
    ASSERT_TRUE(cc.connected());
    auto &ts = fac.last_state();
    ASSERT_EQ(0u, ts.sent.size());

    // A tick before the interval elapses must NOT send a heartbeat.
    clk.advance(9);
    cc.tick();
    EXPECT_EQ(0u, ts.sent.size()) << "no heartbeat before the interval elapses";

    // Crossing the interval sends exactly one connection-level PING.
    clk.advance(1);  // t=10
    cc.tick();
    ASSERT_EQ(1u, ts.sent.size());
    const auto r = SipFrame::decode(ts.sent[0]);
    ASSERT_EQ(SipFrame::Status::Ok, r.status);
    EXPECT_EQ(SipFrame::Op::PING, r.frame.op);
    EXPECT_EQ(0u,                 r.frame.stream_id);
    EXPECT_TRUE(r.frame.payload.empty());
    EXPECT_EQ(1, cc.pings_outstanding());
}

TEST(CloudConnector, Heartbeat_InboundBytesResetMissedCount)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(heartbeat_cfg(/*interval=*/10, /*max_missed=*/3), fac, clk);

    cc.tick();  // connect at t=0
    ASSERT_TRUE(cc.connected());

    // Five heartbeat cycles, each answered by inbound bytes. The miss
    // counter must keep returning to 0 so the tunnel is never dropped.
    for (int cycle = 0; cycle < 5; ++cycle) {
        clk.advance(10);
        cc.tick();
        EXPECT_EQ(1, cc.pings_outstanding()) << "cycle " << cycle;

        // Any inbound bytes count as proof of life (here: a PONG reply).
        cc.on_bytes_received(SipFrame::encode(SipFrame::Op::PONG, 0, ""));
        EXPECT_EQ(0, cc.pings_outstanding()) << "cycle " << cycle;
    }
    EXPECT_TRUE(cc.connected());
}

TEST(CloudConnector, Heartbeat_DropsTunnelAfterMaxMissedPings)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(heartbeat_cfg(/*interval=*/10, /*max_missed=*/3), fac, clk);

    cc.tick();  // connect at t=0
    ASSERT_TRUE(cc.connected());
    TransportState &dead = fac.last_state();

    // Three heartbeat intervals with zero inbound bytes: three PINGs go out,
    // the tunnel stays up (we haven't exceeded max_missed yet).
    for (int i = 1; i <= 3; ++i) {
        clk.advance(10);
        cc.tick();
        EXPECT_TRUE(cc.connected())   << "still connected after " << i << " missed";
        EXPECT_EQ(i, cc.pings_outstanding());
    }
    EXPECT_EQ(3u, dead.sent.size()) << "three PINGs were sent";

    // The fourth heartbeat occasion finds max_missed PINGs unanswered and
    // drops the tunnel instead of sending another.
    clk.advance(10);
    cc.tick();
    EXPECT_FALSE(cc.connected()) << "tunnel dropped after max_missed unanswered PINGs";
    EXPECT_TRUE(dead.closed);
    EXPECT_EQ(3u, dead.sent.size()) << "no PING sent on the drop tick";

    // The drop armed an immediate reconnect (next_reconnect_at == 0).
    EXPECT_EQ(0, cc.next_reconnect_at());
    cc.tick();
    EXPECT_TRUE(cc.connected()) << "reconnects on the next tick after a heartbeat drop";
    EXPECT_EQ(0, cc.pings_outstanding()) << "heartbeat state re-armed on reconnect";
}

TEST(CloudConnector, Heartbeat_DisabledWhenIntervalZero)
{
    FakeFactory fac;
    ManualClock clk;
    CloudConnector cc(heartbeat_cfg(/*interval=*/0, /*max_missed=*/3), fac, clk);

    cc.tick();  // connect at t=0
    ASSERT_TRUE(cc.connected());

    // Far past any sane interval; with the heartbeat disabled nothing is sent
    // and the tunnel is never declared dead.
    clk.advance(3600);
    for (int i = 0; i < 10; ++i) cc.tick();

    EXPECT_TRUE(cc.connected());
    EXPECT_EQ(0u, fac.last_state().sent.size());
    EXPECT_EQ(0, cc.pings_outstanding());
}
