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

## Recovery — if the key is lost

Identify which copy you've lost; the fix depends on it. The two
recoverable scenarios have **zero subscriber impact**. The third
forces every browser to re-subscribe.

### Scenario 1 — local PEM lost, Heroku config intact

No regeneration needed. Pull it back down:

```bash
mkdir -p ~/.vapid
heroku config:get VAPID_PRIVATE_KEY_B64 -a pabx \
  | base64 -d > ~/.vapid/pabx-vapid-private.pem
chmod 600 ~/.vapid/pabx-vapid-private.pem
```

### Scenario 2 — Heroku config lost, local PEM intact

Re-upload from the laptop copy. Both halves can be derived from the
private PEM, so one command does it:

```bash
heroku config:set -a pabx \
  VAPID_PRIVATE_KEY_B64="$(base64 < ~/.vapid/pabx-vapid-private.pem | tr -d '\n')" \
  VAPID_PUBLIC_KEY="$(openssl ec -in ~/.vapid/pabx-vapid-private.pem -pubout -outform DER 2>/dev/null \
                      | tail -c 65 | base64 | tr '+/' '-_' | tr -d '=')" \
  VAPID_SUBJECT='mailto:ops@example.com'
```

Heroku auto-restarts the dyno. No browser-side change.

### Scenario 3 — both lost, must regenerate from scratch

> **Cost:** every existing browser push-subscription becomes
> undeliverable. Users have to re-enable push in the UI (Settings →
> Enable). Stored `push_subscriptions` documents on Mongo will get
> cleaned up lazily by `PushSender`'s 410 handling, or you can wipe
> them manually (step 7 below).

```bash
# 1. Generate fresh P-256 keypair
mkdir -p ~/.vapid
openssl ecparam -name prime256v1 -genkey -noout -out ~/.vapid/pabx-vapid-private.pem
chmod 600 ~/.vapid/pabx-vapid-private.pem

# 2. Derive public key (base64url, 87 chars, starts with 'B')
PUB=$(openssl ec -in ~/.vapid/pabx-vapid-private.pem -pubout -outform DER 2>/dev/null \
      | tail -c 65 | base64 | tr '+/' '-_' | tr -d '=')

# 3. Encode private PEM
B64=$(base64 < ~/.vapid/pabx-vapid-private.pem | tr -d '\n')

# 4. Push to Heroku (auto-restarts dyno)
heroku config:set -a pabx \
  VAPID_PUBLIC_KEY="$PUB" \
  VAPID_PRIVATE_KEY_B64="$B64" \
  VAPID_SUBJECT='mailto:ops@example.com'

# 5. Verify cloud is serving the new key
sleep 5 && curl -sS https://pabx-5fbf3550f938.herokuapp.com/api/v1/push-vapid-key
#    Expect: {"key":"<same value as $PUB>"}

# 6. Verify entrypoint decoded the PEM and pbx-cloud got --vapid-key-path
heroku logs -a pabx -n 100 | grep "opt -V"
#    Expect: opt -V = /tmp/vapid.pem

# 7. (Optional) Wipe stale subscriptions
#    mongo shell:
#      db.push_subscriptions.deleteMany({})

# 8. From the softphone: Settings → Enable. Place an inbound call
#    to a closed tab. Notification fires.
```

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

### … you need to rotate the keypair (planned)

Operationally identical to [Scenario 3](#scenario-3--both-lost-must-regenerate-from-scratch)
above — same subscriber-wipe cost, same commands. Same recipe applies
whether you're rotating because of a suspected compromise or because
both copies of the key were lost.

### … you're bootstrapping a brand-new cloud app

Same generate-then-`heroku config:set` flow as
[Scenario 3](#scenario-3--both-lost-must-regenerate-from-scratch),
just against the new app name. There are no subscribers yet, so the
"cost" caveat doesn't apply.

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
