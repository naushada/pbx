#ifndef ARI_REST_CLIENT_HPP
#define ARI_REST_CLIENT_HPP

#include "ari_client.hpp"
#include "ace/SOCK_Stream.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * @file ari_rest_client.hpp
 * @brief Plain HTTP/1.1 client for Asterisk's `/ari/...` REST surface.
 *
 * Implements `IAriRest` for the endpoints `AriClient` / `CallRouter`
 * call:
 *
 *   POST   /ari/applications/{app}/subscription
 *          ?eventSource=channel:&eventSource=bridge:
 *   POST   /ari/channels/{channel_id}/continue
 *          ?context=...&extension=...&priority=...
 *   POST   /ari/channels
 *          ?endpoint=...&app=...&appArgs=...&channelId=...&callerId=...
 *   POST   /ari/bridges?type=...&bridgeId=...
 *   POST   /ari/bridges/{bridge_id}/addChannel?channel=...
 *   DELETE /ari/channels/{channel_id}?reason=...
 *
 * All go to Asterisk's loopback ARI port (8088). Plain HTTP, not
 * HTTPS — that port is bound to `127.0.0.1` only and isn't reachable
 * off-host. Auth is HTTP Basic in the `Authorization:` header (the
 * password DOES NOT belong in the URL — Asterisk logs full URLs to its
 * access log).
 *
 * Synchronous: every call opens a fresh `ACE_SOCK_Stream`, sends one
 * request, reads the response, closes. We don't pool — these methods
 * fire only at known low-rate points (subscribe at startup, the rest
 * per call setup/teardown), so connection setup is cheaper than the
 * complexity of pooling.
 */

class AriRestClient : public IAriRest {
public:
  struct Config {
    std::string   host     = "127.0.0.1";
    std::uint16_t port     = 8088;
    std::string   username = "asterisk";
    std::string   password = "asterisk";
  };

  explicit AriRestClient(Config cfg);
  ~AriRestClient() override = default;

  // ── IAriRest ─────────────────────────────────────────────────────────

  Response subscribe(const std::string &app_name,
                      const std::vector<std::string> &event_sources) override;
  Response continue_in_dialplan(const std::string &channel_id,
                                 const std::string &context,
                                 const std::string &extension,
                                 int priority) override;
  Response originate(const std::string &endpoint, const std::string &app,
                      const std::string &app_args,
                      const std::string &channel_id,
                      const std::string &caller_id) override;
  Response create_bridge(const std::string &bridge_id,
                          const std::string &type) override;
  Response add_channel_to_bridge(const std::string &bridge_id,
                                  const std::string &channel_id) override;
  Response hangup(const std::string &channel_id,
                   const std::string &reason) override;
  Response get_endpoint(const std::string &tech,
                         const std::string &resource) override;

  // ── Pure-logic helpers (public for unit tests) ───────────────────────

  /// Build the raw HTTP/1.1 request bytes for the subscribe endpoint.
  /// @p basic_auth is the base64-encoded `user:pass` string (NOT the
  /// `Basic ` prefix — that's added inside).
  static std::string build_subscribe_request(
      const std::string &app, const std::vector<std::string> &sources,
      const std::string &host, const std::string &basic_auth);

  static std::string build_continue_request(
      const std::string &channel_id, const std::string &context,
      const std::string &extension, int priority,
      const std::string &host, const std::string &basic_auth);

  /// `callerId` is omitted from the query when @p caller_id is empty —
  /// Asterisk then originates with the endpoint's default CallerID.
  static std::string build_originate_request(
      const std::string &endpoint, const std::string &app,
      const std::string &app_args, const std::string &channel_id,
      const std::string &caller_id, const std::string &host,
      const std::string &basic_auth);

  static std::string build_create_bridge_request(
      const std::string &bridge_id, const std::string &type,
      const std::string &host, const std::string &basic_auth);

  static std::string build_add_channel_request(
      const std::string &bridge_id, const std::string &channel_id,
      const std::string &host, const std::string &basic_auth);

  /// A `DELETE`, not a `POST` — the verb is baked into the request line.
  static std::string build_hangup_request(
      const std::string &channel_id, const std::string &reason,
      const std::string &host, const std::string &basic_auth);

  /// A `GET` — `/ari/endpoints/{tech}/{resource}`. Both path components
  /// are percent-encoded (`PJSIP` + a bare sipUsername in practice).
  static std::string build_get_endpoint_request(
      const std::string &tech, const std::string &resource,
      const std::string &host, const std::string &basic_auth);

  /// Parse an HTTP/1.1 response. Returns {status, body}. status == 0
  /// if the input isn't a well-formed response (no status line, no
  /// header separator, etc.).
  static IAriRest::Response parse_response(const std::string &raw);

  /// Percent-encode any char outside RFC 3986 unreserved. Used for
  /// path components like `channel_id` and query-string values.
  static std::string url_encode(const std::string &s);

  /// Standard base64 (RFC 4648 §4). Re-implemented here so this module
  /// doesn't depend on `AriWsClient`'s identical helper.
  static std::string base64_encode(const std::string &in);

private:
  /// Open a TCP connection, send the raw request bytes, read the
  /// response until the socket closes or Content-Length bytes have
  /// been received. Returns {0, ""} on any I/O failure. Verb-agnostic —
  /// the request line (POST / DELETE / …) is already in @p raw_request.
  Response do_request(const std::string &raw_request);

  Config m_cfg;
};

#endif // ARI_REST_CLIENT_HPP
