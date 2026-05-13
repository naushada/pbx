#ifndef WSFRAME_HPP
#define WSFRAME_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wsframe {

/// Decoded WebSocket frame returned by ws_decode().
struct Frame {
  uint8_t              opcode;  ///< 0x1=text, 0x2=binary, 0x8=close, 0x9=ping, 0xA=pong
  std::vector<uint8_t> payload;
};

/**
 * @brief Encode a payload as a single WebSocket frame (FIN=1).
 *
 * @param payload  Raw bytes to send.
 * @param opcode   Frame opcode (default 0x2 = binary).
 * @param mask     If true, apply masking (required for client→server per RFC 6455).
 * @param mask_key Masking key; 0 means generate a random key.
 */
std::vector<uint8_t> encode(const std::vector<uint8_t>& payload,
                             uint8_t  opcode   = 0x02,
                             bool     mask     = false,
                             uint32_t mask_key = 0);

/**
 * @brief Try to decode one complete WebSocket frame from @p buf.
 *
 * On success the consumed bytes are erased from the front of @p buf and
 * the decoded frame is returned. Returns std::nullopt if @p buf does not
 * yet contain a complete frame (caller should read more bytes and retry).
 */
std::optional<Frame> decode(std::vector<uint8_t>& buf);

/**
 * @brief Compute the Sec-WebSocket-Accept header value from Sec-WebSocket-Key.
 *
 * Uses SHA-1 (OpenSSL) and Base64 encoding as specified in RFC 6455 §1.3.
 */
std::string accept_key(const std::string& sec_websocket_key);

/// Build a minimal WebSocket close frame (opcode 0x8, empty payload).
std::vector<uint8_t> close_frame();

/// Build a ping frame (opcode 0x9, empty payload).
std::vector<uint8_t> ping_frame();

/// Build a pong frame (opcode 0xA) echoing the given payload.
std::vector<uint8_t> pong_frame(const std::vector<uint8_t>& payload = {});

} // namespace wsframe

#endif // WSFRAME_HPP
