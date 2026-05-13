#ifndef MICROSERVICE_PBX_HPP
#define MICROSERVICE_PBX_HPP

#include "mongodbc.hpp"
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

/// Pre-upgrade auth check for /sip-ws. The cloud only allows SIP-WS
/// upgrades from browsers with a valid portal session cookie — defence in
/// depth so anonymous browsers cannot open a path to Asterisk through the
/// tunnel.
///
/// @return Empty string if the upgrade may proceed; otherwise a complete
/// 401 Unauthorized response that the caller should send and then close
/// the socket.
std::string handle_sipws_upgrade(const std::string &req);

} // namespace MicroServicePbx

#endif // MICROSERVICE_PBX_HPP
