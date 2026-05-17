#ifndef PJSIP_PROVISIONER_HPP
#define PJSIP_PROVISIONER_HPP

#include "ari_client.hpp"   // IAriRest

#include <string>

/**
 * @file pjsip_provisioner.hpp
 * @brief Push pjsip endpoint+aor sorcery objects into Asterisk.
 *
 * A subscriber row in Mongo
 *   `{societyId, sipUsername, sipHa1, status, ...}`
 * is materialised in Asterisk as **two** sorcery-memory objects via
 * ARI dynamic-config (DESIGN.md §4):
 *
 *   - `aor      <sipUsername>-aor`  — `max_contacts=5`, `remove_existing=yes`.
 *   - `endpoint <sipUsername>`      — codecs / ICE / DTLS / WebRTC settings,
 *                                      `aors=<sipUsername>-aor`. No `auth` —
 *                                      SIP digest is intentionally disabled
 *                                      (the cloud `/sip-ws` bearer-token
 *                                      gate is the only auth layer; the
 *                                      browser has no SIP password and the
 *                                      WS transport can only be opened by
 *                                      a previously-authenticated session).
 *
 * Provision also issues a best-effort DELETE on the legacy
 * `<sipUsername>-auth` doc so existing Asterisk sorcery state from
 * pre-fix deployments is pruned on the first re-provision pass.
 *
 * Each object is created via PUT (sorcery treats it as create-or-replace,
 * so re-provisioning the same subscriber is idempotent — handy for the
 * agent's bootstrap full-scan and for change-stream replay).
 *
 * deprovision() does the reverse — three DELETEs in safe order
 * (endpoint first, so an in-flight INVITE finds no peer; then auth + aor;
 * the auth delete is a no-op on freshly-provisioned subscribers, kept
 * for legacy cleanup as above).
 *
 * Pure logic — `IAriRest` is injected. ARI errors are logged-and-dropped
 * (best-effort): a transient ARI failure shouldn't crash the watcher.
 *
 * The endpoint field set inlines what `pjsip.conf`'s
 * `endpoint-resident-template` declares — sorcery dynamic-config does NOT
 * inherit `(!)`-marked templates from the config file, so the values that
 * matter (codecs, transport, DTLS, WebRTC) live here as constants.
 */
class PjsipProvisioner {
public:
  /// @param rest      ARI REST surface used for the dynamic-config PUTs.
  /// @param sip_realm Society's SIP realm, used in the auth's `realm`
  ///                  field (must match the realm Asterisk advertises in
  ///                  its 401 challenge — see DESIGN.md §5).
  PjsipProvisioner(IAriRest &rest, std::string sip_realm);

  /// Push or refresh the aor + endpoint sorcery objects for one subscriber.
  /// `sip_username` is the pjsip endpoint's resource id. `sip_ha1` is
  /// ignored — PR #77 dropped digest auth so the param stays only to
  /// keep SubscriberWatcher call sites untouched. No-op for empty
  /// `sip_username`.
  void provision(const std::string &sip_username,
                 const std::string &sip_ha1);

  /// Remove the three sorcery objects for one subscriber. Order is
  /// endpoint → auth → aor so an in-flight INVITE finds no peer first.
  /// Already-deleted objects (404) are silent no-ops.
  void deprovision(const std::string &sip_username);

  /// Replace the SIP realm used in every subsequent `provision()`.
  /// Production driver: the agent's `SOCIETY_BOOTSTRAP` handler calls
  /// this with the canonical sipRealm the cloud read from the
  /// `societies` doc, so the `--sip-realm` CLI flag stops being a
  /// correctness dependency. Already-provisioned subscribers keep
  /// their old realm until `SubscriberWatcher::resync()` re-PUTs them.
  void set_sip_realm(const std::string &realm) { m_sip_realm = realm; }

  /// Test/observability: the realm currently in use.
  const std::string &sip_realm() const { return m_sip_realm; }

private:
  IAriRest   &m_rest;
  std::string m_sip_realm;
};

#endif // PJSIP_PROVISIONER_HPP
