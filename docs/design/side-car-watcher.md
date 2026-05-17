# `pbx-cert-watcher` — cert-rotation sidecar

The on-prem stack runs five containers (Mongo, Asterisk, coturn,
pbx-agent, pbx-wsdbagent). One of them, `pbx-cert-watcher`, is the
operational glue that nobody talks about until certs rotate and
nothing reconnects.

This doc explains what it does, why it's a sidecar, and the
trade-offs behind a 30-line shell script that nobody should ever
need to touch.

---

## 1. The problem

```
┌─────────────── cloud deploy (Heroku) ──────────────────┐
│  Every `./deploy-heroku.sh deploy` AND every CI deploy │
│  mints a FRESH CA + new client cert family inside      │
│  Dockerfile.cloud. The cloud trusts ONLY its current   │
│  CA's certs (PRs #55, #61).                            │
└─────────────────────────────────────────────────────────┘
                            │
                            │  operator ships
                            │  the new tarball
                            ▼
┌────────── society on-prem stack ────────────────────────┐
│   pbx-agent + pbx-wsdbagent read certs ONCE at startup. │
│   They DON'T watch the cert files. After the tarball is │
│   unpacked into ./certs/cloud-issued/{innertls,         │
│   sip-agent}/, the agents keep using the OLD certs in   │
│   memory until manually restarted.                      │
└─────────────────────────────────────────────────────────┘
```

Without a watcher: the operator has to remember to run
`podman restart pbx-agent pbx-wsdbagent` after every cert refresh.
Easy to forget. The agents then fail the inner-TLS handshake on
their next reconnect cycle and the society goes dark.

---

## 2. What the sidecar does

```
┌─ pbx-cert-watcher (alpine:3.19, ~30 lines of sh) ──────────────┐
│                                                                 │
│  while true; do                                                 │
│    NEW = md5sum of every *.crt/*.key/*.pem under /watch         │
│    if NEW != LAST and LAST != "" (real change, not boot):       │
│        for c in pbx-wsdbagent pbx-agent:                        │
│            curl -X POST /libpod/containers/$c/restart           │
│              --unix-socket /run/podman/podman.sock              │
│    LAST = NEW                                                   │
│    sleep 5                                                      │
│  done                                                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
        │                                            │
        │ bind-mount of cert dir                     │ DooD —
        │ (read-only)                                │ podman socket
        ▼                                            ▼ mounted RW
  /watch  ←  ./certs/cloud-issued/             /var/run/podman/podman.sock
                                                ↑ ↑
                  ┌─────────────────────────────┘ └────────────┐
                  ▼                                            ▼
            pbx-agent                                pbx-wsdbagent
       (restarted on cert change)             (restarted on cert change)
```

- The script lives inline in `docker-compose.agent.yml` (search for
  `pbx-cert-watcher:`). Tiny enough to keep with the compose file
  itself — no separate Dockerfile, no entrypoint script.
- Image is stock `docker.io/library/alpine:3.19` + a single
  `apk add curl` at startup. Total RSS at idle: ~3 MB.
- The `LAST != ""` guard suppresses the **first** detection so a
  cold container start doesn't restart the agents pointlessly.

---

## 3. Why a sidecar — vs. building it into each agent

| Approach | Pro | Con |
|---|---|---|
| **inotify in each agent binary** | No extra container | Doubles the cert-watch logic; each agent owns its own restart strategy; bloats the C++ binary with file-watch infra; can't be tuned without a rebuild + redeploy |
| **Polling in each agent** | Same — no extra container | Same problems as inotify; each agent's polling cadence drifts independently |
| **Sidecar (chosen)** | Single source of truth, ONE decision per cert change, restart logic outside the binary (tune without rebuild), portable (alpine works on Pi/x86 identically), ~30 lines of bash | One extra container (3 MB RSS); needs podman socket mounted (DooD trade-off — see §6) |

The dominant reason is **single source of truth**. If both agents
watched independently, a cert rotation that finishes mid-poll could
restart them at slightly different times. With a sidecar driving
both, the restarts are sequential within the same loop iteration —
one observed cert change ⇒ one batch of peer restarts.

---

## 4. Why polling — vs. inotify

- Cert rotation isn't latency-critical. 5 s detection is fine; the
  next cloud↔agent reconnect cycle takes seconds anyway.
- Polling is **portable**. md5sum + shell + sleep work the same
  on amd64 / arm64 / x86_64 alpine. inotify needs `inotify-tools`
  (extra dep) and Linux-specific syscalls — not a problem in
  practice but extra surface for nothing.
- md5-of-md5sums is one line of bash:
  ```sh
  find /watch -type f \( -name '*.crt' -o -name '*.key' \) \
       -exec md5sum {} + 2>/dev/null | sort | md5sum | cut -d' ' -f1
  ```
- 5 s overhead is one md5 of ~6 small files = sub-millisecond CPU per
  poll. Zero practical cost.

---

## 5. The DooD (Docker-out-of-Docker) trick

The watcher's job is to restart **peer containers, not itself**.
It does this by talking to the host's podman engine via the Unix
socket bind-mounted at `/run/podman/podman.sock`:

```yaml
volumes:
  - /run/podman/podman.sock:/var/run/podman/podman.sock
```

From inside the watcher container that path is a local Unix socket;
`curl --unix-socket` POSTs to podman's libpod API which then acts on
the host containers (`/libpod/containers/<name>/restart`).

Same pattern as the new `onprem-pbx-installer` container (PR #97).
Both use DooD to manipulate the host engine from inside a small
helper container without needing podman-in-podman or any nested
runtime.

---

## 6. Trade-offs of mounting the podman socket

The podman socket gives the watcher **root-equivalent access** to
the host's container engine. That's a meaningful privilege grant:
the watcher could in theory create / destroy / exec any container,
read any bind-mounted volume, etc.

Mitigations:

- The watcher's `command:` is hard-coded in `docker-compose.agent.yml`
  — not loading code from disk, not reaching the network for code.
  An attacker would need to first modify the operator's compose file
  to inject anything new.
- Container runs as root inside its own namespace but only invokes
  one specific endpoint (`/libpod/containers/<name>/restart`).
- The host podman socket on the society machine is owned by the
  operator account; if podman is rootless (default on Ubuntu 24.04
  installs), the watcher can only act on that user's containers,
  not the whole system.

**Net judgement:** acceptable for the on-prem stack where the
threat boundary is the operator's own machine. The watcher is part
of the trusted compute base, same as the agent binaries themselves.

---

## 7. Operational notes

### Why I keep hitting `container has dependent containers` during dev

`pbx-cert-watcher` `depends_on: pbx-agent` (so it doesn't restart
agents before they're up). podman enforces this dependency on
removal — `podman rm pbx-agent` alone fails because cert-watcher
still references it. Recreate flow:

```sh
podman rm -f pbx-cert-watcher pbx-agent       # dependent first
podman-compose up -d pbx-agent pbx-cert-watcher
```

### Logs

The watcher logs each loop iteration as
`[cert-watcher HH:MM:SS] cert change detected (OLDMD5 -> NEWMD5);
restarting peers`. Greppable signal in production:

```sh
podman logs --tail 50 pbx-cert-watcher | grep "cert change"
```

If you see this line right before a `pbx-agent` `inner-TLS
handshake failed`, that's the sidecar firing on a stale tarball
(operator dropped wrong file in `./certs/cloud-issued/`).

### Failure modes

| Mode | Symptom | Recovery |
|---|---|---|
| podman socket not mounted | `curl: connection refused` in watcher log | Check `/run/podman/podman.sock` exists on host + compose mount path matches |
| cert dir empty | watcher silently never fires | Verify `./certs/cloud-issued/` has files; check tarball was extracted there |
| Wrong cert family shipped | watcher fires, agents restart, inner-TLS still fails | Operator unpacked an old or wrong-society tarball; re-pull from cloud |
| podman socket permission denied | watcher logs `curl: permission denied` | rootless podman config — check the watcher container's user matches the socket owner |

---

## 8. Future TODOs

- Add a `--dry-run` mode that logs intended restarts without firing
  the curl POST, for operator dry-test of new cert tarballs.
- Optional Prometheus counter (`cert_rotations_observed_total`) for
  ops visibility — currently the only evidence is in the log.
- Inotify-mode flag for sub-second detection if a future deployment
  needs faster pickup (probably never; 5 s is fine).
