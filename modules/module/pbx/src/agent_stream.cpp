#include "agent_stream.hpp"
#include "innertls.hpp"
#include "ws_inner_tls_bridge.hpp"
#include "wsframe.hpp"

#include "ace/Log_Msg.h"
#include "ace/Reactor.h"
#include "ace/Time_Value.h"

#include <memory>

namespace {

constexpr std::size_t kReadChunk = 65536;

// Heroku's router H15-drops a WebSocket after 55s with no bytes in
// either direction. Send a WS PING every 25s so the /agent tunnel stays
// alive between SIP traffic bursts — at least two pings per idle window.
constexpr int kPingIntervalSec = 25;

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

AgentStream::AgentStream(CloudTunnelEndpoint &endpoint, ACE_HANDLE fd,
                          bool auto_attach)
    : m_endpoint(endpoint), m_handle(fd), m_adapter(nullptr) {
  m_stream.set_handle(fd);
  if (auto_attach) attach();
}

AgentStream::~AgentStream() {
  // Defensive: if we never went through handle_close, do the cleanup
  // here. All three calls are idempotent.
  cancel_ping_timer();
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  // Same reason as `AceSslTransport`: m_inner_tls references m_bridge.
  m_inner_tls.reset();
  m_bridge.reset();
}

bool AgentStream::setup_inner_tls(const std::string &cert_path,
                                   const std::string &key_path,
                                   const std::string &ca_path) {
  if (m_attached) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AgentStream:%t] %M %N:%l setup_inner_tls "
                        "called after attach — refusing\n")));
    return false;
  }
  if (m_handle == ACE_INVALID_HANDLE) return false;

  auto recv_raw = [this](void *buf, std::size_t cap) -> long {
    return static_cast<long>(m_stream.recv(buf, cap));
  };
  auto send_raw = [this](const void *buf, std::size_t len) -> long {
    return static_cast<long>(m_stream.send_n(buf, len));
  };
  m_bridge = std::make_unique<WsInnerTlsBridge>(std::move(recv_raw),
                                                 std::move(send_raw),
                                                 /*client_mask=*/false);
  m_inner_tls = std::make_unique<InnerTlsServer>(*m_bridge, cert_path,
                                                  key_path, ca_path);
  if (!m_inner_tls->accept()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AgentStream:%t] %M %N:%l inner-TLS accept "
                        "failed\n")));
    m_inner_tls.reset();
    m_bridge.reset();
    return false;
  }
  m_bridge->switch_to_buffered();
  // Carry leftover socket bytes into the steady-state decoder so app
  // data that arrived in the same TCP segment as the handshake's final
  // record isn't lost on the next handle_input.
  m_recv_buf = m_bridge->leftover_socket_bytes();
  // Capture the agent cert's CN for log labels + future cross-checks
  // against the AGENT_HELLO `societyId`. Empty CN is normal when the
  // agent didn't present a cert (CA not configured on the cloud side).
  m_peer_cn = m_inner_tls->peer_subject_cn();
  if (!m_peer_cn.empty()) {
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [AgentStream:%t] %M %N:%l inner-TLS handshake "
                        "established (peer CN=%s)\n"),
               m_peer_cn.c_str()));
  } else {
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [AgentStream:%t] %M %N:%l inner-TLS handshake "
                        "established (anonymous client — no peer cert)\n")));
  }
  return true;
}

void AgentStream::attach() {
  if (m_attached) return;
  m_attached = true;
  auto adapter = std::make_unique<TransportAdapter>(this);
  m_adapter = adapter.get();
  m_endpoint.on_agent_connected(std::move(adapter));
}

int AgentStream::register_with_reactor() {
  if (reactor()->register_handler(this, ACE_Event_Handler::READ_MASK) == -1)
    return -1;
  // Keep-alive: arm a recurring WS-PING timer (initial delay == interval).
  // Tests that skip register_with_reactor() never schedule a timer, so
  // m_ping_timer stays -1 and cancel_ping_timer() is a no-op for them.
  const ACE_Time_Value interval(kPingIntervalSec);
  m_ping_timer = reactor()->schedule_timer(this, nullptr, interval, interval);
  if (m_ping_timer == -1) {
    // Roll the handler registration back so the caller can `delete` us
    // without leaving a dangling pointer in the reactor.
    reactor()->remove_handler(
        this, ACE_Event_Handler::READ_MASK | ACE_Event_Handler::DONT_CALL);
    return -1;
  }
  return 0;
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
      if (m_inner_tls) {
        // Encrypted TLS record arrived. Hand it to the bridge's
        // buffered queue, then drain plaintext via InnerTls::recv.
        // SSL_read inside recv() drains every record visible in the
        // BIO, so one push + one recv loop covers a peer SSL_write
        // that produced multiple records in this WS frame.
        m_bridge->push_inbound(frame.payload);
        for (;;) {
          std::vector<std::uint8_t> plaintext;
          if (!m_inner_tls->recv(plaintext)) {
            notify_disconnect_once();
            return false;
          }
          if (plaintext.empty()) break;  // WANT_READ — fully drained
          m_endpoint.on_bytes_received(bytes_to_string(plaintext));
          if (m_handle == ACE_INVALID_HANDLE) return false;
        }
      } else {
        m_endpoint.on_bytes_received(bytes_to_string(frame.payload));
        // If the endpoint dropped us mid-call (e.g. invalid SipFrame
        // payload → bridge.on_tunnel_bytes returned false → endpoint
        // released the transport), our socket has been closed via
        // close_socket(). Bail out so handle_input returns -1 and the
        // reactor cleans us up.
        if (m_handle == ACE_INVALID_HANDLE) return false;
      }
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
  // Cancel the timer BEFORE `delete this` so the reactor can never fire
  // handle_timeout on a freed object.
  cancel_ping_timer();
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  delete this;
  return 0;
}

// ── handle_timeout ───────────────────────────────────────────────────────────

int AgentStream::handle_timeout(const ACE_Time_Value & /*tv*/,
                                const void * /*act*/) {
  if (m_handle == ACE_INVALID_HANDLE)
    return -1; // socket already gone — let the reactor run handle_close

  const auto ping = wsframe::ping_frame();
  const ssize_t n = m_stream.send_n(ping.data(), ping.size());
  if (n != static_cast<ssize_t>(ping.size())) {
    // Write failed — the peer (or Heroku) is already gone. Notify the
    // endpoint and return -1 so the reactor tears us down.
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [AgentStream:%t] %M %N:%l keep-alive ping write "
                        "failed; releasing tunnel\n")));
    notify_disconnect_once();
    return -1;
  }
  return 0;
}

// ── Adapter callbacks ────────────────────────────────────────────────────────

bool AgentStream::write_bytes(const std::string &sip_ws_bytes) {
  if (m_handle == ACE_INVALID_HANDLE) return false;

  // Inner-TLS path: bridge wraps each encrypted record in one unmasked
  // WS binary frame and writes to the socket.
  if (m_inner_tls) {
    return m_inner_tls->send(string_to_bytes(sip_ws_bytes));
  }

  // Test-only path: raw text WS frame, no encryption. Production wires
  // the inner-TLS handshake before attach(), so this branch is unused
  // against a real agent peer.
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
  cancel_ping_timer();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
}

// ── cancel_ping_timer ────────────────────────────────────────────────────────

void AgentStream::cancel_ping_timer() {
  if (m_ping_timer != -1 && reactor() != nullptr) {
    reactor()->cancel_timer(m_ping_timer);
  }
  m_ping_timer = -1;
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
