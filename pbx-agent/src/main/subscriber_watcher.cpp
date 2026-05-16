#include "subscriber_watcher.hpp"

#include "json.hpp"

#include "ace/Log_Msg.h"

#include <utility>

using json = nlohmann::json;

namespace {

// nlohmann encodes Mongo ObjectId values as `{"$oid":"<hex>"}`. Reduce
// to the bare hex string so the cache key is small + comparable. Falls
// back to dump() of the whole node if the shape is unexpected (still a
// valid stable key).
std::string oid_str(const json &id_node) {
  if (id_node.is_object() && id_node.contains("$oid") &&
      id_node["$oid"].is_string())
    return id_node["$oid"].get<std::string>();
  return id_node.dump();
}

} // namespace

SubscriberWatcher::SubscriberWatcher(IMongodbClient &db,
                                      std::string society_id,
                                      PjsipProvisioner &provisioner)
    : m_db(db), m_society_id(std::move(society_id)),
      m_provisioner(provisioner) {}

void SubscriberWatcher::bootstrap() {
  // Society-scoped scan. Provision every active row; the cache is also
  // populated so a later delete event yields its sipUsername.
  std::string raw;
  try {
    raw = m_db.get_documents(
        "subscribers",
        R"({"societyId":")" + m_society_id + R"("})",
        "{}");
  } catch (const std::exception &e) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] SubscriberWatcher::bootstrap: "
                        "get_documents failed: %s\n"), e.what()));
    return;
  }

  if (!raw.empty()) {
    json arr;
    try { arr = json::parse(raw); }
    catch (const std::exception &e) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [pbx-agent] SubscriberWatcher::bootstrap: "
                          "JSON parse failed: %s\n"), e.what()));
    }
    if (arr.is_array()) {
      for (const auto &row : arr) {
        if (!row.is_object()) continue;
        const std::string sip_user = row.value("sipUsername", std::string{});
        const std::string sip_ha1  = row.value("sipHa1",      std::string{});
        const std::string status   = row.value("status",      std::string{"active"});
        if (sip_user.empty()) continue;

        if (row.contains("_id"))
          m_id_to_sipuser[oid_str(row["_id"])] = sip_user;

        if (status == "active") {
          m_provisioner.provision(sip_user, sip_ha1);
        } else {
          // Pre-disabled subscribers shouldn't be in Asterisk. A delete
          // is cheap and idempotent.
          m_provisioner.deprovision(sip_user);
        }
      }
    }
  }

  // Open the change stream after the bootstrap so we don't double-apply
  // an in-flight insert (the bootstrap covers everything up to "now";
  // events from "now" forward feed in via the cursor).
  open_stream();
}

void SubscriberWatcher::open_stream() {
  m_stream = m_db.watch_collection("subscribers", m_resume_token);
  if (!m_stream) {
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [pbx-agent] SubscriberWatcher: change stream "
                        "unavailable (mongod likely not a replica set, "
                        "or resume token aged out); bootstrap-only mode "
                        "until next reopen attempt\n")));
  }
}

void SubscriberWatcher::tick() {
  if (!m_stream) {
    // Reopen path. Bootstrap may never have got a cursor (standalone
    // mongod), or a prior try_next exception tore it down. The skip
    // counter keeps us off Mongo while a transient failure is healing.
    if (m_reopen_skip_ticks > 0) { --m_reopen_skip_ticks; return; }
    open_stream();
    if (!m_stream) {
      // Still broken — back off. At 200 ms cadence ~5 ticks is ~1 s.
      m_reopen_skip_ticks = 5;
    }
    return;
  }
  // Pull one event per tick — keeps the reactor responsive; if events
  // back up the next tick (~200 ms later) drains another. For a high-
  // churn society we could loop here with a budget, but v1 says one.
  std::string evt;
  try { evt = m_stream->try_next(0); }
  catch (const std::exception &e) {
    // Cursor's broken (Mongo disconnect, mid-stream error). Drop it and
    // arm reopen — `m_resume_token` from the last successfully-applied
    // event drives the server-side replay so nothing in the oplog
    // window gets lost.
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] SubscriberWatcher::tick: "
                        "change-stream try_next failed: %s; will "
                        "reopen with resume token\n"), e.what()));
    m_stream.reset();
    m_reopen_skip_ticks = 0; // try on next tick immediately
    return;
  }
  if (evt.empty()) return;
  handle_event(evt);
}

void SubscriberWatcher::handle_event(const std::string &event_json) {
  json e;
  try { e = json::parse(event_json); }
  catch (const std::exception &ex) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [pbx-agent] SubscriberWatcher: bad change-stream "
                        "JSON: %s\n"), ex.what()));
    return;
  }

  // Capture this event's resume token BEFORE dispatching. Even if the
  // dispatch is a silent no-op (other society, unknown id), the server
  // has already moved past this event — we must NOT re-receive it on
  // the next reopen, or we'd re-process every dispatched mutation
  // every time Mongo flaps.
  if (e.contains("_id")) m_resume_token = e["_id"].dump();

  const std::string op = e.value("operationType", std::string{});

  if (op == "delete") {
    // documentKey._id is the only identifier we get on delete. Resolve
    // through the cache populated on bootstrap + every prior insert/update.
    if (!e.contains("documentKey") || !e["documentKey"].is_object() ||
        !e["documentKey"].contains("_id"))
      return;
    const std::string id = oid_str(e["documentKey"]["_id"]);
    auto it = m_id_to_sipuser.find(id);
    if (it == m_id_to_sipuser.end()) return; // never saw this _id
    m_provisioner.deprovision(it->second);
    m_id_to_sipuser.erase(it);
    return;
  }

  // insert / update / replace all carry a fullDocument by default.
  if (!e.contains("fullDocument") || !e["fullDocument"].is_object()) return;
  const auto &doc = e["fullDocument"];

  // Society scope — events for other societies on the same Mongo are
  // not ours to act on.
  if (doc.value("societyId", std::string{}) != m_society_id) return;

  const std::string sip_user = doc.value("sipUsername", std::string{});
  const std::string sip_ha1  = doc.value("sipHa1",      std::string{});
  const std::string status   = doc.value("status",      std::string{"active"});
  if (sip_user.empty()) return;

  if (doc.contains("_id"))
    m_id_to_sipuser[oid_str(doc["_id"])] = sip_user;

  if (status == "active") {
    m_provisioner.provision(sip_user, sip_ha1);
  } else {
    m_provisioner.deprovision(sip_user);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test seam — definition for the struct declared at the bottom of the
// header. Lets the spec inject events without standing up a real cursor.
// ─────────────────────────────────────────────────────────────────────────────

void SubscriberWatcherTestAccess::deliver(SubscriberWatcher &w,
                                           const std::string &event_json) {
  w.handle_event(event_json);
}
