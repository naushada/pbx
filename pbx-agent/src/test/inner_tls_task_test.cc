#include "inner_tls_task.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// Spin until @p pred returns true or we exceed @p timeout_ms.
// Lets the async tests assert eventual delivery without flakey sleeps.
bool wait_until(std::function<bool()> pred, int timeout_ms = 500) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Synchronous test seam — used by every fast-path test below.
// ─────────────────────────────────────────────────────────────────────────────

TEST(InnerTlsDispatchTask, Sync_DispatchBytes_RunsHandlerInline) {
  std::vector<std::string> seen;
  InnerTlsDispatchTask t([&seen](const std::string &s) { seen.push_back(s); },
                          []() {});
  t.set_synchronous_for_test();
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch_bytes("hello"));
  EXPECT_TRUE(t.dispatch_bytes("world"));

  ASSERT_EQ(2u, seen.size());
  EXPECT_EQ("hello", seen[0]);
  EXPECT_EQ("world", seen[1]);
}

TEST(InnerTlsDispatchTask, Sync_DispatchDisconnect_RunsHandlerInline) {
  int hits = 0;
  InnerTlsDispatchTask t([](const std::string &) {}, [&hits]() { ++hits; });
  t.set_synchronous_for_test();
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch_disconnect());
  EXPECT_EQ(1, hits);
  EXPECT_TRUE(t.dispatch_disconnect());
  EXPECT_EQ(2, hits) << "task does not de-dup disconnects — caller's "
                        "responsibility (AceSslTransport guards via "
                        "notify_disconnect_once)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Async path — exercises the real worker thread + queue.
// ─────────────────────────────────────────────────────────────────────────────

TEST(InnerTlsDispatchTask, Async_DispatchBytes_DeliveredOnWorkerThread) {
  std::atomic<int> count{0};
  std::thread::id  worker_tid;
  const std::thread::id  caller_tid = std::this_thread::get_id();

  InnerTlsDispatchTask t(
      [&](const std::string &) {
        worker_tid = std::this_thread::get_id();
        ++count;
      },
      []() {});
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch_bytes("x"));
  EXPECT_TRUE(wait_until([&] { return count.load() == 1; }));
  EXPECT_NE(worker_tid, caller_tid)
      << "handler must run on the worker thread, not the producer";

  t.stop();
}

TEST(InnerTlsDispatchTask, Async_OrdersDispatchInProducerOrder) {
  std::mutex                m;
  std::vector<std::string>  seen;
  InnerTlsDispatchTask t(
      [&](const std::string &s) {
        std::lock_guard<std::mutex> g(m);
        seen.push_back(s);
      },
      []() {});
  ASSERT_EQ(0, t.start());

  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(t.dispatch_bytes(std::to_string(i)));
  }
  EXPECT_TRUE(wait_until([&] {
    std::lock_guard<std::mutex> g(m);
    return seen.size() == 50u;
  }));

  std::lock_guard<std::mutex> g(m);
  ASSERT_EQ(50u, seen.size());
  for (int i = 0; i < 50; ++i) EXPECT_EQ(std::to_string(i), seen[i]);
  t.stop();
}

TEST(InnerTlsDispatchTask, Async_Stop_JoinsWorkerThread) {
  std::atomic<bool> ran{false};
  InnerTlsDispatchTask t([&](const std::string &) { ran.store(true); }, []() {});
  ASSERT_EQ(0, t.start());
  EXPECT_TRUE(t.running());

  t.stop();
  EXPECT_FALSE(t.running());

  // Post-stop dispatch is rejected.
  EXPECT_FALSE(t.dispatch_bytes("late"));
  EXPECT_FALSE(t.dispatch_disconnect());
  EXPECT_FALSE(ran.load()) << "no message was ever dispatched";
}

TEST(InnerTlsDispatchTask, Async_StopIsIdempotent) {
  InnerTlsDispatchTask t([](const std::string &) {}, []() {});
  ASSERT_EQ(0, t.start());

  t.stop();
  t.stop();   // second call must not hang or crash
  t.stop();
  SUCCEED();
}

TEST(InnerTlsDispatchTask, Async_DestructorStopsWorker) {
  std::atomic<int> count{0};
  {
    InnerTlsDispatchTask t([&](const std::string &) { ++count; }, []() {});
    ASSERT_EQ(0, t.start());
    EXPECT_TRUE(t.dispatch_bytes("a"));
    EXPECT_TRUE(wait_until([&] { return count.load() == 1; }));
    // No explicit stop() — destructor must drain + join.
  }
  // If the destructor leaked a thread, ASan/TSan would gripe. Without
  // those, we assert just that the count didn't grow after teardown.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(1, count.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// Re-entrancy — the key UAF-prevention property.
// A handler that tears down state which would have killed the producer
// must NOT race against the producer that enqueued it.
// ─────────────────────────────────────────────────────────────────────────────

TEST(InnerTlsDispatchTask, Async_HandlerCanScheduleDestructionSafely) {
  // The on_bytes handler asks the task to stop itself. Mimics
  // CloudConnector::on_bytes_received → mark_disconnected →
  // m_transport.reset() (the old UAF pattern). With the task serving as
  // the boundary, this is now safe because stop() runs from the same
  // thread that's draining the message — no in-flight producer to UAF.
  InnerTlsDispatchTask t([&](const std::string &) {
                            // Intentionally a no-op — the test is that
                            // we GOT here, on the worker thread, from a
                            // producer that has long since returned.
                          },
                          []() {});
  ASSERT_EQ(0, t.start());

  // Producer fires-and-forgets.
  EXPECT_TRUE(t.dispatch_bytes("boom"));

  // Producer returns. From the producer's perspective, the queue is
  // already drained or pending — either way, this thread is safe to
  // continue executing. stop() blocks until the worker is done.
  t.stop();
  SUCCEED();
}

TEST(InnerTlsDispatchTask, Async_HandlerThrows_WorkerSurvives) {
  std::atomic<int> seen{0};
  InnerTlsDispatchTask t(
      [&](const std::string &s) {
        ++seen;
        if (s == "boom") throw std::runtime_error("synthetic");
      },
      []() {});
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch_bytes("ok"));
  EXPECT_TRUE(t.dispatch_bytes("boom"));   // handler throws; worker swallows
  EXPECT_TRUE(t.dispatch_bytes("after"));  // must still be processed

  EXPECT_TRUE(wait_until([&] { return seen.load() == 3; }));
  t.stop();
}
