#!/usr/bin/env bash
#
# bootstrap-society.sh — one-shot CLI to seed a brand-new society + its
# first admin subscriber. Solves the chicken-and-egg: the admin UI
# needs an admin to log in, an admin needs a society row, a society
# needs an operator to call POST /api/v1/society.
#
# What it does, in order:
#
#   1. POSTs /api/v1/society to the cloud → returns the new society's
#      `_id`, `sipRealm`, and `turnSharedSecret`.
#   2. Computes the PBKDF2-SHA256 portal password hash for the admin
#      using the same format the cloud's MongodbClient::hash_password
#      emits ($pbkdf2-sha256$i=600000$<salt-b64>$<key-b64>).
#   3. Writes the admin subscribers row directly via mongosh against
#      the supplied MONGO_URI (cloud REST has no single-row subscriber
#      create endpoint — bulk import doesn't fit because the operator
#      needs to choose the password, not have one generated).
#   4. Prints the SIP realm + TURN shared secret + admin login
#      coordinates to copy into the on-prem `.env` and the UI.
#
# Login afterwards:
#   societyCode = <--society-code>
#   flatNumber  = ADMIN
#   password    = <--admin-password>
#
# Usage:
#   bootstrap-society.sh \
#     --society-code   SUNSET \
#     --society-name   'Sunset Towers' \
#     --admin-email    admin@sunset.example \
#     --admin-password 'secret-pass' \
#     --mongo-uri      mongodb://localhost:27017/pabx \
#    [--cloud-host    pabx-5fbf3550f938.herokuapp.com] \
#    [--cloud-scheme  https]
#
# Requires: curl, python3, mongosh, jq.

set -euo pipefail

SOCIETY_CODE=""
SOCIETY_NAME=""
ADMIN_EMAIL=""
ADMIN_PASSWORD=""
MONGO_URI=""
CLOUD_HOST="${CLOUD_HOST:-pabx-5fbf3550f938.herokuapp.com}"
CLOUD_SCHEME="${CLOUD_SCHEME:-https}"

log()  { printf '\033[1;34m[bootstrap-society]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[bootstrap-society]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[bootstrap-society]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
  exit "${1:-2}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --society-code)   SOCIETY_CODE=$2;   shift 2 ;;
    --society-name)   SOCIETY_NAME=$2;   shift 2 ;;
    --admin-email)    ADMIN_EMAIL=$2;    shift 2 ;;
    --admin-password) ADMIN_PASSWORD=$2; shift 2 ;;
    --mongo-uri)      MONGO_URI=$2;      shift 2 ;;
    --cloud-host)     CLOUD_HOST=$2;     shift 2 ;;
    --cloud-scheme)   CLOUD_SCHEME=$2;   shift 2 ;;
    -h|--help|help)   usage 0 ;;
    *) die "unknown arg: $1 (try --help)" ;;
  esac
done

[ -z "$SOCIETY_CODE" ]  && die "--society-code is required"
[ -z "$SOCIETY_NAME" ]  && die "--society-name is required"
[ -z "$ADMIN_EMAIL" ]   && die "--admin-email is required"
[ -z "$MONGO_URI" ]     && die "--mongo-uri is required (e.g. mongodb://localhost:27017/pabx)"

# Prompt for password if not provided — never echo it.
if [ -z "$ADMIN_PASSWORD" ]; then
  printf 'admin password: '
  stty -echo
  read -r ADMIN_PASSWORD
  stty echo
  printf '\n'
fi
[ -z "$ADMIN_PASSWORD" ] && die "admin password cannot be empty"

for cmd in curl python3 mongosh jq; do
  command -v "$cmd" >/dev/null 2>&1 || die "$cmd not found — install it or adjust PATH"
done

# ─── 1. Create society via cloud REST ─────────────────────────────────
log "POST ${CLOUD_SCHEME}://${CLOUD_HOST}/api/v1/society"
society_rsp=$(curl -fsS -X POST \
  "${CLOUD_SCHEME}://${CLOUD_HOST}/api/v1/society" \
  -H 'Content-Type: application/json' \
  -d "$(jq -n --arg n "$SOCIETY_NAME" --arg c "$SOCIETY_CODE" \
       '{name: $n, code: $c}')") \
  || die "society POST failed — check cloud is reachable + REMOTE_DB pipe is alive"

society_id=$(printf '%s' "$society_rsp" | jq -er '._id')
sip_realm=$(printf '%s' "$society_rsp"  | jq -er '.sipRealm')
turn_secret=$(printf '%s' "$society_rsp" | jq -er '.turnSharedSecret')
log "society created: _id=$society_id sipRealm=$sip_realm"

# ─── 2. PBKDF2-SHA256 password hash ──────────────────────────────────
# Format matches MongodbClient::hash_password:
#   $pbkdf2-sha256$i=600000$<salt-b64>$<key-b64>
# salt = 16 random bytes, key = SHA-256 32-byte output, 600 000 iters.
log "hashing admin password (PBKDF2-SHA256, 600 000 iters)"
password_hash=$(ADMIN_PASSWORD="$ADMIN_PASSWORD" python3 - <<'PY'
import os, base64, hashlib
pw = os.environ['ADMIN_PASSWORD'].encode()
salt = os.urandom(16)
key = hashlib.pbkdf2_hmac('sha256', pw, salt, 600_000, dklen=32)
print(f"$pbkdf2-sha256$i=600000${base64.b64encode(salt).decode()}${base64.b64encode(key).decode()}")
PY
)

# ─── 3. Insert admin subscriber row ──────────────────────────────────
# flatNumber=ADMIN is what the operator types in the login form
# (societyCode + flatNumber + password). role=admin gates the
# Vaadin admin views. SIP credentials are stubbed — admins won't
# REGISTER for a phone identity.
log "inserting admin subscribers row via mongosh"

mongosh --quiet "$MONGO_URI" --eval "
  db.subscribers.insertOne({
    societyId:           '${society_id}',
    flatNumber:          'ADMIN',
    name:                'Society Admin',
    email:               '${ADMIN_EMAIL}',
    role:                'admin',
    status:              'active',
    sipUsername:         'admin-${society_id}',
    sipHa1:              '',
    portalPasswordHash:  '${password_hash}',
    createdAt:           new Date(),
  });
" >/dev/null || die "mongosh insert failed — check MONGO_URI is reachable + DB is rs0-initialized"

# ─── 4. Operator handoff ─────────────────────────────────────────────
cat <<EOF

$(printf '\033[1;32m[bootstrap-society]\033[0m done.')

  Society:
    code               ${SOCIETY_CODE}
    name               ${SOCIETY_NAME}
    _id                ${society_id}
    sipRealm           ${sip_realm}
    turnSharedSecret   ${turn_secret}

  Admin login:
    URL                ${CLOUD_SCHEME}://${CLOUD_HOST}/webui/
    societyCode        ${SOCIETY_CODE}
    flatNumber         ADMIN
    password           (what you just typed)

  Next steps:
    1. Set PBX_AUTH_STRICT=1 on the cloud so strict-mode bcrypt verify
       gates this admin (heroku config:set PBX_AUTH_STRICT=1 -a pabx).
    2. Wire sipRealm + turnSharedSecret into the on-prem .env:
         SIP_REALM=${sip_realm}
         TURN_SHARED_SECRET=${turn_secret}
       (these are normally seeded automatically by setup-society.sh
       too — keep them in sync).
    3. Log into the Vaadin admin UI with the credentials above and
       use Subscribers → Bulk Import to onboard residents from CSV.

EOF
