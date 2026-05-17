#include "pjsip_provisioner.hpp"

#include "json.hpp"

#include "ace/Log_Msg.h"

#include <utility>

using json = nlohmann::json;

namespace {

constexpr char kCfgClass[]    = "res_pjsip";

// Sorcery dynamic-config bodies follow ARI's
//   { "fields": [ {"attribute": "...", "value": "..."}, ... ] }
// shape. Build the array from a vector of (attr, value) pairs to keep
// the call sites readable.
std::string make_fields_body(
    const std::vector<std::pair<std::string, std::string>> &fields) {
  json arr = json::array();
  for (const auto &kv : fields)
    arr.push_back({{"attribute", kv.first}, {"value", kv.second}});
  json body = {{"fields", arr}};
  return body.dump();
}

} // namespace

PjsipProvisioner::PjsipProvisioner(IAriRest &rest, std::string sip_realm)
    : m_rest(rest), m_sip_realm(std::move(sip_realm)) {}

void PjsipProvisioner::provision(const std::string &sip_username,
                                  const std::string & /* sip_ha1 */) {
  // PR #77 dropped digest auth; sip_ha1 is no longer used. Param stays
  // to keep the SubscriberWatcher call sites untouched. Only sip_username
  // can block provisioning now — an empty one means the subscriber doc
  // is genuinely malformed.
  if (sip_username.empty()) {
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [pbx-agent] PjsipProvisioner::provision skipped "
                        "(empty sip_username) — malformed subscriber doc\n")));
    return;
  }

  const std::string auth_id = sip_username + "-auth";
  const std::string aor_id  = sip_username + "-aor";

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [pbx-agent] PjsipProvisioner::provision sip_user=%s "
                      "(realm=%s) — DELETE legacy auth, PUT aor + endpoint\n"),
             sip_username.c_str(), m_sip_realm.c_str()));

  // SIP digest auth is intentionally NOT provisioned. The browser has
  // no password (see ui/src/common/sip-ua-sipjs.ts:18); the cloud
  // `/sip-ws` upgrade authenticates by bearer token, and only that
  // tunnel can reach this Asterisk. A second digest layer would
  // refuse REGISTER unanswerably. Legacy `<user>-auth` docs from
  // pre-fix deployments are pruned best-effort below so re-provisioning
  // an existing subscriber drops the stale auth association on its
  // first pass — Asterisk returns 404 for already-gone, which the
  // ARI client treats as a silent no-op.
  const auto del_auth = m_rest.delete_dynamic_config(kCfgClass, "auth", auth_id);
  if (del_auth.status != 0 && del_auth.status != 204 && del_auth.status != 404)
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] PjsipProvisioner::provision sip_user=%s "
                        "auth DELETE returned %d body=%s\n"),
               sip_username.c_str(), del_auth.status, del_auth.body.c_str()));

  const auto put_aor = m_rest.create_dynamic_config(
      kCfgClass, "aor", aor_id,
      make_fields_body({
          {"type",            "aor"},
          {"max_contacts",    "5"},
          {"remove_existing", "yes"},
      }));
  if (put_aor.status < 200 || put_aor.status >= 300)
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] PjsipProvisioner::provision sip_user=%s "
                        "aor PUT returned %d body=%s\n"),
               sip_username.c_str(), put_aor.status, put_aor.body.c_str()));

  // The endpoint inlines everything `endpoint-resident-template` carries
  // in pjsip.conf — sorcery dynamic objects don't inherit (!)-marked
  // templates from the config file, so the codecs / ICE / DTLS / WebRTC
  // settings have to be repeated here. Keep these in sync with
  // docker/asterisk/pjsip.conf when that file changes.
  const auto put_endpoint = m_rest.create_dynamic_config(
      kCfgClass, "endpoint", sip_username,
      make_fields_body({
          {"type",                          "endpoint"},
          {"context",                       "pbx"},
          {"disallow",                      "all"},
          {"allow",                         "opus,ulaw,alaw"},
          {"direct_media",                  "yes"},
          {"rtcp_mux",                      "yes"},
          {"ice_support",                   "yes"},
          {"use_avpf",                      "yes"},
          {"force_avp",                     "yes"},
          {"media_use_received_transport",  "yes"},
          {"rewrite_contact",               "yes"},
          {"rtp_symmetric",                 "yes"},
          {"transport",                     "transport-ws"},
          {"webrtc",                        "yes"},
          {"media_encryption",              "dtls"},
          {"dtls_verify",                   "fingerprint"},
          {"dtls_setup",                    "actpass"},
          {"dtls_cert_file",                "/etc/asterisk/keys/pbx.crt"},
          {"dtls_private_key",              "/etc/asterisk/keys/pbx.key"},
          {"dtls_rekey",                    "0"},
          // `auth` field deliberately absent — see the comment at
          // the top of provision(). sip_ha1 is unused here too; we
          // accept it to preserve the public signature for now (a
          // future PR removes the parameter once the realm plumbing
          // is cleaned up too).
          {"aors",                          aor_id},
      }));
  if (put_endpoint.status < 200 || put_endpoint.status >= 300)
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] PjsipProvisioner::provision sip_user=%s "
                        "endpoint PUT returned %d body=%s\n"),
               sip_username.c_str(), put_endpoint.status, put_endpoint.body.c_str()));
}

void PjsipProvisioner::deprovision(const std::string &sip_username) {
  if (sip_username.empty()) return;

  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [pbx-agent] PjsipProvisioner::deprovision sip_user=%s "
                      "— DELETE endpoint + auth + aor\n"),
             sip_username.c_str()));

  // Endpoint first: an in-flight INVITE racing the delete should find no
  // peer rather than an endpoint that briefly references a deleted
  // auth/aor. Sorcery's 404 on already-deleted objects is fine.
  m_rest.delete_dynamic_config(kCfgClass, "endpoint", sip_username);
  m_rest.delete_dynamic_config(kCfgClass, "auth",     sip_username + "-auth");
  m_rest.delete_dynamic_config(kCfgClass, "aor",      sip_username + "-aor");
}
