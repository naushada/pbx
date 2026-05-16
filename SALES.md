# onprem-pbx — a society intercom that lives on your network

**For:** residential-society management committees, builders, and IT
installers in India.
**What:** a self-hosted phone system where residents call each other by
flat number from any browser — no SIM card, no app to install, no
per-call cost.
**License:** MIT — use, fork, install for paying customers, embed in a
commercial offering. No royalty.

---

## At a glance

| | |
|---|---|
| **Who installs it** | A society IT vendor (one-time, ~half a day). |
| **Who runs it** | Society's admin via a browser UI. No SSH, no Linux skills required. |
| **What residents need** | A modern browser (Chrome, Safari, Edge, Firefox). That's it. |
| **What the society needs** | One small Linux box on the LAN + one UDP port-forward on the router. No phone lines. No SIM cards. No external SIP trunk. |
| **What it costs to run** | Cloud control plane: ~₹650/mo on a Heroku hobby dyno (or free with caveats). On-prem box: a Raspberry Pi 4 / NUC / repurposed laptop is enough. Voice traffic: **₹0** — calls stay on the society's LAN. |
| **What it replaces** | The wall-mounted intercom phones in every flat. The "press 6, then # to call security" muscle memory. The vendor's quarterly maintenance visits. |

---

## The pitch in three sentences

A resident in flat **A-101** opens their phone's browser, taps the
gate guard's tile, and the guard's web softphone rings on the
ground-floor terminal. The call's voice never leaves the society's
LAN — it goes browser → WiFi → on-prem box → guard's tile, and back.
A society admin onboards all 240 flats in one CSV upload from the
admin dashboard; no per-resident setup.

---

## What's good about it

### 1. Browser-native — zero install for residents

No App Store, no Play Store, no admin coordinating "did everyone
install the new version?" Residents bookmark
`https://your-society.onprem-pbx.in/` and that's the whole onboarding.
WebRTC + SIP-over-WebSocket means it works on iOS Safari, Android
Chrome, and any desktop browser.

### 2. **Privacy by network topology**

For 1:1 calls between two residents on the same WiFi/LAN, audio
**never leaves the building** — peer-to-peer through the on-prem
Asterisk box. No call data on a third-party server. No metadata
trail.

For off-LAN residents (someone calling from their office), audio
relays through the society's own coturn server, still without
touching a third-party media path.

### 3. **Flat-number dialing, not phone numbers**

Residents dial **`A-101`** to reach flat A-101. **`0`** for the
guard. **`*A-101`** to start a conference room. No memorising phone
numbers. No revealing personal mobile numbers to neighbours.

### 4. **One-touch security intercom**

Every flat reaches the main-gate guard with a single digit (`0`),
and the guard can dial any flat by its number. Works the way wall
intercoms have always worked, with a glass screen instead of a plastic
handset.

### 5. **Cloud-managed control plane**

The on-prem box is silent infrastructure. Day-to-day admin work
(onboarding new residents, disabling a moved-out subscriber,
listening to call detail records) happens in a Vaadin admin UI
hosted on Heroku — accessible from anywhere the admin happens to be.
Bulk CSV import does 240 flats in one click; downloadable template
shows the exact format.

### 6. **Open source, MIT-licensed**

Audit the entire codebase. Fork it. Modify it for your specific
society's quirks. Resell installation as a service. No vendor lock-in,
no per-seat licensing, no future "your enterprise tier just doubled in
price."

### 7. **Real security, not theatre**

- mTLS between the on-prem agent and the cloud (Heroku's TLS
  termination is augmented by a second TLS layer inside the
  WebSocket; the cloud verifies the agent's client cert against a
  per-deploy CA).
- DTLS-SRTP on every voice path (browser-to-browser, browser-to-
  Asterisk).
- TURN credentials are time-limited HMAC-SHA1, regenerated per call
  (RFC 5766 §5).
- Admin-role gate on every endpoint that creates / modifies society
  data.

### 8. **Self-contained footprint**

One Linux box. One UDP port forwarded from the society's router.
Five containers (Mongo, Asterisk, coturn, agent, db-tunnel agent),
brought up with one `podman-compose up -d` command. No PSTN gateway.
No SIP trunking contract. No public IP-address per-resident.

### 9. **Per-society multi-tenancy**

One Heroku deployment serves many societies. Each society's data,
SIP realm, and TURN secret are isolated cryptographically — a
Sunset-Towers admin literally cannot see Oaks-Apartments' subscriber
list.

### 10. **Web Push for incoming calls**

Calls ring the resident's phone even when the browser tab is
backgrounded, via VAPID Web Push + a Service Worker. Same UX as
WhatsApp call notifications, without the WhatsApp account.

---

## What it doesn't do (yet — be honest)

### 1. **No outside-line dialing (no PSTN)**

v1 is intra-society only. You can't call a Swiggy delivery person's
mobile from the intercom; you still need your phone for that. (A
PSTN gateway / SIP-trunk integration is plausible as a v2 — the
Asterisk side is ready, the cloud routing logic isn't.)

### 2. **No SMS / chat / video**

Voice calls + conference rooms only. No 1:1 text chat, no video
calls, no group broadcast. (WebRTC supports video — it's a feature
backlog item, not a fundamental limitation.)

### 3. **Single point of failure: the on-prem box**

If the society's on-prem box loses power, the LAN switch dies, or
the SSD wears out, the intercom stops working until it's fixed. No
high availability in v1; no automatic failover to a second box. (A
hot standby pattern is a reasonable next iteration.)

### 4. **Requires a competent installer**

The first-time install needs someone who can: edit a `.env` file, run
a `podman-compose up`, port-forward UDP on the society's router, and
download a cert family from the cloud build. Not a one-click consumer
install. (Plan: a `setup-society.sh` wizard exists and is being
extended; eventually an installer GUI.)

### 5. **Mongo runs without authentication**

The on-prem MongoDB is reachable only from the internal compose
bridge network, not exposed to the host or LAN. Security depends on
the network isolation, not on Mongo credentials. (Acceptable for v1
because the bridge is only 4 peers; can be hardened by adding
`--auth` + a single credential rotated by the installer.)

### 6. **Requires Heroku (or equivalent) for the cloud side**

The cloud control plane runs on Heroku in the default deploy. You
can host it on AWS / GCP / a self-hosted Kubernetes cluster — the
binary takes a port flag and that's the only platform-coupled bit —
but the deploy scripts assume Heroku today. (Moving the cloud
fully on-prem is doable: skip the cloud entirely and run the agent
talking to a local cloud container. The architecture supports it;
the docs don't walk through it.)

### 7. **Safari / iOS Web Push limitations**

iOS Safari only supports Web Push from a "saved-to-home-screen"
PWA, not a regular tab. iOS Safari users get the call experience
but won't get push when the tab isn't focused — they'd miss
incoming calls if the browser is fully closed. (Android Chrome,
desktop browsers all work fine.)

### 8. **Society needs a public IP + UDP port-forward**

For residents calling from off-LAN (someone at their office), the
society's router needs one UDP port (default 3478) DNAT'd to the
on-prem coturn container. Most ISPs in India give a dynamic IP — a
DDNS service (No-IP, Duck DNS) covers that, but it's one more
moving piece.

### 9. **Power outages = downtime**

Tied to point #3 but worth its own line: if your society loses
mains power and the on-prem box isn't on a UPS, the intercom is
down. A small UPS (₹2-3k) handles typical 4-hour outages.

### 10. **First-time install isn't 1-click**

`scripts/bootstrap-society.sh` plus a Heroku deploy plus the
on-prem `podman-compose up` is ~30 minutes for someone who's done
it before, longer for a first-timer. Not a "download installer, run
.exe" experience. (Roadmap: a single installer that does the whole
thing.)

---

## How it compares to alternatives

| | onprem-pbx | Traditional wall intercom | Cloud SaaS (e.g., generic VoIP provider) | WhatsApp / Telegram |
|---|---|---|---|---|
| **Install per flat** | None (browser) | Wall unit + wiring per flat | Per-resident app install + login | App install + phone number |
| **Per-call cost** | ₹0 (LAN media) | ₹0 (analog) | Per-minute or per-seat licensing | ₹0 (data charges only) |
| **Privacy** | Voice never leaves LAN | Voice never leaves building wiring | Voice traverses third-party servers | Metadata traverses third party |
| **Identifies by** | Flat number | Wired panel button | Phone number | Phone number |
| **Hardware** | One Linux box, society-owned | Per-flat panel + central exchange | Nothing on-prem | Personal phones |
| **Resident churn cost** | Edit one CSV row | Replace handset + rewire | Per-seat license churn | Add/remove WhatsApp groups manually |
| **Open source** | MIT, full source | Closed, vendor-locked | Closed, vendor-locked | Closed |
| **PSTN / outside calls** | Not in v1 | Sometimes (extra wiring) | Yes (paid) | Via personal account |

---

## What a fair deployment looks like (cost + effort)

**One-time:**
- Linux box (Raspberry Pi 4, mini PC, or repurposed laptop): **~₹5-15k**
- Small UPS for the box: **~₹2-3k**
- IT installer time for first society setup: **~half a day**
- Domain name + SSL cert for the admin UI (optional): **~₹500/yr**

**Monthly:**
- Heroku cloud hobby dyno: **~₹650/mo** (or ₹0 with free-tier caveats — dyno sleeps after 30 min idle)
- Domain renewal amortised: **~₹50/mo**
- Society's existing internet connection: already paid for; the
  intercom doesn't change that bill
- **Voice itself: ₹0**

**Compared to a typical commercial intercom system:**
- ~₹1,500-3,000 per flat in hardware (wall units, central exchange, wiring)
- ~₹500-1,000/mo per vendor support contract
- Recurring service contracts for AMC

For a 100-flat society, onprem-pbx vs. a commercial system:
- Year 1: onprem-pbx ~₹20k all-in vs. commercial ~₹2 lakh + AMC
- Year 2+: ~₹10k/yr vs. ~₹50k+/yr AMC

---

## Who shouldn't use it (yet)

- A society that needs PSTN dial-out (call ambulances directly from
  the intercom) — wait for v2, or pair with an existing landline.
- A society where the admin won't take any technical responsibility
  at all — you need someone willing to read the runbook and click
  through the Vaadin UI.
- A society that requires "no on-prem hardware" (e.g., a co-living
  building that already runs everything from cloud) — there's no
  cloud-only deployment shape in v1.

---

## Getting started

The runbook for the first society install is in the
[README](./README.md) under "Run the on-prem agent stack" and "Run the
Vaadin admin UI". Architecture in [DESIGN.md](./DESIGN.md), product
requirements in [PRD.md](./PRD.md), security posture in
[`docs/design/security/`](./docs/design/security/).

Questions, pilot interest, custom-installer ask: open a GitHub issue
or reach out via the repo's owner.
