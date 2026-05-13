#ifndef ACE_HTTPS_CLIENT_HPP
#define ACE_HTTPS_CLIENT_HPP

#include "push_sender.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * @file ace_https_client.hpp
 * @brief One-shot HTTPS POST client backed by `ACE_SSL_SOCK_Stream`.
 *
 * Implements `IPushHttpClient` for `PushSender`. Each `post()` opens a
 * fresh TLS connection to the URL's host:port, sends a single HTTP/1.1
 * POST with `Connection: close`, drains the response, and returns
 * `{status, body}`. No connection pooling — Web Push endpoints don't
 * stay sticky, and the request rate is low (one POST per
 * `PUSH_NOTIFY` per subscriber endpoint).
 *
 * TLS uses the system trust store (no client cert, no custom CA). The
 * public push services (`updates.push.services.mozilla.com`,
 * `fcm.googleapis.com`, etc.) all have publicly-trusted certificates,
 * so this is the right default. For private push proxies, swap in a
 * different `IPushHttpClient`.
 */

class AceHttpsClient : public IPushHttpClient {
public:
  AceHttpsClient() = default;
  ~AceHttpsClient() override = default;

  /// One-shot POST. Returns `{0, ""}` on any I/O failure.
  Response post(const std::string &url,
                const std::vector<std::pair<std::string, std::string>> &headers,
                const std::string &body) override;

  // ── Pure-logic helpers (public for unit tests) ───────────────────────

  struct ParsedUrl {
    std::string   scheme;   // "https"
    std::string   host;     // "updates.push.services.mozilla.com"
    std::uint16_t port = 443;
    std::string   path_and_query; // "/wpush/v2/..." with `?` and `#` preserved
    bool          ok = false;
  };

  /// Parse @p url into its parts. Supports `https://host[:port]/path[?q]`.
  /// `ok=false` on anything else.
  static ParsedUrl parse_url(const std::string &url);

  /// Build the raw HTTP/1.1 request bytes for a POST to @p path with the
  /// supplied headers and body. Adds `Host:`, `Content-Length:`, and
  /// `Connection: close` automatically — the caller's `headers` list
  /// should NOT include those (Host duplicates are a server-side
  /// canonicalisation hazard).
  static std::string build_request(
      const std::string &host, const std::string &path,
      const std::vector<std::pair<std::string, std::string>> &headers,
      const std::string &body);

  /// Parse an HTTP/1.x response. Returns `{0, ""}` on garbage input.
  /// Body extraction stops at the supplied buffer end (Content-Length
  /// is informational only — we already drained to EOF).
  static Response parse_response(const std::string &raw);
};

#endif // ACE_HTTPS_CLIENT_HPP
