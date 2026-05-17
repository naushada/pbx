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
#   ./deploy-heroku.sh build-bootstrap  # build the amd64 toolchain image
#                                       # (auto-invoked by `build` when missing;
#                                       # run standalone to pre-warm the cache,
#                                       # e.g. after `podman image prune`)
#   ./deploy-heroku.sh build            # build cloud image only
#                                       # (auto-invokes build-bootstrap if needed)
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
BOOTSTRAP_TAG="localhost/pbx-cpp-builder:bootstrap"
BOOTSTRAP_DOCKERFILE="docker/Dockerfile.bootstrap"

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

# Ensure `localhost/pbx-cpp-builder:bootstrap` exists locally as amd64.
# Dockerfile.cloud's first stage starts with
#   FROM localhost/pbx-cpp-builder:bootstrap
# — if that image is missing OR wrong arch, podman tries to pull from a
# "localhost" registry and fails with "connection refused". Catching it
# here turns the silent-30-min-into-a-cryptic-error into a one-shot
# bootstrap on the first deploy after a prune.
cmd_build_bootstrap() {
  local need_build=0 reason=""
  local existing_arch
  existing_arch=$(podman image inspect "$BOOTSTRAP_TAG" \
                    --format '{{.Architecture}}' 2>/dev/null || echo "")
  if [ -z "$existing_arch" ]; then
    need_build=1; reason="not present on host"
  elif [ "$existing_arch" != "amd64" ]; then
    need_build=1; reason="host bootstrap is $existing_arch but Heroku needs amd64"
  fi

  if [ "$need_build" = "0" ]; then
    log "$BOOTSTRAP_TAG already on host (amd64) — skipping bootstrap build"
    return 0
  fi

  log "$BOOTSTRAP_TAG missing or wrong arch ($reason) — building (~30 min cold, via QEMU)"
  [ -f "$BOOTSTRAP_DOCKERFILE" ] \
    || die "$BOOTSTRAP_DOCKERFILE not found — are you in the repo root?"
  podman build \
    --platform linux/amd64 \
    -f "$BOOTSTRAP_DOCKERFILE" \
    -t "$BOOTSTRAP_TAG" \
    .
  log "$BOOTSTRAP_TAG built — cached for every subsequent cmd_build"
}

# ── cloud ─────────────────────────────────────────────────────────────

cmd_build() {
  # Bootstrap first — Dockerfile.cloud's FROM line depends on it.
  # No-op when already present at the right arch.
  cmd_build_bootstrap

  log "building $CLOUD_TAG (--platform linux/amd64; Heroku Common Runtime is amd64-only)"
  # Direct `podman build` rather than `podman-compose build` — compose
  # tries to pull `localhost/pbx-cpp-builder:bootstrap` from a registry
  # (no `--pull-policy missing` knob in compose), which always fails
  # because that image only exists locally. `podman build` is happy to
  # use the local image straight off.
  podman build \
    --platform linux/amd64 \
    -f docker/Dockerfile.cloud \
    -t "$CLOUD_TAG" \
    .
}

cmd_push() {
  log "pushing $CLOUD_TAG"
  podman push --format=v2s2 "$CLOUD_TAG"
}

cmd_release() {
  log "releasing $PROCESS on $HEROKU_APP"
  heroku container:release "$PROCESS" --app "$HEROKU_APP"
}

# Extract the two on-prem client cert families from the just-built
# image — one for pbx-wsdbagent (`/ws/db`) and one for pbx-agent
# (`/agent` SIP tunnel). Dockerfile.cloud mints both signed by the
# same fresh per-build CA; this step pulls each into its compose
# mount target so the next container start picks up the matching
# pair.
#
# Without this, the cloud would have a fresh CA in the image but the
# on-prem peers would still present the previous build's (now
# untrusted) client certs — exactly the
# `tls_process_client_certificate verify failed` we hit on 2026-05-16
# (wsdbagent against v22, pbx-agent against v23).
cmd_extract_agent_certs() {
  local cid
  cid=$(podman create "$CLOUD_TAG") || die "podman create failed — was the image built?"

  local wsdest="$(pwd)/certs/cloud-issued/innertls"
  local sipdest="$(pwd)/certs/cloud-issued/sip-agent"
  mkdir -p "$wsdest" "$sipdest"

  log "extracting cert families from $CLOUD_TAG"
  podman cp "$cid:/opt/pbx-cloud/agent-certs/."     "$wsdest/"  || { podman rm -f "$cid" >/dev/null 2>&1; die "podman cp /opt/pbx-cloud/agent-certs/ failed"; }
  podman cp "$cid:/opt/pbx-cloud/sip-agent-certs/." "$sipdest/" || { podman rm -f "$cid" >/dev/null 2>&1; die "podman cp /opt/pbx-cloud/sip-agent-certs/ failed"; }
  podman rm -f "$cid" >/dev/null

  chmod 600 "$wsdest"/client.key 2>/dev/null || true
  chmod 600 "$sipdest"/agent.key 2>/dev/null || true

  log "  wsdbagent → $wsdest   ($(ls "$wsdest"  | grep -v gitkeep | tr '\n' ' '))"
  log "  pbx-agent → $sipdest  ($(ls "$sipdest" | grep -v gitkeep | tr '\n' ' '))"
  log "  both pickups happen on next container restart / compose-up"
}

cmd_deploy() {
  cmd_build
  cmd_extract_agent_certs
  cmd_push
  cmd_release
}

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
  login)            cmd_login           ;;
  build-bootstrap)  cmd_build_bootstrap ;;
  build)            cmd_build           ;;
  push)             cmd_push            ;;
  release)          cmd_release         ;;
  deploy)           cmd_deploy          ;;
  build-ui)         cmd_build_ui        ;;
  push-ui)          cmd_push_ui         ;;
  release-ui)       cmd_release_ui      ;;
  deploy-ui)        cmd_deploy_ui       ;;
  deploy-all)       cmd_deploy_all      ;;
  logs)             cmd_logs            ;;
  logs-ui)          cmd_logs_ui         ;;
  open)             cmd_open            ;;
  *)                die "unknown command: $1 (try login | build-bootstrap | build | push | release | deploy | build-ui | push-ui | release-ui | deploy-ui | deploy-all | logs | logs-ui | open)" ;;
esac
