# Society Softphone — Mobile App

React Native (iOS & Android) softphone for onprem-pbx residents. A
resident logs in or self-registers with their society and flat, types
a destination flat number, and calls it.

- **Design:** [`docs/design/mobile-app.md`](../docs/design/mobile-app.md)
- **Test plan:** [`docs/design/mobile-app-tdd.md`](../docs/design/mobile-app-tdd.md)

## Status

**End-to-end runnable** for outbound + foreground inbound calls
once you do the one-time native-shell generation below. The sip.js
engine and the App-shell wiring landed in **PRs #143 + #144**; an
iOS Simulator build can place and receive calls — to/from the
Angular browser softphone or another mobile instance — with no
further code work.

| Layer | Scope | State |
|---|---|---|
| **M0** | Scaffold, Jest/RNTL harness, `FakeCloud`, native-module mocks | ✅ landed |
| **M1** | Auth & registration — validation, API client, session, Login / Register / Dial screens, React Navigation stack | ✅ landed |
| **M2** | Outbound calling — call primitives, SIP tunnel, WebRTC adapter (`MediaSession`), Dial + in-call screens against the `CallController` seam | ✅ landed (PRs #131–#134) |
| **M2 sip.js engine** | Real `SipCallController` backed by sip.js `UserAgent` + `Inviter`; the wrapper lives in `shared/sip-ua/sip-ua-sipjs.ts` (single source shared with the Angular softphone — `mobile/src/sip/sipJsUaFactory.ts` is a thin re-export) | ✅ landed (PR #143; consolidated PR #146) |
| **M3.a** | Push payload + device registration (pure logic) | ✅ landed |
| **M3.b** | Incoming-call glue — `IncomingCallController`, pure ring reducer, accept/decline/timeout against mocked CallKit + signaling | ✅ landed (PR #138) |
| **M3.b sip.js inbound bridge** | `SipInboundBridge` — Call-ID → `IncomingCallHandle` registry, implements the M3.b `IncomingCallSignaling` seam, feeds `IncomingCallController.reportPush` on every inbound INVITE | ✅ landed (PR #143) |
| **App shell** | `AuthContext` + `createSipEnv` factory + `IncomingCallOverlay` (foreground ring UI); `App.tsx` builds the engine on login, tears it down on logout | ✅ landed (PR #144) |
| **Sign-out** | `DialScreen` sign-out button — `sessionStore.clear()` → `setSession(null)` → `navigation.reset(Login)`; UA torn down by the App shell's `useEffect([session])` | ✅ landed (PR #147) |
| M3 native | Real CallKit / PushKit / ConnectionService / FCM for wake-up from killed/backgrounded states | toolchain + device work |
| M4 | Detox E2E across iOS simulator + Android emulator | not started |
| M5 | Manual device matrix → store release | not started |

Current suite: **21 Jest files / 161 tests** (+ `tsc --noEmit` clean), all green via
`docker-compose.mobile-test.yml`.

## Quickstart — run the test suite

Two paths. Pick the one that matches what you have on the host.

### A. In a container (recommended — no host install)

Mirrors the C++ `offtarget` workflow. Needs only podman/podman-compose.

```sh
podman-compose -f docker-compose.mobile-test.yml build mobile-test
podman-compose -f docker-compose.mobile-test.yml run --rm mobile-test
```

> ⚠️ `podman-compose run` alone **reuses the cached image** — when
> `mobile/` source changes, you must run the explicit `build` step
> first or the new code never enters the image (same caveat as
> `offtarget`).

Image: `docker/Dockerfile.mobile-test` — Node 20 + the
node-gyp toolchain, `npm install`, then `npm test --ci`. The current
suite is **21 files / 161 tests**. The build also copies the
top-level `shared/` directory parallel to `mobile/` so the
`shared/sip-ua/` source resolves the same way it does in-tree.

### B. On a dev machine (needed to actually build the native app)

```sh
cd mobile
npm install          # JS deps
npm test             # Jest, same suite as the container
npm run typecheck    # tsc --noEmit
```

Requirements: **Node ≥ 18** and npm.

## How to place a call

The app is **runnable today** on an iOS Simulator (and on a wired
iPhone if you sign the build with your Apple ID in Xcode). There is
no signed `.ipa` / `.apk` and no App Store / Play Store listing yet
— that's M5.

### One-time setup (per dev machine)

```sh
cd mobile
npm install
# Generate the native iOS shell — see "Native projects" below.
# Skip "android" if you don't have Android Studio + JDK 17.
npx @react-native-community/cli@latest init SocietySoftphone \
  --version 0.74.5 --directory /tmp/sn-shell --skip-install
cp -R /tmp/sn-shell/ios /tmp/sn-shell/android ./
cd ios && pod install && cd ..
```

### Boot the app

```sh
npm run ios       # opens iOS Simulator (needs macOS + Xcode)
# npm run android also works on Android Studio + JDK 17
```

For a **second peer** (mobile↔mobile), boot a second iOS Simulator:

```sh
# in another terminal — picks a different sim device
npx react-native run-ios --simulator="iPhone 15 Pro"
```

Or use the **Angular browser softphone** as the other end
(`https://pabx-5fbf3550f938.herokuapp.com/`) — same cloud, same
society, different login.

### What happens on launch

1. **Auto-restore** — App boots, calls `SessionStore.load()`. If a
   saved session is on disk, it skips Login and the sip.js engine
   spins up before DialScreen mounts.
2. **Otherwise: log in or self-register** (Login / Register screens,
   M1.c) with society code, flat number, password. Registration is
   ungated by design for v1 — see
   [`mobile-app.md` §9.1](../docs/design/mobile-app.md). On success
   the screen saves the session, calls `setSession(...)` on
   `AuthContext`, App's `useEffect` builds the sip env, and the
   screen navigates to Dial.
3. **Outbound** — `SipCallController` (PR #143) places a real INVITE
   via sip.js. Call state visibly progresses *Calling → Connected*;
   audio flows via WebRTC + the on-prem coturn.
4. **Inbound** — when another peer dials you, sip.js delivers the
   INVITE; `SipInboundBridge` (PR #143) feeds it to
   `IncomingCallController.reportPush()`, which puts the controller
   into `'ringing'`; `IncomingCallOverlay` (PR #144) paints a
   full-screen Accept / Decline modal over whatever screen is
   active. Accept → `answer()` → 200 OK → connected.
5. **Hangup** from either side ends both. `setSession(null)` would
   tear the UA down (logout button is the only piece of UX still
   missing).

### Prerequisites for an actual call to land

- An **on-prem stack** (Asterisk + coturn + Mongo + `pbx-agent`)
  attached to the cloud — `sudo ./install.sh` on a Linux box, or
  `podman-compose -f docker-compose.agent.yml up`. See the top-
  level `README.md` and `INSTALL.md`.
- Two **seeded residents** of the same society in Mongo (the
  installer / admin import handles this).
- Mic permission on first call (system-prompted on the simulator /
  device).

The runtime topology is unchanged from the Angular browser softphone:
the app talks **only to the cloud**; signalling goes app → cloud →
agent's outbound tunnel → Asterisk; media (RTP) is a separate WebRTC
path through the on-prem coturn. Full picture in
[`mobile-app.md` §2.1](../docs/design/mobile-app.md).

## Native projects (`ios/` and `android/`)

The native shells are **not committed** — they are generated by the
React Native CLI and are large, mostly-boilerplate trees. `npm test`
and the container suite both work **without** them. To run the app on
a device/simulator, generate them once:

```sh
# generate a throwaway RN project of the same name + version, then
# copy its native shells in next to this JS/TS source
npx @react-native-community/cli@latest init SocietySoftphone \
  --version 0.74.5 --directory /tmp/sn-shell --skip-install
cp -R /tmp/sn-shell/ios /tmp/sn-shell/android ./
```

(They are `.gitignore`d at the build-output level only — once
generated, the project files are committed normally.)

## Layout

```
mobile/
  App.tsx                          auth-aware shell — builds sip env on login (PR #144)
  index.js                         RN entry point; imports webrtcGlobals first
  jest.setup.ts                    native-module mocks (M0)
  src/
    api/                           cloud REST client (M1.a)
      cloudClient.ts, http.ts, defaultClient.ts, types.ts
    call/                          call control seams + sip.js adapters
      callController.ts            outbound seam + StubCallController (M2.d)
      sipCallController.ts         sip.js-backed CallController (PR #143)
      incomingCallController.ts    M3.b inbound glue
      sipIncomingSignaling.ts      SipInboundBridge — UA → reportPush + signaling impl (PR #143)
      useCall.ts                   React hook over CallController
    navigation/                    React Navigation native-stack root
    push/                          APNs/FCM payload + device registration (M3.a)
      pushPayload.ts, deviceRegistrar.ts
    screens/                       Login / Register / Dial / InCallPanel / IncomingCallOverlay
      IncomingCallOverlay.tsx      foreground ring Modal (PR #144)
    session/                       keychain-backed session store (M1.b)
    sip/                           pure call models + sip.js engine
      callState.ts                 outbound call reducer (M2.a)
      incomingCall.ts              inbound ring reducer (M3.b)
      sipTunnel.ts                 token'd /sip-ws transport (M2.b)
      mediaSession.ts              WebRTC adapter (M2.c)
      sipUa.ts                     re-export of shared/sip-ua/sip-ua (interface seam)
      sipJsUaFactory.ts            re-export of shared/sip-ua/sip-ua-sipjs (sip.js wrapper)
      webrtcGlobals.ts             installs react-native-webrtc as globals for sip.js platform/web SDH
      sdp.ts, sipUri.ts
    state/                         AppDeps + Auth contexts; sip env factory
      deps.tsx                     AppDeps (includes optional incomingCallController)
      authContext.tsx              {session, setSession} — App shell tracks login (PR #144)
      createSipEnv.ts              factory: Session → wired engine + cleanup (PR #144)
    test/                          in-test fakes
      fakeCloud.ts, fakeCloudTransport.ts, fakeCallController.ts
    validation/                    form-input rules (M1.a)
```

Every layer is introduced **test-first**: a sibling `__tests__/`
directory drives each module before the implementation lands. The
seam pattern (`CallController`, `IncomingCallController`,
`CallKitBridge`, `IncomingCallSignaling`, …) keeps the integration
slice — sip.js, real CallKit / PushKit — as drop-in replacements for
the fakes used in tests.

## How it connects

The app talks **only to the cloud** — never directly to the on-prem
`pbx-agent`. SIP signalling goes app → cloud → (the agent's own
outbound tunnel) → Asterisk; media (RTP) is a separate WebRTC path
relayed through the on-prem coturn. Full detail in
[`mobile-app.md` §2.1](../docs/design/mobile-app.md).

## What's next

Everything authorable in JS is landed. What remains needs native
modules, real devices, or product UX:

- **Real CallKit / PushKit (iOS) and ConnectionService / FCM
  (Android)** behind the `CallKitBridge` seam — wake the app from
  killed / backgrounded states. A no-op stub is wired in
  `createSipEnv.ts` today; `IncomingCallOverlay` handles the
  foreground case without it.
- **M4** — Detox flows across an iOS simulator + Android emulator
  against a dedicated test cloud + a synthetic answerer.
- **M5** — manual device matrix (real APNs / FCM, audio over mobile
  data + Wi-Fi + TURN, Doze / OEM battery killers), TestFlight + Play
  internal testing, then store submission.
