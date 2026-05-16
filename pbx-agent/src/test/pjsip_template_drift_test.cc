// Drift-check test: the `[endpoint-resident-template]` section in
// `docker/asterisk/pjsip.conf` and the endpoint field set
// `PjsipProvisioner::provision()` emits MUST stay aligned.
//
// Why this matters: sorcery dynamic-config (the ARI surface that
// PjsipProvisioner pushes to) does NOT inherit `(!)`-marked templates
// from pjsip.conf. The runtime-provisioned subscribers therefore
// re-inline every setting the template carries. If pjsip.conf's
// template gets a new codec / DTLS tweak / WebRTC flag and the
// provisioner doesn't, dev-fixture endpoints (alice/bob/conf, see
// pjsip.conf) and dynamically-provisioned subscribers silently
// behave differently — and the bug shows up as "calls work for
// alice but not for u_<rand>".
//
// The test is asymmetric: every field the template lists must appear
// with the same value in the provisioner's emitted endpoint. The
// reverse is NOT required — the provisioner adds per-subscriber refs
// (`auth=<sip_user>-auth`, `aors=<sip_user>-aor`) that a template
// wouldn't carry.
//
// `allow=` is special: pjsip.conf can list it once per codec
// (chan_pjsip accumulates), the provisioner uses one comma-separated
// value. The parser normalises both shapes to the same set before
// comparing.

#include "pjsip_provisioner.hpp"
#include "json.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {

// ─── Minimal IAriRest recorder — captures the endpoint PUT body. ──────────

class FakeAriRest : public IAriRest {
public:
  std::string last_endpoint_body;
  Response create_dynamic_config(const std::string &/*cfg_class*/,
                                  const std::string &obj_type,
                                  const std::string &/*id*/,
                                  const std::string &fields_json) override {
    if (obj_type == "endpoint") last_endpoint_body = fields_json;
    return {200, ""};
  }
  // Unused IAriRest surface.
  Response delete_dynamic_config(const std::string &, const std::string &,
                                  const std::string &) override { return {204, ""}; }
  Response subscribe(const std::string &,
                     const std::vector<std::string> &) override { return {204, ""}; }
  Response continue_in_dialplan(const std::string &, const std::string &,
                                 const std::string &, int) override { return {200, ""}; }
  Response originate(const std::string &, const std::string &,
                     const std::string &, const std::string &,
                     const std::string &) override { return {200, ""}; }
  Response create_bridge(const std::string &, const std::string &) override {
    return {200, ""};
  }
  Response add_channel_to_bridge(const std::string &,
                                  const std::string &) override {
    return {204, ""};
  }
  Response hangup(const std::string &, const std::string &) override {
    return {204, ""};
  }
  Response get_endpoint(const std::string &, const std::string &) override {
    return {200, "{}"};
  }
  Response list_endpoints(const std::string &) override { return {200, "[]"}; }
};

// ─── pjsip.conf source location + skip-on-missing helper. ─────────────────

// Test runs under Dockerfile.test which COPYs docker/ into /src/. On dev
// machines the test container isn't used; the test skips rather than
// fails so a `cmake; ctest` from a checkout doesn't false-positive.
constexpr const char *kPjsipConfPath = "/src/docker/asterisk/pjsip.conf";

bool read_file(const std::string &path, std::string &out) {
  std::ifstream f(path);
  if (!f.is_open()) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// Trim leading/trailing whitespace (incl. line continuations from `\`).
std::string trim(const std::string &s) {
  std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// ─── Parse one named `[section]` from a pjsip.conf-style file. ────────────
//
// Returns a multimap (vector of (attr, value)) so repeated keys like
// `allow=` are preserved in source order. Skips comment lines (`;`),
// blank lines, and the section header itself. Stops at the next
// `[section]` header.
//
// Intentionally not a full Asterisk config parser — just enough to
// extract a flat key=value list from one named template.
std::vector<std::pair<std::string, std::string>>
parse_section(const std::string &conf, const std::string &section_name) {
  std::vector<std::pair<std::string, std::string>> out;
  std::istringstream iss(conf);
  std::string line;
  bool in_section = false;
  while (std::getline(iss, line)) {
    const std::string l = trim(line);
    if (l.empty() || l[0] == ';') continue;
    if (l[0] == '[') {
      // `[name]` or `[name](!)` or `[name](!,parent)` — match by name only.
      const std::size_t close = l.find(']');
      if (close == std::string::npos) continue;
      const std::string name = l.substr(1, close - 1);
      in_section = (name == section_name);
      continue;
    }
    if (!in_section) continue;
    const std::size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    out.push_back({trim(l.substr(0, eq)), trim(l.substr(eq + 1))});
  }
  return out;
}

// ─── Decode the PjsipProvisioner endpoint PUT body. ───────────────────────

std::unordered_map<std::string, std::string>
endpoint_fields_from_body(const std::string &body) {
  std::unordered_map<std::string, std::string> out;
  json j = json::parse(body);
  for (const auto &f : j.at("fields"))
    out[f.at("attribute").get<std::string>()] =
        f.at("value").get<std::string>();
  return out;
}

// Split comma-separated codec list "opus,ulaw,alaw" → {"opus","ulaw","alaw"}
std::unordered_set<std::string> split_csv(const std::string &csv) {
  std::unordered_set<std::string> out;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (!item.empty()) out.insert(item);
  }
  return out;
}

} // namespace

// ─── The drift check. ─────────────────────────────────────────────────────

TEST(PjsipTemplateDrift, EndpointResidentTemplate_MatchesProvisionerFields)
{
    std::string conf;
    if (!read_file(kPjsipConfPath, conf)) {
        GTEST_SKIP() << "pjsip.conf not found at " << kPjsipConfPath
                     << " — runs only inside Dockerfile.test (which "
                        "COPYs docker/ into the image).";
    }

    // 1. Parse the [endpoint-resident-template] section into a list of
    //    (attr, value) pairs in source order.
    auto template_fields = parse_section(conf, "endpoint-resident-template");
    ASSERT_FALSE(template_fields.empty())
        << "didn't parse anything from [endpoint-resident-template] — "
           "either the section moved or the parser regressed";

    // 2. Run PjsipProvisioner against a fake ARI; capture the endpoint PUT.
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    prov.provision("test_user", "deadbeef");
    ASSERT_FALSE(rest.last_endpoint_body.empty())
        << "PjsipProvisioner didn't emit an endpoint PUT";
    const auto prov_fields = endpoint_fields_from_body(rest.last_endpoint_body);

    // 3. Subset check: every template field's value must appear in the
    //    provisioner's emitted fields. `type` is allowed to match in either
    //    direction (both emit `type=endpoint`). `allow` needs codec-set
    //    equality, not a string match (template = N "allow=" lines,
    //    provisioner = one comma-separated value).
    std::unordered_set<std::string> template_allow_codecs;
    for (const auto &[attr, value] : template_fields) {
      if (attr == "allow") {
        template_allow_codecs.insert(value);
        continue;
      }
      auto it = prov_fields.find(attr);
      ASSERT_NE(prov_fields.end(), it)
          << "pjsip.conf [endpoint-resident-template] carries `" << attr
          << "` but PjsipProvisioner doesn't emit it. Either add it to "
          << "pjsip_provisioner.cpp's endpoint field list, or drop it "
          << "from the template.";
      EXPECT_EQ(value, it->second)
          << "value mismatch for `" << attr << "`: pjsip.conf has \""
          << value << "\", PjsipProvisioner has \"" << it->second << "\"";
    }

    if (!template_allow_codecs.empty()) {
      auto it = prov_fields.find("allow");
      ASSERT_NE(prov_fields.end(), it)
          << "template lists `allow=` codecs but provisioner emits none";
      auto prov_codecs = split_csv(it->second);
      EXPECT_EQ(template_allow_codecs, prov_codecs)
          << "codec set differs between pjsip.conf and PjsipProvisioner";
    }
}
