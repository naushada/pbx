#include "sip_frame.hpp"
#include <cstring>

namespace SipFrame {

namespace {

inline void write_u32_be(char *dst, std::uint32_t v) {
  dst[0] = static_cast<char>((v >> 24) & 0xFF);
  dst[1] = static_cast<char>((v >> 16) & 0xFF);
  dst[2] = static_cast<char>((v >> 8) & 0xFF);
  dst[3] = static_cast<char>(v & 0xFF);
}

inline std::uint32_t read_u32_be(const char *src) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(src[0])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[2])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[3])));
}

} // namespace

std::string encode(Op op, std::uint32_t stream_id, const std::string &payload) {
  std::string out;
  out.resize(kHeaderSize + payload.size());

  out[0] = static_cast<char>(kVersion);
  out[1] = static_cast<char>(op);
  write_u32_be(&out[2], stream_id);
  write_u32_be(&out[6], static_cast<std::uint32_t>(payload.size()));
  if (!payload.empty())
    std::memcpy(&out[kHeaderSize], payload.data(), payload.size());

  return out;
}

DecodeResult decode(const std::string &buf) {
  DecodeResult r{};

  if (buf.size() < kHeaderSize) {
    r.status = Status::NeedMore;
    return r;
  }

  const auto ver = static_cast<std::uint8_t>(buf[0]);
  if (ver != kVersion) {
    r.status = Status::Invalid;
    return r;
  }

  const auto op_raw    = static_cast<std::uint8_t>(buf[1]);
  const auto stream_id = read_u32_be(&buf[2]);
  const auto plen      = read_u32_be(&buf[6]);

  // Op-code whitelist — every value defined in sip_frame.hpp's Op enum
  // must appear here, or decode() rejects the frame as Invalid. Easy to
  // forget when adding a new op; consider relaxing this to an enum
  // range check in a future cleanup.
  switch (static_cast<Op>(op_raw)) {
    case Op::OPEN: case Op::DATA: case Op::CLOSE:
    case Op::PING: case Op::PONG: case Op::ERR:
    case Op::PUSH_NOTIFY: case Op::CDR_PUSH:
    case Op::SUBSCRIBER_REVOKED:
    case Op::REGISTER_STATE:
    case Op::AGENT_HELLO:
    case Op::SOCIETY_BOOTSTRAP:
    case Op::ASTERISK_STATUS:
      break;
    default:
      r.status = Status::Invalid;
      return r;
  }

  if (plen > kMaxPayload) {
    r.status = Status::Invalid;
    return r;
  }

  if (buf.size() < kHeaderSize + plen) {
    r.status = Status::NeedMore;
    return r;
  }

  r.status         = Status::Ok;
  r.frame.op       = static_cast<Op>(op_raw);
  r.frame.stream_id = stream_id;
  r.frame.payload   = buf.substr(kHeaderSize, plen);
  r.consumed       = kHeaderSize + plen;
  return r;
}

} // namespace SipFrame
