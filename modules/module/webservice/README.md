# webservice — ACE reactor + per-socket handler + worker pool

> **Status:** ✅ Copied verbatim from xpmile (Layer 1), then patched with two onprem-pbx additions: `MicroService::dispatch_pbx_routes` (REST URI table) and a `/sip-ws` upgrade gate in `WebConnection::handle_input`. The `HandoffOrdering` test from the TDD plan is deferred to Layer 3 TunnelE2E.

## Onprem-pbx patches

**1. `MicroService::dispatch_pbx_routes(req, db) -> string`** (new method)

Intercepts PBX URI prefixes before the xpmile route chain so neither `handle_GET` nor `handle_POST` had to be touched (keeps xpmile's regression tests untouched). Routes:

| URI | Method | Handler |
|---|---|---|
| `/api/v1/society` (exact) | `POST` | `MicroServicePbx::handle_society_POST` |
| `/api/v1/subscriber/import` (prefix) | `POST` | `MicroServicePbx::handle_subscriber_import_POST` |
| `/api/v1/cdr` (prefix) | `GET` | `MicroServicePbx::handle_cdr_GET` |
| `/api/v1/push/subscribe` (exact) | `POST` | `MicroServicePbx::handle_push_subscribe_POST` |
| _anything else_ | — | returns empty → caller falls through to xpmile dispatch |

`process_request` calls `dispatch_pbx_routes` first; only if it returns empty does it consult xpmile's method-based chain.

Pinned by **`MicroServiceRouting*`** (7 tests in `modules/module/pbx/test/microservice_pbx_test.cc`):

- `RoutesSocietyPost`, `RoutesSubscriberImportPost`, `RoutesCdrGet`, `RoutesPushSubscribePost` — each PBX URI fires the right handler.
- `FallsThroughOnXpmileUri` (`/api/v1/account/login` returns empty), `FallsThroughOnUnknownUri`, `FallsThroughOnWrongMethod` (`GET /api/v1/society`) — xpmile routes stay reachable.

**2. `/sip-ws` upgrade gate in `WebConnection::handle_input`** (added branch)

When a WS upgrade for `/sip-ws` arrives:

1. Call `MicroServicePbx::handle_sipws_upgrade(request)`. Non-empty return = 401 (no `session=…` cookie). Send and close.
2. Auth ok → send a `503 Service Unavailable` with `X-PBX-Hint: SipBridge tunnel endpoint not yet wired` and close.

Step 2 is a stub: the real `SipBridge::on_browser_upgrade` hand-off needs the cloud-side tunnel endpoint (`CloudTunnelEndpoint`), which lands in Layer 2. The auth-gate behaviour is pinned by `MicroServicePbx.Auth_RejectsAnonymousSipWsUpgrade` / `Auth_AllowsSipWsUpgrade_WithSessionCookie`.

The xpmile `/ws/db` upgrade branch (immediately below the `/sip-ws` branch in the source) is **unchanged** — both upgrades coexist on the same `WebConnection::handle_input` path with identical hand-off mechanics.

## Upcoming (Layer 2)

- Cloud-side `CloudTunnelEndpoint` — the `IPushHttpClient`/`TunnelSink` implementation that actually wraps an `ACE_SSL_SOCK_Stream` to the agent.
- Replace the `/sip-ws` 503 stub with the real hand-off (`remove_handler → m_handle = INVALID → bridge.on_browser_upgrade(raw)`).
- `HandoffOrdering` test (works end-to-end against the real reactor; cheaper than reactor mocking).

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
