#ifndef PBX_PRESENCE_CACHE_HPP
#define PBX_PRESENCE_CACHE_HPP

#include <string>
#include <unordered_map>

/**
 * @file presence_cache.hpp
 * @brief Cloud-side cache of which subscribers are SIP-registered right now.
 *
 * The agent reports REGISTER state to the cloud over the tunnel — a
 * `REGISTER_STATE` SipFrame per `EndpointStateChange` ARI event from
 * Asterisk (DESIGN.md §7). The cloud keeps the latest state per
 * `(societyId, sipUsername)` and serves it from the directory's
 * `online` field (`handle_directory_GET`).
 *
 * Reactor-thread single-threaded; no internal locking. The whole hot
 * path — REGISTER_STATE arrives → bridge fires the handler →
 * `set()` → eventual directory `is_online()` lookup — runs on the
 * single reactor thread that drives `WebServer`.
 *
 * Semantics: "online" means "we have positive evidence the subscriber
 * is registered." A never-seen-yet subscriber is `false` (the safe
 * default — the directory's call button is disabled when offline).
 *
 * Caveat (future item): the cache may go stale across an agent
 * disconnect, since EndpointStateChange only fires on actual changes.
 * For MVP, accept staleness — the cache refreshes naturally as state
 * changes. A clean-room reconciliation on tunnel reconnect is a
 * separate item.
 */
class IPresenceCache {
public:
  virtual ~IPresenceCache() = default;

  /// Update the registration state for one subscriber. Idempotent —
  /// repeated `(soc, user, true)` calls are a no-op.
  virtual void set(const std::string &society_id,
                   const std::string &sip_username, bool online) = 0;

  /// Look up the latest registration state. Returns `false` for any
  /// subscriber the cache hasn't seen.
  virtual bool is_online(const std::string &society_id,
                         const std::string &sip_username) const = 0;
};

/// Production: a tiny `unordered_map` keyed on `<societyId>\0<sipUsername>`.
/// Sized for v1 — a few hundred subscribers per society, one society per
/// agent — so the flat hash table is fine without per-society sharding.
class InMemoryPresenceCache : public IPresenceCache {
public:
  void set(const std::string &society_id,
           const std::string &sip_username, bool online) override {
    m_state[key(society_id, sip_username)] = online;
  }

  bool is_online(const std::string &society_id,
                 const std::string &sip_username) const override {
    auto it = m_state.find(key(society_id, sip_username));
    return it != m_state.end() && it->second;
  }

  /// Test-only: how many subscribers we've ever heard about.
  std::size_t size() const { return m_state.size(); }

private:
  static std::string key(const std::string &society_id,
                         const std::string &sip_username) {
    // NUL byte makes the join unambiguous even if a future societyId or
    // sipUsername gains separator-looking characters.
    return society_id + std::string(1, '\0') + sip_username;
  }

  std::unordered_map<std::string, bool> m_state;
};

#endif // PBX_PRESENCE_CACHE_HPP
