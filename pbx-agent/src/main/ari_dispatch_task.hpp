#ifndef ARI_DISPATCH_TASK_HPP
#define ARI_DISPATCH_TASK_HPP

#include "ace/Task.h"
#include "ace/Synch_Traits.h"

#include <atomic>
#include <functional>

/**
 * @file ari_dispatch_task.hpp
 * @brief Active-Object worker that keeps blocking ARI work off the
 *        reactor thread.
 *
 * @par Why this exists
 * `pbx-agent` runs a single ACE reactor thread. The ARI events
 * WebSocket is read by `AriWsClient::handle_input` on that thread, and
 * — in the very same callstack — the parsed event was handed straight
 * to `AriClient::on_event`, which drives `CallRouter`, which makes
 * **blocking** `/ari` REST round-trips via `AriRestClient` (originate,
 * create_bridge, add_channel_to_bridge, hangup). While that call-setup
 * burst blocked, the reactor thread was not draining the events
 * WebSocket:
 *
 * @code
 *   reactor → AriWsClient::handle_input
 *           → drain_frames()
 *           → on_event(json)
 *           → AriClient::on_event → CallRouter → AriRestClient::*
 *                                  (blocking TCP connect+send+recv,
 *                                   one round-trip each)
 *           ← reactor blocked the whole time → /ari/events not read
 * @endcode
 *
 * Asterisk's `websocket_write_timeout` (100ms by default) then fires on
 * its side, it closes the ARI socket, the agent reconnects, the Stasis
 * app is destroyed+recreated, the in-Stasis channels re-fire
 * StasisStart, and the cycle repeats every ~3s for the whole call — so
 * `CallRouter` never completes originate→answer→bridge and calls stall
 * at "Calling..." with no audio.
 *
 * `AriDispatchTask` is the worker side of an Active Object (POSA vol. 2;
 * the exact shape as the agent's `InnerTlsDispatchTask` and xpmile's
 * `MicroService`). The reactor becomes a pure producer: `AriWsClient`'s
 * callback pushes a closure onto the task's queue via `dispatch()` and
 * returns immediately, so `handle_input` keeps draining the events
 * WebSocket. The task's `svc()` loop runs on its own thread, pops the
 * queue, and runs the closure there — so the blocking `AriRestClient`
 * round-trips never starve the reactor.
 *
 * @par Threading contract
 * The worker is a *single* thread, so closures run one-at-a-time in
 * producer order. Routing **every** `AriClient` entry point through this
 * task (`on_event`, `revoke_subscriber`, `publish_register_snapshot`)
 * keeps `AriClient` / `CallRouter` single-threaded — they keep their
 * "no internal locking" invariant, just on the worker thread instead of
 * the reactor. `MongodbClient` is `mongocxx::pool`-backed so it is safe
 * to share with the reactor-thread `SubscriberWatcher`. The worker's
 * closures may call `CloudConnector::send_frame`, which the agent
 * already reaches from a non-reactor thread (the `InnerTlsDispatchTask`
 * worker drains inbound cloud bytes the same way).
 *
 * @par Test seam
 * `set_synchronous_for_test()` makes `dispatch()` run the closure inline
 * on the caller's thread — no worker, no queue, deterministic ordering
 * for googletest. Production always calls `start()` to spawn the worker.
 *
 * @par Lifecycle
 *   `start()` → `dispatch()` (many) → `stop()`.
 * `stop()` is idempotent and joins the worker thread; the destructor
 * calls it.
 */
class AriDispatchTask : public ACE_Task<ACE_MT_SYNCH> {
public:
  /// A unit of deferred work. Captures whatever the producer needs
  /// (typically the AriClient by reference + the event payload by
  /// value). Runs on the svc() thread — mind capture lifetimes: the
  /// canonical use captures process-lifetime objects from `main()`.
  using Job = std::function<void()>;

  AriDispatchTask();
  ~AriDispatchTask() override;

  AriDispatchTask(const AriDispatchTask &)            = delete;
  AriDispatchTask &operator=(const AriDispatchTask &) = delete;

  /// Spawn the worker thread. Call once before any `dispatch()`.
  /// Returns 0 on success, -1 on `activate()` failure.
  int start();

  /// Drain undelivered jobs, post a poison-pill, and join the worker
  /// thread. Idempotent — safe to call from the destructor.
  void stop();

  /// Enqueue @p job for asynchronous execution on the worker thread.
  /// Thread-safe. Returns false if the task is stopped or the queue
  /// rejected the enqueue. In synchronous test mode the job runs inline
  /// on the caller's thread before returning.
  bool dispatch(Job job);

  /// Test-only: make `dispatch()` run jobs inline. After this, `start()`
  /// spawns no worker thread. Idempotent.
  void set_synchronous_for_test();

  /// True between `start()` and `stop()`. Observable for tests.
  bool running() const { return m_continue.load(); }

  // ── ACE_Task hook ─────────────────────────────────────────────────────

  int svc() override;

private:
  /// Set by `start()`; cleared by `stop()` or by `svc()` observing the
  /// poison pill. Atomic — producers consult it concurrently with the
  /// worker resetting it.
  std::atomic<bool> m_continue{false};

  /// Inline mode for unit tests. No queue, no worker thread.
  std::atomic<bool> m_synchronous{false};

  /// Set by `stop()` so a second call short-circuits without
  /// re-poisoning the (already drained) queue.
  std::atomic<bool> m_stop_called{false};
};

#endif // ARI_DISPATCH_TASK_HPP
