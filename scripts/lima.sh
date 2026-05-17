#!/usr/bin/env bash
# scripts/lima.sh — bring the FULL on-prem container stack up inside a
# Lima VM (Apple Virtualization framework, native arm64 on Apple Silicon
# — no QEMU). Brings up the same five services that production runs:
#
#   pbx-mongo · pbx-asterisk · pbx-coturn · pbx-agent · pbx-wsdbagent
#
# composed from docker-compose.agent.yml. pbx-agent + pbx-wsdbagent dial
# the deployed Heroku cloud (default pabx-5fbf3550f938.herokuapp.com),
# so a successful run end-to-end exercises the real cloud-tunnel path.
#
# Usage:
#   lima start   provision VM, acquire pbx-cpp-builder:bootstrap image
#                (stream from host podman if present, build inside VM
#                otherwise), `podman-compose up --build -d`, watch
#                ${RUN_BUDGET_SECS} (default 120s), capture per-
#                container logs + status. Idempotent — re-runs reuse
#                the VM, the bootstrap image, and built containers.
#                Also resumes from a prior `lima stop` (VM restart +
#                compose up is fast — under a minute).
#   lima stop    `podman-compose stop` inside VM, then `limactl stop`
#                the VM itself. NON-DESTRUCTIVE: containers, volumes,
#                Mongo data, the bootstrap image, and the VM disk all
#                stay put — `lima start` resumes everything. Frees RAM
#                while paused. Use this for an "I'll resume tomorrow"
#                pause; use `lima del` when you actually want a
#                clean slate.
#   lima del     Full nuke: `podman-compose down -v` (drops named
#                volumes — Mongo data goes too), then stop + delete the
#                VM (releases its ~30 GB disk image AND the bootstrap
#                image cached in it). Next `lima start` is a cold
#                bootstrap — ~30 min if the host podman doesn't have a
#                matching-arch bootstrap to stream in.
#
# Why a Lima VM rather than `podman build` on macOS: podman-on-macOS
# emulates linux/amd64 via QEMU, which segfaults on signals during
# ACE+OpenSSL multithreaded I/O (observed during PR #25's dry test).
# Lima's vz mode is a real Linux kernel, native arch, no emulation.
#
# Why a separate VM rather than the developer's macOS Podman Machine:
# clean slate per run (no surprises from leftover containers/volumes),
# matches the on-prem topology exactly (host networking for coturn,
# compose bridge for the rest).

set -euo pipefail

VM=vm-onprem-pbx
LEGACY_VMS=(onprem-pbx-test)   # cleaned up by `lima stop` so old names don't linger
REPO_HOST="$(cd "$(dirname "$0")/.." && pwd -P)"
HEROKU_HOST="${HEROKU_HOST:-pabx-5fbf3550f938.herokuapp.com}"
HEROKU_PORT="${HEROKU_PORT:-443}"
SOCIETY_ID="${SOCIETY_ID:-demo-society}"
RUN_BUDGET_SECS="${RUN_BUDGET_SECS:-120}"
LOG=/tmp/lima.sh.log
COMPOSE=docker-compose.agent.yml

usage() {
  cat <<EOF
usage: $(basename "$0") {start|stop|del|shell [cmd...]|list|logs [-f] <container>|help}
  start            provision Lima VM '${VM}' OR resume a stopped one, acquire
                   pbx-cpp-builder:bootstrap, bring the 6-container on-prem
                   stack up via podman-compose against ${HEROKU_HOST}:${HEROKU_PORT},
                   watch for ${RUN_BUDGET_SECS}s.
  stop             pause: \`podman-compose stop\` inside the VM, then \`limactl stop\`
                   the VM itself. Non-destructive — Mongo data, the bootstrap
                   image, and the VM disk all stay; \`lima start\` resumes.
                   Frees RAM while paused.
  del              nuke: \`podman-compose down -v\` (drops named volumes — Mongo
                   data goes too), then stop + delete the VM (and any legacy
                   ones). Releases the ~30 GB VM disk; next \`lima start\` is
                   a cold bootstrap.
  shell [cmd...]   shell into the VM. With no args, interactive shell.
                   With args, runs them inside the VM and returns:
                     lima shell                          # interactive bash
                     lima shell sudo podman ps           # one-shot command
                     lima shell sudo podman logs pbx-agent | tail -50
  list             list all Lima VMs (thin wrapper around \`limactl list\`).
  logs [-f] NAME   tail container logs from inside the VM. -f follows.
                     lima logs pbx-agent                 # last 100 lines
                     lima logs -f pbx-agent              # follow
                   Bare \`lima logs\` lists running pbx-* containers.
  help             print this message and exit 0.
EOF
  exit "${1:-1}"
}

cmd="${1:-}"
case "$cmd" in
  help|-h|--help) usage 0 ;;
  start) ;;
  stop)
    # Non-destructive pause. Targets only the current VM — legacy VMs
    # are only cleaned up by `del`, since they shouldn't be running.
    printf '\n\033[1;34m[lima] pausing compose stack + VM %s\033[0m\n' "$VM"
    if limactl list -q 2>/dev/null | grep -qx "$VM"; then
      # `compose stop` keeps the container definitions + volumes around;
      # next `compose up` resumes them instantly. Best-effort — a
      # crashed compose still lets the VM stop cleanly below.
      limactl shell "$VM" -- bash -c \
        "cd '$REPO_HOST' && sudo podman-compose -f $COMPOSE stop 2>/dev/null" || true
      # `limactl stop` (no -f) shuts the VM down gracefully and leaves
      # its disk image intact. Strip the `time=…` noise that lima
      # emits.
      limactl stop "$VM" 2>&1 | grep -v "^time=" || true
      echo "[lima] paused. resume with: lima start"
      echo "[lima] nuke the VM instead with: lima del"
    else
      echo "[lima] no VM named '$VM' — nothing to stop."
    fi
    exit 0
    ;;
  del|delete)
    # Destructive nuke. Drops the compose volumes (Mongo data goes
    # too), then deletes the VM disk. Iterates legacy VMs as well —
    # historical names that should never linger.
    printf '\n\033[1;34m[lima] tearing down compose stack + VM %s\033[0m\n' "$VM"
    for v in "$VM" "${LEGACY_VMS[@]}"; do
      if limactl list -q 2>/dev/null | grep -qx "$v"; then
        # Best-effort compose-down -v inside the VM before deletion.
        limactl shell "$v" -- bash -c \
          "cd '$REPO_HOST' && sudo podman-compose -f $COMPOSE down -v 2>/dev/null" || true
        limactl stop -f "$v" 2>&1 | grep -v "^time=" || true
        limactl delete -f "$v" 2>&1 | grep -v "^time=" || true
        echo "[lima] removed VM '$v'"
      fi
    done
    echo "[lima] done — compose stack down, VM and its disk image are gone."
    exit 0
    ;;
  list|ls)
    # Thin wrapper — host-side, no VM required. Useful for "is my VM
    # still around / what state is it in" without typing limactl.
    exec limactl list
    ;;
  logs)
    shift
    follow_flag=""
    if [ "${1:-}" = "-f" ]; then follow_flag="-f"; shift; fi
    if ! limactl list -q 2>/dev/null | grep -qx "$VM"; then
      echo "[lima] no VM named '$VM' — run \`lima start\` first." >&2
      exit 1
    fi
    if [ "$#" -eq 0 ]; then
      # No container name → show what's running so the user can pick.
      echo "[lima] running containers (pick one for \`lima logs <name>\`):"
      exec limactl shell "$VM" -- sudo podman ps \
              --filter name=pbx- --format 'table {{.Names}}\t{{.Status}}'
    fi
    container="$1"
    if [ -n "$follow_flag" ]; then
      # Follow mode — pass through verbatim so Ctrl-C tears the stream
      # cleanly and the user sees podman's native trailing newline.
      exec limactl shell "$VM" -- sudo podman logs -f "$container"
    else
      # Snapshot — last 100 lines is enough to see boot + the
      # current crash-loop cycle without flooding the terminal.
      exec limactl shell "$VM" -- sudo podman logs --tail 100 "$container"
    fi
    ;;
  shell|vm)
    # `vm` retained as a hidden alias for the old name (PR #38 shipped
    # it; PR #65 renamed to `shell` to match limactl + xpmile lingo).
    # Drop the `vm` alias after a few weeks once muscle memory fades.
    shift   # drop the subcommand; remainder (if any) runs inside the VM
    if ! limactl list -q 2>/dev/null | grep -qx "$VM"; then
      echo "[lima] no VM named '$VM' — run \`lima start\` first." >&2
      exit 1
    fi
    if [ "$#" -eq 0 ]; then
      # Interactive shell — drop the `bash -c` wrapper so the user gets
      # a real TTY with their shell history etc. Print a banner BEFORE
      # handing off so the user has a clear "yes I'm in the PABX VM" cue.
      printf '\n\033[1;36m'
      cat <<'BANNER'
########     ###    ########  ##     ##
##     ##   ## ##   ##     ##  ##   ##
##     ##  ##   ##  ##     ##   ## ##
########  ##     ## ########     ###
##        ######### ##     ##   ## ##
##        ##     ## ##     ##  ##   ##
##        ##     ## ########  ##     ##
BANNER
      printf '\033[0m\n  on-prem PABX dev VM — \033[1msudo podman ps\033[0m for container status\n\n'
      exec limactl shell "$VM"
    else
      # One-shot command. `--` passes through to limactl shell, then to
      # the user's args verbatim — quoting/globbing behaves the way
      # they typed it on the host.
      exec limactl shell "$VM" -- "$@"
    fi
    ;;
  *) usage ;;
esac

# Shadow stdout/stderr so step output lands on screen AND in the log
# file for post-mortem.
exec > >(tee "$LOG") 2>&1

step() { printf '\n\033[1;34m[lima] %s\033[0m\n' "$*"; }
SH()   { limactl shell "$VM" -- bash -c "$1"; }

# Sentinel files (inside the VM) for skip-already-done.
SENT_APT=/var/lib/lima-apt-installed
SENT_HOSTNAME=/var/lib/lima-hostname-set

# ─── 1. Provision / reuse the Lima VM ─────────────────────────────────────
step "ensure Lima VM '$VM' is up"
if ! limactl list -q 2>/dev/null | grep -qx "$VM"; then
  cat > /tmp/${VM}.yaml <<EOF
vmType: vz
cpus: 4
memory: 6GiB
disk: 30GiB
images:
- location: "https://cloud-images.ubuntu.com/releases/24.04/release/ubuntu-24.04-server-cloudimg-arm64.img"
  arch: "aarch64"
mounts:
- location: "$REPO_HOST"
  writable: true
mountType: virtiofs
EOF
  limactl create --name="$VM" --tty=false /tmp/${VM}.yaml
  limactl start "$VM"
else
  limactl start "$VM" 2>/dev/null || true
fi

# ─── 2. Apt deps — podman + podman-compose ────────────────────────────────
#
# Noble (24.04) ships podman-compose in main; pip3 fallback covers
# downgrades to older codenames. --break-system-packages because PEP 668
# blocks system-wide pip on Noble; safe here — the VM is single-purpose
# and discarded by `lima stop`.
#
# The `unqualified-search-registries` drop-in is what makes bare
# `mongo:7` / `andrius/asterisk:20` / `coturn/coturn:4.6` resolve to
# docker.io. Stock Debian/Ubuntu podman ships with that list empty for
# safety; without it, every short-name pull hard-errors with "did not
# resolve to an alias." Same surface as Docker, just opt-in.
step "apt deps (podman + podman-compose + registries config)"
SH "
  set -e
  if [ -f $SENT_APT ]; then exit 0; fi
  sudo apt-get update -qq
  sudo apt-get install -y -qq ca-certificates curl podman python3-pip
  sudo apt-get install -y -qq podman-compose \
    || sudo pip3 install --break-system-packages podman-compose
  echo 'unqualified-search-registries = [\"docker.io\"]' \
    | sudo tee /etc/containers/registries.conf.d/00-docker.conf >/dev/null
  sudo touch $SENT_APT
"

# ─── 2b. Set VM hostname → onprem-pabx (cosmetic, for prompt) ─────────────
#
# Default Lima hostname is 'lima-${VM}'; renaming it to onprem-pabx
# gives every shell session (interactive or one-shot) a prompt like
# 'naushada@onprem-pabx:~\$' so the operator can never confuse the VM
# shell with their macOS shell. /etc/hosts gets the same entry so
# sudo's reverse-lookup doesn't pause for a few seconds.
step "set VM hostname → onprem-pabx (cosmetic — for prompt)"
SH "
  set -e
  if [ -f $SENT_HOSTNAME ]; then exit 0; fi
  sudo hostnamectl set-hostname onprem-pabx
  if ! grep -q '127.0.1.1 onprem-pabx' /etc/hosts; then
    echo '127.0.1.1 onprem-pabx' | sudo tee -a /etc/hosts >/dev/null
  fi
  sudo touch $SENT_HOSTNAME
"

# ─── 3. Acquire pbx-cpp-builder:bootstrap image ───────────────────────────
#
# The agent/wsdbagent Dockerfiles `FROM localhost/pbx-cpp-builder:bootstrap`
# — that image bakes ACE/TAO 7.0.0 + mongo-cxx + googletest into an
# ubuntu:20.04 base (matches Dockerfile.agent's runtime stage glibc).
# Three acquisition paths, fastest first:
#
#   a) already in VM         — no-op (re-run case).
#   b) host image arch=VM    — stream via `podman save | podman load`.
#   c) host image arch≠VM    — build inside VM (Apple Silicon devs
#                              usually have only the amd64 deploy
#                              bootstrap on the host; streaming it
#                              would produce `Exec format error` at
#                              every `RUN` step in stage 1).
#   d) host has no bootstrap — build inside VM. ~30 min cold,
#                              sentinel-cached as a tagged image.
step "acquire pbx-cpp-builder:bootstrap image"
vm_uname=$(SH "uname -m" 2>/dev/null | tr -d '\r\n')
case "$vm_uname" in
  aarch64|arm64) vm_arch=arm64 ;;
  x86_64|amd64)  vm_arch=amd64 ;;
  *) vm_arch=unknown ;;
esac
host_arch=$(podman image inspect localhost/pbx-cpp-builder:bootstrap \
              --format '{{.Architecture}}' 2>/dev/null || echo "")

if SH "sudo podman image exists localhost/pbx-cpp-builder:bootstrap" 2>/dev/null; then
  echo "[lima] bootstrap image already present in VM"
elif [ -n "$host_arch" ] && [ "$host_arch" = "$vm_arch" ]; then
  echo "[lima] streaming bootstrap image from host podman → VM podman..."
  podman save localhost/pbx-cpp-builder:bootstrap \
    | limactl shell "$VM" -- sudo podman load
else
  if [ -n "$host_arch" ]; then
    echo "[lima] host bootstrap is $host_arch but VM is $vm_arch — building inside VM (~30 min)"
  else
    echo "[lima] bootstrap not on host either — building inside VM (~30 min)"
  fi
  # Build from docker/Dockerfile.bootstrap (the same file
  # deploy-heroku.sh's `cmd_build_bootstrap` uses for the amd64 host
  # build). One source, both build contexts. The repo is bind-mounted
  # into the VM at $REPO_HOST so podman build inside the VM sees it.
  SH "cd $REPO_HOST && sudo podman build \
        -t localhost/pbx-cpp-builder:bootstrap \
        -f docker/Dockerfile.bootstrap ."
fi

# ─── 4. Per-society config (Asterisk DTLS cert + turnserver.conf) ─────────
#
# setup-society.sh is idempotent on existing artifacts — it'll skip
# regeneration when certs/asterisk-dtls/pbx.crt and certs/turnserver.conf
# both already exist. EXTERNAL_IP=127.0.0.1 keeps coturn happy for boot;
# actual TURN traffic doesn't reach the VM in a dry test, so the IP
# only needs to render-substitute without error.
step "setup-society.sh ($SOCIETY_ID)"
SH "
  cd $REPO_HOST
  if [ ! -f certs/turnserver.conf ] || [ ! -f certs/asterisk-dtls/pbx.crt ]; then
    EXTERNAL_IP=127.0.0.1 ./scripts/setup-society.sh $SOCIETY_ID
  else
    echo '[lima] society artifacts already present — skipping'
  fi
"

# ─── 5. .env for docker-compose.agent.yml ─────────────────────────────────
#
# Overwrites any prior .env on every run so re-runs against a different
# HEROKU_HOST / SOCIETY_ID just work. CERTS_DIR points at the in-repo
# certs/agent-deployed/ triple shipped for the live cloud.
step "write .env (CLOUD_HOST=$HEROKU_HOST, SOCIETY_ID=$SOCIETY_ID)"
SH "cat > $REPO_HOST/.env <<ENVEOF
CLOUD_HOST=$HEROKU_HOST
AGENT_SOCIETY_ID=$SOCIETY_ID
CERTS_DIR=$REPO_HOST/certs/agent-deployed
MONGO_DB_NAME=pabx
TURN_PUBLIC_PORT=3478
ENVEOF
"

# ─── 6. Bring the 6-container stack up ────────────────────────────────────
step "podman-compose up --build -d"
SH "cd $REPO_HOST && sudo podman-compose -f $COMPOSE up --build -d"

# ─── 7. Wait for boot + handshake, then capture state ─────────────────────
step "wait ${RUN_BUDGET_SECS}s for Mongo RS init + cloud-tunnel handshake"
SH "sleep $RUN_BUDGET_SECS"

step "container status"
SH "sudo podman ps -a --filter name=pbx- --format 'table {{.Names}}\t{{.Status}}'"

step "pbx-agent logs (last 60 lines)"
SH "sudo podman logs pbx-agent 2>&1 | tail -60 || echo '[lima] pbx-agent not running'"

step "pbx-wsdbagent logs (last 30 lines)"
SH "sudo podman logs pbx-wsdbagent 2>&1 | tail -30 || echo '[lima] pbx-wsdbagent not running'"

step "pbx-asterisk logs (last 20 lines)"
SH "sudo podman logs pbx-asterisk 2>&1 | tail -20 || echo '[lima] pbx-asterisk not running'"

step "done — stack still up inside VM; \`lima stop\` to tear down. Full log at $LOG"
