#include "ari_dispatch_task.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Spin until @p pred is true or @p timeout_ms elapses. Lets the async
// tests assert eventual delivery without flakey fixed sleeps.
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
// Synchronous test seam — jobs run inline on the caller's thread.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AriDispatchTask, Sync_Dispatch_RunsJobInline) {
  std::vector<int> seen;
  AriDispatchTask t;
  t.set_synchronous_for_test();
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch([&seen]() { seen.push_back(1); }));
  EXPECT_TRUE(t.dispatch([&seen]() { seen.push_back(2); }));

  ASSERT_EQ(2u, seen.size());
  EXPECT_EQ(1, seen[0]);
  EXPECT_EQ(2, seen[1]);
}

TEST(AriDispatchTask, Sync_DispatchRunsOnCallerThread) {
  const std::thread::id caller = std::this_thread::get_id();
  std::thread::id ran_on;
  AriDispatchTask t;
  t.set_synchronous_for_test();
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch([&]() { ran_on = std::this_thread::get_id(); }));
  EXPECT_EQ(caller, ran_on) << "sync mode runs the job inline, no worker";
}

TEST(AriDispatchTask, EmptyJob_IsRejected) {
  AriDispatchTask t;
  t.set_synchronous_for_test();
  ASSERT_EQ(0, t.start());
  EXPECT_FALSE(t.dispatch(AriDispatchTask::Job{}))
      << "an empty std::function is not runnable work";
}

TEST(AriDispatchTask, DispatchBeforeStart_IsRejected) {
  std::atomic<int> ran{0};
  AriDispatchTask t;
  // No start() — the task has not accepted work yet.
  EXPECT_FALSE(t.dispatch([&]() { ++ran; }));
  EXPECT_EQ(0, ran.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// Async path — exercises the real worker thread + queue.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AriDispatchTask, Async_Dispatch_RunsOnWorkerThread) {
  std::atomic<int> count{0};
  std::thread::id  worker_tid;
  const std::thread::id caller_tid = std::this_thread::get_id();

  AriDispatchTask t;
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch([&]() {
    worker_tid = std::this_thread::get_id();
    ++count;
  }));
  EXPECT_TRUE(wait_until([&] { return count.load() == 1; }));
  EXPECT_NE(worker_tid, caller_tid)
      << "the whole point: blocking work runs off the producer's thread";

  t.stop();
}

TEST(AriDispatchTask, Async_RunsJobsInProducerOrder) {
  std::mutex       m;
  std::vector<int> seen;
  AriDispatchTask  t;
  ASSERT_EQ(0, t.start());

  for (int i = 0; i < 50; ++i) {
    ASSERT_TRUE(t.dispatch([&, i]() {
      std::lock_guard<std::mutex> g(m);
      seen.push_back(i);
    }));
  }
  EXPECT_TRUE(wait_until([&] {
    std::lock_guard<std::mutex> g(m);
    return seen.size() == 50u;
  }));

  std::lock_guard<std::mutex> g(m);
  ASSERT_EQ(50u, seen.size());
  for (int i = 0; i < 50; ++i)
    EXPECT_EQ(i, seen[i]) << "single worker thread preserves FIFO order";

  t.stop();
}

TEST(AriDispatchTask, Async_Stop_JoinsWorkerAndRejectsLateWork) {
  std::atomic<int> ran{0};
  AriDispatchTask t;
  ASSERT_EQ(0, t.start());
  EXPECT_TRUE(t.running());

  t.stop();
  EXPECT_FALSE(t.running());

  EXPECT_FALSE(t.dispatch([&]() { ++ran; }))
      << "dispatch after stop() must be rejected";
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(0, ran.load());
}

TEST(AriDispatchTask, Async_StopIsIdempotent) {
  AriDispatchTask t;
  ASSERT_EQ(0, t.start());
  t.stop();
  t.stop();   // second + third calls must not hang or crash
  t.stop();
  SUCCEED();
}

TEST(AriDispatchTask, Async_DestructorStopsWorker) {
  std::atomic<int> count{0};
  {
    AriDispatchTask t;
    ASSERT_EQ(0, t.start());
    EXPECT_TRUE(t.dispatch([&]() { ++count; }));
    EXPECT_TRUE(wait_until([&] { return count.load() == 1; }));
    // No explicit stop() — the destructor must drain + join.
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(1, count.load()) << "no job ran after teardown";
}

TEST(AriDispatchTask, Async_JobThrows_WorkerSurvives) {
  std::atomic<int> seen{0};
  AriDispatchTask t;
  ASSERT_EQ(0, t.start());

  EXPECT_TRUE(t.dispatch([&]() { ++seen; }));
  EXPECT_TRUE(t.dispatch([&]() {
    ++seen;
    throw std::runtime_error("synthetic");   // worker must swallow
  }));
  EXPECT_TRUE(t.dispatch([&]() { ++seen; })); // must still be processed

  EXPECT_TRUE(wait_until([&] { return seen.load() == 3; }))
      << "a throwing job must not kill the worker";
  t.stop();
}

// A job enqueued but not yet popped when stop() runs must be drained
// (freed) without executing — stop() poisons the queue, joins, then
// releases the leftovers. We cannot deterministically force the race,
// so flood the queue and assert the worker stopped cleanly with some
// jobs unrun; the heap Jobs in those blocks are freed by stop()'s drain
// (a leak would surface under ASan in CI).
TEST(AriDispatchTask, Async_StopDrainsUndeliveredJobs) {
  std::atomic<int> ran{0};
  AriDispatchTask t;
  ASSERT_EQ(0, t.start());
  for (int i = 0; i < 2000; ++i)
    t.dispatch([&]() {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      ++ran;
    });
  t.stop();
  // Whatever the worker managed before the poison pill is fine; the
  // contract is only that stop() returns (worker joined) and does not
  // crash or hang while freeing the undelivered remainder.
  const int after_stop = ran.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(after_stop, ran.load()) << "no job ran after stop() returned";
  EXPECT_LE(ran.load(), 2000);
}
