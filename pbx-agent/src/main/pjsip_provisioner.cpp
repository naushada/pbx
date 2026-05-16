#include "pjsip_provisioner.hpp"

#include "json.hpp"

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
                                  const std::string &sip_ha1) {
  if (sip_username.empty() || sip_ha1.empty()) return;

  const std::string auth_id = sip_username + "-auth";
  const std::string aor_id  = sip_username + "-aor";

  // Order: auth + aor first (the endpoint references both), then endpoint.
  // Asterisk tolerates the reverse but creating dependents-first is the
  // sorcery norm and avoids a brief window where endpoint exists pointing
  // at not-yet-created auth/aor.
  m_rest.create_dynamic_config(
      kCfgClass, "auth", auth_id,
      make_fields_body({
          {"type",      "auth"},
          {"auth_type", "md5"},
          {"realm",     m_sip_realm},
          {"username",  sip_username},
          {"md5_cred",  sip_ha1},
      }));

  m_rest.create_dynamic_config(
      kCfgClass, "aor", aor_id,
      make_fields_body({
          {"type",            "aor"},
          {"max_contacts",    "5"},
          {"remove_existing", "yes"},
      }));

  // The endpoint inlines everything `endpoint-resident-template` carries
  // in pjsip.conf — sorcery dynamic objects don't inherit (!)-marked
  // templates from the config file, so the codecs / ICE / DTLS / WebRTC
  // settings have to be repeated here. Keep these in sync with
  // docker/asterisk/pjsip.conf when that file changes.
  m_rest.create_dynamic_config(
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
          {"auth",                          auth_id},
          {"aors",                          aor_id},
      }));
}

void PjsipProvisioner::deprovision(const std::string &sip_username) {
  if (sip_username.empty()) return;

  // Endpoint first: an in-flight INVITE racing the delete should find no
  // peer rather than an endpoint that briefly references a deleted
  // auth/aor. Sorcery's 404 on already-deleted objects is fine.
  m_rest.delete_dynamic_config(kCfgClass, "endpoint", sip_username);
  m_rest.delete_dynamic_config(kCfgClass, "auth",     sip_username + "-auth");
  m_rest.delete_dynamic_config(kCfgClass, "aor",      sip_username + "-aor");
}
