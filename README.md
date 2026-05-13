# onprem-pbx

VoIP PBX for residential societies. Heroku-hosted control plane (C++ / ACE) + on-prem `pbx-agent` (C++ / ACE + Asterisk + coturn + MongoDB) + Angular web softphone (SIP.js + WebRTC). Sibling project to [xpmile](../xpmile).

See:

- [PRD.md](./PRD.md) — product requirements, personas, success metrics.
- [DESIGN.md](./DESIGN.md) — architecture, components, data model, call flows, media security.
- [TDD-PLAN.md](./TDD-PLAN.md) — test layers and implementation order.

## Status

| Layer | What | Status |
|---|---|---|
| 0.b | `MessageParser` base + `Http` subclass refactor + `Sip` subclass | ✅ Complete (commit `f45b40a`) |
| 0.a | `SipFrame` wire-format primitives | ✅ Complete (commit `f45b40a`) |
| 1   | `SipBridge` cloud-side multiplexer | ✅ Complete (first slice) |
| 1   | xpmile module copies — webservice, mongodb, wsdbproxy, security, email, thirdparty | ✅ Verbatim regression-guard copy green |
| 1   | `MicroServicePbx*` REST handlers (society, subscriber-import, cdr, push, sipws-upgrade) | ✅ Complete |
| 1   | `PushSender*` — VAPID JWT (RFC 8292) + Web Push encryption (RFC 8291) + retry/410 | ✅ Complete |
| 1   | Route wiring (`MicroService::dispatch_pbx_routes`) + `/sip-ws` upgrade auth gate in `WebConnection` | ✅ Complete |
| 1   | `HandoffOrdering` test (asserts `remove_handler → m_handle=INVALID → bridge.publish` order in `WebConnection`) | ⏭️ Deferred to Layer 3 TunnelE2E — needs reactor mocking |
| 2   | `SipFrameDemux` — agent-side mirror of `SipBridge`, demuxes cloud tunnel ↔ per-stream Asterisk sockets | ✅ Complete |
| 2   | `CloudConnector` — agent-side mTLS dial-out, reconnect-backoff state machine, outbound buffer | ✅ Complete (state machine; `AceSslTransport` deferred to Layer 3) |
| 2   | `AriClient` — ARI event consumer, bridge-counter admission, CDR writer, conference detection | ✅ Complete (state machine; WS subscription glue deferred to Layer 3) |
| 2   | `CloudTunnelEndpoint` — accept-side of the cloud↔agent tunnel; wired into `WebServer`; `/agent` WS upgrade branch in `WebConnection`; context-aware `/sip-ws` 503 hint | ✅ Complete (`BrowserStream` + agent WS decoder deferred to Layer 3) |
| 3   | `TunnelE2E` — paired in-memory transport harness, end-to-end frame plumbing tests (browser ↔ cloud bridge ↔ tunnel ↔ agent demux ↔ fake Asterisk and back) | ✅ Complete |
| 3   | `BrowserStream` — first concrete ACE binding (ACE_Event_Handler + BrowserSink). Retires the `/sip-ws` 503 stub: real WS hand-off now lives | ✅ Complete |
| 3   | `AgentStream` — cloud-side ACE binding for `/agent` (ACE_Event_Handler + private TransportAdapter implementing IAgentTransport). Retires the `/agent` stub | ✅ Complete |
| 3   | `AceSslTransport` + factory — agent-side outbound mTLS + WS upgrade dial. Concrete `ITransport` for `CloudConnector`'s factory | ✅ Complete |
| 3   | `AriWsClient` — plain-TCP WS client for Asterisk `/ari/events`; HTTP Basic auth; pushes each JSON event into `AriClient::on_event` | ✅ Complete |
| 3   | `HandoffOrdering` source-invariant test — guards the xpmile-CLAUDE.md `remove_handler → m_handle=INVALID → publish` ordering for all 3 WS upgrade branches (`/sip-ws`, `/agent`, `/ws/db`) | ✅ Complete |
| 4   | Production wiring — `pbx-agent` + `pbx-cloud` binaries, `AsteriskWsFactory` (real chan_pjsip WS), `AriRestClient` (admission `continue`), real `PushSender` (AceHttpsClient + VAPID), `docker-compose.{agent,heroku}.yml`, `Dockerfile.{agent,cloud,ui}`, `deploy-heroku.sh` | ✅ Complete (see [§ Layer 4 detail](#layer-4--production-wiring-complete)) |
| UI  | Angular 14 + Clarity softphone — 7 slices: scaffold → login + `AuthGuard` → `SipService` (sip.js seam) → directory + outbound call → inbound + ringtone + Web Push + Service Worker → conference + history + settings + `DeviceService` → `Dockerfile.ui` (nginx) → Playwright E2E | ✅ Complete (see [`ui/README.md`](./ui/README.md)) |

### Test totals: **425 / 425** (C++ 352 + UI karma 61 + UI Playwright 12)

| Layer | Suites | Tests |
|-------|--------|------:|
| 0     | HttpParser 20 + MessageParserBase 8 + SipParser 17 + SipFrame 10 | **55** |
| 1     | SipBridge 14 + MicroServicePbx 11 + MicroServiceRouting 7 + PushSender 8 | **40** |
| 2     | SipFrameDemux 14 + CloudConnector 11 + AriClient 11 + CloudTunnelEndpoint 12 | **48** |
| 3     | TunnelE2E 8 + BrowserStream 8 + AgentStream 9 + AceSslTransport 10 + AriWsClient 14 + HandoffOrdering 8 | **57** |
| 4     | AsteriskWsFactory 13 + AriRestClient 14 + AceHttpsClient 13 | **40** |
| regression | inherited xpmile suites (verbatim copy) | **112** |
| **Total** | **36 suites** | **352** |

| UI suite | Tests |
|----------|------:|
| karma (Jasmine) — `AuthService`, `AuthGuard`, `AuthInterceptor`, `LoginComponent`, `SipService`, `RingtoneService`, `PushService`, `HistoryComponent`, `DirectoryComponent` | **61** |
| Playwright E2E — login, dashboard, directory, history, settings | **12** |
| **UI total** | **73** |

**The MVP is feature-complete end-to-end.** Cloud + agent control plane has real ACE bindings; the on-prem stack composes a working Asterisk/coturn/Mongo/agent topology; the Angular softphone can register over SIP-over-WS, dial directory entries, host inbound calls with VAPID push wakeup, and join a society ConfBridge. Production images for both `pbx-cloud` and `pbx-ui` are smoke-built and deployable to Heroku via `./deploy-heroku.sh deploy-all`.

## Architecture (end of Layer 3)

```
Heroku cloud (C++ / ACE)                  pbx-agent (C++ / ACE) — on-prem
────────────────────────                  ──────────────────────────────
                                                                       
WebServer                                                              
  ├─ WebConnection (per inbound socket)                                
  │    ├─ /sip-ws upgrade ─► BrowserStream ◄──┐                        
  │    ├─ /agent  upgrade ─► AgentStream  ◄──┐│                        
  │    ├─ /ws/db  upgrade ─► WsDbServer       ││ (xpmile, unchanged)   
  │    └─ REST/portal     ─► MicroService     ││                       
  │                          ├─ xpmile routes ││                       
  │                          └─ dispatch_pbx_routes ─► MicroServicePbx 
  │                                            ││                       
  ├─ SipBridge       ◄──────TunnelSink────► CloudTunnelEndpoint        
  │   ├─ set_push_notify_handler ──────► (PushSender::notify hook)     
  │   └─ set_cdr_push_handler    ──────► (Mongo CDR writer hook)       
  │                                                                    
  └─ MongoDB ◄──── wsdbproxy ─── wsdbagent ──── on-prem Mongo          
                                                                       
                                                              ┌───────►│ AceSslTransport
                                                              │        │   ↑      ↓
                              WS over mTLS (Heroku /agent) ◄──┴────────┤   │      │
                                                                       │ CloudConnector
                                                                       │   ↓      ↑
                                                                       │ SipFrameDemux
                                                                       │   ↓
                                                                       │ IAsteriskFactory
                                                                       │    │
                                                                       │    └► Asterisk WS
                                                                       │       (chan_pjsip,
                                                                       │        ws://127:8088)
                                                                       │
                                                                       │ AriClient ◄── AriWsClient
                                                                       │  ├─ admission counter
                                                                       │  ├─ CDR ─► Mongo
                                                                       │  └─ PUSH_NOTIFY ────┐
                                                                       │                     │
                                                                       │  CloudConnector ◄───┘
                                                                       │  .send_frame(…)
                                                                       │       ↑
                                                                       │       └── (also from
                                                                       │              SipFrameDemux
                                                                       │              upstream
                                                                       │              answers)
                                                                       └───────────────────────
```

### How traffic flows

**Browser INVITE → Asterisk:**
`Browser WSS → /sip-ws upgrade → BrowserStream.handle_input → SipBridge.on_browser_data → CloudTunnelEndpoint.send_frame → AgentStream WS → AceSslTransport ← (wire) → CloudConnector.on_bytes_received → SipFrameDemux → local Asterisk WS`

**Asterisk reply / events → Browser:**
`Asterisk WS → SipFrameDemux.on_asterisk_data → CloudConnector.send_frame → AceSslTransport (wire) → AgentStream.handle_input → CloudTunnelEndpoint.on_bytes_received → SipBridge → BrowserStream.send_bytes → Browser WSS`

**Asterisk hangup → Mongo CDR + push:**
`Asterisk → AriWsClient → AriClient.on_event(ChannelDestroyed) → CDR written locally + PUSH_NOTIFY frame → CloudConnector → AgentStream → SipBridge → bridge.cdr_push_handler / push_notify_handler hooks`

### Skipped tests

Three inherited xpmile tests are filtered out by `docker/Dockerfile.test`'s default CMD because they fail in xpmile too and are environment-dependent, not parser/protocol regressions:

| Test                                                          | Why skipped |
|---------------------------------------------------------------|-------------|
| `AccountLoginTest.ValidCredentials_Returns200WithAccountData` | Requires a live MongoDB with xpmile shipment-account seed data. |
| `AccountLoginTest.ResponseBody_ExcludesSensitiveFields`       | Same root cause — depends on the 200 OK path that needs seeded Mongo. |
| `WsDbServer.SecondAgentRejected_When_FirstAlive`              | xpmile's production code returns "stale agent evicted, retry"; the test still asserts a 409 and has drifted from the code. |

Override the filter to include them once a Mongo fixture is wired up:

```sh
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer1 --gtest_filter='*'
```

## Build & run

All container operations use **podman** (not docker). Same toolchain as xpmile.

```sh
# Build the test image. Reuses the cached `pbx-cpp-builder:bootstrap`
# image (a tagged snapshot of xpmile's cpp-builder stage with ACE/TAO 7.0.0,
# googletest, mongo-cxx-driver, and OpenSSL already installed under /usr/local).
# Build time: ~60 s.
podman build -f docker/Dockerfile.test -t onprem-pbx-test:latest .

# Run all GTest suites (3 inherited xpmile tests skipped by default — see
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
from-scratch Dockerfile mirroring xpmile's full cpp-builder is at
[`docker/Dockerfile.test`](./docker/Dockerfile.test) and rebuilds the
toolchain in ~30 min.

## Run the on-prem agent stack

The on-prem deployment runs four containers — `pbx-mongo`, `pbx-asterisk`,
`pbx-coturn`, and `pbx-agent` — composed by `docker-compose.agent.yml`.

```sh
# One-time: copy the env template and fill in real values
# (CLOUD_HOST, AGENT_SOCIETY_ID, CERTS_DIR pointing at agent.crt/key + cloud-ca.pem).
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

Topology:

| Container | Network | Why |
|---|---|---|
| `pbx-mongo` | `pbx-net` (internal bridge) | Subscribers + CDR + push subscriptions. Not exposed to host. |
| `pbx-asterisk` | `pbx-net` (internal bridge) | chan_pjsip WS on `:8088` (agent reaches via service DNS). NOT host-exposed — auth lives in SIP digest, not at the WS layer. |
| `pbx-coturn` | host networking | TURN needs the real public IP in STUN replies. The society opens one UDP port (`TURN_PUBLIC_PORT`, default 3478) and DNATs it to coturn. |
| `pbx-agent`  | `pbx-net` (internal bridge) | Dials the Heroku `/agent` endpoint over mTLS using the `CERTS_DIR` material. |

Asterisk config files (`docker/asterisk/*.conf`) are minimal but functional:

- `pjsip.conf` — WS transport, WebRTC-enabled endpoint template (`webrtc=yes`, `auth_type=md5`, `realm=pbx.local`), sample `alice` / `bob` endpoints. Replace with society-specific endpoints in production.
- `extensions.conf` — Stasis app `pbx` (admission gate) + `pbx-busy` busy-signal context for over-cap calls.
- `ari.conf` — single ARI user `asterisk:asterisk`; **override via `ARI_USER` / `ARI_PASS`** in `.env` before any non-local deployment.
- `http.conf` — listens on `0.0.0.0:8088`, scoped to the internal bridge network.

coturn (`docker/coturn/turnserver.conf`) uses `use-auth-secret` so the cloud's `GET /api/v1/turn-credentials` endpoint can mint time-limited credentials with a shared HMAC-SHA1 secret (RFC 5766 §5). The bundled `static-auth-secret` is dev-only — overwrite before production.

## Run the softphone UI

Angular 14 + Clarity, scaffolded under `ui/` (mirrors `xpmile/ui/` shape). All toolchain ops run inside a `node:16-alpine` podman container:

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

## Deploy the cloud to Heroku

The cloud side runs one container — `pbx-cloud` — pushed to `registry.heroku.com`:

```sh
# One-time: authenticate the local podman daemon with Heroku's registry.
HEROKU_APP=onprem-pbx ./deploy-heroku.sh login

# Build + push + release in one shot.
HEROKU_APP=onprem-pbx ./deploy-heroku.sh deploy

# Individual subcommands (same as xpmile's deploy-heroku.sh):
#   build | push | release | logs | open
HEROKU_APP=onprem-pbx ./deploy-heroku.sh logs
```

The wrapper is a thin shell around `podman-compose -f docker-compose.heroku.yml build pbx-cloud`, `podman push --format=v2s2`, and `heroku container:release`. The image is the same `docker/Dockerfile.cloud` that produces `localhost/onprem-pbx-cloud:latest` locally, just retagged for Heroku.

## Deploy the softphone UI to Heroku

The UI is a separate image (`docker/Dockerfile.ui` — multi-stage `node:16-alpine` build → `nginx:1.25-alpine` runtime). It runs as its own Heroku app and reverse-proxies `/api/*` + `/sip-ws` + `/ws/db` to the cloud app over the public hostname.

```sh
# Build, push, and release the UI image (independent of the cloud).
HEROKU_APP_UI=onprem-pbx-ui ./deploy-heroku.sh deploy-ui

# Or build + deploy *both* sides in one shot:
HEROKU_APP=onprem-pbx HEROKU_APP_UI=onprem-pbx-ui \
  ./deploy-heroku.sh deploy-all

# Tail UI logs:
./deploy-heroku.sh logs-ui

# Open the UI in a browser:
./deploy-heroku.sh open
```

Required Heroku config vars on the UI app:

```sh
heroku config:set \
  BACKEND_ORIGIN=https://onprem-pbx.herokuapp.com \
  --app onprem-pbx-ui
```

`$PORT` is injected by Heroku; nginx listens on it via the templated `nginx.conf`. Runtime DNS (`resolver 8.8.8.8 1.1.1.1`) keeps upstream resolution working on Heroku cold-starts.

For local "production-like" smoke testing the same compose file brings up both services on `pbx-net`:

```sh
HEROKU_APP_CLOUD=onprem-pbx HEROKU_APP_UI=onprem-pbx-ui \
  podman-compose -f docker-compose.heroku.yml up --build
# Browser → http://localhost:8080 (UI), API proxied to pbx-cloud:8080.
```

## Repo layout

```
modules/module/
  http/         # MessageParser base + Http subclass (xpmile origin, refactored)
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
                # (xpmile copy, patched: dispatch_pbx_routes,
                #  /sip-ws + /agent upgrades, BrowserStream + AgentStream
                #  + CloudTunnelEndpoint wiring, sipBridge() accessor)
  mongodb/      # MongodbClient pool (xpmile copy, verbatim)
  wsdbproxy/    # Cloud-side Mongo-over-WSS proxy (xpmile copy, verbatim)
  email/        # SMTP FSM (xpmile copy, verbatim)
  security/     # innertls.cpp — transitively needed by wsdbproxy (xpmile copy)
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
deploy-heroku.sh            # podman + heroku CLI wrapper (xpmile-style)
.env.agent.example          # Template for docker-compose.agent.yml env

docs/           # PRD, DESIGN, TDD-PLAN are at the root; sub-design docs land here
ui/             # Angular 14 + Clarity softphone — all 7 slices complete
                #   src/common/   — auth, sip-ua seam, sip-ua-sipjs production wrapper,
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
| `docker-compose.heroku.yml` + `deploy-heroku.sh` (clone of xpmile's) — `pbx-cloud` built from `docker/Dockerfile.cloud`, tagged `registry.heroku.com/${HEROKU_APP}/web`. Wrapper has `login`/`build`/`push`/`release`/`deploy`/`logs`/`open` subcommands and uses `podman` + the Heroku CLI exactly as xpmile does. | ✅ Complete |
| `ui/` (Angular softphone — SIP.js + WebRTC + Clarity + Service Worker). 7-slice plan documented in [`ui/README.md`](./ui/README.md): scaffold → login (`AuthService` + `AuthGuard` + `AuthInterceptor`) → `SipService` over the sip.js seam → directory + outbound call (`SipCallHandle`, `CallPanelComponent`) → inbound + `RingtoneService` + `PushService` + `src/sw.js` → conference + history + settings + `DeviceService` → `Dockerfile.ui` (nginx). 61 karma specs cover the core services; `app.component.ts` deep-link bug caught by E2E and fixed. | ✅ Complete |
| `docker/Dockerfile.ui` + nginx (multi-stage `node:16-alpine` build → `nginx:1.25-alpine` runtime, `envsubst` template, `resolver 8.8.8.8 1.1.1.1` for Heroku cold-starts, `/api/`, `/sip-ws`, `/ws/db` proxied with WS upgrade headers + 86 400 s timeouts, SPA fallback) | ✅ Complete |
| Playwright E2E (`ui/e2e/`) — 12 specs across login / dashboard / directory / history / settings. Runs against the production-shape bundle served by `http-server`; cloud REST surface mocked via `page.route()` so no backend is needed. | ✅ Complete |

The TDD-style coverage stays the same shape — every Layer 4 piece either has its own GTest suite (where it's pure logic), an integration test (where it's I/O-bound), or a Playwright spec (UI surfaces).

## Implementation order

See [TDD-PLAN.md → Order of work](./TDD-PLAN.md#order-of-work-drives-the-implementation). Each layer is fully green before the next starts.
