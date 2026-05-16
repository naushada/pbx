# mongodb — MongoDB client pool wrapper

> **Status:** ✅ Verbatim copy of the upstream shared-library module — do not modify locally (Layer 1). `MongodbClient.ImplementsIMongodbClient` passes; integration tests requiring a live Mongo land later with a fixture.

Thin, thread-safe wrapper around `mongocxx::pool`. One `MongodbClient` per process (the `mongocxx::instance` is a singleton). Every worker thread in the `webservice` module's `MicroService` pool shares the same client; `pool::acquire()` is thread-safe so no external locking is required.

Database name for onprem-pbx is **`pabx`** ([`DESIGN.md §4`](../../../DESIGN.md#4-data-model-mongodb)). Connection URI in both `pbx-agent` and the `wsdbagent`-tunneled cloud points at `mongodb://<host>/pabx`. (Project + container names retain the `pbx-` prefix; only the Mongo DB name is `pabx`, matching the Heroku app name.)

## Collections

See [`DESIGN.md §4 Data model`](../../../DESIGN.md#4-data-model-mongodb) for the full schema:

- `societies` — society metadata, SIP realm, TURN shared secret, admission cap.
- `flats` — apartments addressable by `number` (e.g. `A-204`).
- `subscribers` — login identity, `sipHa1`, `portalPasswordHash`, role.
- `cdr` — one row per logical call (all forked legs collapsed).
- `push_subscriptions` — Web Push endpoints per subscriber.
- `audit` — admin actions + guard-initiated calls.

## Indexes

- `flats(societyId, number)` unique
- `subscribers(societyId, sipUsername)` unique
- `subscribers(societyId, email)` unique
- `subscribers(societyId, role)` — used for forked-ringing the guard extension
- `cdr(societyId, startedAt)`

## Origin

The module ships as `modules/module/mongodb/` (note the dir is `mongodb`; the header is `mongodbc.hpp`). Verbatim copy of the upstream shared-library module — no API divergence is expected, do not modify locally. Patterns reused: atomic counter via `findOneAndUpdate + $inc` (used here for unique `sipUsername` generation during CSV import).

## Tests

A `Mongodbc*` suite (Layer 2 alongside `AriClient*` work) will exercise the CRUD paths actually used by the cloud and the agent. See [`TDD-PLAN.md`](../../../TDD-PLAN.md).
