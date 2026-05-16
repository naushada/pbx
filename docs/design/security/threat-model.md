# Threat model

A STRIDE pass over the surfaces that handle subscriber data, credentials, audio, or money equivalents (TURN bandwidth). Anything not in this table is either inside an OS-process trust boundary or a single private network segment with no externally addressable surface.

## Surfaces

| # | Surface | Direction | What it carries |
|---|---|---|---|
| 1 | Browser `/login` form | Browser → Heroku cloud | Society code + flat number + plaintext password |
| 2 | REST `/api/v1/*` | Browser → Heroku cloud | Subscriber lookups, CDR reads, push subscriptions, TURN cred mints |
| 3 | SIP-over-WS `/sip-ws` | Browser ↔ Heroku cloud | Subscriber SIP signalling, opaque to the cloud (byte-faithful) |
| 4 | Agent tunnel `/agent` | pbx-agent → Heroku cloud | All in-society SIP signalling, framed per `DESIGN.md` §7 |
| 5 | Asterisk WS `/ws` (chan_pjsip) | pbx-agent → Asterisk | SIP signalling on the on-prem LAN |
| 6 | Asterisk ARI events `/ari/events` | pbx-agent → Asterisk | Channel/bridge lifecycle, drives admission + CDR |
| 7 | Asterisk ARI REST `/ari/*` | pbx-agent → Asterisk | Stasis `continue` for admission |
| 8 | MongoDB | pbx-agent ↔ Mongo | Subscribers, CDR, push subscriptions |
| 9 | coturn STUN/TURN | Browser ↔ coturn | RTP relay candidates (off-LAN browsers) |
| 10 | DTLS-SRTP media | Browser ↔ Browser (1:1) / Browser ↔ Asterisk (conference) | Voice audio |
| 11 | Web Push | Heroku cloud → browser push endpoint | VAPID-signed RFC 8291 ciphertext (`{kind, fromFlat, callId}`) |
| 12 | Service Worker | Browser-local | Notification display + tab-focus on click |

## STRIDE — what we defend, what we accept

Legend: **D** = defended in code or config; **A** = accepted risk (rationale below the table); **N/A** = not applicable on this surface.

| # | Surface | Spoofing | Tampering | Repudiation | Info Disclosure | DoS | Elevation |
|---|---|---|---|---|---|---|---|
| 1 | `/login` | D — POST body, bearer issued only on success | D — TLS 1.2+ end-to-end | D — login attempts loggable | D — TLS 1.2+; no creds in URL | A — rate limit pending; see [§ deferred](#deferred-controls) | D — bearer scoped to subscriber role |
| 2 | REST `/api/v1/*` | D — `AuthInterceptor` attaches Bearer; unauthenticated → 401 | D — TLS 1.2+ | D — server access log | D — TLS | A — same | D — Mongo queries scoped to `req.subscriber.societyId` |
| 3 | `/sip-ws` | D — `?token=<bearer>` validated at upgrade | D — TLS 1.2+ | A — opaque frames, only Asterisk knows REGISTER outcomes | D — TLS | A — connection caps pending | A — once upgraded, the WS is byte-faithful — no further trust check |
| 4 | `/agent` | D — mTLS, client cert pinned to society CA | D — mTLS + length-prefixed framed | D — agent dial log + cloud accept log | D — mTLS | D — single tunnel per society, multiplexed | D — agent has zero cloud-side privileges beyond its tunnel |
| 5 | Asterisk WS | D — SIP digest in REGISTER (HA1 model per `DESIGN.md` §5) | D — internal compose network, not host-exposed | D — pjsip + ARI log | A — internal LAN | A — not a hostile surface | D — Asterisk ARI user has only Stasis access |
| 6 | ARI events | D — HTTP Basic over compose-internal HTTP | D — same | D — Asterisk logs | A — internal LAN | D — Asterisk's own backpressure | A — ARI user has `asterisk:asterisk` default; **override required for prod** |
| 7 | ARI REST | same as 6 | same | same | same | same | same |
| 8 | MongoDB | A — no auth on the compose-internal Mongo | A — same | D — operations log if `--quiet=false` | A — internal LAN | A — Mongo's defaults | A — anyone with `pbx-net` access has full DB |
| 9 | coturn | D — HMAC-SHA1 time-limited creds per RFC 5766 §5 | D — long-term creds rotated quarterly | D — coturn log | D — TURN itself is plain UDP relay; payload is SRTP already | A — `total-quota` 300, `max-bps` 512 kbps in `turnserver.conf` | D — credential is per-user, per-5-min |
| 10 | DTLS-SRTP | D — `a=fingerprint` in SDP, peer-verified | D — SRTP HMAC | D — SDP fingerprints visible in CDR audit trail | D — AES-128-GCM | D — ICE liveness checks | A — for **conferences only**, Asterisk has plaintext (see [`media.md`](./media.md)) |
| 11 | Web Push | D — VAPID JWT (RFC 8292) | D — RFC 8291 AES-128-GCM + RFC 8188 framing | D — `PushSender` log; 410 prunes dead subscriptions | D — payload encrypted to subscriber's ECDH P-256 key | D — `PushSender` retries with backoff; 410 prunes | N/A |
| 12 | Service Worker | A — runs in the same origin as the SPA | D — fetched over TLS, browser checks integrity | N/A | D — payload only ever passes through `event.data.json()` in SW scope | N/A | D — SW scope is origin-bound |

## Trust boundaries

```
  ┌─────────── Heroku (cloud trust zone) ───────────┐    ┌─── On-prem (society trust zone) ───┐
  │ Browser ↔ /api, /sip-ws, /portal                │    │ pbx-agent ↔ Asterisk (compose LAN) │
  │ pbx-cloud REST + WS                              │    │            ↔ Mongo (compose LAN)  │
  │ PushSender → push endpoints                      │    │            ↔ coturn (host net)    │
  └──────────────────────┬───────────────────────────┘    └─────────────────┬─────────────────┘
                          │   mTLS  (/agent)                                  │
                          └─────────────────  TUNNEL  ────────────────────────┘
                          one persistent WSS per society, framed per DESIGN.md §7
```

Three implicit assumptions hold the whole model together:

1. **One society = one pbx-agent.** All Mongo data, all Asterisk endpoints, the TURN realm, and the agent's mTLS cert are scoped to a single societyId. Cross-society data leakage is impossible by construction — the agent simply doesn't know about other societies.
2. **The compose-internal network is trusted.** Mongo, Asterisk, and the agent share `pbx-net` with no auth between them. Anything that lands on the society host with bridge-network access has full mongo. This is intentional — adding auth here means rotating it every install and gains nothing because the threat actor inside the bridge network is already root on the host.
3. **The Heroku app is a public-Internet surface.** Bearer auth for `/api/*`, query-param token for `/sip-ws`, **InnerTLS-over-WS for `/agent` and `/ws/db`** (PR #19 closed the previous "outer-TLS-only is theatre" gap — see [`innertls.md`](./innertls.md)). The outer TLS that Heroku's router terminates remains useful for the public-Internet hop (cert chain validation, eavesdropping protection) but is no longer load-bearing for authentication; the inner handshake on the WS payload is what proves the agent's identity to the dyno.

## Deferred controls

These are flagged in `DESIGN.md` §10 but not yet implemented in code. Promoting them is straightforward; they're listed here so the gap is visible.

| Control | Mentioned in | Status | Notes |
|---|---|---|---|
| Rate-limit `/sip-ws` upgrades per IP | `DESIGN.md` §10 | ❌ Not implemented | Heroku router has a coarse per-app limit; per-IP needs an interceptor in `WebConnection`. Acceptable until the app is widely advertised. |
| CSRF token (`X-CSRF-Token`) | `DESIGN.md` §10 | ❌ Not implemented | The MVP uses Bearer-in-Authorization-header (see [`auth.md`](./auth.md)), which is *largely* CSRF-immune because the browser doesn't auto-attach it. If we move back to cookies, CSRF re-becomes a real concern. |
| Quarterly TURN secret rotation | `DESIGN.md` §10 | ❌ Manual | `societies.turnSharedSecret` exists in the data model; no scheduled job rotates it. |
| MongoDB authentication | `DESIGN.md` (implicit) | ❌ Not enforced | Compose network gates access today. Adding `--auth` is a one-line change but adds another secret to manage. |
| Two-credential model (portal bcrypt + SIP HA1) | `DESIGN.md` §5 | ⚠️ Partial — see [`auth.md`](./auth.md) | UI login takes a single password; SIP digest model exists conceptually but the import flow + bcrypt verification aren't wired. |
| Real subscriber-login auth on cloud | `DESIGN.md` §5 | ⚠️ Dev-mode permissive | `handle_subscriber_login_POST` returns a synthetic session for any non-empty triple. `PBX_AUTH_STRICT=1` short-circuits with 503 — that branch will hold the bcrypt check once subscribers are seeded. See [`controls.md` § 12](./controls.md#12-dev-mode-permissive-login-temporary). |
| `wsdbagent` on-prem (xpmile pattern) | `DESIGN.md` §11 | ✅ Source committed; pending live verification | Standalone binary at `modules/module/wsdbagent/` (verbatim from xpmile). Ships as the `pbx-wsdbagent` compose service. Dials `wss://${CLOUD_HOST}/ws/db` + ACE InnerTLS over the outer WSS. DB name `pabx`. |
| InnerTLS on the `/agent` SIP tunnel | `DESIGN.md` §6.6 / §7 + [`innertls.md`](./innertls.md) | ✅ PR #19 + PR #25 (merged 2026-05-16) | Shared `WsInnerTlsBridge` (dual-mode `IInnerTlsTransport`) layers `InnerTlsClient` (agent) / `InnerTlsServer` (cloud) over the WS frames after the outer upgrade — same trust boundary as `/ws/db`. Agent: `--inner-tls-{cert,key,ca,hostname}`. Cloud: reuses `--tls-{cert,key,ca}`. `AgentStream` ctor split into `(auto_attach=false)` + `setup_inner_tls()` + `attach()` so buffered outbound frames from a prior disconnect can't race the handshake plaintext-onto-the-wire. PR #25 added `InnerTlsServer::peer_subject_cn()` for verified-CN log labels + fixed a latent `SSL_CTX_use_certificate_file`-after-`SSL_new` bug that was silently letting the client present no cert. |
| Agent identity at the cloud (cert CN vs claimed `societyId`) | PR #24 + PR #25 | ⚠️ Partial — cross-check not yet enforced | Cloud reads the agent cert's verified CN via `AgentStream::peer_cn()` (PR #25) for log labels. The agent also self-declares its `societyId` in `AGENT_HELLO` (PR #24). The `peer_cn() == AGENT_HELLO.societyId` cross-check is NOT enforced today — cloud trusts the AGENT_HELLO payload to drive the society-doc lookup. Future hardening: refuse `bootstrap_society()` if CN doesn't match. |
| Cloud `--remote-db` routing | `DESIGN.md` §11 | ⚠️ Not yet enabled on Heroku (D4) | Source-ready (`wsdbproxy` cloud side + `wsdbagent` on-prem side both committed). Flip `REMOTE_DB=1` on Heroku only after the on-prem `pbx-wsdbagent` is verified connected — otherwise REST handlers block waiting for a tunnel that doesn't exist. |

## How this maps to the code

- Bearer attached + 401-handled: `ui/src/common/auth.interceptor.ts`
- AuthGuard on `/main`: `ui/src/common/auth.guard.ts`
- VAPID JWT + payload encryption: `modules/module/pbx/src/push_sender.cpp`
- 410-prune dead push subscriptions: `push_sender.cpp` (handles `HTTP/1.1 410 Gone`)
- DTLS-SRTP enforced (no SDES fallback): `docker/asterisk/pjsip.conf` (`media_encryption=dtls`)
- TURN time-limited creds: `MicroServicePbx::handle_turn_credentials` (cloud) + `docker/coturn/turnserver.conf` (`use-auth-secret`)
- Tunnel framing parser drops malformed input + closes the WSS: `modules/module/pbx/src/sip_frame.cpp` `decode()` returns nullopt
