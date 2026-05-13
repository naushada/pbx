#include "push_sender.hpp"

#include "json.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════════
// push_crypto — primitives shared by encryption + decryption (test helper)
// ═══════════════════════════════════════════════════════════════════════════════

namespace push_crypto {

namespace {

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

// HKDF-SHA256 via OpenSSL EVP. info may be empty.
std::string hkdf_sha256(const std::string &salt, const std::string &ikm,
                        const std::string &info, std::size_t length) {
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!pctx) throw std::runtime_error("HKDF: ctx new failed");

  if (EVP_PKEY_derive_init(pctx) <= 0 ||
      EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(pctx,
                                   reinterpret_cast<const unsigned char *>(salt.data()),
                                   static_cast<int>(salt.size())) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(pctx,
                                   reinterpret_cast<const unsigned char *>(ikm.data()),
                                   static_cast<int>(ikm.size())) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(pctx,
                                    reinterpret_cast<const unsigned char *>(info.data()),
                                    static_cast<int>(info.size())) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    throw std::runtime_error("HKDF: setup failed");
  }

  std::string out(length, '\0');
  std::size_t out_len = length;
  if (EVP_PKEY_derive(pctx,
                       reinterpret_cast<unsigned char *>(&out[0]),
                       &out_len) <= 0) {
    EVP_PKEY_CTX_free(pctx);
    throw std::runtime_error("HKDF: derive failed");
  }
  EVP_PKEY_CTX_free(pctx);
  out.resize(out_len);
  return out;
}

// AES-128-GCM encrypt. Returns ciphertext || 16-byte tag.
std::string aes128gcm_encrypt(const std::string &key, const std::string &nonce,
                              const std::string &plaintext) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("AES-GCM: ctx new failed");

  if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr,
                          reinterpret_cast<const unsigned char *>(key.data()),
                          reinterpret_cast<const unsigned char *>(nonce.data())) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM: init failed");
  }

  std::string out(plaintext.size() + 16, '\0'); // room for tag
  int outl = 0;
  if (EVP_EncryptUpdate(ctx,
                         reinterpret_cast<unsigned char *>(&out[0]), &outl,
                         reinterpret_cast<const unsigned char *>(plaintext.data()),
                         static_cast<int>(plaintext.size())) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM: update failed");
  }
  int final_len = 0;
  if (EVP_EncryptFinal_ex(ctx,
                           reinterpret_cast<unsigned char *>(&out[outl]),
                           &final_len) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM: final failed");
  }

  unsigned char tag[16];
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM: tag get failed");
  }
  EVP_CIPHER_CTX_free(ctx);

  out.resize(outl + final_len);
  out.append(reinterpret_cast<const char *>(tag), 16);
  return out;
}

std::string aes128gcm_decrypt(const std::string &key, const std::string &nonce,
                              const std::string &ct_with_tag) {
  if (ct_with_tag.size() < 16)
    throw std::runtime_error("AES-GCM: input shorter than tag");

  const std::string ciphertext = ct_with_tag.substr(0, ct_with_tag.size() - 16);
  const std::string tag = ct_with_tag.substr(ct_with_tag.size() - 16);

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("AES-GCM-d: ctx new failed");

  if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr,
                          reinterpret_cast<const unsigned char *>(key.data()),
                          reinterpret_cast<const unsigned char *>(nonce.data())) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM-d: init failed");
  }

  std::string out(ciphertext.size(), '\0');
  int outl = 0;
  if (EVP_DecryptUpdate(ctx,
                         reinterpret_cast<unsigned char *>(&out[0]), &outl,
                         reinterpret_cast<const unsigned char *>(ciphertext.data()),
                         static_cast<int>(ciphertext.size())) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM-d: update failed");
  }
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                           const_cast<char *>(tag.data())) != 1) {
    EVP_CIPHER_CTX_free(ctx);
    throw std::runtime_error("AES-GCM-d: tag set failed");
  }
  int final_len = 0;
  int rv = EVP_DecryptFinal_ex(ctx,
                                reinterpret_cast<unsigned char *>(&out[outl]),
                                &final_len);
  EVP_CIPHER_CTX_free(ctx);
  if (rv != 1) throw std::runtime_error("AES-GCM-d: tag verify failed");
  out.resize(outl + final_len);
  return out;
}

// ECDH P-256 shared secret. Both keys may be PEM (private) or 65-byte
// uncompressed SEC1 form (public).
std::string ecdh_p256(const std::string &our_private_pem,
                      const std::string &peer_pub_uncompressed) {
  // Load our private key.
  BIO *priv_bio = BIO_new_mem_buf(our_private_pem.data(),
                                    static_cast<int>(our_private_pem.size()));
  EVP_PKEY *our_pkey = PEM_read_bio_PrivateKey(priv_bio, nullptr, nullptr, nullptr);
  BIO_free(priv_bio);
  if (!our_pkey) throw std::runtime_error("ECDH: bad our private PEM");

  // Build peer EVP_PKEY from uncompressed public bytes.
  EC_KEY *peer_ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (!peer_ec) { EVP_PKEY_free(our_pkey); throw std::runtime_error("ECDH: peer EC new failed"); }
  if (EC_KEY_oct2key(peer_ec,
                      reinterpret_cast<const unsigned char *>(peer_pub_uncompressed.data()),
                      peer_pub_uncompressed.size(),
                      nullptr) != 1) {
    EC_KEY_free(peer_ec); EVP_PKEY_free(our_pkey);
    throw std::runtime_error("ECDH: peer pubkey load failed");
  }
  EVP_PKEY *peer_pkey = EVP_PKEY_new();
  EVP_PKEY_assign_EC_KEY(peer_pkey, peer_ec); // peer_pkey now owns peer_ec

  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(our_pkey, nullptr);
  std::string shared;
  if (!ctx || EVP_PKEY_derive_init(ctx) <= 0 ||
      EVP_PKEY_derive_set_peer(ctx, peer_pkey) <= 0) {
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer_pkey); EVP_PKEY_free(our_pkey);
    throw std::runtime_error("ECDH: derive init failed");
  }
  std::size_t slen = 0;
  EVP_PKEY_derive(ctx, nullptr, &slen);
  shared.resize(slen);
  if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(&shared[0]), &slen) <= 0) {
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(peer_pkey); EVP_PKEY_free(our_pkey);
    throw std::runtime_error("ECDH: derive failed");
  }
  shared.resize(slen);

  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(peer_pkey);
  EVP_PKEY_free(our_pkey);
  return shared;
}

// Compute u32 big-endian.
std::string u32_be(std::uint32_t v) {
  std::string s(4, '\0');
  s[0] = static_cast<char>((v >> 24) & 0xFF);
  s[1] = static_cast<char>((v >> 16) & 0xFF);
  s[2] = static_cast<char>((v >> 8)  & 0xFF);
  s[3] = static_cast<char>(v & 0xFF);
  return s;
}

} // namespace (anonymous)

std::string b64url_encode(const std::string &raw) {
  std::string out;
  out.reserve(((raw.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= raw.size()) {
    const auto b0 = static_cast<unsigned char>(raw[i]);
    const auto b1 = static_cast<unsigned char>(raw[i + 1]);
    const auto b2 = static_cast<unsigned char>(raw[i + 2]);
    out.push_back(kB64Alphabet[(b0 >> 2) & 0x3F]);
    out.push_back(kB64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
    out.push_back(kB64Alphabet[((b1 << 2) | (b2 >> 6)) & 0x3F]);
    out.push_back(kB64Alphabet[b2 & 0x3F]);
    i += 3;
  }
  if (i < raw.size()) {
    const auto b0 = static_cast<unsigned char>(raw[i]);
    out.push_back(kB64Alphabet[(b0 >> 2) & 0x3F]);
    if (i + 1 == raw.size()) {
      out.push_back(kB64Alphabet[(b0 << 4) & 0x3F]);
    } else {
      const auto b1 = static_cast<unsigned char>(raw[i + 1]);
      out.push_back(kB64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
      out.push_back(kB64Alphabet[(b1 << 2) & 0x3F]);
    }
  }
  return out;
}

std::string b64url_decode(const std::string &in) {
  std::string out;
  out.reserve(in.size() * 3 / 4);

  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    const int v = b64_value(static_cast<unsigned char>(c));
    if (v < 0) throw std::runtime_error("b64url_decode: bad char");
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

P256KeyPair p256_generate() {
  EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  EVP_PKEY_paramgen_init(pctx);
  EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);
  EVP_PKEY *params = nullptr;
  EVP_PKEY_paramgen(pctx, &params);

  EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new(params, nullptr);
  EVP_PKEY_keygen_init(kctx);
  EVP_PKEY *pkey = nullptr;
  EVP_PKEY_keygen(kctx, &pkey);

  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  BUF_MEM *bptr;
  BIO_get_mem_ptr(bio, &bptr);
  std::string priv_pem(bptr->data, bptr->length);
  BIO_free(bio);

  EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
  const EC_POINT *pt = EC_KEY_get0_public_key(ec);
  const EC_GROUP *grp = EC_KEY_get0_group(ec);
  std::string pub_uncompressed(65, '\0');
  EC_POINT_point2oct(grp, pt, POINT_CONVERSION_UNCOMPRESSED,
                      reinterpret_cast<unsigned char *>(&pub_uncompressed[0]), 65,
                      nullptr);

  EVP_PKEY_free(pkey);
  EVP_PKEY_free(params);
  EVP_PKEY_CTX_free(kctx);
  EVP_PKEY_CTX_free(pctx);

  return {std::move(priv_pem), std::move(pub_uncompressed)};
}

std::string p256_public_from_pem(const std::string &private_pem) {
  BIO *bio = BIO_new_mem_buf(private_pem.data(), static_cast<int>(private_pem.size()));
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!pkey) throw std::runtime_error("p256_public_from_pem: bad PEM");
  EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
  const EC_POINT *pt = EC_KEY_get0_public_key(ec);
  const EC_GROUP *grp = EC_KEY_get0_group(ec);
  std::string pub(65, '\0');
  EC_POINT_point2oct(grp, pt, POINT_CONVERSION_UNCOMPRESSED,
                      reinterpret_cast<unsigned char *>(&pub[0]), 65, nullptr);
  EVP_PKEY_free(pkey);
  return pub;
}

std::string rand_bytes(std::size_t n) {
  std::string out(n, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char *>(&out[0]),
                  static_cast<int>(n)) != 1)
    throw std::runtime_error("rand_bytes: RAND_bytes failed");
  return out;
}

std::string decrypt_payload_for_testing(const std::string &private_pem,
                                         const std::string &auth,
                                         const std::string &record) {
  // RFC 8188 framing: salt(16) | rs(4) | idlen(1) | keyid(idlen) | ciphertext
  if (record.size() < 21) throw std::runtime_error("record too short");
  const std::string salt = record.substr(0, 16);
  const std::uint8_t idlen = static_cast<std::uint8_t>(record[20]);
  if (record.size() < 21u + idlen) throw std::runtime_error("record header truncated");
  const std::string server_pub = record.substr(21, idlen);
  const std::string ciphertext = record.substr(21 + idlen);

  const std::string subscriber_pub = p256_public_from_pem(private_pem);

  // ECDH
  const std::string shared = ecdh_p256(private_pem, server_pub);

  // Key info per RFC 8291 §3.4: "WebPush: info" 0x00 ua_pub as_pub
  std::string key_info = "WebPush: info";
  key_info.push_back('\0');
  key_info += subscriber_pub;
  key_info += server_pub;

  // IKM = HKDF(salt=auth, IKM=shared, info=key_info, 32)
  const std::string ikm = hkdf_sha256(auth, shared, key_info, 32);

  // CEK info = "Content-Encoding: aes128gcm" 0x00
  std::string cek_info = "Content-Encoding: aes128gcm";
  cek_info.push_back('\0');
  const std::string cek = hkdf_sha256(salt, ikm, cek_info, 16);

  // NONCE info = "Content-Encoding: nonce" 0x00
  std::string nonce_info = "Content-Encoding: nonce";
  nonce_info.push_back('\0');
  const std::string nonce = hkdf_sha256(salt, ikm, nonce_info, 12);

  // Decrypt
  std::string padded = aes128gcm_decrypt(cek, nonce, ciphertext);
  // RFC 8188 §2: trailing 0x02 marker indicates last record.
  if (padded.empty() || padded.back() != 0x02)
    throw std::runtime_error("missing 0x02 record terminator");
  padded.pop_back();
  // Strip remaining 0x00 padding bytes (we use a single-record encoding).
  while (!padded.empty() && padded.back() == 0x00) padded.pop_back();
  return padded;
}

// Internal accessor for PushSender (skips the public namespace prefix elsewhere).
std::string encrypt_payload_impl(const std::string &payload,
                                  const std::string &p256dh_uncompressed,
                                  const std::string &auth,
                                  const std::string &server_priv_pem,
                                  const std::string &server_pub_uncompressed,
                                  const std::string &salt /* 16 bytes */) {
  // ECDH(server_priv, subscriber_p256dh)
  const std::string shared = ecdh_p256(server_priv_pem, p256dh_uncompressed);

  std::string key_info = "WebPush: info";
  key_info.push_back('\0');
  key_info += p256dh_uncompressed;
  key_info += server_pub_uncompressed;
  const std::string ikm = hkdf_sha256(auth, shared, key_info, 32);

  std::string cek_info = "Content-Encoding: aes128gcm";
  cek_info.push_back('\0');
  const std::string cek = hkdf_sha256(salt, ikm, cek_info, 16);

  std::string nonce_info = "Content-Encoding: nonce";
  nonce_info.push_back('\0');
  const std::string nonce = hkdf_sha256(salt, ikm, nonce_info, 12);

  // RFC 8188 §2: append 0x02 record terminator before encryption.
  std::string padded = payload;
  padded.push_back(0x02);
  const std::string ct = aes128gcm_encrypt(cek, nonce, padded);

  // RFC 8188 record header: salt(16) | rs(4 BE, 4096) | idlen(1) | keyid
  std::string record;
  record += salt;
  record += u32_be(4096);
  record.push_back(static_cast<char>(server_pub_uncompressed.size()));
  record += server_pub_uncompressed;
  record += ct;
  return record;
}

} // namespace push_crypto

// ═══════════════════════════════════════════════════════════════════════════════
// PushSender
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// JOSE-compact ECDSA signature: (r || s), each 32 bytes, big-endian.
// OpenSSL produces ASN.1 DER (SEQUENCE { INTEGER r, INTEGER s }); convert.
std::string ecdsa_der_to_jose(const std::string &der) {
  const unsigned char *p = reinterpret_cast<const unsigned char *>(der.data());
  ECDSA_SIG *sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
  if (!sig) throw std::runtime_error("ecdsa_der_to_jose: bad DER");

  const BIGNUM *r = nullptr;
  const BIGNUM *s = nullptr;
  ECDSA_SIG_get0(sig, &r, &s);

  std::string out(64, '\0');
  BN_bn2binpad(r, reinterpret_cast<unsigned char *>(&out[0]),  32);
  BN_bn2binpad(s, reinterpret_cast<unsigned char *>(&out[32]), 32);
  ECDSA_SIG_free(sig);
  return out;
}

std::string ecdsa_sign(const std::string &private_pem,
                       const std::string &data) {
  BIO *bio = BIO_new_mem_buf(private_pem.data(),
                              static_cast<int>(private_pem.size()));
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!pkey) throw std::runtime_error("ecdsa_sign: bad PEM");

  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0)
    throw std::runtime_error("ecdsa_sign: init failed");

  if (EVP_DigestSignUpdate(ctx, data.data(), data.size()) <= 0)
    throw std::runtime_error("ecdsa_sign: update failed");

  std::size_t sig_len = 0;
  EVP_DigestSignFinal(ctx, nullptr, &sig_len);
  std::string der(sig_len, '\0');
  if (EVP_DigestSignFinal(ctx,
                           reinterpret_cast<unsigned char *>(&der[0]),
                           &sig_len) <= 0)
    throw std::runtime_error("ecdsa_sign: final failed");
  der.resize(sig_len);

  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return ecdsa_der_to_jose(der);
}

// "https://push.example.com/x/y/z" -> "https://push.example.com"
std::string origin_of(const std::string &url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  const auto host_end = url.find('/', scheme + 3);
  return (host_end == std::string::npos) ? url : url.substr(0, host_end);
}

} // namespace

struct PushSender::Impl {}; // reserved for future state (none today)

PushSender::PushSender(Config cfg, IPushHttpClient &http,
                       IMongodbClient &db, IClock &clock)
    : m_impl(std::make_shared<Impl>()), m_cfg(std::move(cfg)),
      m_http(http), m_db(db), m_clock(clock) {
  if (m_cfg.vapid_private_pem.empty())
    throw std::runtime_error("PushSender: vapid_private_pem required");
  m_vapid_pub_b64 = push_crypto::b64url_encode(
      push_crypto::p256_public_from_pem(m_cfg.vapid_private_pem));
}

std::string PushSender::sign_vapid_jwt(const std::string &endpoint_origin,
                                        std::int64_t now_unix) const {
  // Header
  const std::string header = R"({"typ":"JWT","alg":"ES256"})";
  // Claims
  json claims;
  claims["aud"] = endpoint_origin;
  claims["exp"] = now_unix + m_cfg.jwt_exp_seconds;
  claims["sub"] = m_cfg.vapid_subject;
  const std::string claims_json = claims.dump();

  const std::string header_b64 = push_crypto::b64url_encode(header);
  const std::string claims_b64 = push_crypto::b64url_encode(claims_json);
  const std::string signing_input = header_b64 + "." + claims_b64;

  const std::string sig_jose = ecdsa_sign(m_cfg.vapid_private_pem, signing_input);
  const std::string sig_b64 = push_crypto::b64url_encode(sig_jose);

  return signing_input + "." + sig_b64;
}

std::string PushSender::build_vapid_auth(const std::string &endpoint_origin,
                                          std::int64_t now_unix) const {
  const std::string jwt = sign_vapid_jwt(endpoint_origin, now_unix);
  return "vapid t=" + jwt + ", k=" + m_vapid_pub_b64;
}

std::string PushSender::encrypt_payload(const std::string &payload,
                                         const std::string &p256dh_b64url,
                                         const std::string &auth_b64url) const {
  const std::string p256dh = push_crypto::b64url_decode(p256dh_b64url);
  const std::string auth   = push_crypto::b64url_decode(auth_b64url);

  // Fresh ephemeral keypair per notification (RFC 8291 §3.3).
  const auto eph = push_crypto::p256_generate();
  const std::string salt = push_crypto::rand_bytes(16);
  return push_crypto::encrypt_payload_impl(payload, p256dh, auth,
                                            eph.private_pem,
                                            eph.public_uncompressed,
                                            salt);
}

int PushSender::notify(const std::string &subscriber_id,
                        const std::string &payload) {
  // Fetch subscriptions for this subscriber.
  const std::string query = R"({"subscriberId":")" + subscriber_id + R"("})";
  const std::string raw   = m_db.get_documents("push_subscriptions", query, "{}");
  if (raw.empty()) return 0;

  json subs;
  try { subs = json::parse(raw); } catch (...) { return 0; }
  if (!subs.is_array()) return 0;

  int delivered = 0;
  for (const auto &sub : subs) {
    if (!sub.contains("endpoint") || !sub.contains("p256dh") || !sub.contains("auth"))
      continue;
    const std::string endpoint = sub["endpoint"].get<std::string>();
    const std::string p256dh   = sub["p256dh"].get<std::string>();
    const std::string auth_b64 = sub["auth"].get<std::string>();
    const std::string sub_id   = sub.value("_id", std::string{});

    const std::string body = encrypt_payload(payload, p256dh, auth_b64);
    const std::string vapid_auth =
        build_vapid_auth(origin_of(endpoint), m_clock.now_unix());

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization",      vapid_auth},
        {"Content-Encoding",   "aes128gcm"},
        {"Content-Type",       "application/octet-stream"},
        {"TTL",                "60"},
    };

    int backoff_ms = m_cfg.initial_backoff_ms;
    bool sent = false;
    for (int attempt = 0; attempt <= m_cfg.max_retries; ++attempt) {
      const auto rsp = m_http.post(endpoint, headers, body);

      if (rsp.status >= 200 && rsp.status < 300) {
        ++delivered;
        sent = true;
        break;
      }
      if (rsp.status == 410) {
        // Subscription is permanently gone.
        if (!sub_id.empty())
          m_db.delete_document("push_subscriptions",
                                R"({"_id":")" + sub_id + R"("})");
        sent = true; // handled; no more retries for this sub
        break;
      }
      if (rsp.status == 503 && attempt < m_cfg.max_retries) {
        if (backoff_ms > 0)
          std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms *= 2;
        continue;
      }
      // Any other failure: give up on this sub but keep going through the list.
      break;
    }
    (void)sent;
  }
  return delivered;
}
