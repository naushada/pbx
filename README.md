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
| 3+  | `BrowserStream` (retires `/sip-ws` 503), `AceSslTransport`, agent WS decoder, Tunnel E2E, Angular UI, Playwright | ⏳ Not started |

**Test totals: 253/253 passing across 27 suites** — our 141 (HttpParser 20, MessageParserBase 8, SipParser 17, SipFrame 10, SipBridge 12, MicroServicePbx 11, PushSender 8, MicroServiceRouting 7, SipFrameDemux 14, CloudConnector 11, AriClient 11, CloudTunnelEndpoint 12) + 112 inherited from xpmile (regression guard).

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

## Build & run (Layer 0)

All container operations use **podman** (not docker). Same toolchain as xpmile.

```sh
# Build the test image. Reuses the cached `pbx-cpp-builder:bootstrap`
# image (a tagged snapshot of xpmile's cpp-builder stage with ACE/TAO 7.0.0,
# googletest, and mongo-cxx-driver already installed under /usr/local).
# Build time: ~30–60 s.
podman build -f docker/Dockerfile.test -t onprem-pbx-test:layer1 .

# Run all GTest suites.
podman run --rm onprem-pbx-test:layer1

# Filter to a specific suite (override the entrypoint to pass flags).
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer1 \
  --gtest_filter='SipParser.*'
```

If `pbx-cpp-builder:bootstrap` is not present locally (e.g. on a fresh
machine), see [`docker/Dockerfile.test`](./docker/Dockerfile.test) — a
from-scratch Dockerfile mirroring xpmile's full cpp-builder lands with
Layer 1 when we need the rest of the toolchain.

## Repo layout

```
modules/module/
  http/         # MessageParser base + Http subclass (xpmile origin, refactored)
                #   inc/  message_parser.hpp, http_parser.hpp
                #   src/  message_parser.cpp,  http_parser.cpp
                #   test/ httpparser_test.{cc,hpp} (regression),
                #         message_parser_test.cc
  sip/          # Sip subclass + compact-header alias table (new)
                #   inc/  sip_parser.hpp
                #   src/  sip_parser.cpp
                #   test/ sip_parser_test.cc
  pbx/          # Cloud-side tunnel framing + (later) SipBridge, PushSender
                #   inc/  sip_frame.hpp
                #   src/  sip_frame.cpp
                #   test/ sip_frame_test.cc
  webservice/   # ACE WebServer / WebConnection / MicroService — Layer 1
  mongodb/      # MongodbClient pool — copied with Layer 2
  wsdbproxy/    # cloud-side Mongo-over-WSS proxy — copied with Layer 2
  email/        # CSV-import credential emails — copied with Layer 1

pbx-agent/      # On-prem daemon (ACE_SSL_SOCK_Connector dial-out,
                # SipFrameDemux, AriClient) — Layer 2.

ui/             # Angular softphone (Clarity + SIP.js + Service Worker)
                # — Layer 5.

docker/         # Dockerfiles. Today: Dockerfile.test (Layer 0 fast path).
docs/           # Long-form design docs and security analysis.
test/           # offtarget binary scaffolding (CMakeLists.txt, main.cc).
scripts/        # Build/deploy helpers.
certs/          # Local-dev cert material (gitignored except templates).
```

## Implementation order

See [TDD-PLAN.md → Order of work](./TDD-PLAN.md#order-of-work-drives-the-implementation). Each layer is fully green before the next starts.
