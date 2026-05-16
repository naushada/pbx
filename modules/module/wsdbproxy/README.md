# wsdbproxy — Mongo-over-WSS proxy (cloud side)

> **Status:** ✅ Verbatim copy of the upstream shared-library module — do not modify locally (Layer 1). 46 inherited tests green (WsFrame 12, DbProto 13, WsDbServer 7, WsMongodbProxy 14); 1 test skipped — `WsDbServer.SecondAgentRejected_When_FirstAlive` drifted from the shared-library production code (see top-level [README §Skipped tests](../../../README.md)).

Cloud-side counterpart to the on-prem `wsdbagent`. Replaces `MongodbClient` in remote-db mode: every DB call is serialized as BSON, sent down the mTLS WSS tunnel to the agent, executed against the on-prem MongoDB, and the result is sent back.

For onprem-pbx this is **the only** way the Heroku cloud reads/writes Mongo — there is no Mongo instance on Heroku. See [`DESIGN.md §4`](../../../DESIGN.md#4-data-model-mongodb).

## Components

- `WsDbProxy` — drop-in replacement for `MongodbClient`. Same public API.
- `WsFrame` / `DbProto` — wire-format helpers. Distinct from the [`pbx/sip_frame`](../pbx/README.md) multiplex format; this is the shared-library's existing DB-call protocol.

## Origin

Verbatim copy of the upstream shared-library `modules/module/wsdbproxy/` — do not modify locally. Identical behaviour; the cloud sees Mongo through a wsdbagent connection that ALSO carries our new `SipFrame` multiplex traffic on a separate WSS upgrade path. No conflict — they are two upgrades on the same listener.

## Tests

`WsProxy*`, `WsFrame*`, `DbProto*`, `WsDbServer*` (inherited from the shared library) — copied alongside the source. All must remain green; regression guard for the copy.
