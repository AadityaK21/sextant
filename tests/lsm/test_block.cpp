#include "block.h"

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "block_builder.h"
#include "internal_key.h"

using namespace sextant::lsm;

namespace {

std::string IKey(const std::string& user_key, SequenceNumber seq = 1,
                 ValueType t = kTypeValue) {
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey(user_key, seq, t));
  return encoded;
}

// Build a block from sorted entries and hand back a reader over it.
class BuiltBlock {
 public:
  explicit BuiltBlock(const std::vector<std::pair<std::string, std::string>>& entries,
                      int restart_interval = 16) {
    BlockBuilder builder(restart_interval);
    for (const auto& [k, v] : entries) builder.Add(Slice(k), Slice(v));
    buffer_ = builder.Finish().ToString();

    BlockContents contents;
    contents.data = Slice(buffer_);
    contents.heap_allocated = false;
    block_ = std::make_unique<Block>(contents);
  }

  Iterator* NewIterator() { return block_->NewIterator(cmp_); }
  size_t encoded_size() const { return buffer_.size(); }

 private:
  std::string buffer_;
  std::unique_ptr<Block> block_;
  InternalKeyComparator cmp_;
};

}  // namespace

TEST(Block, EmptyBlock) {
  BuiltBlock b({});
  std::unique_ptr<Iterator> it(b.NewIterator());
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek(IKey("anything"));
  EXPECT_FALSE(it->Valid());
}

TEST(Block, SingleEntryRoundTrip) {
  const std::string k = IKey("NLRTM");
  BuiltBlock b({{k, "Rotterdam"}});

  std::unique_ptr<Iterator> it(b.NewIterator());
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(k, it->key().ToString());
  EXPECT_EQ("Rotterdam", it->value().ToString());

  it->Next();
  EXPECT_FALSE(it->Valid());
}

TEST(Block, ForwardIterationReturnsEveryEntryInOrder) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 1000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i);
    entries.emplace_back(IKey(buf), "value" + std::to_string(i));
  }

  BuiltBlock b(entries);
  std::unique_ptr<Iterator> it(b.NewIterator());

  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ASSERT_LT(n, entries.size());
    EXPECT_EQ(entries[n].first, it->key().ToString()) << "at index " << n;
    EXPECT_EQ(entries[n].second, it->value().ToString());
    ++n;
  }
  EXPECT_EQ(entries.size(), n);
}

TEST(Block, BackwardIterationMirrorsForward) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k%04d", i);
    entries.emplace_back(IKey(buf), "v" + std::to_string(i));
  }

  BuiltBlock b(entries);
  std::unique_ptr<Iterator> it(b.NewIterator());

  size_t n = entries.size();
  for (it->SeekToLast(); it->Valid(); it->Prev()) {
    ASSERT_GT(n, 0u);
    --n;
    EXPECT_EQ(entries[n].first, it->key().ToString()) << "at index " << n;
  }
  EXPECT_EQ(0u, n);
}

// Seek must land on the first key >= target, crossing restart boundaries
// correctly. With a restart interval of 16 and 500 entries there are ~32
// restart points, so this genuinely exercises the binary search.
TEST(Block, SeekLandsOnFirstKeyAtOrAfterTarget) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 500; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i * 2);  // even numbers only
    entries.emplace_back(IKey(buf), "v" + std::to_string(i));
  }

  BuiltBlock b(entries);
  std::unique_ptr<Iterator> it(b.NewIterator());

  for (int i = 0; i < 500; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%06d", i * 2);
    it->Seek(IKey(buf));
    ASSERT_TRUE(it->Valid()) << "exact seek failed at " << buf;
    EXPECT_EQ(entries[static_cast<size_t>(i)].first, it->key().ToString());
  }

  // An odd key is absent; seek must land on the next even one.
  it->Seek(IKey("key000101"));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(IKey("key000102"), it->key().ToString());

  // Before the first key.
  it->Seek(IKey("aaa"));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(entries.front().first, it->key().ToString());

  // Past the last key: invalid, not wrapped.
  it->Seek(IKey("zzz"));
  EXPECT_FALSE(it->Valid());
}

TEST(Block, RestartIntervalOfOneStillWorks) {
  // restart_interval = 1 means every entry is a restart point and prefix
  // compression is effectively disabled. Correctness must not depend on it.
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 50; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k%03d", i);
    entries.emplace_back(IKey(buf), "v");
  }

  BuiltBlock b(entries, /*restart_interval=*/1);
  std::unique_ptr<Iterator> it(b.NewIterator());

  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    EXPECT_EQ(entries[n].first, it->key().ToString());
    ++n;
  }
  EXPECT_EQ(entries.size(), n);
}

// The payoff for prefix compression, measured rather than asserted. Sextant's
// keys are structured composites that share long prefixes by construction, so
// this ratio is a property of the key design, not a generic LSM claim.
TEST(Block, PrefixCompressionShrinksHighlySimilarKeys) {
  std::vector<std::pair<std::string, std::string>> entries;
  size_t raw_bytes = 0;

  // Simulates a LINKOUT scan: one entity's outgoing links all share a long
  // prefix and differ only in the trailing destination id.
  const std::string prefix = "LINKOUT|0192F3A45B6C7D8E9F0A1B2C3D4E5F60|arrives_at|";
  for (int i = 0; i < 1000; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016d", i);
    const std::string key = IKey(prefix + buf);
    entries.emplace_back(key, "");
    raw_bytes += key.size();
  }

  BuiltBlock b(entries);

  EXPECT_LT(b.encoded_size(), raw_bytes / 2)
      << "expected >2x saving on structured keys; raw=" << raw_bytes
      << " encoded=" << b.encoded_size();
}

TEST(Block, InternalKeyOrderingIsPreservedAcrossVersions) {
  // Same user key at three sequence numbers. The block must return them
  // newest-first, because that is what the read path relies on.
  std::vector<std::pair<std::string, std::string>> entries = {
      {IKey("port", 30), "v30"},
      {IKey("port", 20), "v20"},
      {IKey("port", 10), "v10"},
  };

  BuiltBlock b(entries);
  std::unique_ptr<Iterator> it(b.NewIterator());
  it->SeekToFirst();

  ASSERT_TRUE(it->Valid());
  EXPECT_EQ("v30", it->value().ToString());
  it->Next();
  EXPECT_EQ("v20", it->value().ToString());
  it->Next();
  EXPECT_EQ("v10", it->value().ToString());
}

TEST(Block, RandomisedSeekMatchesStdMap) {
  std::mt19937 rnd(4242);
  std::map<std::string, std::string> reference;

  for (int i = 0; i < 800; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08u", static_cast<unsigned>(rnd() % 100000));
    reference[IKey(buf)] = "v" + std::to_string(i);
  }

  std::vector<std::pair<std::string, std::string>> entries(reference.begin(),
                                                           reference.end());
  // std::map orders bytewise; the block orders by InternalKeyComparator. With
  // one version per user key and a constant sequence, the two agree.
  BuiltBlock b(entries);
  std::unique_ptr<Iterator> it(b.NewIterator());

  for (int probe = 0; probe < 3000; ++probe) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key%08u", static_cast<unsigned>(rnd() % 100000));
    const std::string target = IKey(buf);

    it->Seek(target);
    const auto expected = reference.lower_bound(target);

    if (expected == reference.end()) {
      EXPECT_FALSE(it->Valid()) << "block found a key past the end for " << buf;
    } else {
      ASSERT_TRUE(it->Valid()) << "block missed a key for " << buf;
      EXPECT_EQ(expected->first, it->key().ToString());
      EXPECT_EQ(expected->second, it->value().ToString());
    }
  }
}
