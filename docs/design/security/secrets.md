# Secrets handling

Every piece of sensitive material the system handles, where it lives, who reads it, when it rotates, and what the dev-only placeholder is. If you're setting up a new society or deploying to a new Heroku app, this is the checklist.

## Inventory

| Secret | Trust zone | Lifetime | At rest | In flight |
|---|---|---|---|---|
| Subscriber portal bearer | Cloud-issued, per-session | Indefinite (cleared on logout / 401) | `localStorage` in the browser | TLS-protected `Authorization: Bearer …` / `?token=` |
| SIP digest password (subscriber) | Generated at CSV-import | Until rotated | HA1 in Asterisk `pjsip.conf` per endpoint; plaintext only on the one-time import CSV | TLS (`/sip-ws`) → SIP digest challenge |
| Agent mTLS client cert + key | One per society (per pbx-agent) | Years; revoke by CA rotation | `${CERTS_DIR}/agent.{crt,key}` on the on-prem host | mTLS handshake |
| Cloud CA cert (pinned by agent) | One per cloud deployment | Years | `${CERTS_DIR}/cloud-ca.pem` on the on-prem host | Read at agent start; pinned across reconnects |
| VAPID ECDSA P-256 private key | One per Heroku cloud app | Years; rotate ↔ all subscribers must re-subscribe | Heroku config var `VAPID_KEY_PATH` (PEM file path inside the dyno) or inline content | Used inside `push_sender.cpp` only; never logged |
| TURN shared secret | One per society | Quarterly rotation (per `DESIGN.md` §10) | Mongo `societies.turnSharedSecret`; mirrored to coturn's `static-auth-secret` | HMAC-SHA1 over the wire for credential derivation only |
| ARI credentials | One per society | Until rotated | `docker/asterisk/ari.conf` + agent env (`ARI_USER` / `ARI_PASS`) | HTTP Basic on the compose-internal network |
| MongoDB credentials | n/a (no auth in MVP) | n/a | n/a | n/a — relies on compose-internal isolation |
| Asterisk DTLS cert + key | One per society | Years; regenerate on agent upgrades | `/etc/asterisk/keys/pbx.{crt,key}` (mounted into the asterisk container) | Used only at DTLS handshake; SDP fingerprint binds it |

## Where secrets enter the system

### On the cloud (Heroku)

```sh
# Cloud's REST + WS endpoint
heroku config:set \
  VAPID_KEY_PATH=/app/secrets/vapid.pem \
  VAPID_SUBJECT=mailto:ops@yourcompany.example \
  --app onprem-pbx

# UI app
heroku config:set \
  BACKEND_ORIGIN=https://onprem-pbx.herokuapp.com \
  --app onprem-pbx-ui
```

The VAPID key file ships into the cloud image at build time (multi-stage) or is mounted via Heroku's filesystem (Heroku doesn't really have a secrets-mount story; the file lives inside the image). For the MVP we accept that VAPID key material is baked into the container layer — rotation means rebuilding + redeploying.

### On the on-prem host

```sh
# .env (copied from .env.agent.example, gitignored)
CLOUD_HOST=onprem-pbx.herokuapp.com
CLOUD_PORT=443
AGENT_SOCIETY_ID=<mongo ObjectId>
CERTS_DIR=./certs/agent-deployed
ARI_USER=<override>
ARI_PASS=<override>
```

The `CERTS_DIR` is bind-mounted into both the agent (`/opt/pbx-agent/certs/`) and Asterisk (`/etc/asterisk/keys/`) containers as read-only.

## Rotation playbooks

### VAPID key

When: suspected leak; routine rotation every 2 years.

1. Generate a new ECDSA P-256 key:
   ```sh
   openssl ecparam -genkey -name prime256v1 -noout -out vapid.pem
   openssl ec -in vapid.pem -pubout -outform DER | tail -c 65 | base64url
   # the second command emits the new VAPID_PUBLIC_KEY for the UI
   ```
2. Update `VAPID_KEY_PATH` (Heroku config var) and rebuild the cloud image.
3. **Every subscriber must re-subscribe.** Their existing browser subscription was keyed to the old VAPID public key; the new key cannot decrypt it. Mass-trigger by clearing `push_subscriptions` and prompting users at next login.

### Agent mTLS cert

When: agent host compromise; cert expiry approaching.

1. Generate a new leaf cert signed by the same society CA.
2. Replace `${CERTS_DIR}/agent.{crt,key}` on the on-prem host.
3. `podman-compose -f docker-compose.agent.yml restart pbx-agent`.
4. The next dial picks up the new cert. `CloudConnector`'s exponential backoff means the cutover is at worst a few-second gap.

If the CA itself is compromised, generate a new CA, sign new leafs for all societies, replace `cloud-ca.pem` everywhere, redeploy.

### TURN shared secret

When: quarterly (per `DESIGN.md` §10).

1. Generate a new 32-byte random value:
   ```sh
   openssl rand -base64 32
   ```
2. Update `societies.turnSharedSecret` in Mongo.
3. Update `static-auth-secret` in `docker/coturn/turnserver.conf` for that society.
4. `podman-compose -f docker-compose.agent.yml restart pbx-coturn`.
5. In-flight TURN allocations survive (existing credentials remain valid until their 5-minute TTL); new browsers get the new secret on their next `/api/v1/turn-credentials` call.

### SIP digest password (per subscriber)

When: subscriber requests reset; suspected leak.

1. Admin POSTs `/api/v1/society/<id>/subscribers/<flat>/regenerate-sip-password` (endpoint exists in `MicroServicePbx` per `DESIGN.md` §11; partial in MVP).
2. Cloud generates a new password, computes HA1, updates `subscribers.sipHa1`, emits a one-time CSV download to the admin.
3. Admin distributes the new password out-of-band (email + SMS per `DESIGN.md` §10).
4. The subscriber's browser re-authenticates on next REGISTER.

The old password keeps working until step 2 lands — there is no soft-expiry. Rotation is a single discrete step.

## Dev-only placeholders shipped in the repo

These are intentional and **must be overridden before any non-local deployment**:

| File | Placeholder | Override via |
|---|---|---|
| `docker/coturn/turnserver.conf` | `static-auth-secret=dev-only-shared-secret-override-in-prod` | Edit `turnserver.conf` per society; pair with `societies.turnSharedSecret` |
| `docker/asterisk/ari.conf` | `asterisk:asterisk` ARI user | `.env` env vars `ARI_USER` / `ARI_PASS`, picked up by `docker-compose.agent.yml` |
| `docker/asterisk/pjsip.conf` | sample `alice` / `bob` endpoints | CSV-import (when wired) replaces these with real subscriber endpoints |
| `.env.agent.example` | template with empty `AGENT_SOCIETY_ID` etc. | `cp .env.agent.example .env && $EDITOR .env` |

The defaults are loud and obviously placeholders — but it's worth a deployment-checklist item to actually swap them.

## What's NOT a secret (but might look like one)

- **`subscriber.sipUser` / `subscriber.flatNumber`** — these are user-visible identifiers, not auth material. They appear in CDR rows, SDP From/To fields, and push notification bodies (the displayName). Leaking them reveals who lives where in the society, which is a privacy concern but not a credential concern.
- **VAPID public key** — by design, the UI fetches it from `GET /api/v1/push-vapid-key` over an unauthenticated path. The signature it verifies is the private-key proof from the cloud, not anything secret about the public key itself.
- **Push subscription endpoint URL** — long URLs that *look* secret because they encode an opaque token. They're safe to log; they're only usable in combination with the VAPID private key.

## Secret-scanning hygiene

- `.gitignore` excludes `*.pem`, `*.key`, `*.crt`, `.env`, `.env.local` at the repo root.
- `certs/` has a `.gitkeep` but everything inside it is ignored.
- `.env.agent.example` is the **only** env file checked in; the comment header explicitly says "copy to `.env` and fill in".
- Pre-commit hook recommendation (not enforced yet): `git secrets` or `trufflehog`.

## Cross-references

- Auth flows: [`auth.md`](./auth.md)
- Media key material (DTLS-SRTP): [`media.md`](./media.md)
- Controls + threat model: [`controls.md`](./controls.md), [`threat-model.md`](./threat-model.md)
