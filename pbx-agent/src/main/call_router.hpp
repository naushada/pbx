#ifndef CALL_ROUTER_HPP
#define CALL_ROUTER_HPP

#include "mongodbc.hpp"
#include <string>
#include <vector>

/**
 * @file call_router.hpp
 * @brief Resolves a dialed extension to the SIP endpoints to ring.
 *
 * `pbx-agent`'s dialplan is a thin `Stasis(pbx,${EXTEN})` passthrough —
 * the agent does the routing. `CallRouter` is that routing logic.
 *
 * PR #1a (this file) ships the **resolver** only: the data lookup that
 * turns a dialed extension into the set of `sipUsername`s to ring.
 * PR #1b grows `CallRouter` with the ARI side — originate a leg per
 * target, bridge the answered leg, forked-ring first-answer-wins.
 *
 * Dialed-extension semantics (DESIGN.md §6.2, §9):
 *   - `"0"`         → every active `role=guard` subscriber in the society
 *   - any other ext → a flat number; every active subscriber on that flat
 *
 * The flat lookup is a direct `subscribers` query because the human flat
 * string is denormalized onto each subscriber doc as `flatNumber` (see
 * `handle_subscriber_import_POST`) — no `flats` join needed.
 *
 * Pure logic — `IMongodbClient` is injected; reactor-thread
 * single-threaded, no internal locking.
 */
class CallRouter {
public:
  /// @param society_id  Society this agent serves; scopes every lookup.
  /// @param db          Local Mongo (subscribers collection).
  CallRouter(std::string society_id, IMongodbClient &db);

  /// Resolve @p dialed_ext to the `sipUsername`s that should be rung.
  ///
  /// Returns an empty vector for an unknown flat, the guard extension
  /// when no guards exist, an empty/malformed extension, or a DB error —
  /// callers treat "no targets" as "no route" (the caller leg should get
  /// a busy/again response). Subscribers whose `status` is not "active"
  /// are excluded; rows with a missing/empty `sipUsername` are skipped.
  /// Order is unspecified.
  std::vector<std::string> resolve_targets(const std::string &dialed_ext) const;

private:
  std::string     m_society_id;
  IMongodbClient &m_db;
};

#endif // CALL_ROUTER_HPP
