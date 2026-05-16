#!/usr/bin/env bash
#
# sample_subscriber.sh — seed two demo residents (A101, A102) into the
# on-prem Mongo so you can dial one from the other in the webui softphone.
#
# Idempotent — re-runs replace existing rows of the same flat numbers.
# Writes directly via `podman exec pbx-mongo mongosh` inside the Lima VM,
# so the cloud's REST auth path doesn't need an admin session token.
#
# Usage:
#   scripts/sample_subscriber.sh                       # → soc_sunset + 'changeme'
#   scripts/sample_subscriber.sh --society-code DEMO
#   scripts/sample_subscriber.sh --password 'hunter2'
#
# Login afterwards in the webui softphone:
#   societyCode = <--society-code>     (default: soc_sunset)
#   flatNumber  = A101  /  A102
#   password    = <--password>         (default: changeme)
#
# Requires: lima VM up with the on-prem stack running (`lima start`).

set -euo pipefail

SOCIETY_CODE="${SOCIETY_CODE:-soc_sunset}"
PASSWORD="${PASSWORD:-changeme}"

while [ $# -gt 0 ]; do
  case "$1" in
    --society-code) SOCIETY_CODE=$2; shift 2 ;;
    --password)     PASSWORD=$2;     shift 2 ;;
    -h|--help)      sed -n '2,/^$/p' "$0" | sed 's/^# \?//' ; exit 0 ;;
    *) printf 'unknown arg: %s (try --help)\n' "$1" >&2; exit 2 ;;
  esac
done

log() { printf '\033[1;34m[sample_subscriber]\033[0m %s\n' "$*"; }

# Resolve the lima.sh wrapper next to this script so the user can run
# `./scripts/sample_subscriber.sh` from any cwd.
LIMA_SH="$(cd "$(dirname "$0")" && pwd -P)/lima.sh"

# PBKDF2-SHA256, 600 000 iters — matches MongodbClient::hash_password.
log "hashing password (PBKDF2-SHA256, 600 000 iters)"
PORTAL_HASH=$(PASSWORD="$PASSWORD" python3 - <<'PY'
import os, base64, hashlib
pw   = os.environ['PASSWORD'].encode()
salt = os.urandom(16)
key  = hashlib.pbkdf2_hmac('sha256', pw, salt, 600_000, dklen=32)
print(f"$pbkdf2-sha256$i=600000${base64.b64encode(salt).decode()}${base64.b64encode(key).decode()}")
PY
)

log "seeding A101 + A102 in societies/${SOCIETY_CODE}"
# Escape the leading $ in the hash so the heredoc'd JS sees it literally;
# mongosh treats $-prefixed identifiers as operators otherwise.
ESC_HASH=$(printf '%s' "$PORTAL_HASH" | sed 's/\$/\\$/g')

"$LIMA_SH" shell sudo podman exec pbx-mongo mongosh --quiet --eval "
  db = db.getSiblingDB('pabx');

  // Bail loudly if the society row doesn't exist — without it the
  // login flow's society→subscriber join returns 401 even with the
  // correct password.
  if (db.societies.countDocuments({_id: '${SOCIETY_CODE}'}) === 0) {
    print('ERROR: societies/${SOCIETY_CODE} not found — run bootstrap-society.sh first');
    quit(1);
  }

  ['A101', 'A102'].forEach(flat => {
    const sub_id = 'sub-${SOCIETY_CODE}-' + flat;
    const sip    = flat.toLowerCase();   // sip username = a101 / a102
    const r = db.subscribers.replaceOne(
      {_id: sub_id},
      {
        _id:                sub_id,
        societyId:          '${SOCIETY_CODE}',
        flatNumber:         flat,
        name:               'Resident ' + flat,
        email:              sip + '@${SOCIETY_CODE}.example',
        role:               'resident',
        status:             'active',
        sipUsername:        sip,
        sipHa1:             '',
        portalPasswordHash: '${ESC_HASH}',
        createdAt:          new Date(),
      },
      {upsert: true}
    );
    print('  ' + flat + '  sipUser=' + sip +
          (r.matchedCount ? '  (updated)' : '  (created)'));
  });
"

log "done. log in to the webui at https://pabx-5fbf3550f938.herokuapp.com/"
log "  societyCode = ${SOCIETY_CODE}"
log "  flatNumber  = A101 (or A102)"
log "  password    = ${PASSWORD}"
log "  → call between two browser tabs (incognito is fine for the 2nd)."
