# Cloud-side `/agent` tunnel liveness

**Status:** approved + implemented — PR pending merge
**Tracks:** task #31
**Scope:** `/agent` WS only. `/ws/db` symmetric fix is future TODO.

---

## What this PR actually does (post-investigation)

**Plot twist during implementation:** `CloudTunnelEndpoint::tick()` is
already wired in production via an inline `CloudTunnelHeartbeatTimer`
class in `webservice_main.cpp` — the original "missing driver"
framing was wrong. What's actually missing is:

- **Test coverage**: the inline class has zero tests.
- **Class separation**: the timer is buried in main with the rest of
  the wiring.
- **Tighter detection window**: 15 s × 3 = 45 s is fine but kiosks
  want faster.

So this PR refactors the inline class into a tested
`CloudTunnelTickDriver` (+ 6 unit tests), tightens defaults from
15 s to 5 s, and matches the production tick to the new
heartbeat interval.

## Why

The on-prem stack is going into 24/7 kiosks at society main gates —
unattended hardware that must stay reachable without daily restarts.
Today the agent's `/agent` WS to the cloud is held alive by:

1. **`AgentStream::handle_timeout`** (cloud side, per-connection): a
   25-second WS-PING (RFC 6455 opcode 0x9) timer scheduled in
   `register_with_reactor()`. **Already in production.** This is what
   stops Heroku's 55s idle router from dropping the connection.

2. **`CloudTunnelEndpoint::maybe_heartbeat`** (cloud side, above
   AgentStream): a 15-second `SipFrame::PING` heartbeat with a
   3-missed-PINGs threshold, then `mark_disconnected()`. **Code exists
   but is dead** — `tick()` is never called from a production driver.
   No reactor timer, no thread.

3. **Transport layer**: TCP keepalive / write-error detection on the
   underlying socket. OS-level, opportunistic.

What we **don't** have:

- A production driver that calls `CloudTunnelEndpoint::tick()`.
- Active partition detection — if the TCP write succeeds (buffer
  accepts) but the peer is unreachable, today we discover this only
  on the next signalling DATA frame, which during a kiosk's idle
  period could be many minutes away.

For a security gate kiosk a 45–60 s silent partition window means the
intercom misses live calls. We need detection within ~20 s.

---

## Design

Three concrete changes, all on the cloud side. No agent-side change,
no protocol change.

### Change 1: Wire `CloudTunnelEndpoint::tick()` to a production reactor timer

The existing heartbeat machinery (PING send + missed-counter +
`mark_disconnected()` after N misses) is sound — it just isn't driven.
Add a small adapter that schedules itself periodically and calls
`tick()` on the bound endpoint.

```cpp
// modules/module/pbx/inc/cloud_tunnel_tick_driver.hpp  (new)
class CloudTunnelTickDriver : public ACE_Event_Handler {
public:
  CloudTunnelTickDriver(CloudTunnelEndpoint &cte, int interval_sec);
  int register_with_reactor(ACE_Reactor *r);
  int cancel_with_reactor();
  int handle_timeout(const ACE_Time_Value &, const void *) override;
private:
  CloudTunnelEndpoint &m_cte;
  int                  m_interval_sec;
  long                 m_timer_id = -1;
};
```

Wire it in `modules/module/webservice/src/webservice_main.cpp`
alongside the existing CTE construction (1 new line at construction +
1 at shutdown).

**Why a new class instead of making `CloudTunnelEndpoint` itself
inherit `ACE_Event_Handler`?** CTE is already a `TunnelSink` and
indirectly bound to `SipBridge`. Layering reactor concerns onto it
makes test setup heavier and couples the endpoint to ACE specifically
(today CTE has no ACE dependency — easy to test). Adapter is a
cleaner seam.

### Change 2: Tighten the defaults so kiosks see faster recovery

Today:
- `heartbeat_interval_sec = 15`
- `heartbeat_max_missed = 3`
- Worst-case detection: **45 s** (Heroku will kill at 55 s anyway)

Proposed:
- `heartbeat_interval_sec = 5`
- `heartbeat_max_missed = 3`
- Worst-case detection: **15 s**
- Tick driver fires every 5 s (matches interval)

15 s window means a kiosk's silent partition is caught well before
the next call arrives, and well before Heroku's 55 s.

### Change 3: Explicit partition test in production wiring

Add a smoke test that confirms the wiring stays in place — if a future
refactor accidentally drops the `schedule_timer()` line in
`webservice_main`, today nothing notices. Drive the test via the
new `CloudTunnelTickDriver`:

- `CloudTunnelTickDriver_SchedulesPeriodically` — assert
  `m_timer_id != -1` after `register_with_reactor`, and the timer
  cadence matches the configured interval.
- `CloudTunnelTickDriver_HandleTimeoutCallsTickOnEndpoint` — direct
  call to `handle_timeout()`, assert CTE's `pings_outstanding` advances
  the way `maybe_heartbeat` expects.

Both lean on the existing `IClock` test fake.

The deeper "PING goes out, no PONG comes back within X seconds, drop
the tunnel" semantics are **already implemented** by
`maybe_heartbeat`'s missed-count logic — combined with the tightened
defaults from Change 2, partition detection is the same code path,
just driven now.

---

## TDD plan

Test layer order (bottom up):

| # | Test | What it pins |
|---|---|---|
| 1 | `CloudTunnelTickDriver_ConstructAndDestruct_NoReactor` | Can be created + destroyed without a reactor (e.g. shutdown order safety) |
| 2 | `CloudTunnelTickDriver_RegisterWithReactor_SchedulesTimer` | `register_with_reactor()` calls `schedule_timer` with the configured interval |
| 3 | `CloudTunnelTickDriver_HandleTimeout_CallsTick` | `handle_timeout()` invokes `m_cte.tick()` exactly once per fire |
| 4 | `CloudTunnelTickDriver_CancelWithReactor_StopsFiring` | `cancel_with_reactor()` cancels + sets m_timer_id back to -1 |
| 5 | `CloudTunnelEndpoint_Heartbeat_DriverDriven_DropsAfterMissedThreshold` | End-to-end at the CTE level: fake tick driver, fake clock, fake transport — N consecutive ticks with no inbound bytes drops the agent |

All of these live in a new `cloud_tunnel_tick_driver_test.cc`. Existing
`cloud_tunnel_endpoint_test.cc` (the 21 tests we already have) stays
unchanged — the heartbeat logic itself isn't touched, only its driver.

The `CloudTunnelTickDriver` itself can be exercised without a real
ACE_Reactor by inheriting `ACE_Reactor::reactor()`'s reset trick
(set reactor via `set_reactor(&fake)`). The `handle_timeout()` test
doesn't need any reactor at all — direct call.

---

## Documentation updates (same PR)

- `ARCHITECTURE.md` row "Heroku H15 idle drop": rewrite to reflect
  actual wiring (AgentStream's 25 s WS-PING **plus** the new
  CTE tick driver above it). Note that the inner-TLS SipFrame
  heartbeat had been dead code prior to this PR.
- New TODO in `docs/design/operations/known-issues.md`:
  "/ws/db tunnel needs symmetric liveness driver — same shape as
   the /agent one shipped in PR #XXX."

---

## Future TODO (not this PR)

- Apply the same tick-driver pattern to `/ws/db` (wsdbagent tunnel).
- Consider lifting the interval/max-missed values to env vars for
  operators who want different recovery aggressiveness.
- A "soft" detection mode where PING failure doesn't drop the tunnel
  but marks the agent as "suspect" — useful if we add multi-agent
  fan-out per society later.

---

## Open questions for review

1. **TickDriver lifetime / ownership**: who deletes it?
   - Option A: `WebServer` owns a `unique_ptr<CloudTunnelTickDriver>`,
     dtor cancels + deletes. Symmetric to how `WebServer` already
     owns CTE.
   - Option B: Driver self-deletes from `handle_close`. Matches
     `AgentStream`'s lifecycle.
   - I lean A — driver is process-lifetime, not connection-lifetime.

2. **Interval & threshold defaults** — confirm:
   - tick / heartbeat both at 5 s
   - max_missed at 3 (= 15 s window)
   - or do you want a tighter / looser default?

3. **Config flexibility now or later** — env-var overrides
   (`PBX_AGENT_HEARTBEAT_INTERVAL_SEC` /
   `PBX_AGENT_HEARTBEAT_MAX_MISSED`) in this PR, or punt to the
   future-TODO list?

4. **Logging** — what telemetry do you want on the new driver?
   - Quiet (LM_DEBUG only on schedule + cancel)
   - Verbose (LM_INFO on every tick — noisy, ~12k/min)
   - I lean quiet — LM_INFO only on `mark_disconnected` (already
     exists in `maybe_heartbeat`) and on schedule/cancel; LM_DEBUG
     per tick.

Answer those four and I'll write the tests + the production code in
that order.
