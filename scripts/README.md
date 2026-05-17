# scripts/lima.sh

One-shot harness that brings the **full on-prem container stack** up
inside a Lima VM on the developer's Mac, then points it at the
deployed Heroku cloud. Used for live integration tests when
`offtarget`'s unit tests aren't enough.

## Why a Lima VM (and not host podman)

Podman-on-macOS emulates `linux/amd64` via QEMU. QEMU segfaults on
signals during ACE+OpenSSL multithreaded I/O — observed during PR
#25's dry test and reproduced repeatedly since.

Lima's `vz` mode (Apple Virtualization framework) runs a real Linux
kernel at the host's native arch (arm64 on Apple Silicon). No
emulation, no QEMU, no signal-handling lottery.

The VM is single-purpose and discarded by `lima stop` — clean slate
per run, no leftover containers / volumes / DBs from yesterday's test.

## What `lima start` brings up

Six containers, defined in [`docker-compose.agent.yml`](../docker-compose.agent.yml):

| Container          | Image                                                       | Role                                                                         |
|--------------------|-------------------------------------------------------------|------------------------------------------------------------------------------|
| `pbx-mongo`        | `mongo:7` (1-node replica set, healthcheck does `rs.initiate`) | On-prem subscriber/CDR DB. Provisioner reads it via change-streams.       |
| `pbx-asterisk`     | local build `pbx-asterisk:latest` (Asterisk 20)             | SIP/RTP. Driven via ARI dynamic-config by `pbx-agent`.                       |
| `pbx-coturn`       | `coturn/coturn:4.6`                                         | TURN/STUN for WebRTC NAT traversal.                                          |
| `pbx-agent`        | local build `onprem-pbx-agent:latest`                       | The on-prem control-plane daemon. Dials `/agent` on the Heroku cloud.        |
| `pbx-wsdbagent`    | local build `pbx-wsdbagent:latest`                          | Bridges Heroku → on-prem Mongo over `/ws/db`. Cloud reads/writes via it.     |
| `pbx-cert-watcher` | local build `pbx-cert-watcher:latest`                       | Sidecar that watches the cloud-issued cert dir and HUPs containers on rotation. |

`pbx-agent` and `pbx-wsdbagent` both reach the cloud at
`${HEROKU_HOST}:${HEROKU_PORT}` (default
`pabx-5fbf3550f938.herokuapp.com:443`).

## Subcommands

```
lima start                     # provision VM, build/pull bootstrap, compose up
lima stop                      # compose down + delete VM (releases ~30 GB disk)
lima list                      # `limactl list` — what VMs exist on the host
lima shell                     # interactive bash inside the VM
lima shell <cmd...>            # one-shot — runs in VM and returns
lima logs                      # list running pbx-* containers
lima logs <container>          # last 100 lines from one container
lima logs -f <container>       # follow
```

The `lima` shim is added to `$PATH` by your dev setup (otherwise call
`./scripts/lima.sh <subcommand>`).

## How to see what's running

Three equivalent ways, in order of typing speed:

| Command                                 | What it shows                                                              |
|-----------------------------------------|----------------------------------------------------------------------------|
| `lima logs`                             | Names + status of every `pbx-*` container in the VM.                       |
| `lima shell sudo podman ps`             | Full `podman ps` table (image, ports, uptime).                             |
| `lima shell` then `sudo podman ps`      | Interactive shell — useful when you'll run several commands back-to-back.  |

> **Note:** `sudo` is required for `podman` inside the VM — the
> compose stack runs as root inside the VM. (Outside, on the
> developer's macOS, podman is rootless; the inversion catches
> people once and never again.)

Then `lima logs [-f] <container>` to read or follow that container's
log. Example:

```sh
lima logs pbx-agent | grep "inner-TLS handshake"
lima logs -f pbx-asterisk        # watch a live call
```

## Already inside the VM

Once you're inside the VM (`lima shell` with no args), the `lima` shim
is gone — you talk to podman directly. The compose stack runs as root,
so every podman call needs `sudo`:

```sh
sudo podman ps                                  # running pbx-* containers
sudo podman ps -a                               # include stopped
sudo podman logs pbx-agent | tail -60           # snapshot
sudo podman logs -f pbx-agent                   # follow
sudo podman exec -it pbx-mongo mongosh          # interactive Mongo shell
sudo podman restart pbx-agent                   # bounce one container
sudo podman-compose -f docker-compose.agent.yml \
     up --build -d pbx-agent                    # rebuild + recreate one service
```

The repo is bind-mounted (virtiofs) at the **same absolute path** as
on macOS, so `cd /Users/<you>/repo/onprem-pbx` works inside the VM
and any file you edit on macOS is visible immediately — no `git pull`
inside the VM, no copying.

To get back to the host, `exit` the shell. To go back in for one more
command, `lima shell <cmd…>` from macOS.

## Common workflows

### Re-test after a cloud code change

```sh
# 1. Build + push the new pbx-cloud to Heroku (use podman, see
#    project_open_backlog.md "Deploy workaround" note).
podman build -f docker/Dockerfile.cloud -t localhost/onprem-pbx-cloud:latest .
podman tag localhost/onprem-pbx-cloud:latest \
    registry.heroku.com/pabx/web:latest
podman push --format=v2s2 registry.heroku.com/pabx/web:latest
heroku container:release web --app pabx

# 2. Bounce the on-prem agent so it reconnects to the new cloud.
lima shell sudo podman restart pbx-agent
lima logs -f pbx-agent           # watch for "inner-TLS handshake established"
```

### Re-test after an agent code change

```sh
# 1. Rebuild + recreate just pbx-agent inside the VM. The other five
#    containers stay up.
lima shell "cd $(pwd) && sudo podman-compose -f docker-compose.agent.yml \
    up --build -d pbx-agent"
lima logs -f pbx-agent
```

### Inspect on-prem Mongo

```sh
lima shell sudo podman exec -it pbx-mongo mongosh \
    --eval 'db.getSiblingDB("pabx").subscribers.find().toArray()'
```

### Tear everything down

```sh
lima stop                        # compose down -v, then VM delete
```

After `lima stop` the VM disk image (~30 GB) is gone. The next
`lima start` is a cold provision: ~30 min if the cached
`pbx-cpp-builder:bootstrap` image isn't on the host either (see below).

## The `pbx-cpp-builder:bootstrap` image

The agent/wsdbagent/cloud Dockerfiles all `FROM
localhost/pbx-cpp-builder:bootstrap`. That image bakes ACE/TAO 7.0.0
+ mongo-c-driver 1.19.1 + mongo-cxx-driver v3.6 + googletest 1.12.1
into an `ubuntu:20.04` base. Building it from source is ~30 min
(the inline `Dockerfile.bootstrap` heredoc in this script).

`lima start` acquires the image via this priority chain:

1. Already in the VM → no-op.
2. On host podman with matching arch → stream
   `podman save | podman load` into the VM (~2 min).
3. On host podman but wrong arch → rebuild inside the VM (~30 min).
4. Not on host either → build inside the VM (~30 min).

**Heads-up:** the Lima VM's disk is 30 GB. Container builds + the
bootstrap image fill it fast. If `podman build` starts hanging or
returning "no space left on device", clear out cached layers:

```sh
lima shell sudo podman image prune -f
```

…but note this will evict `pbx-cpp-builder:bootstrap` too, so the
next `lima start` rebuild is the slow path. See
`project_open_backlog.md` and the `podman-VM-disk-full` memory for
the full incident history.

## Environment overrides

| Variable           | Default                                  | Purpose                                                                  |
|--------------------|------------------------------------------|--------------------------------------------------------------------------|
| `HEROKU_HOST`      | `pabx-5fbf3550f938.herokuapp.com`        | Cloud hostname the agent dials.                                          |
| `HEROKU_PORT`      | `443`                                    | Cloud port.                                                              |
| `SOCIETY_ID`       | `demo-society`                           | Written into `.env` as `AGENT_SOCIETY_ID`; used by `setup-society.sh`.   |
| `RUN_BUDGET_SECS`  | `120`                                    | How long `lima start` waits before snapshotting container logs + status. |

Set them on the command line:

```sh
SOCIETY_ID=soc_sunset lima start
```

## Where the script writes things

| Path                                  | What                                                                |
|---------------------------------------|---------------------------------------------------------------------|
| `/tmp/lima.sh.log`                    | Full step-by-step log of the last `lima start` run.                 |
| `/tmp/vm-onprem-pbx.yaml`             | Lima VM spec passed to `limactl create`. Re-used on existing VM.    |
| `<repo>/build-lima/Dockerfile.bootstrap` | The bootstrap Dockerfile, written if a rebuild is needed.         |
| `<repo>/.env`                         | Compose env file. **Overwritten on every `lima start`.**             |
| `<repo>/certs/turnserver.conf`        | Generated by `setup-society.sh` on the first run for a new society. |
| `<repo>/certs/asterisk-dtls/pbx.crt`  | Same.                                                                |

The `<repo>` dir is bind-mounted into the VM via virtiofs, so files
written by the VM are visible on the host immediately.

## Troubleshooting

| Symptom                                                                            | What to try                                                                                                                       |
|------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------|
| `[lima] no VM named 'vm-onprem-pbx'` from `lima shell`                             | Run `lima start` first.                                                                                                           |
| `podman ps` shows no containers after `lima start`                                  | Read `/tmp/lima.sh.log` — usually a podman-compose build error in step 6.                                                         |
| Agent logs show `inner-TLS handshake failed`                                       | Cloud is stale. Redeploy `pbx-cloud` (Heroku registry push), then `lima shell sudo podman restart pbx-agent`.                     |
| Agent logs show `verify failed`                                                    | Cloud-issued cert in `certs/agent-deployed/` doesn't chain to the cloud's CA. See `project_agent_inner_tls_cert_reject` memory.   |
| Browser SIP REGISTER 408s after upgrade                                            | See `project_sip_ws_observability` memory — cloud-side LM_INFO logs added 2026-05-17 narrow down where the frame is dropped.       |
| `Exec format error` during build inside VM                                          | Host bootstrap image is wrong arch. Delete it with `podman rmi localhost/pbx-cpp-builder:bootstrap` and re-`lima start` to rebuild. |
| VM disk full / `podman build` hangs                                                | `lima shell sudo podman image prune -f`. Will evict the bootstrap; next rebuild is ~30 min.                                       |
| Need to clean up an orphaned legacy VM (`onprem-pbx-test`)                          | `lima stop` already does this. If the VM is still in `limactl list`, run `limactl delete -f onprem-pbx-test`.                     |

## See also

- [`docker-compose.agent.yml`](../docker-compose.agent.yml) — the six-service definition.
- [`ARCHITECTURE.md` § 6.2a](../ARCHITECTURE.md) — end-to-end dry-test narrative.
- [`README.md` § One-shot dry test](../README.md) — top-level summary that links here.
- [`scripts/lima.sh`](./lima.sh) — the script itself; inline comments cover acquisition-path heuristics and per-step rationale.
