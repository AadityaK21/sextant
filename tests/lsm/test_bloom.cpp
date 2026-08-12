#include "bloom.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "filter_block.h"

using namespace sextant::lsm;

namespace {

std::string NumKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%09d", i);
  return buf;
}

class BloomTest : public ::testing::Test {
 protected:
  BloomFilterPolicy policy_{10};
  std::vector<std::string> keys_;
  std::string filter_;

  void Add(const std::string& key) { keys_.push_back(key); }

  void Build() {
    std::vector<Slice> slices;
    slices.reserve(keys_.size());
    for (const auto& k : keys_) slices.emplace_back(k);
    filter_.clear();
    policy_.CreateFilter(slices.data(), static_cast<int>(slices.size()), &filter_);
    keys_.clear();
  }

  bool Matches(const std::string& key) {
    if (!keys_.empty()) Build();
    return policy_.KeyMayMatch(Slice(key), Slice(filter_));
  }

  size_t FilterBytes() const { return filter_.size(); }
};

}  // namespace

TEST_F(BloomTest, EmptyFilterMatchesNothing) {
  Build();
  EXPECT_FALSE(Matches("hello"));
  EXPECT_FALSE(Matches("world"));
}

// THE defining property: a bloom filter may produce false positives but must
// NEVER produce a false negative. A false negative would mean the read path
// skips a table that really does hold the key, silently losing data.
TEST_F(BloomTest, NoFalseNegativesEver) {
  for (int i = 0; i < 10000; ++i) Add(NumKey(i));
  Build();

  for (int i = 0; i < 10000; ++i) {
    EXPECT_TRUE(Matches(NumKey(i)))
        << "FALSE NEGATIVE for " << NumKey(i) << " - this would lose data";
  }
}

TEST_F(BloomTest, ProbeCountMatchesTheory) {
  // k = (m/n) * ln2.  At 10 bits per key that is 6.93, so 6 after truncation
  // of the 0.69 approximation.
  EXPECT_EQ(6u, BloomFilterPolicy(10).num_probes());
  EXPECT_EQ(1u, BloomFilterPolicy(1).num_probes());
  EXPECT_EQ(13u, BloomFilterPolicy(20).num_probes());
}

// The false-positive rate should land near the theoretical
//     p = (1 - e^(-kn/m))^k
// At m/n = 10 with k = 6 that is about 1.1%. Allow generous slack: this is a
// probabilistic structure and the test must not be flaky.
TEST_F(BloomTest, FalsePositiveRateIsNearTheory) {
  static constexpr int kNumKeys = 10000;
  for (int i = 0; i < kNumKeys; ++i) Add(NumKey(i));
  Build();

  int false_positives = 0;
  static constexpr int kProbes = 20000;
  for (int i = 0; i < kProbes; ++i) {
    // Probe a disjoint key space so every hit is by definition a false one.
    if (Matches(NumKey(1000000 + i))) ++false_positives;
  }

  const double rate = static_cast<double>(false_positives) / kProbes;

  const double m = static_cast<double>(kNumKeys) * 10.0;
  const double k = static_cast<double>(BloomFilterPolicy(10).num_probes());
  const double theoretical =
      std::pow(1.0 - std::exp(-k * kNumKeys / m), k);

  EXPECT_LT(rate, 0.05) << "false positive rate " << rate
                        << " is far above the ~" << theoretical << " expected";
  EXPECT_GT(theoretical, 0.0);
}

TEST_F(BloomTest, SpaceIsRoughlyTenBitsPerKey) {
  static constexpr int kNumKeys = 10000;
  for (int i = 0; i < kNumKeys; ++i) Add(NumKey(i));
  Build();

  // 10 bits/key = 1.25 bytes/key, plus one trailing probe-count byte.
  const double bytes_per_key = static_cast<double>(FilterBytes()) / kNumKeys;
  EXPECT_GT(bytes_per_key, 1.2);
  EXPECT_LT(bytes_per_key, 1.3);
}

TEST_F(BloomTest, SmallFilterStillWorks) {
  Add("a");
  Add("b");
  Build();
  EXPECT_TRUE(Matches("a"));
  EXPECT_TRUE(Matches("b"));
}

TEST_F(BloomTest, HandlesBinaryKeys) {
  Add(std::string("\x00\x01\xff", 3));
  Add(std::string("\xff\x00", 2));
  Build();
  EXPECT_TRUE(Matches(std::string("\x00\x01\xff", 3)));
  EXPECT_TRUE(Matches(std::string("\xff\x00", 2)));
}

TEST(Hash, IsWellDistributed) {
  // Sanity check that the hash is not degenerate: 10k sequential keys should
  // produce close to 10k distinct hashes.
  std::set<uint32_t> seen;
  for (int i = 0; i < 10000; ++i) {
    const std::string k = NumKey(i);
    seen.insert(Hash(k.data(), k.size(), 0xbc9f1d34));
  }
  EXPECT_GT(seen.size(), 9900u) << "hash collides far too often";
}

// --- filter block ----------------------------------------------------------

TEST(FilterBlock, EmptyBlock) {
  BloomFilterPolicy policy(10);
  FilterBlockBuilder builder(&policy);
  const Slice block = builder.Finish();

  FilterBlockReader reader(&policy, block);
  // No filter covers these offsets, so the reader must answer "maybe" rather
  // than "definitely not". Failing open is mandatory: a false negative loses
  // data, a false positive only costs a wasted read.
  EXPECT_TRUE(reader.KeyMayMatch(0, Slice("foo")));
  EXPECT_TRUE(reader.KeyMayMatch(100000, Slice("foo")));
}

TEST(FilterBlock, SingleChunk) {
  BloomFilterPolicy policy(10);
  FilterBlockBuilder builder(&policy);

  builder.StartBlock(100);
  builder.AddKey(Slice("foo"));
  builder.AddKey(Slice("bar"));
  builder.AddKey(Slice("box"));
  builder.StartBlock(200);
  builder.AddKey(Slice("box"));
  builder.StartBlock(300);
  builder.AddKey(Slice("hello"));

  const Slice block = builder.Finish();
  FilterBlockReader reader(&policy, block);

  EXPECT_TRUE(reader.KeyMayMatch(100, Slice("foo")));
  EXPECT_TRUE(reader.KeyMayMatch(100, Slice("bar")));
  EXPECT_TRUE(reader.KeyMayMatch(100, Slice("box")));
  EXPECT_TRUE(reader.KeyMayMatch(100, Slice("hello")));
  EXPECT_FALSE(reader.KeyMayMatch(100, Slice("missing")));
  EXPECT_FALSE(reader.KeyMayMatch(100, Slice("other")));
}

TEST(FilterBlock, MultipleChunksAreIndependent) {
  BloomFilterPolicy policy(10);
  FilterBlockBuilder builder(&policy);

  // Chunk 0 (offsets 0 .. 2047)
  builder.StartBlock(0);
  builder.AddKey(Slice("foo"));
  builder.AddKey(Slice("bar"));

  // Chunk 1 (offsets 2048 .. 4095) - a different 2 KB range
  builder.StartBlock(3100);
  builder.AddKey(Slice("box"));

  // Chunk 3, skipping chunk 2 entirely
  builder.StartBlock(9000);
  builder.AddKey(Slice("hello"));

  const Slice block = builder.Finish();
  FilterBlockReader reader(&policy, block);

  EXPECT_TRUE(reader.KeyMayMatch(0, Slice("foo")));
  EXPECT_TRUE(reader.KeyMayMatch(2000, Slice("bar")));
  EXPECT_FALSE(reader.KeyMayMatch(0, Slice("box")))
      << "a key from a later chunk must not match an earlier one";

  EXPECT_TRUE(reader.KeyMayMatch(3100, Slice("box")));
  EXPECT_FALSE(reader.KeyMayMatch(3100, Slice("foo")));

  EXPECT_TRUE(reader.KeyMayMatch(9000, Slice("hello")));
  EXPECT_FALSE(reader.KeyMayMatch(9000, Slice("foo")));
}
