#!/usr/bin/env bash
#
# run-admin-ui.sh — start the Vaadin admin UI against a cloud backend.
#
# Runs everything inside a `maven:3.9-eclipse-temurin-17` podman
# container — Java + Maven do NOT need to be installed on the host.
# Mounts onprem/ in and a named volume for the ~/.m2 cache so
# subsequent runs skip dependency re-downloads.
#
# By default points at the live Heroku deployment
# (`pabx-5fbf3550f938.herokuapp.com`). Override with --backend-url or
# `BACKEND_URL=…` for staging / a local pbx-cloud / a forwarded port.
#
# After this is up:
#   open http://localhost:${PORT}/login
#   societyCode = <as bootstrapped via scripts/bootstrap-society.sh>
#   flatNumber  = ADMIN
#   password    = <whatever you set during bootstrap>
#
# Usage:
#   scripts/run-admin-ui.sh                            # → Heroku (default)
#   scripts/run-admin-ui.sh --backend-url http://localhost:8080
#   PORT=9090 scripts/run-admin-ui.sh
#   scripts/run-admin-ui.sh --offline                  # mvn -o, skip update
#   scripts/run-admin-ui.sh --pull                     # podman pull image first
#
# Requires: podman (no Java/Maven on host needed).

set -euo pipefail

DEFAULT_BACKEND="https://pabx-5fbf3550f938.herokuapp.com"
MAVEN_IMAGE="docker.io/library/maven:3.9-eclipse-temurin-17"
M2_VOLUME="pbx-admin-ui-m2"
CONTAINER_NAME="pbx-admin-ui"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
ONPREM_DIR="${REPO_ROOT}/onprem"

BACKEND_URL="${BACKEND_URL:-$DEFAULT_BACKEND}"
PORT="${PORT:-8081}"
OFFLINE=0
PULL=0

log()  { printf '\033[1;34m[admin-ui]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[admin-ui]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --backend-url) BACKEND_URL=$2; shift 2 ;;
    --port)        PORT=$2;        shift 2 ;;
    --offline)     OFFLINE=1;      shift ;;
    --pull)        PULL=1;         shift ;;
    -h|--help|help) usage 0 ;;
    *) die "unknown arg: $1 (try --help)" ;;
  esac
done

[ -f "$ONPREM_DIR/pom.xml" ] \
  || die "onprem/pom.xml not found at $ONPREM_DIR — are you in the onprem-pbx repo?"
command -v podman >/dev/null 2>&1 \
  || die "podman not found — install podman + run \`podman machine init && podman machine start\`"

# Pre-pull on request OR if the image is missing locally so the
# `podman run` below doesn't surprise the operator with a long pull.
if [ "$PULL" = "1" ] || ! podman image exists "$MAVEN_IMAGE" 2>/dev/null; then
  log "pulling $MAVEN_IMAGE (one-time, ~700 MB)…"
  podman pull "$MAVEN_IMAGE"
fi

# Named volume keeps the ~/.m2 cache across runs — first run downloads
# ~150 MB of deps, subsequent runs reuse them. `podman volume create`
# is idempotent: --ignore swallows "already exists".
podman volume create --ignore "$M2_VOLUME" >/dev/null

# If a previous container is still around (e.g. user Ctrl-C'd mid-run
# without --rm), drop it so the name is free.
podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true

log "backend = $BACKEND_URL"
log "port    = $PORT  (container :${PORT} → host :${PORT})"
log "image   = $MAVEN_IMAGE"
log "  override either via env (BACKEND_URL / PORT) or --backend-url / --port"
log "  log in as societyCode=<bootstrap>, flatNumber=ADMIN, password=<bootstrap>"
log "  Ctrl-C stops the server (the container is removed on exit)"

mvn_args=(spring-boot:run
          "-Dspring-boot.run.arguments=--backend.url=$BACKEND_URL --server.port=$PORT")
[ "$OFFLINE" = "1" ] && mvn_args=(-o "${mvn_args[@]}")

# --rm removes the container on exit; -it keeps a TTY so Ctrl-C reaches
# Maven and Spring Boot. The host port matches the container port so
# the operator's `http://localhost:${PORT}` works either way.
exec podman run --rm -it \
  --name "$CONTAINER_NAME" \
  -p "${PORT}:${PORT}" \
  -v "${ONPREM_DIR}:/workspace" \
  -v "${M2_VOLUME}:/root/.m2" \
  -w /workspace \
  "$MAVEN_IMAGE" \
  mvn "${mvn_args[@]}"
