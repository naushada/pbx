#!/usr/bin/env bash
# deploy-heroku.sh — build/push/release pbx-cloud to Heroku.
#
# Mirrors xpmile's deploy-heroku.sh shape: wraps `podman` + the Heroku
# CLI so the common operations are single commands.
#
#   ./deploy-heroku.sh login          # authenticate with registry.heroku.com
#   ./deploy-heroku.sh deploy         # build + push + release  (typical)
#   ./deploy-heroku.sh build          # build only
#   ./deploy-heroku.sh push           # push previously built image
#   ./deploy-heroku.sh release        # release (activate) the pushed image
#   ./deploy-heroku.sh logs           # tail live Heroku logs
#   ./deploy-heroku.sh open           # open the app in the browser
#
# Default app is `onprem-pbx`. Override with HEROKU_APP=<name>.

set -euo pipefail

HEROKU_APP="${HEROKU_APP:-onprem-pbx}"
PROCESS="${PROCESS:-web}"
COMPOSE_FILE="docker-compose.heroku.yml"
IMAGE_TAG="registry.heroku.com/${HEROKU_APP}/${PROCESS}"

log() { printf '\033[1;34m[deploy-heroku]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[deploy-heroku] %s\033[0m\n' "$*" >&2; exit 1; }

require() {
  command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"
}

require podman
require heroku

cmd_login() {
  log "authenticating with registry.heroku.com…"
  heroku auth:token | podman login --username=_ --password-stdin registry.heroku.com
}

cmd_build() {
  log "building $IMAGE_TAG"
  HEROKU_APP="$HEROKU_APP" podman-compose -f "$COMPOSE_FILE" build pbx-cloud
}

cmd_push() {
  log "pushing $IMAGE_TAG"
  podman push --format=v2s2 "$IMAGE_TAG"
}

cmd_release() {
  log "releasing $PROCESS on app $HEROKU_APP"
  heroku container:release "$PROCESS" --app "$HEROKU_APP"
}

cmd_deploy() {
  cmd_build
  cmd_push
  cmd_release
}

cmd_logs() {
  heroku logs --tail --app "$HEROKU_APP"
}

cmd_open() {
  heroku open --app "$HEROKU_APP"
}

case "${1:-deploy}" in
  login)   cmd_login   ;;
  build)   cmd_build   ;;
  push)    cmd_push    ;;
  release) cmd_release ;;
  deploy)  cmd_deploy  ;;
  logs)    cmd_logs    ;;
  open)    cmd_open    ;;
  *)       die "unknown command: $1 (try login|build|push|release|deploy|logs|open)" ;;
esac
