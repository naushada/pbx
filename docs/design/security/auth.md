# Authentication

This document describes **what the MVP ships**, not what `DESIGN.md` §5 originally envisioned. The two diverge on the subscriber-portal credential model; see [§ As-built vs design-doc gap](#as-built-vs-design-doc-gap) at the bottom.

## Three auth surfaces

| Surface | Credential | Cloud verifier | Wire format |
|---|---|---|---|
| Subscriber portal (REST + `/sip-ws`) | Bearer token | `MicroServicePbx::handle_subscriber_login` | `Authorization: Bearer <token>` for REST; `?token=<bearer>` for `/sip-ws` (browsers can't set `Authorization` on a WS upgrade) |
| SIP REGISTER (inside the tunnel) | SIP digest (HA1) | Asterisk (chan_pjsip) | RFC 3261 §22 digest, `auth_type=md5` per `docker/asterisk/pjsip.conf` |
| Agent ↔ cloud tunnel | mTLS client cert + private key | Cloud `WebServer`'s SSL context | OpenSSL handshake on `/agent` WS upgrade |

The three are intentionally independent. Compromising the portal token doesn't yield SIP credentials (Asterisk has its own HA1 list); compromising an agent cert doesn't reveal any subscriber's bearer (the agent's tunnel only carries already-authenticated SIP signalling).

## 1. Subscriber portal — bearer flow

### Login
1. UI `LoginComponent` posts `{ societyId, flatNumber, password }` to `POST /api/v1/subscriber/login`.
2. Cloud verifies against the `subscribers` collection.
3. On success, cloud returns `{ token, subscriber }` JSON.
4. `AuthService.setSession(token, subscriber)` writes both into `localStorage`:
   - `pbxui:auth-token` — opaque server-issued string
   - `pbxui:subscriber` — JSON object (societyId, flatNumber, displayName, sipUser, role)

### Session reuse across reloads
On every load `AuthService` rehydrates from `localStorage` and re-emits the subscriber via `PubsubsvcService.onSubscriber`. Cached state surviving reloads is a deliberate choice — Web Push wakeup needs the SW to be able to focus a logged-in app without prompting again.

### Bearer attached on every REST call
`AuthInterceptor` (registered in `app.module.ts` via `HTTP_INTERCEPTORS`):

```ts
if (!skipAuth && token) {
    req = req.clone({ setHeaders: { Authorization: `Bearer ${token}` } });
}
```

`skipAuth` is true only for the login endpoint itself (no token yet).

### 401 → session clear → /login
Any 401 from a non-login endpoint runs `AuthService.clearSession()` and `Router.navigateByUrl('/login')`. The localStorage keys are removed; the user re-authenticates.

### Route guard
`AuthGuard` returns a `UrlTree` pointing at `/login` when `isAuthenticated()` is false. Applied at `/main` so the dashboard + every child route is gated.

### /sip-ws upgrade
Browsers can't set `Authorization` on a WebSocket upgrade. The MVP passes the bearer as a query param:

```
wss://pabx-5fbf3550f938.herokuapp.com/sip-ws?token=<bearer>
```

This is acceptable because:
- The query string only exists on the wire inside TLS — same protection as the `Authorization` header on REST.
- The bearer is opaque server-issued (not an OAuth refresh token); compromise = need to re-issue, no broader fallout.
- Heroku router logs don't capture query strings on long-lived WS upgrades.

The cloud validates the `?token=` at upgrade time inside `WebConnection`, then hands off to `BrowserStream` for the byte-faithful WS-over-WS plumbing.

### Logout
`MainComponent.onSignOut()` calls `AuthService.clearSession()` and routes to `/login`. localStorage keys are removed; the SIP UA (if connected) is left running until next refresh — fine for the MVP because re-opening sees no bearer and the SIP UA falls over on the next REGISTER.

## 2. SIP REGISTER — digest (HA1)

`DESIGN.md` §5 describes the two-credential model: store `sipHa1 = MD5(sipUsername:sipRealm:sipPassword)`, configure `auth_type=md5` in pjsip, never store `sipPassword`. The bundled `docker/asterisk/pjsip.conf` ships the right setting:

```
auth_type=md5
realm=pbx.local
```

The CSV-import flow that generates SIP passwords + hashes them to HA1 is not yet implemented in the MVP. For the sample `alice` / `bob` endpoints in the shipped `pjsip.conf`, the HA1 is a development placeholder.

### Why digest, not bearer
SIP recomputes `MD5(username:realm:password)` on every challenge. We need a transform Asterisk can recompute. Bcrypt is one-way slow by design — Asterisk can't derive HA1 from it. The two-credential split (portal bcrypt + SIP HA1) keeps SIP working without storing any reversible portal password.

## 3. Agent ↔ cloud — mTLS

The agent dials `wss://pabx-5fbf3550f938.herokuapp.com/agent` from `AceSslTransport`. Both sides present X.509 certs signed by the same society CA. Configuration:

| Side | Key/cert | Mount |
|---|---|---|
| Agent (client) | `agent.crt` + `agent.key` | `${CERTS_DIR}/` → `/opt/pbx-agent/certs/` (compose volume) |
| Agent CA pin | `cloud-ca.pem` | Same dir, mounted read-only |
| Cloud (server) | Heroku-managed cert (outer TLS) + xpmile-style mTLS CA (inner client-cert verify) | Heroku-managed |

Cert lifecycle:
- Generated at society install time. The CA stays on a hardened admin host; only the leaf cert + key ship to the on-prem box.
- Rotation is manual: regenerate, update the compose mount, restart the agent. The cloud accepts the new cert immediately (it pins to the CA, not the leaf).
- A revoked agent cert is enforced by adding its serial to the cloud's CRL or — simpler for the MVP — rotating the CA. The agent fails its next dial.

### Why mTLS, not bearer
- A bearer issued to the agent is bearer-immutable for its lifetime — an agent compromise stays in until the bearer expires.
- mTLS gives us a hardware-rooted (or filesystem-rooted) credential the cloud verifies on every dial. Combined with `CloudConnector`'s exponential-backoff reconnect, an attacker can't replay an old session — they need the private key, which never leaves the on-prem host.

## As-built vs design-doc gap

`DESIGN.md` §5 / §10 specify:

- Email + bcrypt-12 portal password
- `HttpOnly; Secure; SameSite=Strict` session **cookie** required to upgrade `/sip-ws`
- `X-CSRF-Token` header echoed from a per-session value at login

The MVP ships:

- Society code + flat number + plaintext password (over TLS) → bearer token
- Bearer in `localStorage`, attached as `Authorization: Bearer …` on REST, as `?token=` on `/sip-ws`
- No CSRF token (largely unnecessary because cross-origin requests can't read `localStorage` and the browser doesn't auto-attach the bearer)

Why the divergence:
1. **Pragmatic for the slice plan.** Cookie-based auth needs `SameSite` handling for the `/sip-ws` upgrade case and an interceptor on the cloud side to verify it. Bearer is one line in an interceptor.
2. **Push wakeup needs persistent session.** With `HttpOnly` cookies the SW can't tell whether the user is authenticated without making a fetch; with `localStorage` it can read the cached subscriber synchronously.

Trade-offs we accept by shipping Bearer instead:
- **XSS risk.** Cross-origin XSS can read `localStorage` and exfiltrate the bearer. Mitigations: Clarity/Angular templates auto-escape, no `innerHTML` user content, CSP can be added later.
- **No native session timeout.** Cookie `Max-Age` is enforced by the browser; bearer is good until cleared. We accept this for the MVP; a future slice can add `expiresAt` and an interceptor refresh.

When the cookie+CSRF model is wired (a future security slice), the changes are localized:
- Replace `Authorization: Bearer …` interceptor with a cookie-aware fetch (Angular HttpClient honors cookies by default)
- Add `X-CSRF-Token` to POST/PUT/DELETE requests
- Move `?token=` on `/sip-ws` to a `Cookie:` header (works because the WS upgrade is a same-origin GET, browsers attach cookies automatically)

## Code map

| File | What |
|---|---|
| `ui/src/common/auth.service.ts` | localStorage session, rehydrate on construction, emits via PubsubsvcService |
| `ui/src/common/auth.interceptor.ts` | Attaches Bearer on every request except login; 401 → clearSession + /login |
| `ui/src/common/auth.guard.ts` | CanActivate guard returning a UrlTree to /login when unauthenticated |
| `ui/src/app/login/login.component.ts` | Form, error states, navigates on success |
| `ui/src/common/sip.service.ts` | Builds `wss://…/sip-ws?token=<bearer>` from `AuthService.getToken()` |
| `pbx-agent/src/main/ace_ssl_transport.{hpp,cpp}` | Outbound mTLS dial to `/agent`; loads `agent.crt` + `agent.key` + `cloud-ca.pem` from CLI flags |
| `pbx-agent/src/main/main.cpp` | Wires `--tls-cert`, `--tls-key`, `--tls-ca` flags through to AceSslTransport |
| `docker/asterisk/pjsip.conf` | `auth_type=md5`, `realm=pbx.local` |
