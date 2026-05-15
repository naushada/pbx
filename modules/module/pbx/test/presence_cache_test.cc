#include "presence_cache.hpp"

#include <gtest/gtest.h>

TEST(InMemoryPresenceCache, UnseenSubscriberDefaultsToOffline)
{
    InMemoryPresenceCache cache;
    EXPECT_FALSE(cache.is_online("soc1", "u_never_seen"));
    EXPECT_EQ(0u, cache.size());
}

TEST(InMemoryPresenceCache, SetAndIsOnlineRoundTrip)
{
    InMemoryPresenceCache cache;
    cache.set("soc1", "u_alice", true);
    cache.set("soc1", "u_bob",   false);

    EXPECT_TRUE (cache.is_online("soc1", "u_alice"));
    EXPECT_FALSE(cache.is_online("soc1", "u_bob"));
    EXPECT_EQ(2u, cache.size());
}

TEST(InMemoryPresenceCache, SetIsIdempotent)
{
    InMemoryPresenceCache cache;
    cache.set("soc1", "u_alice", true);
    cache.set("soc1", "u_alice", true);   // same state
    cache.set("soc1", "u_alice", false);  // flip
    cache.set("soc1", "u_alice", false);  // again

    EXPECT_FALSE(cache.is_online("soc1", "u_alice"));
    EXPECT_EQ(1u, cache.size());
}

TEST(InMemoryPresenceCache, IsScopedBySocietyId)
{
    // sipUsername is unique-per-society, not globally — two societies
    // could (astronomically improbably) both mint `u_collide`. The cache
    // must keep them separate.
    InMemoryPresenceCache cache;
    cache.set("socA", "u_collide", true);
    cache.set("socB", "u_collide", false);

    EXPECT_TRUE (cache.is_online("socA", "u_collide"));
    EXPECT_FALSE(cache.is_online("socB", "u_collide"));
    // And a third, never-seen society stays offline.
    EXPECT_FALSE(cache.is_online("socC", "u_collide"));
}
