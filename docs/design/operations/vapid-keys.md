# VAPID keys — what they are, where they live, what to do when

> **Why this matters.** The VAPID keypair is the *identity* the cloud
> uses to sign every Web Push JWT (RFC 8292). Lose it and every
> existing browser push-subscription becomes undeliverable; rotate it
> and every subscriber has to re-enable push in the UI. Treat it with
> the same care as a TLS private key.

## What

An ECDSA P-256 keypair shared by:

- **Browsers** (via `pushManager.subscribe({ applicationServerKey: <pub> })` — public key only).
- **Cloud** (via `PushSender::build_vapid_auth` — signs JWTs with the private half).

Same keypair forever, ideally. Browsers persist `PushSubscription`s
keyed to the *exact public bytes* they subscribed with; rotating the
keypair invalidates all of them.

## Where the secret lives

| Location | Form | Authority for | Persisted across |
|---|---|---|---|
| Heroku config var `VAPID_PUBLIC_KEY` | base64url, 87 chars, uncompressed P-256 point starting with `0x04` | What `GET /api/v1/push-vapid-key` returns to the SPA | App lifetime |
| Heroku config var `VAPID_PRIVATE_KEY_B64` | base64-encoded PEM | What the dyno decodes to `/tmp/vapid.pem` at startup and signs JWTs with | App lifetime |
| Heroku config var `VAPID_SUBJECT` | `mailto:` URI | The VAPID `sub` claim (RFC 8292 §2.2) | App lifetime |
| Operator laptop `~/.vapid/pabx-vapid-private.pem` | PEM (chmod 600) | Convenience copy for rotation or re-upload | **This laptop only** |

**Heroku holds the canonical copy.** The laptop file is a stash, not
a backup — see [Backup strategy](#backup-strategy).

## What to do when …

### … you move to a new laptop / lose the local PEM

```bash
mkdir -p ~/.vapid
heroku config:get VAPID_PRIVATE_KEY_B64 -a pabx \
  | base64 -d > ~/.vapid/pabx-vapid-private.pem
chmod 600 ~/.vapid/pabx-vapid-private.pem
```

That's it. Nothing on the cloud side changes; you just re-materialised
the working copy.

### … you want to verify the laptop PEM matches the live cloud

```bash
LOCAL=$(openssl ec -in ~/.vapid/pabx-vapid-private.pem -pubout -outform DER 2>/dev/null \
        | tail -c 65 | base64 | tr '+/' '-_' | tr -d '=')
LIVE=$(curl -sS https://pabx-5fbf3550f938.herokuapp.com/api/v1/push-vapid-key | jq -r .key)
[ "$LOCAL" = "$LIVE" ] && echo "match" || echo "MISMATCH"
```

A mismatch means somebody rotated the cloud config without telling you.

### … the browser shows "applicationServerKey must contain a valid P-256 public key"

Cloud is serving a bogus key. Check:

```bash
curl -sS https://pabx-5fbf3550f938.herokuapp.com/api/v1/push-vapid-key
# Expect a base64url string of length 87 starting with 'B'
# (the 0x04 uncompressed-point prefix encodes to 'B…' in base64url).
```

If it returns `{"key":"BTestPlaceholderReplaceWithRealKey"}` or any
string ≠ 87 chars, the `VAPID_PUBLIC_KEY` config var is wrong. Fix by
running the rotation steps below, *or* (if you still have the matching
private PEM) recompute the public half from it:

```bash
PUB=$(openssl ec -in ~/.vapid/pabx-vapid-private.pem -pubout -outform DER 2>/dev/null \
      | tail -c 65 | base64 | tr '+/' '-_' | tr -d '=')
heroku config:set -a pabx VAPID_PUBLIC_KEY="$PUB"
```

### … push delivers silently (`pushManager.subscribe()` works but no notifications arrive)

Means `--vapid-key-path` isn't being passed at dyno startup — either
`VAPID_PRIVATE_KEY_B64` isn't set, or the entrypoint failed to decode
it. Check:

```bash
heroku config -a pabx | grep VAPID
heroku logs -a pabx -n 200 | grep -iE 'push|vapid'
```

The cloud logs `PushSender: …vapid_public_b64url=…` at startup when
the key is wired correctly; absence of that line means
`webservice_main.cpp` short-circuited (`if (!opt[VAPID_KEY_PATH]) …
log-only`).

### … you need to rotate the keypair

> **Cost:** every existing `PushSubscription` becomes undeliverable.
> Browsers will have to re-`subscribe()` the next time the user opens
> the Settings page and clicks **Enable**. The cloud's
> `push_subscriptions` Mongo collection will be cleaned up lazily as
> the push endpoints return HTTP 410 (`PushSender` already handles
> that gracefully).

```bash
# 1. Generate
openssl ecparam -name prime256v1 -genkey -noout -out ~/.vapid/pabx-vapid-private.pem
chmod 600 ~/.vapid/pabx-vapid-private.pem

# 2. Compute the new public key (base64url) and the new B64 PEM
PUB=$(openssl ec -in ~/.vapid/pabx-vapid-private.pem -pubout -outform DER 2>/dev/null \
      | tail -c 65 | base64 | tr '+/' '-_' | tr -d '=')
B64=$(base64 < ~/.vapid/pabx-vapid-private.pem | tr -d '\n')

# 3. Upload both — Heroku auto-restarts the dyno
heroku config:set -a pabx \
  VAPID_PUBLIC_KEY="$PUB" \
  VAPID_PRIVATE_KEY_B64="$B64"

# 4. (Optional) wipe stale subscriptions so the cloud stops trying to
#    deliver to dead endpoints before 410-cleanup catches them.
#    mongo shell:
#      db.push_subscriptions.deleteMany({})
```

### … you're bootstrapping a brand-new cloud app

Same as rotation above, but you also need `VAPID_SUBJECT`:

```bash
heroku config:set -a <new-app> \
  VAPID_PUBLIC_KEY="$PUB" \
  VAPID_PRIVATE_KEY_B64="$B64" \
  VAPID_SUBJECT='mailto:ops@example.com'
```

The cloud image's entrypoint (`docker/Dockerfile.cloud`) takes care of
the rest: decodes the PEM to `/tmp/vapid.pem` at startup and passes
`--vapid-key-path /tmp/vapid.pem` to `pbx-cloud`.

## Backup strategy

**The laptop copy is not a backup; it's a working copy.** Real backups
live wherever your team stores cloud secrets — 1Password, Bitwarden,
AWS Secrets Manager, a sealed envelope in a safe, whatever.

Failure modes ranked by recoverability:

| Lost | Recovery | Subscriber impact |
|---|---|---|
| Laptop PEM only | Re-pull from `heroku config:get` | None |
| Heroku config var only | Re-upload from any laptop copy (`heroku config:set VAPID_PRIVATE_KEY_B64="$(base64 < ~/.vapid/pabx-vapid-private.pem | tr -d '\n')"`) | None — within seconds the dyno restarts with the same key |
| Both Heroku config **and** every laptop copy | Generate a new keypair (rotation) | Every push subscription wiped; users must re-enable in UI |

## See also

- `modules/module/pbx/inc/push_sender.hpp` — cloud-side VAPID JWT + Web Push encryption.
- `ui/src/common/push.service.ts` — browser subscription flow.
- `docker/Dockerfile.cloud` — `VAPID_PRIVATE_KEY_B64 → /tmp/vapid.pem` entrypoint logic.
