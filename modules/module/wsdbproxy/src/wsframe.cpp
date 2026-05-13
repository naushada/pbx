#include "wsframe.hpp"

#include <cstring>
#include <random>
#include <stdexcept>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace wsframe {

namespace {

// ── masking ───────────────────────────────────────────────────────────────────

void apply_mask(std::vector<uint8_t>& data, uint32_t key)
{
  uint8_t k[4];
  k[0] = (key >> 24) & 0xFF;
  k[1] = (key >> 16) & 0xFF;
  k[2] = (key >>  8) & 0xFF;
  k[3] =  key        & 0xFF;
  for (std::size_t i = 0; i < data.size(); ++i)
    data[i] ^= k[i % 4];
}

uint32_t random_mask_key()
{
  static thread_local std::mt19937 rng{std::random_device{}()};
  return static_cast<uint32_t>(rng());
}

// ── Base64 ────────────────────────────────────────────────────────────────────

std::string base64_encode(const unsigned char* data, std::size_t len)
{
  BIO* bmem = BIO_new(BIO_s_mem());
  BIO* b64  = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO* chain = BIO_push(b64, bmem);
  BIO_write(chain, data, static_cast<int>(len));
  BIO_flush(chain);

  BUF_MEM* ptr = nullptr;
  BIO_get_mem_ptr(chain, &ptr);
  std::string result(ptr->data, ptr->length);
  BIO_free_all(chain);
  return result;
}

} // namespace

// ── encode ────────────────────────────────────────────────────────────────────

std::vector<uint8_t> encode(const std::vector<uint8_t>& payload,
                             uint8_t  opcode,
                             bool     mask,
                             uint32_t mask_key)
{
  std::vector<uint8_t> frame;
  frame.reserve(payload.size() + 14);

  // Byte 0: FIN(1) + RSV(000) + opcode(4 bits)
  frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));

  // Byte 1: MASK + payload length
  uint8_t mask_bit = mask ? 0x80 : 0x00;
  uint64_t plen = payload.size();

  if (plen < 126) {
    frame.push_back(mask_bit | static_cast<uint8_t>(plen));
  } else if (plen < 65536) {
    frame.push_back(mask_bit | 126);
    frame.push_back(static_cast<uint8_t>((plen >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>( plen        & 0xFF));
  } else {
    frame.push_back(mask_bit | 127);
    for (int s = 56; s >= 0; s -= 8)
      frame.push_back(static_cast<uint8_t>((plen >> s) & 0xFF));
  }

  if (mask) {
    uint32_t key = mask_key ? mask_key : random_mask_key();
    frame.push_back(static_cast<uint8_t>((key >> 24) & 0xFF));
    frame.push_back(static_cast<uint8_t>((key >> 16) & 0xFF));
    frame.push_back(static_cast<uint8_t>((key >>  8) & 0xFF));
    frame.push_back(static_cast<uint8_t>( key        & 0xFF));

    std::vector<uint8_t> masked = payload;
    apply_mask(masked, key);
    frame.insert(frame.end(), masked.begin(), masked.end());
  } else {
    frame.insert(frame.end(), payload.begin(), payload.end());
  }

  return frame;
}

// ── decode ────────────────────────────────────────────────────────────────────

std::optional<Frame> decode(std::vector<uint8_t>& buf)
{
  if (buf.size() < 2) return std::nullopt;

  uint8_t  byte0  = buf[0];
  uint8_t  byte1  = buf[1];
  uint8_t  opcode = byte0 & 0x0F;
  bool     masked = (byte1 & 0x80) != 0;
  uint64_t plen   = byte1 & 0x7F;

  std::size_t header_size = 2;

  if (plen == 126) {
    if (buf.size() < 4) return std::nullopt;
    plen = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
    header_size = 4;
  } else if (plen == 127) {
    if (buf.size() < 10) return std::nullopt;
    plen = 0;
    for (int i = 0; i < 8; ++i)
      plen = (plen << 8) | buf[2 + i];
    header_size = 10;
  }

  std::size_t mask_size = masked ? 4 : 0;
  std::size_t total     = header_size + mask_size + plen;

  if (buf.size() < total) return std::nullopt;

  uint8_t mask_key[4] = {};
  if (masked) {
    mask_key[0] = buf[header_size + 0];
    mask_key[1] = buf[header_size + 1];
    mask_key[2] = buf[header_size + 2];
    mask_key[3] = buf[header_size + 3];
  }

  std::vector<uint8_t> payload(buf.begin() + header_size + mask_size,
                                buf.begin() + total);
  if (masked) {
    for (std::size_t i = 0; i < payload.size(); ++i)
      payload[i] ^= mask_key[i % 4];
  }

  buf.erase(buf.begin(), buf.begin() + total);

  return Frame{opcode, std::move(payload)};
}

// ── accept_key ────────────────────────────────────────────────────────────────

std::string accept_key(const std::string& sec_websocket_key)
{
  static const std::string GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string input = sec_websocket_key + GUID;

  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(input.data()),
       input.size(), hash);

  return base64_encode(hash, SHA_DIGEST_LENGTH);
}

// ── control frames ────────────────────────────────────────────────────────────

std::vector<uint8_t> close_frame()
{
  return encode({}, 0x08);
}

std::vector<uint8_t> ping_frame()
{
  return encode({}, 0x09);
}

std::vector<uint8_t> pong_frame(const std::vector<uint8_t>& payload)
{
  return encode(payload, 0x0A);
}

} // namespace wsframe
