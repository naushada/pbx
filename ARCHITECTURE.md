# onprem-pbx Architecture

End-to-end overview for operators bringing the stack up and developers
extending it. **For the original design rationale**, see
[`DESIGN.md`](./DESIGN.md). **For the security threat model**, see
[`docs/design/security/threat-model.md`](./docs/design/security/threat-model.md).
**For the day-to-day get-it-up checklist**, see the
[`README.md`](./README.md). This doc joins those together.

---

## 1. The big picture

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Heroku (pabx-…herokuapp.com)                                            │
│  ────────────────────────────────────────────────────────────────────    │
│                                                                          │
│   pbx-cloud (one container, the ACE WebServer + UI bundle)               │
│     ├─ Serves the Angular SPA at /webui/*                                │
│     ├─ Serves REST at /api/v1/* (login, directory, cdr, push, turn)      │
│     ├─ Accepts /sip-ws WebSocket upgrade from each browser               │
│     ├─ Accepts /agent WebSocket upgrade from each pbx-agent              │
│     └─ Accepts /ws/db WebSocket upgrade from each pbx-wsdbagent          │
│                                                                          │
│   Cloud Mongo (optional, off-image) holds replicated subscribers +       │
│   sessions when --remote-db is OFF (D4: when ON, the cloud reads/        │
│   writes the on-prem Mongo through pbx-wsdbagent's /ws/db tunnel).       │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
                                  ▲       ▲       ▲
                                  │       │       │
               (HTTPS + outer WSS │       │       │ /agent + /ws/db
                terminated at the │       │       │  carry InnerTLS
                Heroku router —   │       │       │  inside the WS
                outer auth is     │       │       │  binary frames —
                public PKI)       │       │       │  THIS is the
                                  │       │       │  real mTLS
                                  │       │       │  trust boundary.
                                  │       │       │
┌────────────────────────────┐    │       │       │    ┌─────────────┐
│  Browser (Angular UI +     │    │       │       │    │  Society    │
│  sip.js + WebRTC)          │────┘       │       │    │  installer  │
│  ── /sip-ws to cloud       │            │       │    │  host       │
│  ── DTLS-SRTP P2P or via   │            │       │    └─────────────┘
│     coturn for 1:1; via    │            │       │
│     Asterisk for           │            │       │
│     conference             │            │       │
└────────────────────────────┘            │       │
                                          │       │
                ┌─────────────────────────┴───────┴────────────────────┐
                │  Society LAN (one pbx-agent stack per society)       │
                │  ────────────────────────────────────────────────    │
                │                                                      │
                │  ┌────────────┐   ┌──────────────┐                   │
                │  │ pbx-agent  │   │ pbx-wsdbagent│  (D1+D2 standalone, │
                │  │            │   │              │   wsdbproxy tunnel)  │
                │  │ • dials    │   │ • dials      │                   │
                │  │   /agent   │   │   /ws/db     │                   │
                │  │ • drives   │   │ • forwards   │                   │
                │  │   Asterisk │   │   Mongo       │                   │
                │  │   via ARI  │   │   queries    │                   │
                │  │ • tails    │   │   to local   │                   │
                │  │   Mongo    │   │   pbx-mongo  │                   │
                │  │   change    │   │              │                   │
                │  │   stream   │   │              │                   │
                │  └─────┬──────┘   └──────┬───────┘                   │
                │        │                 │                           │
                │        │ ARI + Mongo     │ Mongo                     │
                │        ▼                 ▼                           │
                │  ┌──────────────────────────────────────────────┐   │
                │  │ pbx-asterisk      pbx-mongo (--replSet rs0)  │   │
                │  │   chan_pjsip        subscribers + cdr +      │   │
                │  │   WS on :8088       push_subscriptions       │   │
                │  │   ARI on :8088      (oplog enables change    │   │
                │  │   ConfBridge        streams)                 │   │
                │  └──────────────────────────────────────────────┘   │
                │                                                      │
                │  ┌──────────────────────────────────────────────┐   │
                │  │ pbx-coturn (host networking, real public IP) │   │
                │  │   STUN+TURN for browser↔browser ICE          │   │
                │  │   use-auth-secret with time-limited creds    │   │
                │  │   minted by the cloud per session            │   │
                │  └──────────────────────────────────────────────┘   │
                └──────────────────────────────────────────────────────┘
```

Three rules to keep in mind while reading the rest:

1. **The cloud has no SIP and no media.** It only mediates control
   plane: REST, session lookup, push delivery, SIP-over-WS multiplex.
   Audio always stays on the society's LAN (P2P or via the local
   coturn).
2. **The cloud only sees the browser through the agent.** Browsers
   open `/sip-ws` to the cloud; the cloud wraps each browser stream
   into a SipFrame and ships it over the one `/agent` tunnel to
   pbx-agent, which opens a parallel local TCP connection to
   Asterisk's chan_pjsip WS. Browser ↔ Asterisk SIP signalling
   therefore goes browser → cloud → agent → Asterisk and back, with
   each hop owning its own framing layer.
3. **Trust isn't on the outer TLS.** Heroku's router terminates the
   outer TLS at its edge, so any agent-cert-pinning would happen at
   Heroku's reverse proxy, not in our code. The real mTLS boundary
   is **inner TLS inside the WS frames** (PR #19) — see
   [`docs/design/security/innertls.md`](./docs/design/security/innertls.md).

---

## 2. Component reference

### 2.1 Browser softphone (`ui/`)

Angular 14 + Clarity + sip.js + native WebRTC. Lives under `ui/`,
deployed bundled inside the cloud image (`Dockerfile.cloud` stage 2
runs `ng build` and copies the SPA into `/opt/webgui/webui/`).

| Role | Where |
|------|-------|
| Login | `LoginComponent` → `POST /api/v1/subscriber/login` → stores `{token, subscriber}` in `AuthService` (sessionStorage). |
| Auth gate | `AuthGuard` blocks `/main/*` until a valid session is present. `AuthInterceptor` attaches the bearer on every REST call. |
| SIP signalling | `SipService` wraps sip.js; opens `wss://${CLOUD_HOST}/sip-ws?token=…`. |
| Media | sip.js owns the `RTCPeerConnection`. ICE candidates fan out via the TURN creds minted at `GET /api/v1/turn-credentials`. |
| Inbound ring | `RingtoneService` + `PushService` (VAPID). When the tab is closed, the Service Worker (`ui/src/service-worker.ts`) handles the push, calls `notificationclick`, opens the portal which re-registers and accepts the INVITE. |
| Directory | `DirectoryComponent` → `GET /api/v1/subscriber?societyId=…&flatPrefix=…`. `online` field comes from `IPresenceCache` (fed by `REGISTER_STATE` SipFrames from the agent). |

### 2.2 Cloud (`pbx-cloud`)

A single C++ ACE binary built from
`modules/module/webservice/src/webservice_main.cpp`. Stages of
`Dockerfile.cloud` produce a slim Ubuntu runtime image with:

- The `pbx-cloud` binary at `/opt/pbx-cloud/pbx-cloud`.
- The Angular SPA bundle at `/opt/webgui/webui/`.
- `/opt/pbx-cloud/certs/{ca.crt,server.crt,server.key}` — InnerTLS
  material baked in at build time. `ca.key` is **deleted from the
  build context** before the runtime stage so it never ships.

The main components inside the binary:

| Class | Role |
|-------|------|
| `WebServer` | Top-level — owns the listening socket, reactor, ACE config. |
| `WebConnection` (one per inbound socket) | Routes REST vs upgrade. Three upgrade paths: `/sip-ws` → `BrowserStream`, `/agent` → `AgentStream`, `/ws/db` → `WsDbServer`. |
| `MicroServicePbx` + dispatcher | REST handlers (login, directory, cdr, push-subscribe, turn-credentials, etc.). Authentication varies by route. |
| `SipBridge` | Multiplexes every browser's SIP-over-WS stream into the one `/agent` tunnel, prepending a per-stream id. Drives PUSH_NOTIFY + CDR_PUSH out-of-band ops. Handles incoming `AGENT_HELLO` via `set_agent_hello_handler` (PR #24) — looks up the society doc, emits `SOCIETY_BOOTSTRAP` with the canonical `sipRealm`. |
| `CloudTunnelEndpoint` | Accept-side of the cloud↔agent tunnel. Holds the one `IAgentTransport` (an `AgentStream::TransportAdapter`), buffers outbound frames while disconnected, drives a SipFrame-level heartbeat. Carries the InnerTLS config (PR #19) used by every newly-attached `AgentStream`. |
| `AgentStream` | Per-tunnel ACE event handler. `setup_inner_tls()` (PR #19) drives `InnerTlsServer::accept` over the WS frames before `attach()`. `peer_cn()` (PR #25) exposes the verified client cert CN for log labels + future cross-checks against `AGENT_HELLO`'s claimed `societyId`. |
| `IPresenceCache` (`InMemoryPresenceCache`) | Per-`(societyId, sipUsername)` `online` flag. Fed by `REGISTER_STATE` frames from the agent. Read by `handle_directory_GET` to project the `online` column. |
| `PushSender` | VAPID JWT + Web Push encryption (RFC 8291/8292). Driven by `SipBridge::set_push_notify_handler`. |
| `IRevocationSink` (`SipBridge`) | When an admin disables/removes a subscriber, ships a `SUBSCRIBER_REVOKED` SipFrame down to the agent so live calls get hung up. |

### 2.3 Agent (`pbx-agent`)

A single C++ ACE binary in `pbx-agent/src/main/`. One stack per
society. Components:

| Class | Role |
|-------|------|
| `CloudConnector` | Dials `wss://${CLOUD_HOST}/agent` via `AceSslTransport`. Reconnect-backoff (1 s → 30 s cap). SipFrame-level heartbeat (15 s PING, 3-miss drop). Calls `set_on_connected(...)` after every successful (re)connect for presence reconciliation. |
| `AceSslTransport` | Concrete `ITransport` for `CloudConnector`. Outer TLS + WS upgrade + **InnerTLS-over-WS** (PR #19). After the handshake, `WsInnerTlsBridge` switches from blocking to buffered mode so `handle_input` can drive `InnerTls::recv` from the reactor. |
| `SipFrameDemux` | Reads SipFrames off the cloud tunnel; opens (or reuses) a local TCP socket to Asterisk's `ws://127.0.0.1:8088/ws` for each `stream-id`. Bidirectional bytes pipe. |
| `AsteriskWsFactory` | Per-stream concrete `IAsteriskStream`. Plain TCP + WS upgrade to `chan_pjsip`. |
| `AriClient` | Consumes Asterisk's `/ari/events` JSON. Drives admission (bridge-counter cap at 5), CDR writes, `EndpointStateChange` → `REGISTER_STATE` SipFrames, `revoke_subscriber()` for `SUBSCRIBER_REVOKED` frames, `publish_register_snapshot()` for the presence reconciliation on (re)connect. |
| `AriWsClient` | The plain-TCP WS client that feeds `/ari/events` into `AriClient`. |
| `AriRestClient` | The HTTP-Basic REST commander Asterisk exposes at `/ari/*`. Used by `AriClient` for admission `continue`, `originate`, `bridge`, `hangup`, `endpoints`, and (PR #18) `asterisk/config/dynamic` PUT/DELETE for sorcery. |
| `CallRouter` | Forked-ring driver. Dialed extension → list of `sipUsername` targets (`"0"` → guards via the `subscribers(societyId, role)` index, else flat's active subscribers via the denormalised `flatNumber`). Fans out an `originate` per target; first-answer-wins bridges + tears down losers. |
| `MongodbClient` | Reuse of the shared-library `MongodbClient`. Now also exposes `watch_collection(coll, resume_token_json)` (PR #21) backed by `mongocxx::options::change_stream::resume_after()`. |
| `SubscriberWatcher` | Bootstrap full-scan of `subscribers` for the society + change-stream tail at 200 ms cadence. Captures every event's `_id` as the resume token; reopens with `resume_after` on `try_next` exceptions (5-tick backoff after a failed reopen). |
| `PjsipProvisioner` | Materialises a subscriber row as **two** Asterisk sorcery objects (`aor/<user>-aor`, `endpoint/<user>`) via ARI dynamic-config PUTs. Idempotent. Drift-checked against `docker/asterisk/pjsip.conf`'s `[endpoint-resident-template]` by PR #22's `PjsipTemplateDrift` test. SIP digest auth is no longer provisioned (PR #77) — the cloud's `/sip-ws` bearer-token upgrade is the sole auth layer; provision() best-effort-DELETEs any legacy `<user>-auth` doc to prune pre-fix state. PR #100 dropped the empty-`sip_ha1` guard that was silently skipping every resident in the post-PR-#77 import flow (the `sip_ha1` parameter remains in the signature, ignored). `set_sip_realm()` (PR #24) still swaps the realm string on `SOCIETY_BOOTSTRAP` but the realm is now vestigial — nothing in the provisioned objects references it. **Requires `docker/asterisk/sorcery.conf` mapping `pjsip.aor/endpoint/auth` to the `astdb` wizard** (PR #101) — without it Asterisk's default `config` wizard is read-only and every PUT returns `403 "Cannot create sorcery objects of type 'aor'"`. The compose stack bundle-mounts it transparently. |
| `CloudConnector::OnConnectedHandler` (PR #20) | Glue: wired in `main.cpp` to (1) send `AGENT_HELLO` (PR #24) for realm bootstrap, then (2) call `ari_client.publish_register_snapshot()` so every reconnect re-syncs the cloud's presence cache. |
| `SipFrameDemux::SocietyBootstrapHandler` (PR #24) | Glue: wired to a closure in `main.cpp` that, when CLI `--sip-realm` was not passed, calls `pjsip.set_sip_realm(received)` then `watcher.resync()` so the realm correction is applied to every already-provisioned subscriber's auth object. |
| `ReconnectSupervisor`, `SubscriberWatcherTimer` | Reactor timers (1 s + 200 ms). |

### 2.4 Asterisk (`pbx-asterisk`)

Container: `docker.io/andrius/asterisk:20`. Configs mounted from
`docker/asterisk/`:

- `pjsip.conf` — WS transport (`transport-ws`), `[endpoint-resident-template]` carrying every codec / DTLS / WebRTC field, sample `alice` / `bob` / `conf` endpoints for hand testing. **Production endpoints are not in this file** — they're pushed dynamically by `PjsipProvisioner`.
- `extensions.conf` — Stasis app `pbx` (everything goes through `AriClient` for admission), `pbx-busy` for over-cap rejection, `pbx-conf-room` for conference.
- `ari.conf` — single ARI user `asterisk:asterisk` (override in `.env` for production).
- `http.conf` — listens on `0.0.0.0:8088`, bridged-network scope only.
- DTLS-SRTP keys: `/etc/asterisk/keys/pbx.{crt,key}`, generated by
  `scripts/setup-society.sh` (per-call SDP fingerprint binding, so
  self-signed is fine — see [`DESIGN.md`](./DESIGN.md) §8.1).

The bridge between Asterisk and the agent is two parts:

- **WS bytes plane** — `AsteriskWsFactory` opens a TCP socket to
  `:8088/ws` for each browser stream, pipes bytes through.
- **ARI control plane** — `AriWsClient` keeps one persistent connection
  to `/ari/events` (HTTP-Basic), `AriRestClient` issues per-call
  commands over plain HTTP.

### 2.5 coturn (`pbx-coturn`)

Container: `coturn/coturn:4.6`, **host networking** (TURN needs the
real public IP in STUN replies). Configured with `use-auth-secret`
(RFC 5766 §5) — the cloud's `GET /api/v1/turn-credentials` mints
`username = <unix-expiry>:<sipUsername>`, `password = HMAC-SHA1(secret,
username)`. TTL 300 s. The browser passes these straight into its
`RTCConfiguration.iceServers`.

### 2.6 wsdbagent (`pbx-wsdbagent`)

Standalone wsdbproxy-tunnel binary at `modules/module/wsdbagent/` (verbatim copy of the upstream shared-library module — do not modify locally).
Dials `wss://${CLOUD_HOST}/ws/db` with InnerTLS over the outer WSS;
forwards BSON-framed DB requests to the local `pbx-mongo`. Only
active when the cloud is started with `--remote-db` (D4 — pending
live verification; the source path is in place).

### 2.7 MongoDB (`pbx-mongo`)

Container: `mongo:7`, **`mongod --replSet rs0 --bind_ip_all`**
(single-node replica set). The healthcheck performs an idempotent
`rs.initiate()` so dependent services only start once the RS is up.

Collections (all under DB name `pabx`):

| Collection | Owner | Indexes |
|------------|-------|---------|
| `societies` | cloud `MicroServicePbx` | unique on `code` |
| `flats` | cloud import | unique `(societyId, number)` |
| `subscribers` | cloud import + admin endpoints; **watched by agent's `SubscriberWatcher`** | unique `(societyId, sipUsername)`, unique `(societyId, email)`, by-role |
| `sessions` | cloud login | unique on `token`, TTL on `expiresAt` |
| `cdr` | agent (via `CDR_PUSH`) | by `(societyId, startedAt)` |
| `push_subscriptions` | cloud `PushSender` | by `subscriberId` |
| `audit` | cloud admin actions | append-only |

Replica-set requirement (PR #18) is for the agent: only an oplog
enables Mongo change streams. Standalone mongod degrades the
`SubscriberWatcher` to bootstrap-only (live updates require an agent
restart).

---

## 3. Trace one full call

### 3.1 Agent attach + realm bootstrap (one-time per tunnel)

Before any browser-side activity, the agent has to bring its `/agent`
tunnel up and learn the canonical `sipRealm` for its society. This
runs on every agent (re)connect:

```
Agent                                           Cloud
─────                                           ─────

CloudConnector.attempt_connect
   │
   ▼ AceSslTransport.connect_and_handshake
   │   · ACE_SSL_SOCK_Connector.connect       ─► outer TLS via Heroku's
   │     (mTLS material from --tls-cert etc.,    edge router (router
   │     not actually verified at the dyno —     terminates outer TLS,
   │     Heroku terminates outer TLS)            dyno sees plain HTTP/WS)
   │   · HTTP/1.1 GET /agent Upgrade: websocket ─►  WebConnection sees /agent,
   │     ── 101 Switching Protocols ◄──────────    constructs AgentStream
   │                                                with auto_attach=false
   │   · Build WsInnerTlsBridge (blocking)
   │   · Build InnerTlsClient over the bridge
   │   · set_ca + set_cert (SSL_use_certificate_file
   │     — PR #25 fix; SSL_CTX_use_certificate_file
   │     would inherit-after-SSL_new and silently
   │     present no cert)
   │   · InnerTlsClient.handshake ─────────────►  AgentStream.setup_inner_tls
   │                                                · Build WsInnerTlsBridge (blocking)
   │                                                · Build InnerTlsServer (cert/key/ca
   │                                                  from --tls-cert/-key/-ca,
   │                                                  same triple reused from /ws/db)
   │                                                · InnerTlsServer.accept
   │                                                · Capture peer CN into m_peer_cn
   │                                                  via peer_subject_cn (PR #25)
   │     ── inner-TLS established ◄────────────    ── inner-TLS accepted ──
   │                                                · attach() — TransportAdapter
   │                                                  published to endpoint
   │   · bridge.switch_to_buffered
   │   · m_recv_buf = bridge.leftover_socket_bytes
   │
   ▼ CloudConnector.set_on_connected fires
   │
   ▼ send_frame(AGENT_HELLO, 0,           ─────►  AgentStream.handle_input
   │   {"societyId":"<my-society-id>"})            → SipBridge.set_agent_hello_handler
   │                                                · db.get_document("societies",
   │                                                  {_id: "<my-society-id>"})
   │                                                · Parse sipRealm field
   │                                                · bridge.bootstrap_society(soc, realm)
   │                                                · send_frame(SOCIETY_BOOTSTRAP, 0,
   │                                                    {societyId, sipRealm:"<code>.pbx.local"})
   ▼ AriClient.publish_register_snapshot
   │   · GET /ari/endpoints/PJSIP
   │   · for each endpoint → REGISTER_STATE frame (out)
   │
   ▼ SipFrameDemux.dispatch_frame      ◄────────  SOCIETY_BOOTSTRAP received
       op == SOCIETY_BOOTSTRAP
       → set_society_bootstrap_handler fires
         (in main.cpp; honours --sip-realm CLI override)
         · pjsip.set_sip_realm("<code>.pbx.local")
         · watcher.resync()  ← re-runs full_scan, re-PUTs every
                               auth/aor/endpoint with the corrected
                               realm. Cursor + resume token preserved.
```

After this dance the tunnel is fully operational. The agent's
pjsip-realm now matches what the cloud's society doc carries.

**Post-PR #77 note:** SOCIETY_BOOTSTRAP and `set_sip_realm()` are
preserved for now, but the realm no longer affects subscriber
provisioning — `PjsipProvisioner` stopped emitting digest-auth
objects entirely (digest auth was unanswerable: the browser has no
SIP password). The realm machinery + `--sip-realm` CLI flag are
candidates for a follow-up cleanup PR. The per-browser register
flow in §3.2 happens transport-only — no realm comparison, no
HA1 digest exchange.

### 3.2 Subscriber register

```
Browser                       Cloud                     Agent                      Asterisk
─────────                     ───────                   ────────                   ──────────

LoginComponent: form submit
   │
   ▼ POST /api/v1/subscriber/login
   ──────────────────────────►  handle_subscriber_login_POST
                                  · bcrypt-verify portalPasswordHash
                                  · write a sessions row
                                  · Set-Cookie + JSON body
                                {token, subscriber}
   ◄──────────────────────────

SipService: new WebSocket
"wss://CLOUD/sip-ws?token=…"
   │
   ▼ /sip-ws WS upgrade
   ──────────────────────────►  handle_sipws_upgrade
                                  · resolve token → sessions row
                                  · OPEN-frame meta = {societyId, sipUsername}
                                                          │
                                                          ▼ SipBridge.publish(OPEN)
                                                          │  CloudTunnelEndpoint.send_frame(OPEN, sid, meta)
                                                          │       │
                                                          │       ▼ over inner TLS, outer WS
                                                          │  AceSslTransport.handle_input
                                                          │       ▼ SipFrameDemux.on_tunnel_bytes
                                                          │   AsteriskWsFactory.open(sid, meta)
                                                          │       ▼ TCP+WS upgrade to chan_pjsip
                                                          │   IAsteriskStream
                                                                       ◄────── REGISTER (SIP)
                                                                       ──────► 200 OK         (no auth challenge —
                                                                                              PR #77/#78 dropped
                                                                                              digest from both the
                                                                                              dynamic provisioner
                                                                                              and the static
                                                                                              pjsip.conf endpoints;
                                                                                              cloud /sip-ws bearer
                                                                                              gate is sole auth)
                                                                                    │
                                                                       AriWsClient catches
                                                                       EndpointStateChange
                                                                            │
                                                                            ▼
                                                                       AriClient.handle_endpoint_state_change
                                                                       → REGISTER_STATE SipFrame
                                                                            │
                                                                            ▼ over tunnel
                                                                  CloudTunnelEndpoint receives
                                                                  → SipBridge.set_register_state_handler
                                                                  → IPresenceCache.set(soc, user, true)
                                                                  → directory's `online: true`
```

### 3.3 1:1 call within a flat

`Browser A` (`A-101`, two subscribers) dials `A-204`.

```
Browser A                Cloud                  Agent                   Asterisk
─────────                ───────                ────────                ──────────

SipService.invite()
   ▼ SIP INVITE to A-204
   ────────────► (over /sip-ws) ──► (over tunnel) ──► (chan_pjsip)
                                                              ▼ Stasis(pbx, A-204)
                                                              AriClient.handle_stasis_start
                                                                · classify: fresh caller leg
                                                                · admission: count active bridges < 5? OK
                                                                · CallRouter.resolve("A-204")
                                                                    → ["u_resident_a204_1",
                                                                       "u_resident_a204_2"]
                                                                · for each target:
                                                                    AriRestClient.originate
                                                                       app=pbx
                                                                       appArgs=outbound,<callerChannel>
                                                                       endpoint=PJSIP/<user>
                                                              ▼ Asterisk dials each
Browser B1 / B2 (A-204 subscribers) ring (192 Ringing → tunnel → /sip-ws → sip.js)

Browser B1 answers first
   ▼ SIP 200 OK
   (over tunnel)
   ────────────►                                              ▼ Asterisk re-enters Stasis
                                                              AriClient sees second StasisStart
                                                                · classify: outbound leg of caller X
                                                                · CallRouter.on_leg_start
                                                                    · first-answer wins
                                                                    · create_bridge + add caller + add B1
                                                                    · hangup losers (B2) reason="answered"
                                                              ▼ Bridge built
   ◄─── ACK ─── (back through the tunnel)

RTP/SRTP P2P (or via coturn STUN/TURN):
Browser A ◄════ DTLS handshake / SRTP ════► Browser B1
            (direct, ICE-negotiated)

   ── audio is now off-tunnel ──
```

`AriClient` accumulates per-channel context (start time, callee, etc.)
in `ChannelCtx` during `StasisStart` / `ChannelEnteredBridge`. On
`ChannelDestroyed` it builds the CDR row and ships it via
`CDR_PUSH` SipFrame. Per-leg destroys happen first; the last one
finalises with `hangupCause`.

If `A-204` had a guard role and `--auto-answer` was on, the browser's
`SipService.onIncoming` would auto-accept (PR #12).

### 3.4 Conference

```
Browser A dials *FLAT (or sip:conf@…)
   ▼ INVITE → tunnel → Asterisk
                              ▼ Stasis(pbx-conf-room) — different context
                              ConfBridge() mixer
                              ── direct_media=no → audio stays in Asterisk
                              ── per-leg DTLS-SRTP (each browser handshakes
                                  with Asterisk; Asterisk mixes plaintext)
```

This is the unavoidable property of server-side mixers — see
[`DESIGN.md`](./DESIGN.md) §8.2. We document this to society admins:
**conference audio is encrypted on the wire but visible to the on-prem
PBX process during mixing.** 1:1 audio is never visible to the PBX.

### 3.5 Inbound call when the tab is closed

```
Caller dials our subscriber's flat
   ▼ INVITE → Asterisk
              ▼ Stasis(pbx, <flat>)
              CallRouter.resolve(flat) → [sipUsernames]
              ▼ originate per target
              Asterisk attempts → no registered contact (closed tab)
              ▼ ChannelDestroyed before any answer
              AriClient detects → emits PUSH_NOTIFY SipFrame
                                                       │
                                                       ▼ over tunnel
                                          PushSender.notify(subscriberId, payload)
                                                       │
                                                       ▼ VAPID push to browser
                                          Service Worker fires `notificationclick`
                                          Browser reopens, re-REGISTERs,
                                          INVITE is retried.
```

---

## 4. Failure modes

| Failure | What survives | What recovers automatically | Operator action |
|---------|---------------|------------------------------|-----------------|
| Browser network glitch | Other browsers + on-prem stack | sip.js retries the WSS; cloud sees existing `sessions` row | None |
| Single browser tab close | Other browsers; existing channel times out within Asterisk's INVITE expiry; CDR closed with `hangupCause = "missed"` or `"tunnel_lost"` | Service Worker can wake the tab on PUSH_NOTIFY | None |
| `/sip-ws` WS drop mid-call | RTP/SRTP (P2P, off-tunnel); existing call continues; new ones blocked until re-upgrade | sip.js re-opens; cloud routes new browsers via the same bridge | None |
| `/agent` tunnel drop | RTP/SRTP P2P; existing 1:1 conversations continue; conference legs that already joined stay alive; new INVITEs blocked | `CloudConnector` reconnects (1 s → 30 s backoff); cloud waits for the re-attach; `CloudTunnelEndpoint::on_agent_connected` re-arms `SipBridge.m_tunnel` on every attach (PR #101 — before this fix, one disconnect-reconnect cycle silently broke SIP for the cloud's lifetime); **`publish_register_snapshot()` re-syncs presence cache on reconnect** (PR #20) | None for short flaps; check `pbx-agent` logs for prolonged outages |
| Mongo flap | Existing tunnel + calls (the agent caches subscriber materialisation in Asterisk's astdb-backed sorcery — no Mongo lookup per call. **Requires the `docker/asterisk/sorcery.conf` bundle-mount from PR #101**; without it the sorcery PUTs return 403 and the agent has nothing cached.) | `SubscriberWatcher` catches `try_next` exception, reopens cursor with `resumeAfter(captured_token)` (PR #21); 5-tick backoff between failed reopens; events that fell in the gap replay if still in the oplog window | None for short flaps; if the resume token has aged out an agent restart re-applies all subscriber state via bootstrap |
| Asterisk crash | Cloud's REST + `/agent` tunnel; existing browsers (until their NEXT signalling action) | systemd / podman-compose restart; agent's `AriWsClient` reconnects + `AriClient` re-subscribes | Operator: investigate the crash log |
| Cloud dyno bounce (Heroku) | RTP/SRTP P2P; existing 1:1 calls. Browsers see WS close; agent sees WS close | New dyno comes up in seconds; `CloudConnector` reconnects; browsers re-open on next user action | None for routine deploys |
| Heroku H15 idle drop | Should NOT happen — two complementary keep-alives: (1) `AgentStream` and `BrowserStream` both arm a 25 s outer WS-PING (RFC 6455 `0x9`) per-connection — this is what stops Heroku's 55 s edge timeout. (2) `CloudTunnelTickDriver` ticks `CloudTunnelEndpoint::tick()` every 5 s, driving the inner SipFrame-level heartbeat (PING every 5 s, drop after 3 unanswered = 15 s partition-detection window). The 15 s window is intentionally tight for kiosk deployments — see `docs/design/operations/cloud-tunnel-liveness.md`. | — | If it does happen: check both timers fired (cloud logs) |
| Heroku terminates outer TLS — agent cert is meaningless at the router | InnerTLS-over-WS (PR #19) provides the real mTLS at the application layer | — | Both ends MUST be configured with `inner-tls-*` flags; missing config means the cloud refuses the agent at `setup_inner_tls()` |
| Resume token aged out of oplog | `SubscriberWatcher` opens a fresh-from-now cursor (next reopen attempt's `watch_collection(coll, token)` returns null; the no-token fallback opens cleanly) | Next agent restart's `bootstrap()` re-applies every active subscriber | If long Mongo outage suspected, agent restart is cheap |

For *currently-open* operational issues (things the architecture
doesn't yet handle and that you may run into on a fresh setup), see
[`docs/design/operations/known-issues.md`](docs/design/operations/known-issues.md).
Move resolved entries from there back into this table (or the relevant
design section) as they're fixed.

---

## 5. Operator runbook

### 5.1 Deploy the cloud

One-time setup (the InnerTLS material lives in `certs/innertls/`; the
Dockerfile generates a fresh `server.{crt,key}` signed against the
repo CA at build time):

```sh
# Build the cloud image (amd64 for Heroku).
podman build --platform linux/amd64 -f docker/Dockerfile.cloud \
  -t onprem-pbx-cloud:latest .

# Push + release via the deploy script (tag, heroku container:push,
# heroku container:release, smoke test).
./deploy-heroku.sh
```

Heroku-side env vars to set with `heroku config:set --app pabx`:

| Var | Why |
|-----|-----|
| `DB_URI` | If `REMOTE_DB` is unset, this is the cloud Mongo (Atlas or Heroku add-on). |
| `REMOTE_DB=1` | D4 — flip this only after `pbx-wsdbagent` is verified connected. |
| `VAPID_PUBLIC_KEY` / `VAPID_PRIVATE_KEY_B64` / `VAPID_SUBJECT` | For Web Push. The cloud entrypoint decodes `VAPID_PRIVATE_KEY_B64` to `/tmp/vapid.pem` at startup. **Lifecycle, rotation, recovery → [`docs/design/operations/vapid-keys.md`](docs/design/operations/vapid-keys.md).** |
| `TURN_URL` / `TURN_SHARED_SECRET` | Match the on-prem coturn's `realm` + `static-auth-secret`. |
| `PBX_AUTH_STRICT=1` | Once subscribers are seeded — otherwise login is dev-permissive (returns any non-empty triple). |

### 5.2 Deploy the on-prem agent stack

Per society:

```sh
# 1. Provision the host. Reasonable specs: 2 vCPU, 4 GB RAM, 30 GB disk
#    for the Asterisk + Mongo + container overhead; coturn needs ~10 Mbps
#    sustained for a busy society. Linux box with podman.

# 2. Pull the cert pair (operator-supplied; matches the CA the cloud was
#    built with). Three files required at $CERTS_DIR:
#      agent.crt           — InnerTLS client cert (PR #19)
#      agent.key           — InnerTLS client key
#      cloud-ca.pem        — CA that signed the cloud's server.crt
#    The same triple is reused for the outer TLS dial; override the
#    INNER_TLS_* env vars in .env only if you want a separate cert
#    namespace.

# 3. Render the per-society configs:
./scripts/setup-society.sh --society-id=acme --turn-secret=AUTOGEN \
  --public-ip=203.0.113.42 --turn-port=3478
#   Writes:
#     ./certs/asterisk-dtls/{pbx.crt,pbx.key}  — DTLS-SRTP per-call cert
#     ./certs/turnserver.conf                  — rendered coturn config
#     ./.env.agent.example                     — pre-filled .env template

# 4. Configure runtime env.
cp .env.agent.example .env
$EDITOR .env        # set CLOUD_HOST, AGENT_SOCIETY_ID, CERTS_DIR,
                    # CLOUD_PORT (443), TURN_PUBLIC_PORT, ARI_USER/PASS,
                    # and optionally SIP_REALM, INNER_TLS_HOSTNAME

# 5. Up the stack. The healthcheck on pbx-mongo runs an idempotent
#    rs.initiate() on first start; dependents block via depends_on
#    until the RS is up.
podman-compose -f docker-compose.agent.yml up --build -d

# 6. Verify.
podman logs pbx-agent | tail -20
#   ... AceSslTransport: connected + WS-upgraded
#   ... AceSslTransport: inner-TLS handshake established      (PR #19)
#   ... SubscriberWatcher: bootstrap complete (N rows)        (PR #18)
#   ... AriWsClient: connected to Asterisk ARI
#   ... SOCIETY_BOOTSTRAP: sipRealm <default> -> <code>.pbx.local; re-syncing every subscriber  (PR #24, fires only when --sip-realm not passed AND cloud has the society doc)

heroku logs --tail --app pabx | grep agent
#   ... /agent handed off raw fd N -> AgentStream (inner-TLS) (PR #19)
#   ... inner-TLS handshake established (peer CN=…)          (PR #25)
#   ... CloudTunnelEndpoint: has_agent=true
```

`scripts/lima.sh` (see §6.2a) automates a one-shot verification of
the agent side against the deployed Heroku cloud — useful before a
real on-prem install.

### 5.3 Verify presence reconciliation

After a deliberate agent restart:

```sh
podman restart pbx-agent
# Watch the cloud immediately receive REGISTER_STATE for every endpoint.
heroku logs --tail --app pabx | grep REGISTER_STATE
```

The directory's `online` flags should reflect ground truth within
seconds of the reconnect (the snapshot fires from
`CloudConnector::set_on_connected`, before any other traffic).

### 5.4 Add / remove a subscriber

Subscribers are imported via the cloud admin:

```sh
# CSV import.
curl -X POST 'https://pabx-…herokuapp.com/api/v1/subscriber/import?societyId=acme' \
  -F 'csv=@subscribers.csv'
```

Each `subscribers` insert triggers a Mongo change-stream event on the
agent. `SubscriberWatcher` materialises it as a pjsip endpoint within
~200 ms of the cloud write — no Asterisk restart, no `pjsip reload`.
Removal (`DELETE /api/v1/subscriber/<sipUsername>?societyId=…`) is the
same path in reverse, plus the cloud emits a `SUBSCRIBER_REVOKED`
SipFrame so the agent hangs up that subscriber's live channels.

---

## 6. Developer onboarding

### 6.1 Code layout

```
onprem-pbx/
├── CMakeLists.txt                    # Top-level — builds pbx-agent, pbx-cloud, wsdbagent, offtarget
├── pbx-agent/
│   └── src/
│       ├── main/                     # The agent binary's sources
│       │   ├── main.cpp              # Wires every component into the ACE reactor
│       │   ├── ace_ssl_transport.*   # Outbound mTLS + WS + InnerTLS (PR #19)
│       │   ├── cloud_connector.*     # Reconnect-backoff + heartbeat + on_connected (PR #20)
│       │   ├── sip_frame_demux.*     # Per-stream Asterisk WS socket fan-out
│       │   ├── ari_client.*          # ARI event consumer + snapshot publisher (PR #20)
│       │   ├── ari_rest_client.*     # ARI REST commander (PR #18 added dynamic-config)
│       │   ├── ari_ws_client.*       # /ari/events WS consumer
│       │   ├── asterisk_ws_factory.* # AsteriskWsFactory (concrete IAsteriskStream)
│       │   ├── call_router.*         # Forked-ring driver
│       │   ├── pjsip_provisioner.*   # PR #18 — Mongo subscriber → pjsip sorcery
│       │   └── subscriber_watcher.*  # PR #18 — bootstrap + change-stream tail (PR #21 resume)
│       └── test/                     # GTest sources covered by the offtarget binary
├── modules/module/                   # Shared libraries (inherited from the upstream shared-library where noted)
│   ├── pbx/                          # Cloud-side classes: SipBridge, CloudTunnelEndpoint,
│   │                                 #   AgentStream, BrowserStream, PresenceCache, PushSender,
│   │                                 #   AceHttpsClient, MicroServicePbx
│   ├── http/                         # MessageParser + Http subclass (inherited, refactored)
│   ├── sip/                          # SipParser
│   ├── webservice/                   # WebServer + WebConnection + ./webservice_main.cpp
│   ├── mongodb/                      # MongodbClient (PR #18 + #21 added change-stream support)
│   ├── wsdbproxy/                    # /ws/db cloud side + wsframe
│   ├── wsdbagent/                    # /ws/db on-prem side (verbatim copy of the upstream module — do not modify locally)
│   ├── security/                     # InnerTLS + WsInnerTlsBridge (PR #19) + WebSocketTransport
│   └── email/                        # SMTP client (verbatim copy of the upstream module — do not modify locally)
├── docker/                           # Container assets
│   ├── Dockerfile.test               # Builds offtarget; COPYs docker/asterisk/* (PR #22)
│   ├── Dockerfile.agent              # Runtime image for pbx-agent
│   ├── Dockerfile.cloud              # Runtime image for pbx-cloud (with UI bundle)
│   ├── Dockerfile.wsdbagent          # Runtime image for pbx-wsdbagent
│   ├── Dockerfile.ui                 # Standalone UI nginx image (optional)
│   └── asterisk/                     # pjsip.conf, extensions.conf, ari.conf, etc.
├── docker-compose.{agent,heroku,test}.yml
├── certs/innertls/                   # Shared CA + leaves for InnerTLS
├── ui/                               # Angular 14 softphone
├── docs/design/security/             # threat-model, controls, secrets, innertls, etc.
├── ARCHITECTURE.md                   # This file
├── DESIGN.md                         # Original design rationale
├── PRD.md                            # Product requirements
├── README.md                         # Status + how-to-build/run
└── TDD-PLAN.md                       # Test-driven layer plan
```

### 6.2 Build + run the test suite

The offtarget binary holds every GTest in the repo (Layer 0–4 module
tests, integration tests, drift checks).

```sh
# Full build + test run (sandboxed in podman).
podman-compose -f docker-compose.test.yml build --no-cache offtarget
podman-compose -f docker-compose.test.yml run --rm offtarget
# → 516/519 tests pass; 3 baseline failures (documented in README.md).

# Run just one suite.
podman-compose -f docker-compose.test.yml run --rm offtarget \
  ./offtarget --gtest_filter='SubscriberWatcher.*'
```

The first build seeds an image cache (`pbx-cpp-builder:bootstrap` —
ACE/TAO 7.0.0, mongo-cxx-driver, googletest pre-installed); subsequent
no-cache builds reuse it and complete in ~7 minutes.

### 6.2a End-to-end dry test (Lima VM)

For a real-Linux integration smoke against the deployed Heroku cloud
(skips the macOS-podman-via-QEMU segfault path we hit early on):

```sh
lima start    # or: bash scripts/lima.sh start
# … run the dry test …
lima stop     # pause (containers + VM halted, data preserved)
lima del      # tear the VM down (drops volumes; ~30 GB reclaimed)
```

The script provisions a Lima VM (Apple Virtualization framework,
native arm64 on Apple Silicon — no QEMU), follows the established
C++ toolchain recipe verbatim (apt deps +
ACE/TAO 7.0.0 with `make install ssl=1 INSTALL_PREFIX=…` +
mongo-c-driver 1.19.1 + mongo-cxx-driver v3.6 with
`BSONCXX_POLY_USE_MNMLSTC=1` and `CMAKE_INSTALL_PREFIX=/usr/local`),
builds pbx-agent natively, then runs it for 90 s against the deployed
Heroku cloud and captures the log + any coredump.

Idempotent via sentinel files under `/var/lib/dry-test-*`. Tear down
with `limactl stop -f onprem-pbx-test && limactl delete -f onprem-pbx-test`.

Known caveat as of 2026-05-16: the agent SIGSEGVs ~5 s after the
inner-TLS handshake completes — exact site lost in a corrupt stack
that gdb can't unwind. Tracked in `memory:project_open_backlog` as
"Agent post-handshake crash"; needs an ASan/Valgrind rebuild to
localize. The script still validates the build + outer TLS + WS
upgrade + inner-TLS handshake paths.

### 6.3 Layer model

The TDD plan ([`TDD-PLAN.md`](./TDD-PLAN.md)) lays the build out as
layers; each layer is fully tested before the next starts:

- **Layer 0** — pure parsers (HTTP, SIP, SipFrame).
- **Layer 1** — single-process building blocks: `SipBridge`,
  `MicroServicePbx` REST handlers, `PushSender` VAPID +
  Web Push encryption.
- **Layer 2** — agent state machines: `SipFrameDemux`,
  `CloudConnector`, `AriClient`, `CloudTunnelEndpoint`.
  Plus PR #18: `PjsipProvisioner`, `SubscriberWatcher`. Plus PR #20:
  `AriClient::publish_register_snapshot`.
- **Layer 3** — ACE bindings: `BrowserStream`, `AgentStream`,
  `AceSslTransport`, `AriWsClient`, `HandoffOrdering`,
  `WsInnerTlsBridge` (PR #19), `PjsipTemplateDrift` (PR #22).
- **Layer 4** — production wiring: `pbx-agent` + `pbx-cloud` main
  files, real `AsteriskWsFactory`, real `AriRestClient`, real
  `PushSender` over `AceHttpsClient`, container/compose definitions.

### 6.4 How to add a feature

Same recipe regardless of which layer:

1. **Failing test first.** Pick the layer that owns the new
   behaviour. Write a test in the relevant `test/` directory; expect
   compile failure.
2. **Smallest production change.** Add the method/field/handler to
   make the test compile + pass. Reuse the existing fakes
   (`FakeAriRest`, `TestDb`, `FakeFactory`, `RecorderCursor`, etc.).
3. **Wire into `main.cpp`.** Production glue lives in
   `pbx-agent/src/main/main.cpp` (agent) or
   `modules/module/webservice/src/webservice_main.cpp` (cloud).
4. **Add the CLI flag / env var** if there's runtime config.
   `pbx-agent/src/main/main.cpp` uses `ACE_Get_Opt`;
   `Dockerfile.agent` declares ENV defaults; `docker-compose.agent.yml`
   maps env vars; `pbx-agent/README.md` documents them.
5. **Run the offtarget suite.** Expect green.
6. **Open a PR** following the existing convention (one feature per
   PR, branch named `feat/<slug>` or `chore/<slug>`, commit message
   that explains the WHY).

### 6.5 Touchpoints for common edits

| You want to… | Edit |
|--------------|------|
| Add a new REST endpoint | `modules/module/pbx/src/microservice_pbx.cpp` + register in `dispatch_pbx_routes`. |
| Add a new SipFrame op | `modules/module/pbx/inc/sip_frame.hpp` + `sip_frame.cpp` (op-code, encode/decode, payload doc). Wire emitter on the producing side and handler on the consuming side. Document in `DESIGN.md` §7. |
| Add a new Asterisk endpoint field | Edit **both** `docker/asterisk/pjsip.conf` (`[endpoint-resident-template]`) AND `pbx-agent/src/main/pjsip_provisioner.cpp` (the endpoint PUT). `PjsipTemplateDrift` (PR #22) test will fail noisily if you only edit one side. |
| Change inner-TLS cert layout | `certs/innertls/generate.sh` regenerates everything from scratch. Rebuild both cloud image and agent image. All-or-nothing rollout. |
| Add a new admission rule | `pbx-agent/src/main/ari_client.cpp` `handle_stasis_start`. The 5-call cap lives in `m_active_bridges` + `Config::max_concurrent_calls`. |
| Add a new subscriber-event reaction | `pbx-agent/src/main/subscriber_watcher.cpp` `handle_event`. The resume-token machinery (PR #21) is already handled before dispatch. |
| Change the directory shape | Cloud: `handle_directory_GET` in `microservice_pbx.cpp` (projection is the strip — see PR #14). UI: `DirectoryComponent` + `DirectoryEntry` type. |

---

## 7. Pointers into deeper docs

- **Design rationale + section-by-section breakdown:** [`DESIGN.md`](./DESIGN.md).
- **Security threat model:** [`docs/design/security/threat-model.md`](./docs/design/security/threat-model.md).
- **InnerTLS handshake + cert layout:** [`docs/design/security/innertls.md`](./docs/design/security/innertls.md).
- **DTLS-SRTP for browser media:** [`DESIGN.md`](./DESIGN.md) §8 + [`docs/design/security/media.md`](./docs/design/security/media.md).
- **Cloud REST endpoints (live):** [`README.md`](./README.md) § Cloud-side PBX REST endpoints.
- **Module deep-dives:** `modules/module/<name>/README.md` (each module has one — start with `pbx/README.md` for the cloud-side classes).
- **Restart survival:** [`docs/design/operations/restart-survival.md`](./docs/design/operations/restart-survival.md).
- **TDD layer-by-layer plan:** [`TDD-PLAN.md`](./TDD-PLAN.md).
