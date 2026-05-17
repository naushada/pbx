#!/usr/bin/env bash
#
# installer-entrypoint.sh — runs INSIDE the onprem-pbx-installer
# container (docker/Dockerfile.installer). Drives the on-prem stack
# install via Docker-out-of-Docker (DooD): the host's docker daemon
# socket is mounted into this container, so `docker compose up`
# creates containers on the HOST, not nested.
#
# Reads all knobs from env vars (set via `docker run -e KEY=VALUE`).
# Prompts only when running interactively + an env var is missing.
#
# Required env:
#   SOCIETY_CODE           short label, e.g. SUNSET
#   SOCIETY_NAME           full name, e.g. 'Sunset Towers'
#   ADMIN_EMAIL            first admin's email
#   ADMIN_PASSWORD         first admin's password (plaintext; hashed locally)
#   CERTS_TARBALL          path INSIDE the container to the .tar.gz,
#                          e.g. /data/SUNSET-certs.tar.gz
#   HOST_DATA_DIR          the path on the HOST that's bind-mounted at /data
#                          inside this container. Required because the
#                          host's docker daemon interprets bind-mount
#                          paths in compose against the HOST filesystem,
#                          not the container's view.
#
# Optional env:
#   CLOUD_HOST             default: pabx-5fbf3550f938.herokuapp.com
#   MONGO_DB_NAME          default: pabx
#   HEALTH_WAIT_SECS       default: 120
#
# Usage on host (Linux):
#   mkdir -p ~/onprem-pbx-data
#   cp ~/Downloads/SUNSET-certs.tar.gz ~/onprem-pbx-data/
#   docker run --rm -it \
#     -v ~/onprem-pbx-data:/data \
#     -v /var/run/podman/podman.sock:/var/run/docker.sock \   # podman compat
#     -v /var/run/docker.sock:/var/run/docker.sock \          # OR docker
#     -e HOST_DATA_DIR=$HOME/onprem-pbx-data \
#     -e SOCIETY_CODE=SUNSET \
#     -e SOCIETY_NAME='Sunset Towers' \
#     -e ADMIN_EMAIL=admin@sunset.example \
#     -e ADMIN_PASSWORD='choose-something' \
#     -e CERTS_TARBALL=/data/SUNSET-certs.tar.gz \
#     docker.io/naushada/onprem-pbx-installer:latest

set -euo pipefail

# ── Constants ────────────────────────────────────────────────────────
INSTALLER_DIR=/opt/installer
DATA_DIR=/data
COMPOSE_FILE=docker-compose.agent.yml
DEFAULT_CLOUD_HOST="pabx-5fbf3550f938.herokuapp.com"
DEFAULT_MONGO_DB="pabx"
HEALTH_WAIT_SECS="${HEALTH_WAIT_SECS:-120}"

# ── Colour helpers ──────────────────────────────────────────────────
if [[ -t 1 ]]; then
  C_BLUE=$'\033[1;34m'; C_GREEN=$'\033[1;32m'; C_YELLOW=$'\033[1;33m'
  C_RED=$'\033[1;31m';  C_BOLD=$'\033[1m';     C_RESET=$'\033[0m'
else
  C_BLUE=; C_GREEN=; C_YELLOW=; C_RED=; C_BOLD=; C_RESET=
fi

step()  { printf '\n%s[installer] %s%s\n' "$C_BLUE"   "$*" "$C_RESET"; }
ok()    { printf '%s  ✓ %s%s\n'           "$C_GREEN"  "$*" "$C_RESET"; }
warn()  { printf '%s  ! %s%s\n'           "$C_YELLOW" "$*" "$C_RESET" >&2; }
die()   { printf '%s  ✗ %s%s\n'           "$C_RED"    "$*" "$C_RESET" >&2; exit 1; }

# ── 1. Validate env ─────────────────────────────────────────────────
step "Validating inputs"

for v in SOCIETY_CODE SOCIETY_NAME ADMIN_EMAIL ADMIN_PASSWORD CERTS_TARBALL HOST_DATA_DIR; do
  if [[ -z "${!v:-}" ]]; then
    die "Missing required env: $v
  Re-run docker with -e $v=<value>. See:
  https://github.com/naushada/pbx/blob/main/INSTALL-windows.md#path-b"
  fi
done

CLOUD_HOST="${CLOUD_HOST:-$DEFAULT_CLOUD_HOST}"
MONGO_DB="${MONGO_DB_NAME:-$DEFAULT_MONGO_DB}"

[[ -d "$DATA_DIR" ]] || die "$DATA_DIR is missing inside the container.
  Did you forget to mount the host data dir? Re-run with:
    -v <host-path>:$DATA_DIR
    -e HOST_DATA_DIR=<host-path>"

[[ -f "$CERTS_TARBALL" ]] || die "Can't find the cert tarball at:
  $CERTS_TARBALL   (inside the container)
  Put the .tar.gz inside the host dir you mounted at $DATA_DIR."

ok "society       = $SOCIETY_CODE ($SOCIETY_NAME)"
ok "admin email   = $ADMIN_EMAIL"
ok "cloud host    = $CLOUD_HOST"
ok "certs tarball = $CERTS_TARBALL"
ok "host data dir = $HOST_DATA_DIR  (mounted at $DATA_DIR)"

# ── 2. Verify host docker is reachable ──────────────────────────────
step "Pinging host docker daemon"
docker version --format '{{.Server.Version}}' >/dev/null 2>&1 \
  || die "Can't reach the host docker daemon.
  Re-run with the socket mounted:
    Linux:   -v /var/run/docker.sock:/var/run/docker.sock
    Windows: -v //./pipe/docker_engine://./pipe/docker_engine"
ok "host docker reachable: $(docker version --format '{{.Server.Version}}')"

# ── 3. Unpack cert tarball into host data dir ───────────────────────
step "Unpacking certs into $DATA_DIR/certs/cloud-issued/"
mkdir -p "$DATA_DIR/certs/cloud-issued"
tar -xzf "$CERTS_TARBALL" -C "$DATA_DIR/certs/cloud-issued/" \
  || die "Couldn't extract $CERTS_TARBALL. Is it a .tar.gz from your dev team?"
for d in innertls sip-agent; do
  if [[ ! -d "$DATA_DIR/certs/cloud-issued/$d" ]]; then
    warn "Cert tarball doesn't contain certs/cloud-issued/$d — pbx-wsdbagent or pbx-agent will fail to handshake."
  fi
done
ok "Certs unpacked"

# ── 4. Generate per-society Asterisk DTLS + coturn config ───────────
step "Generating per-society config (Asterisk DTLS + coturn)"
# setup-society.sh writes to certs/ relative to the cwd, so cd into
# /data first. The script needs docker/coturn/turnserver.conf.template
# which we baked in at /opt/installer/docker/coturn/. Symlink so
# setup-society finds it via its expected relative path.
cd "$DATA_DIR"
ln -sf "$INSTALLER_DIR/docker" "$DATA_DIR/docker"
SOCIETY_CODE="$SOCIETY_CODE" \
  "$INSTALLER_DIR/scripts/setup-society.sh" "$SOCIETY_CODE" \
  || die "setup-society.sh failed. See output above."
ok "Society config ready"

# ── 5. Drop compose file + .env into the host data dir ──────────────
# Both paths must resolve correctly when the HOST docker daemon
# interprets them. HOST_DATA_DIR is the host-side prefix; the same
# files appear at $DATA_DIR inside this container AND at $HOST_DATA_DIR
# on the host. Compose's bind mounts in docker-compose.agent.yml use
# env vars CERTS_DIR / INNERTLS_DIR / CLOUD_ISSUED_DIR — we set them
# to HOST_DATA_DIR-prefixed absolute paths so the host daemon resolves
# them correctly.
step "Writing $DATA_DIR/$COMPOSE_FILE + $DATA_DIR/.env"
cp "$INSTALLER_DIR/$COMPOSE_FILE" "$DATA_DIR/$COMPOSE_FILE"
cat > "$DATA_DIR/.env" <<ENV
# Generated by installer-entrypoint.sh on $(date '+%Y-%m-%d %H:%M:%S')
CLOUD_HOST=$CLOUD_HOST
AGENT_SOCIETY_ID=$SOCIETY_CODE
CERTS_DIR=$HOST_DATA_DIR/certs/cloud-issued/sip-agent
INNERTLS_DIR=$HOST_DATA_DIR/certs/cloud-issued/innertls
CLOUD_ISSUED_DIR=$HOST_DATA_DIR/certs/cloud-issued
MONGO_DB_NAME=$MONGO_DB
TURN_PUBLIC_PORT=3478
ENV
chmod 600 "$DATA_DIR/.env"
ok "Wrote $DATA_DIR/.env (HOST_DATA_DIR=$HOST_DATA_DIR)"

# ── 6. Pull pre-built images + bring stack up ───────────────────────
step "Pulling images from Docker Hub"
docker compose -f "$DATA_DIR/$COMPOSE_FILE" --env-file "$DATA_DIR/.env" pull \
  || die "docker compose pull failed. Check internet + Docker Hub reachability."
ok "Images pulled"

step "Starting containers (docker compose up -d)"
docker compose -f "$DATA_DIR/$COMPOSE_FILE" --env-file "$DATA_DIR/.env" up -d \
  || die "docker compose up failed. Check: docker compose -f $HOST_DATA_DIR/$COMPOSE_FILE logs"
ok "Containers started on host"

# ── 7. Wait for boot, then verify ───────────────────────────────────
step "Waiting ${HEALTH_WAIT_SECS}s for Mongo + cloud-tunnel handshake"
sleep "$HEALTH_WAIT_SECS"

step "Container status (HOST daemon)"
docker ps --filter name=pbx- --format 'table {{.Names}}\t{{.Status}}'

# ── 8. Seed society + ADMIN in Mongo ────────────────────────────────
step "Seeding society + ADMIN row in Mongo"

# Wait for the replica set to be primary (compose healthcheck races
# with this script). 20 × 3 s = 60 s budget.
for i in $(seq 1 20); do
  if docker exec pbx-mongo mongosh --quiet --eval 'rs.status().ok' 2>/dev/null \
       | grep -q '^1$'; then
    break
  fi
  sleep 3
done

PORTAL_HASH=$(PASSWORD="$ADMIN_PASSWORD" python3 - <<'PY'
import os, base64, hashlib
pw   = os.environ['PASSWORD'].encode()
salt = os.urandom(16)
key  = hashlib.pbkdf2_hmac('sha256', pw, salt, 600_000, dklen=32)
print(f"$pbkdf2-sha256$i=600000${base64.b64encode(salt).decode()}${base64.b64encode(key).decode()}")
PY
)
ESC_HASH=$(printf '%s' "$PORTAL_HASH" | sed 's/\$/\\$/g')

docker exec pbx-mongo mongosh --quiet --eval "
  db = db.getSiblingDB('$MONGO_DB');
  db.societies.replaceOne(
    { _id: '$SOCIETY_CODE' },
    {
      _id:      '$SOCIETY_CODE',
      code:     '$SOCIETY_CODE',
      name:     '$SOCIETY_NAME',
      sipRealm: '$SOCIETY_CODE.pbx.local',
    },
    { upsert: true }
  );
  db.subscribers.replaceOne(
    { _id: 'sub-$SOCIETY_CODE-ADMIN' },
    {
      _id:                'sub-$SOCIETY_CODE-ADMIN',
      societyId:          '$SOCIETY_CODE',
      flatNumber:         'ADMIN',
      name:               'Society Admin',
      email:              '$ADMIN_EMAIL',
      role:               'admin',
      status:             'active',
      sipUsername:        'admin-$SOCIETY_CODE',
      portalPasswordHash: '$ESC_HASH',
      createdAt:          new Date(),
    },
    { upsert: true }
  );
" >/dev/null \
  || die "Mongo seeding failed. Check: docker logs pbx-mongo | tail -50"
ok "Seeded society '$SOCIETY_CODE' + ADMIN ($ADMIN_EMAIL)"

# ── Summary ─────────────────────────────────────────────────────────
printf '\n%s' "$C_BOLD"
printf '%s╔══════════════════════════════════════════════════════════╗%s\n' "$C_BLUE" "$C_RESET"
printf '%s║%s        %sonprem-pbx is installed via installer%s             %s║%s\n' "$C_BLUE" "$C_RESET" "$C_GREEN" "$C_RESET" "$C_BLUE" "$C_RESET"
printf '%s╚══════════════════════════════════════════════════════════╝%s\n' "$C_BLUE" "$C_RESET"

cat <<EOF

  Society code:   $SOCIETY_CODE
  Cloud target:   $CLOUD_HOST
  Host data dir:  $HOST_DATA_DIR
  Reboot survival: containers restart via 'restart: unless-stopped'
                   policy. Make sure docker / Docker Desktop is set to
                   start on host boot.

  ${C_BOLD}Check status (on the HOST):${C_RESET}
    docker ps --filter name=pbx-
    docker logs -f pbx-agent
    docker logs -f pbx-wsdbagent

  ${C_BOLD}Stop / start manually (on the HOST):${C_RESET}
    docker compose -f $HOST_DATA_DIR/$COMPOSE_FILE --env-file $HOST_DATA_DIR/.env stop
    docker compose -f $HOST_DATA_DIR/$COMPOSE_FILE --env-file $HOST_DATA_DIR/.env start

EOF
