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

} // namespace

TEST(CallRouter, ResolvesFlatToItsSubscribers)
{
    FakeMongo db;
    json rows = json::array({sub_row("A-101", "u_alice"),
                             sub_row("A-101", "u_alice_spouse")});
    db.rules.push_back({R"("flatNumber":"A-101")", rows.dump()});

    CallRouter router("soc1", db);
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
    json rows = json::array({sub_row("GATE", "u_guard1", "guard"),
                             sub_row("GATE", "u_guard2", "guard")});
    db.rules.push_back({R"("role":"guard")", rows.dump()});

    CallRouter router("soc1", db);
    auto targets = router.resolve_targets("0");

    ASSERT_EQ(2u, targets.size());
    EXPECT_TRUE(has(targets, "u_guard1"));
    EXPECT_TRUE(has(targets, "u_guard2"));
    EXPECT_NE(std::string::npos, db.last_query.find(R"("role":"guard")"));
}

TEST(CallRouter, UnknownFlatReturnsEmpty)
{
    FakeMongo db;  // no rules — every query resolves to nothing
    CallRouter router("soc1", db);
    EXPECT_TRUE(router.resolve_targets("Z-999").empty());
}

TEST(CallRouter, ExcludesDisabledSubscribers)
{
    FakeMongo db;
    json rows = json::array({sub_row("A-101", "u_active"),
                             sub_row("A-101", "u_disabled", "resident",
                                     "disabled")});
    db.rules.push_back({R"("flatNumber":"A-101")", rows.dump()});

    CallRouter router("soc1", db);
    auto targets = router.resolve_targets("A-101");

    ASSERT_EQ(1u, targets.size());
    EXPECT_EQ("u_active", targets[0]);
}

TEST(CallRouter, SkipsRowsWithMissingOrEmptySipUsername)
{
    FakeMongo db;
    json rows = json::array();
    rows.push_back(sub_row("A-101", "u_good"));
    rows.push_back({{"societyId", "soc1"}, {"flatNumber", "A-101"},
                    {"status", "active"}});                       // no sipUsername
    rows.push_back(sub_row("A-101", ""));                         // empty sipUsername
    db.rules.push_back({R"("flatNumber":"A-101")", rows.dump()});

    CallRouter router("soc1", db);
    auto targets = router.resolve_targets("A-101");

    ASSERT_EQ(1u, targets.size());
    EXPECT_EQ("u_good", targets[0]);
}

TEST(CallRouter, EmptyExtensionReturnsEmpty)
{
    FakeMongo db;
    CallRouter router("soc1", db);
    EXPECT_TRUE(router.resolve_targets("").empty());
}

TEST(CallRouter, MalformedOrFailedDbReturnsEmpty)
{
    FakeMongo db;
    db.rules.push_back({R"("flatNumber":"A-101")", "not json at all"});
    CallRouter router("soc1", db);
    EXPECT_TRUE(router.resolve_targets("A-101").empty()) << "garbage JSON → no route";

    FakeMongo throwing;
    throwing.throw_on_get = true;
    CallRouter router2("soc1", throwing);
    EXPECT_TRUE(router2.resolve_targets("A-101").empty()) << "DB throw → no route";
}
