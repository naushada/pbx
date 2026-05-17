# Installing onprem-pbx on a Windows machine

This is the Windows install guide. For Linux (Ubuntu / Pi /
NUC / typical society hardware), use [`INSTALL.md`](./INSTALL.md)
instead.

> **Read this first:** Windows is **not the recommended runtime
> target** for a society PBX. SIP signalling will probably work; the
> UDP audio path (RTP) and TURN server frequently won't, because
> Docker Desktop on Windows runs containers inside a hidden VM
> (WSL2 or Hyper-V) whose network is NAT'd from the Windows host.
> For real-world deployments, run on a Raspberry Pi or any small
> Linux box on the building's LAN. Windows users (residents and
> admins) just use the browser softphone / admin UI from their
> Windows machines — those work everywhere.
>
> The Windows path documented here is for **testing / dev parity
> only**, not for production calls.

## Two install paths

| Path | Status | UX |
|---|---|---|
| **A — WSL2 + Linux installer** | ✅ Works today | Open Ubuntu in WSL2, run `sudo ./install.sh`. Same script as the Linux path. |
| **B — Cross-platform installer container** | ✅ Shipped | Single `docker run` from PowerShell. Bakes all scripts into the image; talks to the host's Docker daemon via the mounted socket. |

Either path produces the same running stack on the host. Path B is
simpler — no WSL2 install, no Linux distro setup, no bash on the
host. Use B unless you specifically want a WSL2 dev environment for
poking the on-prem stack from the shell.

---

## Path A — WSL2 + install.sh (works today)

### What you need

| Item | Where it comes from |
|---|---|
| Windows 10 21H2+ or Windows 11 | Microsoft |
| WSL2 + Ubuntu 22.04 (or 24.04) distribution | `wsl --install -d Ubuntu` from elevated PowerShell |
| Docker Desktop for Windows with WSL2 backend enabled | https://www.docker.com/products/docker-desktop |
| The onprem-pbx repo (USB / git clone / scp) | Your dev team |
| Per-society cert tarball (`<SOCIETY>-certs.tar.gz`) | Your dev team (see below) |
| Society code your dev team assigned you | Your dev team |

### Get the cert tarball (your dev team does this)

On the dev machine, after a successful Heroku deploy:

```sh
./deploy-heroku.sh package-society-certs SUNSET
# → /tmp/SUNSET-certs.tar.gz  (ship to the society)
```

### Steps

1. **Enable WSL2 + install Ubuntu** (one-time, from elevated PowerShell):

   ```powershell
   wsl --install -d Ubuntu
   ```

   Reboot when prompted. Set up a Unix username + password when Ubuntu
   first launches.

2. **Install Docker Desktop**:
   - Download from https://www.docker.com/products/docker-desktop
   - During install, **enable "Use the WSL 2 based engine"**.
   - In **Settings → Resources → WSL Integration**, toggle on
     **Ubuntu** so `docker` works inside the WSL distro.

3. **Open Ubuntu (WSL2)** and verify Docker is reachable:

   ```sh
   docker version
   # Expect Client + Server both shown. If "Cannot connect to the
   # Docker daemon", Docker Desktop isn't running OR WSL Integration
   # is off.
   ```

4. **Copy the repo + cert tarball into WSL2**. Easiest:

   ```powershell
   # In PowerShell — push the repo + tarball into the Ubuntu home dir
   wsl --cd ~ -- bash -c "mkdir -p ~/onprem-pbx"
   # Then copy via Windows Explorer to \\wsl$\Ubuntu\home\<you>\onprem-pbx\
   # Or from PowerShell:
   cp C:\path\to\onprem-pbx-repo\* \\wsl$\Ubuntu\home\<you>\onprem-pbx\ -Recurse
   cp C:\Downloads\SUNSET-certs.tar.gz \\wsl$\Ubuntu\home\<you>\
   ```

   Or if you already have `git` configured in WSL2:

   ```sh
   # Inside Ubuntu WSL2:
   git clone https://github.com/naushada/pbx ~/onprem-pbx
   ```

5. **Run the installer inside Ubuntu WSL2**:

   ```sh
   cd ~/onprem-pbx
   sudo ./install.sh
   ```

   ⚠️ **Caveat:** `install.sh` installs podman + podman-compose by
   default. If Docker Desktop is your container runtime, you may
   need to either:
   - Install podman-compose anyway (it'll talk to Docker Desktop's
     daemon via a socket-shim — works, just not the default), OR
   - Wait for the installer container path (Task #27) which uses
     `docker` directly.

6. **Answer the 6 prompts** (same as Linux):
   - Society code (e.g. `SUNSET`)
   - Cloud hostname (Enter for default)
   - Path to the cert tarball (the one you copied into WSL2)
   - Society's full name
   - Admin email
   - Admin password (silent input)

7. **Wait ~3-5 min** for image pulls + boot. Then the green
   summary appears.

### Auto-restart after Windows reboot

systemd inside WSL2 needs to be enabled and Docker Desktop needs to
auto-start with Windows. Two steps:

```sh
# Inside Ubuntu WSL2 — enable systemd (one-time):
sudo tee /etc/wsl.conf > /dev/null <<EOF
[boot]
systemd=true
EOF
# Then from PowerShell:
#   wsl --shutdown
#   wsl
# (Restarts the WSL distro with systemd active.)
```

In Windows: **Docker Desktop → Settings → General → "Start Docker
Desktop when you log in"**. WSL2 distros auto-resume when accessed
after that.

The containers themselves use `restart: unless-stopped` so they
come back up the moment Docker Desktop is running and WSL2's
systemd starts the onprem-pbx service.

---

## Path B — Installer container (recommended)

A single `docker run` brings the whole on-prem stack up. The installer
image bakes the scripts + compose file + per-service config sources
into a Debian-slim container; it talks to the host's Docker daemon
via the mounted socket and creates containers on the HOST (not
nested — this is "Docker-out-of-Docker" / DooD).

### Pre-flight (one-time)

1. Install **Docker Desktop for Windows** from https://www.docker.com/products/docker-desktop. Enable the WSL2 backend during install (Docker Desktop uses it internally even though you won't touch WSL2 yourself for Path B).
2. Get the **cert tarball** from your dev team (see Path A's section above).
3. Pick a host directory for the installer to write into, e.g. `C:\Users\<you>\onprem-pbx-data`. Drop the cert tarball into it.

### Run the installer

In PowerShell (NOT WSL2 — straight Windows):

```powershell
# Create the data dir + drop the cert tarball into it first:
New-Item -ItemType Directory -Force -Path "$HOME\onprem-pbx-data" | Out-Null
Copy-Item C:\Downloads\SUNSET-certs.tar.gz "$HOME\onprem-pbx-data\"

# Run the installer container:
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

On **Linux** the equivalent (paths use `/`):

```sh
mkdir -p ~/onprem-pbx-data
cp ~/Downloads/SUNSET-certs.tar.gz ~/onprem-pbx-data/

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

On **macOS** with Docker Desktop, the Linux command works as-is (paths just need to be macOS-style, `/Users/<you>/...`).

### What the installer does inside the container

1. Validates env vars.
2. Pings the host docker daemon (sanity check the socket mount).
3. Unpacks the cert tarball into `$HOST_DATA_DIR/certs/cloud-issued/`.
4. Generates Asterisk DTLS + coturn config into `$HOST_DATA_DIR/certs/` and `$HOST_DATA_DIR/turnserver.conf`.
5. Writes `$HOST_DATA_DIR/.env` with `HOST_DATA_DIR`-prefixed absolute paths for the compose bind-mounts (so the host's docker daemon resolves them correctly).
6. `docker compose pull` + `docker compose up -d` via the host daemon.
7. Waits for Mongo's replica set, runs `docker exec pbx-mongo mongosh` to upsert the society + ADMIN subscriber.
8. Prints a summary with the host-side commands the operator can use later.

### After install

```powershell
docker ps --filter name=pbx-
docker logs -f pbx-agent
```

Auto-restart on host reboot is handled by:
- The compose's `restart: unless-stopped` policy on every container.
- Docker Desktop's **"Start Docker Desktop when you log in"** setting (Settings → General → toggle on).

⚠️ The Windows runtime limitations from Path A apply equally to Path B — the installer container can't fix WSL2's UDP NAT for TURN/RTP. See "Known runtime limitations" below.

---

## Known runtime limitations on Windows + Docker Desktop

These apply to **both** install paths because they're properties of
Docker Desktop's networking, not the installer.

| Limitation | Why | Workaround |
|---|---|---|
| **TURN (coturn) UDP relay doesn't reach external clients** | `network_mode: host` inside Docker Desktop = the WSL2/HyperV VM's network, not the Windows host. UDP forwarding from Windows → VM is unreliable. | Add explicit Windows port-forwards via `netsh portproxy` for the TURN port range. Painful for the UDP range (~10,000-20,000). |
| **Asterisk RTP audio path drops packets on incoming SIP calls** | Same WSL2 NAT problem — incoming UDP RTP from external softphones gets lost between Windows host and the WSL2 VM. | Use only browser softphones connecting via the cloud (which terminates the WebRTC media); avoid external SIP softphones. |
| **No systemd on the Windows host** | systemd is a Linux init system. | `restart: unless-stopped` policy on containers + Docker Desktop auto-start at Windows login. Less robust than systemd on a real Linux box. |
| **WSL2 VM eats ~2-4 GB RAM** | WSL2 runs a Linux VM in the background. | Cap WSL2's memory with a `.wslconfig` in `%USERPROFILE%`: `[wsl2]\nmemory=4GB` |
| **Docker Desktop license** | Docker Desktop is paid for companies > 250 employees or > $10M revenue (as of 2024). | Smaller societies are fine. Larger orgs should buy a license OR switch to a Linux PBX appliance. |

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `wsl: command not found` in PowerShell | Windows 10 < 21H2 doesn't have WSL2. Upgrade Windows, OR use Hyper-V Manager to run an Ubuntu VM manually. |
| `Cannot connect to the Docker daemon` inside WSL2 | Docker Desktop isn't running, OR **Settings → Resources → WSL Integration → Ubuntu** isn't toggled on. |
| `podman: command not found` inside WSL2 | `install.sh` will install it. If apt fails to fetch, check WSL2's DNS: `cat /etc/resolv.conf` should have a working nameserver. |
| Browser softphone says "Cannot reach server" | The cloud is up but your residents' browsers can't reach `https://pabx-…herokuapp.com`. Check the building's firewall isn't blocking 443. |
| Outgoing SIP works, incoming calls drop | Classic WSL2 UDP NAT problem. Either run on a real Linux box OR add Windows port-forwards (painful). |

---

## When to stop trying Windows and use Linux

If you're running into ANY of the runtime limitations above, the time
to switch is now. A Raspberry Pi 4 (8 GB RAM) is ₹5000-7000, runs
this stack natively without any of the WSL2 networking pain, and
gives you a dedicated appliance that's easier to support. The
operator at the society doesn't need to touch the Pi after install
— it just sits in a corner with a network cable.

The Windows path documented here exists because some buildings have
a Windows server already running other things and don't want a
second box. That's a legitimate constraint, but you'll be debugging
WSL2 NAT every time a resident reports a call drop.
