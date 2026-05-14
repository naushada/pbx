# InnerTLS — cloud ↔ on-prem DB tunnel

## Why

Heroku Common Runtime terminates TLS at the edge router. Everything past
the router travels plaintext between the router and the dyno, and the
dyno itself has no way to authenticate which client opened a given
WebSocket. For an opaque DB tunnel this is unacceptable: a hostile
peer could open `wss://pabx-….herokuapp.com/ws/db`, get past the WS
upgrade, and start sending DB requests as if it were the on-prem agent.

The fix is a second TLS handshake **inside** the WebSocket frames.
Both peers verify each other's leaf certificate against a CA they
share out-of-band; only after the inner handshake completes does
either side accept BSON-framed DB traffic.

```
pbx-wsdbagent ── outer WSS ──► Heroku router ──► dyno (wsdbproxy)
                                                      │
            (Heroku-terminated TLS ends here)         ▼
              ┌──────── plaintext WS frames ────────┐
              │  inner TLS handshake (ACE InnerTLS) │
              │  - server cert: CN=pabx.herokuapp.com│
              │  - client cert: CN=pbx-wsdbagent     │
              │  - both signed by onprem-pbx-CA      │
              └──────────────────────────────────────┘
              │ BSON-encoded DbRequest / DbResponse  │
              └──────────────────────────────────────┘
```

## Cert layout

| File         | Holder        | Distribution             | Committed? |
|--------------|---------------|--------------------------|------------|
| `ca.crt`     | both peers    | repo + baked into image  | yes        |
| `ca.key`     | build only    | consumed by Dockerfile   | no         |
| `server.crt` | cloud         | baked into image         | yes (ref)  |
| `server.key` | cloud         | regenerated per build    | no         |
| `client.crt` | wsdbagent     | mounted at runtime       | yes        |
| `client.key` | wsdbagent     | mounted at runtime       | no         |

Generator: `certs/innertls/generate.sh`.

The server cert's CN is the **short** Heroku name `pabx.herokuapp.com`,
not the assigned `pabx-<suffix>.herokuapp.com`. The outer WSS still
targets the assigned name (since Heroku's edge cert covers that); the
inner handshake only checks against `TLS_HOSTNAME=pabx.herokuapp.com`,
which keeps the inner cert stable across Heroku-side hostname reissues.

## Build-time baking (cloud)

`docker/Dockerfile.cloud` copies the repo CA + key into a build-only
location, mints a fresh `server.key`, signs `server.crt` against the
repo CA, then deletes `ca.key` before the runtime layer is composed.
Runtime stage only gets `ca.crt`, `server.crt`, `server.key`. The
private CA never lives in any pushed image layer.

## Runtime wiring

**Cloud** (`pbx-cloud` invocation, set by `Dockerfile.cloud` env):
```
--tls-cert /opt/pbx-cloud/certs/server.crt
--tls-key  /opt/pbx-cloud/certs/server.key
--tls-ca   /opt/pbx-cloud/certs/ca.crt
```

**Agent** (`pbx-wsdbagent`, via `docker-compose.agent.yml`):
```
TLS_CERT=/opt/wsdbagent/innertls/client.crt
TLS_KEY=/opt/wsdbagent/innertls/client.key
TLS_CA=/opt/wsdbagent/innertls/ca.crt
TLS_HOSTNAME=pabx.herokuapp.com
```
Volume mount: `./certs/innertls:/opt/wsdbagent/innertls:ro`.

## Verifying a healthy handshake

Cloud logs should show, per agent reconnect:
```
[WsDbServer] agent connected
[WsDbServer] inner TLS established
[WsDbServer] run_session started
```

If you instead see `inner TLS cert/key not configured — skipping inner
TLS`, the cloud was deployed without the `TLS_*` env vars — check the
Dockerfile baked them into ENV defaults.

If the agent reports `inner TLS handshake failed` with exponential
backoff (5s → 10s → 20s → 40s), the most common causes are:

1. **CA mismatch** — agent's `ca.crt` doesn't match the CA that
   signed cloud's `server.crt`. Re-run `certs/innertls/generate.sh`
   on a clean repo and rebuild the cloud image.
2. **Hostname mismatch** — cloud's cert CN ≠ agent's `TLS_HOSTNAME`.
   Cert is minted with `CN=pabx.herokuapp.com`; agent must use the
   same string.
3. **Stale dyno** — Heroku may still be running the previous slug
   while the new one starts. The agent's exponential backoff handles
   this naturally; wait one cycle (≤40s) after `heroku container:release`.

## Rotating the CA

1. `cd certs/innertls && ./generate.sh` — overwrites `ca.{crt,key}`
   and both leaves.
2. Rebuild the cloud image (`Dockerfile.cloud` will pick up the new CA
   automatically and sign a fresh `server.crt` against it).
3. Push + release the cloud.
4. On every on-prem host, `git pull` to pick up the new `ca.crt` +
   `client.crt`, then `podman-compose restart pbx-wsdbagent`. The
   agent's existing TCP/WS session will reconnect with the new
   material on the next backoff cycle.

Rotation is **all-or-nothing**: agents on the old CA can't talk to a
new-CA cloud, and vice versa. Stagger the cloud + agent updates inside
one maintenance window.
