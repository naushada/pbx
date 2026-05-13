# Media security (DTLS-SRTP)

The control-plane tunnel (`DESIGN.md` §7) carries only signalling. Voice is a separate, end-to-end concern between WebRTC peers (1:1) or between each browser and Asterisk (conference). The two shapes have meaningfully different trust properties — surfaced here so society admins can communicate them to subscribers honestly.

## Wire layout

| Layer | Protocol | Notes |
|---|---|---|
| Key exchange | **DTLS** (RFC 5764) | Browser-mandated for SRTP; the legacy SDES-keys-in-SDP option is removed from Chrome/Edge/Firefox/Safari. |
| Media | **SRTP** (RFC 3711) AES-GCM | Keys derived from the DTLS handshake. DTLS does not encapsulate RTP; only keying. |
| Control | **SRTCP** | Same key material as SRTP. |
| Port sharing | **rtcp-mux** (RFC 5761) | RTP + RTCP share a single UDP port → one ICE candidate carries both. |
| MitM defence | `a=fingerprint:sha-256 …` lines in SDP | DTLS handshake fails if the peer's cert doesn't match the SDP-published fingerprint. |

The `a=fingerprint` is the load-bearing piece. The cloud `SipBridge` is byte-faithful — it never parses SDP — but **even if it did** and rewrote the SDP, it would also have to substitute the fingerprint to match a cert it controls, and the receiving browser would refuse (the SDP path is signed... wait, it isn't). More accurately: an attacker who can both rewrite SDP **and** terminate DTLS handshakes on the path can MitM. We accept this — it's the same trust model every WebRTC service runs.

## Two media-path shapes

### 1:1 with `directmedia=yes` — truly end-to-end

```
Browser A ════════ DTLS handshake ════════ Browser B
       │             (direct via ICE)            │
       │                                         │
       └───────────── SRTP/SRTCP ────────────────┘
                       (P2P or TURN-relayed)
```

After SDP exchange, Asterisk emits a re-INVITE that swaps the media endpoints to the browsers' own ICE candidates. From that point:

- Asterisk relays SDP between the legs but **never holds the master secret**.
- The DTLS handshake is browser-to-browser, gated by the `a=fingerprint` lines in each SDP.
- Audio packets are SRTP from browser A to browser B (or A → TURN → B if both NATs are symmetric).
- The on-prem Asterisk process has **no plaintext audio access** for this call.

This is the WebRTC "trapezoid": signalling through a server, media direct between endpoints.

### Conference (ConfBridge) — each leg DTLS-handshakes with Asterisk

```
Browser A ──DTLS──┐
                  ├──→ Asterisk ConfBridge (plaintext mixer)
Browser B ──DTLS──┤      │
                  │      └──→ each output leg re-encrypted with its own SRTP key
Browser C ──DTLS──┘
```

There's no peer-to-peer equivalent that scales to N legs; a server-side mixer is the only path. The cost: Asterisk has plaintext audio **inside the mixer process only** while the bridge is live. This is an unavoidable property of every server-mixer architecture; the alternative (client-side SFU mesh) doesn't pay off for ≤5-party conferences on residents' devices.

**We document this asymmetry to society admins**: 1:1 audio is never visible to the PBX. Conference audio is encrypted on the wire but is decrypted, mixed, and re-encrypted inside the on-prem PBX. The PBX is on the society's own hardware, but it is a process and an admin compromise of that host = audio access during conferences.

## Asterisk configuration (`docker/asterisk/pjsip.conf`)

```
[endpoint-resident-template](!)
type=endpoint
transport=transport-ws
media_encryption=dtls
dtls_verify=fingerprint
dtls_setup=actpass
dtls_cert_file=/etc/asterisk/keys/pbx.crt
dtls_private_key=/etc/asterisk/keys/pbx.key
dtls_rekey=0
rtcp_mux=yes
ice_support=yes
direct_media=yes        ; flip to "no" for conference-only endpoints
webrtc=yes
auth_type=md5
realm=pbx.local
```

Key invariants:
- `media_encryption=dtls` is **mandatory**. Calls without DTLS fail at SDP negotiation. There is no plaintext-RTP fallback.
- `dtls_verify=fingerprint` enforces the SDP-fingerprint check on the Asterisk side (matters for conference legs and for early-media before 1:1 re-INVITE completes).
- `direct_media=yes` is what flips 1:1 calls into peer-to-peer mode. ConfBridge ignores this flag — bridge legs are always mediated.

The Asterisk DTLS cert (`/etc/asterisk/keys/pbx.{crt,key}`) is **separate** from the agent ↔ cloud mTLS cert. It's generated on first agent install, self-signed; only the SDP fingerprint is consulted at handshake. Rotation: regenerate on agent upgrades; existing calls are unaffected (cert is only used at handshake time).

For 1:1 directmedia, Asterisk's DTLS cert is on the SDP path but **not on the SRTP path** — the browsers handshake directly. The Asterisk cert matters only for conference legs and for the early-media window before re-INVITE swaps endpoints.

## What we explicitly don't do

- **No SDES.** Browsers refuse it; and it would leak keys to anyone observing SDP (cloud `SipBridge`, Heroku router, etc.).
- **No ZRTP.** Unsupported by browsers.
- **No plaintext-RTP fallback.** `media_encryption=dtls` is a hard requirement in `pjsip.conf`.
- **No SRTP master-key replication.** Keys are per-session, derived from DTLS, discarded at session end.

## ICE + TURN

A browser's ICE candidate list includes:
1. Host candidates (LAN IPs).
2. STUN-reflexive candidates (the browser's public IP as seen by coturn).
3. TURN relay candidates (coturn's public IP, used when both NATs are symmetric).

The browser fetches TURN credentials before opening the SIP-WS connection: `GET /api/v1/turn-credentials` returns time-limited `{ urls, username, credential }` (per RFC 5766 §5; see [`controls.md`](./controls.md)). Without this, ICE has no candidates and media setup fails before DTLS even starts.

coturn validates the credential via the shared `static-auth-secret` it knows (`docker/coturn/turnserver.conf`). The HMAC-SHA1 derivation is browser-side from the username (which has format `<unix_timestamp>:<sip_user>`), so coturn can recompute and compare without any per-user database.

## Code map

| File | Role |
|---|---|
| `docker/asterisk/pjsip.conf` | `media_encryption=dtls` + `dtls_verify=fingerprint` + `direct_media=yes` per RFC 5764 §5 |
| `docker/coturn/turnserver.conf` | `use-auth-secret` for time-limited TURN creds (RFC 5766 §5) |
| `MicroServicePbx::handle_turn_credentials` | Mints `{ unix_ts:sip_user, HMAC-SHA1(secret, username), 300s TTL }` for the browser |
| `ui/src/common/sip-ua-sipjs.ts` | `sessionDescriptionHandlerOptions.constraints = { audio: true }` — sip.js handles the DTLS-SRTP setup internally |
| `ui/src/app/call-panel/call-panel.component.ts` | Binds the remote SRTP-decoded `MediaStream` to a hidden `<audio>` element |
