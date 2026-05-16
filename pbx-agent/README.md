# pbx-agent — on-prem daemon

> **Status:** ✅ Layers 0–4 complete and shipped. `pbx-agent/src/main/main.cpp`
> wires every component into one ACE reactor; the binary runs under
> `docker-compose.agent.yml` and has been verified live against the
> deployed Heroku cloud (cloud-tunnel handshake, ARI subscribe, dynamic
> pjsip provisioning). One open backlog item: native-arm64 SIGSEGV ~3 s
> after the InnerTLS handshake — reproducible under Lima, suspected
> AGENT_HELLO / SOCIETY_BOOTSTRAP / SubscriberWatcher interaction;
> needs ASan or Valgrind to localise.

C++/ACE daemon that runs on the society's on-prem host alongside Asterisk, coturn, and MongoDB. Structurally similar to the shared-library `wsdbagent` pattern (dial-out + WSS upgrade + persistent reconnect loop).

## Components

| Class             | Source-of-truth file                   | Status | Role |
|-------------------|----------------------------------------|--------|------|
| `SipFrameDemux`   | `src/main/sip_frame_demux.{hpp,cpp}`   | ✅ Complete | Receives frames off the cloud tunnel; opens (or reuses) a per-stream local socket to Asterisk's `ws://127.0.0.1:8088/ws`; pipes bytes in both directions. Also routes the cloud→agent `SUBSCRIBER_REVOKED` control frame to an installed handler. |
| `CloudConnector`  | `src/main/cloud_connector.{hpp,cpp}`   | ✅ Complete | `ACE_SSL_SOCK_Connector` dial-out to Heroku `/agent`. Maintains the persistent mTLS tunnel, reconnect with exponential backoff (1 s → 30 s cap), buffers outbound frames during transient drops, and runs the SipFrame-level heartbeat (PING every 15 s of inbound silence; drop + reconnect after 3 unanswered). Pure-logic core uses an injected `ITransport`/`ITransportFactory`; the concrete `AceSslTransport` lives in `src/main/ace_ssl_transport.{hpp,cpp}` (Layer 3). |
| `AceSslTransport` | `src/main/ace_ssl_transport.{hpp,cpp}` | ✅ Complete (Layer 3) | Concrete `ITransport` for `CloudConnector`. ACE_SSL_Context + ACE_SSL_SOCK_Connector dial; HTTP WebSocket upgrade to `/agent`; post-upgrade WS-frame layer on top of the SSL stream. Includes the matching `AceSslTransportFactory` that plugs into `CloudConnector::ITransportFactory`. |
| `AriWsClient`     | `src/main/ari_ws_client.{hpp,cpp}`     | ✅ Complete (Layer 3) | Plain-TCP WS client to Asterisk `/ari/events`. HTTP Basic auth in the upgrade request (kept out of the URL so the password doesn't end up in webserver logs). Pushes each inbound JSON text frame to `AriClient::on_event`. Reconnect is the caller's concern. |
| `AsteriskWsFactory` + `AsteriskStream` | `src/main/asterisk_ws_factory.{hpp,cpp}` | ✅ Complete (Layer 4) | Per-stream plain-TCP WS adapter from `SipFrameDemux` to chan_pjsip's `ws://127.0.0.1:8088/ws` endpoint. Same dual-pattern as `AgentStream`: ACE-owned `AsteriskStream` + non-owning back-pointer adapter held by the demux. Advertises `Sec-WebSocket-Protocol: sip` per RFC 7118 §4. No auth at WS layer — SIP digest happens inside the SIP REGISTERs. |
| `AriRestClient`   | `src/main/ari_rest_client.{hpp,cpp}`   | ✅ Complete (Layer 4) | Plain-HTTP/1.1 client for the Asterisk ARI surface `AriClient`/`CallRouter` drive — `subscribe`, `continue`, `originate`, `create_bridge`, `addChannel`, `hangup` (`DELETE`), `get_endpoint` (`GET`). HTTP Basic auth in the `Authorization` header (NOT in `?api_key=` — Asterisk logs full URLs). Synchronous; one fresh `ACE_SOCK_Stream` per call, `Connection: close`. URL-encodes path components + query values per RFC 3986. |
| `AriClient`       | `src/main/ari_client.{hpp,cpp}`        | ✅ Complete (state machine) | ARI event consumer + REST commander. Classifies each `StasisStart` as a caller leg or an originated leg, tracks active bridges (not channels), enforces admission cap, finalises CDRs on `ChannelDestroyed`, detects conference vs P2P from bridge participant count, delegates dialing to `CallRouter`, tears down a revoked subscriber's live channels (`revoke_subscriber`), and reports `EndpointStateChange` to the cloud as `REGISTER_STATE` SipFrames so the cloud's `IPresenceCache` stays in sync. WebSocket subscription wiring is external (production: an ARI WS client that pushes events into `on_event`; tests drive directly). |
| `CallRouter`      | `src/main/call_router.{hpp,cpp}`       | ✅ Complete (state machine) | Resolves a dialed extension to its SIP targets (`"0"` → guards, else the flat's active subscribers) and drives the forked-ring call: originate a leg per target, bridge the first to answer, tear the losers down. No targets / all legs fail / caller bails — all hang the right channels up. Pure event-driven state machine over `IAriRest` + `IMongodbClient`. |
| `PjsipProvisioner` | `src/main/pjsip_provisioner.{hpp,cpp}` | ✅ Complete | Materialises a `subscribers` row as **two** Asterisk sorcery objects (`aor/<user>-aor`, `endpoint/<user>`) via ARI dynamic-config PUT/DELETE. Inlined field set is drift-checked against `docker/asterisk/pjsip.conf`'s `[endpoint-resident-template]` by `PjsipTemplateDrift` (PR #22). **SIP digest auth was dropped in PR #77** — the cloud `/sip-ws` bearer-token upgrade is the sole auth layer; the endpoint no longer carries an `auth=` field and no auth sorcery object is created. `provision()` best-effort-DELETEs any legacy `<user>-auth` doc each pass to prune pre-fix state. `set_sip_realm()` (PR #24) still updates the realm string on `SOCIETY_BOOTSTRAP` but it's vestigial — nothing in the provisioned objects references it. |
| `SubscriberWatcher` | `src/main/subscriber_watcher.{hpp,cpp}` | ✅ Complete | Society-scoped bootstrap full-scan of `subscribers` at 200 ms tick cadence, plus a `mongocxx::change_stream` tail with `resumeAfter` (PR #21) — so a Mongo flap doesn't drop events provided they're still in the oplog window. Drives `PjsipProvisioner` PUT on `insert`/`update` with `status="active"`, DELETE on `delete` or `status!="active"`. |

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

  // Out-of-band tunnel ops (cloud → agent)
  using SubscriberRevokedHandler = std::function<void(const std::string& payload)>;
  void set_subscriber_revoked_handler(SubscriberRevokedHandler);

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
- **`SUBSCRIBER_REVOKED` from cloud** → hand the JSON `{societyId, sipUsername}` payload to the installed `SubscriberRevokedHandler` (production: `AriClient::revoke_subscriber`, which hangs up that subscriber's live Asterisk channels). Not tied to a stream-id; an un-handled frame is a silent no-op, not a protocol error.
- **`PUSH_NOTIFY`/`CDR_PUSH`** (agent → cloud only) ignored if seen here.
- **Invalid frame** → `on_tunnel_bytes` returns `false`; caller drops the tunnel.
- **Tunnel disconnect** → close every Asterisk stream with reason `"tunnel_lost"`, clear the map.

### Behaviour pinned by tests (`src/test/sip_frame_demux_test.cc` — 16 tests)

OPEN behavior: `OnOpenFrame_OpensLocalAsteriskSocket`, `OnOpenFrame_FactoryRefusal_EmitsCloseUpstream`, `OnOpenFrame_DuplicateStreamId_IsIdempotent`.

Cloud → Asterisk: `OnDataFrame_PipesToAsteriskSocket`, `DropsUnknownStreamId`, `OnDataFrame_AsteriskSendFails_ClosesStream`.

Asterisk → cloud: `OnAsteriskData_WrapsInDataFrame`, `OnAsteriskData_UnknownStream_Dropped`, `OnAsteriskClose_EmitsCloseFrame`.

CLOSE from either side: `OnTunnelClose_ClosesAsteriskStream`.

Heartbeat + framing: `PingTriggersPong`, `PartialFrameAcrossReads`.

Out-of-band ops: `SubscriberRevoked_InvokesHandlerWithPayload` (the `{societyId, sipUsername}` payload reaches the handler; no stream opened, nothing sent upstream), `SubscriberRevoked_NoHandler_IsSilentlyIgnored`.

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
    int           heartbeat_interval_sec = 15;  // 0 = heartbeat disabled
    int           heartbeat_max_missed   = 3;
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
  int           pings_outstanding()    const;
};
```

### Behaviour

- **Connect attempt** on `tick()` if `now_unix() >= next_reconnect_at`. Factory call is configured with `host`, `port`, and the three PEM paths verbatim from `Config`.
- **Success** → install the transport, reset `current_backoff_sec` to 0, `reconnect_attempts` to 0, flush every buffered frame in order.
- **Failure** → `current_backoff_sec` doubles each retry starting at `initial_backoff_sec`, capped at `max_backoff_sec`. Series for `(1, 30)`: 1 → 2 → 4 → 8 → 16 → 30 → 30 → … `next_reconnect_at = now + backoff`.
- **`send_frame` while connected** → `SipFrame::encode` + `transport.send`. If `send` returns false, transport is treated as broken: `mark_disconnected` + push the failing frame to the buffer for retry.
- **`send_frame` while disconnected** → push to the outbound buffer. Bounded by `Config::outbound_buffer_max` (0 = unbounded; v1 default).
- **`on_bytes_received`** → forward into the attached `SipFrameDemux`. If demux reports invalid framing, drop the tunnel. Also clears the heartbeat miss counter — any inbound bytes are proof the peer is alive.
- **`on_transport_lost`** → close current transport, tell demux `on_tunnel_disconnect`, zero the backoff so the very next tick retries immediately.
- **Heartbeat** (SipFrame-level, DESIGN.md §7) → while connected, `tick()` sends a connection-level `PING` (stream-id 0, empty payload) once per `heartbeat_interval_sec` of inbound silence. After `heartbeat_max_missed` consecutive unanswered PINGs the tunnel is declared dead and dropped via `mark_disconnected` — catches a peer that has hung without closing the socket. The first PING is one full interval after connect; the counter and clock re-arm on every (re)connect. `heartbeat_interval_sec <= 0` disables it.

### Behaviour pinned by tests (`src/test/cloud_connector_test.cc` — 15 tests)

Connect: `Connects_PresentsClientCert` (factory invoked with configured host/port/cert/key/ca), `ConnectFailure_StaysDisconnected`.

Frame writes: `SendsOpenFramesForNewStreams` (round-trip-decode confirms wire bytes match `OPEN`), `SendsPushNotifyAndCdrPushUpstream`.

Backoff: `AutoReconnectsWithBackoff` — exact series 1 → 2 → 4 → 8 → 16 → 30 → 30 → 30 with sub-backoff tick proves no early retry; `SuccessfulReconnectResetsBackoff`.

Drop survival: `SurvivesIntermittentTunnelDrop` (frames buffered during disconnect flush in order on next successful connect), `SendDuringDisconnect_BuffersUntilReconnect`, `SendMidFlightFailure_MarksDisconnectedAndBuffers` (the frame that triggered the failure is re-queued).

Inbound: `OnBytesReceived_ForwardsIntoDemux` (round-trip a PING through both halves — the demux's PONG reply lands back on the FakeTransport's sent buffer), `OnBytesReceived_InvalidFrame_DropsTunnel`.

Heartbeat: `Heartbeat_SendsPingAfterIdleInterval` (no PING before the interval, exactly one connection-level PING after), `Heartbeat_InboundBytesResetMissedCount` (inbound bytes each cycle keep the miss counter at 0 — never dropped), `Heartbeat_DropsTunnelAfterMaxMissedPings` (3 unanswered PINGs, then drop + immediate reconnect on the next tick), `Heartbeat_DisabledWhenIntervalZero`.

State recorded in a `TransportState` struct that lives in the factory, not the `FakeTransport`, so it survives the `unique_ptr` destruction during disconnect — same fake-side-channel pattern as the SipFrameDemux suite.

---

## AceSslTransport

The concrete `ITransport` that `CloudConnector` uses in production. Lives between `CloudConnector`'s state machine and the actual Heroku TLS socket.

### Dial-out flow

1. `ACE_SSL_Context::instance()` is configured per dial with the caller's CA + client cert + client key. `SSL_VERIFY_PEER` is enabled whenever a CA is supplied so we refuse cloud certs that aren't signed by our trust anchor.
2. `ACE_SSL_SOCK_Connector::connect(m_stream, addr, &timeout)` with a 10-second timeout (Heroku's router can accept the SYN but never complete TLS if the dyno is wedged — the same wedge the shared-library `wsdbagent` already guards against).
3. Send an HTTP/1.1 WebSocket upgrade request: `GET /agent`, `Host`, `Upgrade: websocket`, `Connection: Upgrade`, random base64 `Sec-WebSocket-Key`, `Sec-WebSocket-Version: 13`.
4. Read response headers until `CRLFCRLF`.
5. Validate via `validate_upgrade_response(headers, key)` — `HTTP/1.x 101 …` status + a `Sec-WebSocket-Accept` value matching `wsframe::accept_key(key)`. Any deviation → `connect_and_handshake` returns `false` and the factory returns nullptr (`CloudConnector` then bumps its backoff and retries).

### Post-upgrade traffic

- `ITransport::send(bytes)` encodes the bytes as a **masked text** WS frame (RFC 6455 §5.2 mandates client-side masking) and `send_n`s. Failure → `notify_disconnect_once()` so `CloudConnector::on_transport_lost` fires; the connector's outbound buffer catches the failed frame.
- `handle_input` reads chunks into a `vector<uint8_t>` buffer; `drain_frames()` pulls complete frames via `wsframe::decode` and forwards text/binary payloads to the injected `on_bytes` callback (wired to `CloudConnector::on_bytes_received` in production).
- Inbound `PING` is auto-replied with `wsframe::pong_frame(payload)`. `PONG` is swallowed. `CLOSE` echoes a close frame back and tears down. Unknown opcodes are treated as protocol violations.

### Lifetime

Unlike `BrowserStream`/`AgentStream` (owned by ACE; `delete this` in `handle_close`), `AceSslTransport` is owned by `CloudConnector`'s `unique_ptr<ITransport>`. `handle_close` notifies the connector but does **not** `delete this` — the connector drops the transport when `on_transport_lost` fires, which destroys it cleanly via `unique_ptr::reset()`.

### Public API

```cpp
class AceSslTransport : public ITransport, public ACE_Event_Handler {
public:
  AceSslTransport(std::function<void(const std::string&)> on_bytes,
                  std::function<void()>                    on_disconnect);

  bool connect_and_handshake(const std::string& host, std::uint16_t port,
                             const std::string& cert_path,
                             const std::string& key_path,
                             const std::string& ca_path,
                             const std::string& path = "/agent");
  int  register_with_reactor(ACE_Reactor* reactor);

  // ITransport
  bool send(const std::string& bytes) override;
  void close()                       override;

  // ACE_Event_Handler
  int        handle_input(ACE_HANDLE)                          override;
  int        handle_close(ACE_HANDLE, ACE_Reactor_Mask)        override;
  ACE_HANDLE get_handle() const                                override;

  // Pure-logic helpers (public for unit tests).
  static std::pair<std::string, std::string>
       build_upgrade_request(const std::string& host, const std::string& path);
  static bool
       validate_upgrade_response(const std::string& headers,
                                  const std::string& sec_websocket_key);
};

class AceSslTransportFactory : public ITransportFactory {
public:
  AceSslTransportFactory(ACE_Reactor*                              reactor,
                         std::function<void(const std::string&)>   on_bytes,
                         std::function<void()>                     on_disconnect);
  std::unique_ptr<ITransport>
    create_connected(const std::string& host, std::uint16_t port,
                     const std::string& cert_path,
                     const std::string& key_path,
                     const std::string& ca_path) override;
};
```

### Behaviour pinned by tests (`src/test/ace_ssl_transport_test.cc` — 10 tests)

The actual SSL handshake happy path is integration territory (OpenSSL is not socketpair-friendly), but the **pure-logic helpers** are fully unit-tested and they're where the protocol correctness lives:

- `BuildUpgradeRequest_HasRequiredHeaders` — every required header is present, request ends in `CRLFCRLF`.
- `BuildUpgradeRequest_KeyIs24CharsBase64` — 16 raw bytes → 22 base64 chars + 2 `=` padding.
- `BuildUpgradeRequest_GeneratesUniqueKeys` — 100 sequential builds → 100 unique keys.
- `BuildUpgradeRequest_RespectsCustomPath` — request line reflects the requested path.
- `ValidateUpgradeResponse_AcceptsCorrect101` — well-formed 101 with matching `Sec-WebSocket-Accept` passes.
- `ValidateUpgradeResponse_RejectsNon101` — 400 (or any non-101) is rejected even if the Accept value happens to match.
- `ValidateUpgradeResponse_RejectsMissingAccept` — 101 without the Accept header is rejected.
- `ValidateUpgradeResponse_RejectsWrongAccept` — Accept value not matching `wsframe::accept_key(key)` is rejected.
- `ValidateUpgradeResponse_RejectsGarbage` — empty / non-status-line input is rejected.
- `FactoryUnreachableHost_ReturnsNullptr` — `127.0.0.1:1` is not listening; factory's `create_connected` returns nullptr cleanly within the 10s timeout (no segfault, no exception leak).

The full TLS-handshake happy path lands in Layer 4 podman-compose integration.

---

## AriWsClient

Plain-TCP WebSocket client for Asterisk's `/ari/events` push stream. Asterisk's ARI port is loopback-only on the on-prem host, so no TLS — but **authentication is still required**. The client uses HTTP Basic in the upgrade request (kept out of the URL so the password doesn't land in webserver logs).

### Dial-out flow

1. `ACE_SOCK_Connector::connect` (plain TCP) to `127.0.0.1:8088` with a 10-second timeout.
2. Send an HTTP/1.1 GET request:
   ```
   GET /ari/events?app=pbx HTTP/1.1
   Host: 127.0.0.1
   Upgrade: websocket
   Connection: Upgrade
   Sec-WebSocket-Key: <random base64>
   Sec-WebSocket-Version: 13
   Authorization: Basic <base64(user:pass)>
   ```
3. Read response headers until `CRLFCRLF`.
4. `validate_upgrade_response`: HTTP/1.x 101 + matching `Sec-WebSocket-Accept`.

### Post-upgrade traffic

The wire is read-only from the cloud's standpoint — Asterisk pushes events, we don't write anything except PONG/CLOSE replies. Each text frame is one JSON event ready for `AriClient::on_event(payload)`.

### Lifetime

`AriWsClient` is owned by the caller (typically the `pbx-agent` main loop), not by ACE. `handle_close` cleans up but does NOT `delete this`. Reconnect on Asterisk restart is the caller's concern — a tiny supervisor loop in `main` checks `connected()` and calls `connect_and_handshake()` again. Keeping reconnect out of `AriWsClient` keeps the class surface small and testable.

### Public API

```cpp
class AriWsClient : public ACE_Event_Handler {
public:
  struct Config {
    std::string   host     = "127.0.0.1";
    std::uint16_t port     = 8088;
    std::string   app_name = "pbx";
    std::string   username = "asterisk";
    std::string   password = "asterisk";
  };

  // Production
  AriWsClient(Config, std::function<void(const std::string&)> on_event,
              std::function<void()> on_disconnect = {});
  // Test — takes a pre-connected fd; skips connect + reactor registration
  AriWsClient(ACE_HANDLE, std::function<void(const std::string&)> on_event,
              std::function<void()> on_disconnect = {});

  bool connect_and_handshake();
  int  register_with_reactor(ACE_Reactor*);

  // ACE_Event_Handler
  int        handle_input(ACE_HANDLE)                          override;
  int        handle_close(ACE_HANDLE, ACE_Reactor_Mask)        override;
  ACE_HANDLE get_handle() const                                override;

  void close();
  bool connected() const;

  // Pure-logic (public for unit tests)
  static std::pair<std::string, std::string>
       build_upgrade_request(const std::string& host, const std::string& app,
                              const std::string& user, const std::string& pass);
  static bool
       validate_upgrade_response(const std::string& headers,
                                  const std::string& sec_websocket_key);
  static std::string base64_encode(const std::string&);
};
```

### Behaviour pinned by tests (`src/test/ari_ws_client_test.cc` — 14 tests)

Pure logic:
- `Base64Encode_KnownVectors` — RFC 4648 §10 vectors (`""`, `"f"`, `"fo"`, …, `"foobar"`).
- `Base64Encode_BasicAuthString` — `asterisk:asterisk` → `YXN0ZXJpc2s6YXN0ZXJpc2s=`.
- `BuildUpgradeRequest_IncludesAppParam` — request line includes `?app=pbx`.
- `BuildUpgradeRequest_IncludesBasicAuth` — `Authorization: Basic …` header present.
- `BuildUpgradeRequest_HasRequiredWsHeaders` — `Upgrade`, `Connection`, `Sec-WebSocket-Version: 13`, key echoed.
- `BuildUpgradeRequest_GeneratesUniqueKeys` — 100 sequential builds, all unique.
- `ValidateUpgradeResponse_AcceptsCorrect101`, `…Rejects401`.

socketpair-driven dispatch:
- `OnInput_TextFrame_DispatchesToOnEvent` — masked WS text containing a real ARI JSON event hits the callback.
- `OnInput_PingTriggersPong` — auto-reply.
- `OnInput_CloseFrame_DisconnectsClient` — disconnect callback fires; `handle_input` returns `-1`.
- `MultipleFramesInOneRead` — two consecutive `BridgeCreated` / `BridgeDestroyed` events.
- `PartialFrameAcrossReads` — frame split across two `handle_input` calls reassembles.

Smoke:
- `ConnectUnreachable_ReturnsFalse` — `127.0.0.1:1` not listening; clean nullptr.

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
  virtual Response originate(const std::string& endpoint, const std::string& app,
                              const std::string& app_args,
                              const std::string& channel_id,
                              const std::string& caller_id) = 0;
  virtual Response create_bridge(const std::string& bridge_id,
                                  const std::string& type) = 0;
  virtual Response add_channel_to_bridge(const std::string& bridge_id,
                                          const std::string& channel_id) = 0;
  virtual Response hangup(const std::string& channel_id,
                           const std::string& reason) = 0;
  virtual Response get_endpoint(const std::string& tech,
                                 const std::string& resource) = 0;
};
```

Production: an HTTP client against `http://127.0.0.1:8088/ari/…` with the configured ARI credentials. Tests: `FakeAriRest` that records every call. The same interface is shared by `AriClient` (subscribe + admission `continue` + `get_endpoint` for revocation) and `CallRouter` (originate / bridge / hangup).

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
  AriClient(Config, IAriRest&, IMongodbClient&, CallRouter&);

  void start();                                  // POSTs the subscribe call
  void on_event(const std::string& json_event);  // dispatches by `type`
  void revoke_subscriber(const std::string& sip_username);  // live-call teardown
  int  active_bridges() const;
};
```

### Behaviour

**Event dispatch.** `on_event` parses the JSON, reads `type`, and routes to a per-type handler. Malformed JSON and unknown types are silently dropped.

**`StasisStart` classification.** A leg `CallRouter` originated re-enters Stasis with appArgs `["outbound", "<callerChannelId>"]`; everything else is a fresh caller leg from the dialplan's `Stasis()`. An originated leg is handed to `CallRouter::on_leg_start` and **skips both admission and CDR context** — it's part of an already-admitted call and the logical call's CDR is keyed on the caller channel, so counting or CDR-ing the leg would double up. A caller leg goes through admission, gets a CDR context, and is handed to `CallRouter::on_caller_start` to fork-ring its targets. Every `ChannelDestroyed` is forwarded to `CallRouter::on_channel_gone` before the CDR step.

**Admission.** `BridgeCreated` increments `m_active_bridges` (idempotent on duplicate `bridge.id`). `BridgeDestroyed` decrements (clamped — never negative; absorbs duplicate echoes). When a caller `StasisStart` arrives and `m_active_bridges >= Config::max_concurrent_calls`, `AriClient` calls `IAriRest::continue_in_dialplan(channel_id, busy_context, busy_extension, busy_priority)` — Asterisk's dialplan then plays a busy message and hangs up, causing the SIP caller to see 503. No CDR row is written for rejected channels (the channel never entered Stasis).

**CDR finalisation.** Three events build the per-call context:
1. `StasisStart` records `caller_flat = channel.caller.number`, `callee_flat = channel.dialplan.exten` (or `args[0]`), `started_at = now`.
2. `ChannelEnteredBridge` stamps `bridge_id` and `answered_at = now` (first time only).
3. `ChannelDestroyed` builds the CDR doc, infers `type` from the bridge's channel-count (`>= 3 → "conference"`, else `"p2p"`), normalises `hangupCause` case-insensitively (matches "busy"/"no answer"/"normal" anywhere in `cause_txt`, otherwise passes the original string), inserts into the `cdr` collection.

A `ChannelDestroyed` for a channel we never saw in `StasisStart` is dropped — no phantom CDR.

**Subscriber revocation.** `revoke_subscriber(sip_username)` is driven by a `SUBSCRIBER_REVOKED` tunnel frame (cloud admin disabled/removed the subscriber). It does `IAriRest::get_endpoint("PJSIP", sip_username)`, parses the response's `channel_ids` array, and `hangup`s every channel the endpoint currently owns. Best-effort: an unknown endpoint, an ARI error, a malformed body, or an empty username is a silent no-op.

### Behaviour pinned by tests (`src/test/ari_client_test.cc` — 21 tests)

Subscription: `SubscribesToChannelBridgeEndpointEvents` (REST subscribe called with `app_name` + `channel:`/`bridge:`/`endpoint:` event sources — the third drives the cloud's presence cache via REGISTER_STATE).

Bridge counter: `BridgeCreated_IncrementsActiveCount` (duplicate IDs don't double-count), `BridgeDestroyed_DecrementsActiveCount_NeverNegative` (extra echoes absorbed at 0).

Admission: `AdmissionCap_ReturnsBusyAtFive` (cap=5, 6th `StasisStart` → REST `continue` to `pbx-busy:s:1`, no CDR insert), `AdmissionCap_AllowsUnderCap`.

CDR: `HangupEvent_WritesCdr` (full doc: `societyId`, `callId`, `fromFlat`, `toFlat`, `hangupCause=normal`, `type=p2p`, plus timestamps + `durationSec`), `HangupEvent_BusyCauseNormalised` (Asterisk's `"User busy"` → `"busy"`), `ChannelDestroyed_WithoutStasisStart_NoCdr`.

Conference detection: `ConferenceBridgeEvents_TaggedAsConference` (3 channels in a `mixing` bridge → `type=conference`, includes `conferenceBridge`), `TwoChannelsInBridge_StaysAsP2p`.

CallRouter wiring: `CallerStasisStart_DelegatedToRouter` (a caller `StasisStart` reaches `CallRouter` — observable as the no-route hangup — and still accrues a CDR context), `OutboundLegStasisStart_NoAdmissionCheck_NoCdrContext` (an `outbound,…`-tagged leg, even over the admission cap, is not bounced to busy and writes no phantom CDR — it's delegated to the router instead).

Subscriber revocation: `RevokeSubscriber_HangsUpEveryChannelOfTheEndpoint` (looks the endpoint up by `PJSIP` + bare sipUsername, hangs up every `channel_id`), `RevokeSubscriber_NoActiveChannels_NoHangups`, `RevokeSubscriber_UnknownEndpointOrGarbage_NoHangups` (404 or non-JSON body → silent no-op), `RevokeSubscriber_EmptyUsername_IsNoOp`.

Endpoint state → register-state callback: `SubscribesToChannelBridgeEndpointEvents` confirms `start()` subscribes to the `endpoint:` source too. `EndpointStateChange_Online_FiresHandlerWithTrue` / `_OfflineOrUnknown_FiresHandlerWithFalse` (anything not `"online"` maps to false), `EndpointStateChange_NonPjsipTech_IsIgnored` (only PJSIP endpoints feed the cloud's presence cache), `EndpointStateChange_NoHandler_IsSilentNoOp`.

Robustness: `IgnoresMalformedJson` (bad JSON, empty object, unknown event type — all silently dropped, no state changed).

---

## CallRouter

`pbx-agent`'s dialplan is a thin `Stasis(pbx,${EXTEN})` passthrough — the agent does the routing. `CallRouter` is that routing logic, in two halves: the **resolver** (a Mongo lookup) and the **forked-ring driver** (an event-driven state machine over `IAriRest`). Reactor-thread single-threaded; no internal locking.

### Public API

```cpp
class CallRouter {
public:
  CallRouter(std::string society_id, IMongodbClient&, IAriRest&,
             std::string app_name = "pbx");

  std::vector<std::string> resolve_targets(const std::string& dialed_ext) const;

  void on_caller_start(const std::string& caller_channel_id,
                       const std::string& caller_id,
                       const std::string& dialed_ext);
  void on_leg_start(const std::string& leg_channel_id,
                    const std::string& caller_channel_id);
  void on_channel_gone(const std::string& channel_id);

  std::size_t active_calls() const;
};
```

### Behaviour

**Resolver.** `resolve_targets` runs one society-scoped `subscribers` query: `"0"` → every active `role=guard` subscriber (DESIGN.md §9); any other extension is a flat number → every active subscriber whose denormalized `flatNumber` matches (DESIGN.md §6.2). Disabled subscribers, rows with a missing/empty `sipUsername`, unknown flats, empty extensions, and DB errors/garbage JSON all resolve to "no targets".

**Forked-ring driver.** On `on_caller_start`, the router resolves the dialed extension and `originate`s one ringing leg per target — `endpoint=PJSIP/<sipUsername>`, `app=<app_name>`, `appArgs=outbound,<callerChannelId>` (so the answer is recognisable), and a self-assigned `channelId` (so the fork is tracked without round-tripping the originate response). No targets, or every originate failing, → `hangup(caller, "congestion")`. On `on_leg_start`, the **first** leg to answer wins: `create_bridge` + `add_channel_to_bridge` for the caller and that leg, then `hangup(loser, "answered")` for every sibling still ringing; a later answer (lost the race, or its caller already gone) is hung up. On `on_channel_gone`: a ringing leg is dropped, and when the last one dies unanswered the caller gets `hangup(caller, "no_answer")`; the connected leg dying releases the caller with `"normal"`; the caller dying tears down whatever legs are still attached. Idempotent per caller channel id (Asterisk re-emits `StasisStart`).

Conference (`*FLAT`, DESIGN.md §6.3) is **not** in scope here — the resolver treats a `*`-prefixed extension as an ordinary (unmatched) flat number.

### Behaviour pinned by tests (`src/test/call_router_test.cc` — 17 tests)

Resolver (7): `ResolvesFlatToItsSubscribers`, `ResolvesGuardExtensionToGuards`, `UnknownFlatReturnsEmpty`, `ExcludesDisabledSubscribers`, `SkipsRowsWithMissingOrEmptySipUsername`, `EmptyExtensionReturnsEmpty`, `MalformedOrFailedDbReturnsEmpty`.

Originate fan-out: `ForkRing_OriginatesALegPerTarget` (one `originate` per target, correct `app`/`appArgs`/`callerId`/`endpoint`, distinct leg ids), `ForkRing_NoTargets_HangsUpCaller`, `ForkRing_AllOriginatesFail_HangsUpCaller`, `ForkRing_IsIdempotentPerCaller`.

First-answer-wins: `ForkRing_FirstAnswerWins_BridgesAndTearsDownSiblings` (one bridge, caller + winner added, both losers hung up `"answered"`), `ForkRing_LateAnswerLosesTheRace` (no second bridge).

Teardown: `ForkRing_AllLegsFailBeforeAnswer_HangsUpCaller` (`"no_answer"` only once the last leg dies), `ForkRing_CallerHangsUpWhileRinging_TearsDownLegs` (late leg `ChannelDestroyed` echoes are harmless), `ForkRing_ConnectedLegHangsUp_ReleasesCaller`, `ForkRing_LegAnswersAfterCallerGone_IsHungUp`.

---

## Third-party processes co-located on the same host

Not built or shipped by this directory:

- **Asterisk** with `chan_pjsip`, WS transport on `127.0.0.1:8088`, `directmedia=yes` for 1:1, `ConfBridge` for conferences. DTLS-SRTP per [`DESIGN.md §8`](../DESIGN.md#8-media-security-dtls-srtp).
- **coturn** with `use-auth-secret`. Society opens one public UDP port and DNATs to it.
- **MongoDB**.

## Origin

`CloudConnector` repurposes the shared-library `wsdbagent` pattern. The dial-out + WSS upgrade + reconnect loop is identical; only the per-frame handler changes (BSON DB-call payload → [`sip_frame`](../modules/module/pbx/README.md) multiplex).

## Build & run

The agent ships as a separate container alongside Asterisk + coturn + MongoDB via `docker-compose.agent.yml` at the repo root.

```sh
# From the repo root.
cp .env.agent.example .env
$EDITOR .env                          # set CLOUD_HOST, AGENT_SOCIETY_ID, CERTS_DIR
podman-compose -f docker-compose.agent.yml up --build -d
```

The `pbx-agent` container is built from [`docker/Dockerfile.agent`](../docker/Dockerfile.agent) — multi-stage, `FROM pbx-cpp-builder:bootstrap` for the build stage, `FROM ubuntu:focal` for the runtime stage with only ACE/TAO + mongocxx + OpenSSL shared libs copied across.

The container's `CMD` is intentionally lean — it only passes the two operator-required args plus an optional inner-TLS hostname:

| Env var              | CLI flag passed by CMD          | Note |
|----------------------|---------------------------------|------|
| `CLOUD_HOST`         | `--cloud-host`                  | Heroku hostname for `/agent` WS upgrade (**required**). |
| `AGENT_SOCIETY_ID`   | `--society-id`                  | Mongo ObjectId or short code matching the society document (**required**). The agent emits this in `AGENT_HELLO` so the cloud replies with the canonical sipRealm. |
| `INNER_TLS_HOSTNAME` | `--inner-tls-hostname` (if set) | Optional SAN/CN to verify the cloud's InnerTLS cert against. |

Everything else (mongo URI, asterisk service DNS, ARI app/user/pass, cert paths) lives as compile-time defaults inside [`pbx-agent/src/main/main.cpp`](src/main/main.cpp) since they're invariant for a compose-deployed agent:

| Compile-time default in `main.cpp` | Value |
|------------------------------------|-------|
| `tls_cert` / `tls_key` / `tls_ca` | `/opt/pbx-agent/certs/{agent.crt,agent.key,cloud-ca.pem}` (CERTS_DIR mount lands here) |
| `inner_tls_cert/key/ca` | Same triple — reused for the inner handshake |
| `mongo_uri` | `mongodb://pbx-mongo:27017/pabx` (compose service DNS) |
| `ast_host` / `ast_port` | `pbx-asterisk` / `8088` |
| `ari_app` / `ari_user` / `ari_pass` | `pbx` / `asterisk` / `asterisk` (matches `docker/asterisk/ari.conf`; **override for prod** by adding `command: ["…", "--ari-user", "…"]` to the pbx-agent service) |
| `sip_realm` | empty → cloud-provided via `SOCIETY_BOOTSTRAP`; CLI `--sip-realm` overrides |

Bare-metal / dev / Lima-VM use can still pass any flag explicitly (`./pbx-agent --asterisk-host 127.0.0.1 --mongo-uri …`); the binary's flag surface is unchanged.

Smoke test once the stack is up:

```sh
podman-compose -f docker-compose.agent.yml logs -f pbx-agent
# expect: TLS handshake, /agent upgrade 101, "tunnel ready"
```

## Tests

All component suites live in `src/test/` and run as part of the
offtarget binary (`podman-compose -f docker-compose.test.yml run --rm
offtarget`). Per-layer breakdown:

- **Layer 2 (state machines, no I/O):** `CloudConnector*` (15),
  `SipFrameDemux*` (16), `AriClient*` (21), `CallRouter*` (17),
  `PjsipProvisioner*` (8), `SubscriberWatcher*` (17).
- **Layer 3 (ACE bindings):** `AceSslTransport*` (10), `AriWsClient*` (14),
  `WsInnerTlsBridge*` (11), `PjsipTemplateDrift*` (1).
- **Layer 4:** `AsteriskWsFactory*` (13), `AriRestClient*` (16),
  `AceHttpsClient*` (13).

See [`TDD-PLAN.md`](../TDD-PLAN.md) for the per-layer test policy.
