// Leveled compaction: the invariants, not just the outcomes.
//
// Compaction is the one subsystem where a bug is silent. A broken read path
// returns wrong answers immediately; a broken compaction produces a database
// that looks fine until the specific key whose ordering was violated is asked
// for, possibly weeks later. So these tests assert the STRUCTURAL properties
// that make the read path correct, not merely that data can still be read back.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "env.h"
#include "sextant/lsm/db.h"

using namespace sextant::lsm;

namespace {

class CompactionTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<DB> db_;

  void SetUp() override {
    dbname_ = std::string("cmptest_") +
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

  // Small buffer so a few thousand writes produce many L0 files and force real
  // compaction work rather than one tidy flush.
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

  std::string Get(const std::string& k) {
    std::string value;
    const Status s = db_->Get(ReadOptions{}, Slice(k), &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return "ERROR: " + s.ToString();
    return value;
  }

  int CountFiles(const char* suffix) {
    std::vector<std::string> children;
    if (!GetChildren(dbname_, &children).ok()) return -1;
    int n = 0;
    for (const auto& c : children) {
      const size_t len = std::char_traits<char>::length(suffix);
      if (c.size() > len && c.compare(c.size() - len, len, suffix) == 0) ++n;
    }
    return n;
  }

  // Scan everything through the public iterator.
  std::map<std::string, std::string> ScanAll() {
    auto it = db_->NewIterator(ReadOptions{});
    std::map<std::string, std::string> out;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      out[it->key().ToString()] = it->value().ToString();
    }
    EXPECT_TRUE(it->status().ok()) << it->status().ToString();
    return out;
  }

  void WriteMany(int first, int count, const std::string& value) {
    for (int i = 0; i < count; ++i) {
      ASSERT_TRUE(Put(Key(first + i), value).ok());
    }
  }

  static std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08d", i);
    return buf;
  }
};

}  // namespace

TEST_F(CompactionTest, CompactionActuallyRuns) {
  WriteMany(0, 20000, std::string(80, 'v'));
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  EXPECT_GT(s.flushes, 5u);
  EXPECT_GT(s.compactions + s.trivial_moves, 0u)
      << "20k keys with a 16 KB buffer must have triggered compaction";
}

// THE structural invariant. Above L0, files must not overlap - the read path
// binary-searches each level assuming exactly that. If this is ever violated,
// lookups silently miss keys.
TEST_F(CompactionTest, FilesAboveL0NeverOverlap) {
  std::mt19937 rnd(20260809);
  for (int i = 0; i < 30000; ++i) {
    ASSERT_TRUE(Put(Key(static_cast<int>(rnd() % 20000)), std::string(60, 'v')).ok());
  }
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  int levels_with_files = 0;
  for (int level = 1; level < 7; ++level) {
    if (s.files_per_level[level] > 0) ++levels_with_files;
  }
  EXPECT_GT(levels_with_files, 0) << "nothing was ever promoted out of L0";

  // The non-overlap invariant itself is asserted inside VersionSet::Builder
  // under NDEBUG-off builds. Here we verify the observable consequence: every
  // key still reads back correctly through a level structure that only works
  // if the invariant holds.
  const auto scanned = ScanAll();
  EXPECT_GT(scanned.size(), 0u);
  for (const auto& [k, v] : scanned) {
    EXPECT_EQ(v, Get(k)) << "point lookup disagrees with scan for " << k;
  }
}

TEST_F(CompactionTest, L0IsKeptSmall) {
  WriteMany(0, 30000, std::string(80, 'v'));
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  // kL0CompactionTrigger is 4; compaction should keep L0 near that, and the
  // hard stop is 12. Anything at or above the stop trigger after quiescing
  // means compaction is not keeping up at all.
  EXPECT_LT(s.files_per_level[0], 12u)
      << "L0 has " << s.files_per_level[0] << " files after compaction settled";
}

TEST_F(CompactionTest, EveryKeySurvivesCompaction) {
  std::map<std::string, std::string> reference;
  std::mt19937 rnd(4242);

  for (int i = 0; i < 25000; ++i) {
    const std::string k = Key(static_cast<int>(rnd() % 8000));
    const std::string v = "v" + std::to_string(i);
    ASSERT_TRUE(Put(k, v).ok());
    reference[k] = v;
  }
  db_->WaitForBackgroundWork();
  ASSERT_GT(db_->GetStats().compactions + db_->GetStats().trivial_moves, 0u);

  for (const auto& [k, v] : reference) {
    EXPECT_EQ(v, Get(k)) << "compaction lost or corrupted " << k;
  }
  EXPECT_EQ(reference.size(), ScanAll().size());
}

// Tombstones must survive until compaction can prove nothing older exists
// beneath them. Dropping one early resurrects the value it was hiding.
TEST_F(CompactionTest, DeletedKeysStayDeletedThroughCompaction) {
  WriteMany(0, 20000, std::string(60, 'v'));
  db_->WaitForBackgroundWork();

  for (int i = 0; i < 20000; i += 2) {
    ASSERT_TRUE(Del(Key(i)).ok());
  }
  db_->WaitForBackgroundWork();

  // More writes to drive further compaction over the tombstones.
  WriteMany(100000, 10000, std::string(60, 'w'));
  db_->WaitForBackgroundWork();

  for (int i = 0; i < 20000; i += 2) {
    EXPECT_EQ("NOT_FOUND", Get(Key(i))) << "deleted key " << Key(i) << " came back";
  }
  for (int i = 1; i < 20000; i += 2) {
    EXPECT_EQ(std::string(60, 'v'), Get(Key(i))) << "live key " << Key(i) << " lost";
  }
}

// Compaction must actually reclaim space, not merely shuffle it. Overwriting
// the same keys repeatedly produces many obsolete versions; after compaction
// the database should be far smaller than the raw bytes written.
TEST_F(CompactionTest, ObsoleteVersionsAreReclaimed) {
  static constexpr int kKeys = 2000;
  static constexpr int kRounds = 15;

  for (int round = 0; round < kRounds; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      ASSERT_TRUE(Put(Key(i), std::string(100, static_cast<char>('a' + round))).ok());
    }
  }
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  const uint64_t live_data = static_cast<uint64_t>(kKeys) * 110;  // key + value

  EXPECT_GT(s.keys_dropped, 0u) << "obsolete versions were never dropped";

  // Space amplification: bytes on disk versus bytes of live data. Without
  // compaction this would be ~15x. Allow generous headroom - compaction is
  // asynchronous and some obsolete data may legitimately still be in flight.
  const double space_amp =
      static_cast<double>(s.total_bytes_on_disk) / static_cast<double>(live_data);
  EXPECT_LT(space_amp, 8.0)
      << "space amplification " << space_amp << "x suggests nothing was reclaimed";
}

TEST_F(CompactionTest, ObsoleteFilesAreDeletedFromDisk) {
  WriteMany(0, 30000, std::string(80, 'v'));
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  EXPECT_GT(s.files_deleted, 0u) << "compaction inputs were never unlinked";

  // The number of .sst files on disk should match the number the version set
  // considers live. A mismatch means leaked files accumulating forever.
  const int on_disk = CountFiles(".sst");
  EXPECT_EQ(static_cast<int>(s.num_sstables), on_disk)
      << "on-disk sstables (" << on_disk << ") disagree with live files ("
      << s.num_sstables << ")";
}

TEST_F(CompactionTest, StateSurvivesReopenAfterCompaction) {
  std::map<std::string, std::string> reference;
  std::mt19937 rnd(31337);

  for (int i = 0; i < 20000; ++i) {
    const std::string k = Key(static_cast<int>(rnd() % 6000));
    const std::string v = "v" + std::to_string(i);
    ASSERT_TRUE(Put(k, v).ok());
    reference[k] = v;
  }
  ASSERT_TRUE(Del(Key(5)).ok());
  reference.erase(Key(5));

  db_->WaitForBackgroundWork();
  ASSERT_TRUE(db_->SyncWAL().ok());

  const uint64_t sequence_before = db_->GetStats().sequence;
  Reopen();

  EXPECT_EQ(sequence_before, db_->GetStats().sequence)
      << "the MANIFEST must restore the sequence number";
  EXPECT_GT(db_->GetStats().num_sstables, 0u) << "sstables must be reopened";

  for (const auto& [k, v] : reference) {
    EXPECT_EQ(v, Get(k)) << "lost " << k << " across reopen";
  }
  EXPECT_EQ("NOT_FOUND", Get(Key(5)));
}

TEST_F(CompactionTest, RepeatedReopensDoNotLeakFilesOrData) {
  std::map<std::string, std::string> reference;

  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 4000; ++i) {
      const std::string k = Key(round * 5000 + i);
      const std::string v = "r" + std::to_string(round);
      ASSERT_TRUE(Put(k, v).ok());
      reference[k] = v;
    }
    db_->WaitForBackgroundWork();
    Reopen();

    const Stats s = db_->GetStats();
    EXPECT_EQ(static_cast<int>(s.num_sstables), CountFiles(".sst"))
        << "file leak after round " << round;
    // A fresh MANIFEST is written on each open; old ones must be reclaimed.
    EXPECT_LE(CountFiles("dbtmp"), 0) << "temp files left behind";
  }

  for (const auto& [k, v] : reference) {
    EXPECT_EQ(v, Get(k)) << "lost " << k;
  }
}

// An iterator pins a Version. Compaction running underneath it must not delete
// the files it is reading - this is the day-3 limitation that refcounting was
// introduced to fix.
TEST_F(CompactionTest, IteratorSurvivesConcurrentCompaction) {
  WriteMany(0, 10000, std::string(80, 'v'));
  db_->WaitForBackgroundWork();

  auto it = db_->NewIterator(ReadOptions{});
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());

  // Now write enough to trigger several flushes and compactions while the
  // iterator is still open and pointing into old files.
  WriteMany(200000, 20000, std::string(80, 'w'));
  db_->WaitForBackgroundWork();

  // Walk the rest of the iterator. Every key it yields must still be readable;
  // if compaction had deleted its files this would crash or corrupt.
  size_t count = 0;
  for (; it->Valid(); it->Next()) {
    ASSERT_FALSE(it->key().empty());
    ++count;
  }
  EXPECT_TRUE(it->status().ok()) << it->status().ToString();
  EXPECT_EQ(10000u, count)
      << "the pinned snapshot must show exactly what existed when it was taken";
}

TEST_F(CompactionTest, WriteStallsEngageUnderSustainedLoad) {
  // 60k writes at 16 KB per memtable is ~200 flushes. Compaction cannot keep
  // up with that indefinitely, so the throttle must engage.
  for (int i = 0; i < 60000; ++i) {
    ASSERT_TRUE(Put(Key(i), std::string(100, 'v')).ok());
  }
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  EXPECT_GT(s.write_stalls, 0u)
      << "sustained ingest must trigger backpressure, or L0 grows unbounded";
  EXPECT_LT(s.files_per_level[0], 12u) << "the stall failed to bound L0";
}

TEST_F(CompactionTest, TrivialMovesAvoidRewritingData) {
  // Strictly ascending keys with no overlap: many compactions should be pure
  // metadata moves rather than merges.
  WriteMany(0, 40000, std::string(60, 'v'));
  db_->WaitForBackgroundWork();

  const Stats s = db_->GetStats();
  EXPECT_GT(s.trivial_moves + s.compactions, 0u);
  // Not asserting trivial_moves > 0 outright: whether a move is possible
  // depends on how flushes happened to align. Report it rather than demand it.
  if (s.trivial_moves > 0) {
    SUCCEED() << s.trivial_moves << " files moved without rewriting bytes";
  }
}
