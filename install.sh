#!/usr/bin/env bash
#
# install.sh — one-shot, lay-operator-friendly install for the onprem-pbx
# stack on a society machine. After this finishes, all six containers
# are running AND the stack auto-restarts on reboot.
#
# Target OS: Ubuntu 22.04 or 24.04 (amd64 or arm64).
# Target operator: a building/society admin, not a sysadmin. Every
# prompt and error message reads in plain English with a "what to do
# next" line.
#
# What this script does, in order:
#
#   1. Preflight: confirms Ubuntu, systemd, ≥5 GB free disk,
#      internet reachability.
#   2. Installs podman + podman-compose + openssl + jq via apt
#      (skipped if already present).
#   3. Prompts for:
#        - society code (e.g. SUNSET)
#        - cloud hostname (defaults to the production deployment)
#        - path to the cert tarball your dev team gave you
#   4. Copies the repo to /opt/onprem-pbx (the canonical install dir).
#   5. Unpacks the cert tarball into certs/cloud-issued/.
#   6. Generates the Asterisk DTLS cert + coturn config via
#      scripts/setup-society.sh.
#   7. Writes /opt/onprem-pbx/.env so podman-compose has the knobs.
#   8. Pulls pre-built images from Docker Hub
#      (`docker.io/naushada/onprem-pbx-{agent,wsdbagent}:latest` + the
#      upstream mongo/asterisk/coturn/alpine images), then
#      `podman-compose up -d`. Total ~3-5 min depending on bandwidth.
#   9. Installs the systemd unit (scripts/install-systemd.sh) so the
#      stack restarts on boot.
#  10. Verifies: container status + a quick agent-log peek for the
#      inner-TLS handshake line. Prints a green summary or a red
#      "look at these logs" pointer.
#
# Usage:
#   sudo ./install.sh                    # interactive wizard
#   sudo ./install.sh help               # this message
#
# Non-interactive (CI / unattended re-runs):
#   sudo SOCIETY_CODE=SUNSET \
#        CLOUD_HOST=pabx-5fbf3550f938.herokuapp.com \
#        CERTS_TARBALL=/tmp/sunset-certs.tar.gz \
#        ./install.sh
#
# Re-running this script is safe — every step is idempotent. The
# images, the systemd unit, and the on-prem Mongo data all survive
# a re-run.

set -euo pipefail

# ── Constants ────────────────────────────────────────────────────────
INSTALL_DIR="${INSTALL_DIR:-/opt/onprem-pbx}"
COMPOSE_FILE="docker-compose.agent.yml"
DEFAULT_CLOUD_HOST="pabx-5fbf3550f938.herokuapp.com"
DEFAULT_MONGO_DB="pabx"
DEFAULT_TURN_PORT="3478"
HEALTH_WAIT_SECS="${HEALTH_WAIT_SECS:-120}"
MIN_DISK_GB=5

# Sentinels — so a re-run skips work that already finished.
SENT_APT=/var/lib/onprem-pbx-install/apt-installed
SENT_BOOTSTRAP=/var/lib/onprem-pbx-install/bootstrap-done

# ── Colour helpers ───────────────────────────────────────────────────
if [[ -t 1 ]]; then
  C_BLUE=$'\033[1;34m'; C_GREEN=$'\033[1;32m'; C_YELLOW=$'\033[1;33m'
  C_RED=$'\033[1;31m';  C_BOLD=$'\033[1m';     C_RESET=$'\033[0m'
else
  C_BLUE=; C_GREEN=; C_YELLOW=; C_RED=; C_BOLD=; C_RESET=
fi

step()  { printf '\n%s[install] %s%s\n' "$C_BLUE"   "$*" "$C_RESET"; }
ok()    { printf '%s  ✓ %s%s\n'         "$C_GREEN"  "$*" "$C_RESET"; }
warn()  { printf '%s  ! %s%s\n'         "$C_YELLOW" "$*" "$C_RESET" >&2; }
die()   { printf '%s  ✗ %s%s\n'         "$C_RED"    "$*" "$C_RESET" >&2; exit 1; }
ask()   { printf '%s%s%s ' "$C_BOLD" "$*" "$C_RESET"; }

# ── help ─────────────────────────────────────────────────────────────
usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

case "${1:-}" in
  -h|--help|help) usage 0 ;;
  "") ;;  # proceed
  *) die "unknown arg: $1 (try help)" ;;
esac

# ── 1. Preflight ─────────────────────────────────────────────────────
step "Checking your machine"

[[ $EUID -eq 0 ]] || die "Run me with sudo. Try: sudo ./install.sh"
ok "Running as root"

[[ "$(uname -s)" == "Linux" ]] \
  || die "This installer only works on Linux. You're on $(uname -s)."
ok "Operating system: Linux"

if ! command -v systemctl >/dev/null 2>&1; then
  die "systemd is required (the stack auto-restarts on boot through it). Your machine doesn't have it."
fi
ok "systemd available (containers will auto-restart on reboot)"

if [[ -r /etc/os-release ]]; then
  . /etc/os-release
  case "${ID:-}:${VERSION_ID:-}" in
    ubuntu:22.04|ubuntu:24.04) ok "Ubuntu ${VERSION_ID} (supported)" ;;
    ubuntu:*) warn "Ubuntu ${VERSION_ID:-?} is not in the tested set (22.04 / 24.04). Continuing anyway." ;;
    *) warn "Distribution '${ID:-?}' is not Ubuntu. The installer may still work but isn't tested here." ;;
  esac
else
  warn "Couldn't read /etc/os-release; assuming a Debian-family system."
fi

free_gb=$(df --output=avail / | tail -1 | awk '{print int($1/1024/1024)}')
if [[ "$free_gb" -lt "$MIN_DISK_GB" ]]; then
  die "Only ${free_gb} GB free on / — install needs at least ${MIN_DISK_GB} GB (container images + Mongo data)."
fi
ok "Disk space: ${free_gb} GB free on / (need ${MIN_DISK_GB})"

if ! getent hosts "${DEFAULT_CLOUD_HOST}" >/dev/null 2>&1; then
  warn "Couldn't resolve ${DEFAULT_CLOUD_HOST}. If you're behind a proxy or your DNS is broken, the agent won't reach the cloud after install."
else
  ok "Internet reachable (resolved ${DEFAULT_CLOUD_HOST})"
fi

# ── 2. Install OS dependencies ──────────────────────────────────────
step "Installing dependencies (podman, podman-compose, jq, openssl)"

mkdir -p "$(dirname "$SENT_APT")"
if [[ -f "$SENT_APT" ]]; then
  ok "Already installed on a previous run — skipping"
else
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    podman podman-compose openssl jq curl ca-certificates rsync \
    || die "apt-get install failed. Check your internet connection and try again."
  # Stock Ubuntu podman ships with `unqualified-search-registries` empty;
  # set it so bare `mongo:7` / `coturn/coturn:4.6` short-name pulls
  # resolve to docker.io. Same setting lima.sh writes inside its VM.
  mkdir -p /etc/containers/registries.conf.d
  echo 'unqualified-search-registries = ["docker.io"]' \
    > /etc/containers/registries.conf.d/00-docker.conf
  touch "$SENT_APT"
  ok "Installed: podman $(podman --version | awk '{print $3}'), podman-compose, openssl, jq"
fi

# ── 3. Collect inputs ───────────────────────────────────────────────
step "Configuring your society"

# Prefer env-set values over prompts so unattended re-runs just work.
if [[ -z "${SOCIETY_CODE:-}" ]]; then
  ask "Society code (a short label, e.g. SUNSET):"
  read -r SOCIETY_CODE
fi
[[ -n "$SOCIETY_CODE" ]] || die "Society code cannot be empty."

if [[ -z "${CLOUD_HOST:-}" ]]; then
  ask "Cloud hostname [press Enter for ${DEFAULT_CLOUD_HOST}]:"
  read -r CLOUD_HOST
  CLOUD_HOST="${CLOUD_HOST:-$DEFAULT_CLOUD_HOST}"
fi

if [[ -z "${CERTS_TARBALL:-}" ]]; then
  ask "Path to the cert tarball your dev team gave you (e.g. /tmp/${SOCIETY_CODE}-certs.tar.gz):"
  read -r CERTS_TARBALL
fi
[[ -f "$CERTS_TARBALL" ]] \
  || die "Can't find the cert tarball at: $CERTS_TARBALL"

ok "Society code  = $SOCIETY_CODE"
ok "Cloud host    = $CLOUD_HOST"
ok "Cert tarball  = $CERTS_TARBALL"

# ── 4. Stage repo into INSTALL_DIR ──────────────────────────────────
step "Installing onprem-pbx into ${INSTALL_DIR}"

REPO_ROOT="$(cd "$(dirname "$0")" && pwd -P)"
if [[ ! -f "$REPO_ROOT/$COMPOSE_FILE" ]]; then
  die "$COMPOSE_FILE not found next to install.sh — are you running from the repo root?"
fi

if [[ "$REPO_ROOT" != "$INSTALL_DIR" ]]; then
  mkdir -p "$INSTALL_DIR"
  rsync -a --delete \
        --exclude='.git/' --exclude='ui/node_modules/' --exclude='ui/dist/' \
        --exclude='.claude/' \
        "$REPO_ROOT"/ "$INSTALL_DIR"/
  ok "Copied repo → $INSTALL_DIR"
else
  ok "Already running from $INSTALL_DIR — skipping copy"
fi

cd "$INSTALL_DIR"

# ── 5. Unpack cert tarball ──────────────────────────────────────────
step "Unpacking certs"

mkdir -p "$INSTALL_DIR/certs/cloud-issued"
tar -xzf "$CERTS_TARBALL" -C "$INSTALL_DIR/certs/cloud-issued/" \
  || die "Couldn't extract $CERTS_TARBALL. Is it a .tar.gz from your dev team?"

# Sanity: the dev team's tarball should contain at least these two subdirs.
for d in innertls sip-agent; do
  if [[ ! -d "$INSTALL_DIR/certs/cloud-issued/$d" ]]; then
    warn "Cert tarball doesn't contain certs/cloud-issued/$d — pbx-wsdbagent or pbx-agent will fail to handshake."
  fi
done
ok "Certs unpacked into certs/cloud-issued/"

# ── 6. Per-society config (Asterisk DTLS + coturn) ──────────────────
step "Generating per-society config (Asterisk DTLS + coturn)"
SOCIETY_CODE="$SOCIETY_CODE" ./scripts/setup-society.sh "$SOCIETY_CODE" \
  || die "setup-society.sh failed. See output above."
ok "Society config ready"

# ── 7. Write .env ───────────────────────────────────────────────────
step "Writing .env"
cat > "$INSTALL_DIR/.env" <<ENV
# Generated by install.sh on $(date '+%Y-%m-%d %H:%M:%S')
# Re-run install.sh to regenerate.
CLOUD_HOST=$CLOUD_HOST
AGENT_SOCIETY_ID=$SOCIETY_CODE
CERTS_DIR=$INSTALL_DIR/certs/cloud-issued/sip-agent
INNERTLS_DIR=$INSTALL_DIR/certs/cloud-issued/innertls
MONGO_DB_NAME=$DEFAULT_MONGO_DB
TURN_PUBLIC_PORT=$DEFAULT_TURN_PORT
ENV
chmod 600 "$INSTALL_DIR/.env"
ok "Wrote $INSTALL_DIR/.env"

# ── 8. Bring the stack up ──────────────────────────────────────────
step "Pulling pre-built images from Docker Hub (~3-5 minutes)"
# Pulls docker.io/naushada/onprem-pbx-{agent,wsdbagent}:latest +
# upstream mongo/asterisk/coturn/alpine. CI publishes the two on-prem
# images via .github/workflows/publish-images.yml on every push to
# main. If the pull fails (network outage, registry maintenance), the
# next `up -d` won't recreate the containers — operator can retry.
podman-compose -f "$COMPOSE_FILE" pull \
  || die "podman-compose pull failed. Check internet, then retry: \`sudo ./install.sh\`."
ok "Images pulled"

step "Starting containers"
podman-compose -f "$COMPOSE_FILE" up -d \
  || die "podman-compose up failed. Run \`podman-compose -f $COMPOSE_FILE logs\` to see why."
ok "Containers started"

# ── 9. Wait for boot + spot-check ───────────────────────────────────
step "Waiting ${HEALTH_WAIT_SECS}s for Mongo + cloud-tunnel handshake"
sleep "$HEALTH_WAIT_SECS"

step "Container status"
podman ps --filter name=pbx- --format 'table {{.Names}}\t{{.Status}}'

agent_ok=0
if podman logs pbx-agent 2>&1 | grep -q "inner-TLS handshake established"; then
  agent_ok=1
  ok "pbx-agent connected to the cloud (inner-TLS handshake established)"
else
  warn "pbx-agent doesn't yet show an inner-TLS handshake. Check: podman logs pbx-agent | tail -50"
fi

wsdb_ok=0
if podman logs pbx-wsdbagent 2>&1 | grep -q "session started"; then
  wsdb_ok=1
  ok "pbx-wsdbagent session is live (cloud can read Mongo)"
else
  warn "pbx-wsdbagent doesn't yet show 'session started'. Check: podman logs pbx-wsdbagent | tail -50"
fi

# ── 10. Make it survive reboot ──────────────────────────────────────
step "Installing systemd unit (so the stack auto-starts on reboot)"
INSTALL_DIR="$INSTALL_DIR" ./scripts/install-systemd.sh \
  || die "install-systemd.sh failed. See output above."

# ── Summary ─────────────────────────────────────────────────────────
printf '\n%s' "$C_BOLD"
printf '%s╔══════════════════════════════════════════════════════════╗%s\n' "$C_BLUE" "$C_RESET"
printf '%s║%s            %sonprem-pbx is installed%s                      %s║%s\n' "$C_BLUE" "$C_RESET" "$C_GREEN" "$C_RESET" "$C_BLUE" "$C_RESET"
printf '%s╚══════════════════════════════════════════════════════════╝%s\n' "$C_BLUE" "$C_RESET"

cat <<EOF

  Society code:   $SOCIETY_CODE
  Cloud target:   $CLOUD_HOST
  Install dir:    $INSTALL_DIR
  Reboot survival: ✓ (systemd unit onprem-pbx.service)

  ${C_BOLD}Check status anytime:${C_RESET}
    sudo systemctl status onprem-pbx
    sudo podman ps --filter name=pbx-

  ${C_BOLD}Watch live logs:${C_RESET}
    sudo podman logs -f pbx-agent
    sudo podman logs -f pbx-wsdbagent
    sudo journalctl -u onprem-pbx -f

  ${C_BOLD}Stop / start manually:${C_RESET}
    sudo systemctl stop  onprem-pbx
    sudo systemctl start onprem-pbx

EOF

if [[ "$agent_ok" -eq 1 && "$wsdb_ok" -eq 1 ]]; then
  printf '%s  Everything is green. You can close this terminal.%s\n\n' "$C_GREEN" "$C_RESET"
  exit 0
else
  printf '%s  At least one container hasn'\''t finished its handshake yet.%s\n' "$C_YELLOW" "$C_RESET"
  printf '%s  Wait another minute then run:%s\n\n' "$C_YELLOW" "$C_RESET"
  printf '    sudo podman logs --tail 50 pbx-agent\n'
  printf '    sudo podman logs --tail 50 pbx-wsdbagent\n\n'
  printf '  If you still see errors after that, capture both log dumps and\n'
  printf '  send them to your dev team.\n\n'
  exit 0
fi
