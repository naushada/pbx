#include "ari_dispatch_task.hpp"

#include "ace/Log_Msg.h"
#include "ace/Message_Block.h"
#include "ace/Time_Value.h"

#include <cstring>
#include <utility>

namespace {

/// Single-thread activation flags — mirrors InnerTlsDispatchTask /
/// webservice's MicroService. One thread is the point: the queue
/// serializes job execution so AriClient / CallRouter stay effectively
/// single-threaded (their documented "no internal locking" invariant).
constexpr int kActivateFlags = THR_NEW_LWP | THR_JOINABLE;
constexpr int kThreadCount   = 1;

/// Wrap a heap-allocated Job in a message block. The block carries the
/// raw `Job*` in its data bytes (not the closure itself — ACE_Message_Block
/// is a byte buffer). `svc()` reconstructs the pointer, runs the job, and
/// deletes it. If the block never reaches `svc()` (queue drained by
/// `stop()`), `release_job_mb()` frees the Job so nothing leaks.
ACE_Message_Block *make_job_mb(AriDispatchTask::Job *job) {
  auto *mb = new ACE_Message_Block(sizeof(job));
  std::memcpy(mb->wr_ptr(), &job, sizeof(job));
  mb->wr_ptr(sizeof(job));
  mb->msg_type(ACE_Message_Block::MB_DATA);
  return mb;
}

/// Extract the Job* a `make_job_mb` block carries. Returns nullptr if the
/// block is not a well-formed job block.
AriDispatchTask::Job *job_from_mb(ACE_Message_Block *mb) {
  if (!mb || mb->msg_type() != ACE_Message_Block::MB_DATA ||
      mb->length() != sizeof(AriDispatchTask::Job *))
    return nullptr;
  AriDispatchTask::Job *job = nullptr;
  std::memcpy(&job, mb->rd_ptr(), sizeof(job));
  return job;
}

ACE_Message_Block *make_stop_mb() {
  auto *mb = new ACE_Message_Block();
  mb->msg_type(ACE_Message_Block::MB_STOP);
  return mb;
}

/// Free both the carried Job and the block. Used for messages drained
/// after the poison pill — they would otherwise leak the heap Job.
void release_job_mb(ACE_Message_Block *mb) {
  if (!mb) return;
  delete job_from_mb(mb);   // delete nullptr is a no-op
  mb->release();
}

} // namespace

AriDispatchTask::AriDispatchTask() : ACE_Task<ACE_MT_SYNCH>() {}

AriDispatchTask::~AriDispatchTask() {
  // stop() is idempotent — safe here even if the owner already tore us
  // down explicitly.
  stop();
}

void AriDispatchTask::set_synchronous_for_test() {
  m_synchronous.store(true);
}

int AriDispatchTask::start() {
  if (m_synchronous.load()) {
    // Inline test mode — no thread, but flip m_continue so dispatch()
    // accepts work the same way the real start() does.
    m_continue.store(true);
    return 0;
  }
  m_continue.store(true);
  const int rc = activate(kActivateFlags, kThreadCount);
  if (rc == -1) {
    m_continue.store(false);
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AriDispatchTask] activate failed\n")));
  }
  return rc;
}

void AriDispatchTask::stop() {
  // First caller wins; second caller no-ops.
  bool expected = false;
  if (!m_stop_called.compare_exchange_strong(expected, true)) return;

  m_continue.store(false);

  if (m_synchronous.load()) {
    // No worker, no queue — nothing to drain.
    return;
  }

  // Poison pill — wakes svc() out of getq() so it can observe
  // m_continue == false and exit. putq is best-effort; if the queue is
  // already deactivated we still want to wait().
  ACE_Message_Block *poison = make_stop_mb();
  if (putq(poison) == -1) poison->release();

  // Block until the worker thread exits. activate(THR_JOINABLE) above
  // makes wait() effectively a pthread_join. Without this, the
  // destructor races against an in-flight job invocation.
  wait();

  // Drain anything the worker did not pop before observing the poison.
  // Each undelivered block still owns a heap Job — release_job_mb frees
  // both so neither the closure nor the block leaks.
  ACE_Message_Block *mb     = nullptr;
  ACE_Time_Value     nowait = ACE_Time_Value::zero;
  while (msg_queue() != nullptr &&
         msg_queue()->dequeue_head(mb, &nowait) != -1) {
    release_job_mb(mb);
  }
}

bool AriDispatchTask::dispatch(Job job) {
  if (!job) return false;

  if (m_synchronous.load()) {
    job();
    return true;
  }
  if (!m_continue.load()) return false;

  auto *heap_job = new Job(std::move(job));
  ACE_Message_Block *mb = make_job_mb(heap_job);
  if (putq(mb) == -1) {
    delete heap_job;
    mb->release();
    return false;
  }
  return true;
}

int AriDispatchTask::svc() {
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [AriDispatchTask] svc spawned (thr=%t)\n")));

  while (m_continue.load()) {
    ACE_Message_Block *mb = nullptr;
    if (getq(mb) == -1) {
      // Queue deactivated — treat as a stop signal.
      break;
    }

    if (mb->msg_type() == ACE_Message_Block::MB_STOP) {
      // Poison-pill — graceful shutdown.
      mb->release();
      m_continue.store(false);
      break;
    }

    Job *job = job_from_mb(mb);
    mb->release();
    if (!job) continue;   // malformed block — should not happen

    try {
      (*job)();
    } catch (...) {
      // Jobs must not throw across this boundary; if one does, log and
      // continue so a single bad event doesn't kill the worker (and with
      // it every subsequent ARI event).
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AriDispatchTask] job threw — swallowed\n")));
    }
    delete job;
  }

  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [AriDispatchTask] svc exiting (thr=%t)\n")));
  return 0;
}
