#include "ws_inner_tls_bridge.hpp"

#include "wsframe.hpp"

#include <utility>

namespace {

constexpr std::uint8_t kOpcodeContinuation = 0x0;
constexpr std::uint8_t kOpcodeText         = 0x1;
constexpr std::uint8_t kOpcodeBinary       = 0x2;
constexpr std::uint8_t kOpcodeClose        = 0x8;
constexpr std::uint8_t kOpcodePing         = 0x9;
constexpr std::uint8_t kOpcodePong         = 0xA;

constexpr std::size_t kReadChunk = 8192;

} // namespace

WsInnerTlsBridge::WsInnerTlsBridge(RecvRawFn recv_raw, SendRawFn send_raw,
                                    bool client_mask)
    : m_recv_raw(std::move(recv_raw)),
      m_send_raw(std::move(send_raw)),
      m_client_mask(client_mask) {}

bool WsInnerTlsBridge::send(const std::vector<std::uint8_t> &data) {
  if (m_closed) return false;
  const auto framed = wsframe::encode(data, kOpcodeBinary, m_client_mask);
  const long n = m_send_raw(framed.data(), framed.size());
  return n == static_cast<long>(framed.size());
}

bool WsInnerTlsBridge::recv(std::vector<std::uint8_t> &data) {
  if (m_closed) return false;

  if (!m_buffered) return blocking_recv(data);

  // Buffered mode: hand back one queued payload. Empty queue is NOT a
  // close — it's "nothing decrypted right now"; InnerTls sees an empty
  // wire, hits WANT_READ on SSL_read, and exits its loop. The host's
  // next handle_input pushes more bytes and calls recv again.
  if (m_inbound.empty()) {
    data.clear();
    return true;
  }
  data = std::move(m_inbound.front());
  m_inbound.pop_front();
  return true;
}

void WsInnerTlsBridge::switch_to_buffered() { m_buffered = true; }

std::vector<std::uint8_t> WsInnerTlsBridge::leftover_socket_bytes() {
  std::vector<std::uint8_t> out;
  out.swap(m_socket_buf);
  return out;
}

void WsInnerTlsBridge::push_inbound(std::vector<std::uint8_t> data) {
  if (data.empty()) return;
  m_inbound.push_back(std::move(data));
}

bool WsInnerTlsBridge::blocking_recv(std::vector<std::uint8_t> &data) {
  // Loop until we extract one app-data frame (binary/text/continuation),
  // or the socket closes / a close frame is seen.
  for (;;) {
    auto maybe = wsframe::decode(m_socket_buf);
    if (maybe) {
      const auto &frame = *maybe;
      switch (frame.opcode) {
      case kOpcodeBinary:
      case kOpcodeText:
      case kOpcodeContinuation:
        data = frame.payload;
        return true;

      case kOpcodePing: {
        // Auto-pong with the same payload (RFC 6455 §5.5.3).
        const auto pong = wsframe::encode(frame.payload, kOpcodePong,
                                           m_client_mask);
        if (m_send_raw(pong.data(), pong.size())
              != static_cast<long>(pong.size()))
          return false;
        continue;
      }

      case kOpcodePong:
        continue;

      case kOpcodeClose:
        // Peer initiated close. InnerTls treats false as a fatal drop,
        // which is what the host wants for a mid-handshake close too.
        m_closed = true;
        return false;

      default:
        // Unknown opcode — treat as protocol error.
        return false;
      }
    }

    // Need more bytes — block on the socket.
    std::uint8_t chunk[kReadChunk];
    const long n = m_recv_raw(chunk, sizeof(chunk));
    if (n <= 0) {
      m_closed = true;
      return false;
    }
    m_socket_buf.insert(m_socket_buf.end(), chunk, chunk + n);
  }
}
