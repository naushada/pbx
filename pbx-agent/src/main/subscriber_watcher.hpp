#ifndef SUBSCRIBER_WATCHER_HPP
#define SUBSCRIBER_WATCHER_HPP

#include "mongodbc.hpp"
#include "pjsip_provisioner.hpp"

#include <memory>
#include <string>
#include <unordered_map>

/**
 * @file subscriber_watcher.hpp
 * @brief Keep Asterisk's pjsip endpoints in sync with the `subscribers`
 *        Mongo collection — bootstrap on boot + Mongo change-stream tail
 *        for live updates.
 *
 * Architecture (DESIGN.md §4):
 *
 *   Mongo `subscribers` ──change stream──▶ SubscriberWatcher
 *                                              │
 *                                              ▼
 *                                       PjsipProvisioner
 *                                              │
 *                                              ▼ ARI dynamic-config
 *                                          Asterisk sorcery
 *
 * Steady state: a reactor timer ticks `tick()` every ~200 ms; each
 * tick polls the change stream cursor for one event and dispatches it
 * to `PjsipProvisioner` (insert/update with status=="active" → provision;
 * status!="active" or delete → deprovision).
 *
 * Bootstrap (`bootstrap()`): runs once before the change stream is
 * read, doing a society-scoped full-scan of `subscribers` and
 * provisioning every active row. Idempotent — `PjsipProvisioner` PUTs
 * are create-or-replace.
 *
 * Reactor-thread single-threaded; no internal locking. Best-effort —
 * Mongo errors / change-stream drops are logged-and-swallowed.
 *
 * **Resume-after recovery.** The watcher captures the `_id` resume
 * token of every successfully-applied event. When `try_next()` throws
 * (Mongo disconnect, mid-stream error), the cursor is torn down and
 * `tick()` reopens it via `watch_collection(coll, last_token)` — the
 * server then replays every event strictly after that token, so no
 * mutations are dropped as long as the oplog still holds them
 * (typical oplog window is hours to days, well beyond any reconnect).
 * Reopen attempts are rate-limited via a tick counter so a sustained
 * Mongo outage doesn't hammer the pool. If the token has aged out of
 * the oplog (extended outage), the server rejects the resume and the
 * watcher falls back to a fresh-from-now cursor; missed events get
 * re-applied on the next bootstrap call.
 *
 * **Delete-event tracking.** A change-stream `delete` event carries
 * only `documentKey._id` — no fullDocument. We can't extract the
 * sipUsername to deprovision. Instead, the watcher caches
 * `_id → sipUsername` from every bootstrap row and every
 * insert/update event with a fullDocument; on delete, the cache yields
 * the sipUsername. (An alternative — Mongo's
 * `changeStreamPreAndPostImages` — would push the operational config
 * to the collection-mod surface; the in-process cache keeps that
 * surface clean.)
 */
class SubscriberWatcher {
public:
  /// @param db          Mongo client. Must be a real `MongodbClient`
  ///                    talking to a replica-set mongod for the change
  ///                    stream to open; otherwise `bootstrap()` still
  ///                    runs and `tick()` is a silent no-op.
  /// @param society_id  Watch only rows with this `societyId`. Other
  ///                    societies' events are ignored.
  /// @param provisioner Where each materialised change goes.
  SubscriberWatcher(IMongodbClient &db, std::string society_id,
                    PjsipProvisioner &provisioner);

  /// Initial sync: full-scan `subscribers` for the society and
  /// provision every active row. Open the change stream afterwards
  /// (caller is then expected to drive `tick()` from a reactor timer).
  /// Safe to call again — re-bootstrap re-PUTs every active row, which
  /// sorcery absorbs as no-ops.
  void bootstrap();

  /// Re-run the full-scan + provision step WITHOUT touching the change
  /// stream cursor. Production driver: the agent's `SOCIETY_BOOTSTRAP`
  /// handler invokes this after `PjsipProvisioner::set_sip_realm()` so
  /// every already-provisioned subscriber's auth object gets re-PUT
  /// with the canonical realm — SIP REGISTER digests start matching
  /// what the cloud computed. Idempotent on the sorcery side.
  void resync();

  /// Drive one polling tick. Pulls one change event off the cursor (if
  /// any) and dispatches it. Production: ACE timer @ ~200 ms; tests
  /// invoke directly with a recorder cursor. No-op when the cursor
  /// failed to open (mongod isn't a replica set).
  void tick();

  /// Test/observability: how many subscribers we currently have cached.
  std::size_t cached_count() const { return m_id_to_sipuser.size(); }
  /// Test/observability: last applied event's `_id` (resume token) as
  /// raw JSON. Empty until the first event lands.
  const std::string &resume_token() const { return m_resume_token; }

private:
  /// Apply one change-stream event JSON: dispatch to provision or
  /// deprovision and maintain the `_id → sipUsername` cache.
  /// Public-via-friend would be cleaner; for now, expose for tests via
  /// a thin wrapper.
  void handle_event(const std::string &event_json);
  /// Test seam: drive an event JSON directly without a real cursor.
  friend struct SubscriberWatcherTestAccess;

  /// The full-scan-and-provision body shared between `bootstrap()` and
  /// `resync()`. Does NOT touch the change-stream cursor.
  void run_full_scan();

  /// Try to (re)open the change stream. With `m_resume_token` non-empty
  /// the server replays everything after the last applied event; empty
  /// opens a fresh-from-now cursor. Logs and returns on failure;
  /// the next eligible tick retries.
  void open_stream();

  IMongodbClient                      &m_db;
  std::string                          m_society_id;
  PjsipProvisioner                    &m_provisioner;
  std::unique_ptr<IChangeStreamCursor> m_stream;
  std::unordered_map<std::string, std::string> m_id_to_sipuser;
  /// `_id` of the last successfully-applied event, as bare JSON.
  /// Round-tripped back through `watch_collection(coll, token)` on
  /// reopen so the server resumes from immediately after.
  std::string                          m_resume_token;
  /// Reopen-attempt cooldown. After a failed reopen we wait this many
  /// `tick()` calls before retrying — at the 200 ms production cadence
  /// that's ~1 second of breathing room, enough for typical Mongo
  /// flaps without spamming the pool.
  int                                  m_reopen_skip_ticks = 0;
};

/// Test-only accessor — feeds a JSON change event straight into
/// `handle_event()` without standing up a real change-stream cursor.
/// Declared here (not just `friend`-ed) so that the test TU and the
/// production TU see one definition (avoids ODR drift if it were
/// re-declared inline in each test file).
struct SubscriberWatcherTestAccess {
  static void deliver(SubscriberWatcher &w, const std::string &event_json);
};

#endif // SUBSCRIBER_WATCHER_HPP
