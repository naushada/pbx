#ifndef ARI_WS_CLIENT_HPP
#define ARI_WS_CLIENT_HPP

#include "ace/Event_Handler.h"
#include "ace/SOCK_Stream.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class ACE_Reactor;

/**
 * @file ari_ws_client.hpp
 * @brief Plain-TCP WebSocket client that consumes Asterisk's
 *        `/ari/events` push stream.
 *
 * Asterisk's ARI exposes events at `ws://<host>:8088/ari/events`. The
 * agent dials this on loopback only — there's no TLS in front of the
 * Asterisk ARI port, which is fine because we're co-located on the
 * same on-prem host. HTTP Basic credentials are sent in the upgrade
 * request (we prefer the header over the `?api_key=` query param so
 * the password doesn't end up in webserver logs).
 *
 * Each inbound text frame is one JSON event ready for
 * `AriClient::on_event(payload)`. The client owns no JSON state — it
 * just decodes WS frames and forwards.
 *
 * Lifetime / reactor binding:
 *   - Production: `connect_and_handshake()` then
 *     `register_with_reactor(reactor)`.
 *   - Tests: the `AriWsClient(ACE_HANDLE, …)` constructor takes a
 *     pre-connected fd (typically from `socketpair`) and skips both
 *     the connect AND the reactor registration; the test drives
 *     `handle_input` directly.
 *
 * Reconnect on Asterisk restart is the caller's concern (a tiny
 * supervisor loop in `pbx-agent/main`). Keeping it outside this class
 * keeps the surface small and testable.
 */

class AriWsClient : public ACE_Event_Handler {
public:
  struct Config {
    std::string   host     = "127.0.0.1";
    std::uint16_t port     = 8088;
    std::string   app_name = "pbx";
    std::string   username = "asterisk";
    std::string   password = "asterisk";
  };

  /// Production ctor — must call `connect_and_handshake()` afterward.
  AriWsClient(Config cfg,
              std::function<void(const std::string &)> on_event,
              std::function<void()>                    on_disconnect = {});

  /// Test ctor — takes a pre-connected fd. Skips the network bring-up
  /// and the reactor registration; tests drive `handle_input` directly.
  AriWsClient(ACE_HANDLE fd,
              std::function<void(const std::string &)> on_event,
              std::function<void()>                    on_disconnect = {});

  ~AriWsClient() override;

  AriWsClient(const AriWsClient &) = delete;
  AriWsClient &operator=(const AriWsClient &) = delete;

  /// Plain-TCP connect + HTTP/1.1 WS upgrade. Returns true on full
  /// success (101 + matching `Sec-WebSocket-Accept`).
  bool connect_and_handshake();

  /// Register with @p reactor for `READ_MASK`.
  int register_with_reactor(ACE_Reactor *reactor);

  // ── ACE_Event_Handler ──────────────────────────────────────────────────

  int        handle_input(ACE_HANDLE)                   override;
  int        handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;
  ACE_HANDLE get_handle() const                         override {
    return m_handle;
  }

  /// Close the WS cleanly (best-effort close frame, then socket close).
  void close();

  bool connected() const { return m_handle != ACE_INVALID_HANDLE; }

  // ── Pure-logic helpers (public for unit tests) ─────────────────────────

  /// Build the HTTP/1.1 GET request that initiates the WS upgrade
  /// against Asterisk's ARI events endpoint. Returns
  /// `(request_bytes, sec_websocket_key)`. The caller validates the
  /// 101 response via `validate_upgrade_response()`.
  static std::pair<std::string, std::string>
  build_upgrade_request(const std::string &host, const std::string &app,
                         const std::string &username,
                         const std::string &password);

  /// Same shape as `AceSslTransport::validate_upgrade_response` — 101
  /// + matching `Sec-WebSocket-Accept` (computed via
  /// `wsframe::accept_key`).
  static bool validate_upgrade_response(const std::string &headers,
                                         const std::string &sec_websocket_key);

  /// Encode @p in as standard base64 (RFC 4648 §4; `+`/`/` alphabet,
  /// `=` padding). Exposed for tests; production uses it for the
  /// `Authorization: Basic` header.
  static std::string base64_encode(const std::string &in);

private:
  bool drain_frames();
  void notify_disconnect_once();

  Config                                   m_cfg;
  std::function<void(const std::string &)> m_on_event;
  std::function<void()>                    m_on_disconnect;
  ACE_SOCK_Stream                          m_stream;
  ACE_HANDLE                               m_handle;
  std::vector<std::uint8_t>                m_recv_buf;
  bool                                     m_close_notified = false;
};

#endif // ARI_WS_CLIENT_HPP
