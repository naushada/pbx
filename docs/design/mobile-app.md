# Mobile App (iOS & Android) — Design

A native mobile client for residents, alongside the existing Angular web
softphone. A resident installs the app, logs in (or self-registers) with
their society and flat, types a destination flat number, and calls it.

This document is a **design proposal** — no mobile code exists yet. It
covers scope, architecture, the screens, the API surface, calling and
push, and the security trade-offs of the chosen decisions.

**Decisions locked for v1** (see §11 for the rationale and alternatives):

- **Stack:** React Native (one TypeScript codebase for iOS + Android).
- **Registration:** instant / ungated — a self-registered account is
  immediately usable. This is a **demo/early-adopter posture**; §9
  spells out the impersonation risk and the hardening path.

---

## 1. Goals & non-goals

### Goals (v1)

- **Login** — Society, Flat Number, Password → authenticated session.
- **Self-registration** — create an account from the device: Society,
  Flat Number, Resident Name, optional Mobile, optional Email, Password.
- **Place a call** — type a destination flat number, press **Call**.
- **Receive a call** — a native incoming-call screen rings the device
  even when the app is backgrounded or closed. (Not in the original
  ask, but an intercom that can't *receive* calls is not usable — the
  flat you call has to be able to answer.)
- **One codebase** shipped to the Apple App Store and Google Play.

### Non-goals (v1)

Directory / presence browsing, society conference, call history,
in-app admin, call transfer/hold, video. All are candidates for later
versions — none block the core "call a flat" loop.

---

## 2. Where it fits

The mobile app is **just another client of the existing cloud control
plane** — the same REST API and the same SIP-over-WebSocket tunnel the
web softphone already uses. No change to the on-prem stack.

```
   ┌─────────────────┐         ┌─────────────────┐
   │  Web softphone   │        │   Mobile app    │   ← NEW
   │  (Angular SPA)   │        │ (React Native)  │
   └────────┬────────┘         └────────┬────────┘
            │  HTTPS REST + wss /sip-ws  │
            └─────────────┬──────────────┘
                          ▼
              ┌───────────────────────┐
              │  Cloud control plane  │   (Heroku)
              │  REST API · /sip-ws   │
              │  push fan-out         │
              └───────────┬───────────┘
                          │  wss /agent (mTLS tunnel)
                          ▼
              ┌───────────────────────┐
              │  On-prem society stack │
              │  pbx-agent · Asterisk  │
              │  coturn · MongoDB      │
              └───────────────────────┘
```

The mobile app changes **nothing** on-prem. A self-registered subscriber
lands in MongoDB exactly like a CSV-imported one; the agent's
`SubscriberWatcher` change-stream picks up the new row and
`PjsipProvisioner` materialises the Asterisk endpoint within ~200 ms
(see [`side-car-watcher.md`](./side-car-watcher.md) for the watcher
mechanics). No new on-prem code.

The new work is **client app + a small amount of cloud REST**
(registration endpoint, society-name resolution, mobile push fan-out).

### 2.1 Connectivity — the app never contacts the agent directly

A common misread: "the mobile app connects to the on-prem agent." It
does **not**. The on-prem stack has **no inbound ports** — `pbx-agent`
*dials out* to the cloud. The mobile app talks **only to the cloud**,
and the cloud relays over the tunnel the agent already opened.

There are two independent planes:

**Signalling (SIP — call setup)** — mobile → cloud → agent → Asterisk:

```
 mobile ──wss /sip-ws?token=──► cloud SipBridge ──┐
                                                  │ frames onto the
 pbx-agent ──wss /agent (mTLS, dialled OUT)──► cloud CloudTunnelEndpoint
                                                  │
 cloud relays the SIP frames down that tunnel ────┘
        └─► pbx-agent SipFrameDemux ──► ws :8088 ──► Asterisk
```

The agent is reached **indirectly** — the cloud multiplexes each
`/sip-ws` session onto the agent's own pre-existing outbound `/agent`
tunnel. The app never opens a connection to the agent, and never needs
to be on the society LAN.

**Media (RTP — the audio)** — a *separate* path that does **not** cross
the tunnel and does **not** involve the agent at all:

```
 mobile ◄──ICE / DTLS-SRTP──► coturn ◄────► Asterisk
 (react-native-webrtc)       (on-prem; the one      (on-prem)
                              public UDP port)
```

WebRTC negotiates a media session between the device and Asterisk
directly; because the device is remote and Asterisk is on a private
LAN, ICE relays the audio through **coturn** — the single on-prem
component with a public UDP port. `pbx-agent` is not in the media
path.

Net effect: one public UDP port for coturn is the *only* inbound
exposure a society needs; everything else is outbound. The app works
from any network — mobile data, any Wi-Fi — exactly like the web
softphone.

---

## 3. Tech stack

| Concern | Choice |
|---|---|
| App framework | **React Native** (TypeScript) |
| Media | `react-native-webrtc` — `RTCPeerConnection`, `getUserMedia`, DTLS-SRTP |
| SIP signalling | `sip.js` over React Native's `WebSocket`, with a custom `SessionDescriptionHandler` bridging to `react-native-webrtc` |
| Native call UI | `react-native-callkeep` — CallKit (iOS) + ConnectionService (Android) |
| Push | APNs **PushKit** (iOS VoIP pushes) + **FCM** high-priority data messages (Android) |
| Secure storage | iOS Keychain / Android Keystore (via `react-native-keychain`) |
| State / nav | React Navigation; lightweight store (Zustand or Context) |

**Why React Native:** the web softphone's calling layer is `sip.js`
in TypeScript (`shared/sip-ua/sip-ua-sipjs.ts`, consumed by both
`ui/src/common/sip.service.ts` and `mobile/src/sip/sipCallController.ts`).
React Native keeps the SIP/session logic in the same language and lets
us share the hard-won call-flow code (registration, INVITE, first-
answer handling) rather than re-implementing it twice in Swift +
Kotlin. Alternatives (Flutter, fully-native) are weighed in §11.

The one part that does **not** port for free is the media engine: the
browser gives `sip.js` a `RTCPeerConnection` for free; React Native
needs `react-native-webrtc` and a custom `SessionDescriptionHandler`
that feeds it. That adapter is the main net-new calling code.

---

## 4. Screens & flows

Three screens for v1, plus the system incoming-call UI.

### 4.1 Login

```
┌──────────────────────────────┐
│        Society Softphone     │
│                              │
│  Society      [____________] │
│  Flat number  [____________] │
│  Password     [____________] │
│                              │
│        [   Log in   ]        │
│                              │
│  New here?  Create account → │
└──────────────────────────────┘
```

- **Society** is the society's short code/name (e.g. `SUNSET`). The
  cloud resolves it to a `societyId` — see §5.1.
- On success the cloud returns `{ token, subscriber }`. The token goes
  into the Keychain/Keystore; `subscriber` (flat, sipUsername, role)
  is cached for the session.
- The app then opens the SIP tunnel (§6) and the user lands on Dial.

### 4.2 Create account (self-registration)

```
┌──────────────────────────────┐
│        Create account        │
│                              │
│  Society *      [__________] │
│  Flat number *  [__________] │
│  Resident name* [__________] │
│  Mobile number  [__________] │  (optional)
│  Email          [__________] │  (optional)
│  Password *     [__________] │
│                              │
│       [  Create account  ]   │
└──────────────────────────────┘
```

- `*` = required: Society, Flat Number, Resident Name, Password.
  Mobile and Email are optional.
- On submit the app calls the registration endpoint (§5.2). With
  ungated registration the account is **active immediately**; the app
  auto-logs-in with the returned token and goes straight to Dial.
- Client-side validation only blocks the obvious (empty required
  fields, password length, email shape). All authoritative checks are
  server-side.

### 4.3 Dial & call

```
┌──────────────────────────────┐
│  A-101 · Resident A101    ⚙  │
│                              │
│   Call a flat                │
│   ┌────────────────────────┐ │
│   │  destination flat #    │ │
│   └────────────────────────┘ │
│         [    Call    ]       │
│                              │
│   ● Connected to PBX         │
└──────────────────────────────┘
```

- The user types a destination flat number and presses **Call**.
- The app places a SIP INVITE for that flat over the tunnel; the
  on-prem `CallRouter` forked-rings every active subscriber on the
  destination flat (first-answer-wins). The mobile caller is then
  bridged and `CallRouter` answers the caller leg — same path the web
  softphone uses (see PRs #120/#121 in `RELEASE-NOTES.md`).
- An **in-call screen** (timer, mute, hang-up) is presented; on iOS/
  Android it is the system CallKit/ConnectionService call UI.

### 4.4 Incoming call

When another flat calls this resident, the device shows the **native
incoming-call screen** (full-screen, rings, works from background/
locked) via CallKit / ConnectionService. Accept → media connects;
decline → SIP `486`/`603`. Flow detail in §7.

---

## 5. Cloud API

The mobile app reuses the existing REST surface where possible. Three
touch-points need cloud work.

### 5.1 Login — `POST /api/v1/subscriber/login`

Reuse the existing endpoint. Today it authenticates a resident and
returns `{ token, subscriber }`. Two adjustments:

- The form collects a **Society name/code**, but the cloud keys on
  `societyId`. Either (a) extend `/subscriber/login` to accept
  `societyName` and resolve it against the `societies` collection, or
  (b) add `GET /api/v1/societies/resolve?name=…` and resolve client-
  side first. **(a)** is fewer round-trips and is the recommendation.
- The returned `subscriber` must include `sipUsername`, `flatNumber`,
  and `role` — the app needs all three to register SIP and render Dial.

### 5.2 Registration — `POST /api/v1/subscriber/register`  *(new)*

```jsonc
// request
{
  "societyName": "SUNSET",
  "flatNumber":  "A-101",
  "residentName":"Asha Rao",
  "mobile":      "+91…",      // optional
  "email":       "asha@…",     // optional
  "password":    "……"
}
// response 201
{ "token": "…", "subscriber": { "societyId","flatNumber",
                                "sipUsername","displayName","role" } }
```

Server-side the cloud:

1. Resolves `societyName` → `societyId`; `404` if no such society
   (residents cannot invent a society — only an existing one).
2. Generates a unique, SIP-safe `sipUsername`. SIP user-parts allow
   `-_.`; the **realm/host** must not contain underscores (see PR #76)
   — `sipUsername` is the user-part so it is unconstrained beyond
   SIP-safety. Suggested shape: `<societycode>-<flat>-<rand4>`,
   lower-cased, non-alphanumerics collapsed to `-`.
3. Hashes the password with the cloud's existing subscriber password
   scheme and writes a `subscribers` document with the denormalised
   `flatNumber` (the field `CallRouter` and the directory key on).
4. Returns a session token (auto-login) so the app skips a second
   login.

Multiple residents may register on the same flat — each is its own
`subscribers` row with its own `sipUsername`; a call to that flat
forked-rings all of them. That is already how `CallRouter` resolves a
dialed flat, so no routing change.

**Provisioning is automatic:** the new row triggers the agent's Mongo
change-stream → `PjsipProvisioner` PUTs the Asterisk endpoint. The
resident can place/receive calls within ~200 ms of registering, with
no admin action and no agent restart.

### 5.3 Mobile push registration — `POST /api/v1/push/device`  *(new)*

The web app uses VAPID Web Push. Mobile needs native push so a call
can ring a backgrounded/terminated app:

- The app obtains an APNs (iOS, PushKit VoIP) or FCM (Android) token
  and registers it: `{ sipUsername, platform, token }`.
- The cloud stores it next to the existing `push_subscriptions` data
  and fans out call-wake-up pushes to APNs/FCM (a sibling of the
  existing `PushSender`). See §7.

---

## 6. Calling — SIP & media

After login the app holds a session token and connects the tunnel,
identically to the web softphone:

```
app ── wss://<cloud>/sip-ws?token=… ──► cloud BrowserStream
         │  (SIP-over-WebSocket frames; cloud bearer-token is the
         │   sole SIP auth layer — no SIP digest, see project notes)
         ▼
   cloud SipBridge ── wss /agent tunnel ──► pbx-agent ──► Asterisk
```

- **Signalling:** `sip.js` runs over React Native's `WebSocket`. The
  app sends `REGISTER`, then `INVITE sip:<flat>@pbx.<society>` to
  place a call.
- **Media:** the custom `SessionDescriptionHandler` drives
  `react-native-webrtc` — Opus audio, DTLS-SRTP, ICE. coturn supplies
  STUN/TURN for residents off the society LAN (mobile data, other
  Wi-Fi). v1 is **audio only**.
- **Codec:** offer Opus (the Asterisk image has no Opus transcoder, so
  every leg must be Opus — see `RELEASE-NOTES.md` / PR #120). The app's
  SDP must list Opus; G.711 as a fallback only.

No on-prem change: the mobile leg is just another browser-shaped SIP-
over-WS endpoint to `CallRouter`.

---

## 7. Push & incoming calls

A mobile intercom must ring when the app is **not** in the foreground.
That needs the OS call frameworks, not an in-app socket.

```
caller dials flat ─► Asterisk ─► agent ─► cloud knows a call is
   inbound for sipUsername X
        │
        ├─ iOS:  APNs PushKit VoIP push ─► device wakes ─►
        │          app reports a call to CallKit ─► native ring
        │
        └─ Android: FCM high-priority data msg ─► app's service ─►
                   ConnectionService / full-screen notification ─► ring
   user accepts ─► app opens the SIP tunnel + answers the INVITE ─►
                   media connects
```

- **iOS** mandates **PushKit + CallKit**: a VoIP push *must* result in
  a reported call, or iOS penalises the app. CallKit gives the native
  full-screen incoming UI and routes audio.
- **Android** uses a high-priority **FCM** data message → a foreground
  service + `ConnectionService` (or a full-screen-intent notification)
  for the incoming-call UI.
- The cloud gains a **mobile push sender** beside the existing VAPID
  `PushSender`: when a call is inbound for a subscriber that has a
  registered device token (§5.3), it pushes the wake-up.

Edge cases the design must handle: push delivery latency vs SIP INVITE
timeout (the agent/cloud should hold/early-media the call briefly while
the device wakes); a declined or missed call → SIP `486`/`480`; the app
killed by the OS → PushKit still wakes it on iOS, FCM on Android.

---

## 8. Auth, session & storage

- The session **token** is the only credential stored on-device — in
  the iOS Keychain / Android Keystore, never in plain `AsyncStorage`.
- The password is sent once over TLS at login/registration and never
  persisted on the device.
- All cloud traffic is HTTPS / `wss`. The cloud's `/sip-ws` bearer-
  token check remains the authority for SIP — the app holds no SIP
  password (consistent with the existing no-SIP-digest design).
- Token lifetime mirrors the web app's session model; on `401` the app
  drops to the Login screen.
- Biometric unlock (Face ID / fingerprint) to re-open a stored session
  is a nice-to-have, deferred past v1.

---

## 9. Security considerations

### 9.1 Ungated registration — the accepted risk

v1 registration is **instant and ungated**: anyone who knows a
society's name and a flat number can create a working account for that
flat and place *and receive* calls as that flat.

This is a real **impersonation / harassment** vector:

- A stranger can register as flat A-101 and call neighbours as them.
- A self-registered account also *receives* A-101's calls — it can be
  used to intercept calls meant for the real resident.
- Society name + flat number are low-entropy and easily guessed.

This posture is acceptable **only for a demo / trusted early-adopter
rollout** and must be documented as such to operators. It is the
weakest link in the v1 design and should not ship to an untrusted
audience unchanged.

### 9.2 Hardening path (post-v1)

The registration endpoint is designed so gating can be added without a
client redesign — only the account's initial `status` and an extra
field change:

- **Admin approval queue** — register creates `status: "pending"`; the
  account cannot log in or call until a society admin approves it in
  the Vaadin admin UI. Matches today's admin-vetted CSV model.
- **Society / flat join-code** — admin issues a secret code; the
  register form requires it. Self-serve, no per-signup admin step.
- **OTP** — verify the resident's mobile/email by one-time code before
  the account activates (proves contact ownership; pair with approval
  to also prove flat ownership).

Recommended eventual target: **OTP-verify + admin-approve**. The mobile
fields already collect Mobile and Email precisely so OTP is a drop-in.

### 9.3 Other controls

- Rate-limit `/subscriber/register` and `/subscriber/login` per IP /
  per society to blunt enumeration and bulk-account abuse.
- Enforce a real password policy server-side.
- Log every registration (society, flat, source IP, timestamp) so an
  operator can audit and revoke.
- Existing transport security (outer TLS + inner mTLS on the agent
  tunnel, per-call DTLS-SRTP) is unchanged and already covered by
  [`security/`](./security/).

---

## 10. Provisioning & data model impact

- **No new collection.** A self-registered resident is a normal
  `subscribers` document — same shape as a CSV-imported one, plus
  optional `mobile` / `email` and an origin marker
  (e.g. `source: "mobile-selfreg"`) for auditing.
- **`sipUsername`** is cloud-generated and must be unique society-wide
  (§5.2).
- **`flatNumber`** is denormalised onto the row — `CallRouter` and the
  directory both key on it; registration must populate it.
- **Device push tokens** are new state (§5.3) — a small collection or
  a field on the subscriber.
- The on-prem agent needs **zero changes**: `SubscriberWatcher` +
  `PjsipProvisioner` already provision any new `subscribers` row.

---

## 11. Alternatives considered

**Stack — Flutter.** Single codebase, excellent UI consistency, mature
`flutter_webrtc` + CallKit/ConnectionService plugins. Rejected for v1
only because it shares **no** code with the existing TypeScript
`sip.js` calling layer — the call-flow logic would be re-implemented in
Dart. A reasonable choice if the team prefers Dart.

**Stack — fully native (Swift + Kotlin).** Best possible CallKit /
PushKit / ConnectionService integration and lowest-level control.
Rejected for v1: two codebases, two skill sets, double the maintenance
for an app whose surface is three screens. Worth revisiting if call
quality/integration demands outgrow `react-native-webrtc`.

**Registration — gated (approval / join-code / OTP).** More secure
(§9.2) and the eventual target, but adds an admin step or an extra
factor that slows the demo/early-adopter loop. Deferred by explicit
decision; the endpoint is built so gating is an additive change.

**Calling — a native SIP SDK (e.g. PJSIP/linphone mobile).** Robust,
battle-tested media stacks. Rejected for v1 in favour of reusing
`sip.js`, keeping one signalling implementation across web and mobile;
revisit if the `react-native-webrtc` adapter proves fragile.

---

## 12. Milestones

1. **Project scaffold** — React Native app, navigation, CI for both
   stores, secure-storage wiring.
2. **Auth** — Login + Create-account screens against `/subscriber/
   login` and the new `/subscriber/register`; society-name resolution.
3. **Outbound calling** — tunnel connect, `sip.js` + the
   `react-native-webrtc` SDH, Dial screen → place a call → in-call UI.
4. **Inbound calling** — APNs PushKit + FCM, CallKit / Connection
   service, cloud mobile-push sender, the wake-up→answer flow.
5. **Hardening & store release** — error/edge handling, rate limiting,
   TestFlight / Play internal-testing, then public release.

Registration gating (§9.2) is a **fast follow** once the demo phase
ends — slot it before any untrusted-audience rollout.

---

## 13. Open questions

- **Society identification** — does the resident type a society *code*
  (stable, unfriendly) or a *display name* (friendly, possibly
  ambiguous across societies)? Resolution rules need pinning down
  (§5.1).
- **Call hold during push wake-up** — how long does the cloud/agent
  hold an inbound INVITE while a VoIP push wakes the callee's device,
  and what does the caller hear meanwhile (ringback vs early media)?
- **App distribution** — public App Store / Play listings, or private
  per-society distribution (Apple Business Manager / Play private
  channel)? Affects signing, review, and the onboarding URL.
- **Token lifetime & refresh** on mobile vs the web session model.
- **Multi-device** — one resident, app on phone + tablet: do both
  ring? (Forked-ring already supports it per-`sipUsername`; the
  question is whether one resident gets one `sipUsername` or one per
  device.)

---

## 14. Summary

The mobile app is a **new React Native client** on the existing cloud
control plane — no on-prem change. It adds two screens of cloud work
(a registration endpoint, society-name resolution) and a mobile push
path for incoming calls. The calling layer reuses `sip.js` over
`react-native-webrtc`.

The one deliberate weak spot is **ungated registration** (§9): fine for
a demo, unsafe for an untrusted audience, and built so it can be gated
later without a client rewrite.
