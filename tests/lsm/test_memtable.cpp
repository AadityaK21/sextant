#include "memtable.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace sextant::lsm;

namespace {

class MemTableTest : public ::testing::Test {
 protected:
  InternalKeyComparator cmp_;
  // Day 4 made MemTable refcounted with a private destructor, so it can no
  // longer be a by-value member. Ref on construction, Unref on teardown.
  MemTable* mem_ = nullptr;

  void SetUp() override {
    mem_ = new MemTable(cmp_);
    mem_->Ref();
  }
  void TearDown() override { mem_->Unref(); }

  void Add(SequenceNumber s, ValueType t, const std::string& k, const std::string& v) {
    mem_->Add(s, t, Slice(k), Slice(v));
  }

  // Returns "value", "NOT_FOUND" (tombstone) or "ABSENT" (no entry at all).
  std::string Get(const std::string& key, SequenceNumber snapshot) {
    const LookupKey lk(Slice(key), snapshot);
    std::string value;
    Status s;
    if (!mem_->Get(lk, &value, &s)) return "ABSENT";
    if (s.IsNotFound()) return "NOT_FOUND";
    return value;
  }
};

}  // namespace

TEST_F(MemTableTest, PutAndGet) {
  Add(1, kTypeValue, "NLRTM", "Rotterdam");
  Add(2, kTypeValue, "FIHEL", "Helsinki");

  EXPECT_EQ("Rotterdam", Get("NLRTM", 10));
  EXPECT_EQ("Helsinki", Get("FIHEL", 10));
  EXPECT_EQ("ABSENT", Get("DEHAM", 10));
  EXPECT_EQ(2u, mem_->NumEntries());
}

TEST_F(MemTableTest, EmptyValueIsDistinctFromAbsent) {
  Add(1, kTypeValue, "k", "");
  EXPECT_EQ("", Get("k", 10));
  EXPECT_EQ("ABSENT", Get("missing", 10));
}

TEST_F(MemTableTest, NewestVersionWins) {
  Add(1, kTypeValue, "port", "old");
  Add(2, kTypeValue, "port", "middle");
  Add(3, kTypeValue, "port", "new");

  EXPECT_EQ("new", Get("port", 100));
}

// This is the snapshot semantics that graph traversal depends on: a read at
// sequence N must see the state as of N, not the latest state.
TEST_F(MemTableTest, SnapshotReadsSeeHistoricalVersions) {
  Add(10, kTypeValue, "port", "v10");
  Add(20, kTypeValue, "port", "v20");
  Add(30, kTypeValue, "port", "v30");

  EXPECT_EQ("v30", Get("port", 30));
  EXPECT_EQ("v20", Get("port", 29));
  EXPECT_EQ("v20", Get("port", 20));
  EXPECT_EQ("v10", Get("port", 19));
  EXPECT_EQ("ABSENT", Get("port", 9)) << "before the first write, nothing exists";
}

// A tombstone must report "handled, not found" rather than "keep looking".
// Once SSTables exist, returning ABSENT here would let the read fall through
// to an older level and RESURRECT the deleted value.
TEST_F(MemTableTest, TombstoneShadowsOlderValue) {
  Add(1, kTypeValue, "vessel", "ARUNA CIHAN");
  Add(2, kTypeDeletion, "vessel", "");

  EXPECT_EQ("NOT_FOUND", Get("vessel", 100));
  EXPECT_EQ("ARUNA CIHAN", Get("vessel", 1)) << "the pre-delete snapshot still sees it";
}

TEST_F(MemTableTest, WriteAfterDeleteRestoresTheKey) {
  Add(1, kTypeValue, "k", "first");
  Add(2, kTypeDeletion, "k", "");
  Add(3, kTypeValue, "k", "second");

  EXPECT_EQ("second", Get("k", 100));
  EXPECT_EQ("NOT_FOUND", Get("k", 2));
  EXPECT_EQ("first", Get("k", 1));
}

TEST_F(MemTableTest, HandlesBinaryKeysAndValues) {
  const std::string key("\x00\x01\xff key", 8);
  const std::string value("\x00binary\xff", 8);
  Add(1, kTypeValue, key, value);
  EXPECT_EQ(value, Get(key, 10));
}

TEST_F(MemTableTest, IterationIsOrderedByInternalKey) {
  Add(1, kTypeValue, "b", "1");
  Add(2, kTypeValue, "a", "2");
  Add(3, kTypeValue, "c", "3");
  Add(4, kTypeValue, "a", "4");  // newer version of "a"

  std::vector<std::pair<std::string, SequenceNumber>> got;
  MemTable::Iterator it(mem_);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    ParsedInternalKey p;
    ASSERT_TRUE(ParseInternalKey(it.key(), &p));
    got.emplace_back(p.user_key.ToString(), p.sequence);
  }

  // user key ascending, sequence descending within a user key
  const std::vector<std::pair<std::string, SequenceNumber>> expected = {
      {"a", 4}, {"a", 2}, {"b", 1}, {"c", 3}};
  EXPECT_EQ(expected, got);
}

TEST_F(MemTableTest, IteratorSeek) {
  for (const auto& k : {"aa", "bb", "cc", "dd"}) {
    Add(1, kTypeValue, k, std::string("v_") + k);
  }

  std::string target;
  AppendInternalKey(&target, ParsedInternalKey(Slice("bb"), kMaxSequenceNumber,
                                               kValueTypeForSeek));

  MemTable::Iterator it(mem_);
  it.Seek(Slice(target));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ("bb", ExtractUserKey(it.key()).ToString());
  EXPECT_EQ("v_bb", it.value().ToString());
}

TEST_F(MemTableTest, MemoryUsageGrowsWithData) {
  const size_t before = mem_->ApproximateMemoryUsage();
  for (int i = 0; i < 2000; ++i) {
    Add(static_cast<SequenceNumber>(i + 1), kTypeValue,
        "key" + std::to_string(i), std::string(100, 'v'));
  }
  EXPECT_GT(mem_->ApproximateMemoryUsage(), before + 2000u * 100u);
}
