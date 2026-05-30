# onprem-pbx

VoIP PBX for residential societies. Heroku-hosted control plane (C++ / ACE) + on-prem `pbx-agent` (C++ / ACE + Asterisk + coturn + MongoDB) + Angular web softphone (SIP.js + WebRTC) + React Native mobile softphone (iOS / Android, end-to-end runnable for outbound + foreground inbound calls).

**Live**: <https://pabx-5fbf3550f938.herokuapp.com/webui/> — single Heroku app (`pabx`) serving UI at `/webui/` + REST + SIP-over-WS + `/agent` mTLS tunnel. UI assets at `/webui/main.js`, `/webui/favicon.svg`, `/webui/sw.js`. SPA login is dev-mode permissive — any non-empty `societyCode` / `flatNumber` / `password` returns a synthetic session until the CSV-import flow + `PBX_AUTH_STRICT=1` are wired.

See:

- [SALES.md](./SALES.md) — non-technical pitch for societies / installers / decision-makers. Honest pros + cons, cost comparison vs commercial intercoms.
- [PRD.md](./PRD.md) — product requirements, personas, success metrics.
- [DESIGN.md](./DESIGN.md) — architecture, components, data model, call flows, media security.
- [TDD-PLAN.md](./TDD-PLAN.md) — test layers and implementation order.
- [docs/design/security/](./docs/design/security/) — threat model, auth, controls, media, secrets.

## Status

| Layer | What | Status |
|---|---|---|
| 0.b | `MessageParser` base + `Http` subclass refactor + `Sip` subclass | ✅ Complete (commit `f45b40a`) |
| 0.a | `SipFrame` wire-format primitives | ✅ Complete (commit `f45b40a`) |
| 1   | `SipBridge` cloud-side multiplexer | ✅ Complete (first slice) |
| 1   | Shared-library modules — webservice, mongodb, wsdbproxy, security, email, thirdparty | ✅ Verbatim copy (do not modify locally); regression-guard tests green |
| 1   | `MicroServicePbx*` REST handlers (society, subscriber-import, cdr, push, sipws-upgrade) | ✅ Complete |
| 1   | `PushSender*` — VAPID JWT (RFC 8292) + Web Push encryption (RFC 8291) + retry/410 | ✅ Complete |
| 1   | Route wiring (`MicroService::dispatch_pbx_routes`) + `/sip-ws` upgrade auth gate in `WebConnection` | ✅ Complete |
| 2   | `SipFrameDemux` — agent-side mirror of `SipBridge`, demuxes cloud tunnel ↔ per-stream Asterisk sockets | ✅ Complete |
| 2   | `CloudConnector` — agent-side mTLS dial-out, reconnect-backoff state machine, outbound buffer | ✅ Complete (state machine; `AceSslTransport` deferred to Layer 3) |
| 2   | `AriClient` — ARI event consumer, bridge-counter admission, CDR writer, conference detection | ✅ Complete (state machine; WS subscription glue deferred to Layer 3) |
| 2   | `CloudTunnelEndpoint` — accept-side of the cloud↔agent tunnel; wired into `WebServer`; `/agent` WS upgrade branch in `WebConnection`; context-aware `/sip-ws` 503 hint | ✅ Complete (`BrowserStream` + agent WS decoder deferred to Layer 3) |
| 3   | `TunnelE2E` — paired in-memory transport harness, end-to-end frame plumbing tests (browser ↔ cloud bridge ↔ tunnel ↔ agent demux ↔ fake Asterisk and back) | ✅ Complete |
| 3   | `BrowserStream` — first concrete ACE binding (ACE_Event_Handler + BrowserSink). Retires the `/sip-ws` 503 stub: real WS hand-off now lives | ✅ Complete |
| 3   | `AgentStream` — cloud-side ACE binding for `/agent` (ACE_Event_Handler + private TransportAdapter implementing IAgentTransport). Retires the `/agent` stub | ✅ Complete |
| 3   | `AceSslTransport` + factory — agent-side outbound mTLS + WS upgrade dial. Concrete `ITransport` for `CloudConnector`'s factory | ✅ Complete |
| 3   | `AriWsClient` — plain-TCP WS client for Asterisk `/ari/events`; HTTP Basic auth; pushes each JSON event into `AriClient::on_event` | ✅ Complete |
| 3   | `HandoffOrdering` source-invariant test — guards the well-known `remove_handler → m_handle=INVALID → publish` ordering invariant for all 3 WS upgrade branches (`/sip-ws`, `/agent`, `/ws/db`) | ✅ Complete |
| 4   | Production wiring — `pbx-agent` + `pbx-cloud` binaries, `AsteriskWsFactory` (real chan_pjsip WS), `AriRestClient` (admission `continue`), real `PushSender` (AceHttpsClient + VAPID), `docker-compose.{agent,heroku}.yml`, `Dockerfile.{agent,cloud,ui}`, `deploy-heroku.sh` | ✅ Complete (see [§ Layer 4 detail](#layer-4--production-wiring-complete)) |
| 4   | Cloud REST handlers — `subscriber/login` (dev-mode), `subscriber` directory, `push-vapid-key`, `turn-credentials`, `push-subscribe`. `db_available()` env-var guard short-circuits DB-touching handlers when Mongo isn't configured (defends against Heroku H12 timeouts) | ✅ Complete |
| 4   | D1+D2: `wsdbagent` on-prem — verbatim copy of the upstream shared-library standalone DB-tunnel binary (do not modify locally) + new `pbx-wsdbagent` compose service. Dials `wss://${CLOUD_HOST}/ws/db` with ACE InnerTLS on top of the outer WSS (Heroku terminates outer TLS; inner TLS is the real trust boundary). Mongo DB name is `pabx`. | ✅ Verified live in Lima dry-test (PR #35) — `inner TLS established` ↔ `session started` |
| 4   | D3: InnerTLS over the `/agent` SIP tunnel — shared `WsInnerTlsBridge` (dual-mode `IInnerTlsTransport`); agent `--inner-tls-{cert,key,ca,hostname}`; cloud reuses `--tls-{cert,key,ca}` for both `/ws/db` and `/agent`. `AgentStream` split into `(ctor, auto_attach=false)` + `setup_inner_tls()` + `attach()` so the handshake completes before the endpoint sees a live transport. | ✅ PR #19 (merged) |
| 4   | Dynamic pjsip provisioning — `PjsipProvisioner` materialises subscriber rows as Asterisk aor + endpoint sorcery objects via ARI dynamic-config PUT/DELETE; `SubscriberWatcher` does a society-scoped bootstrap full-scan + Mongo change-stream tail at 200ms cadence. `pbx-mongo` runs as a 1-node replica set with idempotent `rs.initiate()` healthcheck. Agent `--sip-realm` flag (default `<society-id>.pbx.local`). SIP digest auth dropped (PR #77 — browser carries no SIP password); empty-`sip_ha1` skip gate dropped (PR #100); Asterisk `sorcery.conf` bundle-mount maps the types to `astdb,<family>` (PR #101 — without it ARI PUTs return 403 + provisioning silently fails). | ✅ PRs #18 / #77 / #100 / #101 (all merged) |
| 4   | Presence reconciliation — `AriClient::publish_register_snapshot()` calls `GET /ari/endpoints/PJSIP` and emits one `REGISTER_STATE` SipFrame per endpoint; `CloudConnector::set_on_connected()` fires it on every (re)connect. Closes the cache-staleness gap from PR #16. | ✅ PR #20 (merged) |
| 4   | Change-stream resume — `IMongodbClient::watch_collection(coll, resume_token_json)` overload + `mongocxx::options::change_stream::resume_after()`; `SubscriberWatcher` captures every event's `_id` token and reopens on `try_next` exception with a 5-tick backoff (~1s at 200ms cadence). | ✅ PR #21 (merged) |
| 4   | Pjsip drift-check test — parses `[endpoint-resident-template]` from `docker/asterisk/pjsip.conf`, runs `PjsipProvisioner` against a FakeAriRest, asserts every template field is present in the emitted endpoint PUT. Prevents silent divergence between dev-fixture endpoints and runtime-provisioned subscribers. | ✅ PR #22 (merged) |
| 4   | Realm-from-society-doc — agent sends `AGENT_HELLO {societyId}` after every (re)connect; cloud's `SipBridge` looks up `societies/{_id}` and emits `SOCIETY_BOOTSTRAP {societyId, sipRealm}`; agent's `PjsipProvisioner.set_sip_realm()` + `SubscriberWatcher.resync()` re-PUTs every endpoint with the canonical realm. CLI `--sip-realm` still overrides. | ✅ PR #24 (merged) |
| 4   | InnerTLS peer-cert CN exposure + latent-bug fix — `InnerTlsServer::peer_subject_cn()` extracts the agent's verified CN for log labels + future cross-checks; the underlying `SSL_CTX_use_certificate_file`-after-`SSL_new` bug (silently presented no cert) replaced with the per-SSL `SSL_use_certificate_file` API. | ✅ PR #25 (merged) |
| 4   | `scripts/lima.sh` — one-shot Lima-VM dry test harness. Provisions an arm64 Ubuntu VM (Apple Virtualization framework, no QEMU), builds pbx-agent following the established C++ toolchain recipe verbatim, runs against the deployed Heroku cloud, captures any coredump. | ✅ PR #26 (merged) |
| UI  | Angular 14 + Clarity softphone — 7 slices: scaffold → login + `AuthGuard` → `SipService` (sip.js seam) → directory + outbound call → inbound + ringtone + Web Push + Service Worker → conference + history + settings + `DeviceService` → `Dockerfile.ui` (nginx) → Playwright E2E | ✅ Complete (see [`ui/README.md`](./ui/README.md)) |
| Mob | React Native mobile softphone (`mobile/`) — TDD layers M0 (scaffold) → M1 (auth) → M2 (outbound + sip.js `UserAgent` engine sharing the cloud softphone's wrapper via `shared/sip-ua/`) → M3.a (push payload + device registration) → M3.b (incoming-call glue + `SipInboundBridge` + `IncomingCallOverlay` foreground ring UI with `Vibration` alerter) → App-shell auth-aware sip env wiring → sign-out control → guard kiosk auto-answer (DESIGN.md §9) → directory tap-to-call (matches web `DirectoryComponent`) → call history tap-to-redial (matches web `HistoryComponent`) → registration status badge (matches web Dashboard). 24 Jest files / 209 tests green via `docker/Dockerfile.mobile-test`. **End-to-end runnable** for outbound + foreground inbound calls from an iOS Simulator build (one-time `react-native init` for the `ios/` shell is the only remaining dev-machine step). Remaining: real CallKit / PushKit / ConnectionService / FCM for wake-up from background, M4 Detox, M5 device matrix. | ✅ M0–M3.b + sip engine + App shell + sign-out + ring-vibration + autoAnswer + directory + history + reg-badge landed (see [`mobile/README.md`](./mobile/README.md)) |
| CI/CD | `publish-images.yml` workflow auto-deploys `pbx-cloud` to Heroku on every same-path push to main: builds amd64 image, pushes to `registry.heroku.com/pabx/web`, calls `heroku container:release`. Replaces the manual `./deploy-heroku.sh deploy` round-trip; the script stays as an escape hatch for emergency push from a laptop. | ✅ PR #95 |
| CI/CD | `concurrency.cancel-in-progress: true` on the workflow — a newer push auto-cancels older in-flight runs for the same ref. Saves CI minutes; bundles back-to-back merges into one CI cycle (e.g. PR #100 + PR #101 → single run, single deploy). Plus three test gates that block image publish + Heroku release: `offtarget` GTest (PR #96), `mobile-test` Jest + `tsc --noEmit` (PR #155 + #156), and `ui-test` Angular `ng build` (PR #157). All three also run on every `pull_request` against `main` (PR #155) so failures show up at PR-review time, not after main has advanced. Plus `LM_INFO`/`LM_WARNING` re-added to the cloud's `priority_mask` so the observability lines from PR #80 + PR #88 actually emit. | ✅ PR #96 (+ #155/#156/#157 for PR gates) |
| Op   | `install.sh` Mongo seeding — after the compose stack is up, seeds the on-prem Mongo with the society's `societies` row + first ADMIN subscriber (PBKDF2-SHA256 hashed locally, never sent over the wire). Without this the cloud's login returns 401 "Invalid credentials" for everything in strict-auth mode. | ✅ PR #96 |
| Op   | Windows install path documented + `onprem-pbx-installer` container (DooD pattern) — one `docker run` from PowerShell brings the stack up; talks to the host's Docker daemon via the mounted socket. Bundles `scripts/installer-entrypoint.sh` + all per-society compose + cert tooling. Linux operators can also use it as an alternative to `install.sh`. | ✅ PR #97 (docs + container) |
| Op   | Docker Hub repo descriptions auto-synced from [`docker/dockerhub-descriptions/*.md`](./docker/dockerhub-descriptions/) on every image publish — Hub "Overview" tab for each image always reflects current behaviour. Marks `pbx-cpp-builder` as INTERNAL so operators don't accidentally pull it. | ✅ PR #99 |
| Op   | XLSX subscriber import template with YELLOW-highlighted mandatory column headers — operator-friendly first-import experience. Lives at `docs/subscribers-template.xlsx`; generator at `scripts/generate-subscriber-template.py`. | ✅ PR #94 |
| Op   | `PjsipProvisioner` empty-`sip_ha1` skip gate removed (silently dropped every resident since PR #77's import flow legitimately leaves `sipHa1` empty); `CloudTunnelEndpoint::on_agent_connected` re-arms `SipBridge.m_tunnel` after every reconnect (without this, ONE Heroku-idle WS drop on `/agent` permanently broke SIP for the cloud's lifetime); `docker/asterisk/sorcery.conf` maps PJSIP types to `astdb,<family>` so ARI dynamic-config PUTs persist (without it Asterisk returns 403 and the provisioner silently writes nothing). All three are required for REGISTER to reach Asterisk — caught live 2026-05-17. | ✅ PR #100 + PR #101 |
| UI  | Auto-Connect on dashboard land — `DashboardComponent.ngOnInit` calls `sip.connect()` so login = online (consumer-app norm); no manual CONNECT click. `SipService.connect()` is idempotent so re-navigation is a no-op. | ✅ PR #108 |
| 3   | Cloud `/agent` tunnel liveness — `CloudTunnelTickDriver` drives `CloudTunnelEndpoint::tick()` (extracted from an untested inline class, +6 tests); heartbeat tuned to 10 s × 5-missed = 50 s partition window (PR #106's 15 s was too tight for Heroku latency jitter — false-positived mid-REGISTER and dropped the tunnel). | ✅ PR #106 + PR #110 |
| 3   | Agent ARI WebSocket `subscribeAll=true` — without it Asterisk delivers zero events to the app; broke presence + call routing. | ✅ PR #104 / PR #105 |
| 4   | **SIP REGISTER works end-to-end** — `SipFrameDemux::m_tunnel` re-armed on agent reconnect (`CloudConnector::OnConnectedHandler` → `demux.set_tunnel(&connector)`). Before this, one reconnect silently dropped every Asterisk→browser reply (incl. REGISTER `200 OK`) → infinite "Connecting…". Mirror of PR #101's cloud-side `SipBridge` re-arm; caught via the PR #111 reverse-path trace. | ✅ PR #112 |
| 4   | Conference (`JOIN CONFERENCE`) — `AriClient::handle_conference_join` create-or-attaches the society's shared mixing bridge `<society>-conf` + adds the caller channel. `create_bridge` `409 Conflict` (bridge already exists) is treated as success so the 2nd+ joiner is not hung up. | ✅ PR #113 + PR #115 |
| 4   | Directory presence — `AriClient` handles the `ContactStatusChange` ARI event (PJSIP contact lifecycle) → `REGISTER_STATE` SipFrame → cloud `IPresenceCache`. `EndpointStateChange` alone was insufficient: AORs are provisioned `qualify_frequency=0`, so a plain REGISTER never moved endpoint state and every flat showed Offline. | ✅ PR #114 |

### Test totals: **600 / 600** C++ + UI karma 61 + UI Playwright 12 + **150** mobile Jest (no baseline failures — see [Skipped tests](#skipped-tests))

| Layer | Suites | Tests |
|-------|--------|------:|
| 0     | HttpParser 20 + MessageParserBase 8 + SipParser 17 + SipFrame 10 | **55** |
| 1     | SipBridge 14 + MicroServicePbx 25 + MicroServiceRouting 9 + PushSender 8 | **56** |
| 2     | SipFrameDemux 19 + CloudConnector 19 + AriClient 16 + CloudTunnelEndpoint 12 + CallRouter 17 + PjsipProvisioner 8 + SubscriberWatcher 17 | **108** |
| 3     | TunnelE2E 8 + BrowserStream 9 + AgentStream 10 + AceSslTransport 10 + AriWsClient 14 + HandoffOrdering 8 + WsInnerTlsBridge 11 + PjsipTemplateDrift 1 + InnerTLS peer-cn 2 | **73** |
| 4     | AsteriskWsFactory 13 + AriRestClient 16 + AceHttpsClient 13 | **42** |
| 4+    | SipBridge AGENT_HELLO/BOOTSTRAP 4 + SipFrame round-trip 2 (new ops) | **6** |
| regression | inherited shared-library suites (verbatim copy) | **115** |
| **Total** | **44 suites** | **518** |

Counts will drift over time — `podman-compose -f docker-compose.test.yml run --rm offtarget` prints the authoritative number on every run.

| UI suite | Tests |
|----------|------:|
| karma (Jasmine) — `AuthService`, `AuthGuard`, `AuthInterceptor`, `LoginComponent`, `SipService`, `RingtoneService`, `PushService`, `HistoryComponent`, `DirectoryComponent` | **61** |
| Playwright E2E — login, dashboard, directory, history, settings | **12** |
| **UI total** | **73** |

**The MVP is feature-complete end-to-end and live.** Cloud + agent control plane has real ACE bindings; the on-prem stack composes a working Asterisk/coturn/Mongo/agent topology; the Angular softphone can register over SIP-over-WS, dial directory entries, host inbound calls with VAPID push wakeup, and join a society ConfBridge. The cloud image (UI bundled) is deployed to Heroku at `pabx-5fbf3550f938.herokuapp.com`, and an on-prem agent dialing from `podman-compose -f docker-compose.agent.yml` has been verified live (cloud logs `GET /agent HTTP/1.1 Upgrade: websocket` ↔ agent logs `AceSslTransport: connected + WS-upgraded`).

### Cloud-side PBX REST endpoints (live)

| Method + Path | Behaviour |
|---|---|
| `POST /api/v1/subscriber/login` | Dev-mode permissive auth — accepts any `{societyCode, flatNumber, password}` triple, returns `{token, subscriber}`. Strict mode (set `PBX_AUTH_STRICT=1`) reserved for the bcrypt path once subscribers are seeded. |
| `GET /api/v1/subscriber?societyId=…&flatPrefix=…` | Society-scoped directory. Returns `[]` when Mongo is unconfigured (see `db_available()` guard). |
| `GET /api/v1/cdr?societyId=…` | Society-scoped CDR list. Same DB guard. |
| `GET /api/v1/push-vapid-key` | Returns `{key: $VAPID_PUBLIC_KEY}`. |
| `GET /api/v1/turn-credentials` | Mints RFC 5766 §5 HMAC-SHA1 creds from `$TURN_SHARED_SECRET` / `$TURN_URL`. TTL 300 s. |
| `POST /api/v1/push-subscribe` (or legacy `/push/subscribe`) | Persists a Web Push subscription. 503 when Mongo unconfigured. |
| `POST /api/v1/society` | Create a society. Existing handler. |
| `POST /api/v1/subscriber/import` | CSV import. Existing handler. |

## Architecture

```
 Heroku cloud  (C++ / ACE, app pabx)            pbx-agent  (C++ / ACE) — on society LAN
 ──────────────────────────────────             ──────────────────────────────────────

 WebServer
  ├─ WebConnection  (one per inbound socket)
  │   ├─ /sip-ws upgrade ─► resolve sessions token ─► BrowserStream
  │   ├─ /agent  upgrade ─► AgentStream
  │   ├─ /ws/db  upgrade ─► WsDbServer                 (shared library, unchanged)
  │   └─ REST / portal   ─► MicroService ─► dispatch_pbx_routes ─► MicroServicePbx
  │                            · login (strict): bcrypt-verify, then write a
  │                              sessions row and set the session cookie
  │                            · /sip-ws upgrade: resolve the token in
  │                              sessions → subscriber identity → OPEN meta
  ├─ SipBridge ◄───────────── TunnelSink ──────────► CloudTunnelEndpoint
  │   ├─ set_push_notify_handler ─► PushSender::notify  (VAPID Web Push)
  │   └─ set_cdr_push_handler    ─► Mongo CDR writer
  │
  └─ MongoDB ── subscribers · sessions · cdr · push_subscriptions
        └── wsdbproxy ── (WSS + InnerTLS) ── wsdbagent ── on-prem Mongo


   AgentStream ◄═══════════════════════════════════════►  AceSslTransport
                outer WSS (Heroku-terminated, theatre)            ▲    │
                + InnerTLS-over-WS (real mTLS, PR #19)            │    ▼
                · WS-level keep-alive ping (idle H15 guard)       │    │
                · SipFrame PING/PONG heartbeat (peer liveness)    │    │
                                                              CloudConnector
                                                                  ▲    │
                                                                  │    └─► on_connected →
                                                                  │       AriClient.publish_register_snapshot()
                                       upstream send_frame(…)     │       (PR #20: REGISTER_STATE × N)
                                                              SipFrameDemux
                                                                  │    ▲
                                                                  ▼    │
                                                              IAsteriskFactory
                                                                  │
                                                                  └─► Asterisk WS
                                                                      (chan_pjsip,
                                                                       ws://127.0.0.1:8088)

                                                              AriClient ◄── AriWsClient
                                                               ├─ admission bridge-counter
                                                               ├─ CDR ─► on-prem Mongo
                                                               ├─ PUSH_NOTIFY ─► CloudConnector
                                                               ├─ REGISTER_STATE on EndpointStateChange
                                                               │  (PR #16, dispatched via CloudConnector)
                                                               └─ revoke_subscriber on SUBSCRIBER_REVOKED

                                                              SubscriberWatcher ◄── Mongo `subscribers`
                                                               │                  (change-stream tail,
                                                               │                   resume-token-aware, PR #21)
                                                               ▼
                                                              PjsipProvisioner
                                                               │
                                                               ▼ ARI dynamic-config PUT/DELETE
                                                              Asterisk pjsip sorcery
                                                               (auth + aor + endpoint per subscriber)
```

### How traffic flows

**Portal login → SIP-WS session:**
`Browser POST /api/v1/subscriber/login → MicroServicePbx.handle_subscriber_login_POST (strict mode: bcrypt-verify against the subscribers collection) → writes a sessions row + Set-Cookie → UI opens wss://…/sip-ws?token=… → handle_sipws_upgrade resolves the token in sessions → BrowserStream gets {societyId, sipUsername, clientUA} as its OPEN-frame meta`

**Browser INVITE → Asterisk:**
`Browser WSS → /sip-ws upgrade → BrowserStream.handle_input → SipBridge.on_browser_data → CloudTunnelEndpoint.send_frame → AgentStream WS → AceSslTransport ← (wire) → CloudConnector.on_bytes_received → SipFrameDemux → local Asterisk WS`

**Asterisk reply / events → Browser:**
`Asterisk WS → SipFrameDemux.on_asterisk_data → CloudConnector.send_frame → AceSslTransport (wire) → AgentStream.handle_input → CloudTunnelEndpoint.on_bytes_received → SipBridge → BrowserStream.send_bytes → Browser WSS`

**Asterisk hangup → Mongo CDR + push:**
`Asterisk → AriWsClient → AriClient.on_event(ChannelDestroyed) → CDR written locally + PUSH_NOTIFY frame → CloudConnector → AgentStream → SipBridge → bridge.cdr_push_handler / push_notify_handler hooks`

**Tunnel keep-alive (two independent layers):** `AgentStream` / `BrowserStream` send a WebSocket-level ping every 25 s so Heroku's router doesn't H15-drop an idle socket; underneath that, the agent's `CloudConnector` sends a SipFrame `PING` after 15 s of inbound silence and drops + reconnects the tunnel once 3 go unanswered (detects a hung-but-not-closed peer).

### Skipped tests

**No** tests are filtered out — `docker/Dockerfile.test`'s default CMD runs the full suite. Current full-suite result: **600 PASSED / 0 FAILED / 0 SKIPPED**.

Override the filter to include them once a Mongo fixture is wired up:

**Recently retired** — the filter used to exclude:

- `WsDbServer.SecondAgentRejected_When_FirstAlive` — the test asserted the server sent **409 Conflict** to the second agent, but production `on_agent_connected` now evicts the (stale) first agent via `::shutdown(SHUT_RDWR)` and tells the second to back off with **503 + Retry-After: 2**. Test now asserts the new contract (and that the first agent's socket got SHUT_RDWR'd); the `.hpp` docstring updated to match.
- `AccountLoginTest.ValidCredentials_Returns200WithAccountData` and `AccountLoginTest.ResponseBody_ExcludesSensitiveFields` — both were mislabelled as "needs live Mongo" but the underlying bug was a test-fixture schema mismatch: the JSON fixtures seeded `passwordHash` at the **top level** of the account doc, but `handle_account_login_POST` reads it from `loginCredentials.passwordHash` (the same path `migrate_account_passwords` writes to). Nested the hash; both tests pass against the existing `MockMongodbClient`.
- `SeedDataTest.BootstrapAdminHasHashedPassword` — the xpmile-inherited test read `/src/docker/mongo-init.js` which onprem-pbx doesn't ship; recast to read `/src/install.sh`'s `db.subscribers.replaceOne(...)` admin-seed block instead, asserting it uses `portalPasswordHash` + `$pbkdf2-sha256$` modular-crypt form and never inlines `$ADMIN_PASSWORD`.
- `MicroServiceRouting.RoutesSubscriberImportTemplateGET` (the POST half hit the admin-only `/import` route without a session token); PR #141 fixed the test by seeding an admin session + adding `?token=admin-tok` to the URL, the same pattern as the working `RoutesSubscriberImportPost`.
PR #140 separately closed the `InnerTlsTest.*` skip-coverage gap by
generating fresh test keys inside the image at build time (the keys
are gitignored, so a clean-checkout build used to skip all 10
handshake tests silently).

## Build & run

All container operations use **podman** (not docker).

```sh
# Build the test image. Reuses the cached `pbx-cpp-builder:bootstrap`
# image (a tagged snapshot of the cpp-builder stage with ACE/TAO 7.0.0,
# googletest, mongo-cxx-driver, and OpenSSL already installed under /usr/local).
# Build time: ~60 s.
podman build -f docker/Dockerfile.test -t onprem-pbx-test:latest .

# Run all GTest suites (3 inherited shared-library tests skipped by default — see
# §Skipped tests below).
podman run --rm onprem-pbx-test:latest

# Filter to a specific suite (override the entrypoint to pass flags).
podman run --rm --entrypoint ./offtarget onprem-pbx-test:latest \
  --gtest_filter='SipBridge.*:TunnelE2E.*'

# Filter by layer (rough — every suite name maps to a layer in the totals
# table above):
podman run --rm --entrypoint ./offtarget onprem-pbx-test:latest \
  --gtest_filter='HandoffOrdering.*:BrowserStream.*:AgentStream.*:AceSslTransport.*:AriWsClient.*:TunnelE2E.*'
```

If `pbx-cpp-builder:bootstrap` is not present locally (fresh machine), the
from-scratch Dockerfile (full cpp-builder recipe) is at
[`docker/Dockerfile.test`](./docker/Dockerfile.test) and rebuilds the
toolchain in ~30 min.

## Run the on-prem agent stack

> **For a guided end-to-end overview** (one-call trace, failure modes,
> operator runbook, developer onboarding) see
> [`ARCHITECTURE.md`](./ARCHITECTURE.md). This section is the abbreviated
> "get it up" checklist.

The on-prem deployment runs six containers — `pbx-mongo`, `pbx-asterisk`,
`pbx-coturn`, `pbx-agent`, and `pbx-wsdbagent` — composed by
`docker-compose.agent.yml`. `pbx-mongo` runs as a single-node replica set
(`mongod --replSet rs0`) so the agent's `SubscriberWatcher` can tail the
`subscribers` collection's change stream; the healthcheck doubles as an
idempotent `rs.initiate()` so dependent services only start once the RS is
up. On a standalone mongod the bootstrap full-scan still runs but live
change-stream updates fall back to no-op (logged).

```sh
# One-time: copy the env template and fill in real values
# (CLOUD_HOST, AGENT_SOCIETY_ID, CERTS_DIR pointing at agent.crt/key + cloud-ca.pem).
# The CERTS_DIR cert pair is reused for the outer TLS dial AND the inner
# TLS handshake (PR #19) — same trust boundary as /ws/db. Override
# INNER_TLS_* env vars only if you want a separate cert namespace.
cp .env.agent.example .env
$EDITOR .env

# Bring the stack up (builds pbx-agent from docker/Dockerfile.agent on first run).
podman-compose -f docker-compose.agent.yml up --build -d

# Tail logs.
podman-compose -f docker-compose.agent.yml logs -f pbx-agent

# Tear down (keeps the mongo data volume).
podman-compose -f docker-compose.agent.yml down

# Tear down + drop the mongo volume.
podman-compose -f docker-compose.agent.yml down -v
```

Topology — six containers, each with a single responsibility:

| Container | Image / Build | Network | Role | Why on-prem |
|---|---|---|---|---|
| `pbx-mongo` | `mongo:7` (RS mode) | `pbx-net` | Persistent store: `subscribers`, `sessions`, `cdr`, `push_subscriptions`. RS mode (`--replSet rs0`) is required so `SubscriberWatcher` can tail change streams; healthcheck doubles as an idempotent `rs.initiate()`. | Subscriber PII stays on the society LAN. |
| `pbx-asterisk` | `andrius/asterisk:20` | `pbx-net` | The SIP+RTP server. chan_pjsip WS on `:8088` (browsers); ARI on `/ari/*` (control plane); ConfBridge (conferences). Not host-exposed — auth is SIP digest, not WS-layer. | All voice media stays on-prem. |
| `pbx-coturn` | `coturn/coturn:4.6` | **host** (not bridge) | TURN relay for off-LAN browsers (1:1 calls where one leg is outside the society LAN). Host networking so STUN replies carry the real public IP, not the bridge's. The society DNATs one public UDP port (`TURN_PUBLIC_PORT`, default 3478) to it. | TURN needs a public UDP path. |
| `pbx-agent`  | built from `docker/Dockerfile.agent` | `pbx-net` | The control-plane glue. Dials Heroku's `/agent` over outer TLS + InnerTLS (PR #19, mTLS — `CERTS_DIR` provides cert/key/CA for both layers). Runs `SubscriberWatcher` + `PjsipProvisioner` (PR #18, pushes endpoints into Asterisk via ARI dynamic-config), `AriClient` (admission, CDR, presence), `CallRouter` (forked-ring). | Bridges cloud signaling ↔ local Asterisk. |
| `pbx-wsdbagent` | built from `docker/Dockerfile.wsdbagent` | `pbx-net` | DB tunnel. Dials `wss://${CLOUD_HOST}/ws/db` with InnerTLS so the cloud's `MicroServicePbx` handlers (login, directory, turn-credentials) can read/write the on-prem Mongo when Heroku has `REMOTE_DB=1`. Without it the cloud's login returns **503 "On-prem agent not connected"** instead of stalling. | Mongo stays on-prem; cloud reads via this tunnel. |
| `pbx-cert-watcher` | `alpine:3.19` + runtime `apk add curl` | host podman socket (no pbx-net) | Polls `./certs/cloud-issued/` every 5 s. When `./deploy-heroku.sh deploy` extracts a fresh cert family (the cloud regenerates its CA per build), the watcher detects the md5 change and POSTs to the host podman socket to restart `pbx-wsdbagent` + `pbx-agent` so they re-read the new certs. Eliminates the manual `podman restart` step after every deploy. | Self-healing CA-rotation; no operator action needed. |

The six share `pbx-net` (except `pbx-coturn` host-net + `pbx-cert-watcher` which only needs the podman socket). Compose dependency order: `pbx-mongo` healthy → `pbx-agent` + `pbx-wsdbagent` start; `pbx-cert-watcher` waits on both peers before arming the poll; `pbx-asterisk` is independent (agent waits on it via ARI retry, not compose); `pbx-coturn` is fully independent.

Asterisk config files (`docker/asterisk/*.conf`) are minimal but functional:

- `pjsip.conf` — WS transport, WebRTC-enabled endpoint template (`webrtc=yes`, `auth_type=md5`, `realm=pbx.local`), sample `alice` / `bob` / `conf` endpoints. **Production endpoints are provisioned dynamically by `PjsipProvisioner`** — see [§ Dynamic pjsip provisioning](#dynamic-pjsip-provisioning-pr-18) below. The drift-check test (PR #22) guarantees the dev-fixture template and the dynamic-config endpoint fields stay aligned.
- `extensions.conf` — Stasis app `pbx` (admission gate) + `pbx-busy` busy-signal context for over-cap calls.
- `ari.conf` — single ARI user `asterisk:asterisk`; **override via `ARI_USER` / `ARI_PASS`** in `.env` before any non-local deployment.
- `http.conf` — listens on `0.0.0.0:8088`, scoped to the internal bridge network.

coturn (`docker/coturn/turnserver.conf`) uses `use-auth-secret` so the cloud's `GET /api/v1/turn-credentials` endpoint can mint time-limited credentials with a shared HMAC-SHA1 secret (RFC 5766 §5). The bundled `static-auth-secret` is dev-only — overwrite before production.

### Dynamic pjsip provisioning (PR #18; PRs #77/#100/#101 finished the story)

The agent doesn't pre-list endpoints in `pjsip.conf`. On boot
`SubscriberWatcher::bootstrap()` does a society-scoped full-scan of the
`subscribers` collection and pushes each active row into Asterisk as
**two** sorcery objects (`aor/<user>-aor`, `endpoint/<user>`) via ARI
dynamic-config PUTs, plus a best-effort `DELETE auth/<user>-auth` to
prune any legacy `auth` row left over from pre-PR-#77 builds. **SIP
digest auth is intentionally not provisioned** — the browser has no
SIP password (see `shared/sip-ua/sip-ua-sipjs.ts` `authorizationPassword: ''`), the cloud's
`/sip-ws` upgrade is the only auth layer, and a second digest layer
would put Asterisk into an unanswerable 401 loop. After the bootstrap
the watcher tails the Mongo change stream at 200 ms cadence; `insert`
/ `update` with `status="active"` re-PUTs, `status!="active"` or
`delete` DELETEs. The cursor uses `resumeAfter` (PR #21) so a Mongo
flap doesn't drop events — the watcher captures every event's `_id`
token and reopens from there.

**Asterisk needs `sorcery.conf` for these PUTs to land.** Default
Asterisk maps `pjsip.aor/endpoint/auth` to the read-only `config`
wizard, which makes every dynamic-config PUT return
`403 "Cannot create sorcery objects of type 'aor'"`. PR #101 ships
[`docker/asterisk/sorcery.conf`](./docker/asterisk/sorcery.conf)
which maps the types to `astdb,<family>` (the `<family>` is
mandatory — an empty data field makes `res_pjsip` decline to load
entirely). It's bundle-mounted into the container by
[`docker-compose.agent.yml`](./docker-compose.agent.yml) so operators
get this transparently. Persistence lives in
`/var/lib/asterisk/astdb.sqlite3` inside the container's writable
layer; re-provisioning is idempotent so a fresh container catches up
on first agent bootstrap.

PR #100 fixed a separate provisioner-side regression: the
empty-`sip_ha1` guard at the top of `PjsipProvisioner::provision`
was a leftover from the pre-PR-#77 digest-auth era. Because the new
import flow legitimately leaves `sipHa1` empty, this guard silently
dropped every resident from the bootstrap scan. Removed; the
remaining guard is `if (sip_username.empty())` only.

The SIP realm carried on the auth object's `realm` field was
auto-discovered on every (re)connect via PR #24's `AGENT_HELLO` →
`SOCIETY_BOOTSTRAP` round-trip. The realm plumbing remains in place
(observability hook, future-use), but no provisioned object
references it anymore. A future PR will remove the dead realm code.

The codec / DTLS / WebRTC field set the provisioner emits is asserted
to match `pjsip.conf`'s `[endpoint-resident-template]` by
`PjsipTemplateDrift` (PR #22). Edit either side without updating the
other and the test fails noisily.

### One-shot dry test (`scripts/lima.sh`)

`scripts/lima.sh` provisions a Lima VM (Apple Virtualization framework,
native arm64 — no QEMU), then brings up the **full six-container
on-prem stack** via `podman-compose -f docker-compose.agent.yml` inside
the VM. `pbx-agent` + `pbx-wsdbagent` dial the deployed Heroku cloud,
so a green run exercises the real cloud-tunnel handshake end-to-end.

```sh
lima start                       # alias → scripts/lima.sh start
# …~3 min cached / ~30+ min first cold run (bootstrap image build)…
lima shell                          # interactive bash inside the VM
lima shell sudo podman ps           # one-shot command — runs in the VM, returns
lima shell sudo podman logs pbx-agent
lima stop                        # pause: compose stop + VM stop. Non-destructive.
lima del                         # nuke: podman-compose down -v + VM delete (~30 GB freed)
```

What `lima start` does, in order:

1. Provisions or reuses VM `vm-onprem-pbx` (Ubuntu 24.04 arm64).
2. Installs podman + podman-compose.
3. Acquires `localhost/pbx-cpp-builder:bootstrap`. Three paths,
   fastest first: already in VM → no-op; on macOS host with matching
   arch → `podman save | podman load` stream (~60s); otherwise build
   inside VM from the canonical recipe inlined in `scripts/lima.sh`
   (~30 min, one-time).
4. Runs `scripts/setup-society.sh demo-society` once
   (`certs/asterisk-dtls/pbx.crt`, `certs/turnserver.conf`).
5. Writes `.env` with `CLOUD_HOST`, `AGENT_SOCIETY_ID`, `CERTS_DIR`.
6. `podman-compose -f docker-compose.agent.yml up --build -d`.
7. Sleeps `RUN_BUDGET_SECS` (default 120s), then prints container
   status + last logs from `pbx-agent`, `pbx-wsdbagent`,
   `pbx-asterisk`.

Override knobs (env): `HEROKU_HOST`, `HEROKU_PORT`, `SOCIETY_ID`,
`RUN_BUDGET_SECS`. Full reference (subcommands, workflows,
troubleshooting): [`scripts/README.md`](./scripts/README.md). End-to-end
narrative: [`ARCHITECTURE.md` § 6.2a](./ARCHITECTURE.md#62a-end-to-end-dry-test-lima-vm).

## Run the softphone UI

Angular 14 + Clarity, scaffolded under `ui/`. All toolchain ops run inside a `node:16-alpine` podman container:

```sh
# One-time install (writes ui/node_modules + ui/package-lock.json).
podman run --rm -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npm install --legacy-peer-deps --no-audit --no-fund

# Production build (output: ui/dist/pbxui/)
podman run --rm -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npx ng build --configuration production

# Dev server on :4200
podman run --rm -p 4200:4200 -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npx ng serve --host 0.0.0.0
```

See [`ui/README.md`](./ui/README.md) for slice plan and dependency pinning notes.

## Run the mobile softphone test suite

React Native + Jest under `mobile/` — TDD layers M0 → M3.b are JS-
complete. Test runner is containerised (Node not required on the host):

```sh
podman-compose -f docker-compose.mobile-test.yml build mobile-test
podman-compose -f docker-compose.mobile-test.yml run --rm mobile-test
```

> ⚠️ `podman-compose run` alone reuses the cached image — when `mobile/`
> source changes, the explicit `build` step above is required or the
> new code never enters the image (same caveat as `offtarget`).

Current suite: **17 files / 113 tests**. Image:
[`docker/Dockerfile.mobile-test`](./docker/Dockerfile.mobile-test).

Actually building the iOS / Android app (.ipa / .apk) needs the
RN toolchain on a dev machine — see
[`mobile/README.md`](./mobile/README.md) for the dev-machine workflow
and the M4 / M5 device-required layers.

## Run the Vaadin admin UI

The admin UI lives under `onprem/` — Java 17 + Vaadin 24 + Spring Boot
3.2 + Maven, modelled after `xpmile/onprem/`. It's a separate app from
the resident softphone (`ui/`): admins log in to onboard subscribers in
bulk, manage societies, and see at-a-glance dashboards.

```sh
# Start the admin UI on :8081, pointed at the deployed Heroku cloud.
# Java / Maven NOT required on the host — runs inside a podman maven
# container with a named volume for the ~/.m2 cache.
scripts/run-admin-ui.sh

# Override the backend (e.g. against a local pbx-cloud on :8080):
scripts/run-admin-ui.sh --backend-url http://localhost:8080

# Different listen port:
PORT=9090 scripts/run-admin-ui.sh
```

## Install on a society machine

For the turnkey operator install (a building / society admin on an
Ubuntu 22/24 box), see [`INSTALL.md`](./INSTALL.md). For Windows
hosts (Docker Desktop + WSL2), see [`INSTALL-windows.md`](./INSTALL-windows.md)
— but Linux is the recommended runtime target for any real
deployment because WSL2's UDP NAT breaks SIP/RTP audio paths.
One-line summary:

```sh
# On the dev machine, after `./deploy-heroku.sh deploy`:
./deploy-heroku.sh package-society-certs SUNSET   # → /tmp/SUNSET-certs.tar.gz

# On the society machine, after copying the repo + tarball:
sudo ./install.sh                                  # 6 prompts, ~3-5 min
```

`install.sh` is the entry point. It:

1. Pulls pre-built container images from Docker Hub
   (`docker.io/naushada/onprem-pbx-{agent,wsdbagent}`).
2. Generates per-society config (Asterisk DTLS cert + coturn config).
3. **Seeds Mongo with the society + first ADMIN subscriber** — so the
   operator can log into the admin UI immediately. The admin password
   is PBKDF2-SHA256-hashed locally, never sent over the wire.
4. Installs a systemd unit so the stack auto-restarts on reboot.

Six prompts: society code, cloud hostname (default), cert tarball
path, society's full name, admin email, admin password (silent). All
six accept env-var overrides for unattended re-runs — see
[`INSTALL.md`](./INSTALL.md) for the full operator walkthrough.

## Container image releases (CI)

[`.github/workflows/publish-images.yml`](./.github/workflows/publish-images.yml)
is the single workflow that runs all test gates + publishes images +
deploys `pbx-cloud` to Heroku.

**Triggers:**

- `pull_request` against `main` (PR #155) — runs the three test gates
  (`test` + `mobile-test` + `ui-test`) as merge gates. The publish +
  deploy jobs are gated off with `if: github.event_name == 'push' ||
  workflow_dispatch`.
- `push` to `main` (path-filtered to image-relevant files) — runs the
  test gates AND publishes images + deploys to Heroku. End-to-end:
  ~15 min on a warm cache, ~30-90 min cold.
- `workflow_dispatch` — manual run (same shape as push).

### What the workflow does (job graph)

```
                 ┌─► test         ────┐   (offtarget GTest — gates publish on PR + push)
bootstrap ───────┼─► (bootstrap)      │
[push-only]      └─► services         ├─► (publish + deploy — push-only)
                     publish-cloud    │
                     installer        ┘
ui-test          ──── (Angular ng build — runs on PR + push, gates nothing else)
mobile-test      ──── (Jest + tsc --noEmit — runs on PR + push, gates nothing else)
```

`test` `needs: bootstrap` but uses `if: always() && (success ||
skipped)` so it still runs on PR (where bootstrap is gated off).
`mobile-test` and `ui-test` have no bootstrap dependency (no native
C++ build).

| Job | What | Arch | Cache | Notes |
|---|---|---|---|---|
| `bootstrap` | Build `pbx-cpp-builder:bootstrap` (ACE/TAO 7.0.0 + mongo-cxx-driver + googletest on ubuntu:20.04) | amd64 + arm64 | registry buildcache (`mode=max`) | Push-only (PR #155). Slow cold (~30 min/arch); cached re-runs in seconds. `test` and downstream image jobs `FROM` this via `BUILDER_IMAGE` ARG. PRs that touch `Dockerfile.bootstrap` itself are rare; the contributor lands it on main first. |
| `test` | Build `docker/Dockerfile.test` against the (last-published or just-built) bootstrap, run `offtarget` GTest suite (600/600) | amd64 only | none (one-shot) | **Merge gate on PR + publish gate on push.** Catches the class of bug that used to surface only at Heroku deploy (PR #85 missing-header, PR #99 missing opcode whitelist, PR #100 stale provisioner gate, PR #101 missing bridge re-arm). Added in PR #96; gained PR trigger in PR #155. |
| `mobile-test` | Build `docker/Dockerfile.mobile-test`, run `npm run typecheck && npm test --ci` (150/150 jest + 0 typecheck errors) | amd64 only | none (one-shot) | **Merge gate on PR + sanity check on push.** Added in PR #155; gained `tsc --noEmit` in PR #156. |
| `ui-test` | `cd ui && npm ci --legacy-peer-deps && npx ng build --configuration development` | amd64 only | `actions/setup-node`'s npm cache | **Merge gate on PR + sanity check on push.** Angular's AOT template compiler runs on top of tsc; catches template binding errors that plain `tsc` would miss. Same call the production Heroku deploy uses. Added in PR #157. |
| `services` | Build + push `onprem-pbx-agent` + `onprem-pbx-wsdbagent` (matrix) | amd64 + arm64 (QEMU) | registry buildcache per image | Push-only (PR #155). Pulled by `install.sh` via `docker-compose pull`. |
| `publish-cloud` | Build `pbx-cloud`, push to `registry.heroku.com/pabx/web`, `heroku container:release web --app pabx` | amd64 only | (Heroku Common Runtime is amd64) | Push-only (PR #155). Replaces the manual `./deploy-heroku.sh deploy` round-trip (PR #95). The script stays as an escape hatch for emergency push from a laptop. Heroku registry rejects OCI manifests + attestations, so `provenance: false` + `oci-mediatypes=false` are set. |
| `installer` | Build + push `onprem-pbx-installer` (DooD pattern — runs the install scripts against the host's Docker daemon) | amd64 + arm64 (QEMU) | registry buildcache | Push-only (PR #155). Cross-platform install: one `docker run` from macOS/Windows/Linux brings the stack up. Bundles `scripts/installer-entrypoint.sh` + the per-society compose + cert tooling. Added in PR #97. |

Each image-publishing job also runs `peter-evans/dockerhub-description@v4`
to keep the Docker Hub "Overview" tab in sync with
[`docker/dockerhub-descriptions/*.md`](./docker/dockerhub-descriptions/)
— so the public Hub page for each image always reflects current
behaviour without a manual click (PR #99).

### Latest-push-wins (no overlapping runs)

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

A newer push to `main` auto-cancels any earlier in-flight run for
the same ref. Saves CI minutes + avoids the "v40 published from PR
#93 while v41 is being built from PR #95" race (PR #96). Practical
consequence: when you back-to-back-merge two PRs (#100 + #101 today),
only **one** workflow runs, against the second commit, and it builds
+ tests + deploys both fixes together.

### What triggers the workflow

| Action | Triggers workflow? |
|---|---|
| Push commit to a feature branch (no PR yet)                       | No — only `main` is watched |
| Open a PR against main                                            | No — PRs aren't a `push` event for this watcher. Run tests locally instead (see [Local pre-PR test](#local-pre-pr-test-loop)) |
| Merge a PR that touches `pbx-agent/**` or `modules/**`            | **Yes** |
| Merge a PR that touches `docker/Dockerfile.{bootstrap,agent,wsdbagent}` | **Yes** |
| Merge a PR that touches `CMakeLists.txt`                          | **Yes** |
| Merge a PR that touches the workflow itself                       | **Yes** |
| Merge a PR that's docs-only (`*.md`, `scripts/README.md`, …)      | No — saves ~30 min + 2 GB of Hub bandwidth per non-binary PR |
| Click "Re-run workflow" in the Actions UI                          | **Yes** (via `workflow_dispatch`) |

### Published images

| Image | Tags | Consumed by |
|---|---|---|
| `docker.io/naushada/pbx-cpp-builder` | `:bootstrap`, `:<sha>` | INTERNAL — base layer for every other build. Operators never pull this. |
| `docker.io/naushada/onprem-pbx-agent` | `:latest`, `:<sha>` | `install.sh` on society machine via `podman-compose pull` |
| `docker.io/naushada/onprem-pbx-wsdbagent` | `:latest`, `:<sha>` | same |
| `docker.io/naushada/onprem-pbx-installer` | `:latest`, `:<sha>` | Operators on macOS/Windows: `docker run naushada/onprem-pbx-installer …` (see [`INSTALL-windows.md`](./INSTALL-windows.md)) |
| `registry.heroku.com/pabx/web` | `:latest` | Heroku `pabx` app — released to web dyno automatically |

The `:<sha>` tags let a society pin to a known-good build — edit its
`docker-compose.agent.yml` to swap `:latest` for `:<commit-sha>`.

### Required GitHub repo secrets

Set once via *Settings → Secrets and variables → Actions* (repository
secrets, NOT environment secrets):

| Secret | Value | Notes |
|---|---|---|
| `DOCKERHUB_USERNAME` | `naushada` | The Docker Hub account that owns the published repos |
| `DOCKERHUB_TOKEN`    | a Docker Hub access token with **write** scope | Create at https://hub.docker.com/settings/security — do NOT use your account password. Never paste in chat; use `gh secret set` or the web UI. |
| `HEROKU_API_KEY`     | a long-lived Heroku authorization | Create at https://dashboard.heroku.com/account → "Authorizations" → "Create authorization". Do NOT use `heroku auth:token` — those expire and break the workflow mid-CI. |

### Local pre-PR test loop

The CI test gate is a safety net, not a substitute for running the
suite before pushing. The same image CI builds is reproducible locally
via Lima (macOS) or any Linux box with podman:

```sh
# from the repo root
podman build -f docker/Dockerfile.test -t onprem-pbx-test:dev .
podman run --rm onprem-pbx-test:dev
```

Expected: **556 PASSED, 10 SKIPPED** (the skips are baseline — see
[Skipped tests](#skipped-tests)). A new test failure means CI will
block the PR from publishing images / deploying cloud; fix locally
first.

## TLS cert rotation

**Every `./deploy-heroku.sh deploy` mints a fresh per-build CA and a
new client cert family.** `pbx-cert-watcher` lives **on-prem (in the
Lima VM or on the society machine), NOT in cloud.** It watches the
local `certs/cloud-issued/` directory and auto-restarts peers when
the files change.

End-to-end:

```
Dev's machine                  Cloud (Heroku)              On-prem (Lima / society)
─────────────                  ──────────────              ───────────────────────
./deploy-heroku.sh deploy
  │
  ├── cmd_build ─────────────► Dockerfile.cloud mints
  │                            fresh CA + client certs
  │                            inside the image
  │
  ├── cmd_extract_agent_certs
  │   pulls certs OUT of
  │   the new image into
  │   ./certs/cloud-issued/    ← LOCAL files
  │   (on dev's macOS)            on the dev machine
  │
  └── cmd_push + release ────► cloud running new CA
                                                          pbx-cert-watcher (on-prem)
                                                          watches certs/cloud-issued/
                                                          ─────────────────────────
                                                          • In Lima: certs/ is the
                                                            same bind-mounted repo
                                                            as the dev's host →
                                                            watcher sees the change
                                                            → restarts pbx-agent +
                                                            pbx-wsdbagent (~5s).
                                                          • On society machine:
                                                            certs/ only updates if
                                                            install.sh re-unpacks a
                                                            fresh tarball. Watcher
                                                            is idle until that.
```

**Lima dev loop is fully automatic** — the repo bind-mount means the
new certs are visible inside the VM the moment `cmd_extract_agent_certs`
finishes, and `pbx-cert-watcher` (a small `alpine:3.19` sidecar that
polls `/watch` every 5 s) detects the md5 change and POSTs to the host
podman socket to restart `pbx-agent` + `pbx-wsdbagent`. No manual
`podman restart`.

**Society machine is NOT fully automatic** — there's no channel today
from a cloud deploy to a society machine. The operator needs to:

1. On dev: `./deploy-heroku.sh package-society-certs SUNSET` —
   bundles the freshly-extracted certs into `/tmp/SUNSET-certs.tar.gz`.
2. Ship the tarball to the society (scp / email / USB).
3. On society: `sudo CERTS_TARBALL=/path/to/new.tar.gz ./install.sh` —
   the installer unpacks into `/opt/onprem-pbx/certs/cloud-issued/`,
   and the on-prem `pbx-cert-watcher` auto-restarts both peers within
   5 s.

See [`INSTALL.md` — "Refreshing certs after a cloud redeploy"](./INSTALL.md#refreshing-certs-after-a-cloud-redeploy)
for the operator-facing walkthrough.

> **Future gap:** a society-facing pull endpoint protected by a
> one-time installer token would let the society machine fetch its
> current certs directly from the cloud, eliminating the manual
> tarball ride-along. Not built yet.

The cert-watcher container itself: `docker-compose.agent.yml`
service `pbx-cert-watcher` — `alpine:3.19` + an inline shell script
that md5-polls the directory and calls the podman REST socket. No
custom Dockerfile, no published image.

### First-time admin bootstrap

The admin UI's login is gated on a real `subscribers` row with
`role=admin` (the `MicroServicePbx::resolve_admin_session` helper).
No such row exists on a fresh deploy — chicken-and-egg. The
canonical recipe is:

1. **Bring up the on-prem stack** so `pbx-wsdbagent` is connected to
   the cloud's `/ws/db` tunnel. The cloud runs with `REMOTE_DB=1` —
   without an attached wsdbagent the login handler short-circuits to
   `503 "On-prem agent not connected to cloud"` (deliberate; surfaces
   the real failure mode instead of a misleading 401).
   ```sh
   lima start    # full 6-container stack inside the Lima VM
   lima shell sudo podman logs pbx-wsdbagent | grep "session started"
   ```

2. **Seed the first society + admin** via `scripts/bootstrap-society.sh`.
   The CLI POSTs `/api/v1/society` to mint the society's `sipRealm` +
   `turnSharedSecret`, then writes the admin `subscribers` row directly
   with a PBKDF2-SHA256 password hash matching the cloud's
   `MongodbClient::hash_password` format.
   ```sh
   # Defaults shown — pick your own; rotate before any non-dev deploy.
   scripts/bootstrap-society.sh \
     --society-code   SUNSET \
     --society-name   'Sunset Towers' \
     --admin-email    admin@sunset.example \
     --admin-password 'changeme123' \
     --mongo-uri      mongodb://localhost:27017/pabx
   ```
   > **Caveat (live):** the CLI runs `mongosh` against `MONGO_URI` on
   > the operator's host. When Mongo lives inside the Lima VM
   > (`lima start`), it's not reachable from macOS. Workaround until
   > the CLI grows a `--via-lima` flag: do step 1's society POST with
   > curl, then `lima shell sudo podman exec -i pbx-mongo mongosh --eval
   > "db.subscribers.insertOne({…})"` for step 2.

3. **Flip strict mode on the cloud** so the gate actually enforces
   bcrypt + `role=admin`:
   ```sh
   heroku config:set PBX_AUTH_STRICT=1 --app pabx
   ```

4. **Log in** at `http://localhost:8081/login` with the credentials
   from step 2. The Vaadin login form expects three fields:

   | Form field      | What to type                                            |
   |-----------------|---------------------------------------------------------|
   | `Society label` | the `--society-code` you passed to bootstrap (e.g. `SUNSET`) |
   | `Flat number`   | `ADMIN` (bootstrap always seeds the first admin with this flatNumber) |
   | `Password`      | the `--admin-password` you passed to bootstrap          |

   On success you land on the Dashboard with Societies / Subscribers
   links in the sidenav.

See [`onprem/README.md`](./onprem/README.md) for the Vaadin app layout
+ package map, and `scripts/bootstrap-society.sh --help` for every
CLI flag.

## Deploy the cloud to Heroku

The cloud side runs one container — `pbx-cloud` — pushed to `registry.heroku.com/pabx/web`. The simple path is:

```sh
HEROKU_APP=pabx ./deploy-heroku.sh login
HEROKU_APP=pabx ./deploy-heroku.sh deploy
# subcommands: build | push | release | logs | open
```

`deploy-heroku.sh` is a thin wrapper around `podman-compose build`, `podman push --format=v2s2`, and `heroku container:release`. **Two practical gotchas** the wrapper handles for you (or that you'll hit if you build by hand):

1. **`podman push` fails partway through with `authentication required` against Heroku's registry** for some blobs. The Heroku container registry uses scope-based auth tokens that podman's bulk-push doesn't always re-acquire mid-transfer. **Workaround**: pipe the image through `skopeo` — it acquires a fresh token per HEAD/PUT and the push completes:

   ```sh
   podman save -o "$HOME/pbx-cloud.tar" registry.heroku.com/pabx/web
   podman run --rm -v "$HOME:/work:ro" quay.io/skopeo/stable:latest \
       copy --dest-creds="_:$(heroku auth:token)" \
            --format v2s2 --retry-times 5 \
            docker-archive:/work/pbx-cloud.tar \
            docker://registry.heroku.com/pabx/web
   ```

2. **Heroku rejects arm64 images** (Common Runtime is amd64-only). Builds on Apple Silicon **must** use `--platform linux/amd64`. The cached `pbx-cpp-builder:bootstrap` must also be amd64 — re-bootstrap the image from the canonical recipe inlined in [`scripts/lima.sh`](./scripts/lima.sh) (ACE/TAO 7.0.0 + mongo-c-driver 1.19.1 + mongo-cxx-driver v3.6, all installed under `/usr/local`).

The first-time Heroku app preparation:

```sh
heroku stack:set container --app pabx           # flip from buildpack → container stack
heroku config:set \
    VAPID_PUBLIC_KEY=<base64url> \
    VAPID_PRIVATE_KEY_B64=<base64-of-pem> \
    VAPID_SUBJECT='mailto:ops@example.com' \
    TURN_SHARED_SECRET="$(openssl rand -base64 32)" \
    TURN_URL='turn:turn.pbx.local:3478?transport=udp' \
    --app pabx
# Full VAPID lifecycle (generate, rotate, recover, troubleshoot):
#   docs/design/operations/vapid-keys.md
# Add `PBX_AUTH_STRICT=1 DB_URI=mongodb://…` once the subscribers
# collection is seeded; until then the cloud runs in dev-mode.
```

After a release, smoke-check with [`scripts/verify-deploy.sh`](./scripts/verify-deploy.sh):

```sh
./scripts/verify-deploy.sh remote
# 7/7 probes passed — UI index/main.js/favicon/sw.js + SPA fallback + REST reachability
```

### UI is bundled into the cloud image

`docker/Dockerfile.cloud` is three-stage (matches the build pattern per `DESIGN.md` §11):

1. `cpp-builder` — compiles `pbx-cloud` against the cached `pbx-cpp-builder:bootstrap` toolchain.
2. `ui-builder`  — runs `ng build --configuration development` on `ui/`.
3. `runtime`     — Ubuntu 20.04 slim with the binary at `/opt/pbx-cloud/pbx-cloud` and the SPA at `/opt/webgui/webui/`. The C++ webservice serves the bundle from `../webgui/webui/` (relative to its WORKDIR — inherited from the shared-library `webservice.cpp`).

URL surface (single Heroku app, single dyno):

| Path                | What                                                                                                     |
|---------------------|----------------------------------------------------------------------------------------------------------|
| `/webui/`           | SPA index.html. Browser loads `main.js`, `polyfills.js`, `favicon.svg`, etc., all under `/webui/*`.       |
| `/webui/<anything>` | SPA fallback — webservice returns `index.html` and Angular Router takes over.                            |
| `/api/v1/*`         | REST endpoints handled by `MicroService::dispatch_pbx_routes`.                                           |
| `/sip-ws`           | SIP-over-WebSocket upgrade.                                                                              |
| `/ws/db`            | Mongo-over-WSS proxy.                                                                                    |
| `/agent`            | mTLS WS upgrade for the on-prem agent tunnel.                                                            |

The SPA's `<base href>` is set to `/webui/` so all asset paths resolve under the same prefix.

Verify a deployment with `./scripts/verify-deploy.sh remote` — probes seven URLs and reports pass/fail.

### Connect the on-prem stack to the live cloud

Once the cloud is running on Heroku, bring up the on-prem stack on any host (your dev laptop is fine) so the agent dials `/agent` over the public Internet:

```sh
# 1. Generate self-signed mTLS material + replace the cloud CA with
#    the system trust bundle. Heroku's router uses a real CA-signed
#    cert at the edge; the agent's --tls-ca must therefore trust the
#    real root, not our self-signed CA. mTLS client-cert verification
#    is effectively a no-op (Heroku terminates TLS before the dyno
#    ever sees the cert) so the leaf is only for the future where
#    we deploy behind a TLS-passthrough load balancer.
mkdir -p certs/agent-deployed && cd certs/agent-deployed
openssl genrsa -out cloud-ca.key 2048
openssl req -x509 -new -nodes -key cloud-ca.key -sha256 -days 365 \
    -subj '/CN=onprem-pbx-ca' -out cloud-ca.tmp.pem
openssl genrsa -out agent.key 2048
openssl req -new -key agent.key -subj '/CN=pbx-agent-dev' -out agent.csr
openssl x509 -req -in agent.csr -CA cloud-ca.tmp.pem -CAkey cloud-ca.key \
    -CAcreateserial -days 365 -sha256 -out agent.crt
rm -f agent.csr cloud-ca.srl cloud-ca.tmp.pem cloud-ca.key
cp /etc/ssl/cert.pem cloud-ca.pem        # macOS system bundle
cd ../..

# 2. Generate the society-specific bits: DTLS cert + TURN secret +
#    rendered turnserver.conf. Idempotent — re-running keeps existing
#    material; rm + rerun to rotate.
./scripts/setup-society.sh "${AGENT_SOCIETY_ID:-demo-society}"

# 3. Fill in .env from the example.
cp .env.agent.example .env
$EDITOR .env       # set CLOUD_HOST=pabx-5fbf3550f938.herokuapp.com, AGENT_SOCIETY_ID=…

# 4. Build the amd64 images (Heroku is amd64; podman-compose --build
#    doesn't honour `localhost/` FROM lines, so build manually first).
podman build --platform linux/amd64 -f docker/Dockerfile.agent     -t onprem-pbx-agent:latest .
podman build --platform linux/amd64 -f docker/Dockerfile.wsdbagent -t onprem-pbx-wsdbagent:latest .

# 5. Up the stack. mongo + asterisk (with DTLS keys mounted) + coturn
#    (with the rendered turnserver.conf) + pbx-agent + pbx-wsdbagent.
podman-compose -f docker-compose.agent.yml up -d
podman logs pbx-agent | tail -10
#   ... AceSslTransport: connected + WS-upgraded pabx-5fbf3550f938.herokuapp.com:443/agent
#   ... AriWsClient: connected to Asterisk ARI pbx-asterisk:8088 (app=pbx)
```

The corresponding cloud log line — visible via `heroku logs --tail --app pabx` — is `GET /agent HTTP/1.1 ... Upgrade: websocket`. The tunnel is now alive end-to-end.

### Known limitations of the current Heroku deployment

| Limitation | Why | Workaround |
|---|---|---|
| ~~mTLS for `/agent` is not actually verified.~~ | Resolved by **PR #19 (D3)**. Both the agent and the cloud now run InnerTLS over the `/agent` WS frames — same trust boundary as `/ws/db`. The outer TLS that Heroku's router terminates is no longer load-bearing for authentication. Agent flags: `--inner-tls-{cert,key,ca,hostname}`. Cloud reuses `--tls-{cert,key,ca}` for both `/ws/db` and `/agent`. PR #25 adds `InnerTlsServer::peer_subject_cn()` so the cloud can log the verified agent CN; the same PR fixed a latent `SSL_CTX_use_certificate_file`-after-`SSL_new` bug that was silently letting the client present no cert. See [`docs/design/security/innertls.md`](./docs/design/security/innertls.md). | ✅ Done. |
| **Agent SIGSEGV ~5 s after inner-TLS handshake** (observed 2026-05-16 on native arm64). | Reproducible in Lima VM dry test against the deployed Heroku cloud. Stack too corrupted for gdb to unwind. Suspect: my recent `set_on_connected` callback + SubscriberWatcher reopen loop against unreachable Mongo + SOCIETY_BOOTSTRAP handler. Tracked in `memory:project_open_backlog` as "Agent post-handshake crash". Not seen on amd64 deploys. | ⚠️ Open — needs ASan/Valgrind to localize. |
| **Cloud doesn't run with `--remote-db` yet.** | The on-prem `pbx-wsdbagent` container exists (D1+D2 committed) but `REMOTE_DB=1` hasn't been flipped on Heroku because it'd block REST handlers until the wsdbagent is live. | D4: `heroku config:set REMOTE_DB=1 --app pabx` **after** `pbx-wsdbagent` is verified connected. |
| **Login is dev-mode permissive.** | The subscribers collection isn't seeded. `handle_subscriber_login_POST` returns a synthetic session for any non-empty credentials. | Run the CSV-import flow (`POST /api/v1/society/<id>/subscribers/import`) once Mongo is wired, then set `PBX_AUTH_STRICT=1`. |

### On-prem topology (after D1+D2)

```
                                    Heroku app `pabx`
                                    ─────────────────
Browser ──────────HTTPS───────────► pabx-…herokuapp.com
                                       │
                                       ├─► /webui/*           (SPA assets)
                                       ├─► /api/v1/*          (REST → wsdbproxy when REMOTE_DB)
                                       ├─► /sip-ws            (browser → SipBridge → CloudTunnelEndpoint)
                                       ├─► /agent             (← pbx-agent     SIP tunnel, plain WSS today)
                                       └─► /ws/db             (← pbx-wsdbagent DB tunnel, WS + InnerTLS)
                                                                ▲                 ▲
                                                                │                 │
                              ┌─────────────────────────────────┴─────────────────┴─────┐
                              │ docker-compose.agent.yml on the society host             │
                              │                                                          │
                              │   pbx-agent       — SIP tunnel, ARI, AsteriskWsFactory   │
                              │   pbx-wsdbagent   — DB tunnel (wss+InnerTLS) → pbx-mongo │
                              │   pbx-mongo       — collections under `pabx.*`           │
                              │   pbx-asterisk    — chan_pjsip + ConfBridge              │
                              │   pbx-coturn      — STUN/TURN (host net)                 │
                              └──────────────────────────────────────────────────────────┘
```

### Mongo database name

The database name is **`pabx`** (matching the Heroku app name). Project / container / binary names retain the `pbx-` prefix (`pbx-mongo`, `pbx-agent`, `pbx-wsdbagent`, `pbx-cloud`). Connection URIs in both agents append `…/pabx`:

```sh
MONGO_URI="mongodb://pbx-mongo:27017/pabx"
```

This is the default in `docker/Dockerfile.{agent,wsdbagent}` and `.env.agent.example`.

### Optional: standalone UI behind a reverse proxy

`docker/Dockerfile.ui` + `docker/nginx/nginx.conf.template` are still in the repo if you want to run the UI as a separate process (e.g. on a CDN-fronted host that proxies `/api` to a different cloud). The bundled path above is the recommended default.

## Repo layout

```
modules/module/
  http/         # MessageParser base + Http subclass (shared-library origin, refactored)
                #   inc:  message_parser.hpp, http_parser.hpp
                #   src:  message_parser.cpp, http_parser.cpp
                #   test: httpparser_test (regression), message_parser_test
  sip/          # Sip subclass + compact-header alias table (new)
                #   inc/sip_parser.hpp  src/sip_parser.cpp  test/sip_parser_test
  pbx/          # Cloud-side PBX components — the project's biggest module
                #   inc:  sip_frame, sip_bridge, tunnel_sink,
                #         microservice_pbx, push_sender,
                #         cloud_tunnel_endpoint, browser_stream,
                #         agent_stream
                #   src:  matching .cpp files
                #   test: matching _test.cc files
  webservice/   # ACE WebServer / WebConnection / MicroService
                # (shared-library copy, patched: dispatch_pbx_routes,
                #  /sip-ws + /agent upgrades, BrowserStream + AgentStream
                #  + CloudTunnelEndpoint wiring, sipBridge() accessor)
  mongodb/      # MongodbClient pool (verbatim copy of the upstream module — do not modify locally)
  wsdbproxy/    # Cloud-side Mongo-over-WSS proxy (verbatim copy of the upstream module — do not modify locally)
  email/        # SMTP FSM (verbatim copy of the upstream module — do not modify locally)
  security/     # innertls.cpp — transitively needed by wsdbproxy (verbatim copy of the upstream module — do not modify locally)
  thirdparty/   # nlohmann/json.hpp

pbx-agent/      # On-prem daemon — Layer 2 + Layer 3 ACE bindings.
  src/main/     # sip_frame_demux, cloud_connector, ari_client,
                # ace_ssl_transport, ari_ws_client
  src/test/     # matching _test.cc files

test/
  integration/  # Layer 3: tunnel_e2e, handoff_ordering (source-invariant)
  main.cc       # GTest entrypoint
  CMakeLists.txt

docker/         # Container build context
                #   Dockerfile.test   — runs all GTest suites
                #   Dockerfile.agent  — production pbx-agent image (multi-stage)
                #   Dockerfile.cloud  — production pbx-cloud image (multi-stage)
                #   asterisk/         — minimal chan_pjsip + ARI + Stasis config
                #   coturn/           — minimal use-auth-secret turnserver.conf
docker-compose.agent.yml    # On-prem stack: mongo + asterisk + coturn + pbx-agent
docker-compose.heroku.yml   # Cloud stack: pbx-cloud, tagged for registry.heroku.com
deploy-heroku.sh            # podman + heroku CLI wrapper
.env.agent.example          # Template for docker-compose.agent.yml env

docs/           # PRD, DESIGN, TDD-PLAN are at the root; sub-design docs land here
shared/         # Source shared between ui/ and mobile/
                #   sip-ua/       — sip.js wrapper + interface seam (PR #146);
                #                   re-exported by ui/src/common/sip-ua{,-sipjs}.ts
                #                   and mobile/src/sip/{sipUa,sipJsUaFactory}.ts
ui/             # Angular 14 + Clarity softphone — all 7 slices complete
                #   src/common/   — auth, sip-ua seam (re-exports shared/sip-ua),
                #                   sip.service, ringtone.service, push.service,
                #                   device.service, httpsvc, pubsubsvc, app-globals
                #   src/app/      — login, main (shell + sidebar), dashboard,
                #                   directory, history, settings, call-panel
                #   src/sw.js     — Service Worker for push wakeup
                #   e2e/          — Playwright tests (login, dashboard, directory,
                #                   history, settings)
                #   docker/Dockerfile.ui builds the nginx-served production image
scripts/        # Build/deploy helpers — Layer 4
certs/          # Local-dev cert material (gitignored except templates)
```

## Layer 4 — production wiring (complete)

Layer 3 closed out the **state machines + ACE bindings**. Layer 4 is the deployment-and-product surface; none of it changes the tested core.

| Component | Status |
|---|---|
| `pbx-agent/src/main/main.cpp` — agent reactor + CloudConnector + AceSslTransportFactory + SipFrameDemux + AriClient + AriWsClient + tiny reconnect supervisor + MongodbClient | ✅ Linked + `--help` clean. Two placeholders documented inline: `NoopAriRest` (the admission `continue` REST call is not wired yet) and `StubAsteriskFactory` (the per-stream WS connection to local Asterisk's `chan_pjsip` is not wired yet). Both placeholders log loudly and the rest of the agent works around them. |
| Cloud bootstrap in `webservice_main.cpp` — instantiate `SipBridge` + `CloudTunnelEndpoint`; wire `bridge.set_push_notify_handler` (placeholder: log only — `PushSender` not yet wired); wire `bridge.set_cdr_push_handler` → `MongodbClient::create_document("cdr", payload)` | ✅ Both `--remote-db` and local-Mongo modes patched; cloud binary `pbx-cloud` builds + `--help` clean. |
| CMake `pbx-agent` + `pbx-cloud` build targets | ✅ Both binaries link; `BUILD_BINARIES=ON` is on by default. `docker/Dockerfile.test` builds them alongside `offtarget`. |
| Real `AsteriskWsFactory` (replaces `StubAsteriskFactory`) — plain-TCP + WS upgrade to `ws://127.0.0.1:8088/ws` (chan_pjsip transport) with `Sec-WebSocket-Protocol: sip` per RFC 7118 §4 | ✅ Complete |
| Real `IAriRest` impl — `AriRestClient` POSTs to `/ari/applications/{app}/subscription` and `/ari/channels/{cid}/continue` (HTTP Basic auth, URL-encoded path + query) | ✅ Complete |
| Real `PushSender` wiring on cloud — `AceHttpsClient` for HTTPS POSTs + `SystemClock`; `--vapid-key-path` + `--vapid-subject` CLI flags; both branches of `webservice_main.cpp` patched. If flags unset, log-only stub remains. | ✅ Complete |
| `docker-compose.agent.yml` — `pbx-mongo` (mongo:7) + `pbx-asterisk` (andrius/asterisk:20, chan_pjsip + ARI configs in `docker/asterisk/`) + `pbx-coturn` (coturn:4.6, `host` net for STUN replies) + `pbx-agent` (multi-stage `docker/Dockerfile.agent`). `pbx-net` bridge isolates inter-service traffic; Asterisk's WS port stays internal. Env via `.env` (template: `.env.agent.example`). | ✅ Complete |
| `docker-compose.heroku.yml` + `deploy-heroku.sh` — `pbx-cloud` built from `docker/Dockerfile.cloud`, tagged `registry.heroku.com/${HEROKU_APP}/web`. Wrapper has `login`/`build`/`push`/`release`/`deploy`/`logs`/`open` subcommands and uses `podman` + the Heroku CLI. | ✅ Complete |
| `ui/` (Angular softphone — SIP.js + WebRTC + Clarity + Service Worker). 7-slice plan documented in [`ui/README.md`](./ui/README.md): scaffold → login (`AuthService` + `AuthGuard` + `AuthInterceptor`) → `SipService` over the sip.js seam → directory + outbound call (`SipCallHandle`, `CallPanelComponent`) → inbound + `RingtoneService` + `PushService` + `src/sw.js` → conference + history + settings + `DeviceService` → `Dockerfile.ui` (nginx). 61 karma specs cover the core services; `app.component.ts` deep-link bug caught by E2E and fixed. | ✅ Complete |
| `docker/Dockerfile.ui` + nginx (multi-stage `node:16-alpine` build → `nginx:1.25-alpine` runtime, `envsubst` template, `resolver 8.8.8.8 1.1.1.1` for Heroku cold-starts, `/api/`, `/sip-ws`, `/ws/db` proxied with WS upgrade headers + 86 400 s timeouts, SPA fallback) | ✅ Complete |
| Playwright E2E (`ui/e2e/`) — 12 specs across login / dashboard / directory / history / settings. Runs against the production-shape bundle served by `http-server`; cloud REST surface mocked via `page.route()` so no backend is needed. | ✅ Complete |

The TDD-style coverage stays the same shape — every Layer 4 piece either has its own GTest suite (where it's pure logic), an integration test (where it's I/O-bound), or a Playwright spec (UI surfaces).

## Implementation order

See [TDD-PLAN.md → Order of work](./TDD-PLAN.md#order-of-work-drives-the-implementation). Each layer is fully green before the next starts.

## License

[MIT](./LICENSE) — copyright © 2026 onprem-pbx contributors. Use, fork,
embed in proprietary or open products; the only ask is preserving the
copyright + license notice in copies and substantial portions.
