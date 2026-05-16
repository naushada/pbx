# Restart survival — on-prem stack

## What we want

After a host reboot (planned maintenance, power blip, kernel upgrade,
society admin pulls the plug), the full on-prem stack — Mongo,
Asterisk, coturn, pbx-agent, pbx-wsdbagent — must come back up on its
own. No one should have to SSH in and run `podman-compose up -d`.

## Two layers of restart logic

Each layer is necessary but not sufficient:

| Layer | Handles | Doesn't handle |
|-------|---------|----------------|
| Compose `restart: unless-stopped` | A single container crashing | Host reboot — nothing starts podman |
| systemd unit `onprem-pbx.service`  | Host reboot — runs compose-up at boot | Per-container crashes |

The compose policy is already set on every service in
`docker-compose.agent.yml`. The systemd unit is what we add here.

## Install (one-time, per host)

On the on-prem Linux box that runs the stack:

```bash
sudo scripts/install-systemd.sh
```

The script:
1. Refuses to run on non-Linux / non-systemd hosts.
2. Rsyncs the repo to `/opt/onprem-pbx/` (or whatever `INSTALL_DIR=` you set).
3. Drops `systemd/onprem-pbx.service` into `/etc/systemd/system/`.
4. `daemon-reload`, `enable`, `start`.
5. Prints `systemctl status` so you immediately see green/red.

You can put the install dir elsewhere:

```bash
sudo INSTALL_DIR=/srv/onprem-pbx scripts/install-systemd.sh
```

The installer rewrites the unit's `WorkingDirectory` + `EnvironmentFile`
to match.

## What needs to be in place before install

- `podman` and `podman-compose` on the box.
- The repo cloned (or copied) somewhere readable.
- `.env` populated at the install location:
  ```
  CLOUD_HOST=pabx-5fbf3550f938.herokuapp.com
  AGENT_SOCIETY_ID=<your society slug>
  CERTS_DIR=/opt/onprem-pbx/certs/agent-deployed
  ```
- Per-society setup already done: `scripts/setup-society.sh` (DTLS
  cert + coturn config + static-auth-secret).
- The cloud image(s) the compose file references are buildable on the
  host (`podman build` will run from `WorkingDirectory` if a fresh
  image isn't cached).

If `.env` is missing the installer warns but still installs the unit.
First boot will fail with podman-compose complaining about
unset required variables — fix `.env`, then `sudo systemctl restart
onprem-pbx.service`.

## Verifying restart survival

After install:

```bash
sudo systemctl status onprem-pbx.service       # active (exited)
podman ps                                       # all six containers Up

# Force a real reboot, then after the box is back:
sudo systemctl status onprem-pbx.service       # active (exited)
podman ps                                       # all six containers Up again
```

If `podman ps` is empty after boot, the journal is the place to look:

```bash
sudo journalctl -u onprem-pbx.service -b
```

Common first-boot failures:

| Symptom in journal                        | Likely cause |
|-------------------------------------------|--------------|
| `podman-compose: command not found`        | Not installed for root's PATH; `apt install podman-compose` or `pip3 install --upgrade --break-system-packages podman-compose`. |
| `CLOUD_HOST required`                      | `.env` not at the right path; check the unit's `EnvironmentFile`. |
| `Error: short-name "mongo:7" did not resolve` | Set `unqualified-search-registries = ["docker.io"]` in `/etc/containers/registries.conf`. |
| Stack starts but agent can't dial cloud    | DNS not ready when service ran; the unit waits for `network-online.target`, but on some distros that's flaky. Add `ExecStartPre=/bin/sleep 5` as a workaround. |

## Uninstall / pause

```bash
sudo systemctl disable --now onprem-pbx.service     # stop + remove from boot
sudo rm /etc/systemd/system/onprem-pbx.service      # forget the unit
sudo systemctl daemon-reload
```

## Rootful vs rootless podman

The shipped unit runs as root. That's the simplest reliable option for
a dedicated society box. If you must run rootless:

1. Move the unit to the user scope:
   `~/.config/systemd/user/onprem-pbx.service`
2. Drop `WantedBy=multi-user.target`, use `default.target`.
3. Enable user services to start at boot:
   `sudo loginctl enable-linger <user>`
4. `systemctl --user daemon-reload && systemctl --user enable --now onprem-pbx.service`

Rootless adds a real chunk of surface area (lingering, user namespaces,
networking quirks for coturn's `host` mode). For an on-prem box where
podman is the only thing running, rootful is the default we recommend.

## Dev-box note (macOS)

This whole document is Linux-only. On macOS the on-prem stack runs
inside a podman-machine VM; that VM has its own boot story (it comes
back when you `podman machine start`, which you typically do once per
login session). Restart survival on macOS dev boxes is not a goal here.
