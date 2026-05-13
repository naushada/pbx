# pbx — cloud-side PBX module

Cloud-side (Heroku) module. Contains the new code that has no xpmile counterpart: the multiplex tunnel framing, the `SipBridge` event handler, and the VAPID Web Push sender.

Current status:

| Component   | File                                         | Status      |
|-------------|----------------------------------------------|-------------|
| `SipFrame`  | `inc/sip_frame.hpp` + `src/sip_frame.cpp`    | ✅ Complete (Layer 0) |
| `SipBridge` | `inc/sip_bridge.hpp` + `src/sip_bridge.cpp`  | ✅ Complete (Layer 1, first slice) |
| `PushSender`| (not yet)                                    | ⏳ Layer 1, later slice |

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

---

## SipBridge — cloud-side multiplexer

Owns the in-memory mapping `stream_id → BrowserSink`. Driven by callbacks; performs no I/O itself — production wires it to ACE sockets, tests wire it to in-memory fakes. Threading: single-threaded on the ACE reactor thread; no internal locking.

### Abstract sinks (dependency injection)

```cpp
class TunnelSink {
public:
  virtual void send_frame(SipFrame::Op op, std::uint32_t stream_id,
                          const std::string& payload) = 0;
};

class BrowserSink {
public:
  virtual void send_bytes(const std::string& sip_ws_bytes) = 0;
  virtual void close(const std::string& reason) = 0;
};
```

In production: `TunnelSink` wraps an `ACE_SSL_SOCK_Stream` to the agent; `BrowserSink` wraps a `WebConnection`'s outbound side. In tests: `FakeTunnel` records every frame; `FakeBrowser` records bytes + close.

### Public API

```cpp
class SipBridge {
public:
  explicit SipBridge(TunnelSink* tunnel);

  // Browser-side entry points
  std::uint32_t on_browser_upgrade(BrowserSink* browser, const std::string& open_meta);
  void          on_browser_data   (std::uint32_t stream_id, const std::string& bytes);
  void          on_browser_close  (std::uint32_t stream_id, const std::string& reason);

  // Agent-side entry points
  bool on_tunnel_bytes      (const std::string& bytes);  // false = drop tunnel
  void on_tunnel_disconnect ();
  void set_tunnel           (TunnelSink* tunnel);

  // Observability (test surface)
  std::size_t   active_streams () const;
  std::uint32_t last_stream_id () const;
};
```

### Behaviour

- **OPEN on upgrade.** `on_browser_upgrade` allocates the next stream-id (monotonic counter, starts at 1), stores the browser sink, emits an `OPEN` frame with the JSON metadata payload, returns the assigned id.
- **DATA both ways.** Browser bytes → `DATA` frame on tunnel. Tunnel `DATA` frame → routed to the matching `BrowserSink::send_bytes()`. Unknown stream-ids on either side are dropped quietly (don't echo back as `ERR` — that surface belongs to a malicious-peer model we don't need yet).
- **CLOSE semantics.**
  - Browser-initiated close (`on_browser_close`): emit `CLOSE` to agent, erase from map. **Do not** call `close()` on the browser sink — the caller owns that lifetime.
  - Agent-initiated close (incoming `CLOSE` frame): call `close(reason)` on the browser sink, erase from map.
- **PING / PONG.** Agent `PING` → reply `PONG` with the same stream-id (0 for tunnel-level heartbeat). Liveness tracking on the cloud side wires in with `CloudTunnelEndpoint` later.
- **Partial frames.** `on_tunnel_bytes` accumulates into `m_recv_buffer` and decodes as many complete frames as fit. Tail bytes wait for the next call.
- **Invalid frames.** Returns `false`. The caller (cloud tunnel endpoint) drops the tunnel; reconnect logic kicks in.
- **Tunnel disconnect.** Closes every browser sink with reason `"tunnel_lost"`, clears the map and the buffer. The stream-id counter does **not** reset — old ids stay dead forever. New browsers after reconnect get fresh ids.

### Behaviour pinned by tests (`test/sip_bridge_test.cc` — 12 tests)

Upgrade: `OnBrowserUpgrade_AssignsStreamId`, `OnBrowserUpgrade_UniqueIdsUnderManyUpgrades` (1000 sequential upgrades, no collisions).

Browser→tunnel: `OnBrowserData_FramesAndForwards`, `OnBrowserData_UnknownStreamId_DroppedSilently`.

Tunnel→browser demux: `OnTunnelData_DemuxesByStreamId` (two streams, no cross-talk), `OnTunnelData_PartialFrameAcrossReads` (split frame across two recv() calls), `OnTunnelData_PingTriggersPong`, `OnTunnelData_InvalidFrameReturnsFalse`.

Close semantics: `OnBrowserClose_SendsCloseFrame` (asserts `BrowserSink::close()` is NOT called — caller owns), `OnTunnelCloseFrame_ClosesBrowserSink` (asserts the sink IS closed when agent initiated).

Lifecycle: `OnTunnelDisconnect_ClosesAllBrowserConns`, `OnAgentReconnect_NewStreamIdsOnly` (monotonic counter survives reconnect).

> **Not yet pinned:** `HandoffOrdering` (TDD plan Layer 1) — this asserts the `remove_handler → m_handle = INVALID → publish to bridge` ordering in `WebConnection`. The test belongs to the [`webservice/`](../webservice/README.md) module's suite, lands when that module is copied from xpmile.

---

## Coming in Layer 1 (later slice)

### `PushSender` (cloud side)

VAPID-signed Web Push trigger. Receives `PUSH_NOTIFY` frames from the agent, looks up `push_subscriptions` for the target subscriber, signs a JWT (RFC 8292 audience = push endpoint origin, 12 h cap), encrypts the payload with AES-128-GCM (RFC 8291), and POSTs to each browser's push endpoint with backoff + 410 cleanup.

---

## Files

```
pbx/
  inc/
    sip_frame.hpp             # encode/decode API, op enum, status enum
    sip_bridge.hpp            # SipBridge + TunnelSink + BrowserSink interfaces
  src/
    sip_frame.cpp             # ~90 lines, hand-rolled big-endian impl
    sip_bridge.cpp            # ~100 lines, in-memory multiplexer
  test/
    sip_frame_test.cc         # 10 tests
    sip_bridge_test.cc        # 12 tests
```

## Dependencies

- `<cstdint>`, `<cstring>`, `<string>`, `<unordered_map>` — standard library only.
- [`sip_frame`](#sipframe--multiplex-tunnel-wire-format) — internal to this module.
- GTest (tests only).
- **No ACE yet.** The bridge is intentionally I/O-free; the production wrapper that owns the `ACE_Event_Handler` reactor binding lands when [`webservice/`](../webservice/README.md) is copied.

## Build & test

```sh
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer0 \
  --gtest_filter='SipFrame.*:SipBridge.*'
```

Expected: **22/22 PASSED** (10 SipFrame + 12 SipBridge).
