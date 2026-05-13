#include "innertls.hpp"

#include <cstdio>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace {
std::string ssl_error_string()
{
  unsigned long e = ERR_get_error();
  if (e == 0) return "unknown (empty error queue)";
  return ERR_error_string(e, nullptr);
}
} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// InnerTlsClient
// ═══════════════════════════════════════════════════════════════════════════════

InnerTlsClient::InnerTlsClient(ITransport &transport)
  : m_transport(transport),
    m_ctx(SSL_CTX_new(TLS_client_method())),
    m_ssl(SSL_new(m_ctx.get()))
{
    SSL_CTX_set_min_proto_version(m_ctx.get(), TLS1_2_VERSION);

    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(m_ssl.get(), m_rbio, m_wbio);
    SSL_set_connect_state(m_ssl.get());
}

InnerTlsClient::~InnerTlsClient() = default;

void InnerTlsClient::set_ca(const std::string &ca_path)
{
    if (ca_path.empty()) return;
    SSL_CTX_load_verify_locations(m_ctx.get(), ca_path.c_str(), nullptr);
    SSL_CTX_set_verify(m_ctx.get(), SSL_VERIFY_PEER, nullptr);
}

bool InnerTlsClient::handshake()
{
    for (;;) {
        int ret = SSL_connect(m_ssl.get());
        if (ret == 1) {
            flush_wbio();
            return true;
        }

        int err = SSL_get_error(m_ssl.get(), ret);

        // Flush any pending output before reading more input —
        // SSL_connect may have written to wbio (e.g. ClientHello)
        // before returning WANT_READ.
        {
            int pending = BIO_pending(m_wbio);
            if (pending > 0) {
                std::vector<std::uint8_t> out(pending);
                int n = BIO_read(m_wbio, out.data(), pending);
                if (n > 0) {
                    out.resize(n);
                    if (!m_transport.send(out)) return false;
                }
            }
        }

        if (err == SSL_ERROR_WANT_READ) {
            std::vector<std::uint8_t> buf;
            if (!m_transport.recv(buf)) return false;
            if (!buf.empty())
                BIO_write(m_rbio, buf.data(), static_cast<int>(buf.size()));
            if (buf.empty() && BIO_pending(m_wbio) == 0) return false;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            // wbio flushed above; loop
        } else {
            std::string err = ssl_error_string();
            std::fprintf(stderr, "[InnerTlsClient] handshake SSL error: %s\n", err.c_str());
            return false;
        }
    }
}

bool InnerTlsClient::set_cert(const std::string &cert_path, const std::string &key_path)
{
    if (SSL_CTX_use_certificate_file(m_ctx.get(), cert_path.c_str(), SSL_FILETYPE_PEM) != 1)
        return false;
    if (SSL_CTX_use_PrivateKey_file(m_ctx.get(), key_path.c_str(), SSL_FILETYPE_PEM) != 1)
        return false;
    return SSL_CTX_check_private_key(m_ctx.get()) == 1;
}

bool InnerTlsClient::verify_hostname(const std::string &hostname)
{
    X509 *cert = SSL_get_peer_certificate(m_ssl.get());
    if (!cert) return false;

    int ok = X509_check_host(cert, hostname.c_str(), hostname.size(),
                              X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr);
    X509_free(cert);
    return ok == 1;
}

bool InnerTlsClient::send(const std::vector<std::uint8_t> &plaintext)
{
    int ret = SSL_write(m_ssl.get(), plaintext.data(), static_cast<int>(plaintext.size()));
    if (ret <= 0) {
        ERR_clear_error();
        return false;
    }
    return flush_wbio();
}

bool InnerTlsClient::recv(std::vector<std::uint8_t> &plaintext)
{
    std::vector<std::uint8_t> wire;
    if (!m_transport.recv(wire)) return false;
    if (!wire.empty())
        BIO_write(m_rbio, wire.data(), static_cast<int>(wire.size()));

    // SSL_read returns at most one TLS record (~16 KB) per call. The peer's
    // single SSL_write may produce several records, all delivered in one
    // transport frame, so loop until WANT_READ to drain every decrypted
    // record. Without this, large responses are silently truncated and the
    // caller parses an incomplete BSON document.
    plaintext.clear();
    std::uint8_t buf[16384];
    for (;;) {
        int ret = SSL_read(m_ssl.get(), buf, sizeof(buf));
        if (ret > 0) {
            plaintext.insert(plaintext.end(), buf, buf + ret);
            continue;
        }
        int err = SSL_get_error(m_ssl.get(), ret);
        if (err == SSL_ERROR_WANT_READ) return true;
        ERR_clear_error();
        return false;
    }
}

bool InnerTlsClient::flush_wbio()
{
    int pending = BIO_pending(m_wbio);
    if (pending <= 0) return true;

    std::vector<std::uint8_t> buf(pending);
    int n = BIO_read(m_wbio, buf.data(), pending);
    if (n <= 0) return false;
    buf.resize(n);
    return m_transport.send(buf);
}

// ═══════════════════════════════════════════════════════════════════════════════
// InnerTlsServer
// ═══════════════════════════════════════════════════════════════════════════════

InnerTlsServer::InnerTlsServer(ITransport &transport,
                               const std::string &cert_path,
                               const std::string &key_path,
                               const std::string &ca_path)
  : m_transport(transport),
    m_ctx(SSL_CTX_new(TLS_server_method())),
    m_ssl(nullptr)   // created after cert/key config
{
    SSL_CTX_set_min_proto_version(m_ctx.get(), TLS1_2_VERSION);

    SSL_CTX_use_certificate_file(m_ctx.get(), cert_path.c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(m_ctx.get(), key_path.c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_check_private_key(m_ctx.get());

    if (!ca_path.empty()) {
        if (SSL_CTX_load_verify_locations(m_ctx.get(), ca_path.c_str(), nullptr) != 1) {
            std::fprintf(stderr, "[InnerTlsServer] failed to load CA: %s\n",
                         ca_path.c_str());
        }
        // Explicitly load the CA cert into the client CA list — without this
        // OpenSSL may send an empty CA list in the CertificateRequest and the
        // client will not present its certificate.
        BIO *ca_bio = BIO_new_file(ca_path.c_str(), "r");
        if (ca_bio) {
            X509 *ca = PEM_read_bio_X509(ca_bio, nullptr, nullptr, nullptr);
            if (ca) {
                SSL_CTX_add_client_CA(m_ctx.get(), ca);
                X509_free(ca);
            }
            BIO_free(ca_bio);
        }
        // SSL_VERIFY_PEER verifies the client cert if one is presented.
        // We don't use SSL_VERIFY_FAIL_IF_NO_PEER_CERT — the agent's
        // identity is already authenticated by the outer Heroku TLS +
        // WebSocket upgrade; inner TLS provides encryption + server auth.
        SSL_CTX_set_verify(m_ctx.get(), SSL_VERIFY_PEER, nullptr);
    }

    m_ssl.reset(SSL_new(m_ctx.get()));
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(m_ssl.get(), m_rbio, m_wbio);
    SSL_set_accept_state(m_ssl.get());
}

InnerTlsServer::~InnerTlsServer() = default;

bool InnerTlsServer::accept()
{
    for (;;) {
        int ret = SSL_accept(m_ssl.get());
        if (ret == 1) {
            flush_wbio();
            return true;
        }

        int err = SSL_get_error(m_ssl.get(), ret);

        // Flush any pending output before reading more.
        {
            int pending = BIO_pending(m_wbio);
            if (pending > 0) {
                std::vector<std::uint8_t> out(pending);
                int n = BIO_read(m_wbio, out.data(), pending);
                if (n > 0) {
                    out.resize(n);
                    if (!m_transport.send(out)) return false;
                }
            }
        }

        if (err == SSL_ERROR_WANT_READ) {
            std::vector<std::uint8_t> buf;
            if (!m_transport.recv(buf)) return false;
            if (!buf.empty())
                BIO_write(m_rbio, buf.data(), static_cast<int>(buf.size()));
            if (buf.empty() && BIO_pending(m_wbio) == 0) return false;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            // wbio flushed above; loop
        } else {
            std::string err = ssl_error_string();
            std::fprintf(stderr, "[InnerTlsServer] accept SSL error: %s\n", err.c_str());
            return false;
        }
    }
}

bool InnerTlsServer::send(const std::vector<std::uint8_t> &plaintext)
{
    int ret = SSL_write(m_ssl.get(), plaintext.data(), static_cast<int>(plaintext.size()));
    if (ret <= 0) {
        ERR_clear_error();
        return false;
    }
    return flush_wbio();
}

bool InnerTlsServer::recv(std::vector<std::uint8_t> &plaintext)
{
    std::vector<std::uint8_t> wire;
    if (!m_transport.recv(wire)) return false;
    if (!wire.empty())
        BIO_write(m_rbio, wire.data(), static_cast<int>(wire.size()));

    // See InnerTlsClient::recv — loop SSL_read until WANT_READ so a single
    // call returns the full plaintext that one peer SSL_write produced.
    plaintext.clear();
    std::uint8_t buf[16384];
    for (;;) {
        int ret = SSL_read(m_ssl.get(), buf, sizeof(buf));
        if (ret > 0) {
            plaintext.insert(plaintext.end(), buf, buf + ret);
            continue;
        }
        int err = SSL_get_error(m_ssl.get(), ret);
        if (err == SSL_ERROR_WANT_READ) return true;
        ERR_clear_error();
        return false;
    }
}

bool InnerTlsServer::flush_wbio()
{
    int pending = BIO_pending(m_wbio);
    if (pending <= 0) return true;

    std::vector<std::uint8_t> buf(pending);
    int n = BIO_read(m_wbio, buf.data(), pending);
    if (n <= 0) return false;
    buf.resize(n);
    return m_transport.send(buf);
}
