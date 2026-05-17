#ifndef CLOUD_TUNNEL_TICK_DRIVER_HPP
#define CLOUD_TUNNEL_TICK_DRIVER_HPP

#include <ace/Event_Handler.h>

class ACE_Reactor;
class CloudTunnelEndpoint;

/**
 * @file cloud_tunnel_tick_driver.hpp
 * @brief Reactor-side adapter that drives `CloudTunnelEndpoint::tick()`.
 *
 * `CloudTunnelEndpoint` has a SipFrame-level heartbeat
 * (`maybe_heartbeat`) plus a missed-PING threshold that drops the
 * agent on partition. Both live behind `tick()`. Until this driver
 * landed, nothing called `tick()` in production — the heartbeat was
 * dead code and partition detection took as long as the next
 * data-frame write failure.
 *
 * The driver is intentionally a thin layer: schedule a recurring
 * reactor timer at construction time, dispatch each fire to
 * `m_cte.tick()`. No state of its own beyond the timer id.
 *
 * Lifetime: process-lifetime, owned by `WebServer`. NOT
 * self-deleting; the owner cancels + deletes during shutdown.
 *
 * Scope: `/agent` tunnel only for now. `/ws/db` symmetric variant is
 * a future TODO (see docs/design/operations/cloud-tunnel-liveness.md).
 */
class CloudTunnelTickDriver : public ACE_Event_Handler {
public:
  /// @param cte           Endpoint whose `tick()` to drive.
  /// @param interval_sec  Reactor timer period. Production default 5 s
  ///                      (matches `CloudTunnelEndpoint::Config::
  ///                      heartbeat_interval_sec`). Tests pass small
  ///                      values to keep wall-clock cadence out of
  ///                      the assertion path; the
  ///                      `handle_timeout`-direct-drive style means
  ///                      the configured interval rarely matters for
  ///                      test logic, only for `schedule_timer`'s
  ///                      argument when reactor is involved.
  CloudTunnelTickDriver(CloudTunnelEndpoint &cte, int interval_sec);
  ~CloudTunnelTickDriver() override;

  CloudTunnelTickDriver(const CloudTunnelTickDriver &)            = delete;
  CloudTunnelTickDriver &operator=(const CloudTunnelTickDriver &) = delete;

  /// Schedule the recurring timer. Initial delay == interval (no
  /// hammer-on-startup). Returns 0 on success, -1 on schedule_timer
  /// failure. Safe to call only once per instance.
  int register_with_reactor(ACE_Reactor *r);

  /// Cancel the timer. Idempotent — second call is a safe no-op.
  /// Returns 0 always (mirrors how `cancel_timer` reports success
  /// even when there's nothing to cancel).
  int cancel_with_reactor();

  /// Reactor-thread entry point. Forwards to `m_cte.tick()` and
  /// returns 0 to keep the recurring timer alive. Direct-driven by
  /// tests as the simplest test seam.
  int handle_timeout(const ACE_Time_Value &tv, const void *act) override;

  /// Observability for tests: the active timer id, or -1 when
  /// nothing is scheduled / after cancel.
  long timer_id() const { return m_timer_id; }

private:
  CloudTunnelEndpoint &m_cte;
  int                  m_interval_sec;
  long                 m_timer_id = -1;
  ACE_Reactor         *m_reactor = nullptr;
};

#endif // CLOUD_TUNNEL_TICK_DRIVER_HPP
