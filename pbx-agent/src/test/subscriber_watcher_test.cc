// Tests for SubscriberWatcher — the bootstrap+change-stream driver that
// keeps Asterisk's pjsip endpoints in sync with the `subscribers` Mongo
// collection.
//
// We use the real `PjsipProvisioner` driven by a `FakeAriRest` recorder
// (mirrors `pjsip_provisioner_test.cc`'s style) so the assertions read in
// the operator's vocabulary — "provisioned u_alice", "deprovisioned
// u_bob" — rather than mocked-out provisioner calls. The Mongo side is
// fully faked: `RecorderCursor` yields canned change events, `FakeDb`
// returns canned bootstrap docs.

#include "json.hpp"
#include "pjsip_provisioner.hpp"
#include "subscriber_watcher.hpp"

#include <gtest/gtest.h>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

// `SubscriberWatcherTestAccess` is declared at the bottom of
// subscriber_watcher.hpp and defined in subscriber_watcher.cpp.

namespace {

// ─── Fake ARI REST — records dynamic-config PUT/DELETE order. ──────────────

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

  // Unused surface.
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

// ─── Recorder change-stream cursor — yields canned events. ─────────────────

class RecorderCursor : public IChangeStreamCursor {
public:
  std::deque<std::string> queue;
  /// When true, next try_next() throws to simulate a mid-stream Mongo
  /// disconnect. Cleared after firing so a second call would fall back
  /// to normal queue behaviour — though in practice the watcher tears
  /// down the cursor on the throw, so a second call never happens on
  /// the same cursor instance.
  bool throw_on_next = false;
  std::string try_next(int /*max_await_ms*/) override {
    if (throw_on_next) {
      throw_on_next = false;
      throw std::runtime_error("simulated Mongo disconnect");
    }
    if (queue.empty()) return {};
    std::string s = std::move(queue.front());
    queue.pop_front();
    return s;
  }
};

// ─── Fake IMongodbClient — only `get_documents` + `watch_collection` matter.

class FakeDb : public IMongodbClient {
public:
  std::string             bootstrap_json;  ///< canned JSON array
  bool                    enable_stream = true;
  RecorderCursor         *issued_cursor = nullptr; ///< most recent
  /// Queue of cursors to hand out on successive watch_collection() calls.
  /// Empty queue defaults to a fresh single-use RecorderCursor (matches
  /// the prior single-cursor contract). Tests that exercise the
  /// reopen-after-throw path push two cursors here — pre- and
  /// post-disconnect — and assert ordering.
  std::deque<std::unique_ptr<RecorderCursor>> cursor_queue;
  /// Record of every `watch_collection(coll, token)` call. `.token` is
  /// the raw JSON of the resume token (empty for the initial open).
  struct WatchCall { std::string coll; std::string token; };
  std::vector<WatchCall> watch_calls;

  const std::string &get_database() const override { return m_db; }
  std::string create_document(const std::string &, const std::string &,
                               const std::string &) override { return {}; }
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
  std::string get_document(const std::string &, const std::string &,
                            const std::string &) override { return {}; }
  std::string get_documents(const std::string &, const std::string &,
                             const std::string &) override { return bootstrap_json; }
  std::string get_documents(const std::string &, const std::string &) override {
    return bootstrap_json;
  }
  std::string next_awbno(const std::string &) override { return {}; }
  std::string store_file(const std::string &, const std::string &,
                          const std::vector<std::uint8_t> &) override { return {}; }
  std::vector<std::uint8_t> fetch_file(const std::string &) override { return {}; }
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &) override { return {}; }
  bool delete_file(const std::string &) override { return false; }

  std::unique_ptr<IChangeStreamCursor>
  watch_collection(const std::string &coll) override {
    return watch_collection(coll, std::string{});
  }

  std::unique_ptr<IChangeStreamCursor>
  watch_collection(const std::string &coll,
                    const std::string &resume_token_json) override {
    watch_calls.push_back({coll, resume_token_json});
    if (!enable_stream) return nullptr;
    std::unique_ptr<RecorderCursor> cur;
    if (!cursor_queue.empty()) {
      cur = std::move(cursor_queue.front());
      cursor_queue.pop_front();
      if (!cur) return nullptr;  // explicit null = simulate reopen failure
    } else {
      cur = std::make_unique<RecorderCursor>();
    }
    issued_cursor = cur.get();
    return cur;
  }

private:
  std::string m_db = "pbx";
};

// Convenience builders so each test reads top-down.

std::string subscriber_row(const std::string &id, const std::string &sip_user,
                            const std::string &status = "active",
                            const std::string &society = "soc1") {
  json j;
  j["_id"]         = {{"$oid", id}};
  j["societyId"]   = society;
  j["sipUsername"] = sip_user;
  j["status"]      = status;
  return j.dump();
}

std::string event(const std::string &op, const json &full_doc_or_key) {
  json e;
  e["operationType"] = op;
  if (op == "delete") {
    e["documentKey"] = full_doc_or_key;
  } else {
    e["fullDocument"] = full_doc_or_key;
    if (full_doc_or_key.contains("_id"))
      e["documentKey"] = {{"_id", full_doc_or_key["_id"]}};
  }
  return e.dump();
}

} // namespace

// ─── bootstrap ─────────────────────────────────────────────────────────────

TEST(SubscriberWatcher, Bootstrap_ProvisionsActive_DeprovisionsDisabled)
{
    FakeDb db;
    db.bootstrap_json =
        "[" + subscriber_row("0000000000000000000000a1", "u_alice") + "," +
              subscriber_row("0000000000000000000000a2", "u_bob",   "disabled") +
        "]";
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);

    w.bootstrap();

    // alice active → 2 PUTs (aor + endpoint; auth dropped by PR #77)
    //                + 1 DELETE (legacy auth cleanup).
    // bob   disabled → 4 DELETEs (endpoint, legacy auth, current aor `<user>`,
    //                  legacy aor `<user>-aor` — the legacy-aor cleanup
    //                  was added in PR #103 to prune pre-fix astdb rows
    //                  across upgrades).
    EXPECT_EQ(2u, rest.puts.size());
    EXPECT_EQ("u_alice", rest.puts[1].id);  // endpoint id (was index 2 pre-#77)
    EXPECT_EQ(5u, rest.deletes.size());
    // alice's auth-cleanup precedes bob's full deprovision in iteration order.
    EXPECT_EQ("u_alice-auth", rest.deletes[0].id);
    EXPECT_EQ("u_bob",        rest.deletes[1].id);  // bob's endpoint-first delete
    EXPECT_EQ(2u, w.cached_count())
        << "_id→sipUsername cache populated for both rows, so a later "
        << "delete event can resolve sipUsername.";
}

TEST(SubscriberWatcher, Bootstrap_EmptyResult_NoOps)
{
    FakeDb db; // bootstrap_json stays empty
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);

    w.bootstrap();

    EXPECT_TRUE(rest.puts.empty());
    EXPECT_TRUE(rest.deletes.empty());
    EXPECT_EQ(0u, w.cached_count());
}

TEST(SubscriberWatcher, Bootstrap_NoChangeStream_TickIsSilentNoOp)
{
    FakeDb db;
    db.enable_stream  = false;  // mongod isn't a replica set
    db.bootstrap_json = "[" + subscriber_row("a", "u_alice") + "]";
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);

    w.bootstrap();
    // bootstrap still ran — 2 PUTs (aor + endpoint) for u_alice; PR #77
    // dropped the auth PUT so it's no longer 3.
    EXPECT_EQ(2u, rest.puts.size());
    rest.puts.clear();
    rest.deletes.clear();

    // tick() must NOT touch ARI when the cursor never opened.
    w.tick();
    w.tick();
    EXPECT_TRUE(rest.puts.empty());
    EXPECT_TRUE(rest.deletes.empty());
}

// ─── change-stream events via the test seam ────────────────────────────────

TEST(SubscriberWatcher, Event_Insert_Active_Provisions)
{
    FakeDb db;
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();  // cursor opens, queue empty

    json doc = json::parse(subscriber_row("0000000000000000000000c1", "u_carol"));
    SubscriberWatcherTestAccess::deliver(w, event("insert", doc));

    // PR #77: provision() emits aor + endpoint (2 PUTs), no auth.
    EXPECT_EQ(2u, rest.puts.size());
    EXPECT_EQ("u_carol", rest.puts[1].id);  // endpoint id (was index 2 pre-#77)
    EXPECT_EQ(1u, w.cached_count())
        << "insert populates the _id→sipUsername cache so a later delete "
        << "(which only carries documentKey._id) resolves sipUsername.";
}

TEST(SubscriberWatcher, Event_Update_Disabled_Deprovisions)
{
    FakeDb db;
    db.bootstrap_json = "[" + subscriber_row("0000000000000000000000d1", "u_dan") + "]";
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    rest.puts.clear();
    rest.deletes.clear();

    json doc = json::parse(
        subscriber_row("0000000000000000000000d1", "u_dan", "disabled"));
    SubscriberWatcherTestAccess::deliver(w, event("update", doc));

    EXPECT_TRUE(rest.puts.empty());
    EXPECT_EQ(4u, rest.deletes.size());  // endpoint + auth + aor + legacy aor
    EXPECT_EQ("u_dan", rest.deletes[0].id);
}

TEST(SubscriberWatcher, Event_Delete_UsesCachedSipUsername)
{
    FakeDb db;
    db.bootstrap_json = "[" + subscriber_row("0000000000000000000000e1", "u_eve") + "]";
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    rest.puts.clear();
    rest.deletes.clear();

    // Delete event — documentKey._id is the only identifier.
    json del;
    del["operationType"] = "delete";
    del["documentKey"]["_id"] = {{"$oid", "0000000000000000000000e1"}};
    SubscriberWatcherTestAccess::deliver(w, del.dump());

    ASSERT_EQ(4u, rest.deletes.size());  // endpoint + auth + aor + legacy aor
    EXPECT_EQ("u_eve", rest.deletes[0].id);
    EXPECT_EQ(0u, w.cached_count())
        << "cache entry erased once its subscriber is gone.";
}

TEST(SubscriberWatcher, Event_Delete_UnknownId_IsNoOp)
{
    FakeDb db;  // no bootstrap rows
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();

    json del;
    del["operationType"]      = "delete";
    del["documentKey"]["_id"] = {{"$oid", "ffffffffffffffffffffffff"}};
    SubscriberWatcherTestAccess::deliver(w, del.dump());

    // Never saw the _id — nothing to deprovision (avoid blind DELETEs that
    // could clobber a sibling subscriber sharing a sipUsername with another
    // tenant on the same Asterisk).
    EXPECT_TRUE(rest.deletes.empty());
}

TEST(SubscriberWatcher, Event_OtherSocietyId_IsIgnored)
{
    FakeDb db;
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);  // we watch soc1
    w.bootstrap();

    json doc = json::parse(
        subscriber_row("0000000000000000000000f1", "u_eve", "active", "soc2"));
    SubscriberWatcherTestAccess::deliver(w, event("insert", doc));

    EXPECT_TRUE(rest.puts.empty())    << "other society's row not ours to act on";
    EXPECT_TRUE(rest.deletes.empty());
    EXPECT_EQ(0u, w.cached_count());
}

TEST(SubscriberWatcher, Event_MalformedJson_IsSwallowed)
{
    FakeDb db;
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();

    SubscriberWatcherTestAccess::deliver(w, "{not valid json");

    EXPECT_TRUE(rest.puts.empty());
    EXPECT_TRUE(rest.deletes.empty());
}

// ─── tick() drives the cursor ──────────────────────────────────────────────

TEST(SubscriberWatcher, Tick_DrainsOneEventPerCall)
{
    FakeDb db;
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();

    ASSERT_NE(nullptr, db.issued_cursor);
    json a = json::parse(subscriber_row("0000000000000000000000a1", "u_a1"));
    json b = json::parse(subscriber_row("0000000000000000000000a2", "u_a2"));
    db.issued_cursor->queue.push_back(event("insert", a));
    db.issued_cursor->queue.push_back(event("insert", b));

    w.tick();
    // PR #77: provision() now emits 2 PUTs (aor + endpoint), was 3.
    EXPECT_EQ(2u, rest.puts.size())
        << "one tick drains exactly one event — keeps the reactor responsive";

    w.tick();
    EXPECT_EQ(4u, rest.puts.size());

    w.tick();  // queue empty
    EXPECT_EQ(4u, rest.puts.size());
}

// ─── Resume-token capture + reopen recovery ────────────────────────────────

namespace {
/// Build a change event that includes its own `_id` (the resume token).
/// Used by the resume-token tests below — production-shape events look
/// exactly like this when emitted by mongocxx.
std::string event_with_token(const std::string &op, const json &full_doc,
                              const std::string &token_data) {
  json e = json::parse(event("insert", full_doc));
  e["operationType"] = op;
  e["_id"]           = {{"_data", token_data}};
  return e.dump();
}
} // namespace

TEST(SubscriberWatcher, HandleEvent_CapturesIdAsResumeToken)
{
    FakeDb db;
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();

    json doc = json::parse(subscriber_row("0000000000000000000000a1", "u_a"));
    SubscriberWatcherTestAccess::deliver(
        w, event_with_token("insert", doc, "TOKEN-1"));

    // The watcher must carry the bare `_id` JSON forward so a subsequent
    // reopen passes the same shape mongocxx expects in `resume_after`.
    EXPECT_NE(std::string::npos, w.resume_token().find("TOKEN-1"));
}

TEST(SubscriberWatcher, HandleEvent_OtherSociety_StillCapturesToken)
{
    // Even when an event is dispatched as a no-op (wrong society, etc.)
    // the server has moved past it. If we don't capture the token,
    // every reopen replays this event forever.
    FakeDb db;
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();

    json doc = json::parse(
        subscriber_row("0000000000000000000000a1", "u_a", "active", "soc2"));
    SubscriberWatcherTestAccess::deliver(
        w, event_with_token("insert", doc, "OTHER-SOCIETY-TOKEN"));

    EXPECT_TRUE(rest.puts.empty());  // not our society — no provision
    EXPECT_NE(std::string::npos,
              w.resume_token().find("OTHER-SOCIETY-TOKEN"))
        << "token captured even when dispatch is a no-op";
}

TEST(SubscriberWatcher, Tick_TryNextThrows_ReopensWithResumeToken)
{
    FakeDb db;
    // Two cursors: pre-disconnect (cur_a, delivers one event then
    // throws) and post-disconnect (cur_b, queue empty).
    auto cur_a = std::make_unique<RecorderCursor>();
    auto cur_b = std::make_unique<RecorderCursor>();
    RecorderCursor *a_ptr = cur_a.get();
    json doc = json::parse(subscriber_row("0000000000000000000000a1", "u_a"));
    a_ptr->queue.push_back(event_with_token("insert", doc, "RESUME-A"));
    db.cursor_queue.push_back(std::move(cur_a));
    db.cursor_queue.push_back(std::move(cur_b));

    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    ASSERT_EQ(1u, db.watch_calls.size())
        << "first open is cursor A (no resume token yet)";
    EXPECT_EQ("",         db.watch_calls[0].token);
    EXPECT_EQ("subscribers", db.watch_calls[0].coll);

    // Drain the queued event so the watcher captures RESUME-A as its
    // last applied token.
    w.tick();
    // PR #77: provision() now emits 2 PUTs (aor + endpoint), was 3.
    EXPECT_EQ(2u, rest.puts.size());
    EXPECT_NE(std::string::npos, w.resume_token().find("RESUME-A"));

    // Arm cursor A to throw on the next try_next, then tick: the
    // watcher must catch, drop cursor A, and (on the SAME tick or the
    // next) call watch_collection again with the captured token.
    a_ptr->throw_on_next = true;
    w.tick();  // throw → cursor torn down; reopen scheduled for next tick
    w.tick();  // reopen attempt fires — m_reopen_skip_ticks was 0

    ASSERT_EQ(2u, db.watch_calls.size())
        << "second open uses the resume token from the last applied event";
    EXPECT_NE(std::string::npos, db.watch_calls[1].token.find("RESUME-A"));
}

TEST(SubscriberWatcher, Tick_ReopenFailure_BacksOffBeforeRetry)
{
    FakeDb db;
    // Bootstrap cursor succeeds, but it'll throw on the first try_next.
    auto cur_a = std::make_unique<RecorderCursor>();
    cur_a->throw_on_next = true;
    db.cursor_queue.push_back(std::move(cur_a));
    // Every subsequent reopen returns null — simulate Mongo still down.
    for (int i = 0; i < 10; ++i) db.cursor_queue.push_back(nullptr);

    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    ASSERT_EQ(1u, db.watch_calls.size());

    // Throw tears down cursor.
    w.tick();
    // Next tick: reopen attempt (fails, arms backoff = 5 ticks).
    w.tick();
    EXPECT_EQ(2u, db.watch_calls.size());

    // Next 5 ticks should NOT call watch_collection — they're the
    // backoff window so we don't hammer the pool while Mongo's down.
    for (int i = 0; i < 5; ++i) w.tick();
    EXPECT_EQ(2u, db.watch_calls.size())
        << "no reopen attempts during the backoff window";

    // 6th tick after the failed reopen: a new attempt fires.
    w.tick();
    EXPECT_EQ(3u, db.watch_calls.size());
}

TEST(SubscriberWatcher, Resync_ReRunsFullScan_WithCurrentRealm)
{
    // Production driver for resync() is the SOCIETY_BOOTSTRAP handler —
    // PjsipProvisioner.set_sip_realm() is called immediately before
    // resync() so the re-PUT carries the new realm. Verify the
    // re-provision happens (sorcery is idempotent on the wire so the
    // signal is "did the PUTs happen?", not "did the realm change?").
    FakeDb db;
    db.bootstrap_json = "[" + subscriber_row("a", "u_alice") + "]";
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "default.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    const auto puts_after_bootstrap = rest.puts.size();
    // PR #77: provision() emits aor + endpoint (2 PUTs), no auth.
    EXPECT_EQ(2u, puts_after_bootstrap);

    // Cloud sends a new realm; production handler sets it on prov first…
    prov.set_sip_realm("acme.pbx.local");
    // …then triggers resync. The full-scan runs again; sorcery absorbs
    // duplicates, so we just verify that both sorcery objects were
    // re-PUT (the next 2 entries in rest.puts).
    w.resync();
    EXPECT_EQ(puts_after_bootstrap + 2u, rest.puts.size());

    // Note: pre-#77 this test verified the auth PUT carried the new
    // realm in its `realm` field. PR #77 dropped digest auth entirely,
    // so the realm is no longer carried in any PUT body — it lives
    // only on PjsipProvisioner's m_sip_realm state. The re-PUT count
    // above is sufficient to verify resync triggered the full-scan.
}

TEST(SubscriberWatcher, Resync_DoesNotTouchChangeStreamCursor)
{
    FakeDb db;
    db.bootstrap_json = "[" + subscriber_row("a", "u_alice") + "]";
    FakeAriRest rest;
    PjsipProvisioner prov(rest, "default.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    // bootstrap opens cursor #1 (no resume token).
    ASSERT_EQ(1u, db.watch_calls.size());

    w.resync();
    EXPECT_EQ(1u, db.watch_calls.size())
        << "resync must NOT call watch_collection() — the change-stream "
        << "cursor + resume token stay alive across realm corrections";
}

TEST(SubscriberWatcher, Tick_StandaloneMongo_StillReopenAttempts)
{
    // mongod is standalone → bootstrap can't open a cursor, but the
    // operator may flip the deployment to a replica set without
    // restarting the agent. The watcher must keep trying so it picks
    // up the change-streams capability when it appears.
    FakeDb db;
    db.cursor_queue.push_back(nullptr);  // bootstrap fails
    db.cursor_queue.push_back(nullptr);  // first reopen fails
    db.cursor_queue.push_back(std::make_unique<RecorderCursor>()); // RS now up

    FakeAriRest rest;
    PjsipProvisioner prov(rest, "soc1.pbx.local");
    SubscriberWatcher w(db, "soc1", prov);
    w.bootstrap();
    ASSERT_EQ(1u, db.watch_calls.size());

    w.tick();  // first reopen — fails, arms backoff
    EXPECT_EQ(2u, db.watch_calls.size());
    for (int i = 0; i < 5; ++i) w.tick();  // backoff ticks
    EXPECT_EQ(2u, db.watch_calls.size());

    w.tick();  // 6th tick — retry succeeds
    EXPECT_EQ(3u, db.watch_calls.size());
    EXPECT_NE(nullptr, db.issued_cursor);

    // No token was ever captured (bootstrap saw no events), so every
    // reopen attempt sent an empty token.
    for (const auto &call : db.watch_calls) EXPECT_EQ("", call.token);
}
