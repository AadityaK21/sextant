// Crash recovery, 50 times, at 50 different points.
//
// WHY ONE TRUNCATION POINT IS NOT A CRASH TEST
//
// `DBTest.RecoversFromTornWALTail` cuts three bytes off the end of the log and
// checks the database still opens. That proves the happy case and nothing else,
// because three bytes always lands in the middle of the last record's payload -
// the one place the reader is most obviously going to notice.
//
// A real crash lands anywhere: mid-header, mid-CRC, exactly on a record
// boundary, in the middle of a multi-record batch, or inside the very first
// record so the log has no valid content at all. Each of those exercises a
// different branch of the reader, and the ones that only fail at a boundary are
// exactly the ones a single fixed offset will never find.
//
// THE INVARIANT, AND WHY IT IS THE RIGHT ONE
//
// The tempting assertion is "every write except the last survives". That is
// wrong: a WAL record can hold a whole batch, and a cut near the start can lose
// hundreds of writes legitimately.
//
// The property that actually holds is stronger and simpler. The log is
// append-only and written in order, so a torn tail can only ever lose a SUFFIX.
// Therefore the set of surviving keys must be a PREFIX of the write order:
//
//     survivors = k0 k1 k2 ... kn   for some n
//
// A gap in the middle means the reader accepted a record after skipping a
// damaged one, which is data loss the caller was never told about, and it is
// the failure mode that matters. Checking a prefix catches it at every
// truncation point without the test having to know the record format.
//
// Nothing here asserts HOW MANY writes survive. That is a function of buffering
// and batching, not a durability guarantee, and pinning it would make this test
// fail every time the WAL format got more efficient.

#include "sextant/lsm/db.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "env.h"
#include "filename.h"

using namespace sextant::lsm;

namespace {

class CrashTest : public ::testing::Test {
 protected:
  std::string dbname_;

  void SetUp() override {
    dbname_ = std::string("crashtest_") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name();
    Destroy();
  }
  void TearDown() override { Destroy(); }

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

  // The log the next open will replay. Day 4 assigns log numbers from the
  // VersionSet counter and rotates on open, so this is not always 000001.log.
  std::string CurrentLog() {
    std::vector<std::string> children;
    if (!GetChildren(dbname_, &children).ok()) return {};
    std::string path;
    uint64_t highest = 0;
    for (const auto& c : children) {
      uint64_t number = 0;
      FileType type;
      if (ParseFileName(c, &number, &type) && type == FileType::kLog &&
          number >= highest) {
        highest = number;
        path = dbname_ + "/" + c;
      }
    }
    return path;
  }
};

std::string Key(int i) {
  // Fixed width so the keys sort in write order, which is what makes "is this a
  // prefix" a question about the sequence rather than about string ordering.
  char buf[16];
  std::snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}

std::string Value(int i) { return "value-" + std::to_string(i) + "-payload"; }

// Read and write a whole file with stdio.
//
// Deliberately local rather than added to env.h: this is the only caller in the
// project, and env.h carries a standing warning about adding names there
// because <windows.h> turns a long list of plausible ones into macros.
bool SlurpFile(const std::string& path, std::string* out) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  out->clear();
  char buffer[8192];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, n);
  }
  std::fclose(file);
  return true;
}

bool SpitFile(const std::string& path, const std::string& contents) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const size_t written =
      std::fwrite(contents.data(), 1, contents.size(), file);
  std::fclose(file);
  return written == contents.size();
}

}  // namespace

TEST_F(CrashTest, EveryTruncationPointLeavesAPrefixOfTheWrites) {
  static constexpr int kTrials = 50;
  static constexpr int kWrites = 300;

  // A fixed seed: a crash test that picks different offsets every run is one
  // that cannot be re-run against a failure it just found.
  std::mt19937 rnd(20260816);

  uint64_t total_survivors = 0;
  int fewest = kWrites + 1;
  int most = -1;

  for (int trial = 0; trial < kTrials; ++trial) {
    Destroy();

    {
      std::unique_ptr<DB> db;
      Options opts;
      // Big enough that nothing flushes to an SSTable during the run. This test
      // is about the WAL; a flush would move the data somewhere a truncation
      // cannot reach and the trial would prove nothing.
      opts.write_buffer_size = 8u * 1024 * 1024;
      ASSERT_TRUE(DB::Open(opts, dbname_, &db).ok());

      WriteOptions wo;
      wo.sync = true;
      for (int i = 0; i < kWrites; ++i) {
        ASSERT_TRUE(db->Put(wo, Slice(Key(i)), Slice(Value(i))).ok());
      }
    }  // closed, but deliberately NOT cleanly flushed to a table

    const std::string wal = CurrentLog();
    ASSERT_FALSE(wal.empty()) << "trial " << trial << ": no log file";

    uint64_t size = 0;
    ASSERT_TRUE(GetFileSize(wal, &size).ok());
    ASSERT_GT(size, 64u) << "trial " << trial;

    // Spread the cuts across the whole file rather than clustering at the end.
    // Trial 0 cuts a single byte off, which is the boundary case most likely to
    // be handled by accident.
    const uint64_t cut =
        trial == 0 ? size - 1
                   : static_cast<uint64_t>(rnd() % static_cast<uint32_t>(size));
    ASSERT_TRUE(TruncateFile(wal, cut).ok());

    std::unique_ptr<DB> db;
    Options opts;
    opts.paranoid_checks = false;  // a torn tail is expected, not corruption
    const Status s = DB::Open(opts, dbname_, &db);
    ASSERT_TRUE(s.ok()) << "trial " << trial << " cut at " << cut << " of " << size
                        << ": " << s.ToString();

    // Read every key back and check the survivors form a prefix.
    int last_present = -1;
    int survivors = 0;
    for (int i = 0; i < kWrites; ++i) {
      std::string value;
      const Status get = db->Get(ReadOptions{}, Slice(Key(i)), &value);
      if (get.ok()) {
        // A surviving record must hold the value that was written, not a
        // truncated or spliced one. This is the assertion that would catch a
        // reader accepting a partial payload.
        ASSERT_EQ(Value(i), value)
            << "trial " << trial << " cut at " << cut << ": key " << i
            << " came back corrupted";
        ASSERT_EQ(last_present + 1, i)
            << "trial " << trial << " cut at " << cut << " of " << size
            << ": key " << i << " survived but key " << (last_present + 1)
            << " did not. A torn tail can only lose a suffix, so a gap means "
               "recovery accepted a record after skipping a damaged one.";
        last_present = i;
        ++survivors;
      } else {
        ASSERT_TRUE(get.IsNotFound())
            << "trial " << trial << ": unexpected " << get.ToString();
      }
    }

    total_survivors += static_cast<uint64_t>(survivors);
    fewest = std::min(fewest, survivors);
    most = std::max(most, survivors);
  }

  std::printf(
      "  %d trials, %d writes each: %.1f%% survived on average, "
      "worst cut kept %d, best kept %d\n",
      kTrials, kWrites,
      100.0 * static_cast<double>(total_survivors) /
          static_cast<double>(kTrials * kWrites),
      fewest, most);

  // THE TRIALS HAVE TO ACTUALLY VARY, or 50 runs is one run repeated.
  //
  // Note what is NOT asserted: that some trial recovers all 300. It cannot.
  // Every Put here is sync=true, so each one is its own WAL record, and even
  // the one-byte cut in trial 0 destroys the last record. Expecting 300/300
  // was the first version of this check and it failed for that reason - the
  // assertion was wrong, not the engine.
  //
  // What must hold is spread: a cut near the front should lose nearly
  // everything, a cut near the back should lose nearly nothing.
  EXPECT_LT(fewest, kWrites / 10)
      << "no cut landed near the front of the log; the offsets are not spread";
  EXPECT_GT(most, kWrites - kWrites / 10)
      << "no cut landed near the tail; the offsets are not spread";
}

// A log damaged in the middle rather than at the end is CORRUPTION, not a
// crash, and the two deserve different answers. A crash cannot flip a byte in a
// record that was already written; a failing disk can.
TEST_F(CrashTest, AFlippedByteInTheMiddleIsCorruptionRatherThanATornTail) {
  {
    std::unique_ptr<DB> db;
    Options opts;
    opts.write_buffer_size = 8u * 1024 * 1024;
    ASSERT_TRUE(DB::Open(opts, dbname_, &db).ok());
    WriteOptions wo;
    wo.sync = true;
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Put(wo, Slice(Key(i)), Slice(Value(i))).ok());
    }
  }

  const std::string wal = CurrentLog();
  ASSERT_FALSE(wal.empty());

  // Flip a bit a third of the way in, well clear of the tail.
  std::string contents;
  ASSERT_TRUE(SlurpFile(wal, &contents));
  ASSERT_GT(contents.size(), 128u);
  const size_t offset = contents.size() / 3;
  contents[offset] = static_cast<char>(contents[offset] ^ 0x40);
  ASSERT_TRUE(SpitFile(wal, contents));

  std::unique_ptr<DB> db;
  Options opts;
  opts.paranoid_checks = false;
  ASSERT_TRUE(DB::Open(opts, dbname_, &db).ok())
      << "a damaged record should be dropped, not make the database unopenable";

  // The CRC must have caught it, so the damaged record and everything after it
  // is gone. What must NOT happen is the corrupted value being served as if it
  // were real.
  int last_present = -1;
  for (int i = 0; i < 200; ++i) {
    std::string value;
    if (db->Get(ReadOptions{}, Slice(Key(i)), &value).ok()) {
      EXPECT_EQ(Value(i), value) << "key " << i << " was served corrupted";
      EXPECT_EQ(last_present + 1, i) << "recovery skipped past a damaged record";
      last_present = i;
    }
  }
  EXPECT_LT(last_present, 199)
      << "a bit flip a third of the way in should have cost the tail";
}
