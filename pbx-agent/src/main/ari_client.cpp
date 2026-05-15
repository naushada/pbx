#include "ari_client.hpp"

#include "call_router.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {
// appArgs[0] on a StasisStart that belongs to a leg CallRouter
// originated (vs. a fresh caller leg from the dialplan's Stasis()).
constexpr char kOutboundTag[] = "outbound";
} // namespace

AriClient::AriClient(Config cfg, IAriRest &rest, IMongodbClient &db,
                     CallRouter &router)
    : m_cfg(std::move(cfg)), m_rest(rest), m_db(db), m_router(router) {}

void AriClient::start() {
  // v1 wants channel + bridge events for admission control + CDR, and
  // endpoint events so the cloud's presence cache hears about REGISTER
  // state changes (DESIGN.md §4 directory `online` field).
  m_rest.subscribe(m_cfg.app_name, {"channel:", "bridge:", "endpoint:"});
}

void AriClient::on_event(const std::string &json_event) {
  json j;
  try {
    j = json::parse(json_event);
  } catch (...) {
    return; // malformed event — log + drop
  }

  if (!j.contains("type") || !j["type"].is_string()) return;

  const std::string type = j["type"].get<std::string>();
  if (type == "StasisStart")            handle_stasis_start(j);
  else if (type == "BridgeCreated")     handle_bridge_created(j);
  else if (type == "BridgeDestroyed")   handle_bridge_destroyed(j);
  else if (type == "ChannelEnteredBridge")
                                        handle_channel_entered_bridge(j);
  else if (type == "ChannelDestroyed")  handle_channel_destroyed(j);
  else if (type == "EndpointStateChange")
                                        handle_endpoint_state_change(j);
  // Other event types intentionally ignored.
}

// ─────────────────────────────────────────────────────────────────────────────
// EndpointStateChange — agent → cloud REGISTER state replication.
// ─────────────────────────────────────────────────────────────────────────────

void AriClient::handle_endpoint_state_change(const json &j) {
  if (!m_register_state_handler) return;
  if (!j.contains("endpoint") || !j["endpoint"].is_object()) return;
  const auto &ep = j["endpoint"];

  // Only PJSIP endpoints are subscribers — other technologies (e.g. local,
  // sim, IAX2 fixtures) are noise on the presence cache.
  const std::string tech = ep.value("technology", std::string{});
  if (tech != "PJSIP") return;

  const std::string resource = ep.value("resource", std::string{});
  if (resource.empty()) return;

  // ARI endpoint.state is one of "online", "offline", "unknown" — anything
  // not "online" maps to offline (safe default for the directory's call
  // button gating).
  const std::string state = ep.value("state", std::string{});
  m_register_state_handler(resource, state == "online");
}

void AriClient::set_register_state_handler(RegisterStateHandler h) {
  m_register_state_handler = std::move(h);
}

// ─────────────────────────────────────────────────────────────────────────────
// revoke_subscriber — cut a disabled/removed subscriber's live calls.
// ─────────────────────────────────────────────────────────────────────────────

void AriClient::revoke_subscriber(const std::string &sip_username) {
  if (sip_username.empty()) return;

  // Ask Asterisk which channels the subscriber's endpoint currently owns.
  // The endpoint resource is the bare sipUsername (the `PJSIP/` tech prefix
  // is how CallRouter addresses it on originate, too).
  const IAriRest::Response r = m_rest.get_endpoint("PJSIP", sip_username);
  if (r.status < 200 || r.status >= 300)
    return; // unknown endpoint / ARI error — nothing we can do

  json ep;
  try { ep = json::parse(r.body); } catch (...) { return; }
  if (!ep.is_object() || !ep.contains("channel_ids") ||
      !ep["channel_ids"].is_array())
    return;

  // Hang every one of them up. A revoked subscriber should have no live
  // media at all — the cloud already dropped their session + status.
  for (const auto &cid : ep["channel_ids"]) {
    if (!cid.is_string()) continue;
    const std::string id = cid.get<std::string>();
    if (!id.empty()) m_rest.hangup(id, "normal");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// StasisStart — first time we see a new channel. Admission decision here.
// ─────────────────────────────────────────────────────────────────────────────

void AriClient::handle_stasis_start(const json &j) {
  if (!j.contains("channel") || !j["channel"].is_object()) return;
  const auto &ch = j["channel"];
  if (!ch.contains("id") || !ch["id"].is_string()) return;
  const std::string channel_id = ch["id"].get<std::string>();

  // Classify. A leg CallRouter originated re-enters Stasis with appArgs
  // ["outbound", "<callerChannelId>"]; everything else is a fresh caller
  // leg from the dialplan's Stasis(). An originated leg is part of an
  // already-admitted call — it gets neither an admission check (that
  // would double-count) nor a CDR context (the logical call's CDR is
  // keyed on the caller channel, not the leg).
  if (j.contains("args") && j["args"].is_array() && j["args"].size() >= 2 &&
      j["args"][0].is_string() && j["args"][1].is_string() &&
      j["args"][0].get<std::string>() == kOutboundTag) {
    m_router.on_leg_start(channel_id, j["args"][1].get<std::string>());
    return;
  }

  // Admission: count bridges, not channels. The 6th INVITE (i.e. the
  // one that would create the 6th bridge if accepted) is bounced back
  // to dialplan with `continue` — the dialplan plays a busy message
  // and the caller sees 503.
  if (over_admission_cap()) {
    m_rest.continue_in_dialplan(channel_id, m_cfg.busy_context,
                                 m_cfg.busy_extension, m_cfg.busy_priority);
    return;
  }

  // Build per-channel CDR context. ARI's StasisStart carries:
  //   channel.caller.number  → who's calling     (we map to caller_flat)
  //   channel.dialplan.exten → who they're calling
  //   args[0]                → also typically the dialed extension
  ChannelCtx cx;
  cx.channel_id = channel_id;
  cx.started_at = std::time(nullptr);
  if (ch.contains("caller") && ch["caller"].is_object() &&
      ch["caller"].contains("number") && ch["caller"]["number"].is_string()) {
    cx.caller_flat = ch["caller"]["number"].get<std::string>();
  }
  if (ch.contains("dialplan") && ch["dialplan"].is_object() &&
      ch["dialplan"].contains("exten") && ch["dialplan"]["exten"].is_string()) {
    cx.callee_flat = ch["dialplan"]["exten"].get<std::string>();
  }
  if (cx.callee_flat.empty() && j.contains("args") && j["args"].is_array() &&
      !j["args"].empty() && j["args"][0].is_string()) {
    cx.callee_flat = j["args"][0].get<std::string>();
  }

  // Hand the dialed extension to the router — it resolves the flat and
  // forks a ringing leg per target (or hangs the caller up on no route).
  const std::string caller_flat = cx.caller_flat;
  const std::string callee_flat = cx.callee_flat;
  m_channels.emplace(channel_id, std::move(cx));
  m_router.on_caller_start(channel_id, caller_flat, callee_flat);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bridge lifecycle — drives admission counter.
// ─────────────────────────────────────────────────────────────────────────────

void AriClient::handle_bridge_created(const json &j) {
  if (!j.contains("bridge") || !j["bridge"].is_object()) return;
  const auto &br = j["bridge"];
  if (!br.contains("id") || !br["id"].is_string()) return;

  BridgeCtx bx;
  bx.bridge_id = br["id"].get<std::string>();
  if (br.contains("bridge_type") && br["bridge_type"].is_string())
    bx.bridge_type = br["bridge_type"].get<std::string>();

  // Asterisk re-emits BridgeCreated occasionally; guard against double-count.
  if (m_bridges.find(bx.bridge_id) != m_bridges.end()) return;

  m_bridges.emplace(bx.bridge_id, std::move(bx));
  ++m_active_bridges;
}

void AriClient::handle_bridge_destroyed(const json &j) {
  if (!j.contains("bridge") || !j["bridge"].is_object()) return;
  const auto &br = j["bridge"];
  if (!br.contains("id") || !br["id"].is_string()) return;

  if (m_bridges.erase(br["id"].get<std::string>()) > 0) {
    if (m_active_bridges > 0) --m_active_bridges;
    // Clamp: any extra BridgeDestroyed echoes are absorbed.
  }
}

void AriClient::handle_channel_entered_bridge(const json &j) {
  if (!j.contains("channel") || !j["channel"].is_object()) return;
  if (!j.contains("bridge")  || !j["bridge"].is_object())  return;

  const std::string channel_id = j["channel"].value("id", std::string{});
  const std::string bridge_id  = j["bridge"].value("id",  std::string{});
  if (channel_id.empty() || bridge_id.empty()) return;

  auto ch_it = m_channels.find(channel_id);
  if (ch_it != m_channels.end()) {
    ch_it->second.bridge_id = bridge_id;
    if (ch_it->second.answered_at == 0)
      ch_it->second.answered_at = std::time(nullptr);
  }
  auto br_it = m_bridges.find(bridge_id);
  if (br_it != m_bridges.end()) br_it->second.channel_ids.insert(channel_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// ChannelDestroyed — finalize CDR.
// ─────────────────────────────────────────────────────────────────────────────

void AriClient::handle_channel_destroyed(const json &j) {
  if (!j.contains("channel") || !j["channel"].is_object()) return;
  const std::string channel_id = j["channel"].value("id", std::string{});
  if (channel_id.empty()) return;

  // Notify the router first: the destroyed channel may be a ringing leg
  // (drop it; maybe release the caller on all-failed) or the caller
  // itself (tear down its legs). Legs have no CDR context here, so the
  // early return below skips them — the router is their only handler.
  m_router.on_channel_gone(channel_id);

  auto it = m_channels.find(channel_id);
  if (it == m_channels.end()) return; // we never saw the StasisStart
  ChannelCtx cx = std::move(it->second);
  m_channels.erase(it);

  // Determine call type from the bridge's population. A bridge with
  // >= 3 simultaneous channels is conference; otherwise p2p.
  std::string type = "p2p";
  if (!cx.bridge_id.empty()) {
    auto br_it = m_bridges.find(cx.bridge_id);
    if (br_it != m_bridges.end() && br_it->second.channel_ids.size() >= 3)
      type = "conference";
  }

  const std::time_t ended_at = std::time(nullptr);
  const long duration_sec =
      (cx.answered_at > 0 && ended_at >= cx.answered_at)
          ? static_cast<long>(ended_at - cx.answered_at)
          : 0;

  json cdr;
  cdr["societyId"]    = m_cfg.society_id;
  cdr["callId"]       = channel_id;
  cdr["fromFlat"]     = cx.caller_flat;
  cdr["toFlat"]       = cx.callee_flat;
  cdr["startedAt"]    = static_cast<std::int64_t>(cx.started_at);
  cdr["answeredAt"]   = static_cast<std::int64_t>(cx.answered_at);
  cdr["endedAt"]      = static_cast<std::int64_t>(ended_at);
  cdr["durationSec"]  = duration_sec;
  cdr["type"]         = type;
  if (!cx.bridge_id.empty()) cdr["conferenceBridge"] = cx.bridge_id;

  // Hangup cause: ARI puts a numeric `cause` and an optional `cause_txt`.
  // Match Asterisk's standard cause strings case-insensitively so e.g.
  // both "User busy" and "Busy" map to the same enum value.
  std::string cause = "normal";
  if (j.contains("cause_txt") && j["cause_txt"].is_string()) {
    std::string lc = j["cause_txt"].get<std::string>();
    std::string original = lc;
    std::transform(lc.begin(), lc.end(), lc.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    if      (lc.find("busy")      != std::string::npos) cause = "busy";
    else if (lc.find("no answer") != std::string::npos) cause = "missed";
    else if (lc.find("normal")    != std::string::npos) cause = "normal";
    else cause = std::move(original);
  }
  cdr["hangupCause"] = cause;

  m_db.create_document(m_db.get_database(), "cdr", cdr.dump());
}
