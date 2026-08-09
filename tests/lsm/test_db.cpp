#include "sextant/lsm/db.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "env.h"

using namespace sextant::lsm;

namespace {

class DBTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<DB> db_;

  void SetUp() override {
    dbname_ = std::string("dbtest_") +
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
        DeleteFile(dbname_ + "/" + c);
      }
    }
    std::remove(dbname_.c_str());
  }

  void Reopen(Options opts = Options{}) {
    db_.reset();
    ASSERT_TRUE(DB::Open(opts, dbname_, &db_).ok());
  }

  Status Put(const std::string& k, const std::string& v, bool sync = false) {
    WriteOptions wo;
    wo.sync = sync;
    return db_->Put(wo, Slice(k), Slice(v));
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
};

}  // namespace

TEST_F(DBTest, PutGetDelete) {
  ASSERT_TRUE(Put("NLRTM", "Rotterdam").ok());
  EXPECT_EQ("Rotterdam", Get("NLRTM"));
  EXPECT_EQ("NOT_FOUND", Get("DEHAM"));

  ASSERT_TRUE(Del("NLRTM").ok());
  EXPECT_EQ("NOT_FOUND", Get("NLRTM"));
}

TEST_F(DBTest, OverwriteReturnsNewestValue) {
  ASSERT_TRUE(Put("k", "v1").ok());
  ASSERT_TRUE(Put("k", "v2").ok());
  ASSERT_TRUE(Put("k", "v3").ok());
  EXPECT_EQ("v3", Get("k"));
}

TEST_F(DBTest, WriteBatchIsAtomicAndOrdered) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  batch.Put("c", "3");

  ASSERT_EQ(4, batch.Count());
  ASSERT_TRUE(db_->Write(WriteOptions{}, &batch).ok());

  // Each record in a batch gets its own sequence number, so the delete of "a"
  // must win over the earlier put in the SAME batch.
  EXPECT_EQ("NOT_FOUND", Get("a"));
  EXPECT_EQ("2", Get("b"));
  EXPECT_EQ("3", Get("c"));
}

// This is the property an entity merge relies on: an ENTITY record, its LINK
// records and its PROV records must all appear together or not at all.
TEST_F(DBTest, WriteBatchSurvivesReopenAsAUnit) {
  WriteBatch batch;
  batch.Put("ENTITY|port|01H", "{...}");
  batch.Put("LINKOUT|01H|arrives", "");
  batch.Put("PROV|01H|name", "wpi row 2841");
  ASSERT_TRUE(db_->Write(WriteOptions{true}, &batch).ok());

  Reopen();

  EXPECT_EQ("{...}", Get("ENTITY|port|01H"));
  EXPECT_EQ("", Get("LINKOUT|01H|arrives"));
  EXPECT_EQ("wpi row 2841", Get("PROV|01H|name"));
}

TEST_F(DBTest, DataSurvivesReopen) {
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(Put("key" + std::to_string(i), "value" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(db_->SyncWAL().ok());

  Reopen();

  for (int i = 0; i < 500; ++i) {
    EXPECT_EQ("value" + std::to_string(i), Get("key" + std::to_string(i)));
  }
}

TEST_F(DBTest, DeletesSurviveReopen) {
  ASSERT_TRUE(Put("keep", "yes").ok());
  ASSERT_TRUE(Put("drop", "no").ok());
  ASSERT_TRUE(Del("drop").ok());
  ASSERT_TRUE(db_->SyncWAL().ok());

  Reopen();

  EXPECT_EQ("yes", Get("keep"));
  EXPECT_EQ("NOT_FOUND", Get("drop"))
      << "a tombstone must survive recovery, or the delete would be undone";
}

TEST_F(DBTest, SequenceNumbersContinueAfterReopen) {
  ASSERT_TRUE(Put("a", "1").ok());
  ASSERT_TRUE(Put("b", "2").ok());
  const uint64_t before = db_->GetStats().sequence;
  ASSERT_EQ(2u, before);

  ASSERT_TRUE(db_->SyncWAL().ok());
  Reopen();

  EXPECT_EQ(before, db_->GetStats().sequence)
      << "recovery must restore the sequence, not restart it";

  ASSERT_TRUE(Put("c", "3").ok());
  EXPECT_EQ(before + 1, db_->GetStats().sequence);
}

// Snapshots are what let a multi-hop graph traversal see a consistent graph
// across thousands of key lookups.
TEST_F(DBTest, SnapshotIsolatesReadsFromLaterWrites) {
  ASSERT_TRUE(Put("port", "Rotterdam").ok());

  const Snapshot* snap = db_->GetSnapshot();

  ASSERT_TRUE(Put("port", "Rotterdam Botlek").ok());
  ASSERT_TRUE(Put("newport", "Hamburg").ok());

  EXPECT_EQ("Rotterdam", Get("port", snap)) << "the snapshot must not see later writes";
  EXPECT_EQ("NOT_FOUND", Get("newport", snap));

  EXPECT_EQ("Rotterdam Botlek", Get("port")) << "a live read sees the newest value";
  EXPECT_EQ("Hamburg", Get("newport"));

  db_->ReleaseSnapshot(snap);
}

TEST_F(DBTest, SnapshotSeesDataBeforeADelete) {
  ASSERT_TRUE(Put("vessel", "ARUNA CIHAN").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_TRUE(Del("vessel").ok());

  EXPECT_EQ("ARUNA CIHAN", Get("vessel", snap));
  EXPECT_EQ("NOT_FOUND", Get("vessel"));

  db_->ReleaseSnapshot(snap);
}

TEST_F(DBTest, EmptyBatchIsANoop) {
  WriteBatch batch;
  EXPECT_TRUE(db_->Write(WriteOptions{}, &batch).ok());
  EXPECT_EQ(0u, db_->GetStats().sequence);
}

TEST_F(DBTest, HandlesBinaryKeysAndValues) {
  const std::string key("\x00\x01\xffkey", 6);
  const std::string value("val\x00ue", 6);
  ASSERT_TRUE(Put(key, value).ok());
  ASSERT_TRUE(db_->SyncWAL().ok());
  Reopen();
  EXPECT_EQ(value, Get(key));
}

TEST_F(DBTest, HandlesLargeValues) {
  const std::string big(2 * 1024 * 1024, 'x');  // 2 MB, spans many WAL blocks
  ASSERT_TRUE(Put("big", big).ok());
  ASSERT_TRUE(db_->SyncWAL().ok());
  Reopen();
  EXPECT_EQ(big, Get("big"));
}

TEST_F(DBTest, StatsAreTracked) {
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(Put("k" + std::to_string(i), "v").ok());
  for (int i = 0; i < 10; ++i) Get("k" + std::to_string(i));

  const Stats s = db_->GetStats();
  EXPECT_EQ(10u, s.writes);
  EXPECT_EQ(10u, s.reads);
  EXPECT_EQ(10u, s.memtable_hits);
  EXPECT_GT(s.bytes_written, 0u);
  EXPECT_GT(s.memtable_bytes, 0u);
}

// A torn WAL tail is what a crash actually looks like on disk. Every write
// acknowledged before the crash must still be there afterwards.
TEST_F(DBTest, RecoversFromTornWALTail) {
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i), /*sync=*/true).ok());
  }
  db_.reset();  // close cleanly, then damage the tail

  const std::string wal_path = dbname_ + "/000001.log";
  uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(wal_path, &size).ok());
  ASSERT_TRUE(TruncateFile(wal_path, size - 3).ok());

  Options opts;
  opts.paranoid_checks = false;  // a torn tail is expected, not corruption
  Reopen(opts);

  // The last record may be lost; everything before it must survive.
  for (int i = 0; i < 99; ++i) {
    EXPECT_EQ("v" + std::to_string(i), Get("k" + std::to_string(i)))
        << "acknowledged write " << i << " was lost";
  }
}
