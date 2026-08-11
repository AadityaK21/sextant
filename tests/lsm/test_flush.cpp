// Flush behaviour: the moment data stops living only in memory.
//
// These tests set write_buffer_size deliberately small so that a few hundred
// writes force several flushes. That is the whole point - the interesting bugs
// appear when the same user key exists in the memtable AND in one or more L0
// tables at different sequence numbers.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "env.h"
#include "filename.h"
#include "sextant/lsm/db.h"

using namespace sextant::lsm;

namespace {

class FlushTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<DB> db_;

  void SetUp() override {
    dbname_ = std::string("flushtest_") +
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

  Options SmallBufferOptions() const {
    Options opts;
    opts.write_buffer_size = 16 * 1024;  // tiny, so flushes happen constantly
    return opts;
  }

  void Reopen() {
    db_.reset();
    ASSERT_TRUE(DB::Open(SmallBufferOptions(), dbname_, &db_).ok());
  }

  Status Put(const std::string& k, const std::string& v) {
    return db_->Put(WriteOptions{}, Slice(k), Slice(v));
  }
  Status Del(const std::string& k) { return db_->Delete(WriteOptions{}, Slice(k)); }

  std::string Get(const std::string& k, const Snapshot* snap = nullptr) {
    ReadOptions ro;
    ro.snapshot = snap;
    std::string value;
    const Status s = db_->Get(ro, Slice(k), &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return "ERROR: " + s.ToString();
    return value;
  }

  int CountFiles(const char* suffix) {
    std::vector<std::string> children;
    if (!GetChildren(dbname_, &children).ok()) return -1;
    int n = 0;
    for (const auto& c : children) {
      if (c.size() > 4 && c.compare(c.size() - 4, 4, suffix) == 0) ++n;
    }
    return n;
  }
};

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

}  // namespace

TEST_F(FlushTest, WritingPastTheBufferProducesSSTables) {
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(Put(Key(i), std::string(100, 'v')).ok());
  }

  const Stats s = db_->GetStats();
  EXPECT_GT(s.flushes, 0u) << "16 KB buffer and 200 KB of data must have flushed";
  EXPECT_GT(s.num_sstables, 0u);
  EXPECT_GT(s.bytes_flushed, 0u);
  EXPECT_GT(CountFiles(".sst"), 0);
}

TEST_F(FlushTest, EveryKeySurvivesFlushing) {
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(Put(Key(i), "value_" + std::to_string(i)).ok());
  }
  for (int i = 0; i < 3000; ++i) {
    EXPECT_EQ("value_" + std::to_string(i), Get(Key(i))) << "lost " << Key(i);
  }
  EXPECT_GT(db_->GetStats().flushes, 0u);
}

// THE test that matters most on day 2. The same user key is written twice with
// a flush in between, so the old version sits in an L0 SSTable and the new one
// in the memtable. Reading the stale value would mean the read path consulted
// disk before memory.
TEST_F(FlushTest, MemtableShadowsOlderSSTableVersion) {
  ASSERT_TRUE(Put("port", "old_value").ok());

  // Force a flush by filling the buffer with unrelated keys.
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_GT(db_->GetStats().flushes, 0u);

  ASSERT_TRUE(Put("port", "new_value").ok());
  EXPECT_EQ("new_value", Get("port"))
      << "the memtable must be searched before any sstable";
}

// The resurrection bug, tested directly. A delete written after a flush leaves
// the value in an SSTable and the tombstone in the memtable. If the read path
// treated "found a tombstone" as "keep looking", the deleted value would come
// back from disk.
TEST_F(FlushTest, TombstoneInMemtableHidesValueInSSTable) {
  ASSERT_TRUE(Put("vessel", "ARUNA CIHAN").ok());

  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_GT(db_->GetStats().flushes, 0u);

  ASSERT_TRUE(Del("vessel").ok());
  EXPECT_EQ("NOT_FOUND", Get("vessel")) << "the deleted value was resurrected";
}

// And across two SSTables: value in an older table, tombstone in a newer one.
// This is the case that requires L0 to be searched newest-first.
TEST_F(FlushTest, TombstoneInNewerSSTableHidesOlderSSTableValue) {
  ASSERT_TRUE(Put("vessel", "ARUNA CIHAN").ok());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("a_filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  const uint64_t flushes_after_first = db_->GetStats().flushes;
  ASSERT_GT(flushes_after_first, 0u);

  ASSERT_TRUE(Del("vessel").ok());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("b_filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_GT(db_->GetStats().flushes, flushes_after_first)
      << "the tombstone should now live in its own sstable";

  EXPECT_EQ("NOT_FOUND", Get("vessel"))
      << "L0 must be searched newest-first, or the old value wins";
}

TEST_F(FlushTest, DataSurvivesReopenAfterFlushing) {
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(Put(Key(i), "value_" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(Del(Key(7)).ok());
  ASSERT_TRUE(Put(Key(11), "overwritten").ok());
  ASSERT_TRUE(db_->SyncWAL().ok());

  const uint64_t sequence_before = db_->GetStats().sequence;
  Reopen();

  EXPECT_EQ(sequence_before, db_->GetStats().sequence)
      << "the descriptor must restore the sequence number";
  EXPECT_GT(db_->GetStats().num_sstables, 0u) << "sstables must be reopened";

  for (int i = 0; i < 3000; ++i) {
    if (i == 7) {
      EXPECT_EQ("NOT_FOUND", Get(Key(i)));
    } else if (i == 11) {
      EXPECT_EQ("overwritten", Get(Key(i)));
    } else {
      EXPECT_EQ("value_" + std::to_string(i), Get(Key(i))) << "lost " << Key(i);
    }
  }
}

TEST_F(FlushTest, ReopenRepeatedlyWithoutLosingData) {
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 800; ++i) {
      ASSERT_TRUE(Put(Key(round * 1000 + i), "r" + std::to_string(round)).ok());
    }
    ASSERT_NO_FATAL_FAILURE(Reopen()) << "round " << round;

    for (int r = 0; r <= round; ++r) {
      for (int i = 0; i < 800; i += 97) {
        EXPECT_EQ("r" + std::to_string(r), Get(Key(r * 1000 + i)))
            << "round " << round << " lost data from round " << r;
      }
    }
  }
}

// The old log must be deleted only after the descriptor commits, so at no point
// should the directory accumulate logs indefinitely.
TEST_F(FlushTest, OldLogsAreReclaimedAfterFlush) {
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(Put(Key(i), std::string(100, 'v')).ok());
  }
  ASSERT_GT(db_->GetStats().flushes, 1u);
  EXPECT_EQ(1, CountFiles(".log")) << "exactly one live log should remain";
}

TEST_F(FlushTest, SnapshotStillIsolatesAcrossAFlush) {
  ASSERT_TRUE(Put("port", "Rotterdam").ok());
  const Snapshot* snap = db_->GetSnapshot();

  ASSERT_TRUE(Put("port", "Rotterdam Botlek").ok());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_GT(db_->GetStats().flushes, 0u);

  EXPECT_EQ("Rotterdam", Get("port", snap))
      << "a snapshot must survive its data being flushed to disk";
  EXPECT_EQ("Rotterdam Botlek", Get("port"));

  db_->ReleaseSnapshot(snap);
}

TEST_F(FlushTest, ReadsAreServedFromDiskAfterFlush) {
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(Put(Key(i), std::string(80, 'v')).ok());
  }
  for (int i = 0; i < 100; ++i) Get(Key(i));  // early keys are in old tables

  const Stats s = db_->GetStats();
  EXPECT_GT(s.sstable_hits, 0u) << "some reads should be answered from sstables";
  EXPECT_GT(s.sstables_probed, 0u);
}

TEST_F(FlushTest, LargeValuesSurviveFlush) {
  const std::string big(200 * 1024, 'z');  // far larger than one block
  ASSERT_TRUE(Put("big", big).ok());
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(Put("filler" + std::to_string(i), std::string(100, 'x')).ok());
  }
  ASSERT_TRUE(db_->SyncWAL().ok());
  Reopen();
  EXPECT_EQ(big, Get("big"));
}
