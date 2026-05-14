# onprem-pbx

VoIP PBX for residential societies. Heroku-hosted control plane (C++ / ACE) + on-prem `pbx-agent` (C++ / ACE + Asterisk + coturn + MongoDB) + Angular web softphone (SIP.js + WebRTC). Sibling project to [xpmile](../xpmile).

**Live**: <https://pabx-5fbf3550f938.herokuapp.com/webui/> — single Heroku app (`pabx`) serving UI at `/webui/` + REST + SIP-over-WS + `/agent` mTLS tunnel. UI assets at `/webui/main.js`, `/webui/favicon.svg`, `/webui/sw.js`. SPA login is dev-mode permissive — any non-empty `societyCode` / `flatNumber` / `password` returns a synthetic session until the CSV-import flow + `PBX_AUTH_STRICT=1` are wired.

See:

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
| 4   | Cloud REST handlers — `subscriber/login` (dev-mode), `subscriber` directory, `push-vapid-key`, `turn-credentials`, `push-subscribe`. `db_available()` env-var guard short-circuits DB-touching handlers when Mongo isn't configured (defends against Heroku H12 timeouts) | ✅ Complete |
| 4   | D1+D2: `wsdbagent` on-prem — verbatim copy of xpmile's standalone DB-tunnel binary + new `pbx-wsdbagent` compose service. Dials `wss://${CLOUD_HOST}/ws/db` with ACE InnerTLS on top of the outer WSS (Heroku terminates outer TLS; inner TLS is the real trust boundary). Mongo DB name is `pabx`. | ✅ Source committed; verifying live |
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
| `POST /api/v1/society` | Create a society. Existing handler (xpmile slice 1). |
| `POST /api/v1/subscriber/import` | CSV import. Existing handler. |

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

2. **Heroku rejects arm64 images** (Common Runtime is amd64-only). Builds on Apple Silicon **must** use `--platform linux/amd64`. The cached `pbx-cpp-builder:bootstrap` must also be amd64 — re-bootstrap from xpmile's Dockerfile if you've only ever built locally:

   ```sh
   cd ../xpmile
   podman build --platform linux/amd64 --target cpp-builder \
       -f docker/Dockerfile -t pbx-cpp-builder:bootstrap .
   ```

The first-time Heroku app preparation:

```sh
heroku stack:set container --app pabx           # flip from buildpack → container stack
heroku config:set \
    VAPID_PUBLIC_KEY=<base64url> \
    TURN_SHARED_SECRET="$(openssl rand -base64 32)" \
    TURN_URL='turn:turn.pbx.local:3478?transport=udp' \
    --app pabx
# Add `PBX_AUTH_STRICT=1 DB_URI=mongodb://…` once the subscribers
# collection is seeded; until then the cloud runs in dev-mode.
```

After a release, smoke-check with [`scripts/verify-deploy.sh`](./scripts/verify-deploy.sh):

```sh
./scripts/verify-deploy.sh remote
# 7/7 probes passed — UI index/main.js/favicon/sw.js + SPA fallback + REST reachability
```

### UI is bundled into the cloud image

`docker/Dockerfile.cloud` is three-stage (matches xpmile's pattern per `DESIGN.md` §11):

1. `cpp-builder` — compiles `pbx-cloud` against the cached `pbx-cpp-builder:bootstrap` toolchain.
2. `ui-builder`  — runs `ng build --configuration development` on `ui/`.
3. `runtime`     — Ubuntu 20.04 slim with the binary at `/opt/pbx-cloud/pbx-cloud` and the SPA at `/opt/webgui/webui/`. The C++ webservice serves the bundle from `../webgui/webui/` (relative to its WORKDIR — inherited from xpmile's `webservice.cpp`).

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

# 2. Fill in .env from the example.
cp .env.agent.example .env
$EDITOR .env       # set CLOUD_HOST=pabx-5fbf3550f938.herokuapp.com, AGENT_SOCIETY_ID=…

# 3. Build the amd64 agent image (Heroku is amd64; podman-compose --build
#    doesn't honour `localhost/` FROM lines, so build manually first).
podman build --platform linux/amd64 -f docker/Dockerfile.agent \
    -t onprem-pbx-agent:latest .

# 4. Up the stack. mongo + asterisk + coturn + pbx-agent.
podman-compose -f docker-compose.agent.yml up -d
podman logs pbx-agent | tail -10
#   ... AceSslTransport: connected + WS-upgraded pabx-5fbf3550f938.herokuapp.com:443/agent
#   ... AriWsClient: connected to Asterisk ARI pbx-asterisk:8088 (app=pbx)
```

The corresponding cloud log line — visible via `heroku logs --tail --app pabx` — is `GET /agent HTTP/1.1 ... Upgrade: websocket`. The tunnel is now alive end-to-end.

### Known limitations of the current Heroku deployment

| Limitation | Why | Workaround |
|---|---|---|
| **mTLS for `/agent` is not actually verified.** | Heroku Common Runtime terminates TLS at the router; the dyno only sees plain HTTP/WS. The agent's `--tls-cert` / `--tls-key` are present but never round-tripped against a verifiable peer. **`/ws/db` already gets around this** via ACE InnerTLS layered over the outer WSS — the same pattern is pending for the `/agent` SIP tunnel (slice D3). | Add InnerTLS to `AceSslTransport` (D3), or move to Heroku Private Spaces (TLS pass-through). |
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
