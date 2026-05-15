#include "call_router.hpp"

#include "json.hpp"

#include <utility>

using json = nlohmann::json;

namespace {
// DESIGN.md §9: digit "0" is the well-known guard extension. Everything
// else dialed is a flat number.
constexpr char kGuardExtension[] = "0";
} // namespace

CallRouter::CallRouter(std::string society_id, IMongodbClient &db)
    : m_society_id(std::move(society_id)), m_db(db) {}

std::vector<std::string>
CallRouter::resolve_targets(const std::string &dialed_ext) const {
  std::vector<std::string> targets;
  if (dialed_ext.empty()) return targets;

  // Society-scoped subscriber filter. "0" rings every role=guard
  // subscriber (DESIGN.md §9); any other extension is a flat number and
  // rings everyone on that flat (DESIGN.md §6.2). `flatNumber` is the
  // denormalized human string on the subscriber doc.
  json filter = {{"societyId", m_society_id}};
  if (dialed_ext == kGuardExtension)
    filter["role"] = "guard";
  else
    filter["flatNumber"] = dialed_ext;

  std::string rows;
  try {
    rows = m_db.get_documents("subscribers", filter.dump(), "{}");
  } catch (...) {
    return targets; // DB error → no route; caller handles the empty result
  }
  if (rows.empty()) return targets;

  json arr;
  try { arr = json::parse(rows); } catch (...) { return targets; }
  if (!arr.is_array()) return targets;

  for (const auto &row : arr) {
    if (!row.is_object()) continue;
    // A disabled subscriber keeps its flat row but must not be rung.
    if (row.contains("status") && row["status"].is_string() &&
        row["status"].get<std::string>() != "active")
      continue;
    if (row.contains("sipUsername") && row["sipUsername"].is_string()) {
      std::string user = row["sipUsername"].get<std::string>();
      if (!user.empty()) targets.push_back(std::move(user));
    }
  }
  return targets;
}
