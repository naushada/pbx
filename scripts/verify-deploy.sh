#!/usr/bin/env bash
# verify-deploy.sh — curl-probe the deployed (or local) softphone stack.
#
# Usage:
#   ./scripts/verify-deploy.sh                 # probe Heroku (defaults)
#   ./scripts/verify-deploy.sh remote          # same
#   ./scripts/verify-deploy.sh local           # probe http://localhost:8080
#   CLOUD_URL=... UI_URL=... ./scripts/verify-deploy.sh  # custom endpoints
#
# Exit codes:
#   0  — all probes pass
#   1  — one or more probes failed (per-probe failure already logged)
#   2  — bad invocation
#
# Each probe is a single curl with -fsSL (so 4xx/5xx fail), a timeout,
# and optional response-body matcher. Output is the per-probe pass/fail
# table; the script returns non-zero on any failure for easy CI use.

set -uo pipefail

MODE="${1:-remote}"

case "$MODE" in
    remote)
        # Single Heroku app — UI bundled under /webui/, REST under /api/v1/.
        BASE_URL="${BASE_URL:-https://pabx-5fbf3550f938.herokuapp.com}"
        ;;
    local)
        # Local "production-like" run of the bundled cloud image:
        #   podman run --rm -p 8080:8080 \
        #     -e PORT=8080 registry.heroku.com/pabx/web
        BASE_URL="${BASE_URL:-http://localhost:8080}"
        ;;
    *)
        echo "usage: $0 [remote|local]" >&2
        exit 2
        ;;
esac
UI_BASE="${BASE_URL}/webui"

# ── colours ─────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    GREEN=$(printf '\033[0;32m')
    RED=$(printf '\033[0;31m')
    DIM=$(printf '\033[2m')
    RST=$(printf '\033[0m')
else
    GREEN= RED= DIM= RST=
fi

PASS=0
FAIL=0

# probe LABEL URL [MATCHER]      — expects 2xx; must contain MATCHER if given
# probe_alive LABEL URL          — accepts any non-zero HTTP status (proves
#                                  the server responded; useful for proxy
#                                  reachability checks against endpoints that
#                                  legitimately 404).
_probe_curl() {
    local url="$1" tmp="$2"
    curl --max-time 10 \
         --output "$tmp" \
         --silent --show-error \
         --write-out '%{http_code} %{size_download} %{time_total}\n' \
         "$url" 2>/dev/null || echo "000 0 0"
}

_probe_record() {
    local tag="$1" label="$2" http_code="$3" size_dl="$4" time_total="$5" url="$6"
    printf '  %s  %-30s  %s %5sB  %5ss  %s\n' \
        "$tag" "$label" "$http_code" "$size_dl" "$time_total" "${DIM}${url}${RST}"
}

probe() {
    local label="$1" url="$2" matcher="${3:-}"
    local tmp http_code size_dl time_total ok=1
    tmp=$(mktemp)

    read -r http_code size_dl time_total < <(_probe_curl "$url" "$tmp")

    if [[ "$http_code" -lt 200 || "$http_code" -ge 400 ]]; then
        ok=0
    elif [[ -n "$matcher" ]] && ! grep -qi -- "$matcher" "$tmp"; then
        ok=0
    fi

    if (( ok )); then
        _probe_record "${GREEN}PASS${RST}" "$label" "$http_code" "$size_dl" "$time_total" "$url"
        PASS=$((PASS+1))
    else
        _probe_record "${RED}FAIL${RST}"   "$label" "$http_code" "$size_dl" "$time_total" "$url"
        FAIL=$((FAIL+1))
    fi
    rm -f "$tmp"
}

probe_alive() {
    local label="$1" url="$2"
    local tmp http_code size_dl time_total
    tmp=$(mktemp)
    read -r http_code size_dl time_total < <(_probe_curl "$url" "$tmp")

    if [[ "$http_code" != "000" ]]; then
        _probe_record "${GREEN}PASS${RST}" "$label" "$http_code" "$size_dl" "$time_total" "$url"
        PASS=$((PASS+1))
    else
        _probe_record "${RED}FAIL${RST}"   "$label" "$http_code" "$size_dl" "$time_total" "$url"
        FAIL=$((FAIL+1))
    fi
    rm -f "$tmp"
}

printf -- '─── %s probes ──────────────────────────────────────────\n' "$MODE"
printf '  Base: %s\n  UI:   %s\n\n' "$BASE_URL" "$UI_BASE"

probe       "UI /webui/ (index.html)"  "$UI_BASE/"              "Society Softphone"
probe       "UI /webui/main.js"        "$UI_BASE/main.js"
probe       "UI /webui/favicon.svg"    "$UI_BASE/favicon.svg"   "viewBox"
probe       "UI /webui/favicon.ico"    "$UI_BASE/favicon.ico"
probe       "UI /webui/sw.js"          "$UI_BASE/sw.js"         "Softphone"
probe       "SPA fallback (deep link)" "$UI_BASE/main/dashboard" "Society Softphone"
# Cloud REST surfaces — many handlers may legitimately 404 until wired;
# probe_alive verifies the server responded (i.e. SPA fallback didn't fire).
probe_alive "Cloud /api/v1/push-vapid-key" "$BASE_URL/api/v1/push-vapid-key"

echo
if (( FAIL == 0 )); then
    printf '%s  %d/%d probes passed%s\n' "$GREEN" "$PASS" "$((PASS+FAIL))" "$RST"
    exit 0
else
    printf '%s  %d/%d probes failed%s\n' "$RED" "$FAIL" "$((PASS+FAIL))" "$RST"
    exit 1
fi
