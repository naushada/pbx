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
#   lima stop    `podman-compose down -v` inside VM, then stop +
#                delete the VM (releases its ~30 GB disk image).
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
usage: $(basename "$0") {start|stop|vm [cmd...]}
  start         provision Lima VM '${VM}', acquire pbx-cpp-builder:bootstrap,
                bring the 5-container on-prem stack up via podman-compose
                against ${HEROKU_HOST}:${HEROKU_PORT}, watch for ${RUN_BUDGET_SECS}s.
  stop          compose down + stop + delete the VM (and any legacy ones).
  vm [cmd...]   shell into the VM. With no args, interactive shell.
                With args, runs them inside the VM and returns:
                  lima vm                          # interactive bash
                  lima vm sudo podman ps           # one-shot command
                  lima vm sudo podman logs pbx-agent | tail -50
EOF
  exit 1
}

cmd="${1:-}"
case "$cmd" in
  start) ;;
  stop)
    printf '\n\033[1;34m[lima] tearing down compose stack + VM %s\033[0m\n' "$VM"
    for v in "$VM" "${LEGACY_VMS[@]}"; do
      if limactl list -q 2>/dev/null | grep -qx "$v"; then
        # Best-effort compose-down inside the VM before deletion.
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
  vm)
    shift   # drop the `vm` arg; remainder (if any) runs inside the VM
    if ! limactl list -q 2>/dev/null | grep -qx "$VM"; then
      echo "[lima] no VM named '$VM' — run \`lima start\` first." >&2
      exit 1
    fi
    if [ "$#" -eq 0 ]; then
      # Interactive shell — drop the `bash -c` wrapper so the user gets
      # a real TTY with their shell history etc.
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
  # Heredoc the Dockerfile through `tee` to the virtiofs-mounted repo
  # so podman build can pick it up. Build context is empty — every
  # source is pulled from upstream inside the Dockerfile.
  SH "mkdir -p $REPO_HOST/build-lima && cat > $REPO_HOST/build-lima/Dockerfile.bootstrap" <<'DOCKEREOF'
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake build-essential libboost-all-dev \
        libssl-dev libzstd-dev libsasl2-dev zlib1g-dev \
        wget git pkg-config && rm -rf /var/lib/apt/lists/*

# ACE/TAO 7.0.0. ssl=1 on the install line is load-bearing — without
# it the link step fails with `cannot find -lACE_SSL`.
ENV ACE_PREFIX=/usr/local/ACE_TAO-7.0.0
RUN cd /root && \
    wget -q https://github.com/DOCGroup/ACE_TAO/releases/download/ACE%2BTAO-7_0_0/ACE+TAO-7.0.0.tar.gz && \
    tar -xzf ACE+TAO-7.0.0.tar.gz && rm ACE+TAO-7.0.0.tar.gz && \
    cd ACE_wrappers && \
    echo '#include "ace/config-linux.h"' > ace/config.h && \
    printf 'include $(ACE_ROOT)/include/makeinclude/platform_linux.GNU\n' \
        > include/makeinclude/platform_macros.GNU && \
    make install ssl=1 INSTALL_PREFIX=$ACE_PREFIX ACE_ROOT=$(pwd) \
        SSL_ROOT=/usr/include/openssl -j$(nproc) && \
    echo $ACE_PREFIX/lib > /etc/ld.so.conf.d/ace.conf && ldconfig

# mongo-c-driver 1.19.1 — install prefix must be /usr/local to match
# what mongo-cxx-driver's pkg-config probe expects.
RUN cd /root && \
    git clone -b 1.19.1 --depth 1 https://github.com/mongodb/mongo-c-driver.git && \
    cd mongo-c-driver && mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_INSTALL_PREFIX=/usr/local \
             -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF && \
    make -j$(nproc) && make install && ldconfig

# mongo-cxx-driver v3.6 — BSONCXX_POLY_USE_MNMLSTC=1 to avoid the
# boost variant linkage that v3.6 defaults to.
RUN cd /root && \
    git clone -b releases/v3.6 --depth 1 https://github.com/mongodb/mongo-cxx-driver.git && \
    cd mongo-cxx-driver && mkdir -p build && cd build && \
    cmake .. -DBSONCXX_POLY_USE_MNMLSTC=1 \
             -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_INSTALL_PREFIX=/usr/local \
             -DBUILD_TESTING=OFF \
             -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=TRUE && \
    make -j2 mongocxx_shared bsoncxx_shared && make install && ldconfig

# googletest 1.12.1 — offtarget's link target. Matches xpmile.
RUN cd /root && \
    git clone -b release-1.12.1 --depth 1 https://github.com/google/googletest.git && \
    cd googletest && mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local && \
    make -j$(nproc) && make install && ldconfig
DOCKEREOF
  SH "cd $REPO_HOST/build-lima && sudo podman build \
        -t localhost/pbx-cpp-builder:bootstrap -f Dockerfile.bootstrap ."
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

# ─── 6. Bring the 5-container stack up ────────────────────────────────────
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
