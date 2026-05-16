#include "inner_tls_task.hpp"

#include "ace/Log_Msg.h"
#include "ace/Message_Block.h"
#include "ace/Time_Value.h"

#include <cstring>
#include <utility>

namespace {

/// Single-thread activation flags — mirrors webservice/MicroService.
/// One thread is the point: the queue serializes handler invocations so
/// they observe a consistent transport lifecycle.
constexpr int kActivateFlags = THR_NEW_LWP | THR_JOINABLE;
constexpr int kThreadCount   = 1;

/// Wrap a string payload into a message block. Caller transfers
/// ownership to the queue via putq; the worker `release()`s after
/// handler dispatch.
ACE_Message_Block *make_bytes_mb(const std::string &bytes) {
  ACE_Message_Block *mb = new ACE_Message_Block(bytes.size());
  if (!bytes.empty()) {
    std::memcpy(mb->wr_ptr(), bytes.data(), bytes.size());
    mb->wr_ptr(bytes.size());
  }
  mb->msg_type(ACE_Message_Block::MB_DATA);
  return mb;
}

/// MB_HANGUP — producer telling us the underlying transport observed
/// EOF / error. Distinct from MB_STOP, which is the task's own
/// poison-pill for `stop()`.
ACE_Message_Block *make_disconnect_mb() {
  auto *mb = new ACE_Message_Block();
  mb->msg_type(ACE_Message_Block::MB_HANGUP);
  return mb;
}

ACE_Message_Block *make_stop_mb() {
  auto *mb = new ACE_Message_Block();
  mb->msg_type(ACE_Message_Block::MB_STOP);
  return mb;
}

} // namespace

InnerTlsDispatchTask::InnerTlsDispatchTask(PlaintextHandler  on_bytes,
                                             DisconnectHandler on_disconnect)
    : ACE_Task<ACE_MT_SYNCH>(),
      m_on_bytes(std::move(on_bytes)),
      m_on_disconnect(std::move(on_disconnect)) {}

InnerTlsDispatchTask::~InnerTlsDispatchTask() {
  // stop() is idempotent — safe to call here even if the owner already
  // tore us down explicitly.
  stop();
}

void InnerTlsDispatchTask::set_synchronous_for_test() {
  m_synchronous.store(true);
}

int InnerTlsDispatchTask::start() {
  if (m_synchronous.load()) {
    // Inline test mode — no thread, but flip m_continue so dispatch_*
    // accepts work the same way the real start() does.
    m_continue.store(true);
    return 0;
  }
  m_continue.store(true);
  const int rc = activate(kActivateFlags, kThreadCount);
  if (rc == -1) {
    m_continue.store(false);
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [InnerTlsDispatchTask] activate failed\n")));
  }
  return rc;
}

void InnerTlsDispatchTask::stop() {
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
  // destructor races against an in-flight handler invocation.
  wait();

  // Drain anything the worker didn't pop before observing the poison.
  // These messages would otherwise leak — the queue is destroyed with
  // the task, but the heap-allocated ACE_Message_Blocks inside aren't.
  ACE_Message_Block      *mb      = nullptr;
  ACE_Time_Value          nowait  = ACE_Time_Value::zero;
  while (msg_queue() != nullptr &&
         msg_queue()->dequeue_head(mb, &nowait) != -1) {
    if (mb) mb->release();
  }
}

bool InnerTlsDispatchTask::dispatch_bytes(std::string bytes) {
  if (m_synchronous.load()) {
    if (m_on_bytes) m_on_bytes(bytes);
    return true;
  }
  if (!m_continue.load()) return false;
  ACE_Message_Block *mb = make_bytes_mb(bytes);
  if (putq(mb) == -1) {
    mb->release();
    return false;
  }
  return true;
}

bool InnerTlsDispatchTask::dispatch_disconnect() {
  if (m_synchronous.load()) {
    if (m_on_disconnect) m_on_disconnect();
    return true;
  }
  if (!m_continue.load()) return false;
  ACE_Message_Block *mb = make_disconnect_mb();
  if (putq(mb) == -1) {
    mb->release();
    return false;
  }
  return true;
}

int InnerTlsDispatchTask::svc() {
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [InnerTlsDispatchTask] svc spawned (thr=%t)\n")));

  while (m_continue.load()) {
    ACE_Message_Block *mb = nullptr;
    if (getq(mb) == -1) {
      // Queue deactivated — treat as a stop signal.
      break;
    }

    switch (mb->msg_type()) {
    case ACE_Message_Block::MB_DATA: {
      // The producer copied the plaintext into the block; reconstruct
      // a std::string from the readable range and dispatch.
      std::string payload(mb->rd_ptr(), mb->length());
      mb->release();
      try {
        if (m_on_bytes) m_on_bytes(payload);
      } catch (...) {
        // Handlers must not throw across this boundary; if one does,
        // log and continue so a single bad frame doesn't kill the
        // worker (and with it, every subsequent dispatch).
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [InnerTlsDispatchTask] on_bytes "
                            "handler threw — swallowed\n")));
      }
      break;
    }
    case ACE_Message_Block::MB_HANGUP: {
      mb->release();
      try {
        if (m_on_disconnect) m_on_disconnect();
      } catch (...) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [InnerTlsDispatchTask] on_disconnect "
                            "handler threw — swallowed\n")));
      }
      break;
    }
    case ACE_Message_Block::MB_STOP:
    default: {
      // Poison-pill — graceful shutdown.
      mb->release();
      m_continue.store(false);
      break;
    }
    }
  }

  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [InnerTlsDispatchTask] svc exiting (thr=%t)\n")));
  return 0;
}
