# onprem-pbx — Release v1.0

- **Release date:** 2026-05-21
- **Tag:** `v1.0`
- **Branch:** `release/v1.0`
- **Baseline commit:** `766603a` (`main`)

---

## Overview

onprem-pbx is a VoIP intercom/PBX for residential societies. It pairs a
cloud-hosted control plane with a small on-premise stack and a browser
softphone — residents call each other (and the gate/guard) from any
device, no desk phone or app install.

```
   Resident browser  ──wss──►  Cloud (Heroku)  ──wss /agent──►  pbx-agent ─► Asterisk
   (Angular softphone)         control plane                    on-prem society stack
```

- **Cloud control plane** — one Heroku dyno: REST API, the Angular
  softphone bundle, SIP-over-WebSocket relay, Web Push, presence.
- **On-prem society stack** — Asterisk (SIP+RTP), coturn (STUN/TURN),
  MongoDB, and two tunnel daemons (`pbx-agent`, `pbx-wsdbagent`) that
  dial out to the cloud over mutually-authenticated TLS. No inbound
  ports.
- **Web softphone** — Angular + Clarity SPA: login, directory, click-to-
  call, inbound calls with ringtone + Web Push wake-up, society
  conference, call history.

v1.0 is the first release in which a resident-to-resident call connects
**and carries two-way audio** end-to-end, verified live.

---

## What's in v1.0

| Area | Capability |
|---|---|
| Calling | 1-to-1 resident calls, forked-ring to all devices on a flat, first-answer-wins |
| Conference | Society shared room (Asterisk ConfBridge) — dial in, others join automatically |
| Directory | Searchable neighbour directory with live online/offline presence |
| Inbound | Ringtone + Web Push notification so a call rings even with the tab backgrounded |
| Media | WebRTC: Opus audio, DTLS-SRTP encryption, ICE with coturn STUN/TURN relay |
| Provisioning | Subscribers imported via CSV/XLSX; agent materialises Asterisk endpoints from MongoDB within ~200 ms (change-stream driven) |
| Security | Outer TLS + inner mTLS on both cloud tunnels; cloud bearer-token is the sole SIP auth layer; per-call DTLS fingerprint |
| Operations | One-command installer, systemd unit, auto cert-refresh sidecar, CDR records, admission control |
| Admin | Vaadin admin UI for society self-service + bulk subscriber import |

**Tests:** 597 C++ unit/integration tests (582 passing; 15 are
documented environment-dependent baseline — 10 InnerTLS handshake tests
needing certs, 5 cloud tests needing MongoDB), plus the UI karma +
Playwright suites.

### Call-path completion (the work that made v1.0 shippable)

The final push closed the gap between "call rings" and "call has audio":

- **#118** — Asterisk `ari.conf websocket_write_timeout` raised to 10 s.
  The agent's ARI WebSocket was being torn down mid-call; calls stalled
  at "Calling…".
- **#122** — `AriDispatchTask`: ARI event processing moved off the
  reactor thread (Active Object pattern). Removes the root cause of the
  ARI socket starvation; includes a fix for an SSL-write race that
  crashed the agent mid-call.
- **#120** — the agent now originates call legs with `formats=opus`, so
  both browser legs negotiate Opus and the bridge passes audio without a
  transcoder.
- **#121** — `CallRouter` now answers the caller channel, so the
  caller's browser receives the `200 OK` and completes its media
  negotiation.

These build on the earlier 2026-05 call-path series (PRs #101–#117):
SIP REGISTER routing, dynamic pjsip provisioning, presence, conference
join, and reverse-path signalling fixes.

---

## Prerequisites

### Society machine (on-prem)

| Requirement | Detail |
|---|---|
| OS | Ubuntu 22.04 or 24.04 (amd64 or arm64). Linux only for production. |
| RAM | ≥ 3 GB |
| Disk | ≥ 5 GB free |
| Software | `podman` + `podman-compose`, `systemd`, `openssl`, `jq`, `curl`, `python3` — all auto-installed by `install.sh` |
| Network out | TCP 443 outbound to the cloud host (no inbound ports needed) |
| Network in (TURN) | One public UDP port (default 3478) DNAT'd from the society's router to the `pbx-coturn` container — required for residents calling from outside the LAN |
| Privilege | `install.sh` must run as `root` (`sudo`) |

### Accounts & artifacts (provided by the dev/operations team)

- A **Heroku** account hosting the cloud control plane.
- **Docker Hub** reachability — the society stack pulls
  `docker.io/naushada/onprem-pbx-{agent,wsdbagent}`.
- A **per-society certificate tarball** (e.g. `SUNSET-certs.tar.gz`),
  produced by `./deploy-heroku.sh package-society-certs <CODE>` after a
  cloud deploy. This carries the InnerTLS + agent mTLS material.
- A **society code** (short label, e.g. `SUNSET`).

### Build/deploy machine (dev team only)

- Heroku CLI, `podman`, and registry login for the cloud deploy.

---

## Deployment

### A. Cloud control plane (Heroku) — dev/operations team

```sh
./deploy-heroku.sh login       # one-time: authenticate to registry.heroku.com
./deploy-heroku.sh deploy      # build → extract per-society certs → push → release
```

`deploy` runs the full pipeline; the individual steps (`build`, `push`,
`release`) are also available. The UI app has its own targets
(`deploy-ui`) and `deploy-all` does both.

Required Heroku config vars (set once, on the `pabx` app):

| Var | Purpose |
|---|---|
| `REMOTE_DB=1` | Use the on-prem Mongo tunnel (society data stays on-prem) |
| `VAPID_PUBLIC_KEY`, `VAPID_PRIVATE_KEY_B64`, `VAPID_SUBJECT` | Web Push (RFC 8292) |
| `TURN_URL`, `TURN_SHARED_SECRET` | Must match the on-prem coturn config |
| `PBX_AUTH_STRICT=1` | Enforce password verification (set after subscribers are seeded) |

After every cloud deploy, re-package and ship fresh society certs — see
**Upgrades & cert refresh** below.

### B. On-prem society stack — society machine

```sh
# 1. Get the repo and the society cert tarball onto the machine
git clone https://github.com/naushada/pbx ~/onprem-pbx
cd ~/onprem-pbx
#    copy SUNSET-certs.tar.gz here (scp / USB)

# 2. Run the installer (interactive — prompts for society code, cloud
#    host, cert tarball path, society name, admin email, admin password)
sudo ./install.sh
```

Unattended install (CI / scripted) — pass every input as an env var:

```sh
sudo SOCIETY_CODE=SUNSET \
     SOCIETY_NAME='Sunset Towers' \
     ADMIN_EMAIL=admin@sunset.example \
     ADMIN_PASSWORD='choose-a-strong-one' \
     CLOUD_HOST=pabx-5fbf3550f938.herokuapp.com \
     CERTS_TARBALL=/path/to/SUNSET-certs.tar.gz \
     ./install.sh
```

`install.sh` is idempotent and takes ~3–5 minutes. It: preflight-checks
the host, installs dependencies, stages the repo to `/opt/onprem-pbx`,
unpacks the certs, generates the per-society Asterisk DTLS cert + coturn
config, writes `.env`, pulls images, brings the stack up, waits for the
inner-TLS handshake, seeds MongoDB with the society + first admin, and
installs the `onprem-pbx` systemd unit (auto-start on reboot).

**Windows:** supported but **not recommended for production** — WSL2's
UDP NAT breaks RTP audio. A `docker run … onprem-pbx-installer` path is
documented in [`INSTALL-windows.md`](./INSTALL-windows.md).

The full operator guide, including the troubleshooting table, is in
[`INSTALL.md`](./INSTALL.md).

#### The on-prem stack (`docker-compose.agent.yml`)

| Service | Role |
|---|---|
| `pbx-mongo` | Society database (subscribers, sessions, CDR) — single-node replica set so change streams work |
| `pbx-asterisk` | SIP + RTP server (chan_pjsip over WebSocket, ARI control, ConfBridge) |
| `pbx-coturn` | STUN/TURN relay for off-LAN browsers (host networking) |
| `pbx-agent` | Control-plane daemon — dials the cloud `/agent` tunnel, drives call routing + provisioning |
| `pbx-wsdbagent` | DB tunnel — dials the cloud `/ws/db`, keeps society data on-prem |
| `pbx-cert-watcher` | Sidecar — restarts the agents automatically when refreshed certs land |

---

## Running & verifying

### Start / stop / status (society machine)

```sh
sudo systemctl start   onprem-pbx      # bring the stack up
sudo systemctl stop    onprem-pbx      # take it down
sudo systemctl restart onprem-pbx      # restart all services
sudo systemctl status  onprem-pbx      # unit status
sudo podman ps --filter name=pbx-      # running containers
```

### Verify the deployment

Within ~120 s of first boot, the agent logs should show the tunnel and
ARI coming up:

```sh
sudo podman logs pbx-agent | tail -50
#   AceSslTransport: connected + WS-upgraded
#   AceSslTransport: inner-TLS handshake established
#   SubscriberWatcher … run_full_scan complete
#   AriWsClient: connected to Asterisk ARI

sudo podman logs pbx-wsdbagent | tail -20    # expect "session started"
```

Check SIP endpoints once subscribers are seeded:

```sh
sudo podman exec pbx-asterisk asterisk -rx "pjsip show endpoints"
sudo podman exec pbx-asterisk asterisk -rx "pjsip show contacts"
```

### Use the softphone

1. A resident opens the cloud URL (e.g.
   `https://pabx-5fbf3550f938.herokuapp.com/`) in a browser.
2. Logs in with their **flat number** + **password** (the installer
   seeds the first `ADMIN` subscriber; residents are added via the
   admin UI or CSV import — template at
   [`docs/subscribers-template.xlsx`](./docs/subscribers-template.xlsx)).
3. The dashboard auto-connects; the Directory shows neighbours with live
   presence. Click **Call** to dial; **Join conference** for the shared
   room.
4. A successful call: both sides reach a running call timer and hear
   each other.

### Add subscribers

Through the admin UI, or by CSV import:

```sh
curl -X POST 'https://<cloud-host>/api/v1/subscriber/import?societyId=<CODE>' \
  -F 'csv=@subscribers.csv'
```

The agent provisions the new Asterisk endpoints automatically within
~200 ms.

---

## Upgrades & cert refresh

Every cloud deploy mints a fresh CA, so society machines need refreshed
certs to keep the tunnels authenticated:

```sh
# Dev machine, after ./deploy-heroku.sh deploy
./deploy-heroku.sh package-society-certs SUNSET     # → /tmp/SUNSET-certs.tar.gz

# Society machine — re-run the installer (idempotent)
sudo CERTS_TARBALL=/path/to/new/SUNSET-certs.tar.gz ./install.sh
```

`pbx-cert-watcher` detects the new certs within 5 s and restarts
`pbx-agent` + `pbx-wsdbagent` — no manual restart needed.

---

## Known limitations

- **`direct_media` on provisioned endpoints** is left `true`. It is
  benign today (DTLS-SRTP forces Asterisk to stay on the media path via
  `simple_bridge`), but should be set `false` for WebRTC peers in a
  follow-up.
- **No Opus transcoder** in the Asterisk image — all call legs must use
  Opus (v1.0 ensures this). A future non-browser endpoint would need a
  shared codec or `codec_opus`.
- **Windows hosting** is not production-grade — WSL2 UDP NAT breaks RTP.
- **`pbx-mongo` healthcheck** can intermittently report `unhealthy`
  while the database is in fact serving — cosmetic; does not affect
  calls.
- 15 of 597 C++ tests are environment-dependent baseline (need
  certs/MongoDB) and are expected to be skipped/failing outside CI.

---

## Diagnostics & support

Collect logs for the dev team:

```sh
sudo podman logs --tail 100 pbx-agent     > /tmp/pbx-agent.log
sudo podman logs --tail 100 pbx-wsdbagent > /tmp/pbx-wsdbagent.log
sudo podman logs --tail 100 pbx-asterisk  > /tmp/pbx-asterisk.log
sudo systemctl status onprem-pbx          > /tmp/pbx-systemd.log
```

See the troubleshooting table in [`INSTALL.md`](./INSTALL.md), the
operator runbook in [`ARCHITECTURE.md`](./ARCHITECTURE.md), and the
design docs under [`docs/design/`](./docs/design/).
