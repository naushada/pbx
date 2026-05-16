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

---

## InnerTLS on the `/agent` SIP tunnel (PR #19)

The same pattern documented above for `/ws/db` is now also applied to
the `/agent` SIP tunnel. Same shared CA, same trust boundary, same
`certs/innertls/` material.

| Side | Cert role | Provenance | CLI flag(s) |
|------|-----------|------------|-------------|
| Cloud | server | `Dockerfile.cloud` signs `server.crt` at build time against the repo CA, key regenerated per build | `--tls-cert` / `--tls-key` / `--tls-ca` (reused from `/ws/db`) |
| Agent | client | operator mounts `CERTS_DIR` at runtime (`agent.crt` / `agent.key` / `cloud-ca.pem`) | `--inner-tls-cert` / `--inner-tls-key` / `--inner-tls-ca` / `--inner-tls-hostname` |

The shared core is **`WsInnerTlsBridge`** in `modules/module/security/`.
It implements `IInnerTlsTransport` (the renamed `innertls.hpp` interface)
and has two modes:

- **Blocking** — used during the initial handshake. `recv()` does a
  synchronous socket read + WS-deframe + auto-pong; `send()` encodes
  one WS binary frame and writes.
- **Buffered** — switched to after the handshake. The reactor's
  `handle_input` deframes WS itself, calls `push_inbound(payload)`
  for each binary frame, then calls `InnerTls::recv()` to drain
  plaintext. An empty queue returns true with empty data (not a
  close) so `InnerTls::recv` hits `WANT_READ` and exits cleanly.

The handshake therefore blocks one reactor thread briefly (typically
<50 ms over a real Heroku tunnel). v1 architecture is
one-agent-per-cloud-dyno so the stall is acceptable.

### `AgentStream` lifecycle (cloud side)

`WebConnection`'s `/agent` upgrade handler reads
`CloudTunnelEndpoint::inner_tls_config()`; if a cert is configured it
takes the production path:

1. `AgentStream(endpoint, fd, /*auto_attach=*/false)` — wraps the fd
   but does NOT call `endpoint.on_agent_connected` yet.
2. `as->setup_inner_tls(cert, key, ca)` — runs `InnerTlsServer::accept`
   synchronously over the WS frames. Returns false on handshake
   failure; caller `delete`s and returns 500.
3. `as->attach()` — publishes the `TransportAdapter` to
   `CloudTunnelEndpoint::on_agent_connected`, which flushes any
   outbound frames buffered while disconnected.
4. `as->register_with_reactor()` — READ_MASK + 25 s WS-ping timer.

The split between (1)–(2) and (3) is **load-bearing**: if the endpoint
got the adapter before the handshake completed, a buffered outbound
frame from a prior disconnect would write to the WS plaintext and the
agent (now expecting encrypted bytes) would refuse the frame. The
existing `auto_attach=true` ctor remains for tests that drive
`handle_input` directly with raw frames.

### `AceSslTransport` lifecycle (agent side)

`AceSslTransport::connect_and_handshake()` runs the outer mTLS dial,
sends the WS upgrade request, validates the `101` response, then —
when `--inner-tls-cert` is set — layers the inner TLS:

1. Builds `WsInnerTlsBridge` in blocking mode (`client_mask=true`).
2. Builds `InnerTlsClient` over the bridge.
3. `set_ca` + `set_cert` from the CLI flags.
4. `handshake()` — synchronous loop. Drains the socket via the
   bridge's blocking `recv`.
5. `verify_hostname()` if `--inner-tls-hostname` is set.
6. `switch_to_buffered()` on the bridge.
7. Seeds `m_recv_buf` with any leftover socket bytes the handshake
   pulled past the final `Finished` record (so app data that arrived
   in the same TCP segment isn't lost on the first `handle_input`).

After step 7, `handle_input` deframes WS, pushes binary payloads into
the bridge, and calls `InnerTls::recv` to drain plaintext into the
existing `m_on_bytes` callback. The plaintext shape is unchanged
SipFrame bytes, so `SipFrameDemux` upstream needs no changes.

### Sequence

```
Agent                                           Cloud
─────                                           ─────
ACE_SSL_SOCK_Connector::connect                ─►  TCP + outer TLS
                                                   handshake (Heroku
                                                   router terminates)
GET /agent  Upgrade: websocket                 ─►  WebConnection
                                                   sees `/agent`,
                                                   constructs AgentStream
                                                   with auto_attach=false
WsInnerTlsBridge (blocking, client_mask=true)  ◄═►  WsInnerTlsBridge (blocking, server_mask=false)
InnerTlsClient.handshake()                     ◄═►  InnerTlsServer.accept()
                                                       │
                                                       ▼ success
WsInnerTlsBridge.switch_to_buffered()             WsInnerTlsBridge.switch_to_buffered()
m_recv_buf = leftover                              m_recv_buf = leftover
                                                   AgentStream.attach()
                                                       │
                                                       ▼
                                                   endpoint.on_agent_connected
                                                   (flushes buffered frames)
                                                       │
                                                       ▼
register_with_reactor()                            register_with_reactor()
                                                   timer: 25 s WS-ping

steady state:
  AceSslTransport.send(plaintext)
  → InnerTls.send → SSL_write
  → m_send_raw → outer WS binary frame
                                                ─►  handle_input
                                                   → bridge.push_inbound(payload)
                                                   → InnerTls.recv → plaintext
                                                   → endpoint.on_bytes_received
```

The two layers stay independent of each other and of the
WS-level keep-alive ping (`AgentStream`'s 25 s `0x9` ping vs the
SipFrame `0x04` `PING`/`PONG` at 15 s) — see `DESIGN.md` §7.

## Peer cert CN exposure (PR #25)

`InnerTlsServer::peer_subject_cn()` returns the verified agent cert's
Subject CN via `SSL_get_peer_certificate` + `X509_NAME_get_text_by_NID(NID_commonName)`.
The cloud's `AgentStream::setup_inner_tls()` captures it into
`m_peer_cn` post-handshake and logs `(peer CN=…)` so multi-tenant
deployments can see which agent attached. Reserved for future
cross-checks against `AGENT_HELLO`'s claimed `societyId` — if the
cert CN doesn't match, the cloud could refuse `bootstrap_society()`
rather than trust the agent's self-declared id.

Returns empty when the client presented no cert (the
`SSL_VERIFY_PEER` without `FAIL_IF_NO_PEER_CERT` permitted-anonymous
path). The cloud treats empty as "unknown agent" and falls back to
the AGENT_HELLO payload.

### Latent bug fixed by PR #25

`InnerTlsClient::set_cert` originally called `SSL_CTX_use_certificate_file`
+ `SSL_CTX_use_PrivateKey_file`. Those load the cert into the **CTX**, which
only seeds the cert for SSL objects created **after** the call. `m_ssl` was
already built in the ctor (`SSL_new(m_ctx.get())`) BEFORE `set_cert` runs,
so the SSL object inherited an empty default cert at `SSL_new` time and
never picked up what `set_cert` loaded.

Net effect: **every existing `/ws/db` + `/agent` deployment was running
anonymous-client mTLS — the server saw no cert and `peer_subject_cn()`
would have returned empty even when operator passed `--tls-cert` /
`--inner-tls-cert`**. The handshake still succeeded because
`SSL_VERIFY_PEER` accepts anonymous; the cert just never got presented.

Fix: switch to per-SSL `SSL_use_certificate_file` / `SSL_use_PrivateKey_file`
/ `SSL_check_private_key`. After the fix, the new
`PeerSubjectCn_ReadableAfterMtlsHandshake` test exercises the actual mTLS
path and confirms the server reads the client cert's CN.
