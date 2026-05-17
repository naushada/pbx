# onprem-pbx-installer

Cross-platform installer container for the
[onprem-pbx project](https://github.com/naushada/pbx). One
`docker run` command brings the full on-prem PBX stack up on any
host with a Docker daemon — Linux, macOS, or Windows with Docker
Desktop.

Uses the Docker-out-of-Docker (DooD) pattern: this container mounts
the host's Docker socket and creates the on-prem stack's containers
on the HOST (not nested inside this one).

## Operator command

Before running, get the per-society cert tarball from your dev team
(produced by `./deploy-heroku.sh package-society-certs <SOCIETY>`).

### Linux / macOS

```sh
mkdir -p ~/onprem-pbx-data
cp /path/to/SUNSET-certs.tar.gz ~/onprem-pbx-data/

docker run --rm -it \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -v ~/onprem-pbx-data:/data \
  -e HOST_DATA_DIR=$HOME/onprem-pbx-data \
  -e SOCIETY_CODE=SUNSET \
  -e SOCIETY_NAME="Sunset Towers" \
  -e ADMIN_EMAIL=admin@sunset.example \
  -e ADMIN_PASSWORD="choose-something" \
  -e CERTS_TARBALL=/data/SUNSET-certs.tar.gz \
  docker.io/naushada/onprem-pbx-installer:latest
```

### Windows (PowerShell)

```powershell
New-Item -ItemType Directory -Force -Path "$HOME\onprem-pbx-data" | Out-Null
Copy-Item C:\Downloads\SUNSET-certs.tar.gz "$HOME\onprem-pbx-data\"

docker run --rm -it `
  -v "//./pipe/docker_engine:/var/run/docker.sock" `
  -v "${HOME}\onprem-pbx-data:/data" `
  -e HOST_DATA_DIR="$HOME\onprem-pbx-data" `
  -e SOCIETY_CODE=SUNSET `
  -e SOCIETY_NAME="Sunset Towers" `
  -e ADMIN_EMAIL=admin@sunset.example `
  -e ADMIN_PASSWORD="choose-something" `
  -e CERTS_TARBALL=/data/SUNSET-certs.tar.gz `
  docker.io/naushada/onprem-pbx-installer:latest
```

## What it does

1. Validates env vars (fails fast on missing).
2. Pings the host Docker daemon (sanity-checks the socket mount).
3. Unpacks the cert tarball into `/data/certs/cloud-issued/`.
4. Generates Asterisk DTLS + coturn config (via the baked-in
   `setup-society.sh`).
5. Writes `/data/.env` with `HOST_DATA_DIR`-prefixed absolute paths
   so the compose bind-mounts resolve correctly on the host.
6. `docker compose pull` + `docker compose up -d` against the host
   daemon — pulls + starts pbx-mongo, pbx-asterisk, pbx-coturn,
   pbx-agent, pbx-wsdbagent, pbx-cert-watcher.
7. Waits for Mongo's replica set, then upserts society + ADMIN
   subscriber via `docker exec pbx-mongo mongosh`. Password
   hashed locally via PBKDF2-SHA256 — never sent over the wire.
8. Prints day-2 operator commands.

After the green summary, the operator's admin UI login works
immediately with the supplied email + password.

See [`INSTALL-windows.md`](https://github.com/naushada/pbx/blob/main/INSTALL-windows.md)
for the full Windows-specific walkthrough including known runtime
limitations (TURN/RTP via WSL2 NAT).

## Tags

- `latest` — rolling main-branch build.
- `<sha>` — immutable per-commit tag.

Architectures: `linux/amd64`, `linux/arm64`.

Source: https://github.com/naushada/pbx — `docker/Dockerfile.installer`
