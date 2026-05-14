# webservice — ACE reactor + per-socket handler + worker pool

> **Status:** ✅ Copied verbatim from xpmile (Layer 1), then patched with two onprem-pbx additions: `MicroService::dispatch_pbx_routes` (REST URI table) and a `/sip-ws` upgrade gate in `WebConnection::handle_input`. The `HandoffOrdering` test from the TDD plan is deferred to Layer 3 TunnelE2E.

## Onprem-pbx patches

**1. `MicroService::dispatch_pbx_routes(req, db) -> string`** (new method)

Intercepts PBX URI prefixes before the xpmile route chain so neither `handle_GET` nor `handle_POST` had to be touched (keeps xpmile's regression tests untouched). Routes:

| URI | Method | Handler |
|---|---|---|
| `/api/v1/society` (exact) | `POST` | `MicroServicePbx::handle_society_POST` |
| `/api/v1/subscriber/import` (prefix) | `POST` | `MicroServicePbx::handle_subscriber_import_POST` |
| `/api/v1/subscriber/login` (exact) | `POST` | `MicroServicePbx::handle_subscriber_login_POST` |
| `/api/v1/cdr` (prefix) | `GET` | `MicroServicePbx::handle_cdr_GET` |
| `/api/v1/push/subscribe` (exact) | `POST` | `MicroServicePbx::handle_push_subscribe_POST` |
| _anything else_ | — | returns empty → caller falls through to xpmile dispatch |

`process_request` calls `dispatch_pbx_routes` first; only if it returns empty does it consult xpmile's method-based chain.

Pinned by **`MicroServiceRouting*`** (8 tests in `modules/module/pbx/test/microservice_pbx_test.cc`):

- `RoutesSocietyPost`, `RoutesSubscriberImportPost`, `RoutesSubscriberLoginPost`, `RoutesCdrGet`, `RoutesPushSubscribePost` — each PBX URI fires the right handler.
- `FallsThroughOnXpmileUri` (`/api/v1/account/login` returns empty), `FallsThroughOnUnknownUri`, `FallsThroughOnWrongMethod` (`GET /api/v1/society`) — xpmile routes stay reachable.

**2. `/agent` WebSocket upgrade hand-off** (Layer 3)

Mirror of xpmile's existing `/ws/db` upgrade branch. When the on-prem agent dials in to `/agent`, `WebConnection::handle_input`:

1. Detects the WS upgrade (GET `/agent` + `Sec-WebSocket-Key`).
2. Sends the `101 Switching Protocols` response.
3. xpmile-mechanic hand-off ordering: `remove_handler → m_handle = INVALID`.
4. Constructs an [`AgentStream`](../pbx/README.md#agentstream--ace-event-handler-for-the-clouds-agent-socket) on the raw fd. The constructor calls `CloudTunnelEndpoint::on_agent_connected(adapter)` immediately, so `has_agent()` flips true.
5. `as->reactor(reactor()) + as->register_with_reactor()` — registers `READ_MASK` **and** arms the keep-alive ping timer; rolls the registration back on partial failure so the caller's `delete` stays safe.
6. `connectionPool().erase(raw)`.

The Layer 2 "info log only" stub is retired. `AgentStream`'s 10-test suite covers WS decode/encode, ping/pong, the keep-alive ping timer, close handling, frame-boundary edge cases, and the endpoint-initiated release path — all via `socketpair()`, no reactor needed.

**3. `/sip-ws` upgrade — real BrowserStream hand-off** (Layer 3)

When a WS upgrade for `/sip-ws` arrives:

1. Call `MicroServicePbx::handle_sipws_upgrade(request, *parent().mongodbcInst())` (503 if the Mongo client is null). It resolves the browser's `?token=`/`session=` cookie against the `sessions` collection and returns a `SipWsUpgrade{error, open_meta}` — a non-empty `error` (401 for an absent/unknown/expired session) is sent and the socket closed.
2. If the control plane isn't fully wired (`cloudTunnelEndpoint()` / `sipBridge()` null, or no agent connected) → 503 with `X-PBX-AgentConnected:` + `X-PBX-Hint:` headers so monitoring can distinguish the failure modes.
3. Otherwise: complete the WS handshake (`101 Switching Protocols` + `Sec-WebSocket-Accept`), perform the xpmile-mechanic hand-off (`remove_handler → m_handle = INVALID → publish raw fd`), construct a [`BrowserStream`](../pbx/README.md#browserstream--ace-event-handler-for-the-browsers-sip-ws-socket) on the raw fd **with the resolved `open_meta`** (`{societyId, sipUsername, clientUA}`), register it with the same reactor, and release this WebConnection from the pool.

`BrowserStream` (in the `pbx/` module) owns the socket lifetime from there. Its 9-test suite covers WS decode/encode, ping/pong, the keep-alive ping timer, close handling, and frame-boundary edge cases via `socketpair()` — no reactor needed for unit tests.

Session validation + identity resolution is pinned by `MicroServicePbx.Auth_RejectsAnonymousSipWsUpgrade`, `Auth_AllowsSipWsUpgrade_WithSessionCookie`, `SipWsUpgrade_ResolvesSubscriberMeta_FromQueryToken`, `SipWsUpgrade_RejectsUnknownToken`, `SipWsUpgrade_RejectsExpiredSession`.

The xpmile `/ws/db` upgrade branch is **unchanged** — three upgrades (`/ws/db`, `/agent`, `/sip-ws`) now coexist on the same `WebConnection::handle_input` path with the same hand-off mechanics.

## HandoffOrdering invariant

All three WS upgrade branches (`/ws/db`, `/sip-ws`, `/agent`) follow the xpmile-CLAUDE.md mechanic:

```
m_handedOff = true                       // 1
m_stream.set_handle(ACE_INVALID_HANDLE)  // 2
reactor()->remove_handler(this, …)       // 3  ── must precede #4
m_handle = ACE_INVALID_HANDLE            // 4
<publish raw fd to subsystem>            // 5
parent().connectionPool().erase(raw)     // 6  ── deletes `this`
```

The fatal mistake is swapping (3) and (4). `remove_handler` calls `get_handle()` internally to find which fd to deregister from epoll; if `m_handle` is `ACE_INVALID_HANDLE` first, the deregistration silently no-ops, the fd stays in epoll, and the reactor dispatches to the already-deleted `WebConnection` the next time the socket is readable.

This invariant is defended by [`test/integration/handoff_ordering_test.cc`](../../test/integration/handoff_ordering_test.cc) — a **source-invariant test** that reads `webservice.cpp` directly and asserts `remove_handler` precedes `m_handle = ACE_INVALID_HANDLE` for all three branches. Also verifies (5) publish happens after (3) remove, (2) stream's set_handle happens before (3), and (1) `m_handedOff = true` happens before everything else.

The source-grep approach was chosen over a real-reactor end-to-end test because the bug we're guarding against ("swap two adjacent lines") can pass platform-dependent reactor tests but is unambiguous in the source. xpmile's `/ws/db` branch is included as a regression guard against drift in the xpmile copy.

The cloud's HTTP/WSS-serving spine. Three classes copied near-verbatim from xpmile's `modules/module/webservice/` and then extended:

| Class            | Role |
|------------------|------|
| `WebServer`      | Owns the `ACE_Reactor`, the `MongodbClient` pool, and a vector of `MicroService` worker threads. One per process. |
| `WebConnection`  | Per-socket handler. Subclass of `ACE_Event_Handler`. Buffers partial reads until `Http::message_length() > 0`, then enqueues a `WorkCtx*` on the next `MicroService` (round-robin). Detects WS upgrades for `/sip-ws` (browser) and `/agent` (pbx-agent dial-in). |
| `MicroService`   | `ACE_Task` worker thread. Dequeues `WorkCtx`s, dispatches `handle_<resource>_<METHOD>` per URI prefix. All business logic lives here. |

**Extensions over xpmile (Layer 1+):**

- New WS-upgrade hand-off paths in `WebConnection`: `/sip-ws` (publish socket to `SipBridge` in the [`pbx/`](../pbx/README.md) module) and `/agent` (publish to the agent-tunnel acceptor).
- New `MicroService` route handlers: `handle_society_*`, `handle_subscriber_*` (incl. CSV import), `handle_cdr_*`, `handle_push_*`, `handle_turn_credentials_*`.

**Hand-off mechanics.** Identical to xpmile's `/ws/db` pattern, documented in [xpmile's CLAUDE.md §"WebSocket hand-off mechanics"](../../../../xpmile/CLAUDE.md). Order matters: `reactor()->remove_handler(this, READ_MASK | DONT_CALL)` → `m_handle = ACE_INVALID_HANDLE` → publish raw fd to bridge → `connectionPool().erase(raw)`. If `m_handle` is cleared first, `remove_handler` calls `get_handle()`, gets `-1`, and the fd is never removed from epoll — the reactor then dispatches to a deleted `WebConnection` next time the socket is readable.

## Origin

Files will be copied from `xpmile/modules/module/webservice/` and patched (not re-derived). Per the project rule documented in [`DESIGN.md §12`](../../../DESIGN.md#12-reuse-map-dry-against-xpmile).

## Dependencies (when populated)

- ACE / ACE_SSL
- [`http/`](../http/README.md) — `Http::message_length()` drives the socket read loop; `Http` parses each request before dispatch.
- [`mongodb/`](../mongodb/README.md) — `MongodbClient` pool reference is held by `WebServer` and reused by every worker.
- [`wsdbproxy/`](../wsdbproxy/README.md) — alternative DB path in remote-db mode (Heroku).

## Tests

A `WebServicePbx*` suite (Layer 1) will land in `test/` covering the new route handlers and WS-upgrade hand-offs. See [`TDD-PLAN.md` → Layer 1](../../../TDD-PLAN.md).
