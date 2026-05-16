#include "ace_ssl_transport.hpp"
#include "innertls.hpp"
#include "wsframe.hpp"
#include "ws_inner_tls_bridge.hpp"

#include "ace/Log_Msg.h"
#include "ace/INET_Addr.h"
#include "ace/Reactor.h"
#include "ace/SSL/SSL_SOCK_Connector.h"
#include "ace/Time_Value.h"

#include <openssl/rand.h>
#include <openssl/ssl.h>

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

/// Base64-encode 16 random bytes, matching what xpmile's wsdbagent does.
std::string random_ws_key() {
  std::uint8_t raw[16];
  if (RAND_bytes(raw, sizeof(raw)) != 1) {
    // Extremely unlikely; fall back to zeros (still a valid 16-byte key
    // syntactically; the server will accept it because it only echoes a
    // SHA-1 of (key + magic) — there's no entropy requirement on the
    // client side from the server's perspective).
    std::memset(raw, 0, sizeof(raw));
  }
  // Trivial base64 encoder for 16 bytes -> 24-char output (with '=').
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

AceSslTransport::AceSslTransport(
    std::function<void(const std::string &)> on_bytes,
    std::function<void()>                    on_disconnect)
    : m_on_bytes(std::move(on_bytes)),
      m_on_disconnect(std::move(on_disconnect)) {}

AceSslTransport::~AceSslTransport() {
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  // Explicit reset in declaration-reverse order: m_inner_tls holds a
  // reference into m_bridge, so the inner TLS object must die first.
  // unique_ptr destruction order already gives this, but make it visible.
  m_inner_tls.reset();
  m_bridge.reset();
}

// ── connect_and_handshake ────────────────────────────────────────────────────

bool AceSslTransport::connect_and_handshake(const std::string &host,
                                             std::uint16_t      port,
                                             const std::string &cert_path,
                                             const std::string &key_path,
                                             const std::string &ca_path,
                                             const std::string &path,
                                             const InnerTlsConfig &inner_tls) {
  // ── mTLS context ───────────────────────────────────────────────────────
  ACE_SSL_Context *ctx = ACE_SSL_Context::instance();
  ctx->set_mode(ACE_SSL_Context::SSLv23_client);
  if (!ca_path.empty())   ctx->load_trusted_ca(ca_path.c_str());
  if (!cert_path.empty()) ctx->certificate(cert_path.c_str(), SSL_FILETYPE_PEM);
  if (!key_path.empty())  ctx->private_key(key_path.c_str(), SSL_FILETYPE_PEM);
  if (!ca_path.empty())
    SSL_CTX_set_verify(ctx->context(), SSL_VERIFY_PEER, nullptr);

  // ── TLS connect ────────────────────────────────────────────────────────
  ACE_INET_Addr addr(port, host.c_str());
  ACE_Time_Value timeout(10);
  ACE_SSL_SOCK_Connector conn;
  if (conn.connect(m_stream, addr, &timeout) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AceSslTransport] SSL connect failed "
                        "host=%s port=%u errno=%d\n"),
               host.c_str(), static_cast<unsigned>(port), errno));
    return false;
  }
  m_handle = m_stream.get_handle();

  // ── WebSocket upgrade ─────────────────────────────────────────────────
  auto [req, key] = build_upgrade_request(host, path);
  if (m_stream.send_n(req.data(), req.size())
        != static_cast<ssize_t>(req.size())) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AceSslTransport] failed to send WS upgrade\n")));
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
                 ACE_TEXT("%D [AceSslTransport] lost connection during "
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
               ACE_TEXT("%D [AceSslTransport] WS upgrade rejected:\n%s\n"),
               headers.c_str()));
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
    return false;
  }

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [AceSslTransport] connected + WS-upgraded "
                      "%s:%u%s (handle=%d)\n"),
             host.c_str(), static_cast<unsigned>(port), path.c_str(),
             static_cast<int>(m_handle)));

  // ── Inner TLS (over the WS) ──────────────────────────────────────────
  //
  // Heroku terminates the outer TLS at the router so what reaches the
  // dyno is plain HTTP/WS — the agent's `--tls-cert` is never verified
  // against a peer. The real mTLS trust boundary is this inner TLS layer
  // riding the WS binary frames. Skip when no cert configured so unit
  // tests that drive `handle_input` with raw frames keep working; in
  // production the cloud's `AgentStream` requires the inner handshake
  // and the agent would fail downstream without it.
  if (!inner_tls.cert_path.empty()) {
    auto recv_raw = [this](void *buf, std::size_t cap) -> long {
      return static_cast<long>(m_stream.recv(buf, cap));
    };
    auto send_raw = [this](const void *buf, std::size_t len) -> long {
      return static_cast<long>(m_stream.send_n(buf, len));
    };
    m_bridge = std::make_unique<WsInnerTlsBridge>(std::move(recv_raw),
                                                   std::move(send_raw),
                                                   /*client_mask=*/true);
    m_inner_tls = std::make_unique<InnerTlsClient>(*m_bridge);
    if (!inner_tls.ca_path.empty())
      m_inner_tls->set_ca(inner_tls.ca_path);
    if (!m_inner_tls->set_cert(inner_tls.cert_path, inner_tls.key_path)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AceSslTransport] inner-TLS set_cert failed "
                          "(cert=%s key=%s)\n"),
                 inner_tls.cert_path.c_str(), inner_tls.key_path.c_str()));
      m_inner_tls.reset();
      m_bridge.reset();
      m_stream.close();
      m_handle = ACE_INVALID_HANDLE;
      return false;
    }
    if (!m_inner_tls->handshake()) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AceSslTransport] inner-TLS handshake "
                          "failed\n")));
      m_inner_tls.reset();
      m_bridge.reset();
      m_stream.close();
      m_handle = ACE_INVALID_HANDLE;
      return false;
    }
    if (!inner_tls.hostname.empty() &&
        !m_inner_tls->verify_hostname(inner_tls.hostname)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AceSslTransport] inner-TLS hostname "
                          "verification failed: expected %s\n"),
                 inner_tls.hostname.c_str()));
      m_inner_tls.reset();
      m_bridge.reset();
      m_stream.close();
      m_handle = ACE_INVALID_HANDLE;
      return false;
    }
    m_bridge->switch_to_buffered();
    // Carry any bytes the handshake pulled from the socket past its
    // final record into the steady-state frame-decoder buffer; without
    // this, a server message that landed in the same TCP segment as
    // the handshake's `Finished` would be silently dropped.
    m_recv_buf = m_bridge->leftover_socket_bytes();
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [AceSslTransport] inner-TLS handshake "
                        "established\n")));
  }
  return true;
}

int AceSslTransport::register_with_reactor(ACE_Reactor *reactor) {
  if (!reactor) return -1;
  this->reactor(reactor);
  return reactor->register_handler(this, ACE_Event_Handler::READ_MASK);
}

// ── ITransport ───────────────────────────────────────────────────────────────

bool AceSslTransport::send(const std::string &bytes) {
  if (m_handle == ACE_INVALID_HANDLE) return false;

  // Inner-TLS path: the bridge encrypts via OpenSSL, wraps each TLS
  // record in one masked WS binary frame, and writes through m_stream.
  if (m_inner_tls) {
    if (!m_inner_tls->send(string_to_bytes(bytes))) {
      notify_disconnect_once();
      return false;
    }
    return true;
  }

  // Test-only path: raw text WS frame, no encryption. Production always
  // configures inner TLS so this branch never runs against a live cloud.
  // Mask client-to-server frames per RFC 6455 §5.2.
  const auto framed = wsframe::encode(string_to_bytes(bytes),
                                       kOpcodeText, /*mask=*/true);
  const ssize_t n = m_stream.send_n(framed.data(), framed.size());
  if (n != static_cast<ssize_t>(framed.size())) {
    // send_n failure is fatal — treat as disconnect.
    notify_disconnect_once();
    return false;
  }
  return true;
}

void AceSslTransport::close() {
  if (m_handle == ACE_INVALID_HANDLE) return;
  // Best-effort WS close frame, then tear down.
  const auto cls = wsframe::close_frame();
  m_stream.send_n(cls.data(), cls.size());
  m_stream.close();
  m_handle = ACE_INVALID_HANDLE;
  // Don't call on_disconnect — close() is called BY the connector, so
  // notifying it back would be redundant. notify_disconnect_once() is
  // still called from the destructor if it never ran (idempotent).
  m_close_notified = true;
}

// ── ACE_Event_Handler ────────────────────────────────────────────────────────

int AceSslTransport::handle_input(ACE_HANDLE /*h*/) {
  std::uint8_t chunk[kReadChunk];
  const ssize_t n = m_stream.recv(chunk, sizeof(chunk));
  if (n <= 0) {
    notify_disconnect_once();
    return -1;
  }
  m_recv_buf.insert(m_recv_buf.end(), chunk, chunk + n);
  return drain_frames() ? 0 : -1;
}

bool AceSslTransport::drain_frames() {
  for (;;) {
    auto maybe = wsframe::decode(m_recv_buf);
    if (!maybe.has_value()) return true; // need more bytes

    const auto &frame = *maybe;
    switch (frame.opcode) {
    case kOpcodeBinary:
    case kOpcodeText:
    case kOpcodeContinuation:
      if (m_inner_tls) {
        // Encrypted record arrived. Push the binary payload into the
        // bridge's queue, then drain plaintext via the inner TLS recv
        // loop. SSL_read inside InnerTls::recv drains every record it
        // can parse from the BIO, so one call here yields the full
        // plaintext for this WS frame.
        m_bridge->push_inbound(frame.payload);
        for (;;) {
          std::vector<std::uint8_t> plaintext;
          if (!m_inner_tls->recv(plaintext)) {
            notify_disconnect_once();
            return false;
          }
          if (plaintext.empty()) break;  // WANT_READ — fully drained
          if (m_on_bytes) m_on_bytes(bytes_to_string(plaintext));
          if (m_handle == ACE_INVALID_HANDLE) return false;
        }
      } else {
        if (m_on_bytes) m_on_bytes(bytes_to_string(frame.payload));
        if (m_handle == ACE_INVALID_HANDLE) return false;
      }
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

int AceSslTransport::handle_close(ACE_HANDLE /*h*/, ACE_Reactor_Mask /*m*/) {
  notify_disconnect_once();
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  // Note: AceSslTransport is owned by CloudConnector's m_transport
  // unique_ptr, NOT by ACE. We don't delete this — the connector will
  // drop us when notify_disconnect_once → on_disconnect → connector
  // resets m_transport.
  return 0;
}

void AceSslTransport::notify_disconnect_once() {
  if (m_close_notified) return;
  m_close_notified = true;
  if (m_on_disconnect) m_on_disconnect();
}

// ── Pure-logic helpers ───────────────────────────────────────────────────────

std::pair<std::string, std::string>
AceSslTransport::build_upgrade_request(const std::string &host,
                                        const std::string &path) {
  const std::string key = random_ws_key();
  std::ostringstream os;
  os << "GET " << path << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Upgrade: websocket\r\n"
     << "Connection: Upgrade\r\n"
     << "Sec-WebSocket-Key: " << key << "\r\n"
     << "Sec-WebSocket-Version: 13\r\n"
     << "\r\n";
  return {os.str(), key};
}

bool AceSslTransport::validate_upgrade_response(
    const std::string &headers, const std::string &sec_websocket_key) {
  // Must be a 101 Switching Protocols.
  if (headers.compare(0, 9, "HTTP/1.1 ") != 0 &&
      headers.compare(0, 9, "HTTP/1.0 ") != 0)
    return false;
  if (headers.find(" 101 ") == std::string::npos &&
      headers.compare(9, 4, "101 ") != 0)
    return false;

  // Must echo the expected accept.
  const std::string expected = wsframe::accept_key(sec_websocket_key);
  if (headers.find(expected) == std::string::npos) return false;

  return true;
}

// ── AceSslTransportFactory ───────────────────────────────────────────────────

AceSslTransportFactory::AceSslTransportFactory(
    ACE_Reactor                              *reactor,
    std::function<void(const std::string &)>  on_bytes,
    std::function<void()>                     on_disconnect,
    AceSslTransport::InnerTlsConfig           inner_tls)
    : m_reactor(reactor), m_on_bytes(std::move(on_bytes)),
      m_on_disconnect(std::move(on_disconnect)),
      m_inner_tls(std::move(inner_tls)) {}

std::unique_ptr<ITransport>
AceSslTransportFactory::create_connected(const std::string &host,
                                          std::uint16_t      port,
                                          const std::string &cert_path,
                                          const std::string &key_path,
                                          const std::string &ca_path) {
  auto t = std::make_unique<AceSslTransport>(m_on_bytes, m_on_disconnect);
  if (!t->connect_and_handshake(host, port, cert_path, key_path, ca_path,
                                  "/agent", m_inner_tls)) {
    return nullptr;
  }
  if (m_reactor && t->register_with_reactor(m_reactor) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AceSslTransportFactory] register_handler "
                        "failed\n")));
    return nullptr;
  }
  return t;
}
