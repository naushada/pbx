# onprem-pbx-wsdbagent

On-prem DB-tunnel daemon for the [onprem-pbx project](https://github.com/naushada/pbx).
Bridges the cloud-side `pbx-cloud` (running on Heroku) to the
on-prem MongoDB so all reads/writes from the cloud's REST handlers
flow through the on-prem Mongo over a single secure WebSocket
tunnel.

This image is **pulled by operators** on society-host machines as
part of the standard install. **Use the installer**, not a manual
`docker pull` — see the
[`onprem-pbx-installer`](https://hub.docker.com/r/naushada/onprem-pbx-installer)
image.

## What this container does

- Dials `wss://<cloud-host>/ws/db` with ACE InnerTLS on top of the
  outer WSS (Heroku terminates the outer TLS at its router; the
  inner TLS handshake is the real trust boundary).
- Each binary frame carries one BSON-encoded `DbRequest` /
  `DbResponse`.
- Forwards every request to a local `MongodbClient` against
  `mongodb://pbx-mongo:27017/pabx` and writes back the response.
- Reconnects with exponential backoff on tunnel loss.

Standalone binary per xpmile's pattern — no embedded HTTP server,
no business logic, just transport + Mongo client.

## Tags

- `latest` — rolling main-branch build.
- `<sha>` — immutable per-commit tag.

Architectures: `linux/amd64`, `linux/arm64`.

Source: https://github.com/naushada/pbx — `docker/Dockerfile.wsdbagent`
