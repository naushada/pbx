#ifndef PUSH_SENDER_HPP
#define PUSH_SENDER_HPP

#include "iclock.hpp"
#include "mongodbc.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/**
 * @file push_sender.hpp
 * @brief Cloud-side VAPID Web Push trigger.
 *
 * Receives `PUSH_NOTIFY` frames from the agent (via [`SipBridge`](sip_bridge.hpp)),
 * fans out to every Web Push subscription in `push_subscriptions` for the
 * target subscriber, and POSTs a signed-and-encrypted notification to each
 * browser's push endpoint.
 *
 *   VAPID JWT      : RFC 8292   (ECDSA P-256, base64url, ≤ 12 h exp)
 *   Payload crypto : RFC 8291   (ephemeral ECDH P-256 + HKDF-SHA256
 *                                + AES-128-GCM, framed per RFC 8188)
 *
 * I/O is dependency-injected — the class only contains pure crypto and
 * policy. Production wires `AceHttpsClient` (lands with SipBridge's
 * reactor binding); tests use `FakePushHttpClient`.
 */

/// Outbound HTTP/1.1 POST adapter for the Web Push endpoint.
/// Production: ACE_SSL_SOCK_Connector wrapper. Tests: in-memory fake.
class IPushHttpClient {
public:
  virtual ~IPushHttpClient() = default;

  struct Response {
    int         status = 0;        // HTTP status code, 0 if request never sent
    std::string body;
  };

  /// POST `body` to `url` with `headers`. Synchronous; called from a worker
  /// thread. The implementation is responsible for TLS and for the
  /// `Authorization` / `Content-Encoding: aes128gcm` headers being attached.
  virtual Response post(const std::string &url,
                        const std::vector<std::pair<std::string, std::string>> &headers,
                        const std::string &body) = 0;
};

// IClock now lives in the shared `iclock.hpp` (also used by
// CloudTunnelEndpoint). Same interface, just moved out — see that
// header for the rationale.

class PushSender {
public:
  struct Config {
    /// VAPID server identity. PEM-encoded ECDSA P-256 private key. Public
    /// key is derived from it and base64url-encoded into the `k=` token of
    /// the Authorization header on every request.
    std::string vapid_private_pem;

    /// VAPID `sub` claim — `mailto:` URI for the operator (RFC 8292 §2.2).
    std::string vapid_subject;

    /// Cap on JWT exp (now + this many seconds). RFC 8292 mandates ≤ 24 h;
    /// we use 12 h to stay safely below the cap and to minimise the
    /// usefulness of a leaked JWT.
    std::int64_t jwt_exp_seconds = 12 * 60 * 60;

    /// Retry policy.
    int max_retries        = 3;       ///< number of attempts after the first failure
    int initial_backoff_ms = 100;     ///< doubles each retry; 0 disables in tests
  };

  PushSender(Config cfg, IPushHttpClient &http, IMongodbClient &db, IClock &clock);

  /// Send a notification to every push subscription registered for
  /// @p subscriber_id. Returns the number of endpoints to which delivery
  /// SUCCEEDED (HTTP 200/201/204). Subscriptions returning 410 Gone are
  /// removed from Mongo. 503 Service Unavailable triggers exponential
  /// backoff up to `Config::max_retries`.
  int notify(const std::string &subscriber_id, const std::string &payload);

  // ── Crypto building blocks (exposed for unit tests) ──────────────────────

  /// Build the VAPID `Authorization: vapid t=<jwt>, k=<pub>` header value
  /// for @p endpoint_origin (e.g. "https://push.example") at @p now_unix.
  /// Returns the whole header value (without the leading "Authorization:").
  std::string build_vapid_auth(const std::string &endpoint_origin,
                               std::int64_t now_unix) const;

  /// Sign a VAPID JWT for @p endpoint_origin at @p now_unix. Returns
  /// `header.payload.signature` (compact JWS). Public surface for tests.
  std::string sign_vapid_jwt(const std::string &endpoint_origin,
                             std::int64_t now_unix) const;

  /// Encrypt @p payload per RFC 8291 / 8188 (aes128gcm content encoding).
  /// @param p256dh_b64url Subscriber's P-256 public key (uncompressed,
  ///                      base64url-encoded — what the browser ships in
  ///                      `PushSubscription.getKey('p256dh')`).
  /// @param auth_b64url   16-byte subscriber auth secret, base64url.
  /// @return Wire-ready binary blob: salt(16) | rs(4) | idlen(1)
  ///         | keyid(=server-pub, 65) | ciphertext.
  std::string encrypt_payload(const std::string &payload,
                              const std::string &p256dh_b64url,
                              const std::string &auth_b64url) const;

  /// VAPID public key (uncompressed, base64url-encoded). Stable across the
  /// lifetime of this `PushSender`.
  const std::string &vapid_public_b64url() const { return m_vapid_pub_b64; }

private:
  struct Impl;
  std::shared_ptr<Impl> m_impl;
  std::string m_vapid_pub_b64;

  Config m_cfg;
  IPushHttpClient &m_http;
  IMongodbClient  &m_db;
  IClock          &m_clock;
};

// ─────────────────────────────────────────────────────────────────────────────
// Standalone primitives — public for tests' decryption round-trip.
// All inputs/outputs are raw bytes unless suffixed `_b64url`.
// ─────────────────────────────────────────────────────────────────────────────

namespace push_crypto {

/// base64url encode (no padding, '-'/'_' alphabet).
std::string b64url_encode(const std::string &raw);

/// base64url decode. Tolerates padding-less input. Throws on bad chars.
std::string b64url_decode(const std::string &in);

/// Generate a fresh ECDSA / ECDH P-256 keypair. The returned private key
/// is PEM-encoded; the public key is the uncompressed SEC1 form (65 bytes,
/// `0x04 || X || Y`).
struct P256KeyPair {
  std::string private_pem;
  std::string public_uncompressed; // 65 bytes
};
P256KeyPair p256_generate();

/// Derive the SEC1-uncompressed (65 byte) public key from a PEM private key.
std::string p256_public_from_pem(const std::string &private_pem);

/// Cryptographically-secure random bytes.
std::string rand_bytes(std::size_t n);

/// Decrypt a Web Push aes128gcm record sent by `encrypt_payload`. Test
/// helper — production never decrypts. Throws on shape / tag failure.
/// @param private_pem  Subscriber-side P-256 private key (PEM).
/// @param auth         16-byte subscriber auth secret (raw).
/// @param record       Wire blob (salt | rs | idlen | keyid | ciphertext).
std::string decrypt_payload_for_testing(const std::string &private_pem,
                                         const std::string &auth,
                                         const std::string &record);

} // namespace push_crypto

#endif // PUSH_SENDER_HPP
