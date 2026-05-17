#!/usr/bin/env bash
#
# prune-images.sh — reclaim podman image disk on the host AND inside
# the Lima VM (if it's running).
#
# Why this exists: every `podman build` of pbx-cloud / pbx-agent /
# pbx-wsdbagent / pbx-test layers ~3 GB of intermediate images. After a
# few iterations the podman-machine VM (capped at 100 GB on macOS)
# wedges with "no space left on device" and every subsequent build
# fails opaquely. The Lima VM (30 GB by default) hits the same wall
# faster.
#
# Default mode prunes only dangling (untagged) images — safe; keeps
# every named image. Pass --deep to also drop stopped containers,
# unused networks/volumes, and build cache.
#
# Usage:
#   scripts/prune-images.sh           # dangling images only (host + VM)
#   scripts/prune-images.sh --deep    # also stopped containers, build cache, unused volumes
#   scripts/prune-images.sh --host    # host podman only (skip VM)
#   scripts/prune-images.sh --vm      # Lima VM only (skip host)

set -euo pipefail

VM=onprem-pbx-test
DEEP=0
SCOPE=both

for arg in "$@"; do
  case "$arg" in
    --deep) DEEP=1 ;;
    --host) SCOPE=host ;;
    --vm)   SCOPE=vm ;;
    -h|--help|help)
      sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *) echo "unknown arg: $arg (try --help)" >&2; exit 2 ;;
  esac
done

log()  { printf '\033[1;34m[prune]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[prune]\033[0m %s\n' "$*" >&2; }

# Print before/after disk usage so the operator can see what landed.
df_line() {
  local where=$1
  shift
  printf '  %-12s ' "$where:"
  "$@" 2>/dev/null | awk 'NR>1 {used+=$3} END {printf "%.1f GB used by podman images\n", used/1024/1024/1024}'
}

prune_one() {
  local label=$1
  shift
  log "$label — pruning dangling images"
  "$@" image prune -f

  if [ "$DEEP" = "1" ]; then
    log "$label — pruning stopped containers"
    "$@" container prune -f
    log "$label — pruning unused volumes"
    "$@" volume prune -f
    log "$label — pruning unused networks"
    "$@" network prune -f
    log "$label — pruning build cache"
    # `podman build` doesn't expose a top-level builder-cache prune; the
    # buildah intermediate images get caught by `image prune -af` (drops
    # unused TAGGED images too), so opt in only on --deep.
    "$@" image prune -af
  fi
}

# ── host podman ─────────────────────────────────────────────────────
if [ "$SCOPE" = "both" ] || [ "$SCOPE" = "host" ]; then
  if command -v podman >/dev/null 2>&1; then
    prune_one "host podman" podman
  else
    warn "podman not found on host — skipping"
  fi
fi

# ── Lima VM podman ──────────────────────────────────────────────────
if [ "$SCOPE" = "both" ] || [ "$SCOPE" = "vm" ]; then
  if command -v limactl >/dev/null 2>&1 \
     && limactl list -q 2>/dev/null | grep -qx "$VM"; then
    prune_one "Lima VM ($VM)" limactl shell "$VM" -- sudo podman
  else
    log "Lima VM '$VM' not running — skipping (use \`lima start\` first)"
  fi
fi

log "done"
