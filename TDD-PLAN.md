# onprem-pbx — TDD Plan

Tests are written **before** production code, one slice at a time. The order below is the execution order: each layer is fully green before the next starts.

## Conventions

- C++ unit & integration tests run inside the built image as the `offtarget` GTest binary (same as xpmile).
- Filter convention: `--gtest_filter='<Suite>*'`. New suites added under `modules/module/pbx/test/` and `pbx-agent/test/`.
- Angular tests use Karma + Jasmine (xpmile pattern).
- End-to-end browser tests use Playwright headed against a real Asterisk + a synthetic-call helper, in a podman-compose harness.
- "Red → green → refactor": every commit either adds a failing test or makes a failing test pass. CI rejects commits that only add code with no test diff.

---

## Layer 0 — Wire-format primitives (write first; foundation for everything else)

**Status: ✅ Complete (commit `f45b40a` on `main`). 55/55 green under podman.**

| Sub-layer | Suite | Tests | Status |
|---|---|---:|---|
| 0.b regression guard | `HttpParser*` (copied verbatim from xpmile) | 20 | ✅ |
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

Refactoring xpmile's `http_parser` into a `MessageParser` base + `Http` subclass + new `Sip` subclass (see DESIGN.md §11). The order matters: lift the base under test first, prove the xpmile parser still passes its own suite unchanged, then add the SIP layer.

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

**Reused xpmile suite:** `Http*` from `xpmile/modules/module/http/test/httpparser_test.cc` is copied verbatim and must remain 100 % green. This is the regression guard for the refactor — if any xpmile HTTP test breaks, the base extraction is wrong.

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

## Layer 1 — Cloud side: HTTP / WS / Mongo (mostly reused from xpmile)

Reused tests come over verbatim from `xpmile/test/` (xpmile CLAUDE.md says 46 tests across http/webservice/email — all should still pass after copy).

**New suites:**

`MicroServicePbx*` (in `modules/module/pbx/test/microservice_pbx_test.cc`)

| Test | Behavior |
|---|---|
| `MicroServicePbx.SocietyCreate_201`                 | `POST /api/v1/society` with valid body → 201 + Mongo write. |
| `MicroServicePbx.SocietyCreate_DuplicateCode_409`   | Same `code` rejected. |
| `MicroServicePbx.SubscriberImport_GeneratesCreds`   | CSV upload produces one subscriber per row; plaintext password returned ONCE; hash stored, plaintext not. |
| `MicroServicePbx.SubscriberImport_RejectsBadFlat`   | Row referencing unknown flat number → 400 with row index. |
| `MicroServicePbx.SubscriberImport_Idempotent`       | Re-uploading same CSV doesn't double-create or double-email. |
| `MicroServicePbx.CdrList_FiltersBySociety`          | `GET /api/v1/cdr?societyId=X` returns only that society's records. |
| `MicroServicePbx.PushSubscribe_PersistsEndpoint`    | `POST /api/v1/push/subscribe` writes `push_subscriptions`. |
| `MicroServicePbx.Auth_RejectsAnonymousSipWsUpgrade` | WSS upgrade to `/sip-ws` without portal cookie → 401. |

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
| `SipBridge.HandoffOrdering`                          | Asserts `remove_handler → m_handle = INVALID → publish to bridge` order; if violated, test deliberately fails (xpmile CLAUDE.md §"WebSocket hand-off mechanics"). |

---

## Layer 2 — pbx-agent

`CloudConnector*` (in `pbx-agent/test/cloud_connector_test.cc`) — against a fake Heroku WSS in-process.

| Test | Behavior |
|---|---|
| `CloudConnector.Connects_PresentsClientCert`             | mTLS handshake uses configured cert; rejects untrusted server. |
| `CloudConnector.SendsOpenFramesForNewStreams`            | Internal API to register a stream emits OPEN. |
| `CloudConnector.AutoReconnectsWithBackoff`               | After disconnect, reconnects with 1s/2s/4s/... up to 30s cap. |
| `CloudConnector.SurvivesIntermittentTunnelDrop`          | Frames buffered briefly during reconnect; no panic. |

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

1. ✅ Copy xpmile skeleton — minimal subset shipped in Layer 0 commit (`modules/module/http/*`, `test/CMakeLists.txt`, root `CMakeLists.txt`, `docker/Dockerfile.test`, `docker-compose.test.yml`). Remaining xpmile modules (webservice, mongodb, wsdbproxy, email) land alongside Layer 1 when first needed. *(Reuse map in DESIGN.md §12.)*
2. ✅ **Layer 0.b.** `MessageParser` base extracted; xpmile `HttpParser*` 20/20 still green; `MessageParserBase*` 8/8 + `SipParser*` 17/17 green.
3. ✅ Layer 0.a `SipFrame*` 10/10 green.
4. ✅ Layer 1 — complete (modulo `HandoffOrdering`). `SipBridge*` 12/12, xpmile modules verbatim-copy 112/115 (3 environmental skips), `MicroServicePbx*` 11/11, `PushSender*` 8/8, `MicroServiceRouting*` 7/7 (PBX routes intercept first; xpmile URIs fall through). `/sip-ws` upgrade gates on the portal session cookie in `WebConnection::handle_input`; the real `SipBridge` hand-off is stubbed to 503 until the cloud-side tunnel endpoint exists (Layer 2). `HandoffOrdering` deferred to Layer 3 TunnelE2E — it needs ACE reactor mocking that's cheaper to do with a real reactor running.
5. ✅ Layer 2 — feature-complete modulo concrete socket I/O (deferred to Layer 3). `SipFrameDemux*` 14/14, `CloudConnector*` 11/11, `AriClient*` 11/11, `CloudTunnelEndpoint*` 12/12. The `/sip-ws` swap shipped as: `CloudTunnelEndpoint` wired into `WebServer` (`cloudTunnelEndpoint()` accessor parallel to `wsDbServer()`); `WebConnection::handle_input` got an `/agent` WS upgrade branch mirroring `/ws/db`'s hand-off ordering; the `/sip-ws` 503 is now context-aware (`X-PBX-AgentConnected: yes|no`).
6. ✅ Layer 3 (frame plumbing slice) — `TunnelE2E*` 8/8 green. `SipBridge` gained `set_push_notify_handler` / `set_cdr_push_handler` so `PUSH_NOTIFY` / `CDR_PUSH` ops have a place to land on the cloud side. `test/integration/tunnel_harness.hpp` pairs in-memory `ITransport` ↔ `IAgentTransport` and wires `CloudConnector + SipFrameDemux + FakeAsterisk` (agent) ↔ `CloudTunnelEndpoint + SipBridge + FakeBrowser` (cloud) into one TU. Tests cover all 5 TDD-plan cases plus browser-close-reaches-agent and buffered-frames-flush-after-reconnect. Remaining for Layer 3: concrete `BrowserStream` (retires `/sip-ws` 503), agent-side `AceSslTransport`, agent WS decoder for `/agent`, and the `HandoffOrdering` test — collectively the ACE reactor binding work, easiest verified end-to-end against a real reactor.
5. ⏳ Layer 2 — `CloudConnector*`. Implement to green.
6. ⏳ Layer 3 — `TunnelE2E*`. Wire everything end-to-end with fakes.
7. ⏳ Compose Asterisk + coturn locally. Write `MicroServicePbx*` + `AriClient*`. Implement to green.
8. ⏳ `PushSender*`.
9. ⏳ Angular skeleton + Karma specs; implement components.
10. ⏳ Playwright E2E.
11. ⏳ Smoke + first society pilot.

Definition of done for a layer: all tests green on `./offtarget --gtest_filter='<Suite>*'` (or `ng test` / `playwright test`), and CI has caught at least one regression introduced during development of the next layer (proves the test really binds the behaviour).
