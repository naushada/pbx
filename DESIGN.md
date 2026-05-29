# onprem-pbx — Design

VoIP PBX for residential societies. Heroku-hosted control plane + on-prem `pbx-agent` running Asterisk/coturn/MongoDB. Web softphone in the browser. SIP for signaling, RTP/SRTP for media.

---

## 1. Goals and constraints

**Functional**
- Subscribers register from a web softphone and call other subscribers in the **same society** by **flat number**.
- One flat may have multiple subscribers (owner + family + tenants); each has their own SIP identity. Inbound to a flat forks to all registered endpoints.
- Security guards at the main gate are first-class subscribers (well-known extension: digit `0`).
- 1:1 calls keep media P2P. Conference calls within a flat use Asterisk ConfBridge on `pbx-agent`.
- Media is encrypted with **DTLS-SRTP** (RFC 5764). End-to-end for 1:1; per-leg with Asterisk for conferences (see §8).
- Persisted CDRs, hold / mute / DTMF, Web Push (VAPID) wake-up for incoming calls when the browser tab is closed.
- v1 capacity cap: **5 concurrent calls** per society (admission-controlled).

**Non-functional**
- Heroku web dyno: HTTP/WSS only on `$PORT`. No UDP. No inbound to `pbx-agent` through Heroku.
- All new network code in **C++ using ACE_*** APIs, following the shared-library `WebServer` / `WebConnection` / `MicroService`, `ACE_SSL_SOCK_Connector`, `ACE_Reactor`, `ACE_Message_Block` queue patterns.
- Per-society mTLS CA generated at install time. Heroku auto-cert (Let's Encrypt) for public WSS.
- Reuse the shared-library MongoDB client pool (`MongodbClient`).
- Society opens **one public UDP port** with DNAT to coturn on `pbx-agent`.

**Out of scope (v1)**
- PSTN/SIP-trunk to outside world.
- Call recording.
- Voicemail / IVR.
- Federation between societies.

---

## 2. Topology

```
                                  Internet (public)
                                          │
                ┌─────────────────────────┴─────────────────────────┐
                │ Heroku app: onprem-pbx-cloud   (C++ / ACE)        │
                │  ├─ WebServer (ACE reactor) on $PORT              │
                │  │   ├─ /portal/*       Angular static UI         │
                │  │   ├─ /api/v1/*       REST  (subscribers, CDR…) │
                │  │   ├─ /sip-ws         WSS upgrade — SIP-over-WS │
                │  │   └─ /agent          WSS upgrade — mTLS tunnel │
                │  ├─ SipBridge — multiplexes SIP-WS frames onto    │
                │  │   the single agent tunnel                      │
                │  ├─ PushSender — VAPID Web Push                   │
                │  └─ MongodbProxy → wsdbagent (shared-library)     │
                └────────────────────────┬──────────────────────────┘
                                         │  WSS, mTLS (per-society CA)
                                         │  (pbx-agent dials OUT)
            ┌────────────────────────────┴─────────────────────────┐
            │ Society LAN  (pbx-agent host: Linux box, 4-core/8GB) │
            │                                                      │
            │  ┌────────────────┐   ws:5060   ┌──────────────────┐ │
            │  │ pbx-agent      │────────────▶│ Asterisk         │ │
            │  │ (C++ / ACE)    │             │  chan_pjsip      │ │
            │  │ — sip-bridge   │             │  ConfBridge      │ │
            │  │ — wsdbagent    │             │  ARI (WS events) │ │
            │  └────────────────┘             └──────────────────┘ │
            │           │                                          │
            │           ▼                                          │
            │  ┌────────────────┐               ┌──────────────┐   │
            │  │ MongoDB        │               │ coturn       │   │
            │  └────────────────┘               │ (DNAT'd UDP) │   │
            │                                   └──────────────┘   │
            └──────────────────────────────────────────────────────┘
                       ▲                                 ▲
                       │ WSS (SIP-over-WS)               │ UDP/SRTP (ICE)
                       │ via Heroku tunnel               │ direct to coturn
                       │                                 │
              ┌────────┴──────────────────────────┐      │
              │ Browser softphone  (Angular)      │──────┘
              │  SIP.js + WebRTC + Service Worker │
              └───────────────────────────────────┘
```

The browser keeps **two** independent network paths:
1. **Signaling** — WSS to Heroku, tunneled to Asterisk. Always available as long as the cloud is reachable.
2. **Media** — UDP/SRTP directly to the other browser (or to coturn when relay needed). Never traverses Heroku.

---

## 3. Components

### 3.1 Heroku cloud app — `onprem-pbx-cloud` (C++ / ACE)

Single `uniservice`-style binary, modelled on the shared-library `WebServer` pattern. New module: `modules/module/pbx/`.

| Class (ACE-based)        | Role |
|--------------------------|------|
| `WebServer`              | Reused from the shared library. Owns reactor, Mongo proxy, MicroService pool. |
| `WebConnection`          | Per-socket handler. Detects WS upgrade for `/sip-ws` (browser) and `/agent` (pbx-agent). Hand-off pattern identical to the inherited `/ws/db`. |
| `MicroService` (extended)| Routes new endpoints: `POST /api/v1/society`, `POST /api/v1/subscriber/import`, `GET /api/v1/cdr/...`, `POST /api/v1/push/subscribe`, etc. |
| `SipBridge` *(new)*      | Owns the WSS agent socket(s). Frames each browser's SIP-WS traffic with a per-stream id; demux'es responses from the agent back to the right browser connection. Subclass of `ACE_Event_Handler`. |
| `PushSender` *(new)*     | Asynchronous Web-Push (VAPID-signed JWT). Uses `ACE_SOCK_Connector` + curl for HTTPS to push services. Triggered by an INVITE-arrived hook from `SipBridge`. |
| `WsDbServer`             | Reused as-is. Mongo access for cloud-side reads/writes (subscriber profiles, CDR replicated from agent, push subscriptions). |

Heroku app talks to MongoDB only via the existing wsdbagent tunnel. There is no SIP / media logic on Heroku — it is a **frame relay and a control-plane REST/UI host**.

### 3.2 On-prem `pbx-agent` (C++ / ACE)

A small daemon, structurally similar to the shared-library `wsdbagent`. New binary: `pbx-agent`.

| Class (ACE-based)            | Role |
|------------------------------|------|
| `CloudConnector`             | `ACE_SSL_SOCK_Connector` dial-out to Heroku `/agent`. Maintains the persistent mTLS tunnel. Reconnect/backoff loop. |
| `AceSslTransport`            | Outbound `ITransport` for `CloudConnector`. Outer TLS + WS upgrade + layered `InnerTlsClient` over the WS — the real mTLS trust boundary, since Heroku's router terminates the outer TLS. Mirror of `wsdbagent`'s `/ws/db` path; see §3.2 below and `ws_inner_tls_bridge.hpp`. |
| `SipFrameDemux` *(new)*      | Reads frames off the cloud tunnel; opens (or reuses) a local TCP socket to Asterisk's `ws://127.0.0.1:8088/ws` for each unique `stream-id`. Pipes bytes in both directions. |
| `AriClient` *(new)*          | HTTP REST client to Asterisk ARI for admission control (count active channels, enforce 5-call cap), CDR scraping, ConfBridge orchestration. `ACE_SOCK_Connector` + HTTP. |
| `PjsipProvisioner` *(new)*   | Materialises a subscriber row as three Asterisk sorcery objects (auth/aor/endpoint) via ARI dynamic-config PUTs (`/ari/asterisk/config/dynamic`). Idempotent — re-provisioning the same subscriber is the create-or-replace path. |
| `SubscriberWatcher` *(new)*  | Keeps Asterisk's pjsip endpoints in sync with the `subscribers` collection. Bootstraps via a society-scoped full-scan, then tails a Mongo change stream to react to inserts/updates/deletes in real time. Drives `PjsipProvisioner`. See §4.1. |
| `MongoSink`                  | Reuses the shared-library `MongodbClient` to persist CDRs and replicate subscriber records pushed from cloud. |

Third-party processes co-located on the agent host (not rewritten):
- **Asterisk** with `chan_pjsip`, WebSocket transport on `127.0.0.1:8088`, `directmedia=yes` for 1:1, `ConfBridge` for flat-group calls.
- **coturn** bound to LAN + the DNAT'd public UDP port. Configured with `use-auth-secret` (RFC 5766 §5 REST API). The cloud mints **time-limited TURN credentials** for each browser at REGISTER time: `username = <unix-ts-expiry>:<sipUsername>`, `password = HMAC-SHA1(secret, username)`. The shared secret lives in `societies.turnSharedSecret`. Browser gets these via `GET /api/v1/turn-credentials`; passes them straight into its `RTCConfiguration.iceServers`. Asterisk's own `ice_support=yes` is a separate flag enabling ICE on its RTP — unrelated to credential issuance.
- **MongoDB** (reused from the shared library).

### 3.3 Web softphone (Angular, in `ui/`)

- Angular workspace following the project's established conventions (Clarity, `$any()` cast pattern).
- Library: **SIP.js** for SIP-over-WS + WebRTC stack.
- Service Worker for VAPID push handler — on `notificationclick`, opens the portal which re-registers and accepts the incoming INVITE.
- Pages: login, dial-pad, contacts (search by flat), in-call (hold/mute/DTMF), call history, settings, conference room.

---

## 4. Data model (MongoDB)

There is exactly **one MongoDB instance per society**, deployed on the pbx-agent host. The Heroku cloud reads/writes it through the existing wsdbagent tunnel — there is no second copy in the cloud and no replication. When the agent is offline, the cloud portal is read-degraded until the tunnel returns (acknowledged limitation).

**Database name:** `pabx` (matches the Heroku app name `pabx`; the project + container + binary names keep the `pbx-` prefix). All collections below (`societies`, `flats`, `subscribers`, `cdr`, `push_subscriptions`, `audit`) live under `pabx.<collection>`. Connection URIs in both `pbx-agent` and the `wsdbagent`-tunneled cloud point at `mongodb://<host>/pabx`.

`_id` is ObjectId unless noted.

```js
societies: {
  _id, name, code, address,
  sipRealm,                                  // e.g. "<code>.pbx.local" — used in HA1
  publicTurnHost, publicTurnPort,            // society's DNAT'd UDP entry
  turnSharedSecret,                          // for time-limited TURN creds (RFC 5766 §5)
  maxConcurrentCalls: 5,                     // admission cap, bridge-count
  ringTimeoutSec: 30,
  createdAt
}

flats: {
  _id, societyId, number,                    // e.g. "A-204"
  block, floor
}

subscribers: {
  _id, societyId,
  flatId,                                    // null for role != "resident" (admin/guard)
  name, email, phone,
  sipUsername,                               // e.g. "u_<rand>" — unique within society
  sipHa1,                                    // MD5(sipUsername:sipRealm:sipPassword)
                                             // — Asterisk-compatible digest credential
  portalPasswordHash,                        // bcrypt — used only for portal login
  role: "resident"|"admin"|"guard",
  autoAnswer: bool,                          // guard kiosks
  status: "active"|"disabled",
  createdAt
}

cdr: {
  _id, societyId, callId,                    // SIP Call-ID
  fromSubscriberId, fromFlat,                // fromFlat stored as human string ("A-204")
  toFlat,                                    // string; null for guard extension "0"
  toSubscriberIds: [..],                     // every forked-ringed callee
  answeredBy: subscriberId|null,
  startedAt, answeredAt, endedAt,
  durationSec, hangupCause,                  // "normal"|"missed"|"busy"|"capacity_busy"|...
  type: "p2p"|"conference",
  conferenceBridge: string|null
}
// One row per logical call across all forked legs (not one per leg).

push_subscriptions: {
  _id, subscriberId,
  endpoint, p256dh, auth, expirationTime,    // standard Web Push fields
  userAgent, createdAt, lastSeenAt
}

sessions: {
  _id,
  token,                                     // random 16-byte hex — the
                                             //   `Set-Cookie: session=` value
                                             //   and the /sip-ws `?token=`
  email, societyId, sipUsername, role,       // resolved subscriber identity
  createdAt, expiresAt                       // role-based base TTL (resident: 1d, guard: 7d) + sliding refresh on /api/v1/ping and /sip-ws upgrade
}
// Written by POST /api/v1/subscriber/login; the /sip-ws upgrade resolves
// `token` here to build the bridge OPEN-frame metadata. `token` is a plain
// field, not `_id`, because the Mongo client's insert path assumes an
// ObjectId `_id`.

audit: {
  _id, ts, actor, action, target, details    // admin actions + guard-initiated calls
}
```

Indexes: `flats(societyId, number)` unique; `subscribers(societyId, sipUsername)` unique; `subscribers(societyId, email)` unique; `cdr(societyId, startedAt)` for history queries; `subscribers(societyId, role)` for forked-ringing the guard extension; `sessions(token)` unique, plus a TTL index on `sessions(expiresAt)` so expired rows self-evict.

Notes on roles:
- `resident` — must have `flatId`.
- `guard` — `flatId` null. Dialling `0` rings every subscriber with `role=guard` in the society.
- `admin` — `flatId` may be null (e.g. society secretary not living on-site). Cannot place SIP calls unless also assigned a flat.

### 4.1 Subscriber → Asterisk pjsip endpoint provisioning

The `subscribers` collection is the source of truth for who is allowed to
register over SIP. Asterisk needs a matching `pjsip` endpoint (+ auth +
aor) sorcery object for each. Two pieces close the loop on the agent side:

```
Mongo `subscribers` ──change stream──▶ SubscriberWatcher
                                            │
                                            ▼
                                     PjsipProvisioner
                                            │  PUT/DELETE
                                            ▼ /ari/asterisk/config/dynamic
                                        Asterisk sorcery (memory)
```

**Bootstrap** — on agent startup, `SubscriberWatcher::bootstrap()` does a
society-scoped full-scan of `subscribers` and provisions every active row.
Disabled rows get a deprovision (cheap, idempotent). The cache that maps
`_id → sipUsername` is populated here so a later change-stream `delete`
event (which only carries `documentKey._id`) can resolve the sipUsername.

**Live tail** — after bootstrap the watcher opens a Mongo change stream on
`subscribers`. A reactor timer ticks at 200 ms; each tick drains one event
and dispatches:

| Event                                  | Action |
|----------------------------------------|--------|
| `insert`/`update`/`replace`, `status=active`     | `provision(sipUsername, sipHa1)` |
| `insert`/`update`/`replace`, `status!=active`    | `deprovision(sipUsername)`       |
| `delete` (cached id)                                | `deprovision(sipUsername)`       |
| `delete` (unknown id) or other society's row     | dropped — not ours to act on   |

Change streams require Mongo to be a replica set (oplog). The
`pbx-mongo` service in `docker-compose.agent.yml` runs `mongod --replSet
rs0 --bind_ip_all` and the healthcheck doubles as the initiator (idempotent
`rs.initiate()`). On a standalone mongod `watch_collection()` returns null
and `tick()` becomes a silent no-op — bootstrap still works, but live
changes don't propagate until the next process restart.

**Per-subscriber sorcery objects** (PR #77 dropped SIP digest auth —
the cloud `/sip-ws` bearer-token upgrade is the sole auth layer; see
§5 below):

- `aor/<sipUsername>-aor` — `max_contacts=5`, `remove_existing=yes`.
- `endpoint/<sipUsername>` — codecs / ICE / DTLS / WebRTC settings inlined,
  plus `aors=<sipUsername>-aor`. **No `auth` field.** Sorcery
  dynamic-config does NOT inherit `(!)`-marked templates from
  `pjsip.conf`, so the settings live as constants in `PjsipProvisioner`
  and must be kept in sync with `docker/asterisk/pjsip.conf` when the
  template there changes.

PUTs are issued aor → endpoint so the endpoint never briefly references
a not-yet-created aor. DELETEs go in the reverse order (endpoint first
for in-flight INVITE safety; then a best-effort DELETE of the legacy
`<sipUsername>-auth` doc to prune pre-PR-#77 state; then aor). ARI
errors are logged-and-dropped at this layer; the next change-stream
event re-applies, and a stale local state heals naturally on the next
admin edit.

`provision()` also issues the legacy-auth DELETE at the start of every
pass — so existing Asterisk sorcery state from pre-fix deployments is
pruned the first time the subscriber is touched after upgrade. 404
(already gone) is silently absorbed by the ARI client.

---

## 5. Authentication

Two distinct credentials per subscriber, both generated at CSV-import time, both shown ONCE in the admin's one-time-download CSV, never recoverable afterwards.

**Portal login**
- Email + portal password.
- Stored as `portalPasswordHash` (bcrypt, cost 12).
- Validated by `MicroService` on the Heroku cloud (`POST /api/v1/subscriber/login`).
- On success a row is written to the `sessions` collection (§4) and its `token` is returned three ways: an `HttpOnly; Secure; SameSite=Strict` session cookie, and in the JSON body so the UI can pass it as the `/sip-ws` `?token=` query param (browsers can't set headers on `new WebSocket`).
- The `/sip-ws` upgrade resolves that token against `sessions` — an absent / unknown / expired session is a 401. The resolved `societyId` / `sipUsername` become the bridge's `OPEN`-frame metadata (§7).

**SIP REGISTER (transport-only, PR #77 + #78)**

The browser's SIP UA carries **no** SIP password (`ui/src/common/sip-ua-sipjs.ts:18`). REGISTER reaches Asterisk over the cloud-mediated tunnel and is accepted at the transport layer; there is no 401 challenge / HA1 digest exchange. Auth happens once, at the cloud's `/sip-ws` WebSocket upgrade: the bearer token (`?token=…`) is resolved against the `sessions` collection and a 401 is returned for an absent / unknown / expired / disabled-subscriber token. After upgrade, the WS transport itself is the proof of identity for every SIP frame that flows over it.

Both provisioning paths are aligned with this:

- Dynamic (cloud-driven) endpoints — `PjsipProvisioner` no longer creates an `auth` sorcery object and the endpoint no longer carries an `auth=` field (PR #77).
- Static sample endpoints in `docker/asterisk/pjsip.conf` — the hardcoded `auth = …-auth` lines and `[…-auth]` sections for `[alice]` / `[bob]` were dropped (PR #78). The conference `[conf]` endpoint never had auth.

Why this is safe: only the cloud-mediated tunnel reaches this Asterisk (no direct SIP port exposed); the cloud's `/sip-ws` gate validates the bearer before opening the transport. A second digest layer on Asterisk was redundant and unanswerable.

**Vestigial fields** kept for now, removable in a follow-up:
- `subscribers.sipPassword` — still generated at CSV-import (returned in the one-shot download) but never used by Asterisk.
- `subscribers.sipHa1` — still stored but never consulted.
- `societies.sipRealm` — still propagated via `SOCIETY_BOOTSTRAP` tunnel frame and applied via `PjsipProvisioner::set_sip_realm`, but the realm field is no longer present on any provisioned auth object.
- Agent `--sip-realm` CLI flag — accepted but unused by provisioning.

**Authorization**
- Search by flat number is scoped to the caller's `societyId`.
- Admin endpoints gated by `role == "admin"` on the portal session.
- CSRF: state-changing REST endpoints require a custom header `X-CSRF-Token` echoed from a token returned at login (defence-in-depth beyond `SameSite=Strict`).

**Why two credentials are still in the data model**
Portal login uses bcrypt (slow KDF, correct for password storage), and the old SIP credential (`sipPassword`/`sipHa1`) was a deterministic MD5-friendly value SIP digest required. The latter is now unused at the auth layer, but the fields persist because:

1. The admin CSV export still surfaces `sipPassword` to operators — useful if anyone ever needs to swap in a native SIP client that *can* answer digest.
2. Removing the fields ripples into `handle_subscriber_import_POST`, the import-template CSV, every existing subscriber document, and the admin UI. Out of scope for the PRs that introduced bearer-only SIP auth.

---

## 6. Call flows

### 6.1 Registration

```
Browser ──WSS── Heroku /sip-ws ──tunnel── pbx-agent ──ws://── Asterisk
   │                                                              │
   │  REGISTER (digest)  ────────────────────────────────────────▶│
   │  401 (challenge)    ◀────────────────────────────────────────│
   │  REGISTER (auth)    ────────────────────────────────────────▶│
   │  200 OK             ◀────────────────────────────────────────│
```

Before opening the SIP-WS connection, the browser first calls `GET /api/v1/turn-credentials` over the portal session. The cloud responds with `{ urls: ["stun:<host>:<port>", "turn:<host>:<port>?transport=udp"], username, credential }` where `username/credential` are time-limited (RFC 5766 §5). The browser passes these into its `RTCConfiguration.iceServers`. Without this step, ICE has no candidates and media setup fails.

`SipBridge` assigns a monotonic `stream-id` per browser WSS at upgrade time; `SipFrameDemux` opens a matching socket to Asterisk and remembers the mapping. Heroku does no SIP parsing — it is byte-faithful framing.

### 6.2 1:1 call within a flat (P2P media)

1. `Alice@A-101` dials `B-204`.
2. SIP.js sends `INVITE sip:B-204@society.local` with SDP offer that lists ICE candidates from local + public STUN (society's coturn).
3. Asterisk dialplan (or ARI app) resolves `B-204` → all subscribers on flat `B-204` → forks INVITE to each registered AOR.
4. Each callee browser is awake or woken via VAPID push (see 6.4).
5. First callee answers; Asterisk sends `200 OK` with the callee's SDP back to Alice.
6. Because `directmedia=yes`, Asterisk emits re-INVITEs so the two browsers connect ICE directly. After ICE completion, SRTP flows browser↔browser (or via coturn if both NATs symmetric).
7. Either party `BYE` → Asterisk → `AriClient` receives the `ChannelDestroyed` event on its ARI WebSocket subscription and writes the CDR via `MongoSink`. (ARI is event-driven; we do not poll. AMI is unused.)

### 6.3 Conference within a flat (mixer)

- Anyone on the flat dials `*FLAT` (or selects "Group call" in the UI).
- ARI app creates a `ConfBridge`, sends `INVITE` to every registered AOR on the flat.
- All accepted legs join the bridge. Media is mixed on `pbx-agent`; one SRTP stream per leg between browser and Asterisk (NOT P2P). The flag `directmedia` is ignored for confbridge legs.
- CDR `type=conference` with all `toSubscriberIds` populated.

### 6.4 Incoming call with closed tab (VAPID)

1. INVITE arrives at Asterisk; ARI app fires a webhook into `pbx-agent` → tunneled up to Heroku → `PushSender.notify(subscriberId)`.
2. Heroku looks up `push_subscriptions` for that subscriber and POSTs a VAPID-signed payload to each browser's push endpoint.
3. Service Worker shows a notification "Incoming call from A-101".
4. User clicks → SW `clients.openWindow(/incoming?call=<id>)` → page registers, INVITE is still ringing (Asterisk uses `ringtime=30s`), browser sends `200 OK`.
5. If no answer within ringtime: `486 Busy` → CDR logs missed call → next-login UI shows it.

### 6.5 Admission control (5-call cap)

The cap is on **logical calls**, not Asterisk channels. A single 1:1 call creates two channels (caller leg + callee leg); a 3-way conference creates three. Naïvely counting `ChannelCreated` would fire the cap at 2.5 actual calls.

- `AriClient` subscribes to `BridgeCreated` / `BridgeDestroyed` on its ARI WebSocket.
- A counter of active bridges per society. (Asterisk creates a mixing bridge for both 1:1 and conference calls; a bridge corresponds 1:1 with a "logical call".)
- On INVITE arrival, the ARI Stasis app checks the counter against `societies.maxConcurrentCalls`. If at or over the cap, it `Hangup`s the new channel with cause `21` (call rejected) and a `503 Service Unavailable` propagates to the caller. CDR row written with `hangupCause = "capacity_busy"`.

---

### 6.6 Tunnel drop mid-call

The tunnel only carries **signaling**. RTP/SRTP is P2P (or via coturn) and does not depend on the tunnel. So:

- An active 1:1 call's audio **survives** a tunnel drop. Users keep talking.
- New REGISTERs, new INVITEs, and BYEs cannot be conveyed while the tunnel is down.
- If a user hangs up during the outage, the BYE is queued client-side by SIP.js and replayed when the WSS reconnects. If the browser is closed first, Asterisk eventually times the channel out and writes a CDR with `hangupCause = "tunnel_lost"`.
- Conference (ConfBridge) calls **do** require the tunnel for new joins, but existing legs survive — RTP is still flowing browser↔Asterisk on the on-prem LAN/public coturn port, not through the tunnel.
- **Idle-drop prevention (first line of defence).** Heroku's router H15-drops a WebSocket after 55 s with no bytes in *either* direction — and a connected call generates no signalling traffic mid-conversation. The cloud-side `/agent` and `/sip-ws` handlers (`AgentStream` / `BrowserStream`) each arm a 25 s reactor timer that sends a WebSocket-level ping (RFC 6455 opcode `0x9`), so an otherwise-idle socket is never dropped in the first place. This sits *underneath* the SipFrame-level `PING`/`PONG` (§7) and the `CloudConnector` reconnect below — the keep-alive prevents the drop; reconnect recovers from one.
- `CloudConnector` reconnects with exponential backoff (1s → 30s cap). The TDD plan's `CloudConnector.AutoReconnectsWithBackoff` pins this.
- **Presence reconciliation on reconnect (PR #20).** `EndpointStateChange` events only fire on actual transitions — if a SIP user logs in or out while the tunnel is down, the cloud's `IPresenceCache` would stay stale until the next live event (which on a quiet society may be hours). Every successful reconnect triggers `CloudConnector::set_on_connected(...)` → `AriClient::publish_register_snapshot()`, which calls `GET /ari/endpoints/PJSIP` and emits one `REGISTER_STATE` SipFrame per endpoint. The cloud upserts the cache and the directory's `online` column re-syncs to ground truth.
- **Mongo flap separately.** The agent's `SubscriberWatcher` (DESIGN.md §4.1) tails a Mongo change stream on `subscribers`. A Mongo disconnect causes `try_next()` to throw; the watcher catches, drops the cursor, and reopens with `resumeAfter(captured_token)` (PR #21) so events that fell into the disconnect window get replayed — provided they're still in the oplog window. A 5-tick (~1s) backoff between failed reopens keeps the pool from being hammered during an extended outage. If the resume token has aged out, the watcher falls back to a fresh-from-now cursor; the next agent restart's `bootstrap()` re-applies all active state.

---

## 7. Tunnel framing (Heroku ↔ pbx-agent)

A single persistent WSS connection from pbx-agent → Heroku `/agent`, carrying many SIP-WS sessions plus out-of-band control messages.

```
 0       1       2               6               10
 +-------+-------+---------------+---------------+------------------------+
 |ver=1  | op    | stream-id (4) | payload-len(4)| payload (≤ 1 MiB)      |
 +-------+-------+---------------+---------------+------------------------+
```

All integer fields are big-endian. Header is fixed at 10 bytes. `payload-len > 1048576` → peer drops the connection and logs an error.

| op   | name         | direction           | payload |
|------|--------------|---------------------|---------|
| 0x01 | OPEN         | cloud → agent       | `{ societyId, sipUsername, clientUA }` (JSON) — agent opens a fresh socket to local Asterisk for this `stream-id` |
| 0x02 | DATA         | both ways           | raw SIP-WS frame bytes — opaque, byte-faithful |
| 0x03 | CLOSE        | both ways           | `{ reason }` (JSON) — stream-id is closed; both ends release resources |
| 0x04 | PING         | both ways           | empty — agent's `CloudConnector` sends one per 15 s of inbound silence; cloud's `SipBridge` echoes a PONG. The cloud's `CloudTunnelEndpoint` symmetrically PINGs the agent on inbound silence; the agent's `SipFrameDemux` echoes a PONG. |
| 0x05 | PONG         | cloud → agent       | empty — answer to PING. `CloudConnector` drops + reconnects the tunnel after 3 consecutive unanswered PINGs (no inbound bytes of any kind in between) |
| 0x06 | ERROR        | both ways           | `{ code, msg }` — protocol violation; sender closes the tunnel |
| 0x10 | PUSH_NOTIFY  | agent → cloud       | `{ subscriberId, callerFlat, callId }` — agent saw INVITE arrive, cloud should VAPID-push |
| 0x11 | CDR_PUSH     | agent → cloud       | BSON CDR doc — replicates finalized CDR to cloud for portal reads |
| 0x12 | SUBSCRIBER_REVOKED | cloud → agent | `{ societyId, sipUsername }` (JSON) — an admin disabled/removed the subscriber; the agent hangs up that subscriber's live Asterisk channels via ARI. stream-id unused (0). |
| 0x13 | REGISTER_STATE     | agent → cloud | `{ societyId, sipUsername, online }` (JSON) — emitted by `AriClient` on every Asterisk `EndpointStateChange` for a `PJSIP/<resource>` endpoint. The cloud caches it (per-society `IPresenceCache`) and serves the directory's `online` field from it. stream-id unused (0). |
| 0x14 | AGENT_HELLO        | agent → cloud | `{ societyId }` (JSON) — first frame the agent sends after every (re)connect, identifies which society this tunnel represents. Cloud responds with `SOCIETY_BOOTSTRAP`. stream-id unused (0). |
| 0x15 | SOCIETY_BOOTSTRAP  | cloud → agent | `{ societyId, sipRealm }` (JSON) — emitted in response to `AGENT_HELLO`. Cloud looks up `societies/{_id: societyId}`, extracts `sipRealm`, sends it back. Agent's `PjsipProvisioner.set_sip_realm()` adopts it (unless `--sip-realm` was passed on the CLI) and `SubscriberWatcher.resync()` re-PUTs every subscriber's auth object with the canonical realm. stream-id unused (0). |

Parsed in C++ with `ACE_InputCDR` / `ACE_OutputCDR` buffers (or hand-rolled big-endian readers — `ACE_CDR` defaults to platform-native byte order, so explicit `ACE_CDR::swap_4(...)` calls are required when targeting wire-format).

Multiplexing rationale: one mTLS socket per agent, not one per browser. Cuts handshake cost and Heroku connection-count overhead.

**Two keep-alive layers — don't conflate them.** The `0x04` / `0x05` `PING` / `PONG` above are *SipFrame-level*: they ride inside the tunnel payload and exist for end-to-end peer liveness. **Both ends own a heartbeat now** — the agent's `CloudConnector` and the cloud's `CloudTunnelEndpoint` are symmetric: each sends a `PING` after 15 s of inbound silence from the other side, treats *any* inbound bytes (a `PONG` on an idle tunnel, `DATA` during a live call) as proof the peer is alive, and drops the tunnel once 3 consecutive PINGs go unanswered. The agent then reconnects on its backoff; the cloud waits for the agent to re-establish `/agent`. Underneath them, every WebSocket *socket* — the agent's `/agent` and each browser's `/sip-ws` — also runs a *WebSocket-level* keep-alive: `AgentStream` / `BrowserStream` send an RFC 6455 ping (opcode `0x9`) every 25 s so Heroku's router doesn't H15-drop an idle socket (§6.6). The layers are independent: the WS-level ping keeps the *transport* open through an idle period; the SipFrame-level ping detects a *peer* that has gone away — hung, not closed — while the transport still looks healthy.

**Presence frames (op `0x13`, PR #16 + PR #20).** Two paths emit `REGISTER_STATE`:

1. **Live event-driven** — `AriClient::handle_endpoint_state_change` fires one frame per Asterisk `EndpointStateChange` ARI event. `state == "online"` maps to `online: true`; anything else (`offline`, `unknown`) maps to `false`. Only `PJSIP/<resource>` endpoints fire; other technologies are filtered.

2. **Bulk snapshot on (re)connect** — `CloudConnector::set_on_connected(...)` is wired to `AriClient::publish_register_snapshot()`, which calls `GET /ari/endpoints/PJSIP`, parses the array, and emits one `REGISTER_STATE` per entry. This closes the staleness window when the tunnel was down (§6.6). ARI errors during the snapshot are silently skipped — synthesising fake state would be worse than leaving the cache at its last known value until the next live event or reconnect.

**Inner-TLS over the `/agent` WS (PR #19).** Heroku's router terminates the *outer* TLS, so the dyno never sees the agent's client cert; mTLS at that layer is theatre. The agent's `AceSslTransport::connect_and_handshake()` performs the outer mTLS dial + WS upgrade, then layers an `InnerTlsClient` (from `modules/module/security/`) over the WS binary frames using `WsInnerTlsBridge` — a dual-mode `IInnerTlsTransport` that does blocking socket reads during the inner handshake and switches to a buffered queue (fed by `handle_input`) afterwards. The cloud's `AgentStream::setup_inner_tls()` runs the symmetric server handshake **before** `attach()` so any buffered outbound frames from a prior disconnect can't hit the wire un-encrypted. Same shared CA + cert triple as `/ws/db`; `docs/design/security/innertls.md` covers the cert layout. Post-handshake the cloud captures the agent cert's CN via `InnerTlsServer::peer_subject_cn()` (PR #25) into `AgentStream::peer_cn()` for log labels; the underlying latent bug (`SSL_CTX_use_certificate_file` set the cert on the CTX *after* `SSL_new` had already inherited from it, so the client never actually presented a cert) was fixed by switching to the per-SSL `SSL_use_certificate_file` API.

**Society-realm bootstrap (ops `0x14`/`0x15`, PR #24).** The agent's `--sip-realm` CLI flag defaults to `<society-id>.pbx.local`, but the cloud stores the canonical `sipRealm = "<code>.pbx.local"` on each society doc — when `societyId != code` the default doesn't match the realm the cloud used to compute `sipHa1`, and SIP REGISTER digests fail 401. The two-frame handshake closes this:

1. **`AGENT_HELLO`** is the agent's `CloudConnector::set_on_connected(...)` callback's first send, before `publish_register_snapshot`. Payload carries the agent's own `societyId` so the cloud knows which tunnel it's talking to (multi-tenant from day one — no `--cloud-society-id` env var needed).

2. **`SOCIETY_BOOTSTRAP`** is the cloud's `SipBridge::set_agent_hello_handler(...)` reply, after a `societies/{_id}` lookup. Carries `{societyId, sipRealm}`.

3. **Agent applies the realm** via `PjsipProvisioner::set_sip_realm()` (sets the realm for future provisions) + `SubscriberWatcher::resync()` (re-runs the full-scan + provision, so already-bootstrapped subscribers' auth objects get re-PUT with the canonical realm). `resync()` does NOT touch the change-stream cursor — the resume token stays valid for events queued during the resync. The CLI flag still wins if the operator passed `--sip-realm`.

Failure modes are quiet: missing society doc / missing `sipRealm` field / malformed JSON all log + drop, agent keeps whatever realm it had. The flow is exercised by 12 paired tests across `SipFrame`, `SipBridge`, `SipFrameDemux`, `PjsipProvisioner`, `SubscriberWatcher` test suites.

---

## 8. Media security (DTLS-SRTP)

The tunnel (§7) carries only signaling. Media security is a separate, end-to-end matter between WebRTC peers (or peer + Asterisk for conferences).

### 8.1 What's on the wire

| Layer | Protocol | Notes |
|---|---|---|
| Key exchange | **DTLS** (RFC 5764) | Browsers *mandate* DTLS-SRTP. SDES-SRTP (keys in SDP) was deprecated and removed from Chrome/Safari/Firefox. There is no other option to negotiate at the browser. |
| Media wire | **SRTP** (RFC 3711), AES-GCM | Keys derived from the DTLS handshake. DTLS does **not** encapsulate the RTP packets themselves — only the keying. The packets on the wire are SRTP. |
| Control wire | **SRTCP** | Same keys as SRTP. |
| Port sharing | **rtcp-mux** (RFC 5761) | RTCP shares the SRTP UDP port. One ICE candidate carries both. |
| MitM defence | `a=fingerprint:sha-256 …` in SDP | Each peer publishes its DTLS cert fingerprint in the offer/answer. The DTLS handshake is rejected if the presented cert doesn't match. This is what stops an SDP-mangling proxy from substituting its own cert. |

### 8.2 Two media-path encryption shapes

**1:1 with `directmedia=yes`** — DTLS-SRTP is **truly end-to-end** between the two browsers. Asterisk relays SDP (and the fingerprints inside it) but is never on the media path after ICE completes, never sees the master secret, never sees plaintext audio. The trapezoid model.

```
   Browser A ══DTLS handshake══ Browser B           (SDP fingerprints
       │       (direct, via       │                  carried through
       │        ICE candidates)   │                  Asterisk in clear,
       │                          │                  but the key material
       └──────── SRTP/SRTCP ──────┘                  is established directly)
                  (P2P)
```

**Conference (ConfBridge)** — each browser does its own DTLS handshake **with Asterisk** as the peer. Asterisk decrypts each leg, mixes in plaintext inside the bridge process, re-encrypts per-leg with that leg's key. Asterisk has plaintext audio access **inside the mixer process only**. This is an unavoidable property of any server-side mixer; the alternative (client-side SFU mesh) doesn't pay off for 3-on-a-flat conferences and would push CPU/bandwidth onto residents' devices.

```
   Browser A ──DTLS─┐
                    ├─→ Asterisk ConfBridge (plaintext mixer)
   Browser B ──DTLS─┤      │
                    │      └─→ each leg re-encrypted with its own SRTP key
   Browser C ──DTLS─┘
```

We document this asymmetry to society admins: **conference audio is encrypted on the wire but visible to the on-prem PBX process during mixing.** 1:1 audio is never visible to the PBX.

### 8.3 Asterisk configuration

`chan_pjsip` endpoint template in `pjsip.conf`:

```
[endpoint-resident-template](!)
type=endpoint
transport=transport-wss
media_encryption=dtls
dtls_verify=fingerprint
dtls_setup=actpass
dtls_cert_file=/etc/asterisk/keys/pbx.crt
dtls_private_key=/etc/asterisk/keys/pbx.key
dtls_rekey=0
rtcp_mux=yes
ice_support=yes
direct_media=yes        ; flipped to "no" for conference-only endpoints
```

The Asterisk DTLS cert (`/etc/asterisk/keys/pbx.{crt,key}`) is **separate** from the mTLS-tunnel cert. It's generated on first agent install, self-signed; the fingerprint published in SDP is what the browser verifies, not a CA. Rotation: regenerate on `pbx-agent` upgrades; in-flight calls are unaffected (cert is only consulted at handshake time).

For 1:1 directmedia, Asterisk's DTLS cert isn't actually used on the media path — only the browsers' certs are. The Asterisk cert matters only for conference legs and for early-media before re-INVITE completes.

### 8.4 What we don't do

- **No SRTP with SDES** — keys-in-signaling pattern. Browsers refuse, and it would also leak keys to anyone who can see the SDP (e.g. cloud `SipBridge` if it ever did parse SIP).
- **No ZRTP** — unsupported by browsers.
- **No plaintext RTP** as a fallback. `media_encryption=dtls` is hard-required; calls without DTLS fail at SDP negotiation.

---

## 9. Security guard at the main gate

- Modelled as a regular subscriber with `role = "guard"` and `flatId = null`. There is no special flat document — routing is role-based (`subscribers(societyId, role)` index).
- Well-known extension: digit `0`. The Asterisk dialplan resolves `0` → "fork-ring every active subscriber with `role=guard` in this society". Typically 1–2 kiosk endpoints.
- Per-account `autoAnswer: true` can be set on guard subscribers so the kiosk picks up without a click. Default off. Honoured by **both softphones**: web (`ui/src/common/sip.service.ts::SipService.onIncoming`) and mobile (`mobile/src/state/createSipEnv.ts` wires the controller's `shouldAutoAnswer` predicate to `sub.autoAnswer && sub.role === 'guard'`). The role check is defence-in-depth on both sides — `autoAnswer` on a resident is silently ignored.
- Guards can dial any flat using the same search-by-flat UI as residents, scoped to their `societyId`.
- Every guard-initiated call is written to the `audit` collection in addition to the normal CDR row.

---

## 10. Security checklist

- All WSS uses TLS 1.2+ (Heroku-managed cert public side; per-society mTLS CA inside the tunnel).
- Two credentials per subscriber (see §5): `sipHa1` (MD5, Asterisk-readable) + `portalPasswordHash` (bcrypt cost 12). Plaintext of either shown once on CSV-import download, then unrecoverable.
- Portal session cookie `HttpOnly; Secure; SameSite=Strict`.
- CSRF: state-changing REST endpoints (`POST/PUT/DELETE`) require an `X-CSRF-Token` header echoed from a per-session token issued at login.
- Rate-limit `/sip-ws` upgrade attempts per IP (defence against credential stuffing against Asterisk).
- VAPID keys generated per Heroku app deployment; private key in Heroku config var.
- coturn uses RFC 5766 §5 `use-auth-secret`. Cloud mints time-limited TURN credentials (5-minute TTL) on `GET /api/v1/turn-credentials`. The shared secret lives in `societies.turnSharedSecret`; rotated quarterly.
- CSV-import emails credentials over email **only**; SMS recommended as a follow-up out-of-band channel.
- **Clock sync**: pbx-agent host must run NTP. mTLS handshake and SIP digest both fail silently or weirdly with skewed clocks. Installer guide mandates `chrony` or `systemd-timesyncd` enabled at provisioning.

---

## 11. Deployment

| Artifact                 | Where                                    | How |
|--------------------------|------------------------------------------|-----|
| `onprem-pbx-cloud` image | Heroku app `onprem-pbx` (one container)  | `deploy-heroku.sh` |
| `pbx-agent` image        | On-prem host (society)                   | `docker-compose.agent.yml` |
| Asterisk image           | Same host as pbx-agent                   | Same compose file |
| coturn image             | Same host as pbx-agent                   | Same compose file |
| MongoDB                  | Same host as pbx-agent                   | Same compose file |
| Angular UI               | Bundled inside cloud image (`/portal`)   | Built in cpp-builder multi-stage |

CSV-import is a one-shot REST call `POST /api/v1/society/<id>/subscribers/import` (multipart). It generates SIP passwords, hashes them, returns a downloadable CSV of plaintext credentials to the admin.

---

## 12. Reuse map

Anything reused from the upstream shared library is **copied into `onprem-pbx/modules/...` verbatim or near-verbatim** so the two projects can evolve independently; no shared library coupling. The upstream source is the canonical reference; the copy here is patched only as needed.

| New file in onprem-pbx | Source in the shared library | Modifications |
|---|---|---|
| `modules/module/webservice/webserver.{hpp,cpp}` | `modules/module/webservice/webserver.{hpp,cpp}` | Add routes for `/sip-ws` and `/agent` WS upgrades. |
| `modules/module/webservice/webconnection.{hpp,cpp}` | same | Add WS upgrade detection for the two new paths; reuse the exact hand-off mechanics (`remove_handler` → clear `m_handle` → publish to bridge) — the well-known WebSocket hand-off invariant. |
| `modules/module/webservice/microservice.{hpp,cpp}` | same | Add `handle_subscriber_*`, `handle_society_*`, `handle_cdr_*`, `handle_push_*`. URI-prefix routing pattern unchanged. |
| `modules/module/http/inc/message_parser.hpp`<br>`modules/module/http/src/message_parser.cpp` | extracted from `http_parser.{hpp,cpp}` | New base class. Hosts the protocol-agnostic logic: `message_length` (CRLFCRLF + `Content-Length`), `get_header`, `parse_mime_header`, lowercased `m_tokenMap`, `pct_decode`. ~200 lines lifted unchanged. |
| `modules/module/http/inc/http_parser.hpp`<br>`modules/module/http/src/http_parser.cpp` | `http_parser.{hpp,cpp}` | Becomes a thin subclass of `MessageParser`. Keeps `parse_first_line()` (METHOD URI HTTP/1.1), query-string parsing, chunked/gzip body decode. Public API (`uri()`, `method()`, `body()`, `add_element`, `get_element`) preserved so the inherited code compiles unchanged. |
| `modules/module/sip/inc/sip_parser.hpp`<br>`modules/module/sip/src/sip_parser.cpp` *(new — landed in Layer 0)* | inherits from `MessageParser` | SIP-specific bits only: detects request vs status line (`SIP/2.0 …` prefix → status), parses `METHOD Request-URI SIP/2.0` or `SIP/2.0 <code> <reason>`, exposes `is_request()`, `status_code()`, `reason_phrase()`, `error()`. Compact-header alias table (`l`→`content-length`, `v`→`via`, `i`→`call-id`, `f`→`from`, `t`→`to`, `m`→`contact`, `c`→`content-type`, `s`→`subject`, `k`→`supported`, `e`→`content-encoding`, `o`→`event`); `add_element()` canonicalizes on insert so lookups by either form resolve. Multi-value arrival-ordered storage (`m_multiMap`) for `via`, `record-route`, `route`, `contact`, `proxy-authenticate`, `www-authenticate`; `get_all()` returns the vector, `get_element()` returns the topmost entry. Refuses `Transfer-Encoding: chunked` (RFC 3261 §7.5). |
| `modules/module/mongodb/*` | `modules/module/mongodb/*` | Verbatim. `MongodbClient` pool; reuse `next_awbno`-style atomic counter for SIP-username generation. |
| `modules/module/wsdbproxy/*` | same | Verbatim. Cloud reads Mongo through wsdbagent in remote-db mode. |
| `pbx-agent/cloudconnector.{hpp,cpp}` | `wsdbagent/agent.{hpp,cpp}` | Repurpose the `ACE_SSL_SOCK_Connector` dial-out + WSS upgrade + reconnect loop. Replace BSON message handling with the framing protocol (§7). |
| `pbx-agent/main.cpp` | `wsdbagent/main.cpp` | Same bootstrap (ACE reactor + signal handlers); start `CloudConnector` instead of mongo proxy server. |
| `Dockerfile.cloud`, `Dockerfile.agent` | `Dockerfile`, `Dockerfile.wsdbagent` | Same multi-stage `cpp-builder`. `BUILD_TESTS=OFF` for agent. |
| `docker-compose.heroku.yml`, `docker-compose.agent.yml` | same | Add Asterisk + coturn services to agent compose. |
| `deploy-heroku.sh`, `run.sh`, `run-agent.sh` | same | s/marvel/onprem-pbx/ and s/uniservice/pbx-cloud/. |
| `ui/` (Angular skeleton, build config, Clarity setup) | shared-library `ui/` | Keep build pipeline; replace shipment screens with softphone screens. Reuse `$any()`, pdfMake patterns, dynamic FormGroup helpers. |
| `CMakeLists.txt` top-level + per-module | same | Verbatim; add `add_subdirectory(modules/module/pbx)` and `pbx-agent`. |
| `test/` GTest setup (`offtarget` binary) | same | Verbatim. New tests slot in under `BUILD_TESTS=ON`. |

**Genuinely new code** (no shared-library counterpart):

- `modules/module/pbx/{inc,src}/sip_frame.{hpp,cpp}` — wire-format encode/decode (10-byte big-endian header, op-code whitelist, 1 MiB payload cap, `Status::Ok/NeedMore/Invalid` decode result). **Landed in Layer 0.** Hand-rolled big-endian readers chosen over `ACE_CDR` since the wire format does not match `ACE_CDR`'s default native-byte-order layout and the swap calls add no value at this scale.
- `modules/module/pbx/sipbridge.{hpp,cpp}` — multiplexer (ACE event handler). Layer 1.
- `modules/module/pbx/push_sender.{hpp,cpp}` — VAPID + Web Push. Layer 1.
- `pbx-agent/sip_frame_demux.{hpp,cpp}` — agent-side multiplexer. Layer 2.
- `pbx-agent/ari_client.{hpp,cpp}` — Asterisk REST/event consumer. Layer 2.

**Where SIP parsing actually runs:** the cloud `SipBridge` and the on-prem `SipFrameDemux` are **byte-faithful** — they multiplex/demux raw SIP-WS frames and do not parse SIP. Asterisk does the runtime parsing. The `sip_parser` lives in two places:

1. The **test harness** (Layer 4 `SipScenarios*`) — synthetic UAs build INVITE/REGISTER bytes and assert on response lines.
2. **Optional cloud-side abuse filter** on `/sip-ws` (peek the first line, drop obvious garbage / malformed METHODs before forwarding into the tunnel). Off by default in v1.

This keeps the parser off the hot path while still earning its keep in tests and giving us a clean extension point for future cloud-side admission policy.

**Rule of thumb when implementing**: if a behavior already exists in the shared library (HTTP parse, WS upgrade, Mongo CRUD, mTLS dial-out, build/deploy script), copy the file over first, get it compiling under this project, *then* extend. No re-deriving from scratch.

---
