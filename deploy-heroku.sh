#!/usr/bin/env bash
# deploy-heroku.sh — build/push/release the pbx-cloud and/or pbx-ui
# images to Heroku.
#
# Mirrors xpmile's deploy-heroku.sh shape: wraps `podman` + the Heroku
# CLI so common operations are single commands.
#
#   ./deploy-heroku.sh login            # authenticate with registry.heroku.com
#
#   # Cloud (C++ webservice + SIP bridge) — defaults to HEROKU_APP=pabx
#   ./deploy-heroku.sh build            # build cloud image only
#   ./deploy-heroku.sh push             # push cloud image
#   ./deploy-heroku.sh release          # release the cloud image
#   ./deploy-heroku.sh deploy           # build + push + release (cloud)
#
#   # UI (nginx + Angular SPA) — defaults to HEROKU_APP_UI=onprem
#   ./deploy-heroku.sh build-ui
#   ./deploy-heroku.sh push-ui
#   ./deploy-heroku.sh release-ui
#   ./deploy-heroku.sh deploy-ui
#
#   ./deploy-heroku.sh deploy-all       # both sides, cloud first
#   ./deploy-heroku.sh logs             # tail cloud app logs
#   ./deploy-heroku.sh logs-ui          # tail UI app logs
#   ./deploy-heroku.sh open             # open the UI in a browser
#
# Environment overrides (set in shell or .env):
#   HEROKU_APP        cloud app name              (default: pabx)
#   HEROKU_APP_UI     UI app name                 (default: onprem)
#   PROCESS           Heroku process type         (default: web)

set -euo pipefail

HEROKU_APP="${HEROKU_APP:-pabx}"
HEROKU_APP_UI="${HEROKU_APP_UI:-onprem}"
PROCESS="${PROCESS:-web}"
COMPOSE_FILE="docker-compose.heroku.yml"

CLOUD_TAG="registry.heroku.com/${HEROKU_APP}/${PROCESS}"
UI_TAG="registry.heroku.com/${HEROKU_APP_UI}/${PROCESS}"

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

# ── cloud ─────────────────────────────────────────────────────────────

cmd_build() {
  log "building $CLOUD_TAG"
  HEROKU_APP="$HEROKU_APP" HEROKU_APP_CLOUD="$HEROKU_APP" \
    podman-compose -f "$COMPOSE_FILE" build pbx-cloud
}

cmd_push() {
  log "pushing $CLOUD_TAG"
  podman push --format=v2s2 "$CLOUD_TAG"
}

cmd_release() {
  log "releasing $PROCESS on $HEROKU_APP"
  heroku container:release "$PROCESS" --app "$HEROKU_APP"
}

cmd_deploy() { cmd_build; cmd_push; cmd_release; }

cmd_logs() { heroku logs --tail --app "$HEROKU_APP"; }
cmd_open() { heroku open --app "$HEROKU_APP_UI"; }

# ── UI ────────────────────────────────────────────────────────────────

cmd_build_ui() {
  log "building $UI_TAG"
  HEROKU_APP_UI="$HEROKU_APP_UI" \
    podman-compose -f "$COMPOSE_FILE" build pbx-ui
}

cmd_push_ui() {
  log "pushing $UI_TAG"
  podman push --format=v2s2 "$UI_TAG"
}

cmd_release_ui() {
  log "releasing $PROCESS on $HEROKU_APP_UI"
  heroku container:release "$PROCESS" --app "$HEROKU_APP_UI"
}

cmd_deploy_ui() { cmd_build_ui; cmd_push_ui; cmd_release_ui; }

cmd_logs_ui() { heroku logs --tail --app "$HEROKU_APP_UI"; }

# ── combined ──────────────────────────────────────────────────────────

cmd_deploy_all() { cmd_deploy; cmd_deploy_ui; }

case "${1:-deploy}" in
  login)        cmd_login         ;;
  build)        cmd_build         ;;
  push)         cmd_push          ;;
  release)      cmd_release       ;;
  deploy)       cmd_deploy        ;;
  build-ui)     cmd_build_ui      ;;
  push-ui)      cmd_push_ui       ;;
  release-ui)   cmd_release_ui    ;;
  deploy-ui)    cmd_deploy_ui     ;;
  deploy-all)   cmd_deploy_all    ;;
  logs)         cmd_logs          ;;
  logs-ui)      cmd_logs_ui       ;;
  open)         cmd_open          ;;
  *)            die "unknown command: $1 (try login | build | push | release | deploy | build-ui | push-ui | release-ui | deploy-ui | deploy-all | logs | logs-ui | open)" ;;
esac
