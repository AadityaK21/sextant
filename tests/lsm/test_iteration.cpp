// Full-database iteration: merging iterator + DBIter.
//
// This is the primitive everything above the storage engine is built on. A
// LINKOUT prefix scan and a TIDX time-range scan are both just Seek followed
// by Next, so if this is wrong the entire query layer is wrong.

#include <gtest/gtest.h>

#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "env.h"
#include "sextant/lsm/db.h"

using namespace sextant::lsm;

namespace {

class IterationTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<DB> db_;

  void SetUp() override {
    dbname_ = std::string("itertest_") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name();
    Destroy();
    Reopen();
  }
  void TearDown() override {
    db_.reset();
    Destroy();
  }

  void Destroy() {
    std::vector<std::string> children;
    if (GetChildren(dbname_, &children).ok()) {
      for (const auto& c : children) {
        if (c == "." || c == "..") continue;
        RemoveFile(dbname_ + "/" + c);
      }
    }
    std::remove(dbname_.c_str());
  }

  // Small buffer so data is genuinely spread across memtable and several
  // sstables - otherwise this would only exercise the memtable iterator.
  static Options TestOptions() {
    Options opts;
    opts.write_buffer_size = 16 * 1024;
    return opts;
  }

  void Reopen() {
    db_.reset();
    ASSERT_TRUE(DB::Open(TestOptions(), dbname_, &db_).ok());
  }

  Status Put(const std::string& k, const std::string& v) {
    return db_->Put(WriteOptions{}, Slice(k), Slice(v));
  }
  Status Del(const std::string& k) { return db_->Delete(WriteOptions{}, Slice(k)); }

  // Collect the whole database as an ordered vector.
  std::vector<std::pair<std::string, std::string>> ScanAll(
      const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    auto it = db_->NewIterator(ro);
    std::vector<std::pair<std::string, std::string>> out;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      out.emplace_back(it->key().ToString(), it->value().ToString());
    }
    EXPECT_TRUE(it->status().ok()) << it->status().ToString();
    return out;
  }
};

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

}  // namespace

TEST_F(IterationTest, EmptyDatabase) {
  const auto all = ScanAll();
  EXPECT_TRUE(all.empty());
}

TEST_F(IterationTest, MemtableOnly) {
  ASSERT_TRUE(Put("b", "2").ok());
  ASSERT_TRUE(Put("a", "1").ok());
  ASSERT_TRUE(Put("c", "3").ok());

  const std::vector<std::pair<std::string, std::string>> expected = {
      {"a", "1"}, {"b", "2"}, {"c", "3"}};
  EXPECT_EQ(expected, ScanAll()) << "iteration must be sorted, not insertion-ordered";
}

TEST_F(IterationTest, ScanMatchesStdMapAcrossManySSTables) {
  std::map<std::string, std::string> reference;
  std::mt19937 rnd(12345);

  for (int i = 0; i < 8000; ++i) {
    const std::string k = Key(static_cast<int>(rnd() % 4000));
    const std::string v = "v" + std::to_string(i);
    ASSERT_TRUE(Put(k, v).ok());
    reference[k] = v;
  }
  ASSERT_GT(db_->GetStats().flushes, 3u) << "test must span several sstables";

  const auto scanned = ScanAll();
  const std::vector<std::pair<std::string, std::string>> expected(reference.begin(),
                                                                  reference.end());
  ASSERT_EQ(expected.size(), scanned.size());
  EXPECT_EQ(expected, scanned);
}

// The merge must present only the NEWEST version of each key, even when older
// versions sit in different sstables.
TEST_F(IterationTest, OnlyNewestVersionIsVisible) {
  ASSERT_TRUE(Put("port", "v1").ok());
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(Put("filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_TRUE(Put("port", "v2").ok());
  for (int i = 500; i < 1000; ++i) {
    ASSERT_TRUE(Put("filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_TRUE(Put("port", "v3").ok());

  int seen = 0;
  std::string value;
  for (const auto& [k, v] : ScanAll()) {
    if (k == "port") {
      ++seen;
      value = v;
    }
  }
  EXPECT_EQ(1, seen) << "each user key must appear exactly once";
  EXPECT_EQ("v3", value);
}

// Tombstones must hide older values during a scan, including when the value
// lives in an sstable and the tombstone in the memtable.
TEST_F(IterationTest, DeletedKeysDoNotAppear) {
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(Put(Key(i), "v").ok());
  }
  for (int i = 0; i < 2000; i += 3) {
    ASSERT_TRUE(Del(Key(i)).ok());
  }

  const auto all = ScanAll();
  for (const auto& [k, v] : all) {
    for (int i = 0; i < 2000; i += 3) {
      EXPECT_NE(Key(i), k) << "deleted key " << k << " came back in a scan";
    }
  }
  // 2000 keys, every third deleted -> 667 gone.
  EXPECT_EQ(2000u - 667u, all.size());
}

TEST_F(IterationTest, SeekPositionsAtFirstKeyAtOrAfterTarget) {
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put(Key(i * 2), "v" + std::to_string(i)).ok());  // even keys only
  }

  auto it = db_->NewIterator(ReadOptions{});

  it->Seek(Slice(Key(500)));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(Key(500), it->key().ToString());

  it->Seek(Slice(Key(501)));  // absent
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(Key(502), it->key().ToString()) << "seek must round up, not down";

  it->Seek(Slice("aaa"));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(Key(0), it->key().ToString());

  it->Seek(Slice("zzz"));
  EXPECT_FALSE(it->Valid()) << "past the end must be invalid, not wrapped";
}

// A prefix scan is exactly how the ontology layer will traverse links, so it
// gets its own test even though it is just Seek plus Next.
TEST_F(IterationTest, PrefixScanReturnsOnlyTheMatchingRange) {
  ASSERT_TRUE(Put("LINKOUT|aaa|arrives|001", "x").ok());
  ASSERT_TRUE(Put("LINKOUT|aaa|arrives|002", "x").ok());
  ASSERT_TRUE(Put("LINKOUT|aaa|arrives|003", "x").ok());
  ASSERT_TRUE(Put("LINKOUT|aaa|departs|001", "x").ok());
  ASSERT_TRUE(Put("LINKOUT|bbb|arrives|001", "x").ok());
  ASSERT_TRUE(Put("ENTITY|aaa", "x").ok());

  const std::string prefix = "LINKOUT|aaa|arrives|";
  auto it = db_->NewIterator(ReadOptions{});

  std::vector<std::string> found;
  for (it->Seek(Slice(prefix)); it->Valid(); it->Next()) {
    if (!it->key().starts_with(Slice(prefix))) break;
    found.push_back(it->key().ToString());
  }

  ASSERT_EQ(3u, found.size());
  EXPECT_EQ(prefix + "001", found[0]);
  EXPECT_EQ(prefix + "002", found[1]);
  EXPECT_EQ(prefix + "003", found[2]);
}

TEST_F(IterationTest, SnapshotScanSeesTheFrozenState) {
  for (int i = 0; i < 500; ++i) ASSERT_TRUE(Put(Key(i), "before").ok());

  const Snapshot* snap = db_->GetSnapshot();

  for (int i = 0; i < 500; ++i) ASSERT_TRUE(Put(Key(i), "after").ok());
  for (int i = 500; i < 1000; ++i) ASSERT_TRUE(Put(Key(i), "new").ok());
  ASSERT_TRUE(Del(Key(3)).ok());

  const auto snapshot_view = ScanAll(snap);
  EXPECT_EQ(500u, snapshot_view.size()) << "snapshot must not see later inserts";
  for (const auto& [k, v] : snapshot_view) {
    EXPECT_EQ("before", v) << "snapshot saw a later value for " << k;
  }

  const auto live_view = ScanAll();
  EXPECT_EQ(999u, live_view.size());

  db_->ReleaseSnapshot(snap);
}

TEST_F(IterationTest, ScanSurvivesReopen) {
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(Put(Key(i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(Del(Key(42)).ok());
  ASSERT_TRUE(db_->SyncWAL().ok());

  const auto before = ScanAll();
  Reopen();
  const auto after = ScanAll();

  EXPECT_EQ(before, after) << "iteration must be stable across recovery";
  EXPECT_EQ(2999u, after.size());
}

TEST_F(IterationTest, ReverseIterationIsExplicitlyUnsupported) {
  ASSERT_TRUE(Put("a", "1").ok());

  auto it = db_->NewIterator(ReadOptions{});
  it->SeekToLast();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsNotSupported())
      << "unsupported operations must say so rather than silently misbehave";
}
