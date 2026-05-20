#include "ari_client.hpp"
#include "call_router.hpp"
#include "mongodbc.hpp"
#include "json.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// FakeAriRest — records subscribe + continue calls.
// ─────────────────────────────────────────────────────────────────────────────

class FakeAriRest : public IAriRest {
public:
  struct SubscribeCall {
    std::string app;
    std::vector<std::string> sources;
  };
  struct ContinueCall {
    std::string channel_id;
    std::string context;
    std::string extension;
    int         priority;
  };
  struct HangupCall { std::string channel_id, reason; };
  struct EndpointCall { std::string tech, resource; };
  struct BridgeCall { std::string bridge_id, type; };
  struct AddChannelCall { std::string bridge_id, channel_id; };
  std::vector<SubscribeCall>   subscribes;
  std::vector<ContinueCall>    continues;
  std::vector<std::string>     originate_endpoints;
  std::vector<HangupCall>      hangups;
  std::vector<EndpointCall>    endpoint_lookups;
  std::vector<BridgeCall>      bridges;
  std::vector<AddChannelCall>  add_channels;
  Response                   continue_response{200, ""};
  Response                   subscribe_response{204, ""};
  Response                   get_endpoint_response{200, "{}"};

  Response subscribe(const std::string &app,
                      const std::vector<std::string> &sources) override {
    subscribes.push_back({app, sources});
    return subscribe_response;
  }

  Response continue_in_dialplan(const std::string &cid, const std::string &ctx,
                                 const std::string &ext, int prio) override {
    continues.push_back({cid, ctx, ext, prio});
    return continue_response;
  }

  // CallRouter's REST surface — recorded so AriClient↔CallRouter wiring
  // can be asserted; the routing logic itself is covered by
  // call_router_test.
  Response originate(const std::string &endpoint, const std::string &,
                      const std::string &, const std::string &,
                      const std::string &) override {
    originate_endpoints.push_back(endpoint);
    return {200, ""};
  }
  Response create_bridge(const std::string &bridge_id,
                          const std::string &type) override {
    bridges.push_back({bridge_id, type});
    return create_bridge_response;
  }
  Response add_channel_to_bridge(const std::string &bridge_id,
                                  const std::string &channel_id) override {
    add_channels.push_back({bridge_id, channel_id});
    return add_channel_response;
  }
  Response create_bridge_response{200, ""};
  Response add_channel_response{204, ""};
  Response hangup(const std::string &cid, const std::string &reason) override {
    hangups.push_back({cid, reason});
    return {204, ""};
  }
  Response get_endpoint(const std::string &tech,
                         const std::string &resource) override {
    endpoint_lookups.push_back({tech, resource});
    return get_endpoint_response;
  }

  // list_endpoints recording — presence-snapshot tests pre-set the
  // response body to a JSON array and then assert that
  // publish_register_snapshot drove one handler call per entry.
  std::vector<std::string> list_endpoint_techs;
  Response                 list_endpoints_response{200, "[]"};
  Response list_endpoints(const std::string &tech) override {
    list_endpoint_techs.push_back(tech);
    return list_endpoints_response;
  }

  // Dynamic-config recording — PjsipProvisioner tests assert on the
  // exact PUTs and DELETEs that hit Asterisk's sorcery surface.
  struct DynPut    { std::string cfg_class, obj_type, id, fields_json; };
  struct DynDelete { std::string cfg_class, obj_type, id; };
  std::vector<DynPut>    dyn_puts;
  std::vector<DynDelete> dyn_deletes;

  Response create_dynamic_config(const std::string &cfg_class,
                                  const std::string &obj_type,
                                  const std::string &id,
                                  const std::string &fields_json) override {
    dyn_puts.push_back({cfg_class, obj_type, id, fields_json});
    return {200, ""};
  }
  Response delete_dynamic_config(const std::string &cfg_class,
                                  const std::string &obj_type,
                                  const std::string &id) override {
    dyn_deletes.push_back({cfg_class, obj_type, id});
    return {204, ""};
  }
};

// Minimal IMongodbClient that records inserts. CDR is the only collection
// AriClient writes to.
class TestDb : public IMongodbClient {
public:
  struct Insert { std::string coll, doc; };
  std::vector<Insert> inserts;

  const std::string &get_database() const override { return m_db; }
  std::string get_document(const std::string &, const std::string &,
                            const std::string &) override { return {}; }
  std::string create_document(const std::string &, const std::string &coll,
                               const std::string &doc) override {
    inserts.push_back({coll, doc});
    return {};
  }
  std::int32_t create_bulk_document(const std::string &, const std::string &,
                                     const std::string &) override { return 0; }
  bool update_collection(const std::string &, const std::string &,
                          const std::string &) override { return false; }
  std::int32_t update_bulk_document(const std::string &,
                                     const std::vector<std::string> &,
                                     const std::vector<std::string> &) override {
    return 0;
  }
  bool delete_document(const std::string &, const std::string &) override {
    return false;
  }
  std::string get_documents(const std::string &, const std::string &,
                             const std::string &) override { return {}; }
  std::string get_documents(const std::string &,
                             const std::string &) override { return {}; }
  std::string next_awbno(const std::string &) override { return {}; }
  std::string store_file(const std::string &, const std::string &,
                          const std::vector<std::uint8_t> &) override { return {}; }
  std::vector<std::uint8_t> fetch_file(const std::string &) override { return {}; }
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &) override { return {}; }
  bool delete_file(const std::string &) override { return false; }

private:
  std::string m_db = "pbx";
};

// ─────────────────────────────────────────────────────────────────────────────
// Event builders. Match the ARI JSON shape Asterisk actually ships.
// ─────────────────────────────────────────────────────────────────────────────

std::string stasis_start(const std::string &channel_id,
                          const std::string &caller_flat,
                          const std::string &callee_flat) {
  json j;
  j["type"]                          = "StasisStart";
  j["application"]                   = "pbx";
  j["channel"]["id"]                 = channel_id;
  j["channel"]["state"]              = "Ring";
  j["channel"]["caller"]["number"]   = caller_flat;
  j["channel"]["dialplan"]["exten"]  = callee_flat;
  j["args"]                          = json::array({callee_flat});
  return j.dump();
}

// An originated leg re-entering Stasis on answer carries the
// "outbound,<callerChannelId>" appArgs CallRouter tagged it with.
std::string stasis_start_leg(const std::string &leg_channel_id,
                              const std::string &caller_channel_id) {
  json j;
  j["type"]             = "StasisStart";
  j["application"]      = "pbx";
  j["channel"]["id"]    = leg_channel_id;
  j["channel"]["state"] = "Up";
  j["args"]             = json::array({"outbound", caller_channel_id});
  return j.dump();
}

std::string bridge_created(const std::string &bridge_id,
                            const std::string &bridge_type = "mixing") {
  json j;
  j["type"]                  = "BridgeCreated";
  j["application"]           = "pbx";
  j["bridge"]["id"]          = bridge_id;
  j["bridge"]["bridge_type"] = bridge_type;
  return j.dump();
}

std::string bridge_destroyed(const std::string &bridge_id) {
  json j;
  j["type"]         = "BridgeDestroyed";
  j["application"]  = "pbx";
  j["bridge"]["id"] = bridge_id;
  return j.dump();
}

std::string channel_entered_bridge(const std::string &channel_id,
                                    const std::string &bridge_id) {
  json j;
  j["type"]          = "ChannelEnteredBridge";
  j["application"]   = "pbx";
  j["channel"]["id"] = channel_id;
  j["bridge"]["id"]  = bridge_id;
  return j.dump();
}

// Asterisk's EndpointStateChange — driven by REGISTER / qualify-loss for a
// pjsip endpoint. `state` is one of "online", "offline", "unknown".
std::string endpoint_state_change(const std::string &tech,
                                   const std::string &resource,
                                   const std::string &state) {
  json j;
  j["type"]                  = "EndpointStateChange";
  j["application"]           = "pbx";
  j["endpoint"]["technology"] = tech;
  j["endpoint"]["resource"]   = resource;
  j["endpoint"]["state"]      = state;
  return j.dump();
}

// Asterisk's ContactStatusChange — fires on the PJSIP contact
// lifecycle. `status` is one of "Created", "Removed", "Reachable",
// "Unreachable", "NonQualified", "Unknown".
std::string contact_status_change(const std::string &tech,
                                   const std::string &resource,
                                   const std::string &status) {
  json j;
  j["type"]                            = "ContactStatusChange";
  j["application"]                     = "pbx";
  j["endpoint"]["technology"]          = tech;
  j["endpoint"]["resource"]            = resource;
  j["contact_info"]["contact_status"]  = status;
  j["contact_info"]["aor"]             = resource;
  return j.dump();
}

std::string channel_destroyed(const std::string &channel_id,
                                const std::string &cause_txt = "Normal Clearing") {
  json j;
  j["type"]          = "ChannelDestroyed";
  j["application"]   = "pbx";
  j["channel"]["id"] = channel_id;
  j["cause"]         = 16;
  j["cause_txt"]     = cause_txt;
  return j.dump();
}

AriClient::Config default_cfg() {
  AriClient::Config c;
  c.society_id           = "s1";
  c.app_name             = "pbx";
  c.max_concurrent_calls = 5;
  c.busy_context         = "pbx-busy";
  c.busy_extension       = "s";
  c.busy_priority        = 1;
  return c;
}

} // namespace

// ── Start subscribes ─────────────────────────────────────────────────────────

TEST(AriClient, SubscribesToChannelBridgeEndpointEvents)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.start();

    ASSERT_EQ(1u, rest.subscribes.size());
    EXPECT_EQ("pbx", rest.subscribes[0].app);
    const auto &srcs = rest.subscribes[0].sources;
    // channel + bridge drive admission/CDR; endpoint drives the cloud's
    // presence cache via REGISTER_STATE frames.
    EXPECT_NE(srcs.end(),
              std::find(srcs.begin(), srcs.end(), "channel:"));
    EXPECT_NE(srcs.end(),
              std::find(srcs.begin(), srcs.end(), "bridge:"));
    EXPECT_NE(srcs.end(),
              std::find(srcs.begin(), srcs.end(), "endpoint:"));
}

// ── Bridge counter ───────────────────────────────────────────────────────────

TEST(AriClient, BridgeCreated_IncrementsActiveCount)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(bridge_created("br-1"));
    EXPECT_EQ(1, c.active_bridges());
    c.on_event(bridge_created("br-2"));
    EXPECT_EQ(2, c.active_bridges());
    // Duplicate BridgeCreated for the same id must not double-count.
    c.on_event(bridge_created("br-1"));
    EXPECT_EQ(2, c.active_bridges());
}

TEST(AriClient, BridgeDestroyed_DecrementsActiveCount_NeverNegative)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(bridge_created("br-1"));
    c.on_event(bridge_created("br-2"));
    EXPECT_EQ(2, c.active_bridges());

    c.on_event(bridge_destroyed("br-1"));
    EXPECT_EQ(1, c.active_bridges());
    c.on_event(bridge_destroyed("br-2"));
    EXPECT_EQ(0, c.active_bridges());

    // Extra BridgeDestroyed echo (Asterisk does this sometimes) must
    // not drive the counter negative.
    c.on_event(bridge_destroyed("br-1"));
    c.on_event(bridge_destroyed("ghost-bridge"));
    EXPECT_EQ(0, c.active_bridges());
}

// ── Admission cap ────────────────────────────────────────────────────────────

TEST(AriClient, AdmissionCap_ReturnsBusyAtFive)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);  // cap = 5

    // Five calls already up (bridges 1..5).
    for (int i = 1; i <= 5; ++i)
        c.on_event(bridge_created("br-" + std::to_string(i)));
    EXPECT_EQ(5, c.active_bridges());

    // 6th StasisStart arrives → admission denied → REST continue to busy.
    c.on_event(stasis_start("ch-6", "A-101", "B-204"));
    ASSERT_EQ(1u, rest.continues.size());
    EXPECT_EQ("ch-6",      rest.continues[0].channel_id);
    EXPECT_EQ("pbx-busy",  rest.continues[0].context);
    EXPECT_EQ("s",         rest.continues[0].extension);
    EXPECT_EQ(1,           rest.continues[0].priority);

    // No CDR row written for the rejected call (it never made it past
    // admission control — Asterisk's busy handler writes its own log).
    EXPECT_EQ(0u, db.inserts.size());
}

TEST(AriClient, AdmissionCap_AllowsUnderCap)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(bridge_created("br-1"));
    c.on_event(bridge_created("br-2"));
    c.on_event(stasis_start("ch-1", "A-101", "B-204"));

    EXPECT_TRUE(rest.continues.empty())
        << "channel must NOT be bounced to busy when under cap";
}

// ── Hangup writes CDR ────────────────────────────────────────────────────────

TEST(AriClient, HangupEvent_WritesCdr)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(stasis_start("ch-1", "A-101", "B-204"));
    c.on_event(bridge_created("br-1"));
    c.on_event(channel_entered_bridge("ch-1", "br-1"));
    c.on_event(channel_entered_bridge("ch-2", "br-1"));
    c.on_event(channel_destroyed("ch-1", "Normal Clearing"));

    ASSERT_EQ(1u, db.inserts.size());
    EXPECT_EQ("cdr", db.inserts[0].coll);

    const json cdr = json::parse(db.inserts[0].doc);
    EXPECT_EQ("s1",         cdr["societyId"]);
    EXPECT_EQ("ch-1",       cdr["callId"]);
    EXPECT_EQ("A-101",      cdr["fromFlat"]);
    EXPECT_EQ("B-204",      cdr["toFlat"]);
    EXPECT_EQ("normal",     cdr["hangupCause"]);
    EXPECT_EQ("p2p",        cdr["type"]);
    EXPECT_TRUE(cdr.contains("startedAt"));
    EXPECT_TRUE(cdr.contains("answeredAt"));
    EXPECT_TRUE(cdr.contains("endedAt"));
    EXPECT_TRUE(cdr.contains("durationSec"));
}

TEST(AriClient, HangupEvent_BusyCauseNormalised)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(stasis_start("ch-1", "A-101", "B-204"));
    c.on_event(channel_destroyed("ch-1", "User busy"));

    ASSERT_EQ(1u, db.inserts.size());
    const json cdr = json::parse(db.inserts[0].doc);
    EXPECT_EQ("busy", cdr["hangupCause"]);
}

TEST(AriClient, ChannelDestroyed_WithoutStasisStart_NoCdr)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // Stray ChannelDestroyed for a channel we never observed (e.g. it
    // never entered Stasis). Must NOT write a phantom CDR.
    c.on_event(channel_destroyed("ch-unknown"));
    EXPECT_EQ(0u, db.inserts.size());
}

// ── Conference detection ─────────────────────────────────────────────────────

TEST(AriClient, ConferenceBridgeEvents_TaggedAsConference)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // Three subscribers on the same flat join a conference bridge.
    c.on_event(stasis_start("ch-1", "A-204", "*204"));
    c.on_event(bridge_created("conf-204", "mixing"));
    c.on_event(channel_entered_bridge("ch-1", "conf-204"));
    c.on_event(channel_entered_bridge("ch-2", "conf-204"));
    c.on_event(channel_entered_bridge("ch-3", "conf-204"));

    c.on_event(channel_destroyed("ch-1"));

    ASSERT_EQ(1u, db.inserts.size());
    const json cdr = json::parse(db.inserts[0].doc);
    EXPECT_EQ("conference", cdr["type"]);
    EXPECT_EQ("conf-204",   cdr["conferenceBridge"]);
}

TEST(AriClient, TwoChannelsInBridge_StaysAsP2p)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(stasis_start("ch-1", "A-101", "B-204"));
    c.on_event(bridge_created("br-1", "mixing"));
    c.on_event(channel_entered_bridge("ch-1", "br-1"));
    c.on_event(channel_entered_bridge("ch-2", "br-1"));
    c.on_event(channel_destroyed("ch-1"));

    ASSERT_EQ(1u, db.inserts.size());
    const json cdr = json::parse(db.inserts[0].doc);
    EXPECT_EQ("p2p", cdr["type"]);
}

// ── CallRouter wiring ────────────────────────────────────────────────────────

TEST(AriClient, CallerStasisStart_DelegatedToRouter)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // TestDb resolves no subscribers, so CallRouter treats the dialed
    // extension as "no route" and hangs the caller channel up — which is
    // observable proof the caller StasisStart was handed to the router.
    c.on_event(stasis_start("ch-1", "A-101", "B-204"));

    ASSERT_EQ(1u, rest.hangups.size());
    EXPECT_EQ("ch-1", rest.hangups[0].channel_id);
    // A caller channel still gets a CDR context (unlike an outbound leg).
    c.on_event(channel_destroyed("ch-1"));
    EXPECT_EQ(1u, db.inserts.size());
}

// ── Conference join ──────────────────────────────────────────────────────────

TEST(AriClient, ConfExtension_JoinsSocietyMixingBridge)
{
    // Dialing "conf" must NOT fork-ring a flat — it joins the shared
    // society mixing bridge `<society>-conf`.
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(stasis_start("ch-conf-1", "A-101", "conf"));

    ASSERT_EQ(1u, rest.bridges.size())
        << "first conf joiner creates the society mixing bridge";
    EXPECT_EQ("s1-conf", rest.bridges[0].bridge_id);
    EXPECT_EQ("mixing",  rest.bridges[0].type);

    ASSERT_EQ(1u, rest.add_channels.size());
    EXPECT_EQ("s1-conf",    rest.add_channels[0].bridge_id);
    EXPECT_EQ("ch-conf-1",  rest.add_channels[0].channel_id);

    // Conf path bypasses CallRouter entirely — no flat resolution, so
    // no congestion hangup (which is what the no-route path would do).
    EXPECT_TRUE(rest.hangups.empty());
    EXPECT_TRUE(rest.originate_endpoints.empty());
}

TEST(AriClient, ConfExtension_SecondJoiner_ReusesSameBridge)
{
    // Every resident dialing "conf" lands in the same room. The bridge
    // id is fixed per society; `create_bridge` with a taken id is
    // create-or-attach, so the second joiner re-issues it harmlessly.
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event(stasis_start("ch-conf-1", "A-101", "conf"));
    c.on_event(stasis_start("ch-conf-2", "A-102", "conf"));

    // Both joiners target the SAME bridge id.
    ASSERT_EQ(2u, rest.add_channels.size());
    EXPECT_EQ("s1-conf", rest.add_channels[0].bridge_id);
    EXPECT_EQ("s1-conf", rest.add_channels[1].bridge_id);
    EXPECT_EQ("ch-conf-1", rest.add_channels[0].channel_id);
    EXPECT_EQ("ch-conf-2", rest.add_channels[1].channel_id);
}

TEST(AriClient, ConfExtension_CreateBridgeFails_HangsUpCaller)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);
    rest.create_bridge_response = {500, "boom"};

    c.on_event(stasis_start("ch-conf-1", "A-101", "conf"));

    EXPECT_TRUE(rest.add_channels.empty())
        << "no add-channel attempt once the bridge create failed";
    ASSERT_EQ(1u, rest.hangups.size());
    EXPECT_EQ("ch-conf-1", rest.hangups[0].channel_id);
}

TEST(AriClient, OutboundLegStasisStart_NoAdmissionCheck_NoCdrContext)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // Five calls already up — at the admission cap.
    for (int i = 1; i <= 5; ++i)
        c.on_event(bridge_created("br-" + std::to_string(i)));

    // An originated leg answers. It belongs to an already-admitted call,
    // so it must NOT be bounced to the busy handler...
    c.on_event(stasis_start_leg("leg-1", "ch-1"));
    EXPECT_TRUE(rest.continues.empty())
        << "an outbound leg is not itself subject to admission control";

    // ...and it must NOT accrue its own CDR context — its later
    // ChannelDestroyed writes no phantom CDR row (the logical call's CDR
    // is keyed on the caller channel).
    c.on_event(channel_destroyed("leg-1"));
    EXPECT_EQ(0u, db.inserts.size());

    // It WAS delegated to the router: with no caller "ch-1" tracked,
    // CallRouter hangs the stray leg up.
    ASSERT_EQ(1u, rest.hangups.size());
    EXPECT_EQ("leg-1", rest.hangups[0].channel_id);
}

// ── revoke_subscriber: live-call teardown ────────────────────────────────────

TEST(AriClient, RevokeSubscriber_HangsUpEveryChannelOfTheEndpoint)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    rest.get_endpoint_response = {
        200, R"({"resource":"u_a","channel_ids":["ch-1","ch-2"]})"};

    c.revoke_subscriber("u_a");

    // The endpoint was looked up by PJSIP tech + bare sipUsername...
    ASSERT_EQ(1u, rest.endpoint_lookups.size());
    EXPECT_EQ("PJSIP", rest.endpoint_lookups[0].tech);
    EXPECT_EQ("u_a",   rest.endpoint_lookups[0].resource);
    // ...and every channel it owns was hung up.
    ASSERT_EQ(2u, rest.hangups.size());
    EXPECT_EQ("ch-1", rest.hangups[0].channel_id);
    EXPECT_EQ("ch-2", rest.hangups[1].channel_id);
    EXPECT_EQ("normal", rest.hangups[0].reason);
}

TEST(AriClient, RevokeSubscriber_NoActiveChannels_NoHangups)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    rest.get_endpoint_response = {200, R"({"resource":"u_a","channel_ids":[]})"};
    c.revoke_subscriber("u_a");

    EXPECT_EQ(1u, rest.endpoint_lookups.size());
    EXPECT_TRUE(rest.hangups.empty()) << "endpoint idle → nothing to tear down";
}

TEST(AriClient, RevokeSubscriber_UnknownEndpointOrGarbage_NoHangups)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    rest.get_endpoint_response = {404, ""};        // no such endpoint
    c.revoke_subscriber("u_ghost");
    EXPECT_TRUE(rest.hangups.empty());

    rest.get_endpoint_response = {200, "not json"}; // ARI returned garbage
    c.revoke_subscriber("u_a");
    EXPECT_TRUE(rest.hangups.empty()) << "malformed body → silent no-op, no crash";
}

TEST(AriClient, RevokeSubscriber_EmptyUsername_IsNoOp)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.revoke_subscriber("");

    EXPECT_TRUE(rest.endpoint_lookups.empty());
    EXPECT_TRUE(rest.hangups.empty());
}

// ── EndpointStateChange → register-state callback ────────────────────────────

TEST(AriClient, EndpointStateChange_Online_FiresHandlerWithTrue)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    struct Hit { std::string user; bool online; };
    std::vector<Hit> hits;
    c.set_register_state_handler(
        [&hits](const std::string &user, bool online) {
            hits.push_back({user, online});
        });

    c.on_event(endpoint_state_change("PJSIP", "u_alice", "online"));

    ASSERT_EQ(1u, hits.size());
    EXPECT_EQ("u_alice", hits[0].user);
    EXPECT_TRUE(hits[0].online);
}

TEST(AriClient, EndpointStateChange_OfflineOrUnknown_FiresHandlerWithFalse)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    struct Hit { std::string user; bool online; };
    std::vector<Hit> hits;
    c.set_register_state_handler(
        [&hits](const std::string &user, bool online) {
            hits.push_back({user, online});
        });

    c.on_event(endpoint_state_change("PJSIP", "u_a", "offline"));
    c.on_event(endpoint_state_change("PJSIP", "u_b", "unknown"));

    ASSERT_EQ(2u, hits.size());
    EXPECT_FALSE(hits[0].online);
    EXPECT_FALSE(hits[1].online) << "anything not \"online\" maps to false";
}

TEST(AriClient, EndpointStateChange_NonPjsipTech_IsIgnored)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    int hits = 0;
    c.set_register_state_handler([&hits](const std::string &, bool) { ++hits; });

    c.on_event(endpoint_state_change("Local", "loop", "online"));
    c.on_event(endpoint_state_change("IAX2",  "trunk", "online"));

    EXPECT_EQ(0, hits) << "only PJSIP endpoints feed the presence cache";
}

TEST(AriClient, EndpointStateChange_NoHandler_IsSilentNoOp)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // No handler installed — must not crash, must not throw.
    c.on_event(endpoint_state_change("PJSIP", "u_alice", "online"));
}

// ── ContactStatusChange → register-state callback (task #37) ─────────────────

TEST(AriClient, ContactStatusChange_CreatedOrNonQualified_FiresHandlerWithTrue)
{
    // The presence-cache fix: with qualify_frequency=0, a plain REGISTER
    // never moves EndpointStateChange — but ContactStatusChange fires
    // "Created" immediately, and "NonQualified" is the steady state.
    // Both must register the subscriber as online.
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    struct Hit { std::string user; bool online; };
    std::vector<Hit> hits;
    c.set_register_state_handler(
        [&hits](const std::string &user, bool online) {
            hits.push_back({user, online});
        });

    c.on_event(contact_status_change("PJSIP", "a101", "Created"));
    c.on_event(contact_status_change("PJSIP", "a102", "NonQualified"));
    c.on_event(contact_status_change("PJSIP", "a103", "Reachable"));

    ASSERT_EQ(3u, hits.size());
    EXPECT_EQ("a101", hits[0].user); EXPECT_TRUE(hits[0].online);
    EXPECT_EQ("a102", hits[1].user); EXPECT_TRUE(hits[1].online);
    EXPECT_EQ("a103", hits[2].user); EXPECT_TRUE(hits[2].online);
}

TEST(AriClient, ContactStatusChange_RemovedOrUnreachable_FiresHandlerWithFalse)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    struct Hit { std::string user; bool online; };
    std::vector<Hit> hits;
    c.set_register_state_handler(
        [&hits](const std::string &user, bool online) {
            hits.push_back({user, online});
        });

    c.on_event(contact_status_change("PJSIP", "a101", "Removed"));
    c.on_event(contact_status_change("PJSIP", "a102", "Unreachable"));
    c.on_event(contact_status_change("PJSIP", "a103", "Unknown"));

    ASSERT_EQ(3u, hits.size());
    EXPECT_FALSE(hits[0].online);
    EXPECT_FALSE(hits[1].online);
    EXPECT_FALSE(hits[2].online) << "unknown status maps to offline (safe)";
}

TEST(AriClient, ContactStatusChange_NonPjsip_OrNoHandler_IsIgnored)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // No handler installed — must not crash.
    c.on_event(contact_status_change("PJSIP", "a101", "Created"));

    int hits = 0;
    c.set_register_state_handler([&hits](const std::string &, bool) { ++hits; });
    c.on_event(contact_status_change("Local", "loop", "Created"));
    EXPECT_EQ(0, hits) << "only PJSIP contacts feed the presence cache";
}

// ── publish_register_snapshot — presence reconciliation on (re)connect ──────

namespace {
// Builds an `ARI /endpoints/PJSIP` array body matching what Asterisk's
// `GET /ari/endpoints/PJSIP` returns: one object per endpoint, with
// `technology`, `resource`, `state` (online/offline/unknown), and an
// empty `channel_ids` array.
std::string pjsip_endpoint_list(
    const std::vector<std::pair<std::string, std::string>> &entries) {
  json arr = json::array();
  for (const auto &[resource, state] : entries) {
    arr.push_back({
        {"technology",  "PJSIP"},
        {"resource",    resource},
        {"state",       state},
        {"channel_ids", json::array()},
    });
  }
  return arr.dump();
}
} // namespace

TEST(AriClient, PublishRegisterSnapshot_FiresHandlerPerEndpoint)
{
    FakeAriRest rest;
    rest.list_endpoints_response = {200, pjsip_endpoint_list({
        {"u_alice", "online"},
        {"u_bob",   "offline"},
        {"u_carol", "unknown"},
    })};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    std::vector<std::pair<std::string, bool>> hits;
    c.set_register_state_handler(
        [&hits](const std::string &user, bool online) {
            hits.emplace_back(user, online);
        });

    c.publish_register_snapshot();

    // Exactly one ARI list call to /ari/endpoints/PJSIP, regardless of
    // how many entries the response carried.
    ASSERT_EQ(1u, rest.list_endpoint_techs.size());
    EXPECT_EQ("PJSIP", rest.list_endpoint_techs[0]);

    // Three handler calls in array order; only "online" maps to true
    // (matches handle_endpoint_state_change's mapping).
    ASSERT_EQ(3u, hits.size());
    EXPECT_EQ("u_alice", hits[0].first);
    EXPECT_TRUE(hits[0].second);
    EXPECT_EQ("u_bob",   hits[1].first);
    EXPECT_FALSE(hits[1].second);
    EXPECT_EQ("u_carol", hits[2].first);
    EXPECT_FALSE(hits[2].second)
        << "any state other than 'online' must map to false";
}

TEST(AriClient, PublishRegisterSnapshot_NoHandler_IsSilentNoOp)
{
    FakeAriRest rest;
    rest.list_endpoints_response = {200, pjsip_endpoint_list({{"u_a", "online"}})};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    // Production sets the handler before plumbing in the on-connected
    // hook; defensive against an early reconnect arriving before
    // handler installation.
    c.publish_register_snapshot();

    EXPECT_TRUE(rest.list_endpoint_techs.empty())
        << "skip the ARI call entirely when there's nothing to publish "
        << "into";
}

TEST(AriClient, PublishRegisterSnapshot_AriError_NoHandlerCalls)
{
    FakeAriRest rest;
    rest.list_endpoints_response = {503, "Service Unavailable"};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    int hits = 0;
    c.set_register_state_handler(
        [&hits](const std::string &, bool) { ++hits; });

    c.publish_register_snapshot();
    EXPECT_EQ(0, hits)
        << "transient ARI failure mid-reconnect must not synthesise "
        << "fake state — the cache stays at its last known value until "
        << "the next snapshot or live EndpointStateChange.";
}

TEST(AriClient, PublishRegisterSnapshot_BadJson_NoHandlerCalls)
{
    FakeAriRest rest;
    rest.list_endpoints_response = {200, "not json"};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    int hits = 0;
    c.set_register_state_handler(
        [&hits](const std::string &, bool) { ++hits; });

    c.publish_register_snapshot();
    EXPECT_EQ(0, hits);
}

// ── publish_register_snapshot — asterisk-status emission ───────────────────

TEST(AriClient, PublishRegisterSnapshot_FiresAsteriskStatusTrueOn2xx)
{
    FakeAriRest rest;
    rest.list_endpoints_response = {200, pjsip_endpoint_list({})};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    std::vector<bool> status_hits;
    c.set_asterisk_status_handler(
        [&status_hits](bool connected) { status_hits.push_back(connected); });

    c.publish_register_snapshot();

    ASSERT_EQ(1u, status_hits.size());
    EXPECT_TRUE(status_hits[0]);
}

TEST(AriClient, PublishRegisterSnapshot_FiresAsteriskStatusFalseOnAriError)
{
    FakeAriRest rest;
    rest.list_endpoints_response = {503, "Service Unavailable"};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    std::vector<bool> status_hits;
    c.set_asterisk_status_handler(
        [&status_hits](bool connected) { status_hits.push_back(connected); });

    c.publish_register_snapshot();

    ASSERT_EQ(1u, status_hits.size());
    EXPECT_FALSE(status_hits[0])
        << "any non-2xx (including 0 for transport failure) is reported "
        << "as disconnected — same safe default the chip renders for "
        << "agent-down.";
}

TEST(AriClient, PublishRegisterSnapshot_AsteriskStatusFiresEvenWithoutRegisterHandler)
{
    // The two handlers are independent — operators can wire only the
    // asterisk-status one (e.g. a future deploy that runs admin chips
    // without presence) and the ARI probe still happens.
    FakeAriRest rest;
    rest.list_endpoints_response = {200, pjsip_endpoint_list({{"u_a", "online"}})};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    bool fired = false;
    c.set_asterisk_status_handler(
        [&fired](bool) { fired = true; });
    // Deliberately NOT setting the register-state handler.

    c.publish_register_snapshot();

    EXPECT_TRUE(fired);
    ASSERT_EQ(1u, rest.list_endpoint_techs.size());
}

TEST(AriClient, PublishRegisterSnapshot_SkipsNonPjsipAndEmptyResource)
{
    FakeAriRest rest;
    // Hand-rolled body that includes a non-PJSIP entry and a PJSIP one
    // with an empty resource — both should be filtered before the
    // handler runs (mirrors handle_endpoint_state_change's rules).
    rest.list_endpoints_response = {200, R"([
        {"technology":"Local","resource":"loop","state":"online"},
        {"technology":"PJSIP","resource":"","state":"online"},
        {"technology":"PJSIP","resource":"u_d","state":"online"}
    ])"};
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    std::vector<std::string> who;
    c.set_register_state_handler(
        [&who](const std::string &user, bool) { who.push_back(user); });

    c.publish_register_snapshot();
    ASSERT_EQ(1u, who.size());
    EXPECT_EQ("u_d", who[0]);
}

// ── Malformed input tolerance ────────────────────────────────────────────────

TEST(AriClient, IgnoresMalformedJson)
{
    FakeAriRest rest;
    TestDb      db;
    CallRouter  router("s1", db, rest);
    AriClient   c(default_cfg(), rest, db, router);

    c.on_event("not-json-at-all");
    c.on_event("{}");
    c.on_event(R"({"type":"UnknownEventType","foo":"bar"})");

    EXPECT_EQ(0, c.active_bridges());
    EXPECT_EQ(0u, db.inserts.size());
    EXPECT_EQ(0u, rest.continues.size());
}
