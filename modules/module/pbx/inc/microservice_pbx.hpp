#ifndef MICROSERVICE_PBX_HPP
#define MICROSERVICE_PBX_HPP

#include "mongodbc.hpp"
#include "presence_cache.hpp"
#include "revocation_sink.hpp"
#include <string>

/**
 * @file microservice_pbx.hpp
 * @brief Cloud-side REST handlers for the onprem-pbx control plane.
 *
 * Free functions, all of shape
 *   `std::string handler(const std::string& raw_http_request,
 *                        IMongodbClient& db);`
 * matching xpmile's `MicroService::handle_account_login_POST` style, which
 * keeps them unit-testable against `MockMongodbClient` without spinning up a
 * MongoDB.
 *
 * Wired into `MicroService::process_request()` by URI prefix in a later
 * slice (once webservice.cpp is patched). Today these are stand-alone and
 * the test suite drives them directly.
 *
 * See `DESIGN.md §4` for the Mongo schema each handler reads/writes and
 * `TDD-PLAN.md` Layer 1 §MicroServicePbx for behavioural contracts.
 */
namespace MicroServicePbx {

/// POST /api/v1/society
/// Body: `{"name":"<str>","code":"<str>","address":"<str>"}`
/// On success: 201 Created with the inserted doc (includes generated
/// `sipRealm`, `turnSharedSecret`, `maxConcurrentCalls`, `ringTimeoutSec`).
/// On duplicate `code`: 409 Conflict.
std::string handle_society_POST(const std::string &req, IMongodbClient &db);

/// GET /api/v1/societies
/// Lists every society as a JSON array. Each row is the persisted doc
/// with `turnSharedSecret` stripped — the shared secret is exposed
/// only via the per-society detail endpoint so a stray list call can't
/// leak it. Falls through to `[]` when `db_available()` is false
/// (no Mongo configured — same H12 guard the other GETs use).
std::string handle_societies_GET(const std::string &req, IMongodbClient &db);

/// GET /api/v1/society/<id>
/// Returns the full society doc for that `_id`, including the
/// `sipRealm` and `turnSharedSecret` the operator copies into the
/// on-prem `.env` / `setup-society.sh` flow. 400 if the id segment
/// is missing, 404 if no such society. (Admin-role gating is added
/// in a later PR — until then this is no-auth, matching every other
/// REST handler today.)
std::string handle_society_detail_GET(const std::string &req,
                                      IMongodbClient &db);

/// POST /api/v1/subscriber/import
/// Body: a CSV (Content-Type: text/csv) with header row
///   `flat_number,name,email,phone,role`
/// + query param `societyId=<id>`.
///
/// For each row: validates the flat exists, generates `sipUsername`,
/// `sipPassword`, and `portalPassword`, stores `sipHa1` (MD5) and
/// `portalPasswordHash` (bcrypt). Idempotent on (societyId, email):
/// re-running the import skips existing subscribers.
///
/// Response: CSV with the plaintext credentials (one-shot download — the
/// plaintext is never recoverable afterwards). 400 if a flat is missing
/// (body names the offending row index).
std::string handle_subscriber_import_POST(const std::string &req,
                                          IMongodbClient &db);

/// GET /api/v1/cdr?societyId=<id>
/// Returns the CDR rows for that society as a JSON array. 400 if
/// `societyId` query param is missing.
std::string handle_cdr_GET(const std::string &req, IMongodbClient &db);

/// POST /api/v1/push/subscribe
/// Body: `{"subscriberId":"...","endpoint":"...","p256dh":"...","auth":"..."}`
/// Persists the Web Push subscription. 201 Created on success, 400 on
/// malformed body.
std::string handle_push_subscribe_POST(const std::string &req,
                                       IMongodbClient &db);

/// POST /api/v1/subscriber/login
/// Body: `{"email":"...","password":"..."}` (portal login is email-keyed —
/// DESIGN.md §5; email is globally unique across societies).
/// Authenticates the subscriber and issues a bearer token (32-char hex,
/// 16 bytes from `RAND_bytes`).
///   - **Strict mode** (ENV `PBX_AUTH_STRICT=1`): looks the subscriber up
///     in the `subscribers` collection by email, verifies the password
///     against the stored bcrypt `portalPasswordHash`, and requires
///     `status == "active"`. The response profile has `portalPasswordHash`
///     and `sipHa1` stripped. Bad email/password → 401; disabled → 403.
///   - **Dev mode** (default, for deploys with no seeded Mongo data): any
///     non-empty `{email, password}` is accepted and a synthetic profile
///     is returned.
std::string handle_subscriber_login_POST(const std::string &req,
                                         IMongodbClient &db);

/// GET /api/v1/subscriber?societyId=<id>&flatPrefix=<prefix>
/// Projects each persisted `subscribers` row to the UI's
/// `DirectoryEntry { flatNumber, displayName, sipUri, online }` shape.
/// `online` reads from @p presence (the cloud-side `IPresenceCache`,
/// fed by `REGISTER_STATE` tunnel frames from the agent) — `false` for
/// any subscriber the cache hasn't seen yet. When @p presence is null
/// (e.g. `dispatch_pbx_routes` invoked from a routing unit test with
/// no owning `WebServer`), `online` is `false` for every row — same
/// safe default the UI's call button keys off.
std::string handle_directory_GET(const std::string &req, IMongodbClient &db,
                                 const IPresenceCache *presence = nullptr);

/// PUT /api/v1/subscriber/<sipUsername>?societyId=<id>
/// Body: `{"status":"active"|"disabled"}` — the admin disable/re-enable
/// switch. Flips `subscribers.status` for the (societyId, sipUsername)
/// row. Disabling also **revokes**: every `sessions` row for that
/// subscriber is deleted (so the bearer can't be re-resolved at
/// `/sip-ws`) and, when @p revoke is non-null, the agent is signalled
/// to hang up any call that is live right now. Re-enabling does neither.
///   - 200 OK on success (`{status, sipUsername, subscriberStatus}`)
///   - 400 missing sipUsername / societyId / bad body / bad status value
///   - 404 unknown subscriber in that society
std::string handle_subscriber_status_PUT(const std::string &req,
                                         IMongodbClient &db,
                                         IRevocationSink *revoke = nullptr);

/// DELETE /api/v1/subscriber/<sipUsername>?societyId=<id>
/// Permanently removes the (societyId, sipUsername) subscriber doc, then
/// revokes exactly as the disable path does (drops `sessions` rows,
/// signals @p revoke). 200 OK on success, 400 on missing path/query
/// params, 404 if no such subscriber.
std::string handle_subscriber_DELETE(const std::string &req,
                                     IMongodbClient &db,
                                     IRevocationSink *revoke = nullptr);

/// GET /api/v1/push-vapid-key
/// Returns `{"key": "<base64url public key>"}`. Source: the
/// `VAPID_PUBLIC_KEY` env var, populated by `deploy-heroku.sh` from
/// the generated VAPID keypair. Returns an empty key string if the
/// env var is unset (push will silently no-op on the UI side).
std::string handle_push_vapid_key_GET(const std::string &req,
                                      IMongodbClient &db);

/// GET /api/v1/turn-credentials
/// Mints time-limited TURN credentials per RFC 5766 §5:
///   username   = "<unix-now+ttl>:<sip-user>"
///   credential = base64(HMAC-SHA1(`societies.turnSharedSecret`, username))
/// Returns `{ urls, username, credential, ttlSec }`. The TURN_URL env
/// var supplies the relay address (e.g.
/// "turn:turn.example.local:3478?transport=udp"); ttl defaults to
/// 5 minutes.
std::string handle_turn_credentials_GET(const std::string &req,
                                        IMongodbClient &db);

/// GET /api/v1/ping
/// Lightweight health endpoint for the UI's keep-alive heartbeat.
/// Returns `{"ok": true, "ts": <unix-ms>}`. Does not touch the DB.
std::string handle_ping_GET(const std::string &req, IMongodbClient &db);

/// Result of the /sip-ws pre-upgrade auth check.
struct SipWsUpgrade {
  /// Non-empty → a complete HTTP error response the caller should send
  /// before closing the socket. When empty, the upgrade may proceed.
  std::string error;
  /// Valid only when `error` is empty: the JSON payload for the bridge's
  /// `OPEN` frame, identifying the subscriber behind this stream —
  /// `{"societyId","sipUsername","clientUA"}` (DESIGN.md §7).
  std::string open_meta;
};

/// Pre-upgrade auth + identity resolution for /sip-ws. The browser presents
/// its portal session as a `?token=` query param (browsers can't set headers
/// on `new WebSocket`) or a `session=` cookie; this resolves it against the
/// `sessions` collection, rejecting an absent / unknown / expired session,
/// and on success builds the `OPEN`-frame metadata from the session record.
SipWsUpgrade handle_sipws_upgrade(const std::string &req, IMongodbClient &db);

/// Result of the admin-only REST gate (see resolve_admin_session()).
struct AdminSession {
  /// Non-empty → a complete HTTP error response (401 or 403) the caller
  /// must return to the client INSTEAD of invoking the handler.
  /// Empty → the bearer is valid AND carries role=admin; the handler may run.
  std::string error;
  /// Valid only when `error` is empty: the resolved session identity,
  /// copied out of the `sessions` row.
  std::string society_id;
  std::string sip_username;
  std::string role;
};

/// Admin-role gate for the cloud's admin-only REST endpoints. Resolves the
/// portal session the same way handle_sipws_upgrade does (`?token=` query
/// param OR `session=` cookie), looks it up in `sessions`, and refuses any
/// request that isn't carrying a still-valid bearer whose row has
/// `role == "admin"`.
/// Returns:
///   error.empty()        → session valid + admin, identity fields populated
///   error: 401 response  → missing/unknown bearer, or expired session
///                          (expired rows are best-effort deleted)
///   error: 403 response  → bearer valid but role is not "admin"
AdminSession resolve_admin_session(const std::string &req, IMongodbClient &db);

} // namespace MicroServicePbx

#endif // MICROSERVICE_PBX_HPP
