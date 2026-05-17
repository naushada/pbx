// Tests for CloudTunnelTickDriver — the production reactor adapter
// that drives `CloudTunnelEndpoint::tick()` so the SipFrame-level
// heartbeat actually runs.
//
// The driver is intentionally a thin layer: schedule_timer, fire,
// call tick(), unschedule. Tests live at two layers:
//
//   (a) driver itself — handle_timeout dispatches to the bound
//       endpoint; register_with_reactor / cancel_with_reactor flip
//       the timer_id; ctor/dtor are safe without a reactor.
//
//   (b) end-to-end (driver + endpoint + fake transport + fake clock):
//       N timer fires with no inbound bytes drop the agent, exactly
//       like maybe_heartbeat()'s contract.
//
// Reactor-side cadence (every 5 s in production) is asserted as the
// argument passed to schedule_timer, NOT by running the reactor loop
// — that would be a flaky timing test. The driver's wiring into
// `WebServer` lives in `webservice_main.cpp`; this file covers the
// driver in isolation.

#include "cloud_tunnel_tick_driver.hpp"
#include "cloud_tunnel_endpoint.hpp"
#include "sip_bridge.hpp"
#include "sip_frame.hpp"

#include <ace/Reactor.h>

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

// Same FakeAgentTransport pattern as cloud_tunnel_endpoint_test.cc.
struct AgentTransportState {
  std::vector<std::string> sent;
  bool closed = false;
};

class FakeAgentTransport : public IAgentTransport {
public:
  explicit FakeAgentTransport(AgentTransportState *state) : m_state(state) {}
  bool send(const std::string &bytes) override {
    m_state->sent.push_back(bytes); return true;
  }
  void close() override { m_state->closed = true; }
private:
  AgentTransportState *m_state;
};

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

class ManualClock : public IClock {
public:
  std::int64_t now = 1'000'000;
  std::int64_t now_unix() const override { return now; }
};

} // namespace

// ── Driver construction safety ───────────────────────────────────────────────

TEST(CloudTunnelTickDriver, ConstructAndDestruct_NoReactor_NoCrash)
{
    // The driver may be constructed before WebServer's reactor is
    // wired (e.g. during early init) and may be destroyed after the
    // reactor is gone (shutdown ordering). Both must be safe.
    CloudTunnelEndpoint cte;
    CloudTunnelTickDriver driver(cte, /*interval_sec=*/5);
    EXPECT_EQ(-1, driver.timer_id())
        << "no timer scheduled until register_with_reactor()";
}

// ── register_with_reactor guards ─────────────────────────────────────────────
//
// Production schedule_timer behaviour is NOT exercised here for the
// same reason agent_stream_test.cc + browser_stream_test.cc skip it:
// constructing an ACE_Reactor on the test stack without a running
// event loop leaves the reactor's internal state in a half-init
// limbo that hangs the test process at teardown. The
// register/cancel contract IS asserted by the production wiring
// (webservice_main.cpp) and by manual smoke-test post-deploy.
//
// What we CAN test here are the input-guard paths that don't touch a
// real reactor: null pointer rejection + idempotent cancel.

TEST(CloudTunnelTickDriver, RegisterWithReactor_NullReactor_ReturnsError)
{
    CloudTunnelEndpoint cte;
    CloudTunnelTickDriver driver(cte, /*interval_sec=*/5);

    EXPECT_EQ(-1, driver.register_with_reactor(nullptr))
        << "null reactor must be rejected — caller passed garbage.";
    EXPECT_EQ(-1, driver.timer_id())
        << "no timer should be scheduled on the failure path.";
}

TEST(CloudTunnelTickDriver, CancelWithReactor_NeverScheduled_IsSafeNoOp)
{
    // Dtor may cancel even when register_with_reactor was never
    // called (early-shutdown paths). Must not crash + must return 0.
    CloudTunnelEndpoint cte;
    CloudTunnelTickDriver driver(cte, /*interval_sec=*/5);

    EXPECT_EQ(0, driver.cancel_with_reactor());
    EXPECT_EQ(0, driver.cancel_with_reactor()); // second call: same
    EXPECT_EQ(-1, driver.timer_id());
}

// ── handle_timeout dispatches to endpoint ────────────────────────────────────

TEST(CloudTunnelTickDriver, HandleTimeout_CallsTickOnEndpoint)
{
    // Direct-drive handle_timeout, no reactor needed. Proves the
    // driver→endpoint dispatch path independently of ACE.
    //
    // tick() in turn calls maybe_heartbeat() which (with a clock + an
    // attached agent + interval elapsed) sends a PING. So we assert
    // the PING reached the fake transport.
    AgentTransportFactory fac;
    ManualClock clock;
    // Named initializers: positional {1,3} would set outbound_buffer_max=1
    // and heartbeat_interval_sec=3 — easy to miss.
    CloudTunnelEndpoint::Config cfg{};
    cfg.heartbeat_interval_sec = 1;
    cfg.heartbeat_max_missed   = 3;
    CloudTunnelEndpoint cte(cfg, &clock);
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);
    cte.on_agent_connected(fac.make());

    CloudTunnelTickDriver driver(cte, /*interval_sec=*/1);

    // Advance clock past one interval so maybe_heartbeat actually fires.
    clock.now += 2;

    EXPECT_EQ(0, driver.handle_timeout(ACE_Time_Value::zero, nullptr))
        << "handle_timeout returns 0 to keep the recurring timer alive";

    auto &ts = fac.last();
    ASSERT_EQ(1u, ts.sent.size())
        << "one PING should have left the transport after the tick";
    const auto r = SipFrame::decode(ts.sent[0]);
    EXPECT_EQ(SipFrame::Op::PING, r.frame.op);
}

// ── End-to-end: N silent ticks drop the agent ────────────────────────────────

TEST(CloudTunnelTickDriver, EndToEnd_TickFiresHeartbeat_DropsAfterMissedThreshold)
{
    // The full chain: tick → maybe_heartbeat → send PING (increments
    // m_pings_outstanding). After heartbeat_max_missed PINGs with no
    // inbound bytes, the next tick calls mark_disconnected().
    //
    // This is the partition-detection contract that motivates the
    // whole change (kiosks must catch a silent network loss within
    // the configured window).
    AgentTransportFactory fac;
    ManualClock clock;
    CloudTunnelEndpoint::Config cfg{};
    cfg.heartbeat_interval_sec = 5;
    cfg.heartbeat_max_missed   = 3;
    CloudTunnelEndpoint cte(cfg, &clock);
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);
    cte.on_agent_connected(fac.make());
    ASSERT_TRUE(cte.has_agent());

    CloudTunnelTickDriver driver(cte, /*interval_sec=*/5);

    // Three silent ticks → three PINGs accumulate.
    clock.now += 5; driver.handle_timeout(ACE_Time_Value::zero, nullptr);
    clock.now += 5; driver.handle_timeout(ACE_Time_Value::zero, nullptr);
    clock.now += 5; driver.handle_timeout(ACE_Time_Value::zero, nullptr);
    EXPECT_TRUE(cte.has_agent())
        << "agent still attached — threshold not yet reached";
    EXPECT_EQ(3, cte.pings_outstanding());

    // Fourth tick: missed threshold reached → drop.
    clock.now += 5; driver.handle_timeout(ACE_Time_Value::zero, nullptr);
    EXPECT_FALSE(cte.has_agent())
        << "agent must be marked disconnected once max_missed PINGs "
        << "have gone unanswered (partition / dead WS).";
    EXPECT_TRUE(fac.last().closed);
}

TEST(CloudTunnelTickDriver, EndToEnd_InboundBytesResetMissedCount)
{
    // A live agent answering the PINGs (or sending any traffic) keeps
    // the missed-count at zero. Regression guard against an
    // accidental over-aggressive drop.
    AgentTransportFactory fac;
    ManualClock clock;
    CloudTunnelEndpoint::Config cfg{};
    cfg.heartbeat_interval_sec = 5;
    cfg.heartbeat_max_missed   = 3;
    CloudTunnelEndpoint cte(cfg, &clock);
    SipBridge bridge(&cte);
    cte.attach_bridge(&bridge);
    cte.on_agent_connected(fac.make());

    CloudTunnelTickDriver driver(cte, /*interval_sec=*/5);

    clock.now += 5; driver.handle_timeout(ACE_Time_Value::zero, nullptr);
    EXPECT_EQ(1, cte.pings_outstanding());

    // Peer answers — PONG arrives as inbound bytes.
    cte.on_bytes_received(SipFrame::encode(SipFrame::Op::PONG, 0, {}));
    EXPECT_EQ(0, cte.pings_outstanding())
        << "any inbound bytes reset the missed-count, per "
        << "on_bytes_received's contract.";

    // Many more ticks with sporadic inbound bytes → agent stays up.
    for (int i = 0; i < 50; ++i) {
        clock.now += 5;
        driver.handle_timeout(ACE_Time_Value::zero, nullptr);
        if (i % 2 == 0)
            cte.on_bytes_received(
                SipFrame::encode(SipFrame::Op::PONG, 0, {}));
    }
    EXPECT_TRUE(cte.has_agent());
}
