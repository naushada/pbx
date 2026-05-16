# `docker/coturn/`

coturn config **template** rendered per society. The rendered output
is what `docker-compose.agent.yml` actually mounts into the
`pbx-coturn` container (`coturn/coturn:4.6`).

## What's in this directory

| File | Role |
|------|------|
| `turnserver.conf.template` | coturn config skeleton with `${REALM}`, `${STATIC_AUTH_SECRET}`, `${EXTERNAL_IP}` placeholders. Defines the listening port, `use-auth-secret` mode for time-limited credentials, the realm scope, and the public IP coturn announces in STUN replies. |

`scripts/setup-society.sh` substitutes the three placeholders and
writes the rendered output to `${TURN_CONF_PATH:-./certs/turnserver.conf}`.
That rendered file is **per-society** (real secret, real IP, real
realm) and lives outside the repo on purpose — it's secret-bearing
config the operator manages alongside the cert mount.

## Where the resulting `pbx-coturn` container runs

**On-prem only.** `pbx-coturn` service in `docker-compose.agent.yml`,
alongside `pbx-mongo`, `pbx-asterisk`, `pbx-agent`, `pbx-wsdbagent`.
Heroku has no coturn — TURN traffic never reaches the cloud.

The container uses `network_mode: host` so coturn can announce the
host's real public IP in STUN replies. The trade-off is loss of
bridge-network isolation; the simpler alternative (publish a single
port through compose's NAT) wouldn't give coturn access to the real
IP and STUN replies would carry the bridge's internal address
instead.

## Why coturn at all

| Call shape | Media path | coturn role |
|------------|-----------|-------------|
| 1:1 inside the society's LAN | Direct browser↔browser P2P via host ICE candidates | Not used |
| 1:1 with one browser off-LAN | TURN relay through coturn | **Required** |
| ConfBridge conference | Each leg's RTP traverses the on-prem Asterisk box (server-side mixer) | Not used |

See [`DESIGN.md`](../../DESIGN.md) §3.2 + §6.2/§6.3 for the call-shape
breakdown.

## Runtime contract

| Thing | How it's wired |
|-------|----------------|
| Listening port | `${TURN_PUBLIC_PORT:-3478}` (set in `.env`). Society DNATs ONE public UDP port to this. |
| Mounted config | `${TURN_CONF_PATH:-./certs/turnserver.conf}` → `/etc/coturn/turnserver.conf:ro`. Rendered by `setup-society.sh`; NOT committed. |
| Auth | Time-limited credentials minted by the cloud's `GET /api/v1/turn-credentials` endpoint: `username=<unix-expiry>:<sipUsername>`, `password=HMAC-SHA1(static-auth-secret, username)`. TTL 300 s. RFC 5766 §5. |
| Shared secret | `static-auth-secret` in the rendered conf MUST match `societies.turnSharedSecret` in Mongo. setup-society.sh seeds both from a single `openssl rand -base64 32`. |

## Required setup before first `podman-compose up`

```sh
./scripts/setup-society.sh --society-id=<id> --turn-secret=AUTOGEN \
  --public-ip=<ip> --turn-port=3478
```

That:
1. Generates a 32-byte base64 shared secret (or reuses an existing one
   in `certs/turnserver.conf`).
2. Renders `turnserver.conf.template` → `certs/turnserver.conf` with
   the real realm + IP + secret.
3. Also generates the Asterisk DTLS cert — see
   [`docker/asterisk/README.md`](../asterisk/README.md).

The secret then needs to be mirrored into Mongo (`societies.turnSharedSecret`)
so the cloud's `handle_turn_credentials_GET` can sign creds with the
same key coturn validates them against. The cloud's "create society"
flow does this when run normally; if you're testing manually, drop
the secret onto the society doc via `mongosh`.

## Rotating the secret

1. Re-run `setup-society.sh` (or hand-edit `static-auth-secret=` in
   the rendered file).
2. Update `societies.turnSharedSecret` in Mongo to match.
3. `podman-compose -f docker-compose.agent.yml restart pbx-coturn`.

Existing calls survive — TURN allocations are valid until their 5-min
TTL. New allocations use the new secret.

## See also

- [`docker-compose.agent.yml`](../../docker-compose.agent.yml) — the
  `pbx-coturn` service that mounts the rendered conf.
- [`scripts/setup-society.sh`](../../scripts/setup-society.sh) —
  template renderer + secret generator.
- [`DESIGN.md`](../../DESIGN.md) §3.2 (TURN auth flow), §6.2 / §6.3
  (when TURN is on the media path).
- [`modules/module/pbx/src/microservice_pbx.cpp`](../../modules/module/pbx/src/microservice_pbx.cpp)
  `handle_turn_credentials_GET` — the cloud-side credential minter.
