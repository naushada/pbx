#include "call_router.hpp"
#include "mongodbc.hpp"
#include "json.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// Minimal IMongodbClient — CallRouter only calls
// get_documents(coll, query, proj). `rules` maps a query-substring to a
// canned response; the first matching rule wins (same pattern as the
// microservice suite's TestDb).
class FakeMongo : public IMongodbClient {
public:
  std::vector<std::pair<std::string, std::string>> rules;  // {fragment, response}
  bool throw_on_get = false;
  std::string last_query;  // captured for query-shape assertions

  std::string get_documents(const std::string &coll, const std::string &query,
                            const std::string & /*proj*/) override {
    if (throw_on_get) throw std::runtime_error("injected DB error");
    last_query = query;
    if (coll != "subscribers") return {};
    for (const auto &r : rules)
      if (query.find(r.first) != std::string::npos) return r.second;
    return {};
  }

  // ── unused IMongodbClient surface ──────────────────────────────────────
  const std::string &get_database() const override { return m_db; }
  std::string get_document(const std::string &, const std::string &,
                           const std::string &) override { return {}; }
  std::string get_documents(const std::string &,
                            const std::string &) override { return {}; }
  std::string create_document(const std::string &, const std::string &,
                              const std::string &) override { return {}; }
  std::int32_t create_bulk_document(const std::string &, const std::string &,
                                    const std::string &) override { return 0; }
  bool update_collection(const std::string &, const std::string &,
                         const std::string &) override { return false; }
  std::int32_t update_bulk_document(
      const std::string &, const std::vector<std::string> &,
      const std::vector<std::string> &) override { return 0; }
  bool delete_document(const std::string &, const std::string &) override {
    return false;
  }
  std::string next_awbno(const std::string &) override { return {}; }
  std::string store_file(const std::string &, const std::string &,
                         const std::vector<std::uint8_t> &) override {
    return {};
  }
  std::vector<std::uint8_t> fetch_file(const std::string &) override {
    return {};
  }
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &) override {
    return {};
  }
  bool delete_file(const std::string &) override { return false; }

private:
  std::string m_db = "pabx";
};

// Records every ARI side effect CallRouter performs. `originate_status`
// is the status CallRouter sees back — non-2xx means the leg never came
// up, so it must not be tracked as ringing.
class FakeAriRest : public IAriRest {
public:
  struct Originate {
    std::string endpoint, app, app_args, channel_id, caller_id;
  };
  struct CreateBridge { std::string bridge_id, type; };
  struct AddChannel   { std::string bridge_id, channel_id; };
  struct Hangup       { std::string channel_id, reason; };

  std::vector<Originate>    originates;
  std::vector<CreateBridge> created_bridges;
  std::vector<AddChannel>   added_channels;
  std::vector<Hangup>       hangups;
  int originate_status = 200;

  Response originate(const std::string &endpoint, const std::string &app,
                     const std::string &app_args, const std::string &channel_id,
                     const std::string &caller_id) override {
    originates.push_back({endpoint, app, app_args, channel_id, caller_id});
    return {originate_status, {}};
  }
  Response create_bridge(const std::string &bridge_id,
                         const std::string &type) override {
    created_bridges.push_back({bridge_id, type});
    return {200, {}};
  }
  Response add_channel_to_bridge(const std::string &bridge_id,
                                 const std::string &channel_id) override {
    added_channels.push_back({bridge_id, channel_id});
    return {204, {}};
  }
  Response hangup(const std::string &channel_id,
                  const std::string &reason) override {
    hangups.push_back({channel_id, reason});
    return {204, {}};
  }

  // ── unused IAriRest surface ────────────────────────────────────────────
  Response subscribe(const std::string &,
                     const std::vector<std::string> &) override {
    return {204, {}};
  }
  Response continue_in_dialplan(const std::string &, const std::string &,
                                const std::string &, int) override {
    return {200, {}};
  }
  Response get_endpoint(const std::string &, const std::string &) override {
    return {200, "{}"};
  }
  Response create_dynamic_config(const std::string &, const std::string &,
                                  const std::string &,
                                  const std::string &) override {
    return {200, "{}"};
  }
  Response delete_dynamic_config(const std::string &, const std::string &,
                                  const std::string &) override {
    return {204, ""};
  }
};

// A subscribers row shaped like handle_subscriber_import_POST writes it.
json sub_row(const std::string &flat_number, const std::string &sip_username,
             const std::string &role = "resident",
             const std::string &status = "active") {
  return {
      {"societyId",   "soc1"},
      {"flatNumber",  flat_number},
      {"role",        role},
      {"status",      status},
      {"sipUsername", sip_username},
  };
}

bool has(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// Count hangups for a given channel id.
std::size_t hangups_for(const FakeAriRest &rest, const std::string &cid) {
  return static_cast<std::size_t>(std::count_if(
      rest.hangups.begin(), rest.hangups.end(),
      [&](const FakeAriRest::Hangup &h) { return h.channel_id == cid; }));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// resolve_targets — the data lookup half.
// ─────────────────────────────────────────────────────────────────────────────

TEST(CallRouter, ResolvesFlatToItsSubscribers)
{
    FakeMongo db;
    FakeAriRest rest;
    json rows = json::array({sub_row("A-101", "u_alice"),
                             sub_row("A-101", "u_alice_spouse")});
    db.rules.push_back({R"("flatNumber":"A-101")", rows.dump()});

    CallRouter router("soc1", db, rest);
    auto targets = router.resolve_targets("A-101");

    ASSERT_EQ(2u, targets.size());
    EXPECT_TRUE(has(targets, "u_alice"));
    EXPECT_TRUE(has(targets, "u_alice_spouse"));
    // Society-scoped, keyed on the denormalized flatNumber.
    EXPECT_NE(std::string::npos, db.last_query.find(R"("societyId":"soc1")"));
    EXPECT_NE(std::string::npos, db.last_query.find(R"("flatNumber":"A-101")"));
}

TEST(CallRouter, ResolvesGuardExtensionToGuards)
{
    FakeMongo db;
    FakeAriRest rest;
    json rows = json::array({sub_row("GATE", "u_guard1", "guard"),
                             sub_row("GATE", "u_guard2", "guard")});
    db.rules.push_back({R"("role":"guard")", rows.dump()});

    CallRouter router("soc1", db, rest);
    auto targets = router.resolve_targets("0");

    ASSERT_EQ(2u, targets.size());
    EXPECT_TRUE(has(targets, "u_guard1"));
    EXPECT_TRUE(has(targets, "u_guard2"));
    EXPECT_NE(std::string::npos, db.last_query.find(R"("role":"guard")"));
}

TEST(CallRouter, UnknownFlatReturnsEmpty)
{
    FakeMongo db;  // no rules — every query resolves to nothing
    FakeAriRest rest;
    CallRouter router("soc1", db, rest);
    EXPECT_TRUE(router.resolve_targets("Z-999").empty());
}

TEST(CallRouter, ExcludesDisabledSubscribers)
{
    FakeMongo db;
    FakeAriRest rest;
    json rows = json::array({sub_row("A-101", "u_active"),
                             sub_row("A-101", "u_disabled", "resident",
                                     "disabled")});
    db.rules.push_back({R"("flatNumber":"A-101")", rows.dump()});

    CallRouter router("soc1", db, rest);
    auto targets = router.resolve_targets("A-101");

    ASSERT_EQ(1u, targets.size());
    EXPECT_EQ("u_active", targets[0]);
}

TEST(CallRouter, SkipsRowsWithMissingOrEmptySipUsername)
{
    FakeMongo db;
    FakeAriRest rest;
    json rows = json::array();
    rows.push_back(sub_row("A-101", "u_good"));
    rows.push_back({{"societyId", "soc1"}, {"flatNumber", "A-101"},
                    {"status", "active"}});                       // no sipUsername
    rows.push_back(sub_row("A-101", ""));                         // empty sipUsername
    db.rules.push_back({R"("flatNumber":"A-101")", rows.dump()});

    CallRouter router("soc1", db, rest);
    auto targets = router.resolve_targets("A-101");

    ASSERT_EQ(1u, targets.size());
    EXPECT_EQ("u_good", targets[0]);
}

TEST(CallRouter, EmptyExtensionReturnsEmpty)
{
    FakeMongo db;
    FakeAriRest rest;
    CallRouter router("soc1", db, rest);
    EXPECT_TRUE(router.resolve_targets("").empty());
}

TEST(CallRouter, MalformedOrFailedDbReturnsEmpty)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")", "not json at all"});
    CallRouter router("soc1", db, rest);
    EXPECT_TRUE(router.resolve_targets("A-101").empty()) << "garbage JSON → no route";

    FakeMongo throwing;
    FakeAriRest rest2;
    throwing.throw_on_get = true;
    CallRouter router2("soc1", throwing, rest2);
    EXPECT_TRUE(router2.resolve_targets("A-101").empty()) << "DB throw → no route";
}

// ─────────────────────────────────────────────────────────────────────────────
// Forked-ring driver — originate fan-out.
// ─────────────────────────────────────────────────────────────────────────────

TEST(CallRouter, ForkRing_OriginatesALegPerTarget)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_alice"),
                     sub_row("A-101", "u_bob")}).dump()});

    CallRouter router("soc1", db, rest, "pbx");
    router.on_caller_start("ch-1", "A-101", "A-101");

    ASSERT_EQ(2u, rest.originates.size());
    for (const auto &o : rest.originates) {
        EXPECT_EQ("pbx", o.app);
        // The answered leg must re-enter Stasis tagged to its caller.
        EXPECT_EQ("outbound,ch-1", o.app_args);
        EXPECT_EQ("A-101", o.caller_id);
        EXPECT_TRUE(o.endpoint == "PJSIP/u_alice" ||
                    o.endpoint == "PJSIP/u_bob") << o.endpoint;
        EXPECT_FALSE(o.channel_id.empty());
    }
    // Distinct leg channel ids — one tracked fork each.
    EXPECT_NE(rest.originates[0].channel_id, rest.originates[1].channel_id);
    EXPECT_TRUE(rest.hangups.empty());
    EXPECT_EQ(1u, router.active_calls());
}

TEST(CallRouter, ForkRing_NoTargets_HangsUpCaller)
{
    FakeMongo db;  // unknown flat — resolves to nothing
    FakeAriRest rest;
    CallRouter router("soc1", db, rest);

    router.on_caller_start("ch-1", "A-101", "Z-999");

    EXPECT_TRUE(rest.originates.empty());
    ASSERT_EQ(1u, rest.hangups.size());
    EXPECT_EQ("ch-1", rest.hangups[0].channel_id);
    EXPECT_EQ("congestion", rest.hangups[0].reason);
    EXPECT_EQ(0u, router.active_calls()) << "no route → nothing to track";
}

TEST(CallRouter, ForkRing_AllOriginatesFail_HangsUpCaller)
{
    FakeMongo db;
    FakeAriRest rest;
    rest.originate_status = 503;  // Asterisk refuses every leg
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_alice"),
                     sub_row("A-101", "u_bob")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");

    EXPECT_EQ(2u, rest.originates.size()) << "both legs attempted";
    ASSERT_EQ(1u, rest.hangups.size());
    EXPECT_EQ("ch-1", rest.hangups[0].channel_id);
    EXPECT_EQ("congestion", rest.hangups[0].reason);
    EXPECT_EQ(0u, router.active_calls());
}

TEST(CallRouter, ForkRing_IsIdempotentPerCaller)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_alice")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    router.on_caller_start("ch-1", "A-101", "A-101");  // Asterisk re-emit

    EXPECT_EQ(1u, rest.originates.size()) << "second pass must be a no-op";
    EXPECT_EQ(1u, router.active_calls());
}

// ─────────────────────────────────────────────────────────────────────────────
// Forked-ring driver — first-answer-wins.
// ─────────────────────────────────────────────────────────────────────────────

TEST(CallRouter, ForkRing_FirstAnswerWins_BridgesAndTearsDownSiblings)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_a"),
                     sub_row("A-101", "u_b"),
                     sub_row("A-101", "u_c")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    ASSERT_EQ(3u, rest.originates.size());

    const std::string winner = rest.originates[0].channel_id;
    const std::string loser1 = rest.originates[1].channel_id;
    const std::string loser2 = rest.originates[2].channel_id;

    router.on_leg_start(winner, "ch-1");

    // One bridge, caller + winner pulled into it.
    ASSERT_EQ(1u, rest.created_bridges.size());
    const std::string bridge = rest.created_bridges[0].bridge_id;
    EXPECT_EQ("mixing", rest.created_bridges[0].type);
    ASSERT_EQ(2u, rest.added_channels.size());
    EXPECT_EQ(bridge, rest.added_channels[0].bridge_id);
    EXPECT_EQ(bridge, rest.added_channels[1].bridge_id);
    EXPECT_TRUE((rest.added_channels[0].channel_id == "ch-1" &&
                 rest.added_channels[1].channel_id == winner) ||
                (rest.added_channels[0].channel_id == winner &&
                 rest.added_channels[1].channel_id == "ch-1"));

    // The two siblings still ringing are hung up; the winner and caller
    // are not.
    EXPECT_EQ(1u, hangups_for(rest, loser1));
    EXPECT_EQ(1u, hangups_for(rest, loser2));
    EXPECT_EQ(0u, hangups_for(rest, winner));
    EXPECT_EQ(0u, hangups_for(rest, "ch-1"));
    for (const auto &h : rest.hangups)
        EXPECT_EQ("answered", h.reason) << "losers lost to an answer";
    EXPECT_EQ(1u, router.active_calls());
}

TEST(CallRouter, ForkRing_LateAnswerLosesTheRace)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_a"),
                     sub_row("A-101", "u_b")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    const std::string winner = rest.originates[0].channel_id;
    const std::string late   = rest.originates[1].channel_id;

    router.on_leg_start(winner, "ch-1");          // first answer
    rest.created_bridges.clear();                 // isolate the late answer
    router.on_leg_start(late, "ch-1");            // the loser answers anyway

    EXPECT_TRUE(rest.created_bridges.empty()) << "no second bridge";
    // `late` was already torn down as a loser, then answers; either way
    // it ends up hung up — and never bridged.
    EXPECT_GE(hangups_for(rest, late), 1u);
    EXPECT_EQ(0u, hangups_for(rest, "ch-1"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Forked-ring driver — teardown paths.
// ─────────────────────────────────────────────────────────────────────────────

TEST(CallRouter, ForkRing_AllLegsFailBeforeAnswer_HangsUpCaller)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_a"),
                     sub_row("A-101", "u_b")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    const std::string leg0 = rest.originates[0].channel_id;
    const std::string leg1 = rest.originates[1].channel_id;

    router.on_channel_gone(leg0);
    EXPECT_EQ(0u, hangups_for(rest, "ch-1")) << "one leg still ringing";

    router.on_channel_gone(leg1);
    ASSERT_EQ(1u, hangups_for(rest, "ch-1"));
    EXPECT_EQ("no_answer", rest.hangups.back().reason);
    EXPECT_EQ(0u, router.active_calls());
}

TEST(CallRouter, ForkRing_CallerHangsUpWhileRinging_TearsDownLegs)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_a"),
                     sub_row("A-101", "u_b")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    const std::string leg0 = rest.originates[0].channel_id;
    const std::string leg1 = rest.originates[1].channel_id;

    router.on_channel_gone("ch-1");  // caller bails before anyone answers

    EXPECT_EQ(1u, hangups_for(rest, leg0));
    EXPECT_EQ(1u, hangups_for(rest, leg1));
    for (const auto &h : rest.hangups)
        EXPECT_EQ("normal", h.reason);
    EXPECT_EQ(0u, router.active_calls());

    // The legs' own ChannelDestroyed events still arrive afterwards —
    // must be harmless (caller already gone).
    router.on_channel_gone(leg0);
    router.on_channel_gone(leg1);
    EXPECT_EQ(2u, rest.hangups.size()) << "no spurious extra hangups";
}

TEST(CallRouter, ForkRing_ConnectedLegHangsUp_ReleasesCaller)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_a")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    const std::string leg = rest.originates[0].channel_id;
    router.on_leg_start(leg, "ch-1");          // answered + bridged

    router.on_channel_gone(leg);               // the callee hangs up

    ASSERT_EQ(1u, hangups_for(rest, "ch-1"));
    EXPECT_EQ("normal", rest.hangups.back().reason);
    EXPECT_EQ(0u, router.active_calls());
}

TEST(CallRouter, ForkRing_LegAnswersAfterCallerGone_IsHungUp)
{
    FakeMongo db;
    FakeAriRest rest;
    db.rules.push_back({R"("flatNumber":"A-101")",
        json::array({sub_row("A-101", "u_a")}).dump()});

    CallRouter router("soc1", db, rest);
    router.on_caller_start("ch-1", "A-101", "A-101");
    const std::string leg = rest.originates[0].channel_id;

    router.on_channel_gone("ch-1");  // caller gone; leg torn down + call erased
    rest.hangups.clear();

    // The leg answers anyway (race with the hangup) — nothing to bridge to.
    router.on_leg_start(leg, "ch-1");
    ASSERT_EQ(1u, rest.hangups.size());
    EXPECT_EQ(leg, rest.hangups[0].channel_id);
    EXPECT_TRUE(rest.created_bridges.empty());
}
