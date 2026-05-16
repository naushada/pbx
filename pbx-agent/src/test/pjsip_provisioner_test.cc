#include "pjsip_provisioner.hpp"
#include "json.hpp"

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

class FakeAriRest : public IAriRest {
public:
  struct DynPut    { std::string cfg_class, obj_type, id, fields_json; };
  struct DynDelete { std::string cfg_class, obj_type, id; };
  std::vector<DynPut>    puts;
  std::vector<DynDelete> deletes;

  Response create_dynamic_config(const std::string &cfg_class,
                                  const std::string &obj_type,
                                  const std::string &id,
                                  const std::string &fields_json) override {
    puts.push_back({cfg_class, obj_type, id, fields_json});
    return {200, ""};
  }
  Response delete_dynamic_config(const std::string &cfg_class,
                                  const std::string &obj_type,
                                  const std::string &id) override {
    deletes.push_back({cfg_class, obj_type, id});
    return {204, ""};
  }

  // Unused IAriRest surface — return placeholders.
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
  Response list_endpoints(const std::string &) override {
    return {200, "[]"};
  }
};

// Pull the {attribute,value} array out of a fields-body JSON into a
// flat map for compact assertions.
std::unordered_map<std::string, std::string>
parse_fields(const std::string &body) {
  std::unordered_map<std::string, std::string> out;
  json j = json::parse(body);
  for (const auto &f : j.at("fields"))
    out[f.at("attribute").get<std::string>()] = f.at("value").get<std::string>();
  return out;
}

} // namespace

TEST(PjsipProvisioner, Provision_PutsAuthAorEndpoint_InOrder)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.provision("u_alice", "deadbeefdeadbeefdeadbeefdeadbeef");

    // Three PUTs: auth, aor, endpoint — in that dependency order.
    ASSERT_EQ(3u, rest.puts.size());
    EXPECT_EQ("res_pjsip", rest.puts[0].cfg_class);
    EXPECT_EQ("auth",      rest.puts[0].obj_type);
    EXPECT_EQ("u_alice-auth", rest.puts[0].id);
    EXPECT_EQ("aor",       rest.puts[1].obj_type);
    EXPECT_EQ("u_alice-aor",  rest.puts[1].id);
    EXPECT_EQ("endpoint",  rest.puts[2].obj_type);
    EXPECT_EQ("u_alice",      rest.puts[2].id);

    EXPECT_TRUE(rest.deletes.empty());
}

TEST(PjsipProvisioner, Provision_AuthFieldsCarryRealmAndHa1)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");
    p.provision("u_alice", "abcd1234abcd1234abcd1234abcd1234");

    auto auth = parse_fields(rest.puts[0].fields_json);
    EXPECT_EQ("auth",                              auth["type"]);
    EXPECT_EQ("md5",                               auth["auth_type"]);
    EXPECT_EQ("soc1.pbx.local",                    auth["realm"]);
    EXPECT_EQ("u_alice",                           auth["username"]);
    EXPECT_EQ("abcd1234abcd1234abcd1234abcd1234",  auth["md5_cred"]);
}

TEST(PjsipProvisioner, Provision_EndpointFieldsMatchTemplate)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");
    p.provision("u_alice", "ha1");

    auto ep = parse_fields(rest.puts[2].fields_json);
    // The endpoint references its sibling auth + aor by id.
    EXPECT_EQ("u_alice-auth", ep["auth"]);
    EXPECT_EQ("u_alice-aor",  ep["aors"]);
    // WebRTC/DTLS settings inlined from pjsip.conf's
    // endpoint-resident-template (sorcery doesn't inherit (!)-templates).
    EXPECT_EQ("yes",          ep["webrtc"]);
    EXPECT_EQ("transport-ws", ep["transport"]);
    EXPECT_EQ("dtls",         ep["media_encryption"]);
    EXPECT_EQ("opus,ulaw,alaw", ep["allow"]);
    EXPECT_EQ("pbx",          ep["context"]);
}

TEST(PjsipProvisioner, Provision_EmptyInputs_AreNoOps)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.provision("",         "ha1");
    p.provision("u_alice",  "");

    EXPECT_TRUE(rest.puts.empty())    << "must not push partial state";
    EXPECT_TRUE(rest.deletes.empty());
}

TEST(PjsipProvisioner, Deprovision_DeletesEndpointFirst_ThenAuthAndAor)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.deprovision("u_alice");

    ASSERT_EQ(3u, rest.deletes.size());
    // Endpoint first so an in-flight INVITE finds no peer immediately.
    EXPECT_EQ("endpoint",     rest.deletes[0].obj_type);
    EXPECT_EQ("u_alice",      rest.deletes[0].id);
    EXPECT_EQ("auth",         rest.deletes[1].obj_type);
    EXPECT_EQ("u_alice-auth", rest.deletes[1].id);
    EXPECT_EQ("aor",          rest.deletes[2].obj_type);
    EXPECT_EQ("u_alice-aor",  rest.deletes[2].id);

    EXPECT_TRUE(rest.puts.empty());
}

TEST(PjsipProvisioner, Deprovision_EmptyUsername_NoOp)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");
    p.deprovision("");
    EXPECT_TRUE(rest.deletes.empty());
}

TEST(PjsipProvisioner, ProvisionTwice_IdempotentReplace)
{
    // ARI dynamic-config PUT is create-or-replace, so re-provisioning
    // the same subscriber is the idempotent retry path used by the
    // bootstrap full-scan and change-stream replay.
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.provision("u_alice", "ha1");
    p.provision("u_alice", "ha1");

    EXPECT_EQ(6u, rest.puts.size())
        << "second pass re-PUTs all three sorcery objects (sorcery itself "
        << "absorbs the duplicate as a no-op).";
    EXPECT_TRUE(rest.deletes.empty());
}

TEST(PjsipProvisioner, SetSipRealm_AppliesToSubsequentProvisions)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "default.pbx.local");
    EXPECT_EQ("default.pbx.local", p.sip_realm());

    // First provision uses the ctor realm.
    p.provision("u_alice", "ha1");
    auto first_auth = parse_fields(rest.puts[0].fields_json);
    EXPECT_EQ("default.pbx.local", first_auth["realm"]);
    rest.puts.clear();

    // SOCIETY_BOOTSTRAP arrives — agent calls set_sip_realm() with the
    // canonical value the cloud read out of the societies doc.
    p.set_sip_realm("acme.pbx.local");
    EXPECT_EQ("acme.pbx.local", p.sip_realm());

    // Next provision uses the new realm.
    p.provision("u_bob", "ha1");
    auto next_auth = parse_fields(rest.puts[0].fields_json);
    EXPECT_EQ("acme.pbx.local", next_auth["realm"])
        << "set_sip_realm must apply to subsequent provisions — otherwise "
        << "SubscriberWatcher::resync() couldn't correct the realm of "
        << "already-bootstrap'd subscribers";
}
