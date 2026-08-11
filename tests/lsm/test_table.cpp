#include "table.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>

#include "env.h"
#include "internal_key.h"
#include "table_builder.h"

using namespace sextant::lsm;

namespace {

std::string IKey(const std::string& user_key, SequenceNumber seq = 1,
                 ValueType t = kTypeValue) {
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey(user_key, seq, t));
  return encoded;
}

class TableTest : public ::testing::Test {
 protected:
  std::string path_;
  std::unique_ptr<Table> table_;
  uint64_t file_size_ = 0;

  void SetUp() override {
    path_ = std::string("tabletest_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".sst";
    std::remove(path_.c_str());
  }
  void TearDown() override {
    table_.reset();
    std::remove(path_.c_str());
  }

  // Build a table from sorted entries, then reopen it from disk.
  void Build(const std::vector<std::pair<std::string, std::string>>& entries) {
    {
      std::unique_ptr<WritableFile> file;
      ASSERT_TRUE(WritableFile::Open(path_, /*append=*/false, &file).ok());

      TableBuilder builder(Options{}, file.get());
      for (const auto& [k, v] : entries) builder.Add(Slice(k), Slice(v));
      ASSERT_TRUE(builder.Finish().ok());
      file_size_ = builder.FileSize();
      ASSERT_TRUE(file->Close().ok());
    }

    uint64_t on_disk = 0;
    ASSERT_TRUE(GetFileSize(path_, &on_disk).ok());
    ASSERT_EQ(file_size_, on_disk) << "builder's reported size must match the file";

    std::unique_ptr<RandomAccessFile> rfile;
    ASSERT_TRUE(RandomAccessFile::Open(path_, &rfile).ok());
    ASSERT_TRUE(Table::Open(Options{}, std::move(rfile), on_disk, &table_).ok());
  }

  // Point lookup returning "value", "DELETED", or "ABSENT".
  std::string Get(const std::string& user_key, SequenceNumber snapshot = 1000) {
    const LookupKey lkey(Slice(user_key), snapshot);
    std::string out = "ABSENT";
    const Slice uk = lkey.user_key();

    struct Ctx {
      const Slice* user_key;
      std::string* out;
    } ctx{&uk, &out};

    const Status s = table_->InternalGet(
        ReadOptions{}, lkey.internal_key(), &ctx,
        [](void* arg, const Slice& ikey, const Slice& v) {
          auto* c = reinterpret_cast<Ctx*>(arg);
          ParsedInternalKey parsed;
          if (!ParseInternalKey(ikey, &parsed)) return;
          if (parsed.user_key.compare(*c->user_key) != 0) return;
          *c->out = (parsed.type == kTypeValue) ? v.ToString() : "DELETED";
        });
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
  }
};

}  // namespace

TEST_F(TableTest, SingleEntry) {
  Build({{IKey("NLRTM"), "Rotterdam"}});
  EXPECT_EQ("Rotterdam", Get("NLRTM"));
  EXPECT_EQ("ABSENT", Get("DEHAM"));
}

TEST_F(TableTest, EveryKeyIsFindable) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 5000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08d", i);
    entries.emplace_back(IKey(buf), "value_" + std::to_string(i));
  }
  Build(entries);

  for (int i = 0; i < 5000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08d", i);
    EXPECT_EQ("value_" + std::to_string(i), Get(buf)) << "missing " << buf;
  }
  EXPECT_EQ("ABSENT", Get("key99999999"));
  EXPECT_EQ("ABSENT", Get("aaa"));
}

// 5000 entries at ~40 bytes each greatly exceeds the 4 KB block size, so this
// table has many data blocks and the index is doing real work.
TEST_F(TableTest, SpansManyBlocks) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 5000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08d", i);
    entries.emplace_back(IKey(buf), std::string(100, 'v'));
  }
  Build(entries);

  EXPECT_GT(file_size_, 50u * 1024) << "expected a multi-block table";

  std::unique_ptr<Iterator> it(table_->NewIterator(ReadOptions{}));
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ASSERT_LT(n, entries.size());
    EXPECT_EQ(entries[n].first, it->key().ToString()) << "at index " << n;
    ++n;
  }
  EXPECT_EQ(entries.size(), n)
      << "the two-level iterator must cross every block boundary";
  EXPECT_TRUE(it->status().ok());
}

TEST_F(TableTest, IteratorBackwardsAcrossBlocks) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 3000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08d", i);
    entries.emplace_back(IKey(buf), std::string(50, 'x'));
  }
  Build(entries);

  std::unique_ptr<Iterator> it(table_->NewIterator(ReadOptions{}));
  size_t n = entries.size();
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    ASSERT_GT(n, 0u);
    --n;
    EXPECT_EQ(entries[n].first, it->key().ToString()) << "at index " << n;
  }
  EXPECT_EQ(0u, n);
}

TEST_F(TableTest, TombstonesAreStoredAndReported) {
  Build({
      {IKey("gone", 5, kTypeDeletion), ""},
      {IKey("kept", 5, kTypeValue), "here"},
  });

  EXPECT_EQ("DELETED", Get("gone"));
  EXPECT_EQ("here", Get("kept"));
}

TEST_F(TableTest, MultipleVersionsResolveNewestFirst) {
  // Internal keys sort newest-first within a user key, so this is the order a
  // builder receives them from a memtable walk.
  Build({
      {IKey("port", 30), "v30"},
      {IKey("port", 20), "v20"},
      {IKey("port", 10), "v10"},
  });

  EXPECT_EQ("v30", Get("port", 100)) << "a fresh read sees the newest version";
  EXPECT_EQ("v20", Get("port", 25)) << "a snapshot read sees the version as of then";
  EXPECT_EQ("v10", Get("port", 15));
  EXPECT_EQ("ABSENT", Get("port", 5)) << "before the first write, nothing exists";
}

TEST_F(TableTest, HandlesBinaryKeysAndValues) {
  const std::string key("\x01\x00\xff key", 8);
  const std::string value("val\x00ue\xff", 8);
  Build({{IKey(key), value}});
  EXPECT_EQ(value, Get(key));
}

TEST_F(TableTest, HandlesEmptyValues) {
  Build({{IKey("a"), ""}, {IKey("b"), "x"}});
  EXPECT_EQ("", Get("a"));
  EXPECT_EQ("x", Get("b"));
}

TEST_F(TableTest, SeekMatchesStdMapOverRandomProbes) {
  std::mt19937 rnd(31337);
  std::map<std::string, std::string> reference;
  for (int i = 0; i < 2000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08u", static_cast<unsigned>(rnd() % 50000));
    reference[IKey(buf)] = "v" + std::to_string(i);
  }

  Build({reference.begin(), reference.end()});

  std::unique_ptr<Iterator> it(table_->NewIterator(ReadOptions{}));
  for (int probe = 0; probe < 5000; ++probe) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08u", static_cast<unsigned>(rnd() % 50000));
    const std::string target = IKey(buf);

    it->Seek(target);
    const auto expected = reference.lower_bound(target);
    if (expected == reference.end()) {
      EXPECT_FALSE(it->Valid()) << "found a key past the end for " << buf;
    } else {
      ASSERT_TRUE(it->Valid()) << "missed a key for " << buf;
      EXPECT_EQ(expected->first, it->key().ToString());
      EXPECT_EQ(expected->second, it->value().ToString());
    }
  }
}

// Corruption must be detected, not silently returned. This is what the CRC in
// every block trailer is for.
TEST_F(TableTest, CorruptedBlockIsDetected) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 2000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08d", i);
    entries.emplace_back(IKey(buf), std::string(60, 'v'));
  }
  Build(entries);
  table_.reset();

  // Flip a bit early in the file, inside the first data block.
  std::FILE* f = std::fopen(path_.c_str(), "r+b");
  ASSERT_NE(nullptr, f);
  std::fseek(f, 64, SEEK_SET);
  const int c = std::fgetc(f);
  std::fseek(f, 64, SEEK_SET);
  std::fputc(c ^ 0x80, f);
  std::fclose(f);

  uint64_t on_disk = 0;
  ASSERT_TRUE(GetFileSize(path_, &on_disk).ok());
  std::unique_ptr<RandomAccessFile> rfile;
  ASSERT_TRUE(RandomAccessFile::Open(path_, &rfile).ok());

  std::unique_ptr<Table> t;
  ASSERT_TRUE(Table::Open(Options{}, std::move(rfile), on_disk, &t).ok())
      << "the footer and index are intact, so Open should still succeed";

  std::unique_ptr<Iterator> it(t->NewIterator(ReadOptions{}));
  bool saw_error = false;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
  }
  if (!it->status().ok()) saw_error = true;
  EXPECT_TRUE(saw_error) << "the block CRC must reject the flipped bit";
}

TEST_F(TableTest, TruncatedFileIsRejectedAtOpen) {
  Build({{IKey("a"), "1"}, {IKey("b"), "2"}});
  table_.reset();

  uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(path_, &size).ok());
  ASSERT_TRUE(TruncateFile(path_, size - 10).ok());  // eat part of the footer

  std::unique_ptr<RandomAccessFile> rfile;
  ASSERT_TRUE(RandomAccessFile::Open(path_, &rfile).ok());
  std::unique_ptr<Table> t;
  const Status s = Table::Open(Options{}, std::move(rfile), size - 10, &t);
  EXPECT_TRUE(s.IsCorruption()) << "expected corruption, got: " << s.ToString();
}
