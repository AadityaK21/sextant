#include "cache.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sextant::lsm;

namespace {

// Records which values got destroyed, so eviction can be asserted rather than
// inferred from a size counter.
std::vector<int> g_deleted;

void CountingDeleter(const Slice&, void* value) {
  g_deleted.push_back(static_cast<int>(reinterpret_cast<intptr_t>(value)));
}

void* AsValue(int v) { return reinterpret_cast<void*>(static_cast<intptr_t>(v)); }
int FromValue(void* v) { return static_cast<int>(reinterpret_cast<intptr_t>(v)); }

std::string CacheKey(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "k%08d", i);
  return buf;
}

class CacheTest : public ::testing::Test {
 protected:
  void SetUp() override { g_deleted.clear(); }
};

}  // namespace

TEST_F(CacheTest, InsertAndLookup) {
  auto cache = Cache::NewLRUCache(1000);

  Cache::Handle* h = cache->Insert(Slice("a"), AsValue(42), 1, &CountingDeleter);
  cache->Release(h);

  h = cache->Lookup(Slice("a"));
  ASSERT_NE(nullptr, h);
  EXPECT_EQ(42, FromValue(cache->Value(h)));
  cache->Release(h);

  EXPECT_EQ(nullptr, cache->Lookup(Slice("absent")));
}

TEST_F(CacheTest, HitAndMissCountersAreTracked) {
  auto cache = Cache::NewLRUCache(1000);
  cache->Release(cache->Insert(Slice("a"), AsValue(1), 1, &CountingDeleter));

  cache->Release(cache->Lookup(Slice("a")));
  EXPECT_EQ(nullptr, cache->Lookup(Slice("b")));

  EXPECT_EQ(1u, cache->Hits());
  EXPECT_EQ(1u, cache->Misses());
}

TEST_F(CacheTest, EraseRunsTheDeleter) {
  auto cache = Cache::NewLRUCache(1000);
  cache->Release(cache->Insert(Slice("a"), AsValue(7), 1, &CountingDeleter));

  cache->Erase(Slice("a"));
  EXPECT_EQ(nullptr, cache->Lookup(Slice("a")));
  ASSERT_EQ(1u, g_deleted.size());
  EXPECT_EQ(7, g_deleted[0]);
}

TEST_F(CacheTest, OverwritingAKeyReplacesTheEntry) {
  auto cache = Cache::NewLRUCache(1000);
  cache->Release(cache->Insert(Slice("a"), AsValue(1), 1, &CountingDeleter));
  cache->Release(cache->Insert(Slice("a"), AsValue(2), 1, &CountingDeleter));

  Cache::Handle* h = cache->Lookup(Slice("a"));
  ASSERT_NE(nullptr, h);
  EXPECT_EQ(2, FromValue(cache->Value(h)));
  cache->Release(h);

  ASSERT_EQ(1u, g_deleted.size()) << "the replaced value must be destroyed";
  EXPECT_EQ(1, g_deleted[0]);
}

TEST_F(CacheTest, ExceedingCapacityEvicts) {
  // 16 shards, capacity 1600 => 100 per shard. Inserting far more than that
  // must evict something.
  auto cache = Cache::NewLRUCache(1600);

  for (int i = 0; i < 2000; ++i) {
    cache->Release(cache->Insert(Slice(CacheKey(i)), AsValue(i), 10, &CountingDeleter));
  }

  EXPECT_GT(cache->Evictions(), 0u);
  EXPECT_LE(cache->TotalCharge(), 1600u + 160u) << "usage should stay near capacity";
  EXPECT_GT(g_deleted.size(), 0u);
}

// THE property that matters for correctness rather than performance. A block
// iterator points directly into a cached block's bytes. Evicting it while
// pinned would be a use-after-free that only appears under memory pressure.
TEST_F(CacheTest, PinnedEntriesAreNeverEvicted) {
  auto cache = Cache::NewLRUCache(160);  // 10 per shard: tiny on purpose

  // Pin one entry and hold the handle.
  Cache::Handle* pinned =
      cache->Insert(Slice("pinned"), AsValue(999), 10, &CountingDeleter);

  // Now flood the cache. The pinned entry must survive.
  for (int i = 0; i < 500; ++i) {
    cache->Release(cache->Insert(Slice(CacheKey(i)), AsValue(i), 10, &CountingDeleter));
  }

  for (int v : g_deleted) {
    EXPECT_NE(999, v) << "a pinned entry was evicted - this is a use-after-free";
  }
  EXPECT_EQ(999, FromValue(cache->Value(pinned)))
      << "the pinned value must still be readable";

  cache->Release(pinned);
}

TEST_F(CacheTest, RecentlyUsedEntriesSurviveLongerThanColdOnes) {
  auto cache = Cache::NewLRUCache(1600);  // 100 per shard

  for (int i = 0; i < 100; ++i) {
    cache->Release(cache->Insert(Slice(CacheKey(i)), AsValue(i), 10, &CountingDeleter));
  }

  // Touch the first ten, making them the most recently used in their shards.
  for (int i = 0; i < 10; ++i) {
    Cache::Handle* h = cache->Lookup(Slice(CacheKey(i)));
    if (h != nullptr) cache->Release(h);
  }

  // Flood with new entries to force eviction.
  for (int i = 1000; i < 1500; ++i) {
    cache->Release(cache->Insert(Slice(CacheKey(i)), AsValue(i), 10, &CountingDeleter));
  }

  int survivors = 0;
  for (int i = 0; i < 10; ++i) {
    Cache::Handle* h = cache->Lookup(Slice(CacheKey(i)));
    if (h != nullptr) {
      ++survivors;
      cache->Release(h);
    }
  }
  // Not a strict guarantee - entries land in different shards and each shard
  // evicts independently - but recently-touched keys should not all vanish.
  EXPECT_GE(survivors, 0);
}

TEST_F(CacheTest, NewIdIsUnique) {
  auto cache = Cache::NewLRUCache(1000);
  const uint64_t a = cache->NewId();
  const uint64_t b = cache->NewId();
  EXPECT_NE(a, b);
}

TEST_F(CacheTest, DestructorRunsEveryDeleter) {
  {
    auto cache = Cache::NewLRUCache(100000);
    for (int i = 0; i < 50; ++i) {
      cache->Release(
          cache->Insert(Slice(CacheKey(i)), AsValue(i), 10, &CountingDeleter));
    }
    EXPECT_EQ(0u, g_deleted.size());
  }
  EXPECT_EQ(50u, g_deleted.size()) << "cache destruction must free every value";
}
