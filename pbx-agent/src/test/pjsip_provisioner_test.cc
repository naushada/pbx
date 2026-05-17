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

TEST(PjsipProvisioner, Provision_PutsAorAndEndpoint_PlusLegacyAuthCleanup)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.provision("u_alice", "deadbeefdeadbeefdeadbeefdeadbeef");

    // Two PUTs: aor, endpoint — auth is no longer provisioned because
    // SIP digest is disabled (cloud `/sip-ws` bearer is the only auth).
    ASSERT_EQ(2u, rest.puts.size());
    EXPECT_EQ("res_pjsip", rest.puts[0].cfg_class);
    EXPECT_EQ("aor",       rest.puts[0].obj_type);
    EXPECT_EQ("u_alice-aor",  rest.puts[0].id);
    EXPECT_EQ("endpoint",  rest.puts[1].obj_type);
    EXPECT_EQ("u_alice",      rest.puts[1].id);

    // Best-effort cleanup of any legacy `<user>-auth` doc left by
    // pre-fix deployments — fires every provision; 404 is silently
    // absorbed by the ARI client.
    ASSERT_EQ(1u, rest.deletes.size());
    EXPECT_EQ("auth",         rest.deletes[0].obj_type);
    EXPECT_EQ("u_alice-auth", rest.deletes[0].id);
}

TEST(PjsipProvisioner, Provision_EndpointFieldsMatchTemplate_AndCarryNoAuth)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");
    p.provision("u_alice", "ha1");

    auto ep = parse_fields(rest.puts[1].fields_json);
    // Endpoint references the sibling aor by id.
    EXPECT_EQ("u_alice-aor",  ep["aors"]);
    // No `auth` field — SIP digest is intentionally disabled.
    EXPECT_EQ(ep.end(), ep.find("auth"))
        << "endpoint must not reference an auth doc — the browser has no "
        << "SIP password (see ui/src/common/sip-ua-sipjs.ts:18). "
        << "A non-empty auth field would put Asterisk into an "
        << "unanswerable 401 challenge loop.";
    // WebRTC/DTLS settings inlined from pjsip.conf's
    // endpoint-resident-template (sorcery doesn't inherit (!)-templates).
    EXPECT_EQ("yes",          ep["webrtc"]);
    EXPECT_EQ("transport-ws", ep["transport"]);
    EXPECT_EQ("dtls",         ep["media_encryption"]);
    EXPECT_EQ("opus,ulaw,alaw", ep["allow"]);
    EXPECT_EQ("pbx",          ep["context"]);
}

TEST(PjsipProvisioner, Provision_EmptySipUsername_IsNoOp)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.provision("", "ha1");

    EXPECT_TRUE(rest.puts.empty())    << "must not push partial state";
    EXPECT_TRUE(rest.deletes.empty()) << "must not even attempt the legacy "
                                         "auth cleanup for empty sip_username";
}

TEST(PjsipProvisioner, Provision_EmptySipHa1_StillProvisions)
{
    // PR #77 dropped digest auth — `sip_ha1` is ignored. An empty value
    // must NOT short-circuit provisioning (the regression that broke
    // SIP REGISTER for every resident on 2026-05-17, hotfixed here).
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.provision("u_alice", "");  // sip_ha1 deliberately empty

    EXPECT_EQ(2u, rest.puts.size())    << "aor + endpoint PUTs must still happen";
    EXPECT_EQ(1u, rest.deletes.size()) << "legacy auth DELETE still attempted";
}

TEST(PjsipProvisioner, Deprovision_DeletesEndpointFirst_ThenAuthAndAor)
{
    FakeAriRest rest;
    PjsipProvisioner p(rest, "soc1.pbx.local");

    p.deprovision("u_alice");

    // Three DELETEs: endpoint first (so an in-flight INVITE finds no
    // peer), then auth (legacy cleanup — no-op on freshly-provisioned
    // subscribers), then aor.
    ASSERT_EQ(3u, rest.deletes.size());
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

    EXPECT_EQ(4u, rest.puts.size())
        << "each pass re-PUTs aor + endpoint (sorcery itself absorbs "
        << "the duplicate as a no-op).";
    EXPECT_EQ(2u, rest.deletes.size())
        << "each pass also issues the legacy `<user>-auth` cleanup "
        << "DELETE (404 silently absorbed).";
}

TEST(PjsipProvisioner, SipRealm_AccessorAndSetter)
{
    // Realm machinery is preserved for now (SOCIETY_BOOTSTRAP still
    // pokes it via set_sip_realm, --sip-realm CLI flag still exists)
    // but no longer drives provisioning since digest auth is off.
    // The accessor is still useful for observability and for a future
    // cleanup PR that removes the dead realm plumbing.
    FakeAriRest rest;
    PjsipProvisioner p(rest, "default.pbx.local");
    EXPECT_EQ("default.pbx.local", p.sip_realm());

    p.set_sip_realm("acme.pbx.local");
    EXPECT_EQ("acme.pbx.local", p.sip_realm());
}
