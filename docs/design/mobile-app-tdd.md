# Mobile App — TDD Plan

Test-driven build plan for the React Native mobile client described in
[`mobile-app.md`](./mobile-app.md). Tests are written **before**
production code, one slice at a time; the order below is the execution
order — each layer is fully green before the next starts.

This mirrors the discipline of the root [`TDD-PLAN.md`](../../TDD-PLAN.md)
(the C++/Angular system), adapted to the React Native toolchain.

## Status (as of 2026-05-29)

**End-to-end runnable** for outbound + foreground inbound calls from
an iOS Simulator build, with sign-out + the shared sip-ua module +
mobile-test CI gate landed.

| Layer | State |
|---|---|
| M0 — Scaffold & harness | ✅ landed |
| M1 — Auth & registration (a/b/c/d) | ✅ landed |
| M2 — Outbound calling (a/b/c/d) | ✅ landed against the `CallController` seam |
| **M2 sip.js engine** | ✅ landed PR #143 — `SipCallController` backed by sip.js `UserAgent`+`Inviter`; consolidated into a single shared source at `shared/sip-ua/sip-ua-sipjs.ts` by PR #146 (both ui/ and mobile/ now re-export from there) |
| M3.a — Push payload + device registration | ✅ landed |
| M3.b — Incoming-call glue (mocked CallKit) | ✅ landed |
| **M3.b sip.js inbound bridge** | ✅ landed PR #143 — `SipInboundBridge` implements `IncomingCallSignaling` + feeds `reportPush` on every UA-delivered INVITE |
| **App shell** | ✅ landed PR #144 — `AuthContext` + `createSipEnv` factory + `IncomingCallOverlay` foreground ring Modal; `App.tsx` builds the engine on login, tears it down on logout |
| **Sign-out UI** | ✅ landed PR #147 — `DialScreen` sign-out button: `sessionStore.clear()` → `setSession(null)` (App shell tears the UA down) → `navigation.reset(Login)` |
| **Incoming-call vibration** | ✅ landed PR #162 — `IncomingCallAlerter` pulses the device while `IncomingCallController` is in `'ringing'` state (1 s on / 2 s off pattern via RN's built-in `Vibration` API). Closes the parity gap vs the web softphone's `RingtoneService` for the foreground case; audio ring will land alongside real CallKit/PushKit when that slice ships. |
| **Guard kiosk auto-answer** | ✅ landed PR #163 — `Subscriber.autoAnswer?: boolean` + `SubscriberRole` union (`'resident' \| 'guard' \| 'admin'`); `IncomingCallController` accepts a `shouldAutoAnswer` predicate; `createSipEnv` AND-s `sub.autoAnswer && sub.role === 'guard'` (defence-in-depth — `autoAnswer` on a resident is silently ignored). Auto-answered calls skip CallKit display + ring timer + (transitively) the Vibration alerter, matching `SipService.onIncoming` in the web softphone (DESIGN.md §9). |
| **Mobile typecheck in CI** | ✅ landed PR #156 — `Dockerfile.mobile-test`'s CMD runs `npm run typecheck && npm test --ci`; closed 7 latent TS errors that babel-jest had been silently transforming around. PR #155 + #156 wire `mobile-test` into `.github/workflows/publish-images.yml` as a PR merge gate. |
| **Lockfile + npm ci** | ✅ landed PR #154 — `mobile/package-lock.json` committed, Dockerfile.mobile-test switched from `npm install` to `npm ci` for deterministic builds. |
| M3 native | not started — real CallKit / PushKit / ConnectionService / FCM for wake-up from killed/backgrounded states (the M3.b `CallKitBridge` seam already accepts the right shape; the in-app `IncomingCallOverlay` Modal handles the foreground case without it) |
| M4 — End-to-end (Detox) | not started — needs iOS simulator + Android emulator + a test cloud + synthetic answerer |
| M5 — Manual device matrix | not started |

Containerised Jest runner shipped via PR #137
([`docker/Dockerfile.mobile-test`](../../docker/Dockerfile.mobile-test))
— the entire suite (**21 files / 159 tests** + **0 typecheck errors**)
is reproducible without an RN install on the host. See
[`mobile/README.md`](../../mobile/README.md) for invocation and the
how-to-call walkthrough.

**On main as of 2026-05-29:** the mobile work that doesn't need a
native toolchain or physical devices is fully landed. The remaining
items (M3 native, M4 Detox, M5 device matrix) are gated on Xcode +
Android SDK + simulators + a fleet — verification scope, not
implementation scope.

---

## Conventions

- **Unit & component tests:** Jest + `@testing-library/react-native`.
  Run with `npm test`; CI gate `npm test -- --coverage`.
- **E2E tests:** Detox, driving the app on an iOS simulator + an
  Android emulator. (Maestro is an acceptable lighter alternative for
  flows Detox struggles with.)
- **Native modules are mocked in Jest** — `react-native-webrtc`,
  `react-native-callkeep`, push, and `react-native-keychain` have no
  JS-testable behaviour in the runner; each gets a hand-written
  `jest.mock` fake. Their *real* behaviour is covered only at the E2E
  layer and by a manual device matrix (§M5).
- **No live backend in unit/integration tests.** A **fake cloud** —
  an in-process HTTP + WebSocket double that speaks the real
  `/api/v1/*` REST shapes and the `/sip-ws` frame protocol — backs the
  integration slices. Detox runs against a dedicated test cloud +
  on-prem stack with a synthetic answerer.
- **Red → green → refactor:** every commit either adds a failing test
  or makes one pass. A code-only diff with no test diff is rejected in
  review.
- **Test pyramid:** lots of unit, a solid band of component +
  integration, a thin E2E layer. Anything that can be pure logic
  (validation, API mapping, the call state machine, SDP shaping) is
  pulled *out* of components so it is unit-testable without a renderer.

---

## Layer M0 — Scaffold & harness (write first)

No feature code — this layer makes the next four testable.

| Slice | Suite | What it pins down |
|---|---|---|
| M0.1 | `smoke` | App boots and renders the root navigator without crashing — the Jest + RNTL pipeline works. |
| M0.2 | `ci` | `npm test` runs in CI for both platform configs; coverage is collected; a failing test fails the build. |
| M0.3 | `harness/fakeCloud` | The fake-cloud double answers `/subscriber/login`, `/subscriber/register`, society-resolve, and accepts a `/sip-ws` upgrade — proven by tests *of the double itself*. |
| M0.4 | `harness/nativeMocks` | `jest.mock` fakes for `react-native-webrtc`, `react-native-callkeep`, push, Keychain expose the interfaces the app will call. |

**Exit:** `npm test` green; Detox builds a debug app for both platforms.

---

## Layer M1 — Auth & registration

Pure logic and the two entry screens. No SIP yet.

### M1.a Validation & API client (unit)

**Suites:** `validation*`, `apiClient*`

| Test | What it pins down |
|---|---|
| `validation.LoginForm_RejectsEmptyFields` | Society / flat / password all required; empty → field error. |
| `validation.RegisterForm_RequiredVsOptional` | Society, flat, resident name, password required; mobile + email optional and may be blank. |
| `validation.RegisterForm_EmailShape` | A present-but-malformed email is rejected; absent email passes. |
| `validation.PasswordPolicy` | Password below the minimum length/strength is rejected client-side. |
| `apiClient.Login_BuildsRequest` | `POST /api/v1/subscriber/login` body carries society + flat + password; correct headers. |
| `apiClient.Login_Maps401` | `401` → typed `InvalidCredentials` error, not a raw throw. |
| `apiClient.Register_BuildsRequest` | `POST /api/v1/subscriber/register` body includes optional fields only when set. |
| `apiClient.Register_MapsErrors` | `404` unknown society and `409` duplicate map to distinct typed errors. |
| `apiClient.ResolvesSociety` | A society name/code resolves to a `societyId` (or surfaces "not found"). |
| `apiClient.NetworkError_IsTyped` | A transport failure surfaces as a retryable typed error, never an unhandled rejection. |

### M1.b Session & secure storage (unit)

**Suite:** `session*`

| Test | What it pins down |
|---|---|
| `session.StoresTokenInKeychain` | The token is written via the Keychain/Keystore wrapper — never plain storage. |
| `session.RestoresOnLaunch` | A stored token rehydrates the session on cold start. |
| `session.ClearsOn401` | Any API `401` clears the stored token and drops to Login. |
| `session.PasswordNeverPersisted` | The password is never written to any store after the request. |

### M1.c Login & Create-account screens (component)

**Suites:** `LoginScreen*`, `RegisterScreen*`

| Test | What it pins down |
|---|---|
| `LoginScreen.RendersFields` | Society, Flat, Password inputs + Log-in + "Create account" link present. |
| `LoginScreen.SubmitCallsApi` | Tapping Log in invokes the API client with the typed values. |
| `LoginScreen.ShowsServerError` | An `InvalidCredentials` error renders an inline message; no crash. |
| `LoginScreen.NavigatesOnSuccess` | A successful login routes to Dial. |
| `RegisterScreen.MarksOptionalFields` | Mobile/email visibly optional; form submits with them blank. |
| `RegisterScreen.AutoLoginOnSuccess` | `register` returning a token routes straight to Dial (no second login). |
| `RegisterScreen.SurfacesDuplicate` | A `409` duplicate renders a clear, actionable message. |

### M1.d Auth flow (integration, fake cloud)

**Suite:** `authFlow*`

| Test | What it pins down |
|---|---|
| `authFlow.LoginHappyPath` | Login → token stored → Dial, end to end against the fake cloud. |
| `authFlow.RegisterHappyPath` | Register (ungated) → account active → auto-login → Dial. |
| `authFlow.UnknownSociety` | Login/register against a non-existent society fails cleanly with the right message. |
| `authFlow.RateLimited` | A `429` from the fake cloud is surfaced as "try again later" (guards the §9.3 rate-limit contract). |

**Exit:** a user can register and log in against the fake cloud; token handling is proven.

---

## Layer M2 — Outbound calling (SIP + media)

The core loop: type a flat, press Call, hear audio.

### M2.a Call primitives (unit)

**Suites:** `sipUri*`, `sdp*`, `callMachine*`

| Test | What it pins down |
|---|---|
| `sipUri.BuildsForFlat` | A destination flat → `sip:<flat>@pbx.<society>`, SIP-safe (no underscores in the host — PR #76). |
| `sipUri.RejectsBadFlat` | Empty / non-dialable flat input is rejected before any INVITE. |
| `sdp.OffersOpusFirst` | The generated SDP lists Opus first (Asterisk has no Opus transcoder — PR #120); G.711 only as fallback. |
| `callMachine.IdleToCalling` | Pressing Call moves `idle → calling` and emits one INVITE. |
| `callMachine.CallingToConnected` | A `200 OK` moves `calling → connected` and starts the timer. |
| `callMachine.RemoteHangup` | A `BYE` moves `connected → ended`; resources released. |
| `callMachine.LocalHangup` | Hang-up from any state emits the correct SIP and ends cleanly. |
| `callMachine.CallRejected` | A `486/603` surfaces "busy / declined", returns to `idle`. |
| `callMachine.NoDoubleCall` | Call is a no-op while a call is already active. |

### M2.b Tunnel + SIP transport (integration, mocked WS)

**Suite:** `sipTransport*`

| Test | What it pins down |
|---|---|
| `sipTransport.ConnectsWithToken` | The app opens `/sip-ws?token=…` with the session token; rejects on a missing token. |
| `sipTransport.RegisterOnConnect` | On tunnel up the app sends SIP `REGISTER`; a `200` marks it registered. |
| `sipTransport.ReconnectsOnDrop` | A dropped tunnel reconnects with backoff and re-`REGISTER`s. |
| `sipTransport.OutboundInviteOverTunnel` | An INVITE from the call machine is framed and written to the tunnel. |

### M2.c WebRTC adapter (integration, mock RTCPeerConnection)

**Suite:** `webrtcSdh*`

| Test | What it pins down |
|---|---|
| `webrtcSdh.CreatesOffer` | The `SessionDescriptionHandler` produces a local offer from `react-native-webrtc`. |
| `webrtcSdh.AppliesAnswer` | A remote answer is applied to the peer connection. |
| `webrtcSdh.WiresIceToTunnel` | ICE candidates flow to/from the SIP layer. |
| `webrtcSdh.ReleasesOnEnd` | Hang-up tears down the peer connection and releases the mic. |

### M2.d Dial & in-call screens (component)

**Suites:** `DialScreen*`, `InCallScreen*`

| Test | What it pins down |
|---|---|
| `DialScreen.ShowsIdentityAndPbxState` | Header shows the logged-in flat; a "connected to PBX" indicator reflects tunnel state. |
| `DialScreen.CallDisabledWhenInputEmpty` | Call button is disabled until a valid flat is entered. |
| `DialScreen.CallInvokesMachine` | Pressing Call drives the call state machine with the typed flat. |
| `InCallScreen.RendersTimerMuteHangup` | Connected state shows a running timer, mute, hang-up. |
| `InCallScreen.MuteTogglesAudioTrack` | Mute enables/disables the local audio track. |
| `InCallScreen.HangupEndsCall` | Hang-up ends the call and returns to Dial. |

**Exit:** against the fake cloud, the app registers, places a call, and the call machine + WebRTC adapter complete a (mock-media) session.

---

## Layer M3 — Inbound calling & push

A flat must *ring* when the app is backgrounded or closed.

### M3.a Push payload & device registration (unit)

**Suites:** `pushPayload*`, `deviceToken*`

| Test | What it pins down |
|---|---|
| `pushPayload.ParsesWakeUp` | An APNs/FCM wake-up payload parses to `{caller, callId}`; a malformed payload is dropped, not crashed-on. |
| `deviceToken.RegistersWithCloud` | The APNs/FCM token is posted to `/api/v1/push/device` with `sipUsername` + platform. |
| `deviceToken.ReregistersOnRotate` | A rotated token re-registers; the stale one is replaced. |

### M3.b Incoming-call glue (integration, mock callkeep)

**Suite:** `incomingCall*`

| Test | What it pins down |
|---|---|
| `incomingCall.PushReportsToCallKit` | A wake-up push calls `callkeep.displayIncomingCall` with the caller identity. |
| `incomingCall.AcceptConnectsMedia` | Accepting opens the tunnel (if needed) and answers the INVITE; media connects. |
| `incomingCall.DeclineSendsReject` | Declining emits SIP `486`/`603` and ends the call. |
| `incomingCall.MissedCallTimeout` | An unanswered call ends on timeout and is logged as missed. |
| `incomingCall.AppKilled_PushStillRings` | With the app terminated, a push still produces a reported call (simulated entry point). |

> CallKit / ConnectionService present a *native* UI that Jest cannot
> render — these tests cover the **JS glue** (what the app tells
> callkeep, and how it reacts). The native screen itself is verified
> only at M4/M5.

**Exit:** the incoming-call control flow is proven against mocked
native modules; a backgrounded app can be woken and answer.

---

## Layer M4 — End-to-end (Detox, real backend)

Detox on a simulator + emulator, against a dedicated test cloud + an
on-prem stack with a synthetic answerer.

| Flow | What it pins down |
|---|---|
| `e2e.RegisterThenLand` | Fresh install → Create account → lands on Dial, callable. |
| `e2e.LoginThenLand` | Existing resident logs in → Dial. |
| `e2e.PlaceCallToFlat` | Type a flat → Call → synthetic answerer picks up → two-way audio path establishes. |
| `e2e.ReceiveCall` | The synthetic caller dials this device → native incoming UI → accept → connected. |
| `e2e.CallFromBackground` | App backgrounded → incoming call → push wakes it → ring → answer. |
| `e2e.TokenExpiryRelogin` | An expired session drops to Login; re-login restores calling. |
| `e2e.TunnelDropMidCall` | Tunnel drop is handled gracefully (recover or clean-end, no zombie call). |
| `e2e.OfflineRegister` | Registration with no connectivity fails with a clear, retryable error. |

**Exit:** the full register → call → receive loop is green on both
platforms in CI.

---

## Layer M5 — Release smoke (manual + device matrix)

Not everything is automatable. Before each store submission, a manual
pass on a real-device matrix:

- Real APNs **PushKit** + CallKit on physical iPhones (push wake-up,
  lock-screen ring, audio routing, CallKit call history).
- Real **FCM** + ConnectionService / full-screen intent on physical
  Androids across vendors (Doze mode, OEM battery killers).
- Real audio quality over mobile data, Wi-Fi, and through coturn TURN
  relay (off-LAN).
- Permission prompts (microphone, notifications) on first run.

Tracked as a checklist in the release ticket; TestFlight + Play
internal-testing precede public release.

---

## Order of work

1. **M0** — scaffold, Jest/RNTL/Detox, fake cloud, native mocks.
2. **M1** — auth: validation → API client → session → screens → flow.
3. **M2** — outbound calling: primitives → transport → WebRTC adapter
   → Dial/in-call screens.
4. **M3** — inbound calling: push payload → device token → incoming
   glue.
5. **M4** — Detox E2E across both platforms.
6. **M5** — manual device matrix, then store submission.

Each layer is fully green before the next begins. M1–M4 map 1:1 onto
the design doc's milestones ([`mobile-app.md`](./mobile-app.md) §12).

### What this plan deliberately cannot prove with automated tests

- Native CallKit / PushKit / ConnectionService **UI and OS behaviour**
  — covered by M4 (Detox, limited) and M5 (manual).
- Real **APNs/FCM delivery** and timing — M5.
- Real **audio quality / echo / NAT traversal** — M5.
- App Store / Play **review** outcomes.

These are called out so coverage numbers are not mistaken for
confidence in the parts only a real device exercises.
