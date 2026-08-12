// The order-preserving encodings.
//
// Every test here asserts the same property in a different form: BYTE ORDER
// EQUALS LOGICAL ORDER. If any of these fail, secondary indexes silently
// return wrong answers for range queries - the worst kind of bug, because
// point lookups keep working and nothing crashes.

#include "ordered.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace sextant::codec;

namespace {

int ByteCompare(const std::string& a, const std::string& b) {
  const int r = a.compare(b);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

template <typename T>
int LogicalCompare(T a, T b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

std::string EncStr(const std::string& s) {
  std::string out;
  EncodeOrderedString(&out, Slice(s));
  return out;
}

std::string EncInt(int64_t v) {
  std::string out;
  EncodeOrderedInt64(&out, v);
  return out;
}

std::string EncDouble(double v) {
  std::string out;
  EncodeOrderedDouble(&out, v);
  return out;
}

}  // namespace

// --- strings ----------------------------------------------------------------

TEST(OrderedString, RoundTrips) {
  const std::vector<std::string> cases = {
      "", "a", "Rotterdam", "NLRTM",
      std::string("with\0nul", 8),
      std::string("\0\0\0", 3),
      std::string("\xFF\xFF", 2),
      std::string("\0\xFF", 2),        // looks like an escape sequence
      std::string(1000, 'x'),
  };

  for (const auto& original : cases) {
    std::string encoded;
    EncodeOrderedString(&encoded, Slice(original));

    Slice input(encoded);
    std::string decoded;
    ASSERT_TRUE(DecodeOrderedString(&input, &decoded))
        << "failed to decode a " << original.size() << "-byte value";
    EXPECT_EQ(original, decoded);
    EXPECT_TRUE(input.empty()) << "decoder left trailing bytes";
  }
}

TEST(OrderedString, ByteOrderMatchesLogicalOrder) {
  std::vector<std::string> values = {
      "", "a", "aa", "ab", "b", "z",
      "Rotterdam", "Rotterdam Botlek", "Rotterdamm",
      std::string("a\0", 2), std::string("a\0b", 3),
      std::string("\0", 1), std::string("\xFF", 1),
  };

  for (const auto& a : values) {
    for (const auto& b : values) {
      EXPECT_EQ(LogicalCompare(a, b), ByteCompare(EncStr(a), EncStr(b)))
          << "ordering broke for " << a.size() << "-byte vs " << b.size()
          << "-byte value";
    }
  }
}

// A value containing NUL is the case a naive terminator gets wrong: it would
// truncate at the first NUL and two different values would encode identically.
TEST(OrderedString, EmbeddedNulsDoNotCollide) {
  const std::string a("port\0one", 8);
  const std::string b("port\0two", 8);
  const std::string c("port", 4);

  EXPECT_NE(EncStr(a), EncStr(b));
  EXPECT_NE(EncStr(a), EncStr(c));
  EXPECT_LT(EncStr(c), EncStr(a)) << "the shorter value must sort first";
}

TEST(OrderedString, PrefixSortsBeforeExtension) {
  // The terminator (0x00 0x00) must compare below an escaped NUL (0x00 0xFF)
  // and below every real byte, or prefixes sort in the wrong place.
  EXPECT_LT(EncStr("port"), EncStr("ports"));
  EXPECT_LT(EncStr("port"), EncStr(std::string("port\0", 5)));
  EXPECT_LT(EncStr(std::string("port\0", 5)), EncStr("ports"));
}

TEST(OrderedString, RandomisedOrderingHolds) {
  std::mt19937 rnd(20260812);
  std::vector<std::string> values;
  for (int i = 0; i < 400; ++i) {
    const size_t len = rnd() % 12;
    std::string s;
    for (size_t j = 0; j < len; ++j) {
      // Draw from the full byte range so NUL and 0xFF appear regularly.
      s.push_back(static_cast<char>(rnd() % 256));
    }
    values.push_back(s);
  }

  for (size_t i = 0; i < values.size(); ++i) {
    for (size_t j = i; j < values.size(); ++j) {
      EXPECT_EQ(LogicalCompare(values[i], values[j]),
                ByteCompare(EncStr(values[i]), EncStr(values[j])))
          << "at i=" << i << " j=" << j;
    }
  }
}

TEST(OrderedString, RejectsTruncatedInput) {
  std::string encoded = EncStr("hello");
  encoded.resize(encoded.size() - 1);  // chop half the terminator

  Slice input(encoded);
  std::string out;
  EXPECT_FALSE(DecodeOrderedString(&input, &out));
}

TEST(OrderedString, DecodesConsecutiveValues) {
  std::string buf;
  EncodeOrderedString(&buf, Slice("first"));
  EncodeOrderedString(&buf, Slice(std::string("mid\0dle", 7)));
  EncodeOrderedString(&buf, Slice("last"));

  Slice input(buf);
  std::string a, b, c;
  ASSERT_TRUE(DecodeOrderedString(&input, &a));
  ASSERT_TRUE(DecodeOrderedString(&input, &b));
  ASSERT_TRUE(DecodeOrderedString(&input, &c));

  EXPECT_EQ("first", a);
  EXPECT_EQ(std::string("mid\0dle", 7), b);
  EXPECT_EQ("last", c);
  EXPECT_TRUE(input.empty());
}

// --- integers ---------------------------------------------------------------

TEST(OrderedInt, RoundTrips) {
  const std::vector<int64_t> cases = {
      0, 1, -1, 42, -42,
      std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::max(),
      1LL << 40, -(1LL << 40),
  };

  for (int64_t v : cases) {
    std::string encoded = EncInt(v);
    Slice input(encoded);
    int64_t decoded = 0;
    ASSERT_TRUE(DecodeOrderedInt64(&input, &decoded));
    EXPECT_EQ(v, decoded);
  }
}

// The failure this prevents: in two's complement, -1 is 0xFFFF... which as raw
// bytes looks LARGER than 1. Without the sign flip, every negative timestamp
// or coordinate sorts above every positive one.
TEST(OrderedInt, NegativesSortBelowPositives) {
  EXPECT_LT(EncInt(-1), EncInt(0));
  EXPECT_LT(EncInt(-1000), EncInt(-1));
  EXPECT_LT(EncInt(0), EncInt(1));
  EXPECT_LT(EncInt(std::numeric_limits<int64_t>::min()),
            EncInt(std::numeric_limits<int64_t>::max()));
}

TEST(OrderedInt, RandomisedOrderingHolds) {
  std::mt19937_64 rnd(999);
  for (int i = 0; i < 20000; ++i) {
    const auto a = static_cast<int64_t>(rnd());
    const auto b = static_cast<int64_t>(rnd());
    EXPECT_EQ(LogicalCompare(a, b), ByteCompare(EncInt(a), EncInt(b)))
        << "a=" << a << " b=" << b;
  }
}

// --- doubles ----------------------------------------------------------------

TEST(OrderedDouble, RoundTrips) {
  const std::vector<double> cases = {
      0.0, 1.0, -1.0, 0.5, -0.5,
      51.9225, 4.47917,        // Rotterdam
      -33.8688, 151.2093,      // Sydney
      1e300, -1e300, 1e-300, -1e-300,
  };

  for (double v : cases) {
    std::string encoded = EncDouble(v);
    Slice input(encoded);
    double decoded = 0;
    ASSERT_TRUE(DecodeOrderedDouble(&input, &decoded));
    EXPECT_DOUBLE_EQ(v, decoded);
  }
}

// The trap: IEEE 754 negatives ascend in magnitude while descending in value,
// so raw bytes sort them backwards. -1.0 must come before -0.5, not after.
TEST(OrderedDouble, NegativesSortInTheRightDirection) {
  EXPECT_LT(EncDouble(-1000.0), EncDouble(-1.0));
  EXPECT_LT(EncDouble(-1.0), EncDouble(-0.5));
  EXPECT_LT(EncDouble(-0.5), EncDouble(0.0));
  EXPECT_LT(EncDouble(0.0), EncDouble(0.5));
  EXPECT_LT(EncDouble(0.5), EncDouble(1000.0));
}

TEST(OrderedDouble, HandlesLatitudeRangeCorrectly) {
  // The real use: a range scan over port latitudes, which spans the equator.
  std::vector<double> lats = {-89.9, -45.0, -0.001, 0.0, 0.001, 45.0, 51.9225, 89.9};

  std::vector<std::string> encoded;
  for (double v : lats) encoded.push_back(EncDouble(v));

  for (size_t i = 1; i < encoded.size(); ++i) {
    EXPECT_LT(encoded[i - 1], encoded[i])
        << "latitude ordering broke between " << lats[i - 1] << " and " << lats[i];
  }
}

TEST(OrderedDouble, RandomisedOrderingHolds) {
  std::mt19937_64 rnd(31337);
  std::uniform_real_distribution<double> dist(-1e6, 1e6);

  for (int i = 0; i < 20000; ++i) {
    const double a = dist(rnd);
    const double b = dist(rnd);
    EXPECT_EQ(LogicalCompare(a, b), ByteCompare(EncDouble(a), EncDouble(b)))
        << "a=" << a << " b=" << b;
  }
}

TEST(OrderedDouble, SortingEncodedValuesMatchesSortingRawValues) {
  std::mt19937_64 rnd(4242);
  std::uniform_real_distribution<double> dist(-1000.0, 1000.0);

  std::vector<double> values;
  for (int i = 0; i < 2000; ++i) values.push_back(dist(rnd));

  std::vector<std::string> encoded;
  for (double v : values) encoded.push_back(EncDouble(v));

  std::sort(values.begin(), values.end());
  std::sort(encoded.begin(), encoded.end());

  for (size_t i = 0; i < values.size(); ++i) {
    Slice input(encoded[i]);
    double decoded = 0;
    ASSERT_TRUE(DecodeOrderedDouble(&input, &decoded));
    EXPECT_DOUBLE_EQ(values[i], decoded) << "at position " << i;
  }
}
