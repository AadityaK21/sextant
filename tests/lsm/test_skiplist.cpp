#include "skiplist.h"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "arena.h"

using namespace sextant::lsm;

namespace {

struct TestComparator {
  int operator()(const uint64_t& a, const uint64_t& b) const {
    if (a < b) return -1;
    if (a > b) return +1;
    return 0;
  }
};

using TestList = SkipList<uint64_t, TestComparator>;

}  // namespace

TEST(SkipList, EmptyList) {
  Arena arena;
  TestList list(TestComparator{}, &arena);

  EXPECT_FALSE(list.Contains(10));

  TestList::Iterator iter(&list);
  EXPECT_FALSE(iter.Valid());
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek(100);
  EXPECT_FALSE(iter.Valid());
  iter.SeekToLast();
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipList, InsertAndLookupMatchesStdSet) {
  static constexpr int kN = 2000;
  static constexpr int kRange = 5000;

  Arena arena;
  TestList list(TestComparator{}, &arena);
  std::set<uint64_t> reference;
  std::mt19937_64 rnd(1000);

  for (int i = 0; i < kN; ++i) {
    const uint64_t key = rnd() % kRange;
    if (reference.insert(key).second) {
      list.Insert(key);
    }
  }

  for (int i = 0; i < kRange; ++i) {
    const bool expected = reference.count(static_cast<uint64_t>(i)) > 0;
    EXPECT_EQ(expected, list.Contains(static_cast<uint64_t>(i))) << "key=" << i;
  }
}

TEST(SkipList, ForwardIterationIsSorted) {
  Arena arena;
  TestList list(TestComparator{}, &arena);
  std::set<uint64_t> reference;
  std::mt19937_64 rnd(7);

  for (int i = 0; i < 1000; ++i) {
    const uint64_t key = rnd() % 10000;
    if (reference.insert(key).second) list.Insert(key);
  }

  std::vector<uint64_t> got;
  TestList::Iterator iter(&list);
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) got.push_back(iter.key());

  const std::vector<uint64_t> expected(reference.begin(), reference.end());
  EXPECT_EQ(expected, got);
}

TEST(SkipList, BackwardIterationIsSorted) {
  Arena arena;
  TestList list(TestComparator{}, &arena);
  std::set<uint64_t> reference;
  for (uint64_t k : {5u, 1u, 9u, 3u, 7u}) {
    reference.insert(k);
    list.Insert(k);
  }

  std::vector<uint64_t> got;
  TestList::Iterator iter(&list);
  for (iter.SeekToLast(); iter.Valid(); iter.Prev()) got.push_back(iter.key());

  std::vector<uint64_t> expected(reference.rbegin(), reference.rend());
  EXPECT_EQ(expected, got);
}

TEST(SkipList, SeekLandsOnFirstKeyGreaterOrEqual) {
  Arena arena;
  TestList list(TestComparator{}, &arena);
  for (uint64_t k : {10u, 20u, 30u, 40u}) list.Insert(k);

  TestList::Iterator iter(&list);

  iter.Seek(0);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(10u, iter.key());

  iter.Seek(20);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(20u, iter.key()) << "an exact match must land on the key itself";

  iter.Seek(25);
  ASSERT_TRUE(iter.Valid());
  EXPECT_EQ(30u, iter.key());

  iter.Seek(41);
  EXPECT_FALSE(iter.Valid()) << "past the end must be invalid, not wrap";
}

// The concurrency contract: one writer, many readers, no locks on the read
// path. A reader must never observe a partially-constructed node. If the
// release/acquire pairing in SkipList::Insert were wrong, this test would
// crash or read garbage under TSan/ASan.
TEST(SkipList, ConcurrentReadersSeeConsistentState) {
  static constexpr uint64_t kN = 20000;

  Arena arena;
  TestList list(TestComparator{}, &arena);
  std::atomic<uint64_t> written{0};
  std::atomic<bool> failed{false};

  std::thread writer([&] {
    for (uint64_t i = 1; i <= kN; ++i) {
      list.Insert(i);
      written.store(i, std::memory_order_release);
    }
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (written.load(std::memory_order_acquire) < kN) {
        const uint64_t high_water = written.load(std::memory_order_acquire);
        // Every key at or below the high-water mark must already be visible.
        if (high_water > 0 && !list.Contains(high_water / 2 + 1)) {
          if (high_water / 2 + 1 <= high_water) failed.store(true);
        }
        // Walking the list must never crash or produce out-of-order keys.
        uint64_t prev = 0;
        TestList::Iterator iter(&list);
        for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
          if (iter.key() <= prev) failed.store(true);
          prev = iter.key();
        }
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  EXPECT_FALSE(failed.load());
  for (uint64_t i = 1; i <= kN; ++i) ASSERT_TRUE(list.Contains(i)) << "missing " << i;
}
