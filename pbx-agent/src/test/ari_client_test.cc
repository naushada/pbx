#include "ari_client.hpp"
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
  std::vector<SubscribeCall> subscribes;
  std::vector<ContinueCall>  continues;
  Response                   continue_response{200, ""};
  Response                   subscribe_response{204, ""};

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

TEST(AriClient, SubscribesToChannelEvents)
{
    FakeAriRest rest;
    TestDb      db;
    AriClient   c(default_cfg(), rest, db);

    c.start();

    ASSERT_EQ(1u, rest.subscribes.size());
    EXPECT_EQ("pbx", rest.subscribes[0].app);
    const auto &srcs = rest.subscribes[0].sources;
    // Channel and bridge event sources both subscribed.
    EXPECT_NE(srcs.end(),
              std::find(srcs.begin(), srcs.end(), "channel:"));
    EXPECT_NE(srcs.end(),
              std::find(srcs.begin(), srcs.end(), "bridge:"));
}

// ── Bridge counter ───────────────────────────────────────────────────────────

TEST(AriClient, BridgeCreated_IncrementsActiveCount)
{
    FakeAriRest rest;
    TestDb      db;
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);  // cap = 5

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
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);

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
    AriClient   c(default_cfg(), rest, db);

    c.on_event(stasis_start("ch-1", "A-101", "B-204"));
    c.on_event(bridge_created("br-1", "mixing"));
    c.on_event(channel_entered_bridge("ch-1", "br-1"));
    c.on_event(channel_entered_bridge("ch-2", "br-1"));
    c.on_event(channel_destroyed("ch-1"));

    ASSERT_EQ(1u, db.inserts.size());
    const json cdr = json::parse(db.inserts[0].doc);
    EXPECT_EQ("p2p", cdr["type"]);
}

// ── Malformed input tolerance ────────────────────────────────────────────────

TEST(AriClient, IgnoresMalformedJson)
{
    FakeAriRest rest;
    TestDb      db;
    AriClient   c(default_cfg(), rest, db);

    c.on_event("not-json-at-all");
    c.on_event("{}");
    c.on_event(R"({"type":"UnknownEventType","foo":"bar"})");

    EXPECT_EQ(0, c.active_bridges());
    EXPECT_EQ(0u, db.inserts.size());
    EXPECT_EQ(0u, rest.continues.size());
}
