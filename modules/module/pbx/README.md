# pbx — cloud-side PBX module

Cloud-side (Heroku) module. Contains the new code that has no xpmile counterpart: the multiplex tunnel framing, the `SipBridge` event handler, and the VAPID Web Push sender.

Today (Layer 0):

| Component   | File                                       | Status      |
|-------------|--------------------------------------------|-------------|
| `SipFrame`  | `inc/sip_frame.hpp` + `src/sip_frame.cpp`  | ✅ Complete |
| `SipBridge` | (not yet)                                  | ⏳ Layer 1  |
| `PushSender`| (not yet)                                  | ⏳ Layer 1  |

---

## SipFrame — multiplex tunnel wire format

A tiny, length-prefixed, big-endian binary framing protocol carried over the single mTLS WSS tunnel between Heroku and `pbx-agent`. One physical tunnel multiplexes many browser SIP-WS sessions plus out-of-band control messages (push notifications, CDR replication, heartbeats).

See [`DESIGN.md §7`](../../../DESIGN.md#7-tunnel-framing-heroku--pbx-agent) for the architectural rationale.

### Wire format

```
 0       1       2               6               10
 +-------+-------+---------------+---------------+--------+
 | ver=1 | op    | stream-id (4) | payload-len(4)| payload|
 +-------+-------+---------------+---------------+--------+
```

- All integers **big-endian**.
- Header is fixed at 10 bytes.
- `payload-len > 1 MiB` → decoder returns `Invalid` (peer must drop the connection).

### Op-codes

| Op   | Name         | Direction           | Payload                                                                 |
|------|--------------|---------------------|-------------------------------------------------------------------------|
| 0x01 | `OPEN`       | cloud → agent       | `{societyId, sipUsername, clientUA}` (JSON) — agent opens an Asterisk socket for this stream-id |
| 0x02 | `DATA`       | both ways           | raw SIP-WS frame bytes — opaque, byte-faithful                          |
| 0x03 | `CLOSE`      | both ways           | `{reason}` (JSON) — release stream-id                                   |
| 0x04 | `PING`       | both ways           | empty — sent every 15 s by the quiet side                               |
| 0x05 | `PONG`       | both ways           | mirrors `PING` — three missed `PONG`s → drop tunnel                     |
| 0x06 | `ERR`        | both ways           | `{code, msg}` — protocol violation; sender closes the tunnel            |
| 0x10 | `PUSH_NOTIFY`| agent → cloud       | `{subscriberId, callerFlat, callId}` — INVITE arrived, cloud should VAPID-push |
| 0x11 | `CDR_PUSH`   | agent → cloud       | BSON CDR doc — replicates finalized CDR to cloud for portal reads       |

Op-code 0x00 and any code outside the whitelist above causes `decode()` to return `Invalid` — protects against bit-flips in the version byte being mistaken for an unknown op.

### Public API

```cpp
namespace SipFrame {

constexpr std::uint8_t kVersion    = 0x01;
constexpr std::size_t  kHeaderSize = 10;
constexpr std::size_t  kMaxPayload = 1 * 1024 * 1024;  // 1 MiB

enum class Op : std::uint8_t {
  OPEN = 0x01, DATA = 0x02, CLOSE = 0x03,
  PING = 0x04, PONG = 0x05, ERR = 0x06,
  PUSH_NOTIFY = 0x10, CDR_PUSH = 0x11,
};

struct Frame {
  Op            op;
  std::uint32_t stream_id;
  std::string   payload;
};

enum class Status { Ok, NeedMore, Invalid };

struct DecodeResult {
  Status      status;
  Frame       frame;         // valid only when status == Ok
  std::size_t consumed = 0;  // bytes consumed from the buffer
};

std::string  encode(Op op, std::uint32_t stream_id, const std::string& payload);
DecodeResult decode(const std::string& buf);

} // namespace SipFrame
```

`decode()` returns:

- `Ok` — a complete frame; caller advances by `consumed` bytes.
- `NeedMore` — header or payload not yet complete; caller leaves the buffer alone and retries after more bytes arrive.
- `Invalid` — protocol error; caller closes the tunnel (with `ERR` first, if it can).

### Why hand-rolled big-endian readers, not ACE_CDR?

`ACE_InputCDR` / `ACE_OutputCDR` default to **native byte order** with explicit `ACE_CDR::swap_4()` calls required for wire format. For an 8-byte integer field on either side of a 1-byte op code, the swap call overhead exceeds the value-add. The hand-rolled big-endian readers are six lines, branch-free, and trivially auditable. See [`DESIGN.md §12`](../../../DESIGN.md#12-reuse-map-dry-against-xpmile) note.

### Behaviour pinned by tests (`test/sip_frame_test.cc` — 10 tests)

Round-trip per op: `SerializeRoundTrip_Open`, `SerializeRoundTrip_Data` (binary-safe — NUL byte mid-payload), `SerializeRoundTrip_PingPong`.

Error paths: `RejectsBadVersion`, `RejectsBadOp`, `RejectsTruncatedHeader`, `RejectsTruncatedPayload`.

Streaming framing: `HandlesPayloadAtBufferBoundary` (two frames concatenated, first `decode()` consumes only frame 1).

Safety: `MaxPayloadGuard` (1 MiB + 1 → `Invalid`).

Wire format: `WireFormat_IsBigEndian` (encoded `0x01020304` appears as bytes `01 02 03 04`).

---

## Coming in Layer 1

### `SipBridge` (cloud side)

ACE event handler. Owns the WSS agent socket. When a browser upgrades to `/sip-ws`:

1. Assign a monotonic `stream_id`.
2. Send `OPEN` down the tunnel with browser metadata.
3. On every browser WSS frame, wrap in `DATA` and forward.
4. On every `DATA` frame from the agent, demux by `stream_id` back to the right browser connection.
5. On browser close, send `CLOSE`.

### `PushSender` (cloud side)

VAPID-signed Web Push trigger. Receives `PUSH_NOTIFY` frames from the agent, looks up `push_subscriptions` for the target subscriber, signs a JWT (RFC 8292 audience = push endpoint origin, 12 h cap), encrypts the payload with AES-128-GCM (RFC 8291), and POSTs to each browser's push endpoint with backoff + 410 cleanup.

---

## Files

```
pbx/
  inc/
    sip_frame.hpp             # encode/decode API, op enum, status enum
  src/
    sip_frame.cpp             # 90 lines, hand-rolled big-endian impl
  test/
    sip_frame_test.cc         # 10 tests
```

## Dependencies

- `<cstdint>`, `<cstring>`, `<string>` — standard library only.
- GTest (tests only).
- No ACE (yet — `SipBridge` will bring it in).

## Build & test

```sh
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer0 \
  --gtest_filter='SipFrame.*'
```

Expected: **10/10 PASSED**.
