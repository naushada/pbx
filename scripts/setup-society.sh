#!/usr/bin/env bash
#
# setup-society.sh — one-shot society install.
#
# Generates everything `podman-compose -f docker-compose.agent.yml up`
# needs that isn't already in the repo:
#
#   1. Asterisk DTLS-SRTP cert (self-signed; browsers verify it via the
#      per-call SDP `a=fingerprint:sha-256 …` line, not a CA chain).
#   2. 32-byte random `static-auth-secret` for coturn (also the value
#      that should land in `societies.turnSharedSecret` in Mongo so the
#      cloud's GET /api/v1/turn-credentials mints matching HMAC creds).
#   3. Rendered turnserver.conf with the secret + realm + external-ip
#      substituted in.
#
# Does NOT generate the cloud↔agent mTLS material — that's a separate
# concern (see README "Connect the on-prem stack to the live cloud").
# Does NOT bring the compose stack up — review the rendered files
# first, then run `podman-compose -f docker-compose.agent.yml up -d`.
#
# Usage:
#   ./scripts/setup-society.sh [SOCIETY_CODE]
#
# Outputs (under repo root):
#   certs/asterisk-dtls/pbx.crt
#   certs/asterisk-dtls/pbx.key
#   certs/turnserver.conf      (rendered from docker/coturn/turnserver.conf.template)
#
# Env (optional overrides):
#   SOCIETY_CODE        Society short code  (default: arg $1, then "demo-society")
#   EXTERNAL_IP         Public IP of the host running coturn
#                       (default: best-effort via ifconfig.me / ipify.org;
#                        falls back to "auto")
#   ASTERISK_KEYS_DIR   Where to write the DTLS cert  (default: certs/asterisk-dtls)
#   TURN_CONF_PATH      Where to write turnserver.conf (default: certs/turnserver.conf)

set -euo pipefail

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

case "${1:-}" in
  -h|--help|help) usage 0 ;;
esac

SOCIETY_CODE="${1:-${SOCIETY_CODE:-demo-society}}"
ASTERISK_KEYS_DIR="${ASTERISK_KEYS_DIR:-certs/asterisk-dtls}"
TURN_CONF_PATH="${TURN_CONF_PATH:-certs/turnserver.conf}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

log() { printf '\033[1;34m[setup-society]\033[0m %s\n' "$*"; }

# ── 1. Asterisk DTLS cert ────────────────────────────────────────────
log "generating Asterisk DTLS cert at ${ASTERISK_KEYS_DIR}/pbx.{crt,key}"
mkdir -p "$ASTERISK_KEYS_DIR"
if [[ -f "${ASTERISK_KEYS_DIR}/pbx.crt" ]]; then
    log "  …existing cert found, leaving it alone (rm + rerun to rotate)"
else
    # ECDSA P-256 cert — what WebRTC implementations actually want.
    openssl ecparam -genkey -name prime256v1 -noout \
        -out "${ASTERISK_KEYS_DIR}/pbx.key" 2>/dev/null
    openssl req -new -x509 -key "${ASTERISK_KEYS_DIR}/pbx.key" \
        -subj "/CN=pbx-asterisk-${SOCIETY_CODE}" \
        -days 3650 -sha256 \
        -out "${ASTERISK_KEYS_DIR}/pbx.crt" 2>/dev/null
    chmod 600 "${ASTERISK_KEYS_DIR}/pbx.key"

    FP=$(openssl x509 -in "${ASTERISK_KEYS_DIR}/pbx.crt" -noout -fingerprint -sha256 \
            | sed 's/^.*=//')
    log "  ✓ generated. SHA-256 fingerprint Asterisk will publish in SDP:"
    log "    $FP"
fi

# ── 2. coturn static-auth-secret ─────────────────────────────────────
mkdir -p "$(dirname "$TURN_CONF_PATH")"
if [[ -f "$TURN_CONF_PATH" ]] && grep -q '^static-auth-secret=' "$TURN_CONF_PATH"; then
    # `openssl rand -base64 32` produces 44 chars with a trailing `=`
    # pad. `awk -F= '… { print $2 }'` would split on every `=` and drop
    # the trailing pad — re-running the script would silently shift the
    # secret to a truncated variant and break HMAC against whatever
    # `societies.turnSharedSecret` the cloud Mongo holds. `sed` here
    # consumes the *first* `=` only, preserving the rest of the line.
    STATIC_AUTH_SECRET=$(sed -n 's/^static-auth-secret=//p' "$TURN_CONF_PATH" | head -n 1)
    log "reusing existing static-auth-secret from ${TURN_CONF_PATH}"
else
    STATIC_AUTH_SECRET=$(openssl rand -base64 32)
    log "generated new static-auth-secret (32 bytes base64)"
fi

# ── 3. Detect external IP (best-effort) ──────────────────────────────
# Validate that the response actually looks like an address — a captive
# portal or proxy can return HTML, an error page, or a hostname and
# without the check it'd land in `external-ip=` and crash coturn at
# startup (or worse, work but advertise the wrong address in ICE
# candidates and silently break TURN allocation).
is_ip_address() {
    # IPv4 dotted-quad OR IPv6 (any plausible shape — we don't need to
    # be RFC-strict here, just rule out captive-portal HTML).
    [[ "$1" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] && return 0
    [[ "$1" =~ ^[0-9a-fA-F:]+$ && "$1" == *":"* ]] && return 0
    return 1
}

if [[ -z "${EXTERNAL_IP:-}" ]]; then
    log "detecting external IP via ifconfig.me…"
    EXTERNAL_IP=$(curl -fsS --max-time 5 https://ifconfig.me 2>/dev/null || true)
    if [[ -n "$EXTERNAL_IP" ]] && ! is_ip_address "$EXTERNAL_IP"; then
        log "  …ifconfig.me returned '${EXTERNAL_IP}' — not an IP address"
        log "    (captive portal? proxy?). Discarding."
        EXTERNAL_IP=""
    fi
    if [[ -z "$EXTERNAL_IP" ]]; then
        log "  …couldn't detect a usable address. Falling back to 'auto' —"
        log "    coturn will STUN-itself to discover. Set EXTERNAL_IP"
        log "    explicitly if the society host is double-NAT'd."
        EXTERNAL_IP=auto
    else
        log "  external IP = $EXTERNAL_IP"
    fi
fi

# ── 4. Render turnserver.conf from template ──────────────────────────
REALM="${SOCIETY_CODE}.pbx.local"
log "rendering ${TURN_CONF_PATH} (realm=$REALM)"
export REALM STATIC_AUTH_SECRET EXTERNAL_IP
envsubst '${REALM} ${STATIC_AUTH_SECRET} ${EXTERNAL_IP}' \
    < docker/coturn/turnserver.conf.template \
    > "$TURN_CONF_PATH"

# ── 5. Print follow-up commands ──────────────────────────────────────
cat <<EOF

────────────────────────────────────────────────────────────────────
Society install ready.

  Society code:           ${SOCIETY_CODE}
  Asterisk DTLS cert:     ${ASTERISK_KEYS_DIR}/pbx.crt
  Asterisk DTLS key:      ${ASTERISK_KEYS_DIR}/pbx.key
  coturn config:          ${TURN_CONF_PATH}
  coturn realm:           ${REALM}
  coturn external-ip:     ${EXTERNAL_IP}
  TURN shared secret:     ${STATIC_AUTH_SECRET}

NEXT — sync the TURN shared secret to the cloud's Mongo:

  mongosh "<cloud Mongo URI>" --eval '
    db.societies.updateOne(
      { code: "${SOCIETY_CODE}" },
      { \$set: { turnSharedSecret: "${STATIC_AUTH_SECRET}" } },
      { upsert: true }
    )'

THEN — bring the on-prem stack up:

  podman-compose -f docker-compose.agent.yml up -d

VERIFY — agent + wsdbagent both connect:

  podman logs pbx-agent     | grep 'connected + WS-upgraded'
  podman logs pbx-wsdbagent | grep 'session started'
────────────────────────────────────────────────────────────────────
EOF
