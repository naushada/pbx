# webservice — ACE reactor + per-socket handler + worker pool

> **Status:** ✅ Copied verbatim from xpmile (Layer 1). Extensions for SIP-WS/agent upgrade hand-off, new route handlers, and the `HandoffOrdering` test land in the upcoming slices.

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
