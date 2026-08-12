#include "wal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "env.h"

using namespace sextant::lsm;

namespace {

class WALTest : public ::testing::Test {
 protected:
  std::string path_;

  void SetUp() override {
    path_ = std::string("wal_test_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".log";
    std::remove(path_.c_str());
  }
  void TearDown() override { std::remove(path_.c_str()); }

  void WriteAll(const std::vector<std::string>& records) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Open(path_, /*append=*/false, &file).ok());
    wal::Writer writer(file.get());
    for (const auto& r : records) {
      ASSERT_TRUE(writer.AddRecord(Slice(r)).ok());
    }
    ASSERT_TRUE(writer.Sync().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  struct Reporter : public wal::Reader::Reporter {
    size_t dropped_bytes = 0;
    int corruptions = 0;
    void Corruption(size_t bytes, const Status&) override {
      dropped_bytes += bytes;
      ++corruptions;
    }
  };

  std::vector<std::string> ReadAll(Reporter* reporter = nullptr) {
    Reporter local;
    if (reporter == nullptr) reporter = &local;

    std::unique_ptr<SequentialFile> file;
    EXPECT_TRUE(SequentialFile::Open(path_, &file).ok());

    wal::Reader reader(file.get(), reporter, /*checksum=*/true);
    std::vector<std::string> out;
    std::string scratch;
    Slice record;
    while (reader.ReadRecord(&record, &scratch)) out.push_back(record.ToString());
    return out;
  }
};

}  // namespace

TEST_F(WALTest, EmptyLog) {
  WriteAll({});
  EXPECT_TRUE(ReadAll().empty());
}

TEST_F(WALTest, SmallRecordsRoundTrip) {
  const std::vector<std::string> records = {"foo", "bar", "", "xyzzy",
                                            std::string("with\0nul", 8)};
  WriteAll(records);
  EXPECT_EQ(records, ReadAll());
}

// A record larger than one 32 KB block must be split into FIRST/MIDDLE/LAST
// fragments and reassembled transparently.
TEST_F(WALTest, RecordSpanningManyBlocks) {
  std::vector<std::string> records = {
      std::string(wal::kBlockSize - 100, 'a'),  // just under a block
      std::string(wal::kBlockSize, 'b'),        // exactly a block
      std::string(wal::kBlockSize * 3 + 17, 'c'),
      "small tail",
  };
  WriteAll(records);
  EXPECT_EQ(records, ReadAll());
}

TEST_F(WALTest, ManyRandomSizedRecords) {
  std::mt19937 rnd(4242);
  std::vector<std::string> records;
  for (int i = 0; i < 500; ++i) {
    const size_t len = rnd() % 3000;
    records.emplace_back(len, static_cast<char>('a' + (i % 26)));
  }
  WriteAll(records);
  EXPECT_EQ(records, ReadAll());
}

// THE CRASH INVARIANT.
//
// A crash mid-append leaves a partial trailing record. On recovery its CRC
// fails (or its header is incomplete), the reader stops cleanly, and every
// record that was fully written before the crash survives. Truncating the file
// simulates exactly this without having to kill a process.
TEST_F(WALTest, TruncatedTailDropsOnlyTheLastRecord) {
  std::vector<std::string> records;
  for (int i = 0; i < 20; ++i) records.push_back("record_" + std::to_string(i));
  WriteAll(records);

  uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(path_, &size).ok());

  // Chop a few bytes off the end - a torn final record.
  ASSERT_TRUE(TruncateFile(path_, size - 4).ok());

  const std::vector<std::string> recovered = ReadAll();

  ASSERT_EQ(records.size() - 1, recovered.size())
      << "exactly one record - the torn one - should be lost";
  for (size_t i = 0; i < recovered.size(); ++i) {
    EXPECT_EQ(records[i], recovered[i]) << "record " << i << " must be intact";
  }
}

TEST_F(WALTest, TruncationMidHeaderIsHandled) {
  WriteAll({"alpha", "beta", "gamma"});

  uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(path_, &size).ok());
  // Leave only part of the final 7-byte header.
  ASSERT_TRUE(TruncateFile(path_, size - std::string("gamma").size() - 3).ok());

  const std::vector<std::string> recovered = ReadAll();
  ASSERT_EQ(2u, recovered.size());
  EXPECT_EQ("alpha", recovered[0]);
  EXPECT_EQ("beta", recovered[1]);
}

// Bit rot in the middle of the log must be DETECTED, not silently replayed.
TEST_F(WALTest, CorruptedPayloadIsDetected) {
  WriteAll({"alpha", "beta", "gamma"});

  // Flip a bit inside the second record's payload.
  std::FILE* f = std::fopen(path_.c_str(), "r+b");
  ASSERT_NE(nullptr, f);
  const long offset = wal::kHeaderSize + 5 + wal::kHeaderSize + 1;  // inside "beta"
  std::fseek(f, offset, SEEK_SET);
  int c = std::fgetc(f);
  std::fseek(f, offset, SEEK_SET);
  std::fputc(c ^ 0x40, f);
  std::fclose(f);

  Reporter reporter;
  const std::vector<std::string> recovered = ReadAll(&reporter);

  EXPECT_GT(reporter.corruptions, 0) << "the CRC must catch the flipped bit";
  for (const auto& r : recovered) {
    EXPECT_NE("beta", r) << "the corrupted record must not be returned as valid";
  }
}

// The WAL is opened in append mode after recovery, so a reopened log must
// continue correctly from the previous block offset rather than restarting.
TEST_F(WALTest, AppendAfterReopenContinuesTheLog) {
  WriteAll({"first", "second"});

  uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(path_, &size).ok());

  {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Open(path_, /*append=*/true, &file).ok());
    wal::Writer writer(file.get(), size);
    ASSERT_TRUE(writer.AddRecord(Slice("third")).ok());
    ASSERT_TRUE(writer.Sync().ok());
    ASSERT_TRUE(file->Close().ok());
  }

  const std::vector<std::string> expected = {"first", "second", "third"};
  EXPECT_EQ(expected, ReadAll());
}
