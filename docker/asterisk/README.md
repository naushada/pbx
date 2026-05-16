# `docker/asterisk/`

Six minimal Asterisk config files mounted read-only into the
`pbx-asterisk` container (`andrius/asterisk:20`). They're the entire
Asterisk surface the on-prem stack uses — chan_pjsip over WebSocket
for SIP-WS browsers, ARI for control, ConfBridge for conferences.

## What's in this directory

| File | Role |
|------|------|
| `asterisk.conf` | Base options. Minimal — Asterisk's default config tree has dozens of files; we only ship the six we actually configure. |
| `http.conf` | Built-in HTTP server. Hosts `/ari/*` (consumed by `AriRestClient` / `AriWsClient`) and `/ws` (chan_pjsip WS transport for browser SIP-WS). Bound to `0.0.0.0:8088`, internal to the `pbx-net` compose bridge. |
| `ari.conf` | Single ARI user `asterisk:asterisk`. The agent's `AriClient` subscribes to channel + bridge events; `AriRestClient` issues `continue` / `originate` / `bridge` / `hangup` / dynamic-config PUTs. **Override the dev creds in production.** |
| `pjsip.conf` | WebSocket transport + `[endpoint-resident-template]` with the codec / DTLS / WebRTC field set + sample `alice` / `bob` / `conf` dev fixtures. **Production subscriber endpoints are NOT in this file** — they're pushed dynamically by `PjsipProvisioner` via ARI dynamic-config (DESIGN.md §4.1). The template field set is drift-checked against `PjsipProvisioner` by `PjsipTemplateDrift` (PR #22). |
| `extensions.conf` | Dialplan. Stasis app `pbx` hands every inbound INVITE to the agent's `AriClient`; `pbx-busy` plays "all lines busy" for over-cap rejection; `pbx-conf-room` handles ConfBridge join. |
| `confbridge.conf` | One shared conference profile. The UI dials `sip:conf@pbx.<society>` → chan_pjsip matches the user-part to the `[conf]` endpoint in `pjsip.conf` → dialplan ConfBridge()s the channel. |

## Where the resulting `pbx-asterisk` container runs

**On-prem only.** Brought up as the `pbx-asterisk` service in
`docker-compose.agent.yml` alongside `pbx-mongo`, `pbx-coturn`,
`pbx-agent`, `pbx-wsdbagent`. Not used by the cloud — Heroku has no
Asterisk and no SIP.

The agent reaches Asterisk at `pbx-asterisk:8088` via compose service
DNS (matches the `ast_host` default baked into
`pbx-agent/src/main/main.cpp`). The port is **not** exposed to the
host — chan_pjsip's WS transport has no auth at the WS layer (SIP
digest handles authentication), so we keep it internal.

## Runtime contract

Two volumes:

| Mount | Source | Why |
|-------|--------|-----|
| `/etc/asterisk/*.conf` | Each file in this directory, `:ro` | The six configs above. |
| `/etc/asterisk/keys/` | `${ASTERISK_KEYS_DIR:-./certs/asterisk-dtls}`, `:ro` | DTLS-SRTP cert + key (`pbx.crt`, `pbx.key`). Browsers verify the SDP `a=fingerprint:sha-256 …` line per call, not a CA chain (RFC 5764), so self-signed is correct. |

Required setup before first `podman-compose up`:

```sh
./scripts/setup-society.sh --society-id=<id> --turn-secret=AUTOGEN \
  --public-ip=<ip> --turn-port=3478
```

That generates the Asterisk DTLS cert at the default `ASTERISK_KEYS_DIR`
path. The same script also renders the coturn config — see
[`docker/coturn/README.md`](../coturn/README.md).

## Dynamic vs static endpoints

The two endpoint pools coexist:

| Pool | Source | Used for |
|------|--------|----------|
| **Static** — `alice`, `bob`, `conf` in `pjsip.conf` | Hand-written config file | Dev fixtures (REGISTER as alice/bob from a soft-phone for local testing); `conf` is the synthetic endpoint that catches `sip:conf@…` for ConfBridge join. |
| **Dynamic** — every real subscriber | `PjsipProvisioner` PUTs auth/aor/endpoint sorcery objects via ARI dynamic-config (`/ari/asterisk/config/dynamic/res_pjsip/...`) | Production subscribers. Created on `SubscriberWatcher` bootstrap full-scan + every Mongo change-stream event. |

The dynamic pool inherits **nothing** from `pjsip.conf`'s
`[endpoint-resident-template]` — sorcery dynamic-config doesn't apply
`(!)`-marked templates. The drift-check test enforces that
`PjsipProvisioner`'s inlined field set matches the template; edit
either side and the test fires noisily.

## See also

- [`docker-compose.agent.yml`](../../docker-compose.agent.yml) —
  the `pbx-asterisk` service that mounts these files.
- [`scripts/setup-society.sh`](../../scripts/setup-society.sh) —
  generates the DTLS cert at first run.
- [`DESIGN.md`](../../DESIGN.md) §3.2 (agent components),
  §4.1 (pjsip provisioning), §8 (DTLS-SRTP media security).
- [`pbx-agent/src/main/pjsip_provisioner.{cpp,hpp}`](../../pbx-agent/src/main/) —
  the dynamic-config producer that PRs subscribers into Asterisk's
  sorcery from Mongo.
