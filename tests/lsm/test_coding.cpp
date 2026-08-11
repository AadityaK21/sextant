#include "coding.h"

#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace sextant::lsm;

// The central property of the encoding: for fixed-width big-endian integers,
// BYTE ORDER EQUALS NUMERIC ORDER. Everything in src/codec depends on this -
// it is what turns "voyages into this port last quarter" into one range scan.
TEST(Coding, Fixed64BEPreservesNumericOrderUnderMemcmp) {
  std::mt19937_64 rnd(12345);
  for (int i = 0; i < 20000; ++i) {
    const uint64_t a = rnd();
    const uint64_t b = rnd();

    std::string ea, eb;
    PutFixed64BE(&ea, a);
    PutFixed64BE(&eb, b);

    const int byte_order = Slice(ea).compare(Slice(eb));
    const int num_order = (a < b) ? -1 : (a > b ? 1 : 0);

    EXPECT_EQ(num_order, byte_order < 0 ? -1 : (byte_order > 0 ? 1 : 0))
        << "a=" << a << " b=" << b;
  }
}

TEST(Coding, Fixed32BEPreservesNumericOrderUnderMemcmp) {
  std::mt19937 rnd(999);
  for (int i = 0; i < 20000; ++i) {
    const uint32_t a = rnd();
    const uint32_t b = rnd();

    std::string ea, eb;
    PutFixed32BE(&ea, a);
    PutFixed32BE(&eb, b);

    const int byte_order = Slice(ea).compare(Slice(eb));
    const int num_order = (a < b) ? -1 : (a > b ? 1 : 0);
    EXPECT_EQ(num_order, byte_order < 0 ? -1 : (byte_order > 0 ? 1 : 0));
  }
}

// Sanity check that we really are big-endian on the wire, independent of the
// host's native byte order.
TEST(Coding, Fixed64BEByteLayoutIsExplicit) {
  std::string s;
  PutFixed64BE(&s, 0x0102030405060708ull);
  ASSERT_EQ(8u, s.size());
  EXPECT_EQ(static_cast<uint8_t>(s[0]), 0x01);
  EXPECT_EQ(static_cast<uint8_t>(s[7]), 0x08);
}

TEST(Coding, Fixed32RoundTrip) {
  std::string s;
  const uint32_t values[] = {0u, 1u, 255u, 256u, 65535u, 1u << 24,
                             std::numeric_limits<uint32_t>::max()};
  for (uint32_t v : values) PutFixed32BE(&s, v);

  const char* p = s.data();
  for (uint32_t v : values) {
    EXPECT_EQ(v, DecodeFixed32BE(p));
    p += 4;
  }
}

TEST(Coding, Fixed64RoundTrip) {
  std::string s;
  const uint64_t values[] = {0ull, 1ull, 255ull, 1ull << 32, 1ull << 55,
                             std::numeric_limits<uint64_t>::max()};
  for (uint64_t v : values) PutFixed64BE(&s, v);

  const char* p = s.data();
  for (uint64_t v : values) {
    EXPECT_EQ(v, DecodeFixed64BE(p));
    p += 8;
  }
}

TEST(Coding, Varint32RoundTrip) {
  std::string s;
  for (uint32_t i = 0; i < (32 * 32); ++i) {
    const uint32_t v = (i / 32) << (i % 32);
    PutVarint32(&s, v);
  }

  const char* p = s.data();
  const char* limit = p + s.size();
  for (uint32_t i = 0; i < (32 * 32); ++i) {
    const uint32_t expected = (i / 32) << (i % 32);
    uint32_t actual = 0;
    p = GetVarint32Ptr(p, limit, &actual);
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(expected, actual);
  }
  EXPECT_EQ(p, limit);
}

TEST(Coding, Varint64RoundTrip) {
  std::vector<uint64_t> values{0, 100, ~static_cast<uint64_t>(0)};
  for (uint32_t k = 0; k < 64; ++k) {
    const uint64_t power = 1ull << k;
    values.push_back(power);
    values.push_back(power - 1);
    values.push_back(power + 1);
  }

  std::string s;
  for (uint64_t v : values) PutVarint64(&s, v);

  const char* p = s.data();
  const char* limit = p + s.size();
  for (uint64_t expected : values) {
    uint64_t actual = 0;
    p = GetVarint64Ptr(p, limit, &actual);
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(expected, actual);
  }
  EXPECT_EQ(p, limit);
}

TEST(Coding, VarintLengthMatchesEncodedSize) {
  const std::vector<uint64_t> cases = {0, 127, 128, 16383, 16384, 1ull << 35,
                                       std::numeric_limits<uint64_t>::max()};
  for (uint64_t v : cases) {
    std::string s;
    PutVarint64(&s, v);
    EXPECT_EQ(static_cast<size_t>(VarintLength(v)), s.size()) << "v=" << v;
  }
}

TEST(Coding, TruncatedVarintIsRejected) {
  std::string s;
  PutVarint32(&s, 1u << 28);  // multi-byte
  ASSERT_GT(s.size(), 1u);

  uint32_t value = 0;
  // Cut the encoding short: the decoder must refuse rather than read past.
  const char* p = s.data();
  EXPECT_EQ(nullptr, GetVarint32Ptr(p, p + s.size() - 1, &value));
}

TEST(Coding, LengthPrefixedSliceRoundTrip) {
  std::string s;
  PutLengthPrefixedSlice(&s, Slice(""));
  PutLengthPrefixedSlice(&s, Slice("foo"));
  PutLengthPrefixedSlice(&s, Slice(std::string(200, 'x')));
  PutLengthPrefixedSlice(&s, Slice(std::string("bin\0ary", 7)));

  Slice input(s);
  Slice v;

  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ("", v.ToString());
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ("foo", v.ToString());
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ(std::string(200, 'x'), v.ToString());
  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &v));
  EXPECT_EQ(std::string("bin\0ary", 7), v.ToString());

  EXPECT_TRUE(input.empty());
}

TEST(Coding, TruncatedLengthPrefixedSliceIsRejected) {
  std::string s;
  PutLengthPrefixedSlice(&s, Slice("hello world"));
  s.resize(s.size() - 3);  // chop the payload

  Slice input(s);
  Slice v;
  EXPECT_FALSE(GetLengthPrefixedSlice(&input, &v));
}
