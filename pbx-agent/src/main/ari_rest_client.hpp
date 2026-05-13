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
 * Implements `IAriRest` for the two endpoints `AriClient` calls:
 *
 *   POST /ari/applications/{app}/subscription
 *        ?eventSource=channel:&eventSource=bridge:
 *
 *   POST /ari/channels/{channel_id}/continue
 *        ?context=...&extension=...&priority=...
 *
 * Both go to Asterisk's loopback ARI port (8088). Plain HTTP, not
 * HTTPS — that port is bound to `127.0.0.1` only and isn't reachable
 * off-host. Auth is HTTP Basic in the `Authorization:` header (the
 * password DOES NOT belong in the URL — Asterisk logs full URLs to its
 * access log).
 *
 * Synchronous: every call opens a fresh `ACE_SOCK_Stream`, sends one
 * POST, reads the response, closes. We don't pool — both methods fire
 * only at known low-rate points (subscribe at startup, continue on
 * admission denial), so connection setup is cheaper than the
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
  /// been received. Returns {0, ""} on any I/O failure.
  Response do_post(const std::string &raw_request);

  Config m_cfg;
};

#endif // ARI_REST_CLIENT_HPP
