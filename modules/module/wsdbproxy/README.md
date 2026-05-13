# wsdbproxy — Mongo-over-WSS proxy (cloud side)

> **Status:** ⏳ Empty. Copied verbatim from xpmile in **Layer 2**.

Cloud-side counterpart to the on-prem `wsdbagent`. Replaces `MongodbClient` in remote-db mode: every DB call is serialized as BSON, sent down the mTLS WSS tunnel to the agent, executed against the on-prem MongoDB, and the result is sent back.

For onprem-pbx this is **the only** way the Heroku cloud reads/writes Mongo — there is no Mongo instance on Heroku. See [`DESIGN.md §4`](../../../DESIGN.md#4-data-model-mongodb).

## Components

- `WsDbProxy` — drop-in replacement for `MongodbClient`. Same public API.
- `WsFrame` / `DbProto` — wire-format helpers. Distinct from the [`pbx/sip_frame`](../pbx/README.md) multiplex format; this is xpmile's existing DB-call protocol.

## Origin

Verbatim copy from `xpmile/modules/module/wsdbproxy/`. Identical behaviour; the cloud sees Mongo through a wsdbagent connection that ALSO carries our new `SipFrame` multiplex traffic on a separate WSS upgrade path. No conflict — they are two upgrades on the same listener.

## Tests

`WsProxy*`, `WsFrame*`, `DbProto*`, `WsDbServer*` (xpmile origin) — copied alongside the source. All must remain green; regression guard for the copy.
