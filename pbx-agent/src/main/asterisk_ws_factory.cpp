#include "asterisk_ws_factory.hpp"
#include "wsframe.hpp"

#include "ace/INET_Addr.h"
#include "ace/Log_Msg.h"
#include "ace/Reactor.h"
#include "ace/SOCK_Connector.h"
#include "ace/Time_Value.h"

#include <openssl/rand.h>

#include <cstring>
#include <sstream>
#include <utility>

namespace {

constexpr std::size_t kReadChunk = 65536;

constexpr std::uint8_t kOpcodeContinuation = 0x0;
constexpr std::uint8_t kOpcodeText         = 0x1;
constexpr std::uint8_t kOpcodeBinary       = 0x2;
constexpr std::uint8_t kOpcodeClose        = 0x8;
constexpr std::uint8_t kOpcodePing         = 0x9;
constexpr std::uint8_t kOpcodePong         = 0xA;

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

// ─────────────────────────────────────────────────────────────────────────────
// AsteriskStream
// ─────────────────────────────────────────────────────────────────────────────

AsteriskStream::AsteriskStream(SipFrameDemux &demux, std::uint32_t stream_id)
    : m_demux(demux), m_stream_id(stream_id) {}

AsteriskStream::AsteriskStream(SipFrameDemux &demux, std::uint32_t stream_id,
                                ACE_HANDLE fd)
    : m_demux(demux), m_stream_id(stream_id), m_handle(fd) {
  m_stream.set_handle(fd);
}

AsteriskStream::~AsteriskStream() {
  // Detach adapter first so any straggling adapter.close() / send_bytes()
  // call after we've started destruction is a no-op rather than UB.
  if (m_adapter) {
    m_adapter->m_s = nullptr;
    m_adapter = nullptr;
  }
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
}

bool AsteriskStream::connect_and_handshake(const std::string &host,
                                            std::uint16_t      port,
                                            const std::string &path) {
  ACE_INET_Addr addr(port, host.c_str());
  ACE_Time_Value timeout(10);
  ACE_SOCK_Connector conn;
  if (conn.connect(m_stream, addr, &timeout) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AsteriskStream sid=%u] connect failed host=%s "
                        "port=%u errno=%d\n"),
               m_stream_id, host.c_str(), static_cast<unsigned>(port), errno));
    return false;
  }
  m_handle = m_stream.get_handle();

  auto [req, key] = build_upgrade_request(host, path);
  if (m_stream.send_n(req.data(), req.size())
        != static_cast<ssize_t>(req.size())) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AsteriskStream sid=%u] failed to send WS "
                        "upgrade\n"), m_stream_id));
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
                 ACE_TEXT("%D [AsteriskStream sid=%u] lost connection "
                          "during WS upgrade\n"), m_stream_id));
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
               ACE_TEXT("%D [AsteriskStream sid=%u] WS upgrade rejected:\n%s\n"),
               m_stream_id, headers.c_str()));
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
    return false;
  }

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [AsteriskStream sid=%u] connected to %s:%u%s\n"),
             m_stream_id, host.c_str(), static_cast<unsigned>(port),
             path.c_str()));
  return true;
}

int AsteriskStream::register_with_reactor(ACE_Reactor *reactor) {
  if (!reactor) return -1;
  this->reactor(reactor);
  return reactor->register_handler(this, ACE_Event_Handler::READ_MASK);
}

std::unique_ptr<IAsteriskStream> AsteriskStream::make_adapter() {
  auto a = std::make_unique<DemuxAdapter>(this);
  m_adapter = a.get();
  return a;
}

// ── handle_input ─────────────────────────────────────────────────────────────

int AsteriskStream::handle_input(ACE_HANDLE /*h*/) {
  std::uint8_t chunk[kReadChunk];
  const ssize_t n = m_stream.recv(chunk, sizeof(chunk));
  if (n <= 0) {
    notify_eof_once("peer_closed");
    return -1;
  }
  m_recv_buf.insert(m_recv_buf.end(), chunk, chunk + n);
  return drain_frames() ? 0 : -1;
}

bool AsteriskStream::drain_frames() {
  for (;;) {
    auto maybe = wsframe::decode(m_recv_buf);
    if (!maybe.has_value()) return true;

    const auto &frame = *maybe;
    switch (frame.opcode) {
    case kOpcodeBinary:
    case kOpcodeText:
    case kOpcodeContinuation:
      m_demux.on_asterisk_data(m_stream_id, bytes_to_string(frame.payload));
      // The demux can release us mid-call (drops our entry from
      // m_streams → adapter destroyed → m_adapter dangles). Bail out
      // if we got detached.
      if (m_adapter == nullptr) return false;
      break;
    case kOpcodePing: {
      // Client→server frames MUST be masked (RFC 6455 §5.2). The
      // shorthand `wsframe::pong_frame()` doesn't mask, so we encode
      // the pong explicitly with mask=true.
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
      notify_eof_once("asterisk_closed");
      return false;
    }
    default:
      notify_eof_once("ws_protocol_error");
      return false;
    }
  }
}

int AsteriskStream::handle_close(ACE_HANDLE /*h*/, ACE_Reactor_Mask /*m*/) {
  notify_eof_once("handle_close");
  if (m_handle != ACE_INVALID_HANDLE) {
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  delete this;
  return 0;
}

bool AsteriskStream::write_bytes(const std::string &bytes) {
  if (m_handle == ACE_INVALID_HANDLE) return false;
  // RFC 7118 §2 mandates text frames for SIP-over-WS. Client-side
  // (us-to-Asterisk) MUST be masked per RFC 6455 §5.2.
  const auto framed =
      wsframe::encode(string_to_bytes(bytes), kOpcodeText, /*mask=*/true);
  const ssize_t n = m_stream.send_n(framed.data(), framed.size());
  if (n != static_cast<ssize_t>(framed.size())) {
    notify_eof_once("asterisk_write_failed");
    return false;
  }
  return true;
}

void AsteriskStream::close_socket(const std::string &reason) {
  // Demux is releasing us via unique_ptr.reset() on its map — the
  // adapter is about to die. Drop our pointer first.
  m_adapter = nullptr;
  if (m_handle != ACE_INVALID_HANDLE) {
    // Best-effort WS close + close fd. The reactor will eventually call
    // handle_close on our zombie state and delete this.
    const auto cls = wsframe::close_frame();
    m_stream.send_n(cls.data(), cls.size());
    m_stream.close();
    m_handle = ACE_INVALID_HANDLE;
  }
  // Don't call notify_eof_once — we're being released BY the demux, so
  // notifying it back would be redundant and re-entrant.
  m_eof_notified = true;
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [AsteriskStream sid=%u] closed by demux: %s\n"),
             m_stream_id, reason.c_str()));
}

void AsteriskStream::notify_eof_once(const std::string &reason) {
  if (m_eof_notified) return;
  m_eof_notified = true;
  // Detach adapter BEFORE telling the demux to release; the demux will
  // destroy the adapter via m_streams.erase() inside on_asterisk_eof.
  if (m_adapter) {
    m_adapter->m_s = nullptr;
    m_adapter = nullptr;
  }
  m_demux.on_asterisk_eof(m_stream_id, reason);
}

// ── Pure-logic helpers ───────────────────────────────────────────────────────

std::pair<std::string, std::string>
AsteriskStream::build_upgrade_request(const std::string &host,
                                       const std::string &path) {
  const std::string key = random_ws_key();
  std::ostringstream os;
  // chan_pjsip's WS transport requires the Sec-WebSocket-Protocol: sip
  // header (RFC 7118 §4). Asterisk replies with the same value if it
  // accepts the subprotocol.
  os << "GET " << path << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Upgrade: websocket\r\n"
     << "Connection: Upgrade\r\n"
     << "Sec-WebSocket-Key: " << key << "\r\n"
     << "Sec-WebSocket-Version: 13\r\n"
     << "Sec-WebSocket-Protocol: sip\r\n"
     << "\r\n";
  return {os.str(), key};
}

bool AsteriskStream::validate_upgrade_response(
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

// ─────────────────────────────────────────────────────────────────────────────
// AsteriskWsFactory
// ─────────────────────────────────────────────────────────────────────────────

AsteriskWsFactory::AsteriskWsFactory(ACE_Reactor *reactor,
                                      SipFrameDemux &demux,
                                      std::string host, std::uint16_t port,
                                      std::string path)
    : m_reactor(reactor), m_demux(demux), m_host(std::move(host)),
      m_port(port), m_path(std::move(path)) {}

std::unique_ptr<IAsteriskStream>
AsteriskWsFactory::open(std::uint32_t stream_id, const std::string &meta) {
  (void)meta; // chan_pjsip's WS doesn't take per-stream metadata at upgrade

  auto stream = std::make_unique<AsteriskStream>(m_demux, stream_id);
  if (!stream->connect_and_handshake(m_host, m_port, m_path)) {
    return nullptr;
  }
  if (m_reactor && stream->register_with_reactor(m_reactor) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AsteriskWsFactory] register_handler failed for "
                        "sid=%u\n"), stream_id));
    return nullptr;
  }

  // From here on, ACE owns the stream (delete this in handle_close). We
  // give the demux a non-owning view via the adapter, and release the
  // unique_ptr that was holding it.
  AsteriskStream *raw = stream.release();
  return raw->make_adapter();
}
