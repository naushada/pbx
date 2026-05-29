# onprem-pbx — TDD Plan

Tests are written **before** production code, one slice at a time. The order below is the execution order: each layer is fully green before the next starts.

## Status as of 2026-05-29

| Layer | Scope | Status |
|---|---|---|
| **L0** Wire-format primitives | `SipFrame*`, `HttpParser*`, `MessageParserBase*`, `SipParser*` | ✅ Done |
| **L1** Cloud HTTP / WS / Mongo | `SipBridge*`, `MicroServicePbx*`, `MicroServiceRouting*`, `PushSender*`, inherited modules | ✅ Done |
| **L2** pbx-agent | `SipFrameDemux*`, `CloudConnector*`, `AriClient*`, `CloudTunnelEndpoint*` | ✅ Done |
| **L3** Cloud ↔ Agent integration | `TunnelE2E*`, `BrowserStream*`, `AgentStream*`, `AceSslTransport*`, `AriWsClient*`, `HandoffOrdering*` (source-invariant) | ✅ Done |
| **L4** SIP correctness (real Asterisk) | `SipScenarios*` Python/pytest + sipp + real Asterisk | ⏳ Not started — gated on a docker-compose test fixture with Asterisk + coturn + a sipp UA; existing call-routing tests (`AriClient*`, `CallRouter*`) cover the seam against ARI fakes today |
| **L5a** UI Karma | `dashboard`, `directory`, `history`, `login`, `auth.guard`, `app-globals` + more (61 specs per README) | ✅ Done |
| **L5b** UI Playwright | `ui/e2e/tests/{login,dashboard,directory,history,settings}.spec.ts` (~12 tests) | ✅ Done |
| **L5c** Mobile RN | TDD layers M0 → M3.b + sip engine + auth shell + sign-out + incoming-call vibration + guard kiosk auto-answer (DESIGN.md §9) + ring re-arm after end + concurrent-call busy gate — 169 jest tests across 21 files. Detailed plan in [`docs/design/mobile-app-tdd.md`](./docs/design/mobile-app-tdd.md) | ✅ Through M3.b (App-shell, sign-out, ring-vibration, guard autoAnswer, ring rearm, busy gate); M4 Detox / M5 device matrix gated on simulators + real devices |
| **L6** Smoke | Production deploy validation runs the `test` (offtarget GTest) + `mobile-test` (Jest+tsc) + `ui-test` (ng build) gates on every PR and push (see `.github/workflows/publish-images.yml`); no separate post-deploy `/healthz`+canary runner yet | 🟡 CI gates done; post-deploy canary still manual |

**Offtarget baseline (`docker-compose.test.yml`):** 600/600 PASSED, 0 SKIPPED, no `gtest_filter`. (The four xpmile-inherited failures retired by PRs #151 + #152 + #153 — see `README.md` "Skipped tests" section.)

**CI merge gates** on every PR (PR #155+#156+#157): `offtarget` GTest 600/600, `mobile-test` Jest 150/150 + `tsc --noEmit` clean, `ui-test` Angular `ng build` clean.

The remaining real work is L4 (Asterisk-fixture sipp scenarios) and L6 post-deploy canary; everything else is shipped.

## Conventions

- C++ unit & integration tests run inside the built image as the `offtarget` GTest binary.
- Filter convention: `--gtest_filter='<Suite>*'`. New suites added under `modules/module/pbx/test/` and `pbx-agent/test/`.
- Angular tests use Karma + Jasmine.
- End-to-end browser tests use Playwright headed against a real Asterisk + a synthetic-call helper, in a podman-compose harness.
- Mobile (React Native) tests use Jest + `react-native-testing-library`; same suite runs under `docker/Dockerfile.mobile-test`. TS strictness is enforced separately via `npm run typecheck` in the same CMD (PR #156).
- "Red → green → refactor": every commit either adds a failing test or makes a failing test pass. CI rejects commits that only add code with no test diff.

---

## Layer 0 — Wire-format primitives (write first; foundation for everything else)

**Status: ✅ Complete (commit `f45b40a` on `main`). 55/55 green under podman.**

| Sub-layer | Suite | Tests | Status |
|---|---|---:|---|
| 0.b regression guard | `HttpParser*` (verbatim copy of the upstream shared-library suite) | 20 | ✅ |
| 0.b base | `MessageParserBase*` | 8 | ✅ |
| 0.b SIP | `SipParser*` | 17 | ✅ |
| 0.a frames | `SipFrame*` | 10 | ✅ |

Run: `podman build -f docker/Dockerfile.test -t onprem-pbx-test:layer0 . && podman run --rm onprem-pbx-test:layer0`

### 0.a Tunnel frame primitives

**Suite:** `SipFrame*` (in `modules/module/pbx/test/sip_frame_test.cc`)

| Test | What it pins down |
|---|---|
| `SipFrame.SerializeRoundTrip_Open`         | OPEN op encodes/decodes; stream-id and society-id preserved. |
| `SipFrame.SerializeRoundTrip_Data`         | DATA frame preserves arbitrary payload bytes (binary-safe). |
| `SipFrame.RejectsBadVersion`               | Decoder errors cleanly on unknown version byte; no crash. |
| `SipFrame.RejectsTruncatedHeader`          | Decoder returns "need more bytes" without consuming. |
| `SipFrame.RejectsTruncatedPayload`         | Header parses but payload short → "need more". |
| `SipFrame.HandlesPayloadAtBufferBoundary`  | Frame split across two `recv()` buffers reassembles. |
| `SipFrame.MaxPayloadGuard`                 | Refuses payload-len > MAX (e.g. 1 MiB), drops connection. |

Only after this suite is green do we let any code build a frame.

### 0.b Message-parser refactor (HTTP ⇄ SIP shared base)

Refactoring the inherited `http_parser` into a `MessageParser` base + `Http` subclass + new `Sip` subclass (see DESIGN.md §11). The order matters: lift the base under test first, prove the inherited parser still passes its own suite unchanged, then add the SIP layer.

**Suite:** `MessageParserBase*` (in `modules/module/http/test/message_parser_test.cc`)

| Test | What it pins down |
|---|---|
| `MessageParserBase.HeaderSeparator_Detected`         | `get_header()` returns prefix ending in `CRLFCRLF`. |
| `MessageParserBase.MessageLength_NeedsMore_NoHeaders`| Returns 0 when CRLFCRLF not yet seen. |
| `MessageParserBase.MessageLength_NoBody`             | Returns header length only when no `Content-Length`. |
| `MessageParserBase.MessageLength_WithContentLength`  | Returns `header_len + N`. |
| `MessageParserBase.MimeHeader_LowercasedKeys`        | Mixed-case input keyed as lowercase; lookup case-insensitive. |
| `MessageParserBase.MimeHeader_HandlesCRLFOnlyTerminator` | Body starts right after the blank line, not consumed as a header. |
| `MessageParserBase.PctDecode_KnownVectors`           | Spec examples (`%20`, `%2F`, double-encoded) round-trip. |

**Inherited suite:** `Http*` from the upstream shared-library `modules/module/http/test/httpparser_test.cc` is copied verbatim and must remain 100 % green. This is the regression guard for the refactor — if any inherited HTTP test breaks, the base extraction is wrong.

**Suite:** `SipParser*` (in `modules/module/sip/test/sip_parser_test.cc`)

| Test | What it pins down |
|---|---|
| `SipParser.ParsesRequestLine_Invite`             | `INVITE sip:b-204@society SIP/2.0` → `method() == "INVITE"`, `uri() == "sip:b-204@society"`, `is_request() == true`. |
| `SipParser.ParsesRequestLine_Register`           | Same shape, REGISTER. |
| `SipParser.ParsesStatusLine_200Ok`               | `SIP/2.0 200 OK` → `is_request() == false`, `status_code() == 200`, `reason_phrase() == "OK"`. |
| `SipParser.ParsesStatusLine_486Busy`             | 486 / "Busy Here" round-trips. |
| `SipParser.DistinguishesRequestFromStatus`       | A status line is NOT mis-parsed as `method() == "SIP/2.0"`. (Direct regression against the current `parse_uri` bug if base reused naively.) |
| `SipParser.CompactHeader_LResolvesToContentLength`| `l: 142\r\n` → `get_element("Content-Length") == "142"` AND `message_length` accounts for the body. |
| `SipParser.CompactHeader_VResolvesToVia`         | `v:` resolves; multiple `v:` rows accumulate. |
| `SipParser.CompactHeader_AllAliases`             | One test asserts every alias: `i/f/t/m/c/s/k/e/o` resolve to canonical names. |
| `SipParser.CanonicalAndCompact_Coexist`          | Mixed `Via:` and `v:` in same message preserved in arrival order (matters for proxy routing). |
| `SipParser.MultipleVia_PreservedInOrder`         | `get_all("via")` returns vector in arrival order; `get_element("via")` returns the first (top-most). |
| `SipParser.MultipleRecordRoute_PreservedInOrder` | Same multi-value behavior for `Record-Route`. |
| `SipParser.MessageLength_UsesCompactContentLen`  | Body framing works when sender uses `l:` not `Content-Length:` (the most common bite). |
| `SipParser.ChunkedEncoding_Rejected`             | RFC 3261 forbids it; parser errors cleanly (no crash, no half-parse). |
| `SipParser.RejectsMalformedFirstLine`            | Garbage first line → parser surfaces an error state, doesn't throw. |
| `SipParser.HandlesSdpBody_Opaque`                | SDP body returned as raw bytes, no further parsing or mutation. |

Only after Layer 0.a + 0.b are green do we move to Layer 1.

---

## Layer 1 — Cloud side: HTTP / WS / Mongo (mostly inherited from the shared library)

Reused tests come over verbatim from the upstream shared-library `test/` tree (46 tests across http/webservice/email — all should still pass after copy).

**New suites:**

`MicroServicePbx*` (in `modules/module/pbx/test/microservice_pbx_test.cc`)

| Test | Behavior |
|---|---|
| `MicroServicePbx.SocietyCreate_201`                 | `POST /api/v1/society` with valid body → 201 + Mongo write. |
| `MicroServicePbx.SocietyCreate_DuplicateCode_409`   | Same `code` rejected. |
| `MicroServicePbx.SubscriberImport_GeneratesCreds`   | CSV upload produces one subscriber per row; plaintext password returned ONCE; hash stored, plaintext not. |
| `MicroServicePbx.SubscriberImport_RejectsBadFlat`   | Row referencing unknown flat number → 400 with row index. |
| `MicroServicePbx.SubscriberImport_Idempotent`       | Re-uploading same CSV doesn't double-create or double-email. |
| `MicroServicePbx.Directory_FiltersByFlatPrefix`     | `GET /api/v1/subscriber?societyId=X&flatPrefix=A` case-insensitively filters on the denormalized `flatNumber`. |
| `MicroServicePbx.Directory_StripsSecrets`           | The directory response never carries `portalPasswordHash` / `sipHa1`. |
| `MicroServicePbx.CdrList_FiltersBySociety`          | `GET /api/v1/cdr?societyId=X` returns only that society's records. |
| `MicroServicePbx.PushSubscribe_PersistsEndpoint`    | `POST /api/v1/push/subscribe` writes `push_subscriptions`. |
| `MicroServicePbx.SubscriberLogin_DevMode_AcceptsAnyCredentials` | Dev mode (`PBX_AUTH_STRICT` unset): any non-empty `{email,password}` → 200 + token + synthetic profile; persists a `sessions` row + `Set-Cookie`. |
| `MicroServicePbx.SubscriberLogin_Strict_ValidCredentials_200`   | Strict mode: email lookup + bcrypt `verify_password` match → 200; `portalPasswordHash`/`sipHa1` stripped; `sessions` row persisted. |
| `MicroServicePbx.SubscriberLogin_Strict_WrongPassword_401`      | Strict: bad password / unknown email → 401. |
| `MicroServicePbx.SubscriberLogin_Strict_DisabledAccount_403`    | Strict: `status != "active"` → 403 even with the right password. |
| `MicroServicePbx.Auth_RejectsAnonymousSipWsUpgrade` | `/sip-ws` upgrade with no `?token=`/cookie → 401. |
| `MicroServicePbx.Auth_AllowsSipWsUpgrade_WithSessionCookie` | Seeded `sessions` row resolved via `session=` cookie → upgrade proceeds, `open_meta` carries `societyId`/`sipUsername`. |
| `MicroServicePbx.SipWsUpgrade_ResolvesSubscriberMeta_FromQueryToken` | Same via the UI's `?token=` query param. |
| `MicroServicePbx.SipWsUpgrade_RejectsUnknownToken` / `…_RejectsExpiredSession` | Token absent from `sessions`, or `expiresAt` in the past → 401. |

`PushSender*` (in `modules/module/pbx/test/push_sender_test.cc`)

| Test | Behavior |
|---|---|
| `PushSender.VapidJwt_HasCorrectAudience`           | JWT `aud` = origin of push endpoint. |
| `PushSender.VapidJwt_ExpiresIn12hMax`              | RFC 8292 cap. |
| `PushSender.EncryptsPayloadAes128Gcm`              | Decryption with subscriber's keys round-trips. |
| `PushSender.RetriesOn503`                          | Exponential backoff; max 3. |
| `PushSender.DropsSubscriptionOn410Gone`            | Mongo record removed. |

`SipBridge*` (in `modules/module/pbx/test/sip_bridge_test.cc`) — uses a fake tunnel socket + a fake browser socket.

| Test | Behavior |
|---|---|
| `SipBridge.OnBrowserUpgrade_AssignsStreamId`         | Sequential ids; collision-free under concurrency. |
| `SipBridge.OnBrowserData_FramesAndForwards`          | Browser bytes wrapped in DATA frame, sent on tunnel. |
| `SipBridge.OnTunnelData_DemuxesByStreamId`           | Reverse path: stream-id selects the correct browser conn. |
| `SipBridge.OnBrowserClose_SendsCloseFrame`           | CLOSE op emitted with reason. |
| `SipBridge.OnTunnelDisconnect_ClosesAllBrowserConns` | All multiplexed streams notified, sockets shut down. |
| `SipBridge.OnAgentReconnect_NewStreamIdsOnly`        | Old browser sessions stay closed; new browsers get fresh ids on the new tunnel. |
| `SipBridge.HandoffOrdering`                          | Asserts `remove_handler → m_handle = INVALID → publish to bridge` order; if violated, test deliberately fails (the well-known WebSocket hand-off invariant). |

---

## Layer 2 — pbx-agent

`CloudConnector*` (in `pbx-agent/test/cloud_connector_test.cc`) — against a fake Heroku WSS in-process.

| Test | Behavior |
|---|---|
| `CloudConnector.Connects_PresentsClientCert`             | mTLS handshake uses configured cert; rejects untrusted server. |
| `CloudConnector.SendsOpenFramesForNewStreams`            | Internal API to register a stream emits OPEN. |
| `CloudConnector.AutoReconnectsWithBackoff`               | After disconnect, reconnects with 1s/2s/4s/... up to 30s cap. |
| `CloudConnector.SurvivesIntermittentTunnelDrop`          | Frames buffered briefly during reconnect; no panic. |
| `CloudConnector.Heartbeat_SendsPingAfterIdleInterval`    | After `heartbeat_interval_sec` of inbound silence, `tick()` emits one connection-level PING; none before. |
| `CloudConnector.Heartbeat_InboundBytesResetMissedCount`  | Any inbound bytes clear the miss counter — a responsive peer is never dropped. |
| `CloudConnector.Heartbeat_DropsTunnelAfterMaxMissedPings`| `heartbeat_max_missed` unanswered PINGs → drop + immediate reconnect. |
| `CloudConnector.Heartbeat_DisabledWhenIntervalZero`      | `heartbeat_interval_sec == 0` sends nothing and never drops. |

`SipFrameDemux*` (in `pbx-agent/test/sip_frame_demux_test.cc`)

| Test | Behavior |
|---|---|
| `SipFrameDemux.OnOpenFrame_OpensLocalAsteriskSocket`     | TCP connect to 127.0.0.1:8088 with correct path. |
| `SipFrameDemux.OnDataFrame_PipesToAsteriskSocket`        | Raw payload bytes appear on the Asterisk-bound socket unchanged. |
| `SipFrameDemux.OnAsteriskData_WrapsInDataFrame`          | Reverse path; correct stream-id. |
| `SipFrameDemux.OnAsteriskClose_EmitsCloseFrame`          | Local socket EOF → CLOSE frame upstream. |
| `SipFrameDemux.DropsUnknownStreamId`                     | DATA for a never-OPENed stream is logged + dropped, no abort. |

`AriClient*` (in `pbx-agent/test/ari_client_test.cc`) — against a stub HTTP server pretending to be Asterisk ARI.

| Test | Behavior |
|---|---|
| `AriClient.SubscribesToChannelEvents`                    | Issues WS subscribe to `channels.>`. |
| `AriClient.ChannelCreated_IncrementsActiveCount`         | Society active-call counter increments. |
| `AriClient.ChannelDestroyed_DecrementsActiveCount`       | Decrements; never negative. |
| `AriClient.AdmissionCap_ReturnsBusyAtFive`               | 6th concurrent → `continue` to busy handler / 503-equivalent. |
| `AriClient.HangupEvent_WritesCdr`                        | Mongo CDR doc populated with caller, callees, duration, cause. |
| `AriClient.ConferenceBridgeEvents_TaggedAsConference`    | CDR `type=conference` when channels are in a ConfBridge. |

---

## Layer 3 — Cloud ↔ Agent integration (end-to-end frame plumbing, no Asterisk)

`TunnelE2E*` (in `test/integration/tunnel_e2e_test.cc`) — boots both binaries in-process, no real Heroku.

| Test | Behavior |
|---|---|
| `TunnelE2E.BrowserBytesReachAsteriskFake_AndBack`        | A fake "browser" socket on cloud sends bytes; a fake "Asterisk" on agent receives them; reply bytes come back. |
| `TunnelE2E.TwoBrowsersMultiplexedOverOneTunnel`          | Two browser streams interleave without cross-talk. |
| `TunnelE2E.AgentRestart_BrowsersGetCleanReset`           | Kill agent; browsers see WS close (not partial garbage). |
| `TunnelE2E.PushNotifyFromAgentToCloud_TriggersWebPush`   | Agent emits PUSH_NOTIFY frame; cloud `PushSender` is called with the right subscriberId. |
| `TunnelE2E.CdrPushFromAgentToCloud_PersistsInMongo`      | CDR_PUSH frame → cloud Mongo write via wsdbagent path. |

### Post-Layer-3 reliability slice — WebSocket keep-alive

`AgentStream` / `BrowserStream` gained a recurring 25 s reactor timer that sends an RFC 6455 ping so Heroku's router doesn't H15-drop an idle `/agent` or `/sip-ws` socket (DESIGN.md §6.6, §7). Wired through `register_with_reactor()`, which now registers `READ_MASK` *and* arms the timer.

| Test | Behavior |
|---|---|
| `AgentStream.HandleTimeout_EmitsWsPing`   | Firing the keep-alive timer callback writes an unmasked WS ping frame (`0x89`) to the socket. |
| `BrowserStream.HandleTimeout_EmitsWsPing` | Same for the browser-side `/sip-ws` handler. |

Suites are now `BrowserStream*` 9/9, `AgentStream*` 10/10.

---

## Layer 4 — SIP correctness (real Asterisk in a container)

`SipScenarios*` — Python pytest harness (driven from CI) that runs real Asterisk + real coturn + a `sipp`-based UA. Asserts on signaling traces and CDR.

| Test | Behavior |
|---|---|
| `register_succeeds_with_correct_digest`                  | sipp registers as `u_alice`; 200 OK. |
| `register_fails_with_wrong_password`                     | 403. |
| `unknown_flat_returns_404`                               | INVITE to `Z-999@society.local` → 404 from dialplan. |
| `1to1_call_uses_directmedia`                             | Asterisk emits re-INVITE with `c=` line pointing endpoint-to-endpoint; rtpdebug confirms no RTP touches the box. |
| `forked_flat_with_two_residents_one_answers`             | Both AORs ring; first to answer wins; the other gets `487`. |
| `conference_for_flat_bridges_all_legs`                   | Three subscribers on a flat join; ConfBridge created; CDR `type=conference`. |
| `concurrent_call_cap_returns_busy`                       | 6th simultaneous call gets `503`. |
| `gate_extension_zero_rings_guard`                        | Dialling `0` rings every subscriber with `role=guard`. |

---

## Layer 5 — Browser softphone (Angular + Playwright)

Karma unit tests:

| Test file | Behavior |
|---|---|
| `dialpad.component.spec.ts`            | Search box debounced; flat-number filter; numeric-only dialpad input. |
| `softphone.service.spec.ts`            | SIP.js UA construction; WSS URL derived from window origin; reconnects on socket close. |
| `push.service.spec.ts`                 | VAPID subscribe; sends `endpoint/p256dh/auth` to `/api/v1/push/subscribe`. |
| `incoming.component.spec.ts`           | Accept/reject buttons drive SIP.js `accept()`/`reject()`. |

Playwright E2E (uses real cloud + real agent + real Asterisk in compose):

| Test | Behavior |
|---|---|
| `e2e.login_and_register`                       | Resident logs in; UA reports `registered`. |
| `e2e.dial_by_flat_completes`                   | Two browsers (Alice@A-101, Bob@B-204) — Alice dials; Bob sees ringing; both go to in-call. |
| `e2e.hold_resume_dtmf`                         | Mid-call: hold toggles SDP `sendonly`; DTMF `5` sends RFC 4733. |
| `e2e.missed_call_then_push_wakeup`             | Bob closes his tab; Alice dials; Bob's OS receives push notification; click opens page and answers within ringtime. |
| `e2e.conference_three_residents`               | Three browsers join a flat conference. |
| `e2e.cap_blocks_sixth_call`                    | After 5 calls in progress, 6th caller sees "busy". |
| `e2e.guard_kiosk_auto_answer`                  | Guard account configured for auto-answer; dialling `0` connects instantly. |

---

## Layer 6 — Smoke (post-deploy)

Run after every deploy against the live Heroku app + a staging society agent:

- `/healthz` returns 200 on cloud.
- pbx-agent reports tunnel `up=true` to cloud heartbeat endpoint.
- A canary subscriber successfully registers via sipp from outside the LAN.

---

## Order of work (drives the implementation)

The originally-planned order ran L0 → L1 → L2 → L3 → L4 (Asterisk) →
L5 (browser) → L6 (smoke). Reality ran L0 → L1 → L2 → L3 → L5 → L6
(CI gates) before L4. Mobile (L5c) is a parallel slice on the M0-M3
layering, not part of the original plan — see
[`docs/design/mobile-app-tdd.md`](./docs/design/mobile-app-tdd.md).

**Done** (in landed order):

1. ✅ Seed repo skeleton from the shared library — minimal subset shipped in Layer 0 commit (`modules/module/http/*`, `test/CMakeLists.txt`, root `CMakeLists.txt`, `docker/Dockerfile.test`, `docker-compose.test.yml`). Remaining inherited modules (webservice, mongodb, wsdbproxy, email) land alongside Layer 1 when first needed. *(Reuse map in DESIGN.md §12.)*
2. ✅ **Layer 0.b.** `MessageParser` base extracted; inherited `HttpParser*` 20/20 still green; `MessageParserBase*` 8/8 + `SipParser*` 17/17 green.
3. ✅ **Layer 0.a** `SipFrame*` 10/10 green.
4. ✅ **Layer 1** — `SipBridge*` 12/12, `MicroServicePbx*` 23/23, `PushSender*` 8/8, `MicroServiceRouting*` 9/9. The subscriber directory denormalizes `flatNumber` onto each row, strips secrets from its response, and the `/api/v1/subscriber/login` strict-mode (`PBX_AUTH_STRICT=1`) does email-keyed Mongo lookup + bcrypt verify; login persists a `sessions` row and `/sip-ws` resolves that token to the OPEN frame's identity. (See also: the 2026-05-29 baseline-failure cleanup PRs #151–#153 which retired the 4 xpmile-inherited test gaps the inherited copies had been carrying.)
5. ✅ **Layer 2** — `SipFrameDemux*` 14/14, `CloudConnector*` 15/15 (includes the SipFrame-level heartbeat slice — agent-originated PING + PONG/inbound liveness tracking), `AriClient*` 11/11, `CloudTunnelEndpoint*` 12/12. The `/sip-ws` swap shipped as: `CloudTunnelEndpoint` wired into `WebServer`; `WebConnection::handle_input` got an `/agent` WS upgrade branch mirroring `/ws/db`'s hand-off ordering; the `/sip-ws` 503 is context-aware (`X-PBX-AgentConnected: yes|no`).
6. ✅ **Layer 3** — `TunnelE2E*` 8/8, `BrowserStream*` 9/9, `AgentStream*` 10/10, `AceSslTransport*` 10/10, `AriWsClient*` 14/14, `HandoffOrdering*` 8/8 (source-invariant test against `webservice.cpp` — asserts `remove_handler` precedes `m_handle = ACE_INVALID_HANDLE` for all three WS upgrade branches; cheaper and sharper than a real-reactor test for this specific bug class).
7. ✅ **InnerTLS over `/agent` (D3)** — full mTLS handshake through paired `WsInnerTlsBridge` instances tested via `FullMtlsHandshake_ViaPairedBridges` (PR #148). `InnerTlsServer` cert-verify failures now log a labelled error code + the presented cert's subject/issuer/serial via a `verify_callback` (PR #150) — the next live deploy will surface specifically what's failing.
8. ✅ **Layer 5a (UI Karma) + 5b (UI Playwright)** — 61 Karma specs + 5 Playwright e2e flows shipped (`ui/src/**/*.spec.ts` + `ui/e2e/tests/*.spec.ts`).
9. ✅ **Layer 5c (Mobile RN)** — through M3.b + sip engine + auth shell + sign-out; 150 jest tests + 0 typecheck errors. See [`docs/design/mobile-app-tdd.md`](./docs/design/mobile-app-tdd.md) for the layer-by-layer detail.
10. ✅ **Layer 6 CI side** — every PR runs `offtarget` (600/600) + `mobile-test` (Jest + tsc) + `ui-test` (Angular `ng build`) as merge gates; `publish-images.yml` push-jobs publish images + auto-deploy `pbx-cloud` to Heroku.
11. ✅ **Shared `sip-ua/`** — `shared/sip-ua/sip-ua.ts` + `sip-ua-sipjs.ts` consolidated from ui/mobile duplicates (PR #146).
12. ✅ **Operator-script hygiene** — `install.sh` tty-safe password prompt + DNS check on chosen host (PR #158), `setup-society.sh` base64-pad preservation + IP validation (PR #159), `bootstrap-society.sh` tty fix (PR #160), `installer-entrypoint.sh` DNS check on chosen host (PR #161 — symmetric to #158 for the Path B / DooD install).

**Pending** (require external access I haven't been granted):

- ⏳ **Layer 4 (`SipScenarios*`)** — needs a docker-compose harness with Asterisk + coturn + sipp UAs. Designable offline; verification needs the harness running.
- ⏳ **Layer 5c mobile native** — real CallKit / PushKit / ConnectionService / FCM behind the existing `CallKitBridge` seam (a no-op stub is wired in `createSipEnv.ts`). Needs Xcode + Android SDK + simulators.
- ⏳ **Mobile M4 Detox + M5 device matrix** — needs simulators + physical devices.
- ⏳ **Layer 6 post-deploy canary** — `/healthz` + a sipp register from outside the LAN against the live Heroku cloud. Designable offline; verification needs Heroku auth.
- ⏳ **`/agent` cert-reject root-cause** — diagnostic instrumentation laid via PR #150 (verify_callback) on top of the bridge being ruled out by PR #148. Needs a live agent reconnect against Heroku to surface what the labelled log line says.

Definition of done for a layer: all tests green on `./offtarget --gtest_filter='<Suite>*'` (or `ng test` / `playwright test`), and CI has caught at least one regression introduced during development of the next layer (proves the test really binds the behaviour).
