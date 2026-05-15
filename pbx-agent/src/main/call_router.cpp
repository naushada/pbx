#include "call_router.hpp"

#include "json.hpp"

#include <utility>

using json = nlohmann::json;

namespace {
// DESIGN.md §9: digit "0" is the well-known guard extension. Everything
// else dialed is a flat number.
constexpr char kGuardExtension[] = "0";

// The Stasis appArgs tag a caller's StasisStart never carries but every
// originated leg does — see AriClient::handle_stasis_start.
constexpr char kOutboundTag[] = "outbound";
} // namespace

CallRouter::CallRouter(std::string society_id, IMongodbClient &db,
                       IAriRest &rest, std::string app_name)
    : m_society_id(std::move(society_id)), m_db(db), m_rest(rest),
      m_app_name(std::move(app_name)) {}

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

// ─────────────────────────────────────────────────────────────────────────────
// Forked-ring driver
// ─────────────────────────────────────────────────────────────────────────────

void CallRouter::on_caller_start(const std::string &caller,
                                 const std::string &caller_id,
                                 const std::string &dialed_ext) {
  // Asterisk can re-emit StasisStart for a channel — one routing pass
  // per caller.
  if (m_calls.count(caller)) return;

  const std::vector<std::string> targets = resolve_targets(dialed_ext);
  if (targets.empty()) {
    // Nothing to ring. Release the caller leg; the dialplan's Hangup()
    // after Stasis() returns turns this into a busy upstream.
    m_rest.hangup(caller, "congestion");
    return;
  }

  // Fork a ringing leg per target. The leg's channel id is ours to
  // assign (ARI `channelId` param) so we can track the fork without
  // round-tripping the originate response. appArgs carries
  // "outbound,<caller>" so the answered leg's StasisStart is
  // recognisable as a leg of *this* call.
  PendingCall call;
  call.dialed_ext = dialed_ext;
  std::size_t i = 0;
  for (const auto &user : targets) {
    const std::string leg = caller + "-leg-" + std::to_string(i++);
    const IAriRest::Response r = m_rest.originate(
        "PJSIP/" + user, m_app_name, std::string(kOutboundTag) + "," + caller,
        leg, caller_id);
    // A failed originate never produces a channel, so it would never
    // produce a ChannelDestroyed either — don't track it as ringing or
    // the all-failed check below could never fire.
    if (r.status >= 200 && r.status < 300) {
      call.ringing_legs.insert(leg);
      m_leg_to_caller[leg] = caller;
    }
  }

  if (call.ringing_legs.empty()) {
    // Every originate failed — same outcome as no route.
    m_rest.hangup(caller, "congestion");
    return;
  }
  m_calls.emplace(caller, std::move(call));
}

void CallRouter::on_leg_start(const std::string &leg,
                              const std::string &caller) {
  auto it = m_calls.find(caller);
  if (it == m_calls.end()) {
    // The caller is already gone (hung up while ringing) — there is
    // nothing left to bridge this answered leg to.
    m_rest.hangup(leg, "normal");
    return;
  }
  PendingCall &call = it->second;

  if (!call.connected_leg.empty()) {
    // Idempotent on a re-emitted StasisStart for the winner itself.
    if (call.connected_leg == leg) return;
    // A sibling already answered; this leg lost the forked-ring race.
    m_rest.hangup(leg, "answered");
    call.ringing_legs.erase(leg);
    return;
  }

  // First answer wins: bridge the caller with this leg, then tear down
  // every sibling that is still ringing.
  call.connected_leg = leg;
  call.ringing_legs.erase(leg);

  const std::string bridge = caller + "-bridge";
  m_rest.create_bridge(bridge, "mixing");
  m_rest.add_channel_to_bridge(bridge, caller);
  m_rest.add_channel_to_bridge(bridge, leg);

  for (const auto &loser : call.ringing_legs)
    m_rest.hangup(loser, "answered");
  call.ringing_legs.clear();
}

void CallRouter::on_channel_gone(const std::string &channel_id) {
  // Is it a caller? Tear down every leg still attached to its call.
  auto cit = m_calls.find(channel_id);
  if (cit != m_calls.end()) {
    for (const auto &leg : cit->second.ringing_legs)
      m_rest.hangup(leg, "normal");
    if (!cit->second.connected_leg.empty())
      m_rest.hangup(cit->second.connected_leg, "normal");
    // The m_leg_to_caller entries self-evict when those legs' own
    // ChannelDestroyed events arrive (their caller lookup will miss).
    m_calls.erase(cit);
    return;
  }

  // Is it a leg of some call?
  auto lit = m_leg_to_caller.find(channel_id);
  if (lit == m_leg_to_caller.end()) return; // unknown / already cleaned up
  const std::string caller = lit->second;
  m_leg_to_caller.erase(lit);

  auto call_it = m_calls.find(caller);
  if (call_it == m_calls.end()) return; // caller already torn down
  PendingCall &call = call_it->second;

  if (channel_id == call.connected_leg) {
    // The connected callee hung up → end the call, release the caller.
    m_rest.hangup(caller, "normal");
    m_calls.erase(call_it);
    return;
  }

  // A still-ringing (or just-torn-down loser) leg died.
  call.ringing_legs.erase(channel_id);
  if (call.connected_leg.empty() && call.ringing_legs.empty()) {
    // Every leg failed before anyone answered → no answer.
    m_rest.hangup(caller, "no_answer");
    m_calls.erase(call_it);
  }
}
