#ifndef CALL_ROUTER_HPP
#define CALL_ROUTER_HPP

#include "ari_client.hpp"   // IAriRest
#include "mongodbc.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @file call_router.hpp
 * @brief Resolves a dialed extension and drives the forked-ring call.
 *
 * `pbx-agent`'s dialplan is a thin `Stasis(pbx,${EXTEN})` passthrough —
 * the agent does the routing. `CallRouter` is that routing logic, in
 * two halves:
 *
 *   1. **Resolver** — `resolve_targets()` turns a dialed extension into
 *      the set of `sipUsername`s to ring (data lookup, pure logic).
 *   2. **Forked-ring driver** — `on_caller_start()` / `on_leg_start()` /
 *      `on_channel_gone()` originate a ringing leg per target, bridge
 *      the first to answer, and tear the rest down (first-answer-wins).
 *
 * Dialed-extension semantics (DESIGN.md §6.2, §9):
 *   - `"0"`         → every active `role=guard` subscriber in the society
 *   - any other ext → a flat number; every active subscriber on that flat
 *
 * The flat lookup is a direct `subscribers` query because the human flat
 * string is denormalized onto each subscriber doc as `flatNumber` (see
 * `handle_subscriber_import_POST`) — no `flats` join needed.
 *
 * **Event wiring.** `AriClient` owns the ARI event stream and forwards:
 *   - a caller's `StasisStart`        → `on_caller_start()`
 *   - an originated leg's `StasisStart` (tagged `outbound,<caller>`)
 *                                     → `on_leg_start()`
 *   - any `ChannelDestroyed`          → `on_channel_gone()`
 * `CallRouter` reacts by calling `IAriRest` (originate / create_bridge /
 * add_channel_to_bridge / hangup). The originate→answer round-trip
 * arrives back as a fresh `StasisStart`, so the driver is a pure state
 * machine over events — it never blocks waiting for a leg to answer.
 *
 * Reactor-thread single-threaded; no internal locking.
 */
class CallRouter {
public:
  /// @param society_id  Society this agent serves; scopes every lookup.
  /// @param db          Local Mongo (subscribers collection).
  /// @param rest        ARI REST surface used to originate / bridge / hang up.
  /// @param app_name    Stasis app name an originated leg re-enters on answer.
  CallRouter(std::string society_id, IMongodbClient &db, IAriRest &rest,
             std::string app_name = "pbx");

  /// Resolve @p dialed_ext to the `sipUsername`s that should be rung.
  ///
  /// Returns an empty vector for an unknown flat, the guard extension
  /// when no guards exist, an empty/malformed extension, or a DB error —
  /// callers treat "no targets" as "no route". Subscribers whose
  /// `status` is not "active" are excluded; rows with a missing/empty
  /// `sipUsername` are skipped. Order is unspecified.
  std::vector<std::string> resolve_targets(const std::string &dialed_ext) const;

  // ── Forked-ring driver ─────────────────────────────────────────────────

  /// A caller channel entered Stasis. Resolve @p dialed_ext and originate
  /// one ringing leg per target, tagging each leg `outbound,<caller>` so
  /// its answer is recognisable. No targets (no route) → hang up the
  /// caller. Idempotent per @p caller_channel_id (Asterisk re-emits).
  void on_caller_start(const std::string &caller_channel_id,
                       const std::string &caller_id,
                       const std::string &dialed_ext);

  /// An originated leg answered and re-entered Stasis. The first leg to
  /// do so wins: bridge it with the caller and hang up the siblings that
  /// are still ringing. A later leg (lost the race, or its caller is
  /// already gone) is hung up.
  void on_leg_start(const std::string &leg_channel_id,
                    const std::string &caller_channel_id);

  /// A channel was destroyed. If it's a ringing leg, drop it — and when
  /// the last leg dies with no one having answered, hang up the caller
  /// (no answer). If it's the connected leg, release the caller. If it's
  /// the caller, hang up whatever legs are still attached.
  void on_channel_gone(const std::string &channel_id);

  // ── Observability ──────────────────────────────────────────────────────

  /// Calls currently being routed (ringing or bridged).
  std::size_t active_calls() const { return m_calls.size(); }

private:
  // One in-flight call, keyed in m_calls by the caller's channel id.
  struct PendingCall {
    std::string dialed_ext;
    // Legs originated for this call that have not yet answered. The
    // winner is moved out of this set into connected_leg on answer.
    std::unordered_set<std::string> ringing_legs;
    // The leg that answered first and is bridged with the caller; empty
    // until first-answer-wins fires.
    std::string connected_leg;
  };

  std::string     m_society_id;
  IMongodbClient &m_db;
  IAriRest       &m_rest;
  std::string     m_app_name;

  // caller channel id → its in-flight call.
  std::unordered_map<std::string, PendingCall> m_calls;
  // leg channel id → owning caller channel id. Reverse index so a leg's
  // ChannelDestroyed can find its call. Entries self-evict in
  // on_channel_gone when the leg is actually destroyed.
  std::unordered_map<std::string, std::string> m_leg_to_caller;
};

#endif // CALL_ROUTER_HPP
