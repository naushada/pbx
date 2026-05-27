# Security controls

Compensating, per-surface, enforced. Each entry says **what** the control is, **where** it lives in code, and **what failure mode it defends against**.

## 1. Admission cap — 5 concurrent calls per society

**What.** `AriClient` maintains a per-society counter of active bridges. When `BridgeCreated` arrives the counter increments; `BridgeDestroyed` decrements (never below zero). On `StasisStart` (a new channel arriving at the Stasis app), if the counter is already at `societies.maxConcurrentCalls`, the channel is rejected via ARI `POST /channels/{id}/continue` into the `pbx-busy` extension, which plays a busy tone and hangs up. No CDR is written for the rejected call.

**Why bridges, not channels.** A single 1:1 call has two channels (caller + callee). A 3-party conference has three. Naïvely counting `ChannelCreated` would fire the cap at 2.5 real calls. A bridge maps 1:1 to a "logical call" — for both 1:1 (mixing bridge) and conference (mixing bridge with N participants).

**Where.**
- `pbx-agent/src/main/ari_client.cpp` — bridge counter + admission gate
- `pbx-agent/src/main/ari_rest_client.cpp` — `continue` REST call
- `docker/asterisk/extensions.conf` — `pbx-busy` context

**Failure modes defended.**
- Resource exhaustion (DoS via call flood) — the cap caps Asterisk's RTP/SRTP load.
- Bill shock on coturn — TURN relay bandwidth is bounded.

**Test coverage.** `ari_client_test.cc::AdmissionCap_ReturnsBusyAtFive`, `AdmissionCap_AllowsUnderCap`, `BridgeDestroyed_DecrementsActiveCount_NeverNegative`.

## 2. Society scoping

**What.** Every subscriber, CDR row, push subscription, and TURN credential is keyed on `societyId`. The on-prem agent only knows its own society's data. The cloud's REST handlers (`MicroServicePbx`) scope all queries to `req.subscriber.societyId`.

**Where.**
- `MicroServicePbx::handle_directory_search` adds `societyId` to the Mongo filter from the request bearer's subscriber.
- `MicroServicePbx::handle_cdr_read` same.
- Each `pbx-agent` mTLS cert pins to a single societyId.
- Mongo indexes: `subscribers(societyId, flatNumber)`, `cdr(societyId, startedAt)`, `push_subscriptions(societyId, subscriberId)`.

**Failure modes defended.**
- Cross-society data leakage — impossible without forging a bearer for a different society's subscriber, which requires the cloud's signing key.
- Lateral movement after one society's pbx-agent compromise — the agent's tunnel only carries its own society's traffic; the cloud routes by stream-id to the matching society agent.

## 3. Web Push payload encryption (RFC 8291)

**What.** When the cloud needs to wake a sleeping browser ("incoming call from A-204"), it POSTs to the browser's push endpoint with:
- A **VAPID JWT** (RFC 8292) signed with the app's ECDSA P-256 private key, proving the cloud's identity to the push service.
- A **payload** (JSON: `{ kind: 'incoming-call', fromFlat, callId, displayName }`) encrypted to the **subscriber's** ECDH P-256 public key using `AES-128-GCM` with HKDF-SHA256 key derivation, framed per RFC 8188 ("aes128gcm" content coding).

The push service relays the ciphertext blob; only the browser holding the private key can decrypt. The push service never sees the call metadata.

**Where.**
- `modules/module/pbx/src/push_sender.cpp` — VAPID JWT signing, RFC 8291 encryption pipeline, RFC 8188 framing, retry-with-backoff, 410-Gone subscription pruning
- `modules/module/pbx/src/ace_https_client.cpp` — outbound HTTPS to the push service with SSL_VERIFY_PEER against the system trust store
- `ui/src/common/push.service.ts` — `pushManager.subscribe({ applicationServerKey: vapidPublicKey, userVisibleOnly: true })`
- `ui/src/sw.js` — `event.data.json()` returns the **decrypted** payload (the browser decrypts before handing it to the SW)

**Failure modes defended.**
- Push-service operator reading the payload — they can't (they only see ciphertext).
- Stale subscription leaking another user's call — the encryption is to the per-subscriber key, so a stale endpoint at most fails to decrypt and we get a 410, which prunes.
- Push-endpoint spoofing — the VAPID JWT proves the sender is the actual cloud app.

**Test coverage.** `push_sender_test.cc` (8 tests in Layer 1, +5 in Layer 4 for the wired-up HTTPS path).

## 4. Time-limited TURN credentials (RFC 5766 §5)

**What.** Browsers don't get a long-lived TURN password. Instead, `GET /api/v1/turn-credentials` returns:

```json
{
    "urls":       ["turn:turn.<society>.local:3478?transport=udp"],
    "username":   "<unix_ts>:<sip_user>",
    "credential": "<base64(HMAC-SHA1(turnSharedSecret, username))>",
    "ttlSec":     300
}
```

The browser passes these into `RTCConfiguration.iceServers`. coturn's `use-auth-secret` mode recomputes the HMAC from the username and validates without any per-user database.

**Where.**
- Cloud: `MicroServicePbx::handle_turn_credentials` (mints the bundle)
- coturn config: `docker/coturn/turnserver.conf` (`use-auth-secret`, `static-auth-secret=…`)
- Society document: `societies.turnSharedSecret` (rotated quarterly per `DESIGN.md` §10)

**Failure modes defended.**
- TURN relay theft by a leaked browser → password lasts 5 minutes, not forever.
- Cross-society relay abuse → each society has its own shared secret.

**Deferred.** Quarterly rotation is not yet automated. A scheduled job rotating `turnSharedSecret` and pushing the new value to coturn is the missing piece.

## 5. Bridge-network isolation for Asterisk

**What.** Asterisk's `chan_pjsip` WS port (`:8088`) is published only on the compose-internal `pbx-net` bridge — never to the host. Only the `pbx-agent` container reaches Asterisk; nothing else does.

**Where.** `docker-compose.agent.yml` — `pbx-asterisk` declares no `ports:`; only `pbx-coturn` is host-net (it has to be, for STUN replies to carry the real public IP).

**Failure modes defended.**
- Direct SIP scanning from the public Internet — port unreachable.
- Compromise of any other compose service ≠ Asterisk auth bypass (SIP digest still required).

**Trade-off.** Anything that lands inside `pbx-net` (e.g. a compromised pbx-agent) can talk to Asterisk freely. We accept this — the agent is already trusted with the mTLS cert for `/agent`.

## 6. mTLS for cloud ↔ agent

**What.** The agent's outbound dial to `wss://pabx-5fbf3550f938.herokuapp.com/agent` uses ACE_SSL_SOCK_Connector with a society-scoped client cert/key. The cloud verifies the cert against the society CA.

**Where.**
- `pbx-agent/src/main/ace_ssl_transport.cpp` — loads `agent.crt` + `agent.key` + `cloud-ca.pem` from CLI paths
- `pbx-agent/src/main/main.cpp` — `--tls-cert / --tls-key / --tls-ca` flags
- `docker-compose.agent.yml` — `${CERTS_DIR}:/opt/pbx-agent/certs:ro` bind mount

**Failure modes defended.**
- Agent impersonation (someone else dialing `/agent` claiming to be a society) — they'd need the private key, which never leaves the on-prem host.
- Cloud impersonation (DNS hijack) — the agent pins to `cloud-ca.pem`; a different cert means TLS handshake failure.

## 7. WebSocket hand-off ordering invariant

**What.** When a `/sip-ws` upgrade succeeds, `WebConnection` must:
1. `remove_handler` the WebConnection from the reactor
2. Set its `m_handle` to `ACE_INVALID_HANDLE`
3. Publish the upgraded socket to `SipBridge`

In that order. Reversing any two of these leaks the socket, double-frees, or leaves a stale reactor entry. Same invariant for `/agent` (cloud accept side) and `/ws/db` (wsdbproxy tunnel).

**Where.** `modules/module/webservice/src/webconnection.cpp` — handler shape; `test/integration/handoff_ordering_test.cc` — source-invariant regression guard. The test reads `webconnection.cpp` and asserts the textual ordering of the three calls for each of the three WS upgrade branches.

**Failure modes defended.**
- Memory-safety bugs from out-of-order teardown (caught at build time by the source-invariant test).
- Regressions when extending the same hand-off pattern to future endpoints.

## 8. 410-Gone push subscription pruning

**What.** When `PushSender` POSTs to a push endpoint and gets `HTTP/1.1 410 Gone`, the subscription is dead (user revoked, browser uninstalled, etc.). `PushSender` deletes the row from `push_subscriptions` so we don't keep notifying a dead endpoint.

**Where.** `modules/module/pbx/src/push_sender.cpp` — `handle_status(410)` → `db.delete_one("push_subscriptions", subscriptionId)`.

**Failure modes defended.**
- Indefinite retry against a dead endpoint (bill, log noise).
- Push-service rate-limiting the cloud's IP because of repeated 410s.

## 9. Auto-reject second-incoming call (busy)

**What.** When a subscriber is already in a call and a second INVITE arrives over `/sip-ws`, `SipService` auto-rejects the new call with SIP 486 Busy Here. Same applies at the UA level via `ua.delegate.onInvite` when no listener is registered (defensive default in `SipJsUaHandle.dispatchIncoming`).

**Where.** `ui/src/common/sip.service.ts::onIncoming`, `shared/sip-ua/sip-ua-sipjs.ts::SipJsUaHandle::dispatchIncoming`.

**Failure modes defended.**
- Call-waiting UX confusion — user doesn't see two overlapping incoming-call panels.
- Race conditions in mute/hangup state for two parallel calls.

## 10. Source-of-truth scoping

**What.** Sensitive material has exactly one source of truth:

| Material | Source | Never duplicated |
|---|---|---|
| Bearer token | Cloud, returned in `/api/v1/subscriber/login` response | Stored only in `localStorage`; not in URL fragments, not in cookies |
| SIP digest password | Set at CSV-import; HA1 stored in Asterisk | Plaintext shown once on download CSV, then unrecoverable |
| VAPID private key | Heroku config var `VAPID_KEY_PATH` (PEM file mount or inline) | Never logged; only used inside `push_sender.cpp` |
| Mongo `turnSharedSecret` | `societies.turnSharedSecret` doc | Mirrored to coturn's `static-auth-secret` at install; not in code |
| Agent mTLS private key | `${CERTS_DIR}/agent.key` on the on-prem host | Never leaves the host |

**Failure modes defended.**
- Configuration drift between intended secret and deployed secret.
- Accidental commit of secret material (templates only — `.env.agent.example` has no real values).

## 11. DB-availability guard on REST handlers

**What.** Every PBX REST handler that calls `db.get_documents()` or `db.create_document()` first checks `db_available()` — true only when `DB_URI` or `REMOTE_DB` env is set. If the DB isn't configured the handler returns an empty result (`200 OK []` for GETs, `503 Service Unavailable` for the push-subscribe POST) **without** invoking the Mongo client.

**Where.**
- `modules/module/pbx/src/microservice_pbx.cpp`: anonymous-namespace helpers `env_or()` + `db_available()`; each DB-backed handler short-circuits at the top.

**Failure modes defended.**
- Heroku H12 timeouts (the Mongo C driver blocks for 30 + s waiting for a connection that will never arrive when the cloud is running without a DB addon — Heroku then 503s the request and the worker recovers but spends real time blocked).
- Worker thread starvation under traffic when many handlers are mid-`get_documents()` against an unreachable Mongo.

**Trade-off.** This is a deployment-state guard, not a correctness one. With a real DB, the guard is a no-op. Once `--remote-db` is wired through the wsdbagent tunnel (see `auth.md` § live cloud routes), the guard still helps short-circuit during agent-tunnel drops.

## 12. Dev-mode permissive login (temporary)

**What.** `MicroServicePbx::handle_subscriber_login_POST` accepts any non-empty `{societyCode, flatNumber, password}` triple and returns a synthetic subscriber + a random bearer token. When `PBX_AUTH_STRICT=1` is set, the handler refuses with `503` instead of issuing a session — that's the path that will hold the real bcrypt check against `subscribers.portalPasswordHash` once CSV-import has seeded the collection.

**Where.**
- `modules/module/pbx/src/microservice_pbx.cpp` — `handle_subscriber_login_POST`.

**Failure modes defended (in dev-mode).** None — this is explicitly an open door for the MVP demo. **Risk**: anyone hitting the live URL gets a usable session and full read access to the empty/anonymised endpoints. Mitigation: the deploy has no PII to expose; the synthetic subscriber's `societyId` field reflects whatever the caller typed, so no cross-society leakage is possible.

**Promotion plan.** Set `PBX_AUTH_STRICT=1` once the cloud has a `DB_URI` or the agent's wsdbagent tunnel is live, and replace the `503` branch with the real bcrypt check.

## Cross-references

- Auth flows: [`auth.md`](./auth.md)
- Media (DTLS-SRTP): [`media.md`](./media.md)
- Secrets handling: [`secrets.md`](./secrets.md)
- Threat model + STRIDE: [`threat-model.md`](./threat-model.md)
