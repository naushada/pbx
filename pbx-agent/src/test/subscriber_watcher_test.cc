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
  std::string try_next(int /*max_await_ms*/) override {
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
  RecorderCursor         *issued_cursor = nullptr; ///< observed by the test

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
  watch_collection(const std::string &) override {
    if (!enable_stream) return nullptr;
    auto cur = std::make_unique<RecorderCursor>();
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
  j["sipHa1"]      = "ha1-" + sip_user;
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

    // alice active → 3 PUTs; bob disabled → 3 DELETEs.
    EXPECT_EQ(3u, rest.puts.size());
    EXPECT_EQ("u_alice", rest.puts[2].id);  // endpoint id
    EXPECT_EQ(3u, rest.deletes.size());
    EXPECT_EQ("u_bob",   rest.deletes[0].id);  // endpoint-first delete
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
    // bootstrap still ran
    EXPECT_EQ(3u, rest.puts.size());
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

    EXPECT_EQ(3u, rest.puts.size());
    EXPECT_EQ("u_carol", rest.puts[2].id);
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
    EXPECT_EQ(3u, rest.deletes.size());
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

    ASSERT_EQ(3u, rest.deletes.size());
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
    EXPECT_EQ(3u, rest.puts.size())
        << "one tick drains exactly one event — keeps the reactor responsive";

    w.tick();
    EXPECT_EQ(6u, rest.puts.size());

    w.tick();  // queue empty
    EXPECT_EQ(6u, rest.puts.size());
}
