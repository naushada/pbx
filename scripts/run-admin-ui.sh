#!/usr/bin/env bash
#
# run-admin-ui.sh — start the Vaadin admin UI against a cloud backend.
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
#
# Requires: Java 17+, Maven 3.9+ (one-time check below).
#
# Exit 1 if Java/Maven missing OR if onprem/pom.xml not where we expect.

set -euo pipefail

DEFAULT_BACKEND="https://pabx-5fbf3550f938.herokuapp.com"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
POM="${REPO_ROOT}/onprem/pom.xml"

BACKEND_URL="${BACKEND_URL:-$DEFAULT_BACKEND}"
PORT="${PORT:-8081}"
OFFLINE=0

log()  { printf '\033[1;34m[admin-ui]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[admin-ui]\033[0m %s\n' "$*" >&2; }
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
    -h|--help)     usage 0 ;;
    *) die "unknown arg: $1 (try --help)" ;;
  esac
done

[ -f "$POM" ] || die "onprem/pom.xml not found at $POM — are you in the onprem-pbx repo?"

command -v java >/dev/null 2>&1 || die "java not found — install Java 17+ (brew install openjdk@17)"
command -v mvn  >/dev/null 2>&1 || die "mvn not found — install Maven 3.9+ (brew install maven)"

# Refuse Java < 17 — Vaadin 24 + Spring Boot 3.2 hard-require it.
javamajor=$(java -version 2>&1 | awk -F'"' '/version/ {split($2, v, "."); print v[1]}')
# Note: 1.8.0_xxx style javas put "1" first; only modern javas (≥9) put the
# real major first. If the parse gave us "1", treat as too-old.
if [ -z "$javamajor" ] || [ "$javamajor" = "1" ] || [ "$javamajor" -lt 17 ] 2>/dev/null; then
  die "java is too old (saw '$javamajor') — Vaadin 24 needs Java 17+"
fi

log "backend = $BACKEND_URL"
log "port    = $PORT"
log "pom     = $POM"
log "  override either via env (BACKEND_URL / PORT) or --backend-url / --port"
log "  log in as societyCode=<bootstrap>, flatNumber=ADMIN, password=<bootstrap>"
log "  Ctrl-C stops the server (Spring Boot Maven plugin handles shutdown)"

mvn_args=(-f "$POM" spring-boot:run
          -Dspring-boot.run.arguments="--backend.url=$BACKEND_URL --server.port=$PORT")
if [ "$OFFLINE" = "1" ]; then
  mvn_args=(-o "${mvn_args[@]}")
fi

exec mvn "${mvn_args[@]}"
