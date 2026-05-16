#ifndef INNER_TLS_TASK_HPP
#define INNER_TLS_TASK_HPP

#include "ace/Task.h"
#include "ace/Synch_Traits.h"

#include <atomic>
#include <functional>
#include <string>

/**
 * @file inner_tls_task.hpp
 * @brief Active-Object dispatcher that breaks the reactor → callback →
 *        self-destruct UAF cycle in `AceSslTransport`.
 *
 * @par Why this exists
 * Before this task, `AceSslTransport::drain_frames` called the
 * `on_bytes` handler synchronously from inside its own reactor
 * callback. The handler chain is:
 *
 * @code
 *   reactor → AceSslTransport::handle_input
 *           → drain_frames()
 *           → m_on_bytes(plaintext)
 *           → CloudConnector::on_bytes_received
 *           → SipFrameDemux::feed
 *           → (handler may signal disconnect)
 *           → CloudConnector::mark_disconnected()
 *           → m_transport.reset()        ← destroys AceSslTransport
 *                                          while drain_frames is on the
 *                                          stack above it → SIGBUS on
 *                                          return to drain_frames.
 * @endcode
 *
 * Same UAF on the disconnect path: `notify_disconnect_once` ->
 * `m_on_disconnect` -> mark_disconnected -> reset(this).
 *
 * `InnerTlsDispatchTask` is the worker side of an Active Object pattern
 * (POSA volume 2; mirrors xpmile's `MicroService` shape used in
 * `modules/module/webservice`). The reactor becomes a pure producer:
 * it pushes plaintext (or a disconnect signal) onto the task's queue
 * via `dispatch_bytes()` / `dispatch_disconnect()` and returns
 * immediately. The task's `svc()` loop runs on its own thread, pops the
 * queue, and invokes the handlers there — so any recursive call to
 * `CloudConnector::mark_disconnected()` from inside a handler destroys
 * the transport from outside the reactor's call frame.
 *
 * @par Test seam
 * Tests can call @c set_synchronous_for_test() to make `dispatch_*`
 * invoke the handlers inline on the caller's thread. No worker thread,
 * no queue — deterministic ordering for googletest. Production code
 * always calls @c start() to spawn the worker.
 *
 * @par Lifecycle
 *   `start()` → `dispatch_*()` (many) → `stop()`.
 * @c stop() is idempotent and joins the worker thread.
 */
class InnerTlsDispatchTask : public ACE_Task<ACE_MT_SYNCH> {
public:
  using PlaintextHandler  = std::function<void(const std::string &)>;
  using DisconnectHandler = std::function<void()>;

  /// Both handlers run on the svc() thread (or inline in test mode).
  /// They are owned by this task — be mindful of capture lifetimes
  /// (the canonical use captures the CloudConnector by reference, and
  /// the connector outlives the task by construction in `main()`).
  InnerTlsDispatchTask(PlaintextHandler on_bytes,
                        DisconnectHandler on_disconnect);

  ~InnerTlsDispatchTask() override;

  InnerTlsDispatchTask(const InnerTlsDispatchTask &)            = delete;
  InnerTlsDispatchTask &operator=(const InnerTlsDispatchTask &) = delete;

  /// Spawn the worker thread. Call once before any dispatch_*.
  /// Returns 0 on success, -1 on `activate()` failure.
  int start();

  /// Drain undelivered messages, post a poison-pill, and join the
  /// worker thread. Idempotent — safe to call from the destructor.
  void stop();

  /// Enqueue plaintext for asynchronous delivery to the on_bytes
  /// handler. Thread-safe. Returns false if the task is stopped or the
  /// underlying queue rejected the enqueue.
  bool dispatch_bytes(std::string bytes);

  /// Enqueue a disconnect signal. Thread-safe. Returns false if the
  /// task is stopped. At-most-once dispatch is the caller's
  /// responsibility (AceSslTransport already enforces this via
  /// `notify_disconnect_once`).
  bool dispatch_disconnect();

  /// Test-only: make dispatch_* run handlers inline. After this, no
  /// worker thread will be spawned by `start()`. Idempotent.
  void set_synchronous_for_test();

  /// True between `start()` and `stop()`. Observable for tests.
  bool running() const { return m_continue.load(); }

  // ── ACE_Task hooks ────────────────────────────────────────────────────

  int svc() override;

private:
  PlaintextHandler  m_on_bytes;
  DisconnectHandler m_on_disconnect;

  /// Set by `start()`; cleared by `stop()` or by `svc()` observing the
  /// poison pill. Atomic because producers consult it concurrently with
  /// the worker resetting it.
  std::atomic<bool> m_continue{false};

  /// Inline mode for unit tests. No queue, no worker thread.
  std::atomic<bool> m_synchronous{false};

  /// Set by `stop()` so a second call short-circuits without re-poisoning.
  std::atomic<bool> m_stop_called{false};
};

#endif // INNER_TLS_TASK_HPP
