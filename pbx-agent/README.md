# pbx-agent — on-prem daemon

> **Status:** 🔄 Layer 2 in progress. ✅ `SipFrameDemux` complete (first slice). ⏳ `CloudConnector` and `AriClient` next.

C++/ACE daemon that runs on the society's on-prem host alongside Asterisk, coturn, and MongoDB. Structurally similar to xpmile's `wsdbagent` (the analog directory in xpmile is `xpmile/onprem/` — same role, different name).

## Components

| Class             | Source-of-truth file                   | Status | Role |
|-------------------|----------------------------------------|--------|------|
| `SipFrameDemux`   | `src/main/sip_frame_demux.{hpp,cpp}`   | ✅ Complete | Receives frames off the cloud tunnel; opens (or reuses) a per-stream local socket to Asterisk's `ws://127.0.0.1:8088/ws`; pipes bytes in both directions. |
| `CloudConnector`  | `src/main/cloud_connector.{hpp,cpp}`   | ⏳ Layer 2 next | `ACE_SSL_SOCK_Connector` dial-out to Heroku `/agent`. Maintains the persistent mTLS tunnel, reconnect with exponential backoff (1 s → 30 s cap). Pattern lifted from xpmile's `wsdbagent/agent.{hpp,cpp}`. |
| `AriClient`       | `src/main/ari_client.{hpp,cpp}`        | ⏳ Layer 2 | HTTP REST + WebSocket-events client for Asterisk ARI. Subscribes to `BridgeCreated`/`BridgeDestroyed` for admission control (counts bridges, not channels — see [`DESIGN.md §6.5`](../DESIGN.md#65-admission-control-5-call-cap)), and `ChannelDestroyed` for CDR finalization. |
| `MongoSink`       | (uses [`mongodb/`](../modules/module/mongodb/README.md)) | ⏳ Layer 2 | Persists CDR rows and replicates subscriber records pushed from the cloud over `OPEN` frames. |

---

## SipFrameDemux

Agent-side mirror of the cloud's [`SipBridge`](../modules/module/pbx/README.md#sipbridge--cloud-side-multiplexer). Receives `SipFrame` bytes from the cloud tunnel and demuxes them onto per-stream local sockets to Asterisk. Bytes arriving from Asterisk (replies) are re-framed as `DATA` and sent back up the tunnel.

### Dependency injection

```cpp
class IAsteriskStream {
public:
  virtual bool send_bytes(const std::string& sip_ws_bytes) = 0;
  virtual void close      (const std::string& reason)       = 0;
};

class IAsteriskFactory {
public:
  virtual std::unique_ptr<IAsteriskStream>
    open(std::uint32_t stream_id, const std::string& open_meta) = 0;
};
```

Production wires `IAsteriskFactory::open()` to a TCP-connect + WS-handshake against `ws://127.0.0.1:8088/ws` and registers an `ACE_Event_Handler` that feeds Asterisk-bound bytes back into `SipFrameDemux::on_asterisk_data` and EOF into `on_asterisk_eof`. Tests substitute the in-memory `FakeAsteriskFactory` and an `AsteriskState` side-channel that survives the stream's destruction.

### Public API

```cpp
class SipFrameDemux {
public:
  SipFrameDemux(TunnelSink* tunnel, IAsteriskFactory& factory);

  // Tunnel-side
  bool on_tunnel_bytes      (const std::string& bytes);   // false = drop tunnel
  void on_tunnel_disconnect ();
  void set_tunnel           (TunnelSink* tunnel);

  // Asterisk-side (called by the production socket binding)
  void on_asterisk_data (std::uint32_t stream_id, const std::string& bytes);
  void on_asterisk_eof  (std::uint32_t stream_id, const std::string& reason);

  std::size_t active_streams() const;
};
```

`TunnelSink` is the shared interface from `modules/module/pbx/inc/tunnel_sink.hpp` (same interface the cloud's `SipBridge` writes into — both ends of the tunnel see the same wire shape).

### Behaviour

- **OPEN.** `factory.open(stream_id, meta)` returns the new `IAsteriskStream`; demux stores it. Idempotent — a duplicate `OPEN` for the same `stream_id` is silently ignored.
- **OPEN refusal** (`factory.open` returns `nullptr`, e.g. Asterisk unreachable) → emit `CLOSE(stream_id, "asterisk_unreachable")` back up the tunnel so the browser doesn't hang.
- **DATA from cloud** → write to the matching stream's socket. If the local write fails, demux emits CLOSE upstream and forgets the stream.
- **DATA from Asterisk** → wrap as `DATA(stream_id, bytes)` upstream. Unknown `stream_id` is dropped silently (the stream was already closed).
- **CLOSE from cloud** → call `IAsteriskStream::close(reason)` and forget the stream.
- **Asterisk EOF** (`on_asterisk_eof`) → emit `CLOSE(stream_id, reason)` upstream and forget.
- **PING from cloud** → reply `PONG(0, "")`.
- **`PUSH_NOTIFY`/`CDR_PUSH`** (agent → cloud only) ignored if seen here.
- **Invalid frame** → `on_tunnel_bytes` returns `false`; caller drops the tunnel.
- **Tunnel disconnect** → close every Asterisk stream with reason `"tunnel_lost"`, clear the map.

### Behaviour pinned by tests (`src/test/sip_frame_demux_test.cc` — 14 tests)

OPEN behavior: `OnOpenFrame_OpensLocalAsteriskSocket`, `OnOpenFrame_FactoryRefusal_EmitsCloseUpstream`, `OnOpenFrame_DuplicateStreamId_IsIdempotent`.

Cloud → Asterisk: `OnDataFrame_PipesToAsteriskSocket`, `DropsUnknownStreamId`, `OnDataFrame_AsteriskSendFails_ClosesStream`.

Asterisk → cloud: `OnAsteriskData_WrapsInDataFrame`, `OnAsteriskData_UnknownStream_Dropped`, `OnAsteriskClose_EmitsCloseFrame`.

CLOSE from either side: `OnTunnelClose_ClosesAsteriskStream`.

Heartbeat + framing: `PingTriggersPong`, `PartialFrameAcrossReads`.

Lifecycle / safety: `OnTunnelDisconnect_ClosesAllAsteriskStreams`, `OnInvalidFrame_ReturnsFalse`.

The fake's `AsteriskState` outlives the `IAsteriskStream` instance (state lives in the factory, not the stream) so close-then-erase tests survive `unique_ptr` destruction of the fake.

---

Third-party processes co-located on the same host (not built or shipped by this directory):

- **Asterisk** with `chan_pjsip`, WS transport on `127.0.0.1:8088`, `directmedia=yes` for 1:1, `ConfBridge` for conferences. DTLS-SRTP per [`DESIGN.md §8`](../../DESIGN.md#8-media-security-dtls-srtp).
- **coturn** with `use-auth-secret`. Society opens one public UDP port and DNATs to it.
- **MongoDB**.

## Origin

`CloudConnector` repurposes the xpmile pattern at `xpmile/modules/module/wsdbagent/`. The dial-out + WSS upgrade + reconnect loop is identical; only the per-frame handler changes (BSON DB-call payload → [`sip_frame`](../modules/module/pbx/README.md) multiplex).

## Build & run (when populated)

The agent ships as a separate container alongside Asterisk + coturn + MongoDB via `docker-compose.agent.yml`. See `xpmile/docker-compose.agent.yml` as the template.

## Tests (Layer 2)

`CloudConnector*`, `SipFrameDemux*`, `AriClient*` — see [`TDD-PLAN.md → Layer 2`](../TDD-PLAN.md).
