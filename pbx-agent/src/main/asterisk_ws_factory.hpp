#ifndef ASTERISK_WS_FACTORY_HPP
#define ASTERISK_WS_FACTORY_HPP

#include "sip_frame_demux.hpp"
#include "ace/Event_Handler.h"
#include "ace/SOCK_Stream.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ACE_Reactor;

/**
 * @file asterisk_ws_factory.hpp
 * @brief Plain-TCP WebSocket adapter from `SipFrameDemux` to local
 *        Asterisk's `chan_pjsip` WS transport (`ws://127.0.0.1:8088/ws`).
 *
 * Symmetric to `AgentStream` on the cloud side. Same lifetime pattern:
 *   - `AsteriskStream` is an `ACE_Event_Handler`, owned by ACE (delete
 *     this in handle_close).
 *   - A private `DemuxAdapter` implements `IAsteriskStream` with a
 *     non-owning back-pointer to the AsteriskStream. The demux owns the
 *     adapter via `unique_ptr` (one per stream-id in its map).
 *   - On every release path the back-pointer is nulled BEFORE the
 *     adapter is destroyed, so the demux can't dereference a dead
 *     AsteriskStream.
 *
 * Auth: none at the WS layer. chan_pjsip's WS transport accepts
 * unauthenticated upgrades; SIP digest auth happens inside the SIP
 * REGISTER messages that ride the WS frames.
 */

class AsteriskStream : public ACE_Event_Handler {
public:
  /// Production constructor — dial + WS upgrade happen in
  /// `connect_and_handshake()`.
  AsteriskStream(SipFrameDemux &demux, std::uint32_t stream_id);

  /// Test constructor — takes a pre-connected fd, skips connect and
  /// reactor registration.
  AsteriskStream(SipFrameDemux &demux, std::uint32_t stream_id,
                  ACE_HANDLE fd);

  ~AsteriskStream() override;

  AsteriskStream(const AsteriskStream &) = delete;
  AsteriskStream &operator=(const AsteriskStream &) = delete;

  /// Plain TCP connect + HTTP/1.1 WS upgrade to `host:port` at @p path.
  /// Returns true on full success (101 + matching Sec-WebSocket-Accept).
  bool connect_and_handshake(const std::string &host, std::uint16_t port,
                              const std::string &path = "/ws");

  /// Register with @p reactor for `READ_MASK`.
  int register_with_reactor(ACE_Reactor *reactor);

  /// Hand the demux its `IAsteriskStream` view of us. Returns a unique_ptr
  /// to a freshly-constructed adapter pointing back at this object.
  /// Called exactly once, by the factory.
  std::unique_ptr<IAsteriskStream> make_adapter();

  // ── ACE_Event_Handler ──────────────────────────────────────────────────

  int        handle_input(ACE_HANDLE)                   override;
  int        handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;
  ACE_HANDLE get_handle() const                         override {
    return m_handle;
  }

  // ── Adapter callbacks (private impl details exposed for tests) ─────────

  /// Encode bytes as a WS text frame (RFC 7118 §2) and send to Asterisk.
  bool write_bytes(const std::string &bytes);

  /// Close the socket. Idempotent. Called via the adapter's close().
  void close_socket(const std::string &reason);

  // ── Pure-logic helpers (public for unit tests) ─────────────────────────

  static std::pair<std::string, std::string>
  build_upgrade_request(const std::string &host, const std::string &path);

  static bool validate_upgrade_response(const std::string &headers,
                                         const std::string &sec_websocket_key);

private:
  /// `IAsteriskStream` impl owned by the demux. Holds a back pointer to
  /// the AsteriskStream which the latter nulls at every release point.
  struct DemuxAdapter : public IAsteriskStream {
    AsteriskStream *m_s;
    explicit DemuxAdapter(AsteriskStream *s) : m_s(s) {}
    bool send_bytes(const std::string &b) override {
      return m_s ? m_s->write_bytes(b) : false;
    }
    void close(const std::string &reason) override {
      if (m_s) m_s->close_socket(reason);
    }
  };

  bool drain_frames();
  void notify_eof_once(const std::string &reason);

  SipFrameDemux           &m_demux;
  std::uint32_t            m_stream_id;
  ACE_HANDLE               m_handle = ACE_INVALID_HANDLE;
  ACE_SOCK_Stream          m_stream;
  DemuxAdapter            *m_adapter = nullptr; // non-owning; demux owns
  std::vector<std::uint8_t> m_recv_buf;
  bool                     m_eof_notified = false;
};

/// Factory that produces one connected `AsteriskStream` per OPEN frame
/// the demux sees from the cloud. Construct one of these and pass it
/// to `SipFrameDemux`'s constructor.
class AsteriskWsFactory : public IAsteriskFactory {
public:
  /// @param reactor  Reactor on which to register new streams.
  /// @param demux    Demux to notify on inbound bytes / EOF.
  /// @param host     Asterisk hostname (default "127.0.0.1").
  /// @param port     Asterisk WS port (default 8088).
  /// @param path     Asterisk chan_pjsip WS path (default "/ws").
  AsteriskWsFactory(ACE_Reactor *reactor, SipFrameDemux &demux,
                     std::string host = "127.0.0.1",
                     std::uint16_t port = 8088,
                     std::string path = "/ws");

  std::unique_ptr<IAsteriskStream> open(std::uint32_t stream_id,
                                         const std::string &meta) override;

private:
  ACE_Reactor   *m_reactor;
  SipFrameDemux &m_demux;
  std::string    m_host;
  std::uint16_t  m_port;
  std::string    m_path;
};

#endif // ASTERISK_WS_FACTORY_HPP
