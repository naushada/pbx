#include "ari_ws_client.hpp"
#include "wsframe.hpp"

#include "ace/INET_Addr.h"
#include "ace/Log_Msg.h"
#include "ace/Reactor.h"
#include "ace/SOCK_Connector.h"
#include "ace/Time_Value.h"

#include <openssl/rand.h>

#include <cstring>
#include <sstream>

namespace {

constexpr std::size_t kReadChunk = 65536;

constexpr std::uint8_t kOpcodeContinuation = 0x0;
constexpr std::uint8_t kOpcodeText         = 0x1;
constexpr std::uint8_t kOpcodeBinary       = 0x2;
constexpr std::uint8_t kOpcodeClose        = 0x8;
constexpr std::uint8_t kOpcodePing         = 0x9;
constexpr std::uint8_t kOpcodePong         = 0xA;

/// 24-char base64 of 16 random bytes (matches what xpmile/wsdbagent uses).
std::string random_ws_key() {
  std::uint8_t raw[16];
  if (RAND_bytes(raw, sizeof(raw)) != 1) std::memset(raw, 0, sizeof(raw));
  static const char alpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(24);
  for (std::size_t i = 0; i < 16; i += 3) {
    std::uint32_t v = static_cast<std::uint32_t>(raw[i]) << 16;
    if (i + 1 < 16) v |= static_cast<std::uint32_t>(raw[i + 1]) << 8;
    if (i + 2 < 16) v |= static_cast<std::uint32_t>(raw[i + 2]);
    out.push_back(alpha[(v >> 18) & 0x3F]);
    out.push_back(alpha[(v >> 12) & 0x3F]);
    out.push_back((i + 1 < 16) ? alpha[(v >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < 16) ? alpha[v & 0x3F]        : '=');
  }
  return out;
}

inline std::string bytes_to_string(const std::vector<std::uint8_t> &v) {
  return std::string(reinterpret_cast<const char *>(v.data()), v.size());
}

inline std::vector<std::uint8_t> string_to_bytes(const std::string &s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

} // namespace

AriWsClient::AriWsClient(Config cfg,
                          std::function<void(const std::string &)> on_event,
                          std::function<void()>                    on_disconnect)
    : m_cfg(std::move(cfg)),
      m_on_event(std::move(on_event)),
      m_on_disconnect(std::move(on_disconnect)),
      m_handle(ACE_INVALID_HANDLE) {}

AriWsClient::AriWsClient(ACE_HANDLE fd,
                          std::function<void(const std::string &)> on_event,
                          std::function<void()>                    on_disconnect)
    : m_on_event(std::move(on_event)),
      m_on_disconnect(std::move(on_disconnect)),
      m_handle(fd) {
  m_stream.set_handle(fd);
}

AriWsClient::~AriWsClient() {
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
}

// ── connect_and_handshake ────────────────────────────────────────────────────

bool AriWsClient::connect_and_handshake() {
  ACE_INET_Addr addr(m_cfg.port, m_cfg.host.c_str());
  ACE_Time_Value timeout(10);
  ACE_SOCK_Connector conn;
  if (conn.connect(m_stream, addr, &timeout) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AriWsClient] connect failed host=%s port=%u "
                        "errno=%d\n"),
               m_cfg.host.c_str(), static_cast<unsigned>(m_cfg.port), errno));
    return false;
  }
  m_handle = m_stream.get_handle();

  auto [req, key] = build_upgrade_request(m_cfg.host, m_cfg.app_name,
                                            m_cfg.username, m_cfg.password);
  if (m_stream.send_n(req.data(), req.size())
        != static_cast<ssize_t>(req.size())) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AriWsClient] failed to send WS upgrade\n")));
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
    return false;
  }

  std::string headers;
  headers.reserve(512);
  char c = 0;
  while (headers.size() < 4096) {
    if (m_stream.recv_n(&c, 1) != 1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AriWsClient] lost connection during "
                          "WS upgrade\n")));
      m_stream.close();
      m_handle = ACE_INVALID_HANDLE;
      return false;
    }
    headers += c;
    if (headers.size() >= 4 &&
        headers.substr(headers.size() - 4) == "\r\n\r\n")
      break;
  }

  if (!validate_upgrade_response(headers, key)) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AriWsClient] WS upgrade rejected:\n%s\n"),
               headers.c_str()));
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
    return false;
  }

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [AriWsClient] connected to Asterisk ARI %s:%u "
                      "(app=%s)\n"),
             m_cfg.host.c_str(), static_cast<unsigned>(m_cfg.port),
             m_cfg.app_name.c_str()));
  return true;
}

int AriWsClient::register_with_reactor(ACE_Reactor *reactor) {
  if (!reactor) return -1;
  this->reactor(reactor);
  return reactor->register_handler(this, ACE_Event_Handler::READ_MASK);
}

// ── ACE_Event_Handler ────────────────────────────────────────────────────────

int AriWsClient::handle_input(ACE_HANDLE /*h*/) {
  std::uint8_t chunk[kReadChunk];
  const ssize_t n = m_stream.recv(chunk, sizeof(chunk));
  if (n <= 0) {
    notify_disconnect_once();
    return -1;
  }
  m_recv_buf.insert(m_recv_buf.end(), chunk, chunk + n);
  return drain_frames() ? 0 : -1;
}

bool AriWsClient::drain_frames() {
  for (;;) {
    auto maybe = wsframe::decode(m_recv_buf);
    if (!maybe.has_value()) return true; // need more bytes

    const auto &frame = *maybe;
    switch (frame.opcode) {
    case kOpcodeBinary:
    case kOpcodeText:
    case kOpcodeContinuation:
      if (m_on_event) m_on_event(bytes_to_string(frame.payload));
      if (m_handle == ACE_INVALID_HANDLE) return false;
      break;
    case kOpcodePing: {
      // Client→server frames MUST be masked (RFC 6455 §5.2).
      const auto pong = wsframe::encode(frame.payload, kOpcodePong,
                                          /*mask=*/true);
      m_stream.send_n(pong.data(), pong.size());
      break;
    }
    case kOpcodePong:
      break;
    case kOpcodeClose: {
      const auto cls = wsframe::close_frame();
      m_stream.send_n(cls.data(), cls.size());
      notify_disconnect_once();
      return false;
    }
    default:
      notify_disconnect_once();
      return false;
    }
  }
}

int AriWsClient::handle_close(ACE_HANDLE /*h*/, ACE_Reactor_Mask /*m*/) {
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  // Owned by the caller (typically pbx-agent/main), not by ACE. The
  // caller's supervisor loop decides whether/when to reconnect.
  return 0;
}

void AriWsClient::close() {
  if (m_handle == ACE_INVALID_HANDLE) return;
  const auto cls = wsframe::close_frame();
  m_stream.send_n(cls.data(), cls.size());
  m_stream.close();
  m_handle = ACE_INVALID_HANDLE;
  m_close_notified = true;
}

void AriWsClient::notify_disconnect_once() {
  if (m_close_notified) return;
  m_close_notified = true;
  if (m_on_disconnect) m_on_disconnect();
}

// ── Pure-logic helpers ───────────────────────────────────────────────────────

std::string AriWsClient::base64_encode(const std::string &in) {
  static const char alpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= in.size()) {
    const auto b0 = static_cast<unsigned char>(in[i]);
    const auto b1 = static_cast<unsigned char>(in[i + 1]);
    const auto b2 = static_cast<unsigned char>(in[i + 2]);
    out.push_back(alpha[(b0 >> 2) & 0x3F]);
    out.push_back(alpha[((b0 << 4) | (b1 >> 4)) & 0x3F]);
    out.push_back(alpha[((b1 << 2) | (b2 >> 6)) & 0x3F]);
    out.push_back(alpha[b2 & 0x3F]);
    i += 3;
  }
  if (i < in.size()) {
    const auto b0 = static_cast<unsigned char>(in[i]);
    out.push_back(alpha[(b0 >> 2) & 0x3F]);
    if (i + 1 == in.size()) {
      out.push_back(alpha[(b0 << 4) & 0x3F]);
      out.push_back('=');
      out.push_back('=');
    } else {
      const auto b1 = static_cast<unsigned char>(in[i + 1]);
      out.push_back(alpha[((b0 << 4) | (b1 >> 4)) & 0x3F]);
      out.push_back(alpha[(b1 << 2) & 0x3F]);
      out.push_back('=');
    }
  }
  return out;
}

std::pair<std::string, std::string>
AriWsClient::build_upgrade_request(const std::string &host,
                                    const std::string &app,
                                    const std::string &username,
                                    const std::string &password) {
  const std::string key = random_ws_key();
  const std::string basic = base64_encode(username + ":" + password);

  std::ostringstream os;
  os << "GET /ari/events?app=" << app << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Upgrade: websocket\r\n"
     << "Connection: Upgrade\r\n"
     << "Sec-WebSocket-Key: " << key << "\r\n"
     << "Sec-WebSocket-Version: 13\r\n"
     << "Authorization: Basic " << basic << "\r\n"
     << "\r\n";
  return {os.str(), key};
}

bool AriWsClient::validate_upgrade_response(
    const std::string &headers, const std::string &sec_websocket_key) {
  if (headers.compare(0, 9, "HTTP/1.1 ") != 0 &&
      headers.compare(0, 9, "HTTP/1.0 ") != 0)
    return false;
  if (headers.find(" 101 ") == std::string::npos &&
      headers.compare(9, 4, "101 ") != 0)
    return false;
  const std::string expected = wsframe::accept_key(sec_websocket_key);
  return headers.find(expected) != std::string::npos;
}
