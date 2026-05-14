#include "browser_stream.hpp"
#include "wsframe.hpp"

#include "ace/Log_Msg.h"
#include "ace/Reactor.h"
#include "ace/Time_Value.h"

#include <cstring>

namespace {

constexpr std::size_t kReadChunk = 65536;

// Heroku's router H15-drops a WebSocket after 55s with no bytes in
// either direction. A SIP call can sit idle far longer than that
// (mid-conversation there's no signalling traffic), so send a WS PING
// every 25s to keep the /sip-ws socket alive.
constexpr int kPingIntervalSec = 25;

// WS opcodes (RFC 6455 §5.2).
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

BrowserStream::BrowserStream(SipBridge &bridge, ACE_HANDLE fd,
                              const std::string &open_meta)
    : m_bridge(bridge), m_handle(fd) {
  m_stream.set_handle(fd);
  m_stream_id = m_bridge.on_browser_upgrade(this, open_meta);
}

BrowserStream::~BrowserStream() {
  // Defensive: if for some reason the bridge wasn't notified (e.g. test
  // path), do it now. All three calls are idempotent.
  cancel_ping_timer();
  notify_close_once("destroyed");
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
}

int BrowserStream::register_with_reactor() {
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

int BrowserStream::handle_input(ACE_HANDLE /*h*/) {
  std::uint8_t chunk[kReadChunk];
  const ssize_t n = m_stream.recv(chunk, sizeof(chunk));
  if (n <= 0) {
    // Peer closed or read error.
    notify_close_once("peer_closed");
    return -1; // ACE calls handle_close(...).
  }
  m_recv_buf.insert(m_recv_buf.end(), chunk, chunk + n);
  return drain_frames() ? 0 : -1;
}

bool BrowserStream::drain_frames() {
  for (;;) {
    auto maybe = wsframe::decode(m_recv_buf);
    if (!maybe.has_value()) return true; // need more bytes

    const auto &frame = *maybe;
    switch (frame.opcode) {
    case kOpcodeBinary:
    case kOpcodeText: // SIP-over-WS uses text per RFC 7118; accept either
    case kOpcodeContinuation:
      m_bridge.on_browser_data(m_stream_id, bytes_to_string(frame.payload));
      break;

    case kOpcodePing: {
      const auto pong = wsframe::pong_frame(frame.payload);
      m_stream.send_n(pong.data(), pong.size());
      break;
    }

    case kOpcodePong:
      // Heartbeat ack; ignored at this layer.
      break;

    case kOpcodeClose: {
      // Echo a close frame back (RFC 6455 §5.5.1) and tear down.
      const auto close = wsframe::close_frame();
      m_stream.send_n(close.data(), close.size());
      notify_close_once("browser_closed");
      return false;
    }

    default:
      // Unknown opcode — protocol violation. Drop.
      notify_close_once("ws_protocol_error");
      return false;
    }
  }
}

// ── handle_close ─────────────────────────────────────────────────────────────

int BrowserStream::handle_close(ACE_HANDLE /*h*/, ACE_Reactor_Mask /*m*/) {
  // Cancel the timer BEFORE `delete this` so the reactor can never fire
  // handle_timeout on a freed object.
  cancel_ping_timer();
  notify_close_once("handle_close");
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  delete this;
  return 0;
}

// ── handle_timeout ───────────────────────────────────────────────────────────

int BrowserStream::handle_timeout(const ACE_Time_Value & /*tv*/,
                                  const void * /*act*/) {
  if (m_handle == ACE_INVALID_HANDLE)
    return -1; // socket already gone — let the reactor run handle_close

  const auto ping = wsframe::ping_frame();
  const ssize_t n = m_stream.send_n(ping.data(), ping.size());
  if (n != static_cast<ssize_t>(ping.size())) {
    // Write failed — the peer (or Heroku) is already gone. Notify the
    // bridge and return -1 so the reactor tears us down.
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [BrowserStream:%t] %M %N:%l keep-alive ping write "
                        "failed sid=%u; releasing stream\n"),
               m_stream_id));
    notify_close_once("keepalive_write_failed");
    return -1;
  }
  return 0;
}

// ── BrowserSink ──────────────────────────────────────────────────────────────

void BrowserStream::send_bytes(const std::string &sip_ws_bytes) {
  if (m_handle == ACE_INVALID_HANDLE) return;
  // SIP.js in the browser expects text frames for SIP messages
  // (RFC 7118 §2 mandates text). We emit text by default.
  const auto framed =
      wsframe::encode(string_to_bytes(sip_ws_bytes), kOpcodeText, /*mask=*/false);
  m_stream.send_n(framed.data(), framed.size());
}

void BrowserStream::close(const std::string &reason) {
  if (m_handle == ACE_INVALID_HANDLE) return;
  // Best-effort send a close frame, then close the socket. The bridge
  // does NOT need an `on_browser_close` callback here — `close()` is
  // called BY the bridge to release us, so the bridge is the one that
  // initiated the close.
  cancel_ping_timer();
  const auto close = wsframe::close_frame();
  m_stream.send_n(close.data(), close.size());
  // `notify_close_once` marks us as already closed so handle_close
  // doesn't try to call back into the bridge map after we're released.
  m_close_notified = true;
  m_stream.close();
  m_handle = ACE_INVALID_HANDLE;
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [BrowserStream:%t] %M %N:%l closed sid=%u reason=%s\n"),
             m_stream_id, reason.c_str()));
}

// ── notify_close_once ────────────────────────────────────────────────────────

void BrowserStream::notify_close_once(const std::string &reason) {
  if (m_close_notified) return;
  m_close_notified = true;
  m_bridge.on_browser_close(m_stream_id, reason);
}

// ── cancel_ping_timer ────────────────────────────────────────────────────────

void BrowserStream::cancel_ping_timer() {
  if (m_ping_timer != -1 && reactor() != nullptr) {
    reactor()->cancel_timer(m_ping_timer);
  }
  m_ping_timer = -1;
}
