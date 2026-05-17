# Installing onprem-pbx on a society machine

This is the lay-operator install guide for **Linux** hosts (Ubuntu
22/24 — Raspberry Pi, NUC, mini-PC, generic server). For **Windows
hosts** (Docker Desktop + WSL2), see [`INSTALL-windows.md`](./INSTALL-windows.md)
— though Linux is the recommended runtime target for any real PBX
deployment.

If you're a developer, you're probably looking for
[`README.md`](README.md) or [`ARCHITECTURE.md`](ARCHITECTURE.md).

## What you need

- A Linux box (Ubuntu 22.04 or 24.04, amd64 or arm64) physically at
  the society / building.
- At least **5 GB** of free disk space and **3 GB** of RAM.
- Internet access — the box dials the cloud over TCP 443.
- The **cert tarball** your dev team sent you (a `.tar.gz` file with
  the per-society TLS material).
- The **society code** your dev team gave you (a short label, e.g.
  `SUNSET`).

## What to do

1. Copy the onprem-pbx folder onto the machine (USB stick, scp, git
   clone — whatever is easiest).
2. Open a terminal, `cd` into that folder.
3. Run the installer:

   ```sh
   sudo ./install.sh
   ```

4. Answer three prompts:
   - **Society code** — e.g. `SUNSET`.
   - **Cloud hostname** — press Enter to accept the default (the
     production deployment).
   - **Cert tarball path** — the path to the `.tar.gz` file your dev
     team sent you (e.g. `/tmp/SUNSET-certs.tar.gz`).
5. Wait. The installer pulls pre-built container images from Docker
   Hub (~3–5 minutes on a typical connection). Subsequent runs finish
   in seconds. **Don't close the terminal.**
6. When the installer prints the green box, the stack is up and will
   restart automatically when the machine reboots.

## How to check things later

```sh
sudo systemctl status onprem-pbx           # is the service up?
sudo podman ps --filter name=pbx-          # which containers are running?
sudo podman logs -f pbx-agent              # follow the agent's log
sudo journalctl -u onprem-pbx -f           # follow the systemd unit
```

## Re-running the installer

Safe — every step is idempotent. Re-run after a new cert tarball
arrives, or after you upgrade the repo.

## Stopping / starting manually

```sh
sudo systemctl stop  onprem-pbx
sudo systemctl start onprem-pbx
```

## Uninstall

```sh
sudo systemctl disable --now onprem-pbx
sudo podman-compose -f /opt/onprem-pbx/docker-compose.agent.yml down -v
sudo rm -rf /opt/onprem-pbx
sudo rm /etc/systemd/system/onprem-pbx.service
```

## For your dev team — generating a cert tarball

Run on your dev machine after `./deploy-heroku.sh deploy`:

```sh
./deploy-heroku.sh package-society-certs SUNSET
# → /tmp/SUNSET-certs.tar.gz
# ship it to the society (scp / USB / email), then run install.sh there.
```

## Refreshing certs after a cloud redeploy

**Every `./deploy-heroku.sh deploy` on the dev side mints a fresh CA
and a fresh per-build client cert family.** The cloud trusts only its
current CA's certs, so a society running yesterday's certs will fail
the inner-TLS handshake until it picks up the new ones.

Here's how to land fresh certs on a society machine after the cloud
has been redeployed:

1. **Dev** — re-run the packaging command (it reads from
   `certs/cloud-issued/`, which `deploy-heroku.sh deploy` just
   refreshed):

   ```sh
   ./deploy-heroku.sh package-society-certs SUNSET
   ```

2. **Ship** the new `/tmp/SUNSET-certs.tar.gz` to the society
   machine — scp, email, USB stick.

3. **Society** — re-run the installer (it's safe to re-run; only
   the certs change):

   ```sh
   sudo CERTS_TARBALL=/path/to/new/SUNSET-certs.tar.gz ./install.sh
   ```

   The installer unpacks the tarball into
   `/opt/onprem-pbx/certs/cloud-issued/`. The on-prem
   **`pbx-cert-watcher`** container detects the file md5 change within
   5 s and POSTs to the host podman socket to restart `pbx-agent` +
   `pbx-wsdbagent` automatically — no manual `systemctl restart`.

> ⚠️ **Gap:** today there's no auto-channel between the dev's cloud
> deploy and the society machine. The cert tarball ride-along is
> manual. A future enhancement: a society-facing pull endpoint
> protected by a one-time installer token so the society can fetch
> its current certs directly from the cloud. Until that lands, plan
> a quick "ship + reinstall" pass after every cloud deploy that
> changes the CA.

## Troubleshooting

| Symptom                                                | Likely cause / fix                                                                              |
|--------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `Run me with sudo`                                     | Add `sudo` in front: `sudo ./install.sh`.                                                       |
| `Only N GB free on /` (where N < 5)                    | Free up disk first: `sudo apt-get autoremove --purge` / `sudo journalctl --vacuum-size=200M`.   |
| `Couldn't resolve pabx-…herokuapp.com`                 | DNS or internet is broken on this machine. Check `ping 8.8.8.8` then `nslookup`.                |
| `Can't find the cert tarball`                          | Mistyped path. Use tab-completion in the prompt.                                                |
| pbx-agent doesn't show `inner-TLS handshake`           | Cert tarball is wrong society OR is stale. Get a fresh one from the dev team.                   |
| `podman-compose pull failed` in step 8                 | Internet to Docker Hub is blocked. Check `curl -I https://docker.io` then retry the installer.  |

If something else goes wrong, run this and email the output to your
dev team:

```sh
sudo podman logs --tail 100 pbx-agent     > /tmp/pbx-agent.log
sudo podman logs --tail 100 pbx-wsdbagent > /tmp/pbx-wsdbagent.log
sudo systemctl status onprem-pbx          > /tmp/pbx-systemd.log
```
