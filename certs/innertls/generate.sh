#!/usr/bin/env bash
# Generate CA, server, client certificates for wsdbagent ↔ wsdbproxy
# inner TLS (ACE InnerTLS over the /ws/db WebSocket).
#
# Heroku Common Runtime terminates the outer TLS at the router, so the
# only way the cloud's wsdbproxy can be sure it's talking to a real
# pbx-wsdbagent — and vice versa — is to layer a second TLS handshake
# inside the WebSocket frames. Both peers verify the other's leaf cert
# against this shared CA.
#
# Layout (matches xpmile/certs/):
#   ca.crt        — project CA, distributed to both peers, committed
#   ca.key        — CA private key, gitignored, build-time only
#   server.crt    — pbx-cloud's identity, baked into the cloud image
#   server.key    — pbx-cloud's private key, baked into the cloud image
#                   (NOTE: regenerated fresh on every Dockerfile.cloud
#                    build — see the cert-baking stage there)
#   client.crt    — pbx-wsdbagent's identity, mounted at runtime
#   client.key    — pbx-wsdbagent's private key, mounted at runtime
#
# Run once on a clean clone; commit the .crt files. The .key files
# stay on the developer's machine; the operator who's deploying a
# new society gets them via a separate secure channel (or runs this
# script themselves to mint a fresh CA, which means the existing
# baked cloud image needs a rebuild + redeploy too).

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

# ── CA ──────────────────────────────────────────────────────────────
openssl genrsa -out ca.key 4096 2>/dev/null
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
  -subj "/O=onprem-pbx/CN=onprem-pbx-CA" 2>/dev/null

# ── Server (pbx-cloud) ──────────────────────────────────────────────
# CN = pabx.herokuapp.com. The wsdbagent's outer WSS connection still
# targets the Heroku-assigned hostname (pabx-<suffix>.herokuapp.com),
# but its --tls-hostname is set to pabx.herokuapp.com so the InnerTLS
# CN check matches this cert. Using the short name keeps the cert
# stable across Heroku reissues of the long-form suffix.
openssl genrsa -out server.key 4096 2>/dev/null
openssl req -new -key server.key -out server.csr \
  -subj "/O=onprem-pbx/CN=pabx.herokuapp.com" 2>/dev/null
openssl x509 -req -days 3650 -in server.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt 2>/dev/null
rm -f server.csr

# ── Client (pbx-wsdbagent) ──────────────────────────────────────────
openssl genrsa -out client.key 4096 2>/dev/null
openssl req -new -key client.key -out client.csr \
  -subj "/O=onprem-pbx/CN=pbx-wsdbagent" 2>/dev/null
openssl x509 -req -days 3650 -in client.csr \
  -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt 2>/dev/null
rm -f client.csr ca.srl

chmod 600 ca.key server.key client.key

cat <<EOF
Generated InnerTLS material in certs/innertls/:
  ca.crt       — project CA           (committed)
  ca.key       — CA private key       (gitignored; build-time only)
  server.crt   — pbx-cloud identity   (committed for reference;
                                       Dockerfile.cloud regenerates a
                                       fresh server.key on each build
                                       and signs with the in-repo CA)
  server.key   — pbx-cloud key        (gitignored)
  client.crt   — pbx-wsdbagent ident. (committed)
  client.key   — pbx-wsdbagent key    (gitignored; mounted at runtime)

Distribution:
  - Cloud:  Dockerfile.cloud bakes server.{crt,key} + ca.crt into the
            runtime image. Pushed to Heroku → all dynos trust this CA.
  - Agent:  certs/innertls/ is mounted into pbx-wsdbagent at runtime
            via docker-compose.agent.yml.
EOF
