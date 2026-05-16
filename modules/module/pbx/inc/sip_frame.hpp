#ifndef SIP_FRAME_HPP
#define SIP_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Wire format for the Heroku ↔ pbx-agent multiplex tunnel.
 *
 * Header is fixed at 10 bytes, big-endian:
 *
 *   0       1       2               6               10
 *   +-------+-------+---------------+---------------+--------+
 *   | ver=1 | op    | stream-id (4) | payload-len(4)| payload|
 *   +-------+-------+---------------+---------------+--------+
 *
 * See DESIGN.md §7 for the op-code table and direction semantics.
 *
 * Encode: `encode(op, stream-id, payload) -> bytes`.
 * Decode: `decode(buf) -> {frame, bytes-consumed, status}`. Status reports
 *   `Ok`, `NeedMore` (header or payload not yet complete), or `Invalid`
 *   (bad version / oversize / etc.).
 */
namespace SipFrame {

constexpr std::uint8_t kVersion = 0x01;
constexpr std::size_t  kHeaderSize = 10;
constexpr std::size_t  kMaxPayload = 1 * 1024 * 1024;  // 1 MiB cap per DESIGN.md §7

enum class Op : std::uint8_t {
  OPEN              = 0x01,  // cloud -> agent: open a stream-id with browser meta
  DATA              = 0x02,  // bidirectional: raw SIP-WS frame bytes
  CLOSE             = 0x03,  // bidirectional: terminate stream-id
  PING              = 0x04,  // bidirectional: heartbeat
  PONG              = 0x05,  // bidirectional: heartbeat ack
  ERR               = 0x06,  // bidirectional: protocol violation
  PUSH_NOTIFY       = 0x10,  // agent -> cloud: trigger Web Push for incoming INVITE
  CDR_PUSH          = 0x11,  // agent -> cloud: finalized CDR (BSON payload)
  SUBSCRIBER_REVOKED = 0x12, // cloud -> agent: a subscriber was disabled/removed;
                             // payload is JSON {societyId, sipUsername}. The
                             // agent tears down that subscriber's live Asterisk
                             // channels via ARI. stream-id is unused (0).
  REGISTER_STATE     = 0x13, // agent -> cloud: a subscriber's SIP REGISTER
                             // state changed. Payload JSON
                             // {societyId, sipUsername, online}. Driven by
                             // Asterisk's `EndpointStateChange` ARI event; the
                             // cloud caches it for the directory's `online`
                             // column. stream-id is unused (0).
  AGENT_HELLO        = 0x14, // agent -> cloud: agent identifies itself by
                             // societyId on every (re)connect. Payload JSON
                             // {societyId}. Cloud responds with a
                             // SOCIETY_BOOTSTRAP carrying the canonical
                             // sipRealm so the agent's PjsipProvisioner
                             // doesn't have to guess the realm from a CLI
                             // flag. stream-id is unused (0).
  SOCIETY_BOOTSTRAP  = 0x15, // cloud -> agent: per-society config the agent
                             // needs but doesn't have locally. Payload JSON
                             // {societyId, sipRealm}. Emitted in response to
                             // AGENT_HELLO; on receipt the agent updates its
                             // PjsipProvisioner realm and (if it differs from
                             // the realm a prior bootstrap used) re-PUTs every
                             // subscriber's auth object via
                             // SubscriberWatcher::resync(). stream-id unused.
};

struct Frame {
  Op           op;
  std::uint32_t stream_id;
  std::string  payload;
};

enum class Status {
  Ok,        // a complete frame was decoded
  NeedMore,  // header or payload not yet complete; call again with more bytes
  Invalid,   // protocol error (bad version, oversize payload, etc.)
};

struct DecodeResult {
  Status      status;
  Frame       frame;        // valid only when status == Ok
  std::size_t consumed = 0; // bytes consumed from the buffer
};

/// Encode a frame to its wire bytes.
std::string encode(Op op, std::uint32_t stream_id, const std::string &payload);

/// Decode one frame from the front of @p buf. Does not modify @p buf.
DecodeResult decode(const std::string &buf);

} // namespace SipFrame

#endif // SIP_FRAME_HPP
