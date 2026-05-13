#ifndef ACE_SSL_TRANSPORT_HPP
#define ACE_SSL_TRANSPORT_HPP

#include "cloud_connector.hpp"
#include "ace/Event_Handler.h"
#include "ace/SSL/SSL_SOCK_Stream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ACE_Reactor;

/**
 * @file ace_ssl_transport.hpp
 * @brief Agent-side outbound mTLS + WebSocket transport for `CloudConnector`.
 *
 * Implements `ITransport`. Production code wires this via
 * `AceSslTransportFactory` (which implements
 * `CloudConnector::ITransportFactory`). The handshake follows the
 * canonical pattern from xpmile's `wsdbagent` — `ACE_SSL_Context` set
 * up with CA + client cert + client key, `ACE_SSL_SOCK_Connector` with
 * a 10-second timeout, then an HTTP/1.1 WebSocket upgrade to `/agent`
 * validated against `wsframe::accept_key`.
 *
 * Post-upgrade traffic is multiplexed onto the SSL stream as WebSocket
 * frames. `ITransport::send(bytes)` encodes one text frame and writes
 * via `send_n`; `handle_input` reads via `recv` into a buffer, runs
 * `wsframe::decode` in a loop, and forwards each text/binary payload
 * to the injected `on_bytes` callback.
 *
 * The reactor binding is non-owning — production passes the
 * connector's reactor at construction; tests can skip
 * `register_with_reactor()` and drive `handle_input` directly with a
 * pre-connected fd.
 */

class AceSslTransport : public ITransport, public ACE_Event_Handler {
public:
  /// @param on_bytes      Called with the SipFrame-bytes payload of each
  ///                      inbound text/binary WS frame. Production wires
  ///                      this to `CloudConnector::on_bytes_received`.
  /// @param on_disconnect Called once when the transport observes a
  ///                      socket error, peer close, or `close()` from
  ///                      the connector. Production wires this to
  ///                      `CloudConnector::on_transport_lost`.
  AceSslTransport(std::function<void(const std::string &)> on_bytes,
                   std::function<void()>                     on_disconnect);

  ~AceSslTransport() override;

  AceSslTransport(const AceSslTransport &) = delete;
  AceSslTransport &operator=(const AceSslTransport &) = delete;

  /// Build the `ACE_SSL_Context`, dial @p host:@p port, perform the
  /// WebSocket upgrade to @p path. Returns true on full success.
  bool connect_and_handshake(const std::string &host, std::uint16_t port,
                              const std::string &cert_path,
                              const std::string &key_path,
                              const std::string &ca_path,
                              const std::string &path = "/agent");

  /// Register with @p reactor for `READ_MASK`. Production calls this
  /// after a successful `connect_and_handshake`. Tests can skip it and
  /// drive `handle_input` directly.
  int register_with_reactor(ACE_Reactor *reactor);

  // ── ITransport ─────────────────────────────────────────────────────────

  bool send(const std::string &bytes) override;
  void close() override;

  // ── ACE_Event_Handler ──────────────────────────────────────────────────

  int handle_input(ACE_HANDLE) override;
  int handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;
  ACE_HANDLE get_handle() const override { return m_handle; }

  // ── Pure-logic helpers (public for unit tests) ─────────────────────────

  /// Generate the WebSocket upgrade request to send to the server.
  /// Returns `(request_bytes, sec_websocket_key)` — the caller passes the
  /// key to `validate_upgrade_response` after reading the 101 reply.
  static std::pair<std::string, std::string>
  build_upgrade_request(const std::string &host, const std::string &path);

  /// Validate the response header block from the server. @p
  /// sec_websocket_key is the one we sent in the request.
  /// @return true iff the response is a `101 Switching Protocols` with
  /// a matching `Sec-WebSocket-Accept`.
  static bool validate_upgrade_response(const std::string &headers,
                                         const std::string &sec_websocket_key);

private:
  bool drain_frames();
  void notify_disconnect_once();

  std::function<void(const std::string &)> m_on_bytes;
  std::function<void()>                    m_on_disconnect;
  ACE_SSL_SOCK_Stream                      m_stream;
  ACE_HANDLE                               m_handle = ACE_INVALID_HANDLE;
  std::vector<std::uint8_t>                m_recv_buf;
  bool                                     m_close_notified = false;
};

/// `ITransportFactory` that returns real `AceSslTransport` instances.
/// Construct this in `main()` after building `CloudConnector` and pass
/// it to the connector's constructor.
class AceSslTransportFactory : public ITransportFactory {
public:
  AceSslTransportFactory(ACE_Reactor                              *reactor,
                          std::function<void(const std::string &)> on_bytes,
                          std::function<void()>                     on_disconnect);

  std::unique_ptr<ITransport>
  create_connected(const std::string &host, std::uint16_t port,
                   const std::string &cert_path,
                   const std::string &key_path,
                   const std::string &ca_path) override;

private:
  ACE_Reactor                              *m_reactor;
  std::function<void(const std::string &)>  m_on_bytes;
  std::function<void()>                     m_on_disconnect;
};

#endif // ACE_SSL_TRANSPORT_HPP
