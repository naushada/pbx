# pbx-agent — on-prem daemon

> **Status:** 🔄 Layer 2 in progress. ✅ `SipFrameDemux` complete. ✅ `CloudConnector` state machine complete (concrete `AceSslTransport` deferred to Layer 3 integration). ✅ `AriClient` complete (event processor + admission counter + CDR writer; WebSocket subscription glue deferred to Layer 3). ⏳ Real `/sip-ws` hand-off swap next.

C++/ACE daemon that runs on the society's on-prem host alongside Asterisk, coturn, and MongoDB. Structurally similar to xpmile's `wsdbagent` (the analog directory in xpmile is `xpmile/onprem/` — same role, different name).

## Components

| Class             | Source-of-truth file                   | Status | Role |
|-------------------|----------------------------------------|--------|------|
| `SipFrameDemux`   | `src/main/sip_frame_demux.{hpp,cpp}`   | ✅ Complete | Receives frames off the cloud tunnel; opens (or reuses) a per-stream local socket to Asterisk's `ws://127.0.0.1:8088/ws`; pipes bytes in both directions. |
| `CloudConnector`  | `src/main/cloud_connector.{hpp,cpp}`   | ✅ Complete (state machine) | `ACE_SSL_SOCK_Connector` dial-out to Heroku `/agent`. Maintains the persistent mTLS tunnel, reconnect with exponential backoff (1 s → 30 s cap), buffers outbound frames during transient drops. Pure-logic core uses an injected `ITransport`/`ITransportFactory`; the concrete `AceSslTransport` lands with Layer 3 integration. |
| `AriClient`       | `src/main/ari_client.{hpp,cpp}`        | ✅ Complete (state machine) | ARI event consumer + REST commander. Tracks active bridges (not channels), enforces admission cap, finalises CDRs on `ChannelDestroyed`, detects conference vs P2P from bridge participant count. WebSocket subscription wiring is external (production: an ARI WS client that pushes events into `on_event`; tests drive directly). |
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

## CloudConnector

Agent-side dialer for the mTLS tunnel to Heroku `/agent`. Implements `TunnelSink` so `SipFrameDemux` (and the upcoming `AriClient` for `PUSH_NOTIFY` / `CDR_PUSH`) can write frames upstream without knowing about ACE or TLS. The reactor-thread state machine is pure logic + policy — concrete socket I/O lives behind two interfaces.

### Dependency injection

```cpp
class ITransport {
public:
  virtual bool send (const std::string& bytes) = 0;  // false = transport broken
  virtual void close() = 0;                          // idempotent
};

class ITransportFactory {
public:
  virtual std::unique_ptr<ITransport>
    create_connected(const std::string& host, std::uint16_t port,
                     const std::string& client_cert_path,
                     const std::string& client_key_path,
                     const std::string& server_ca_path) = 0;
};

class IClock { public: virtual std::int64_t now_unix() const = 0; };
```

Production: `AceSslTransportFactory` wraps an `ACE_SSL_SOCK_Connector` and presents the configured mTLS material. Tests substitute a `FakeFactory` that records every connect attempt's parameters and scripts the outcomes (`true`/`false` per attempt).

### Public API

```cpp
class CloudConnector : public TunnelSink {
public:
  struct Config {
    std::string   host;
    std::uint16_t port = 443;
    std::string   client_cert_path, client_key_path, server_ca_path;
    int           initial_backoff_sec = 1;
    int           max_backoff_sec     = 30;
    std::size_t   outbound_buffer_max = 0;  // 0 = unbounded (v1)
  };
  CloudConnector(Config, ITransportFactory&, IClock&);

  void attach_demux(SipFrameDemux* demux);

  // TunnelSink
  void send_frame(SipFrame::Op, std::uint32_t, const std::string&) override;

  void tick();                                        // production: ACE timer
  void on_bytes_received(const std::string& bytes);   // production: ACE handler
  void on_transport_lost();                           // production: ACE close

  bool          connected()            const;
  int           reconnect_attempts()   const;
  int           current_backoff_sec()  const;
  std::int64_t  next_reconnect_at()    const;
  std::size_t   buffered_frame_count() const;
};
```

### Behaviour

- **Connect attempt** on `tick()` if `now_unix() >= next_reconnect_at`. Factory call is configured with `host`, `port`, and the three PEM paths verbatim from `Config`.
- **Success** → install the transport, reset `current_backoff_sec` to 0, `reconnect_attempts` to 0, flush every buffered frame in order.
- **Failure** → `current_backoff_sec` doubles each retry starting at `initial_backoff_sec`, capped at `max_backoff_sec`. Series for `(1, 30)`: 1 → 2 → 4 → 8 → 16 → 30 → 30 → … `next_reconnect_at = now + backoff`.
- **`send_frame` while connected** → `SipFrame::encode` + `transport.send`. If `send` returns false, transport is treated as broken: `mark_disconnected` + push the failing frame to the buffer for retry.
- **`send_frame` while disconnected** → push to the outbound buffer. Bounded by `Config::outbound_buffer_max` (0 = unbounded; v1 default).
- **`on_bytes_received`** → forward into the attached `SipFrameDemux`. If demux reports invalid framing, drop the tunnel.
- **`on_transport_lost`** → close current transport, tell demux `on_tunnel_disconnect`, zero the backoff so the very next tick retries immediately.

### Behaviour pinned by tests (`src/test/cloud_connector_test.cc` — 11 tests)

Connect: `Connects_PresentsClientCert` (factory invoked with configured host/port/cert/key/ca), `ConnectFailure_StaysDisconnected`.

Frame writes: `SendsOpenFramesForNewStreams` (round-trip-decode confirms wire bytes match `OPEN`), `SendsPushNotifyAndCdrPushUpstream`.

Backoff: `AutoReconnectsWithBackoff` — exact series 1 → 2 → 4 → 8 → 16 → 30 → 30 → 30 with sub-backoff tick proves no early retry; `SuccessfulReconnectResetsBackoff`.

Drop survival: `SurvivesIntermittentTunnelDrop` (frames buffered during disconnect flush in order on next successful connect), `SendDuringDisconnect_BuffersUntilReconnect`, `SendMidFlightFailure_MarksDisconnectedAndBuffers` (the frame that triggered the failure is re-queued).

Inbound: `OnBytesReceived_ForwardsIntoDemux` (round-trip a PING through both halves — the demux's PONG reply lands back on the FakeTransport's sent buffer), `OnBytesReceived_InvalidFrame_DropsTunnel`.

State recorded in a `TransportState` struct that lives in the factory, not the `FakeTransport`, so it survives the `unique_ptr` destruction during disconnect — same fake-side-channel pattern as the SipFrameDemux suite.

### Deferred to Layer 3

The concrete `AceSslTransport` (real `ACE_SSL_SOCK_Connector` + cert presentation + reactor binding + read-handler that calls `on_bytes_received`). Cheaper to verify end-to-end against a real Heroku stub than to mock the entire ACE stack here.

---

## AriClient

Asterisk REST Interface consumer. Owns the per-call accumulators (channels, bridges) and writes CDR docs on hangup. Reactor-thread single-threaded; no internal locking.

### Dependency injection

```cpp
class IAriRest {
public:
  struct Response { int status; std::string body; };
  virtual Response subscribe(const std::string& app,
                              const std::vector<std::string>& event_sources) = 0;
  virtual Response continue_in_dialplan(const std::string& channel_id,
                                         const std::string& context,
                                         const std::string& extension,
                                         int priority) = 0;
};
```

Production: an HTTP client against `http://127.0.0.1:8088/ari/…` with the configured ARI credentials. Tests: `FakeAriRest` that records every call.

The WebSocket event stream is **external** to `AriClient` — production wires an ARI WS client that calls `client.on_event(json_string)` per event. Tests drive `on_event` directly with hand-rolled JSON. This keeps `AriClient` itself testable without any networking.

### Public API

```cpp
class AriClient {
public:
  struct Config {
    std::string society_id;
    std::string app_name             = "pbx";
    int         max_concurrent_calls = 5;
    std::string busy_context         = "pbx-busy";
    std::string busy_extension       = "s";
    int         busy_priority        = 1;
  };
  AriClient(Config, IAriRest&, IMongodbClient&);

  void start();                                  // POSTs the subscribe call
  void on_event(const std::string& json_event);  // dispatches by `type`
  int  active_bridges() const;
};
```

### Behaviour

**Event dispatch.** `on_event` parses the JSON, reads `type`, and routes to a per-type handler. Malformed JSON and unknown types are silently dropped.

**Admission.** `BridgeCreated` increments `m_active_bridges` (idempotent on duplicate `bridge.id`). `BridgeDestroyed` decrements (clamped — never negative; absorbs duplicate echoes). When `StasisStart` arrives and `m_active_bridges >= Config::max_concurrent_calls`, `AriClient` calls `IAriRest::continue_in_dialplan(channel_id, busy_context, busy_extension, busy_priority)` — Asterisk's dialplan then plays a busy message and hangs up, causing the SIP caller to see 503. No CDR row is written for rejected channels (the channel never entered Stasis).

**CDR finalisation.** Three events build the per-call context:
1. `StasisStart` records `caller_flat = channel.caller.number`, `callee_flat = channel.dialplan.exten` (or `args[0]`), `started_at = now`.
2. `ChannelEnteredBridge` stamps `bridge_id` and `answered_at = now` (first time only).
3. `ChannelDestroyed` builds the CDR doc, infers `type` from the bridge's channel-count (`>= 3 → "conference"`, else `"p2p"`), normalises `hangupCause` case-insensitively (matches "busy"/"no answer"/"normal" anywhere in `cause_txt`, otherwise passes the original string), inserts into the `cdr` collection.

A `ChannelDestroyed` for a channel we never saw in `StasisStart` is dropped — no phantom CDR.

### Behaviour pinned by tests (`src/test/ari_client_test.cc` — 11 tests)

Subscription: `SubscribesToChannelEvents` (REST subscribe called with `app_name` + `channel:`/`bridge:` event sources).

Bridge counter: `BridgeCreated_IncrementsActiveCount` (duplicate IDs don't double-count), `BridgeDestroyed_DecrementsActiveCount_NeverNegative` (extra echoes absorbed at 0).

Admission: `AdmissionCap_ReturnsBusyAtFive` (cap=5, 6th `StasisStart` → REST `continue` to `pbx-busy:s:1`, no CDR insert), `AdmissionCap_AllowsUnderCap`.

CDR: `HangupEvent_WritesCdr` (full doc: `societyId`, `callId`, `fromFlat`, `toFlat`, `hangupCause=normal`, `type=p2p`, plus timestamps + `durationSec`), `HangupEvent_BusyCauseNormalised` (Asterisk's `"User busy"` → `"busy"`), `ChannelDestroyed_WithoutStasisStart_NoCdr`.

Conference detection: `ConferenceBridgeEvents_TaggedAsConference` (3 channels in a `mixing` bridge → `type=conference`, includes `conferenceBridge`), `TwoChannelsInBridge_StaysAsP2p`.

Robustness: `IgnoresMalformedJson` (bad JSON, empty object, unknown event type — all silently dropped, no state changed).

---

## WebSocket subscription glue (Layer 3)

The actual `ws://127.0.0.1:8088/ari/events?api_key=…&app=pbx` connection that pushes parsed events into `AriClient::on_event` lives outside this module. Production: an `AriEventStream` running on the same ACE reactor, reading JSON events and dispatching. Tests don't need it — `on_event` is the testable seam.

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
