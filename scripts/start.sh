#!/usr/bin/env bash
# scripts/start.sh — one-shot unattended dry test for pbx-agent.
#
# Provisions a Lima VM (Apple Virtualization framework, native arm64
# on Apple Silicon — no QEMU), follows xpmile/docker/Dockerfile's
# build recipe verbatim, builds pbx-agent, then runs it against the
# deployed Heroku cloud and captures logs + any coredump.
#
# Why a separate VM instead of `podman build`: podman-on-macOS
# emulates linux/amd64 via QEMU, which segfaults on signals during
# ACE+OpenSSL multithreaded I/O (observed during PR #25's dry test).
# Lima vz gives us a real Linux kernel with no emulation.
#
# Why following xpmile's Dockerfile recipe: that's what
# `pbx-cpp-builder:bootstrap` was built from. Different ACE/mongocxx
# build flags break the link step (saw this with the wrong
# CMAKE_INSTALL_PREFIX on mongocxx and missing `ssl=1` on ACE).
#
# Idempotent. Re-run skips already-done steps via sentinel files.
# Tear down: `limactl stop -f onprem-pbx-test && limactl delete -f onprem-pbx-test`.

set -euo pipefail

VM=onprem-pbx-test
REPO_HOST="$(cd "$(dirname "$0")/.." && pwd -P)"
HEROKU_HOST="${HEROKU_HOST:-pabx-5fbf3550f938.herokuapp.com}"
HEROKU_PORT="${HEROKU_PORT:-443}"
SOCIETY_ID="${SOCIETY_ID:-demo-society}"
RUN_BUDGET_SECS="${RUN_BUDGET_SECS:-90}"
LOG=/tmp/start.sh.log

# Shadow stdout/stderr so step output lands on screen AND in the
# log file for post-mortem.
exec > >(tee "$LOG") 2>&1

step() { printf '\n\033[1;34m[start.sh] %s\033[0m\n' "$*"; }
SH()   { limactl shell "$VM" -- bash -c "$1"; }

# Sentinel files (inside the VM) for skip-already-done.
SENT_APT=/var/lib/dry-test-apt
SENT_ACE=/var/lib/dry-test-ace
SENT_MCXX=/var/lib/dry-test-mongocxx

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

# ─── 2. apt deps — matches xpmile/docker/Dockerfile lines 9–20 ────────────
step "apt deps (xpmile recipe)"
SH "
  set -e
  if [ -f $SENT_APT ]; then exit 0; fi
  sudo apt-get update -qq
  sudo apt-get install -y -qq \
    ca-certificates \
    cmake \
    build-essential \
    libboost-all-dev \
    libssl-dev \
    libzstd-dev \
    libsasl2-dev \
    zlib1g-dev \
    wget \
    git \
    pkg-config \
    gdb
  sudo touch $SENT_APT
"

# ─── 3. ACE/TAO 7.0.0 — matches xpmile/docker/Dockerfile lines 23–34 ──────
#
# Critical bits: ssl=1 on the `make install` line (NOT a separate
# build step), and the SSL_ROOT=/usr/include/openssl pointer. Without
# ssl=1 the link step fails with `cannot find -lACE_SSL` — exactly
# what tripped us up earlier.
step "ACE/TAO 7.0.0 (xpmile recipe)"
SH "
  set -e
  if [ -f $SENT_ACE ]; then echo 'already built'; exit 0; fi
  cd /root 2>/dev/null || cd ~
  ACE_SRC=\$PWD/ACE_wrappers
  ACE_PREFIX=/usr/local/ACE_TAO-7.0.0
  sudo wget -q https://github.com/DOCGroup/ACE_TAO/releases/download/ACE%2BTAO-7_0_0/ACE+TAO-7.0.0.tar.gz
  sudo tar -xzf ACE+TAO-7.0.0.tar.gz
  sudo rm ACE+TAO-7.0.0.tar.gz
  echo '#include \"ace/config-linux.h\"' | sudo tee \$ACE_SRC/ace/config.h >/dev/null
  echo \"include \\\$(ACE_SRC)/include/makeinclude/platform_linux.GNU\" | \
    sudo tee \$ACE_SRC/include/makeinclude/platform_macros.GNU >/dev/null
  cd \$ACE_SRC
  sudo make install ssl=1 \
       INSTALL_PREFIX=\$ACE_PREFIX \
       ACE_ROOT=\$ACE_SRC \
       SSL_ROOT=/usr/include/openssl \
       -j\$(nproc) > /tmp/ace-build.log 2>&1
  echo /usr/local/ACE_TAO-7.0.0/lib | sudo tee /etc/ld.so.conf.d/ace.conf >/dev/null
  sudo ldconfig
  sudo touch $SENT_ACE
"

# ─── 4. mongo-c-driver + mongo-cxx-driver — matches xpmile lines 37–60 ───
step "mongo-c-driver 1.19.1 + mongo-cxx-driver v3.6 (xpmile recipe)"
SH "
  set -e
  if [ -f $SENT_MCXX ]; then echo 'already built'; exit 0; fi
  cd /root 2>/dev/null || cd ~

  sudo git clone -b 1.19.1 --depth 1 https://github.com/mongodb/mongo-c-driver.git mongo-c-driver
  cd mongo-c-driver && sudo mkdir -p build && cd build
  sudo cmake .. -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_INSTALL_PREFIX=/usr/local \
                -DENABLE_TESTS=OFF \
                -DENABLE_EXAMPLES=OFF >/dev/null
  sudo make -j\$(nproc) > /tmp/mongoc-build.log 2>&1
  sudo make install
  sudo ldconfig

  cd /root 2>/dev/null || cd ~
  sudo git clone -b releases/v3.6 --depth 1 https://github.com/mongodb/mongo-cxx-driver.git mongo-cxx-driver
  cd mongo-cxx-driver && sudo mkdir -p build && cd build
  sudo cmake .. \
       -DBSONCXX_POLY_USE_MNMLSTC=1 \
       -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_INSTALL_PREFIX=/usr/local \
       -DBUILD_TESTING=OFF \
       -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=TRUE >/dev/null
  sudo make -j2 mongocxx_shared bsoncxx_shared > /tmp/mongocxx-build.log 2>&1
  sudo make install
  sudo ldconfig

  sudo touch $SENT_MCXX
"

# ─── 5. Build pbx-agent ────────────────────────────────────────────────────
step "build pbx-agent (native arm64)"
SH "
  set -e
  cd $REPO_HOST
  mkdir -p build-lima && cd build-lima
  cmake -DBUILD_TESTS=OFF -DBUILD_BINARIES=ON .. > /tmp/cmake.log 2>&1
  make -j\$(nproc) pbx-agent > /tmp/make.log 2>&1
  ls -la pbx-agent
"

# ─── 6. Run against Heroku ────────────────────────────────────────────────
#
# Mongo + Asterisk aren't installed in the VM — we're testing the
# cloud-tunnel path. The 2 s serverSelectionTimeoutMS on the Mongo
# URI keeps bootstrap from blocking 30 s on a dead mongod so the
# reactor enters event-loop fast enough to observe the connect-
# /handshake/AGENT_HELLO sequence within RUN_BUDGET_SECS.
step "run pbx-agent against $HEROKU_HOST:$HEROKU_PORT for ${RUN_BUDGET_SECS}s"
SH "
  set -e
  ulimit -c unlimited
  cd $REPO_HOST
  timeout $RUN_BUDGET_SECS ./build-lima/pbx-agent \
    --cloud-host       $HEROKU_HOST \
    --cloud-port       $HEROKU_PORT \
    --tls-cert         ./certs/agent-deployed/agent.crt \
    --tls-key          ./certs/agent-deployed/agent.key \
    --tls-ca           ./certs/agent-deployed/cloud-ca.pem \
    --inner-tls-cert   ./certs/agent-deployed/agent.crt \
    --inner-tls-key    ./certs/agent-deployed/agent.key \
    --inner-tls-ca     ./certs/agent-deployed/cloud-ca.pem \
    --mongo-uri        'mongodb://127.0.0.1:27017/pabx?serverSelectionTimeoutMS=2000' \
    --society-id       $SOCIETY_ID \
    --asterisk-host    127.0.0.1 \
    --asterisk-port    8088 2>&1 || true
"

# ─── 7. Coredump (if any) ─────────────────────────────────────────────────
step "coredump check"
SH '
  ls -t /var/lib/apport/coredump/core.*pbx-agent* 2>/dev/null | head -1 | while read core; do
    echo "found: $core"
    sudo gdb -batch -ex "bt 20" '"$REPO_HOST"'/build-lima/pbx-agent "$core" 2>&1 | tail -25
  done
'

step "done — full log at $LOG"
