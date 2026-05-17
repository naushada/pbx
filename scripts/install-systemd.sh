#!/usr/bin/env bash
# Install (or reinstall) the systemd unit that brings up the on-prem
# PBX stack on host boot. Idempotent — safe to re-run after upgrades.
#
# Steps:
#   1. Sanity-check we're on a systemd Linux box (not macOS).
#   2. Copy the repo into INSTALL_DIR if it isn't already there.
#   3. Drop the unit into /etc/systemd/system/.
#   4. daemon-reload, enable, and (re)start.
#   5. Show status so the operator immediately sees red/green.
#
# Run as root:  sudo scripts/install-systemd.sh
# Override target dir: sudo INSTALL_DIR=/srv/onprem-pbx scripts/install-systemd.sh

set -euo pipefail

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

case "${1:-}" in
  -h|--help|help) usage 0 ;;
  "") ;;  # no arg — proceed
  *) echo "unknown arg: $1 (try help)" >&2; exit 2 ;;
esac

INSTALL_DIR="${INSTALL_DIR:-/opt/onprem-pbx}"
UNIT_SRC="systemd/onprem-pbx.service"
UNIT_DST="/etc/systemd/system/onprem-pbx.service"

# ── Preflight ───────────────────────────────────────────────────────
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "ERROR: this installer is for Linux on-prem hosts (you're on $(uname -s))." >&2
    echo "On macOS dev boxes, restart survival comes from podman-machine itself."   >&2
    exit 1
fi
if ! command -v systemctl >/dev/null; then
    echo "ERROR: systemctl not found — not a systemd host." >&2
    exit 1
fi
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: must run as root (try sudo)." >&2
    exit 1
fi
if ! command -v podman-compose >/dev/null; then
    echo "ERROR: podman-compose not found in PATH. Install it first:" >&2
    echo "  Ubuntu/Debian: apt install podman-compose"  >&2
    echo "  Fedora/Arch:   pip3 install podman-compose" >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ ! -f "$REPO_ROOT/$UNIT_SRC" ]]; then
    echo "ERROR: $UNIT_SRC not found relative to $REPO_ROOT." >&2
    exit 1
fi

# ── 1. Sync repo into INSTALL_DIR ──────────────────────────────────
# We rsync rather than symlink so the operator's /opt/onprem-pbx is a
# stable, complete tree even if the repo clone is later removed. If
# the operator IS running from /opt/onprem-pbx already, the rsync is a
# no-op.
if [[ "$REPO_ROOT" != "$INSTALL_DIR" ]]; then
    echo "Syncing repo → $INSTALL_DIR …"
    mkdir -p "$INSTALL_DIR"
    rsync -a --delete \
          --exclude='.git/' --exclude='ui/node_modules/' --exclude='ui/dist/' \
          "$REPO_ROOT"/ "$INSTALL_DIR"/
fi

# ── 2. .env sanity ─────────────────────────────────────────────────
if [[ ! -f "$INSTALL_DIR/.env" ]]; then
    cat <<EOF >&2

WARNING: $INSTALL_DIR/.env is missing. The service will fail to start
until you create it. Minimum required keys:

  CLOUD_HOST=pabx-5fbf3550f938.herokuapp.com
  AGENT_SOCIETY_ID=demo
  CERTS_DIR=$INSTALL_DIR/certs/agent-deployed

EOF
fi

# ── 3. Install the unit ────────────────────────────────────────────
echo "Installing $UNIT_DST …"
install -m 644 "$REPO_ROOT/$UNIT_SRC" "$UNIT_DST"

# Patch WorkingDirectory in-place if the operator chose a non-default
# install dir. The unit ships with /opt/onprem-pbx baked in.
if [[ "$INSTALL_DIR" != "/opt/onprem-pbx" ]]; then
    sed -i "s|^WorkingDirectory=/opt/onprem-pbx|WorkingDirectory=$INSTALL_DIR|" "$UNIT_DST"
    sed -i "s|^EnvironmentFile=-/opt/onprem-pbx/.env|EnvironmentFile=-$INSTALL_DIR/.env|" "$UNIT_DST"
fi

# ── 4. Enable + start ──────────────────────────────────────────────
systemctl daemon-reload
systemctl enable onprem-pbx.service
systemctl restart onprem-pbx.service

# ── 5. Status ──────────────────────────────────────────────────────
echo
systemctl --no-pager status onprem-pbx.service || true
echo
echo "Installed. The stack will now come up on every boot."
echo "Manual control:"
echo "  sudo systemctl start   onprem-pbx.service"
echo "  sudo systemctl stop    onprem-pbx.service"
echo "  sudo systemctl status  onprem-pbx.service"
echo "  sudo journalctl -u onprem-pbx.service -f"
