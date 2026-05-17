#ifndef ARI_CLIENT_HPP
#define ARI_CLIENT_HPP

#include "mongodbc.hpp"
#include "json.hpp"
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @file ari_client.hpp
 * @brief Asterisk REST Interface (ARI) consumer for `pbx-agent`.
 *
 * Receives parsed ARI events (`StasisStart`, `BridgeCreated`,
 * `BridgeDestroyed`, `ChannelEnteredBridge`, `ChannelDestroyed`, …) and
 * does four jobs:
 *
 *   0. **Call routing** — on a caller's `StasisStart`, hands the dialed
 *      extension to `CallRouter`, which resolves it to the SIP targets
 *      and forks a ringing leg per target. An originated leg re-enters
 *      Stasis tagged `outbound,<callerChannelId>`; `AriClient`
 *      recognises that tag, routes it to `CallRouter::on_leg_start`,
 *      and skips admission + CDR for it (a leg is not its own call).
 *
 *   1. **Admission control** — counts active bridges (NOT channels —
 *      a 1:1 call has two channels but a single bridge; see
 *      `DESIGN.md §6.5`). When a new `StasisStart` would push the
 *      society over `max_concurrent_calls`, issues a REST `continue`
 *      to route the channel to a busy-handler dialplan extension.
 *
 *   2. **CDR finalization** — accumulates per-channel state from
 *      `StasisStart` + `ChannelEnteredBridge` events; on
 *      `ChannelDestroyed` builds and writes a CDR document to the
 *      `cdr` collection. Conference vs P2P is determined from the
 *      bridge's participant count.
 *
 *   3. **App subscription** — on `start()`, issues the REST call(s)
 *      needed to subscribe the Stasis app for the relevant event
 *      sources.
 *
 * I/O is dependency-injected. Production wires `IAriRest` to an HTTP
 * client against `http://127.0.0.1:8088/ari/...`; the WebSocket event
 * stream is owned externally and pushes parsed events into
 * `on_event()`. Tests drive both directly.
 *
 * Reactor-thread single-threaded; no internal locking.
 */

/// Asterisk ARI REST adapter. The methods that matter for v1:
class IAriRest {
public:
  virtual ~IAriRest() = default;

  struct Response {
    int status = 0;
    std::string body;
  };

  /// `POST /ari/applications/{app}/subscription?eventSource=…` for each
  /// source we care about. Tests record the call.
  virtual Response subscribe(const std::string &app_name,
                              const std::vector<std::string> &event_sources) = 0;

  /// `POST /ari/channels/{channel_id}/continue` — route the channel
  /// back to a dialplan extension. Used to escape the Stasis app when
  /// admission is denied (we hand the call to a busy-handler).
  virtual Response continue_in_dialplan(const std::string &channel_id,
                                         const std::string &context,
                                         const std::string &extension,
                                         int priority) = 0;

  /// `POST /ari/channels` — originate a new channel that re-enters the
  /// Stasis app on answer. `CallRouter` forks one of these per ringing
  /// target. @p channel_id pre-assigns the channel id (ARI `channelId`
  /// param) so the fork can be tracked without parsing the response
  /// body; @p app_args is the comma-joined Stasis arg list the answered
  /// leg re-enters with (we use `outbound,<callerChannelId>`).
  virtual Response originate(const std::string &endpoint,
                              const std::string &app,
                              const std::string &app_args,
                              const std::string &channel_id,
                              const std::string &caller_id) = 0;

  /// `POST /ari/bridges` — create a bridge. @p bridge_id pre-assigns the
  /// id; @p type is `"mixing"` for both 1:1 and conference calls.
  virtual Response create_bridge(const std::string &bridge_id,
                                  const std::string &type) = 0;

  /// `POST /ari/bridges/{bridge_id}/addChannel` — pull a channel into a
  /// bridge. Adding a still-ringing channel answers it.
  virtual Response add_channel_to_bridge(const std::string &bridge_id,
                                          const std::string &channel_id) = 0;

  /// `DELETE /ari/channels/{channel_id}` — hang up. @p reason is an ARI
  /// hangup reason: `"normal"`, `"busy"`, `"congestion"`, `"no_answer"`,
  /// or `"answered"` (the latter for forked-ring legs that lost the race).
  virtual Response hangup(const std::string &channel_id,
                           const std::string &reason) = 0;

  /// `GET /ari/endpoints/{tech}/{resource}` — fetch one endpoint's state.
  /// The response body carries a `channel_ids` array of the channels that
  /// endpoint currently owns; `AriClient::revoke_subscriber` uses it to
  /// find (and hang up) a revoked subscriber's live calls.
  virtual Response get_endpoint(const std::string &tech,
                                 const std::string &resource) = 0;

  /// `GET /ari/endpoints/{tech}` — list every endpoint of @p tech. The
  /// body is a JSON array of objects, each carrying at least
  /// `technology`, `resource`, and `state`.
  /// `AriClient::publish_register_snapshot` calls this on tunnel
  /// reconnect to refresh the cloud's presence cache — Asterisk's
  /// `EndpointStateChange` only fires on actual changes, so a
  /// disconnect can leave the cache stale.
  virtual Response list_endpoints(const std::string &tech) = 0;

  /// `PUT /ari/asterisk/config/dynamic/{cfg_class}/{obj_type}/{id}` —
  /// create or replace a sorcery-memory config object. @p fields_json is
  /// the request body the caller has already serialised:
  ///   `{"fields":[{"attribute":"...","value":"..."}, ...]}`
  /// Used by `PjsipProvisioner` to push pjsip endpoint/auth/aor objects
  /// into Asterisk's memory sorcery without rewriting `pjsip.conf` or
  /// running a `pjsip reload`.
  virtual Response create_dynamic_config(const std::string &cfg_class,
                                          const std::string &obj_type,
                                          const std::string &id,
                                          const std::string &fields_json) = 0;

  /// `DELETE /ari/asterisk/config/dynamic/{cfg_class}/{obj_type}/{id}` —
  /// remove a previously-created sorcery-memory config object. Idempotent
  /// at the agent level: calling on an unknown id returns 404 which the
  /// `PjsipProvisioner` treats as already-gone (silent no-op).
  virtual Response delete_dynamic_config(const std::string &cfg_class,
                                          const std::string &obj_type,
                                          const std::string &id) = 0;
};

/// Resolves dialed extensions and drives the forked-ring originate/bridge
/// state machine. Defined in `call_router.hpp`; `AriClient` only holds a
/// reference, so a forward declaration is enough here.
class CallRouter;

class AriClient {
public:
  struct Config {
    /// Society this agent serves. Stamped on every CDR row.
    std::string society_id;

    /// Stasis application name. Stamped into the `subscribe()` call.
    std::string app_name = "pbx";

    /// Admission cap — counts ACTIVE BRIDGES (logical calls), not
    /// channels. v1 default per DESIGN.md §1 is 5.
    int max_concurrent_calls = 5;

    /// Where to send rejected channels. Dialplan extension that should
    /// play "all lines busy" and hang up. Default matches the typical
    /// pjsip.conf wiring.
    std::string busy_context   = "pbx-busy";
    std::string busy_extension = "s";
    int         busy_priority  = 1;
  };

  AriClient(Config cfg, IAriRest &rest, IMongodbClient &db,
            CallRouter &router);

  /// Subscribe the Stasis app for the event sources we care about.
  /// Idempotent — production calls this once at boot; tests call it
  /// explicitly to assert on the subscription side effect.
  void start();

  /// Process one parsed ARI event. JSON shape is whatever Asterisk
  /// sent us — we extract the fields we need and tolerate the rest.
  void on_event(const std::string &json_event);

  /// Tear down a subscriber's live calls. Looks up the
  /// `PJSIP/<sip_username>` endpoint via ARI and hangs up every channel
  /// it currently owns. Driven by a `SUBSCRIBER_REVOKED` tunnel frame —
  /// the cloud admin disabled or removed the subscriber. Best-effort: a
  /// no-such-endpoint / ARI error / empty channel list is a silent no-op.
  void revoke_subscriber(const std::string &sip_username);

  /// Install a callback fired on every Asterisk `EndpointStateChange`
  /// ARI event. Production wires this to a `CloudConnector::send_frame`
  /// emitting a `REGISTER_STATE` SipFrame so the cloud's presence cache
  /// stays in sync; tests substitute a recorder. Only `PJSIP/<resource>`
  /// endpoints fire the callback — other technologies are ignored.
  using RegisterStateHandler = std::function<void(
      const std::string &sip_username, bool online)>;
  void set_register_state_handler(RegisterStateHandler h);

  /// Install a callback fired with the agent's view of whether Asterisk
  /// is reachable via ARI. Production wires this to a
  /// `CloudConnector::send_frame` emitting an `ASTERISK_STATUS` SipFrame
  /// so the cloud's admin connectivity chip can render a real signal
  /// instead of the hardcoded "pending" placeholder; tests substitute a
  /// recorder. Driven by the same `list_endpoints` ARI round-trip that
  /// powers `publish_register_snapshot()` — costs zero new periodic work.
  using AsteriskStatusHandler = std::function<void(bool connected)>;
  void set_asterisk_status_handler(AsteriskStatusHandler h);

  /// Refresh the cloud's presence cache with the agent's current view
  /// of every PJSIP endpoint. Calls `GET /ari/endpoints/PJSIP`, then
  /// drives `m_register_state_handler` once per endpoint. Idempotent —
  /// re-publishing the same state is just an upsert on the cache.
  ///
  /// Production: invoked from a `CloudConnector::set_on_connected`
  /// callback so a reconnect resynchronises the cache. Without this,
  /// any state change that happened while the tunnel was down stays
  /// invisible until the next live `EndpointStateChange` event (which
  /// might not arrive for hours on a quiet society).
  void publish_register_snapshot();

  // ── Observability ──────────────────────────────────────────────────────

  int active_bridges() const { return m_active_bridges; }

private:
  // Per-call accumulators keyed by channel id. Built up during
  // StasisStart / ChannelEnteredBridge and consumed on
  // ChannelDestroyed.
  struct ChannelCtx {
    std::string channel_id;
    std::string caller_flat;   // e.g. "A-101"
    std::string callee_flat;   // dialed extension (StasisStart args[0])
    std::string bridge_id;
    std::time_t started_at  = 0;
    std::time_t answered_at = 0;
  };
  // Per-bridge accumulators keyed by bridge id.
  struct BridgeCtx {
    std::string bridge_id;
    std::string bridge_type;          // "mixing", "holding", …
    std::unordered_set<std::string> channel_ids;
  };

  void handle_stasis_start    (const nlohmann::json &);
  void handle_bridge_created  (const nlohmann::json &);
  void handle_bridge_destroyed(const nlohmann::json &);
  void handle_channel_entered_bridge(const nlohmann::json &);
  void handle_channel_destroyed(const nlohmann::json &);
  void handle_endpoint_state_change (const nlohmann::json &);

  bool over_admission_cap() const { return m_active_bridges >= m_cfg.max_concurrent_calls; }

  Config           m_cfg;
  IAriRest        &m_rest;
  IMongodbClient  &m_db;
  CallRouter      &m_router;

  int m_active_bridges = 0;

  std::unordered_map<std::string, ChannelCtx> m_channels;
  std::unordered_map<std::string, BridgeCtx>  m_bridges;

  RegisterStateHandler  m_register_state_handler;
  AsteriskStatusHandler m_asterisk_status_handler;
};

#endif // ARI_CLIENT_HPP
