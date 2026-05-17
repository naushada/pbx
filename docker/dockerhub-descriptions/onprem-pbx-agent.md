# onprem-pbx-agent

On-prem control-plane daemon for the [onprem-pbx project](https://github.com/naushada/pbx)
— a residential-society VoIP PBX with a cloud-hosted Heroku
control plane and a per-society on-prem Asterisk + Mongo + coturn
stack.

This image is **pulled by operators** on society-host machines as
part of the standard install. The recommended way to get a society
host running is **NOT** to `docker pull` this image directly —
use the one-shot installer instead:

```sh
sudo ./install.sh
# or via the cross-platform installer container:
docker run --rm -it \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -v ~/onprem-pbx-data:/data \
  -e HOST_DATA_DIR=$HOME/onprem-pbx-data \
  -e SOCIETY_CODE=… -e SOCIETY_NAME=… \
  -e ADMIN_EMAIL=… -e ADMIN_PASSWORD=… \
  -e CERTS_TARBALL=/data/…-certs.tar.gz \
  docker.io/naushada/onprem-pbx-installer:latest
```

The installer (or `install.sh`) handles config, certs, Mongo seeding,
and brings the full 6-container stack up via `docker-compose`.

## What this container does

- Dials `wss://<cloud-host>/agent` and maintains an InnerTLS tunnel to
  the cloud's `CloudTunnelEndpoint`.
- Receives `OPEN`/`DATA`/`CLOSE` SipFrames from the cloud and forwards
  each to a per-browser-stream socket against Asterisk's
  `chan_pjsip` `transport-ws`.
- Drives Asterisk admission control + CDR finalisation via ARI events.
- Provisions PJSIP endpoints/aors/auths dynamically per subscriber
  via ARI sorcery (`PjsipProvisioner`).
- Tails Mongo subscriber change-streams to keep Asterisk's config in
  sync (`SubscriberWatcher`).

## Tags

- `latest` — rolling main-branch build.
- `<sha>` — immutable per-commit tag.

Architectures: `linux/amd64`, `linux/arm64`.

Source: https://github.com/naushada/pbx — `docker/Dockerfile.agent`
