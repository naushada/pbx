#include "agent_stream.hpp"
#include "wsframe.hpp"

#include "ace/Log_Msg.h"
#include "ace/Reactor.h"

#include <memory>

namespace {

constexpr std::size_t kReadChunk = 65536;

constexpr std::uint8_t kOpcodeContinuation = 0x0;
constexpr std::uint8_t kOpcodeText         = 0x1;
constexpr std::uint8_t kOpcodeBinary       = 0x2;
constexpr std::uint8_t kOpcodeClose        = 0x8;
constexpr std::uint8_t kOpcodePing         = 0x9;
constexpr std::uint8_t kOpcodePong         = 0xA;

inline std::string bytes_to_string(const std::vector<std::uint8_t> &v) {
  return std::string(reinterpret_cast<const char *>(v.data()), v.size());
}

inline std::vector<std::uint8_t> string_to_bytes(const std::string &s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

} // namespace

AgentStream::AgentStream(CloudTunnelEndpoint &endpoint, ACE_HANDLE fd)
    : m_endpoint(endpoint), m_handle(fd), m_adapter(nullptr) {
  m_stream.set_handle(fd);
  auto adapter = std::make_unique<TransportAdapter>(this);
  m_adapter = adapter.get();
  m_endpoint.on_agent_connected(std::move(adapter));
}

AgentStream::~AgentStream() {
  // Defensive: if we never went through handle_close, do the cleanup
  // here. Both calls are idempotent.
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
}

int AgentStream::register_with_reactor() {
  return reactor()->register_handler(this, ACE_Event_Handler::READ_MASK);
}

// ── handle_input ─────────────────────────────────────────────────────────────

int AgentStream::handle_input(ACE_HANDLE /*h*/) {
  std::uint8_t chunk[kReadChunk];
  const ssize_t n = m_stream.recv(chunk, sizeof(chunk));
  if (n <= 0) {
    notify_disconnect_once();
    return -1; // ACE calls handle_close(...)
  }
  m_recv_buf.insert(m_recv_buf.end(), chunk, chunk + n);
  return drain_frames() ? 0 : -1;
}

bool AgentStream::drain_frames() {
  for (;;) {
    auto maybe = wsframe::decode(m_recv_buf);
    if (!maybe.has_value()) return true; // need more bytes

    const auto &frame = *maybe;
    switch (frame.opcode) {
    case kOpcodeBinary:
    case kOpcodeText:
    case kOpcodeContinuation:
      m_endpoint.on_bytes_received(bytes_to_string(frame.payload));
      // If the endpoint dropped us mid-call (e.g. invalid SipFrame
      // payload → bridge.on_tunnel_bytes returned false → endpoint
      // released the transport), our socket has been closed via
      // close_socket(). Bail out so handle_input returns -1 and the
      // reactor cleans us up.
      if (m_handle == ACE_INVALID_HANDLE) return false;
      break;

    case kOpcodePing: {
      const auto pong = wsframe::pong_frame(frame.payload);
      m_stream.send_n(pong.data(), pong.size());
      break;
    }

    case kOpcodePong:
      // Heartbeat ack — swallow.
      break;

    case kOpcodeClose: {
      // Echo a close frame back (RFC 6455 §5.5.1) then disconnect.
      const auto close = wsframe::close_frame();
      m_stream.send_n(close.data(), close.size());
      notify_disconnect_once();
      return false;
    }

    default:
      notify_disconnect_once();
      return false;
    }
  }
}

// ── handle_close ─────────────────────────────────────────────────────────────

int AgentStream::handle_close(ACE_HANDLE /*h*/, ACE_Reactor_Mask /*m*/) {
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  delete this;
  return 0;
}

// ── Adapter callbacks ────────────────────────────────────────────────────────

bool AgentStream::write_bytes(const std::string &sip_ws_bytes) {
  if (m_handle == ACE_INVALID_HANDLE) return false;
  // SIP-over-WS uses text frames (RFC 7118 §2). Server side is unmasked.
  const auto framed =
      wsframe::encode(string_to_bytes(sip_ws_bytes), kOpcodeText, /*mask=*/false);
  const ssize_t n =
      m_stream.send_n(framed.data(), framed.size());
  return n == static_cast<ssize_t>(framed.size());
}

void AgentStream::close_socket() {
  // Endpoint is releasing us via reset() on its unique_ptr; the adapter
  // is about to die. Drop our back-pointer first so a re-entrant
  // close-path (extremely unlikely on a single-threaded reactor) doesn't
  // dereference it.
  m_adapter = nullptr;
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
}

// ── notify_disconnect_once ───────────────────────────────────────────────────

void AgentStream::notify_disconnect_once() {
  if (m_close_notified) return;
  m_close_notified = true;

  // Detach the adapter BEFORE telling the endpoint to release, so that
  // any re-entrant close routed through the adapter sees a stale m_s
  // and is a no-op. The adapter will be destroyed inside
  // on_agent_disconnected() (endpoint resets its unique_ptr); after this
  // line m_adapter is conceptually dangling and we don't touch it again.
  if (m_adapter) {
    m_adapter->m_s = nullptr;
    m_adapter = nullptr;
  }
  m_endpoint.on_agent_disconnected();
}
