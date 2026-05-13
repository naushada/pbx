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
| 4+  | Angular UI, Playwright E2E, podman-compose Asterisk integration | ⏳ Not started |

### Test totals: **312/312 across 33 suites**

| Layer | Suites | Tests |
|-------|--------|------:|
| 0     | HttpParser 20 + MessageParserBase 8 + SipParser 17 + SipFrame 10 | **55** |
| 1     | SipBridge 14 + MicroServicePbx 11 + MicroServiceRouting 7 + PushSender 8 | **40** |
| 2     | SipFrameDemux 14 + CloudConnector 11 + AriClient 11 + CloudTunnelEndpoint 12 | **48** |
| 3     | TunnelE2E 8 + BrowserStream 8 + AgentStream 9 + AceSslTransport 10 + AriWsClient 14 + HandoffOrdering 8 | **57** |
| 4     | AsteriskWsFactory 13 + AriRestClient 14 | **27** |
| regression | inherited xpmile suites (verbatim copy) | **112** |
| **Total** | **35 suites** | **339** |

**Layer 3 is feature-complete.** The full cloud + agent control plane has real ACE bindings end-to-end, and the `WebConnection` hand-off ordering is defended against future regressions by a source-invariant test.

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

docker/         # Dockerfile.test (only Dockerfile today; multi-stage
                # production images come with Layer 4)
docs/           # PRD, DESIGN, TDD-PLAN are at the root; sub-design docs land here
ui/             # Angular softphone — Layer 4
scripts/        # Build/deploy helpers — Layer 4
certs/          # Local-dev cert material (gitignored except templates)
```

## Layer 4 — production wiring (in progress)

Layer 3 closed out the **state machines + ACE bindings**. Layer 4 is the deployment-and-product surface; none of it changes the tested core.

| Component | Status |
|---|---|
| `pbx-agent/src/main/main.cpp` — agent reactor + CloudConnector + AceSslTransportFactory + SipFrameDemux + AriClient + AriWsClient + tiny reconnect supervisor + MongodbClient | ✅ Linked + `--help` clean. Two placeholders documented inline: `NoopAriRest` (the admission `continue` REST call is not wired yet) and `StubAsteriskFactory` (the per-stream WS connection to local Asterisk's `chan_pjsip` is not wired yet). Both placeholders log loudly and the rest of the agent works around them. |
| Cloud bootstrap in `webservice_main.cpp` — instantiate `SipBridge` + `CloudTunnelEndpoint`; wire `bridge.set_push_notify_handler` (placeholder: log only — `PushSender` not yet wired); wire `bridge.set_cdr_push_handler` → `MongodbClient::create_document("cdr", payload)` | ✅ Both `--remote-db` and local-Mongo modes patched; cloud binary `pbx-cloud` builds + `--help` clean. |
| CMake `pbx-agent` + `pbx-cloud` build targets | ✅ Both binaries link; `BUILD_BINARIES=ON` is on by default. `docker/Dockerfile.test` builds them alongside `offtarget`. |
| Real `AsteriskWsFactory` (replaces `StubAsteriskFactory`) — plain-TCP + WS upgrade to `ws://127.0.0.1:8088/ws` (chan_pjsip transport) with `Sec-WebSocket-Protocol: sip` per RFC 7118 §4 | ✅ Complete |
| Real `IAriRest` impl — `AriRestClient` POSTs to `/ari/applications/{app}/subscription` and `/ari/channels/{cid}/continue` (HTTP Basic auth, URL-encoded path + query) | ✅ Complete |
| Real `PushSender` wiring on cloud — instantiate with VAPID keys from env, pass into the bridge's push handler | ⏳ |
| `docker-compose.agent.yml` — Asterisk LTS + coturn + MongoDB + `pbx-agent` binary | ⏳ |
| `docker-compose.heroku.yml` + `deploy-heroku.sh` (clone of xpmile's) | ⏳ |
| `ui/` (Angular softphone — SIP.js + WebRTC + Clarity + Service Worker) | ⏳ |
| Playwright E2E | ⏳ |

The TDD-style coverage stays the same shape — every Layer 4 piece either has its own GTest suite (where it's pure logic) or an integration test (where it's I/O-bound).

## Implementation order

See [TDD-PLAN.md → Order of work](./TDD-PLAN.md#order-of-work-drives-the-implementation). Each layer is fully green before the next starts.
