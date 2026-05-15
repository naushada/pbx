# pbx — cloud-side PBX module

Cloud-side (Heroku) module. Contains the new code that has no xpmile counterpart: the multiplex tunnel framing, the `SipBridge` event handler, and the VAPID Web Push sender.

Current status:

| Component   | File                                         | Status      |
|-------------|----------------------------------------------|-------------|
| `SipFrame`            | `inc/sip_frame.hpp` + `src/sip_frame.cpp`                       | ✅ Complete (Layer 0) |
| `SipBridge`           | `inc/sip_bridge.hpp` + `src/sip_bridge.cpp`                     | ✅ Complete (Layer 1, first slice) |
| `MicroServicePbx`     | `inc/microservice_pbx.hpp` + `src/microservice_pbx.cpp`         | ✅ Complete (Layer 1, second slice) |
| `PushSender`          | `inc/push_sender.hpp` + `src/push_sender.cpp`                   | ✅ Complete (Layer 1, third slice) |
| `CloudTunnelEndpoint` | `inc/cloud_tunnel_endpoint.hpp` + `src/cloud_tunnel_endpoint.cpp` | ✅ Complete (Layer 2, /sip-ws swap slice) |
| `BrowserStream`       | `inc/browser_stream.hpp` + `src/browser_stream.cpp`             | ✅ Complete (Layer 3, ACE binding for `/sip-ws`) |
| `AgentStream`         | `inc/agent_stream.hpp` + `src/agent_stream.cpp`                 | ✅ Complete (Layer 3, ACE binding for `/agent`) |
| `AceHttpsClient`      | `inc/ace_https_client.hpp` + `src/ace_https_client.cpp`         | ✅ Complete (Layer 4, concrete `IPushHttpClient` for `PushSender`) |

---

## SipFrame — multiplex tunnel wire format

A tiny, length-prefixed, big-endian binary framing protocol carried over the single mTLS WSS tunnel between Heroku and `pbx-agent`. One physical tunnel multiplexes many browser SIP-WS sessions plus out-of-band control messages (push notifications, CDR replication, heartbeats).

See [`DESIGN.md §7`](../../../DESIGN.md#7-tunnel-framing-heroku--pbx-agent) for the architectural rationale.

### Wire format

```
 0       1       2               6               10
 +-------+-------+---------------+---------------+--------+
 | ver=1 | op    | stream-id (4) | payload-len(4)| payload|
 +-------+-------+---------------+---------------+--------+
```

- All integers **big-endian**.
- Header is fixed at 10 bytes.
- `payload-len > 1 MiB` → decoder returns `Invalid` (peer must drop the connection).

### Op-codes

| Op   | Name         | Direction           | Payload                                                                 |
|------|--------------|---------------------|-------------------------------------------------------------------------|
| 0x01 | `OPEN`       | cloud → agent       | `{societyId, sipUsername, clientUA}` (JSON) — agent opens an Asterisk socket for this stream-id |
| 0x02 | `DATA`       | both ways           | raw SIP-WS frame bytes — opaque, byte-faithful                          |
| 0x03 | `CLOSE`      | both ways           | `{reason}` (JSON) — release stream-id                                   |
| 0x04 | `PING`       | agent → cloud       | empty — agent's `CloudConnector` sends one per 15 s of inbound silence; cloud's `SipBridge` echoes a `PONG` |
| 0x05 | `PONG`       | cloud → agent       | empty — answer to `PING`; agent drops + reconnects the tunnel after 3 consecutive unanswered `PING`s |
| 0x06 | `ERR`        | both ways           | `{code, msg}` — protocol violation; sender closes the tunnel            |
| 0x10 | `PUSH_NOTIFY`| agent → cloud       | `{subscriberId, callerFlat, callId}` — INVITE arrived, cloud should VAPID-push |
| 0x11 | `CDR_PUSH`   | agent → cloud       | BSON CDR doc — replicates finalized CDR to cloud for portal reads       |
| 0x12 | `SUBSCRIBER_REVOKED` | cloud → agent | `{societyId, sipUsername}` (JSON) — an admin disabled/removed the subscriber; agent hangs up that subscriber's live Asterisk channels via ARI. stream-id unused (0) |

Op-code 0x00 and any code outside the whitelist above causes `decode()` to return `Invalid` — protects against bit-flips in the version byte being mistaken for an unknown op.

### Public API

```cpp
namespace SipFrame {

constexpr std::uint8_t kVersion    = 0x01;
constexpr std::size_t  kHeaderSize = 10;
constexpr std::size_t  kMaxPayload = 1 * 1024 * 1024;  // 1 MiB

enum class Op : std::uint8_t {
  OPEN = 0x01, DATA = 0x02, CLOSE = 0x03,
  PING = 0x04, PONG = 0x05, ERR = 0x06,
  PUSH_NOTIFY = 0x10, CDR_PUSH = 0x11,
  SUBSCRIBER_REVOKED = 0x12,
};

struct Frame {
  Op            op;
  std::uint32_t stream_id;
  std::string   payload;
};

enum class Status { Ok, NeedMore, Invalid };

struct DecodeResult {
  Status      status;
  Frame       frame;         // valid only when status == Ok
  std::size_t consumed = 0;  // bytes consumed from the buffer
};

std::string  encode(Op op, std::uint32_t stream_id, const std::string& payload);
DecodeResult decode(const std::string& buf);

} // namespace SipFrame
```

`decode()` returns:

- `Ok` — a complete frame; caller advances by `consumed` bytes.
- `NeedMore` — header or payload not yet complete; caller leaves the buffer alone and retries after more bytes arrive.
- `Invalid` — protocol error; caller closes the tunnel (with `ERR` first, if it can).

### Why hand-rolled big-endian readers, not ACE_CDR?

`ACE_InputCDR` / `ACE_OutputCDR` default to **native byte order** with explicit `ACE_CDR::swap_4()` calls required for wire format. For an 8-byte integer field on either side of a 1-byte op code, the swap call overhead exceeds the value-add. The hand-rolled big-endian readers are six lines, branch-free, and trivially auditable. See [`DESIGN.md §12`](../../../DESIGN.md#12-reuse-map-dry-against-xpmile) note.

### Behaviour pinned by tests (`test/sip_frame_test.cc` — 11 tests)

Round-trip per op: `SerializeRoundTrip_Open`, `SerializeRoundTrip_Data` (binary-safe — NUL byte mid-payload), `SerializeRoundTrip_PingPong`, `SerializeRoundTrip_SubscriberRevoked`.

Error paths: `RejectsBadVersion`, `RejectsBadOp`, `RejectsTruncatedHeader`, `RejectsTruncatedPayload`.

Streaming framing: `HandlesPayloadAtBufferBoundary` (two frames concatenated, first `decode()` consumes only frame 1).

Safety: `MaxPayloadGuard` (1 MiB + 1 → `Invalid`).

Wire format: `WireFormat_IsBigEndian` (encoded `0x01020304` appears as bytes `01 02 03 04`).

---

---

## SipBridge — cloud-side multiplexer

Owns the in-memory mapping `stream_id → BrowserSink`. Driven by callbacks; performs no I/O itself — production wires it to ACE sockets, tests wire it to in-memory fakes. Threading: single-threaded on the ACE reactor thread; no internal locking.

### Abstract sinks (dependency injection)

```cpp
class TunnelSink {
public:
  virtual void send_frame(SipFrame::Op op, std::uint32_t stream_id,
                          const std::string& payload) = 0;
};

class BrowserSink {
public:
  virtual void send_bytes(const std::string& sip_ws_bytes) = 0;
  virtual void close(const std::string& reason) = 0;
};
```

In production: `TunnelSink` wraps an `ACE_SSL_SOCK_Stream` to the agent; `BrowserSink` wraps a `WebConnection`'s outbound side. In tests: `FakeTunnel` records every frame; `FakeBrowser` records bytes + close.

### Public API

```cpp
class SipBridge : public IRevocationSink {
public:
  explicit SipBridge(TunnelSink* tunnel);

  // Browser-side entry points
  std::uint32_t on_browser_upgrade(BrowserSink* browser, const std::string& open_meta);
  void          on_browser_data   (std::uint32_t stream_id, const std::string& bytes);
  void          on_browser_close  (std::uint32_t stream_id, const std::string& reason);

  // Agent-side entry points
  bool on_tunnel_bytes      (const std::string& bytes);  // false = drop tunnel
  void on_tunnel_disconnect ();
  void set_tunnel           (TunnelSink* tunnel);

  // Out-of-band tunnel ops (agent → cloud)
  using PushNotifyHandler = std::function<void(const std::string& payload)>;
  using CdrPushHandler    = std::function<void(const std::string& payload)>;
  void set_push_notify_handler(PushNotifyHandler);  // production: PushSender::notify
  void set_cdr_push_handler   (CdrPushHandler);     // production: Mongo CDR writer

  // Out-of-band tunnel ops (cloud → agent) — IRevocationSink
  void revoke(const std::string& society_id,
              const std::string& sip_username) override;  // emits SUBSCRIBER_REVOKED

  // Observability (test surface)
  std::size_t   active_streams () const;
  std::uint32_t last_stream_id () const;
};
```

`SipBridge` is the production [`IRevocationSink`](inc/revocation_sink.hpp) — `revoke()` is the seam the cloud's `MicroServicePbx::handle_subscriber_*` lifecycle handlers call when an admin disables/removes a subscriber. It encodes a `SUBSCRIBER_REVOKED` frame (stream-id 0, JSON `{societyId, sipUsername}`) and writes it down the tunnel. No-op when no tunnel is attached.

### Behaviour

- **OPEN on upgrade.** `on_browser_upgrade` allocates the next stream-id (monotonic counter, starts at 1), stores the browser sink, emits an `OPEN` frame with the JSON metadata payload, returns the assigned id.
- **DATA both ways.** Browser bytes → `DATA` frame on tunnel. Tunnel `DATA` frame → routed to the matching `BrowserSink::send_bytes()`. Unknown stream-ids on either side are dropped quietly (don't echo back as `ERR` — that surface belongs to a malicious-peer model we don't need yet).
- **CLOSE semantics.**
  - Browser-initiated close (`on_browser_close`): emit `CLOSE` to agent, erase from map. **Do not** call `close()` on the browser sink — the caller owns that lifetime.
  - Agent-initiated close (incoming `CLOSE` frame): call `close(reason)` on the browser sink, erase from map.
- **PING / PONG.** The cloud is the heartbeat *responder*: an inbound `PING` is answered with a `PONG` carrying the same stream-id (0 for the tunnel-level heartbeat). The cloud does not originate `PING`s or track liveness — the agent's `CloudConnector` owns the heartbeat and drops + reconnects the tunnel on missed `PONG`s (DESIGN.md §7). An inbound `PONG` at the cloud is unexpected in v1 and dropped.
- **Partial frames.** `on_tunnel_bytes` accumulates into `m_recv_buffer` and decodes as many complete frames as fit. Tail bytes wait for the next call.
- **Invalid frames.** Returns `false`. The caller (cloud tunnel endpoint) drops the tunnel; reconnect logic kicks in.
- **Tunnel disconnect.** Closes every browser sink with reason `"tunnel_lost"`, clears the map and the buffer. The stream-id counter does **not** reset — old ids stay dead forever. New browsers after reconnect get fresh ids.

### Behaviour pinned by tests (`test/sip_bridge_test.cc` — 17 tests)

Upgrade: `OnBrowserUpgrade_AssignsStreamId`, `OnBrowserUpgrade_UniqueIdsUnderManyUpgrades` (1000 sequential upgrades, no collisions).

Browser→tunnel: `OnBrowserData_FramesAndForwards`, `OnBrowserData_UnknownStreamId_DroppedSilently`.

Tunnel→browser demux: `OnTunnelData_DemuxesByStreamId` (two streams, no cross-talk), `OnTunnelData_PartialFrameAcrossReads` (split frame across two recv() calls), `OnTunnelData_PingTriggersPong`, `OnTunnelData_InvalidFrameReturnsFalse`.

Close semantics: `OnBrowserClose_SendsCloseFrame` (asserts `BrowserSink::close()` is NOT called — caller owns), `OnTunnelCloseFrame_ClosesBrowserSink` (asserts the sink IS closed when agent initiated).

Lifecycle: `OnTunnelDisconnect_ClosesAllBrowserConns`, `OnAgentReconnect_NewStreamIdsOnly` (monotonic counter survives reconnect).

Out-of-band ops: `OnTunnelPushNotify_InvokesPushHandler`, `OnTunnelCdrPush_InvokesCdrHandler` — production wires these to `PushSender::notify` and a Mongo CDR writer respectively. End-to-end coverage in [Layer 3 TunnelE2E](../../../test/integration/tunnel_e2e_test.cc).

Revocation (`IRevocationSink`): `Revoke_EmitsSubscriberRevokedFrame` (a `SUBSCRIBER_REVOKED` frame with stream-id 0 and the `{societyId, sipUsername}` payload), `Revoke_NoTunnel_IsSilentNoOp` (agent disconnected → no crash, no frame), `Revoke_AfterReconnect_GoesToNewTunnel`.

> **Not yet pinned:** `HandoffOrdering` (TDD plan Layer 1) — this asserts the `remove_handler → m_handle = INVALID → publish to bridge` ordering in `WebConnection`. The test belongs to the [`webservice/`](../webservice/README.md) module's suite, lands when that module is copied from xpmile.

---

---

## MicroServicePbx — REST handlers

Free functions in the `MicroServicePbx::` namespace, each of shape `(const std::string& req, IMongodbClient& db) -> std::string`. They follow xpmile's `MicroService::handle_account_login_POST` style: take a raw HTTP request, return a raw HTTP response. The choice of free functions (vs methods on `MicroService`) keeps the xpmile-copied `webservice/` module untouched and locates all PBX-specific logic in our own module.

Wired into `MicroService::process_request()` by URI prefix when the webservice slice patches it in (a small targeted patch — not part of this commit).

### Handlers

| URI                                      | Method | Function                                  | Behaviour |
|------------------------------------------|--------|-------------------------------------------|-----------|
| `/api/v1/society`                        | `POST` | `handle_society_POST`                     | Create society; generates `sipRealm`, `turnSharedSecret`, defaults `maxConcurrentCalls=5`, `ringTimeoutSec=30`. 409 on duplicate `code`. |
| `/api/v1/subscriber/import?societyId=…`  | `POST` | `handle_subscriber_import_POST`           | CSV body (`flat_number,name,email,phone,role`). Generates `sipUsername`, `sipPassword`, `portalPassword` per row; stores **only** `sipHa1` (MD5(user:realm:pwd)) and `portalPasswordHash` (bcrypt), plus a denormalized `flatNumber` (the directory filters/displays on it). Returns the plaintexts in a downloadable CSV (one-shot). Idempotent on `(societyId, email)`. 400 with row index if a flat is unknown. |
| `/api/v1/subscriber?societyId=…&flatPrefix=…` | `GET` | `handle_directory_GET`                 | Society-scoped subscriber directory; optional case-insensitive `flatPrefix` filter on the denormalized `flatNumber`. Strips `portalPasswordHash` and `sipHa1` from every row. 400 if `societyId` is missing; `[]` when no DB is configured. |
| `/api/v1/subscriber/<sipUsername>?societyId=…` | `PUT` | `handle_subscriber_status_PUT`        | Admin disable/re-enable. Body `{status:"active"\|"disabled"}` flips `subscribers.status` for the (societyId, sipUsername) row. Disabling also **revokes**: deletes every `sessions` row for the subscriber and, when an `IRevocationSink` is wired, signals the agent (`SUBSCRIBER_REVOKED`) to hang up any live call. 400 bad path/query/body, 404 unknown. |
| `/api/v1/subscriber/<sipUsername>?societyId=…` | `DELETE` | `handle_subscriber_DELETE`          | Admin removal. Deletes the subscriber doc, then revokes exactly as the disable path does (`sessions` purge + `IRevocationSink`). 400/404 as above. |
| `/api/v1/cdr?societyId=…`                | `GET`  | `handle_cdr_GET`                          | Returns the society's CDR rows as JSON. 400 if `societyId` is missing. |
| `/api/v1/push/subscribe`                 | `POST` | `handle_push_subscribe_POST`              | Persists a Web Push subscription (`subscriberId`, `endpoint`, `p256dh`, `auth`). 400 on missing fields. |
| `/api/v1/subscriber/login`               | `POST` | `handle_subscriber_login_POST`            | Body `{email, password}`. Strict mode (`PBX_AUTH_STRICT=1`): Mongo lookup by email + bcrypt `verify_password` + `status=="active"`; dev mode synthesises a profile. On success writes a `sessions` row and replies with a `Set-Cookie: session=…; HttpOnly; Secure; SameSite=Strict` header + `{token, subscriber}` body. 401/403 on bad/disabled. |
| `/sip-ws` (pre-upgrade)                  | `GET`  | `handle_sipws_upgrade`                    | Resolves the `?token=` query param (or `session=` cookie) against the `sessions` collection. Returns a `SipWsUpgrade{error, open_meta}`: `error` is a 401 for an absent/unknown/expired session; in **strict mode** it also re-checks the backing `subscribers` row and returns 403 if the subscriber was disabled or removed after the session was minted (the subscriber-lifecycle gate — a disabled user can't keep `/sip-ws` for the rest of the 24h TTL). Otherwise `open_meta` is the `{societyId, sipUsername, clientUA}` JSON for the bridge's OPEN frame. |

The two lifecycle handlers take an optional `IRevocationSink*` (default `nullptr`). `MicroService::dispatch_pbx_routes` passes the cloud's `SipBridge`; the routing unit tests (no owning `WebServer`) pass `nullptr` — the handlers still do their DB-side revocation, just without the cloud→agent frame.

### Crypto

- **sipHa1**: `MD5(sipUsername : sipRealm : sipPassword)` via OpenSSL `EVP_md5`. Asterisk's `auth_type=md5` consumes this directly; we never store the plaintext SIP password.
- **portalPasswordHash**: `MongodbClient::hash_password()` (xpmile-provided bcrypt) for the portal login.
- **Random secrets**: `OPENSSL_RAND_bytes` over a 62-char alphanumeric alphabet (fallback to `std::random_device` if `RAND_bytes` fails). `sipPassword`/`portalPassword` are 16 chars, `sipUsername` is 10 chars prefixed `u_`, `turnSharedSecret` is 32 chars.

### Behaviour pinned by tests (`test/microservice_pbx_test.cc` — 36 tests)

`MicroServicePbx.SocietyCreate_201`, `SocietyCreate_DuplicateCode_409`.

`SubscriberImport_GeneratesCreds` — asserts `sipHa1` + `portalPasswordHash` + the denormalized `flatNumber` ARE in the inserted doc, plaintext `sipPassword` and `portalPassword` are NOT.

`SubscriberImport_RejectsBadFlat` — error body names the offending row index and flat number.

`SubscriberImport_Idempotent` — re-importing a row whose email already exists adds a `skipped` line and does not insert again.

`Directory_FiltersByFlatPrefix` — case-insensitive `flatPrefix` filter on `flatNumber`; `Directory_StripsSecrets` — `portalPasswordHash`/`sipHa1` never in the response; `Directory_MissingSocietyId_400`.

`CdrList_FiltersBySociety`, `CdrList_MissingSocietyId_400`.

`PushSubscribe_PersistsEndpoint`, `PushSubscribe_RejectsMissingFields`.

`SubscriberLogin_DevMode_AcceptsAnyCredentials`, `SubscriberLogin_MissingField_400` — login contract + dev mode; the dev case also asserts a `Set-Cookie` header and a `sessions` insert whose row carries the body's token.

`SubscriberLogin_Strict_ValidCredentials_200` (real bcrypt round-trip; `portalPasswordHash`/`sipHa1` stripped from the response; `sessions` row persisted), `SubscriberLogin_Strict_WrongPassword_401`, `SubscriberLogin_Strict_UnknownEmail_401`, `SubscriberLogin_Strict_DisabledAccount_403`.

`Auth_RejectsAnonymousSipWsUpgrade` (no token → 401), `Auth_AllowsSipWsUpgrade_WithSessionCookie` (seeded `sessions` row via cookie → `open_meta` resolved), `SipWsUpgrade_ResolvesSubscriberMeta_FromQueryToken` (the UI's `?token=` path), `SipWsUpgrade_RejectsUnknownToken`, `SipWsUpgrade_RejectsExpiredSession`.

Subscriber lifecycle — disable/re-enable/remove: `SubscriberStatus_Disable_FlipsStatus_DropsSessions_Revokes` (the three-part revocation: `$set status`, `sessions` delete, `IRevocationSink::revoke`), `SubscriberStatus_Reenable_FlipsStatusOnly_NoRevoke`, `SubscriberStatus_NullRevokeSink_StillDoesDbWork`, `SubscriberStatus_UnknownSubscriber_404`, `SubscriberStatus_MissingSocietyId_400`, `SubscriberStatus_BadStatusValue_400`, `SubscriberDelete_RemovesDoc_DropsSessions_Revokes`, `SubscriberDelete_UnknownSubscriber_404`, `SubscriberDelete_MissingSocietyId_400`.

Subscriber-lifecycle gate on `/sip-ws` (strict mode): `SipWsUpgrade_Strict_RejectsDisabledSubscriber`, `SipWsUpgrade_Strict_RejectsRemovedSubscriber` (no backing `subscribers` row → 403), `SipWsUpgrade_Strict_AllowsActiveSubscriber`, `SipWsUpgrade_DevMode_SkipsSubscriberStatusCheck` (dev-mode sessions have no `subscribers` row — the gate must be skipped).

Tests use a per-suite `TestDb` (subclass of `IMongodbClient`) that maps `(collection, query-fragment)` to a canned response — small extension over xpmile's single-result `MockMongodbClient` because MicroServicePbx handlers issue several different queries per call (society lookup + flat lookup + duplicate-email check). It records `inserts`, `updates`, and `deletes` for write-side assertions.

---

---

## PushSender — VAPID Web Push trigger

Receives `PUSH_NOTIFY` frames from the agent (eventually — wired through `SipBridge`'s `PUSH_NOTIFY` dispatch in a later slice), looks up `push_subscriptions` for the target subscriber, signs a VAPID JWT (RFC 8292), encrypts the payload (RFC 8291), and POSTs to each browser's push endpoint with retry + 410 cleanup. Cryptographic + policy core only; HTTP I/O and time-of-day are dependency-injected.

### Dependency injection

```cpp
class IPushHttpClient {
public:
  struct Response { int status; std::string body; };
  virtual Response post(const std::string& url,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::string& body) = 0;
};

class IClock { public: virtual std::int64_t now_unix() const = 0; };
```

Production wires `IPushHttpClient` to an ACE_SSL_SOCK_Connector wrapper (lands with the cloud's eventual reactor binding); tests use `FakeHttpClient` that scripts per-attempt responses and records every call. `IClock` lets `VapidJwt_ExpiresIn12hMax` pin "now" deterministically.

### VAPID JWT (RFC 8292)

- Header: `{"typ":"JWT","alg":"ES256"}`
- Claims: `{"aud":"<endpoint origin>","exp":<now + 12 h>,"sub":"mailto:..."}`
- Signature: ECDSA P-256 over `b64url(header) . b64url(claims)`, encoded as the **JOSE 64-byte `r||s`** (not OpenSSL's ASN.1 DER form — converted via `ECDSA_SIG_get0` + `BN_bn2binpad`).
- Authorization header value: `vapid t=<jwt>, k=<server-pub-b64url>`.

The server's VAPID public key is derived once at construction from the configured private PEM and cached as `vapid_public_b64url()`.

### Payload encryption (RFC 8291 / RFC 8188 aes128gcm)

For each push:

1. Generate a **fresh ephemeral ECDH P-256 keypair** (RFC 8291 §3.3 forbids reuse).
2. ECDH shared secret = `ECDH(server_ephemeral_priv, subscriber.p256dh)`.
3. `IKM = HKDF-SHA256(salt=auth, IKM=shared, info="WebPush: info" || 0x00 || subscriber_pub || server_pub, 32)`.
4. Random 16-byte content-encoding salt.
5. `CEK   = HKDF-SHA256(salt, IKM, "Content-Encoding: aes128gcm" || 0x00, 16)`.
6. `NONCE = HKDF-SHA256(salt, IKM, "Content-Encoding: nonce"       || 0x00, 12)`.
7. Append `0x02` record terminator to plaintext (RFC 8188 §2 single-record encoding).
8. Encrypt with AES-128-GCM(CEK, NONCE).
9. Frame as `salt(16) | rs=4096 BE(4) | idlen=65(1) | server_pub(65) | ciphertext||tag(16)`.

The decryption path (`push_crypto::decrypt_payload_for_testing`) is the inverse using the same OpenSSL primitives — test-only.

### Retry / cleanup policy

| HTTP response | Action |
|---|---|
| 2xx | Counted as delivered. |
| 410 Gone | Subscription removed from Mongo (`delete_document` on `push_subscriptions`). No retry. |
| 503 | Exponential backoff up to `Config::max_retries` (default 3); doubles each retry. `initial_backoff_ms = 0` in tests so they don't sleep. |
| Anything else | Give up on this subscription; keep iterating others. |

### Public API

```cpp
class PushSender {
public:
  struct Config { /* vapid_private_pem, vapid_subject, jwt_exp_seconds,
                      max_retries, initial_backoff_ms */ };
  PushSender(Config, IPushHttpClient&, IMongodbClient&, IClock&);

  int  notify(const std::string& subscriber_id, const std::string& payload);
  std::string build_vapid_auth(const std::string& origin, std::int64_t now) const;
  std::string sign_vapid_jwt  (const std::string& origin, std::int64_t now) const;
  std::string encrypt_payload (const std::string& payload,
                               const std::string& p256dh_b64url,
                               const std::string& auth_b64url) const;
  const std::string& vapid_public_b64url() const;
};

namespace push_crypto {
  std::string b64url_encode(const std::string&);
  std::string b64url_decode(const std::string&);
  struct P256KeyPair { std::string private_pem, public_uncompressed; };
  P256KeyPair p256_generate();
  std::string p256_public_from_pem(const std::string& private_pem);
  std::string rand_bytes(std::size_t n);
  std::string decrypt_payload_for_testing(const std::string& private_pem,
                                          const std::string& auth,
                                          const std::string& record);
}
```

### Behaviour pinned by tests (`test/push_sender_test.cc` — 8 tests)

VAPID: `VapidJwt_HasCorrectAudience` (parsing the signed JWT confirms `aud = endpoint origin`), `VapidJwt_ExpiresIn12hMax` (`exp - now ≤ 12 h`), `BuildVapidAuth_IncludesPublicKeyAndToken` (header value contains `vapid t=…` and `, k=<pub>`).

Encryption: `EncryptsPayloadAes128Gcm` — full round-trip. Generate a "browser" P-256 keypair + 16-byte auth secret, encrypt with `PushSender::encrypt_payload`, decrypt with `push_crypto::decrypt_payload_for_testing`, assert `==` against the original plaintext. Also asserts the wire-format `idlen=65` framing byte.

Retry / cleanup: `RetriesOn503` (FakeHttpClient scripts `[503,503,201]` — observe 3 calls, 1 delivered), `GivesUpAfterMaxRetries` (4 × 503 → 4 calls then give up), `DropsSubscriptionOn410Gone` (410 → no retry, `db.delete_document` called with the right `_id` filter).

Edge: `NoSubscriptions_NotifyReturnsZero`.

---

## CloudTunnelEndpoint — cloud-side accept-end of the tunnel

Mirror of agent-side [`CloudConnector`](../../../pbx-agent/README.md#cloudconnector). Where `CloudConnector` actively dials the cloud, `CloudTunnelEndpoint` is **fed** a connected transport after `WebConnection`'s `/agent` WebSocket-upgrade hand-off. Implements `TunnelSink` so `SipBridge` and the cloud's `PushSender` can write frames upstream without knowing about ACE_SSL or WS framing.

Owned by `WebServer` and exposed via `WebServer::cloudTunnelEndpoint()` (parallel to xpmile's `wsDbServer()` accessor). Bound to the cloud's `SipBridge` instance at startup via `endpoint.attach_bridge(&bridge)`.

### Dependency injection

```cpp
class IAgentTransport {  // identical shape to agent-side ITransport
public:
  virtual bool send (const std::string& bytes) = 0;
  virtual void close() = 0;
};
```

Production: an ACE_SSL_SOCK_Stream wrapper installed by `WebConnection::handle_input` after the `/agent` WS upgrade. Tests: in-memory `FakeAgentTransport` whose state lives in a separate struct outside the `unique_ptr`, same fake-side-channel pattern as `CloudConnector` / `SipFrameDemux`.

### Public API

```cpp
class CloudTunnelEndpoint : public TunnelSink {
public:
  struct Config { std::size_t outbound_buffer_max = 0; };  // 0 = unbounded
  CloudTunnelEndpoint();
  explicit CloudTunnelEndpoint(Config);

  void attach_bridge(SipBridge*);

  // External signals (production: ACE event handler that owns the agent fd)
  void on_agent_connected   (std::unique_ptr<IAgentTransport>);
  void on_bytes_received    (const std::string&);
  void on_agent_disconnected();

  // TunnelSink
  void send_frame(SipFrame::Op, std::uint32_t, const std::string&) override;

  bool        has_agent()            const;
  std::size_t buffered_frame_count() const;
};
```

### Behaviour

Symmetric to `CloudConnector`'s state machine, accept-side:

- **`on_agent_connected`** — install the transport; flush any frames buffered while no agent was attached. If a previous agent is somehow still hanging on, tear it down first so the bridge sees a clean disconnect.
- **`on_bytes_received`** — forward to `SipBridge::on_tunnel_bytes`. If the bridge reports a protocol violation, drop the agent (close transport + tell bridge `on_tunnel_disconnect`).
- **`on_agent_disconnected`** — close transport, tell the bridge `on_tunnel_disconnect` (which propagates to every registered `BrowserSink::close("tunnel_lost")`). The **outbound buffer survives** — frames produced during the outage flush on the next agent attach. The bridge's stream-id counter does NOT reset (DESIGN.md §7).
- **`send_frame`** (`TunnelSink`) — if connected, `SipFrame::encode` + write. If the write fails mid-flight, treat as disconnect and re-queue the failing frame. If no agent, buffer.

### Behaviour pinned by tests (`test/cloud_tunnel_endpoint_test.cc` — 12 tests)

Lifecycle: `NoAgent_BeforeAttach`, `AcceptsAgentAndExposesHasAgent`, `OnAgentDisconnected_TellsBridge` (verifies `transport.close()` was called, every registered `BrowserSink` was closed, and the stream-id counter survived the reconnect).

Frame writes: `SendFrameWhenConnected_WritesEncodedBytes` (round-trip-decode confirms wire bytes), `SendFrameWhenDisconnected_Buffers`, `ReconnectAgentFlushesBuffer`, `AgentDisconnect_BufferSurvivesForNextAgent`, `SendMidFlightFailure_MarksDisconnectedAndBuffers`.

Inbound: `OnBytesReceived_ForwardsIntoBridge` (full PING→PONG round-trip through `endpoint → bridge → endpoint → fake transport`), `OnBytesReceived_InvalidFrame_DropsTunnel`, `OnBytesReceived_NoBridge_NoOp`.

Browser end-to-end: `BrowserDataRoundTripsThroughBridgeAndAgent` — a browser registers with `bridge.on_browser_upgrade` (OPEN frame lands on the agent transport), sends a SIP REGISTER (DATA frame lands), and the agent replies with `401 Unauthorized` which `on_bytes_received` demuxes back to the browser's `FakeBrowser::got`. Both halves wired together without any real network.

### Deferred to Layer 3

The concrete ACE event handler that wraps the agent fd, decodes the inbound WS frames, calls `endpoint.on_bytes_received(payload)`, and exposes an `IAgentTransport` that encodes outbound WS frames. Same scope boundary as the agent's `AceSslTransport` — both are cheap to verify end-to-end against a real ACE reactor.

---

## BrowserStream — ACE event handler for the browser's `/sip-ws` socket

The first concrete reactor binding. Lives between the browser's WebSocket and the cloud's `SipBridge`. Inherits both `ACE_Event_Handler` (so the reactor delivers read events) and `BrowserSink` (so the bridge can write back).

### Construction & lifetime

```cpp
// In WebConnection::handle_input, after the WS 101 response is sent:
auto *bs = new BrowserStream(*bridge, raw_fd, open_meta);
bs->reactor(reactor());
bs->register_with_reactor();   // READ_MASK + keep-alive ping timer
```

- Constructor calls `bridge.on_browser_upgrade(this, open_meta)` and stores the returned stream-id.
- `register_with_reactor()` registers for `READ_MASK` **and** arms the keep-alive timer (see below). On partial failure it rolls the registration back, so the caller can `delete` the stream safely.
- ACE owns the lifetime once registered. `handle_close` calls `bridge.on_browser_close(...)` and `delete this`.
- `close()` (called by the bridge to release the stream) sends a WS close frame, closes the socket, and marks the stream as already-notified so the eventual `handle_close` doesn't double-call into the bridge map.

### Frame handling

Uses xpmile's `wsframe::{encode, decode, ping_frame, pong_frame, close_frame}`. On `handle_input`:

| Inbound WS opcode | Action |
|---|---|
| `0x1` (text) / `0x2` (binary) / `0x0` (continuation) | `bridge.on_browser_data(stream_id, payload)` |
| `0x9` (ping) | reply with `wsframe::pong_frame(payload)` |
| `0xA` (pong) | swallowed (heartbeat ack) |
| `0x8` (close) | echo a close frame back, `bridge.on_browser_close(stream_id, "browser_closed")`, return `-1` so ACE calls `handle_close` |
| anything else | protocol violation; same close path with reason `"ws_protocol_error"` |

Outbound (`send_bytes`): encode as a WS **text** frame (RFC 7118 §2 mandates text for SIP-over-WS), unmasked (server→client), and `send_n` to the socket.

### Keep-alive

Heroku's router H15-drops a WebSocket after 55s with no bytes in either direction, and a SIP call can sit idle far longer than that mid-conversation. `register_with_reactor()` schedules a recurring reactor timer; `handle_timeout` sends an unmasked WS **ping** every 25s (≥ two pings per idle window) to keep the `/sip-ws` socket alive. A failed ping write tears the stream down via `notify_close_once` + `-1`. The timer is cancelled (idempotently) from `handle_close`, `close()`, and the destructor so the reactor can never fire `handle_timeout` on a freed object.

### Behaviour pinned by tests (`test/browser_stream_test.cc` — 9 tests)

Tests use `socketpair(AF_UNIX, SOCK_STREAM)` so `ACE_SOCK_Stream::recv` reads from a real fd. No reactor needed — `handle_input` (and `handle_timeout`) are invoked directly by the test driver.

- `ConstructorRegistersStreamWithBridge`, `OnInput_DecodesAndDispatches`, `OnInput_PingTriggersPong`, `HandleTimeout_EmitsWsPing`, `OnInput_CloseFrame_TellsBridge`, `SendBytes_EncodesWsFrame`, `Close_SendsWsCloseFrame`, `MultipleFramesInOneRead`, `PartialFrameAcrossReads`.

### `/sip-ws` swap in WebConnection

The Layer 1 / Layer 2 503 stub is retired. The new flow in `WebConnection::handle_input`:

1. Resolve the portal session — `handle_sipws_upgrade(request, *mongodbcInst())` looks the `?token=`/cookie up in the `sessions` collection. A null Mongo client or an absent/unknown/expired session → send the error response and close. On success it yields the OPEN-frame `open_meta`.
2. If `WebServer::cloudTunnelEndpoint()` is null, `sipBridge()` is null, or the agent isn't connected → 503 with `X-PBX-AgentConnected:` + `X-PBX-Hint:` headers.
3. Otherwise: 101 Switching Protocols + xpmile-mechanic hand-off ordering (`remove_handler → m_handle=INVALID`) → construct `BrowserStream` on the raw fd with the resolved `open_meta` → register on the same reactor → release this WebConnection from the pool.

---

## AgentStream — ACE event handler for the cloud's `/agent` socket

Symmetric mirror of `BrowserStream`. Owns the cloud-side `/agent` fd after the WS upgrade; exposes itself to `CloudTunnelEndpoint` via a private `TransportAdapter` (because the endpoint takes a `std::unique_ptr<IAgentTransport>` while ACE owns the stream's lifetime).

### Adapter lifetime invariant

The `TransportAdapter` lives inside the endpoint's `unique_ptr`. `AgentStream` holds a non-owning back-pointer `m_adapter`. Both release paths null the pointer **before** the adapter is destroyed, so it's never dereferenced after death:

- **Endpoint-initiated release** (e.g. invalid `SipFrame` payload → `bridge.on_tunnel_bytes` returned false → `endpoint.mark_disconnected`): endpoint calls `m_transport->close()` → `adapter.close()` → `AgentStream::close_socket()` which nulls `m_adapter` and closes the fd. Then `endpoint.mark_disconnected` calls `m_transport.reset()` — adapter destroyed.
- **Peer-initiated release** (close frame or EOF on socket): `AgentStream::notify_disconnect_once()` detaches the adapter (`adapter.m_s = nullptr`), nulls `m_adapter`, then calls `endpoint.on_agent_disconnected()`. The endpoint's own teardown sees a no-op adapter close and resets the unique_ptr.

`m_close_notified` guards `notify_disconnect_once` so it runs at most once across the destructor + `handle_close` + peer-close paths.

### Frame handling

Identical opcode table to `BrowserStream`:

| Inbound WS opcode | Action |
|---|---|
| text / binary / continuation | `endpoint.on_bytes_received(payload)` — if endpoint released us mid-loop (`m_handle == INVALID`), bail out with `-1` so ACE calls `handle_close` |
| ping | reply with `wsframe::pong_frame(payload)` |
| pong | swallowed |
| close | echo close frame, `notify_disconnect_once()`, return `-1` |
| anything else | protocol error; same release path |

Outbound (`write_bytes`, invoked by adapter): encode as WS text (RFC 7118), unmasked.

### Keep-alive

Same H15 problem as `BrowserStream`: `register_with_reactor()` arms a recurring 25s timer and `handle_timeout` sends an unmasked WS **ping** so an otherwise-idle `/agent` tunnel stays alive between SIP traffic bursts. A failed ping write calls `notify_disconnect_once()` and returns `-1`; the timer is cancelled (idempotently) from `handle_close`, `close_socket`, and the destructor.

### Behaviour pinned by tests (`test/agent_stream_test.cc` — 10 tests)

Tests use `socketpair()` so reads come from a real fd; `handle_input` (and `handle_timeout`) are invoked directly. Coverage:

- `ConstructorAttachesToEndpoint` — endpoint's `has_agent()` flips true on construction, back to false on destruction.
- `OnInput_ForwardsToEndpoint` — masked WS text frame from peer carries a `SipFrame::DATA` payload; round-trips through the endpoint → bridge → `BrowserSink::send_bytes`.
- `OnInput_PingTriggersPong` — ping bytes from peer → unmasked PONG (`0x8A`) bytes on the other socket end.
- `HandleTimeout_EmitsWsPing` — firing the keep-alive timer writes an unmasked WS ping (`0x89`) to the socket.
- `OnInput_CloseFrame_DisconnectsEndpoint` — peer-side WS close → endpoint released.
- `EndpointSendFrame_WritesWsFrameToSocket` — `endpoint.send_frame(CDR_PUSH, …)` → unmasked WS text frame on the peer side, decoded payload round-trip-matches the original `SipFrame::encode` bytes.
- `EndpointReleasesTransport_ClosesSocket` — `endpoint.on_agent_disconnected()` → AgentStream's fd is closed (subsequent `recv` on the AgentStream side fails).
- `MultipleFramesInOneRead`, `PartialFrameAcrossReads` — same boundary cases as the BrowserStream suite.
- `OnInput_InvalidSipFrame_DropsAgent` — malformed `SipFrame` payload inside a WS text frame triggers the endpoint-initiated release path; `handle_input` returns `-1` so ACE cleans up.

### `/agent` swap in WebConnection

Mirrors the `/sip-ws` swap: on detected `/agent` WS upgrade:
1. Send `101 Switching Protocols` + `Sec-WebSocket-Accept`.
2. xpmile-mechanic hand-off (`remove_handler → m_handle=INVALID`).
3. Construct an `AgentStream` on the raw fd (which immediately calls `endpoint.on_agent_connected`).
4. `as->reactor(reactor()) + as->register_with_reactor()` (READ_MASK + keep-alive ping timer).
5. `connectionPool().erase(raw)`.

The previous Layer 2 "info log only" stub is now retired — `has_agent()` flips true the moment the agent's WS upgrade completes.

---

## Files

```
pbx/
  inc/
    sip_frame.hpp             # encode/decode API, op enum, status enum
    sip_bridge.hpp            # SipBridge + TunnelSink + BrowserSink interfaces
    microservice_pbx.hpp      # MicroServicePbx:: REST handlers (namespace API)
    push_sender.hpp           # PushSender + IPushHttpClient + IClock + push_crypto::
    cloud_tunnel_endpoint.hpp # CloudTunnelEndpoint + IAgentTransport
    tunnel_sink.hpp           # shared interface used by both SipBridge and CloudTunnelEndpoint
  src/
    sip_frame.cpp             # ~90 lines, hand-rolled big-endian impl
    sip_bridge.cpp            # ~100 lines, in-memory multiplexer
    microservice_pbx.cpp      # ~300 lines, society/subscriber/cdr/push/upgrade
    push_sender.cpp           # ~400 lines, VAPID JWT + RFC 8291 + retry policy
    cloud_tunnel_endpoint.cpp # ~70 lines, accept-side state machine
  test/
    sip_frame_test.cc           # 10 tests
    sip_bridge_test.cc          # 12 tests
    microservice_pbx_test.cc    # 11+7 tests (+ MicroServiceRouting suite)
    push_sender_test.cc         #  8 tests
    cloud_tunnel_endpoint_test.cc # 12 tests
```

## Dependencies

- C++20 standard library (`<cstdint>`, `<string>`, `<unordered_map>`, `<sstream>`, …).
- [`http/`](../http/README.md) — `Http` parser for REST handlers.
- [`mongodb/`](../mongodb/README.md) — `IMongodbClient` interface + `MongodbClient::hash_password`.
- [`webservice/`](../webservice/README.md) — `MicroService::build_response*` helpers (transitive; not yet directly used by MicroServicePbx but lives in the same `offtarget` binary).
- `nlohmann/json` (single header in `modules/module/thirdparty/`).
- OpenSSL (`-lcrypto`) for `EVP_md5`, `RAND_bytes`, `EVP_aes_128_gcm`, ECDSA/ECDH P-256, HKDF-SHA256.
- GTest (tests only).
- **No ACE yet.** SipBridge and MicroServicePbx are intentionally I/O-free; the production wrappers that bind to the ACE reactor land when `webservice/` is patched to route by URI prefix.

## Build & test

```sh
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer2 \
  --gtest_filter='SipFrame.*:SipBridge.*:MicroServicePbx.*:MicroServiceRouting.*:PushSender.*:CloudTunnelEndpoint.*'
```

Expected: **60/60 PASSED** (10 SipFrame + 12 SipBridge + 11 MicroServicePbx + 7 MicroServiceRouting + 8 PushSender + 12 CloudTunnelEndpoint).
