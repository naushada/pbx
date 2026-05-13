#ifndef INNERTLS_HPP
#define INNERTLS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>

// Custom deleters for OpenSSL types — zero-overhead unique_ptr integration.
namespace detail {
struct SslCtxDeleter { void operator()(SSL_CTX *p) const noexcept { SSL_CTX_free(p); } };
struct SslDeleter    { void operator()(SSL *p)    const noexcept { SSL_free(p); } };
} // namespace detail

using SslCtxPtr = std::unique_ptr<SSL_CTX, detail::SslCtxDeleter>;
using SslPtr    = std::unique_ptr<SSL,    detail::SslDeleter>;

/**
 * @brief Abstract transport for inner-TLS I/O.
 *
 * Decouples the TLS layer from the underlying WebSocket frame send/recv so
 * InnerTlsClient/Server can be tested with a MockTransport.
 */
class ITransport {
public:
  virtual ~ITransport() = default;
  virtual bool send(const std::vector<std::uint8_t> &data) = 0;
  virtual bool recv(std::vector<std::uint8_t> &data) = 0;
};

/**
 * @brief Inner-TLS client (wsdbagent side).
 *
 * After WebSocket upgrade, creates a TLS session over the transport using
 * OpenSSL memory BIOs.  Application data written via send() is encrypted,
 * sent through the transport, decrypted by the peer, and returned by recv().
 */
class InnerTlsClient {
public:
  InnerTlsClient(ITransport &transport);
  ~InnerTlsClient();

  InnerTlsClient(const InnerTlsClient &) = delete;
  InnerTlsClient &operator=(const InnerTlsClient &) = delete;

  /// Perform TLS handshake as client. Returns true on success.
  bool handshake();

  /// Send plaintext through the encrypted tunnel.
  bool send(const std::vector<std::uint8_t> &plaintext);

  /// Receive plaintext from the encrypted tunnel.
  bool recv(std::vector<std::uint8_t> &plaintext);

  /// Enable certificate verification (CA path for server cert).
  void set_ca(const std::string &ca_path);

  /// Load client certificate and key for mTLS (presented to server).
  bool set_cert(const std::string &cert_path, const std::string &key_path);

  /// Verify the server certificate's CN/SAN matches @p hostname.
  /// Must be called after a successful handshake.
  bool verify_hostname(const std::string &hostname);

private:
  ITransport &m_transport;
  SslCtxPtr   m_ctx;
  SslPtr      m_ssl;
  BIO        *m_rbio = nullptr;  // owned by m_ssl after SSL_set_bio
  BIO        *m_wbio = nullptr;  // owned by m_ssl after SSL_set_bio

  bool flush_wbio();
};

/**
 * @brief Inner-TLS server (wsdbproxy / WsDbServer side).
 *
 * Accepts an inner-TLS connection from the agent over the transport.  Requires
 * a certificate + private key.  Optionally requires client certificate (mTLS).
 */
class InnerTlsServer {
public:
  /**
   * @param transport   WebSocket transport.
   * @param cert_path   PEM-encoded server certificate file path.
   * @param key_path    PEM-encoded server private key file path.
   * @param ca_path     Optional CA cert for verifying client certificates.
   */
  InnerTlsServer(ITransport &transport, const std::string &cert_path,
                 const std::string &key_path,
                 const std::string &ca_path = "");
  ~InnerTlsServer();

  InnerTlsServer(const InnerTlsServer &) = delete;
  InnerTlsServer &operator=(const InnerTlsServer &) = delete;

  /// Accept TLS handshake from client. Returns true on success.
  bool accept();

  /// Send plaintext through the encrypted tunnel.
  bool send(const std::vector<std::uint8_t> &plaintext);

  /// Receive plaintext from the encrypted tunnel.
  bool recv(std::vector<std::uint8_t> &plaintext);

private:
  ITransport &m_transport;
  SslCtxPtr   m_ctx;
  SslPtr      m_ssl;
  BIO        *m_rbio = nullptr;  // owned by m_ssl after SSL_set_bio
  BIO        *m_wbio = nullptr;  // owned by m_ssl after SSL_set_bio

  bool flush_wbio();
};

#endif // INNERTLS_HPP
