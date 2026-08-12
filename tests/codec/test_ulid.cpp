#include "ulid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace sextant::codec;

TEST(Ulid, BinaryRoundTrip) {
  const Ulid original = Ulid::Generate();

  Ulid decoded;
  ASSERT_TRUE(Ulid::FromBinary(original.AsSlice(), &decoded));
  EXPECT_EQ(original, decoded);
}

TEST(Ulid, StringRoundTrip) {
  for (int i = 0; i < 1000; ++i) {
    const Ulid original = Ulid::Generate();
    const std::string text = original.ToString();

    ASSERT_EQ(Ulid::kTextSize, text.size());

    Ulid decoded;
    ASSERT_TRUE(Ulid::FromString(text, &decoded)) << "failed to parse " << text;
    EXPECT_EQ(original, decoded) << "round trip broke for " << text;
  }
}

TEST(Ulid, TextUsesCrockfordAlphabet) {
  const std::string text = Ulid::Generate().ToString();
  for (char c : text) {
    // I, L, O and U are excluded so a transcribed id cannot be misread.
    EXPECT_EQ(std::string::npos, std::string("ILOU").find(c))
        << "ambiguous character '" << c << "' in " << text;
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
        << "unexpected character '" << c << "'";
  }
}

TEST(Ulid, CrockfordDecodingIsForgiving) {
  const Ulid original = Ulid::FromParts(1700000000000ULL, 0x1234, 0xDEADBEEFCAFEBABEULL);
  const std::string text = original.ToString();

  // Lowercase must parse identically - a human retyping an id should not have
  // to get the case right.
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  Ulid decoded;
  ASSERT_TRUE(Ulid::FromString(lower, &decoded));
  EXPECT_EQ(original, decoded);
}

TEST(Ulid, RejectsMalformedText) {
  Ulid out;
  EXPECT_FALSE(Ulid::FromString("", &out));
  EXPECT_FALSE(Ulid::FromString("TOOSHORT", &out));
  EXPECT_FALSE(Ulid::FromString(std::string(27, '0'), &out));
  EXPECT_FALSE(Ulid::FromString(std::string(26, '!'), &out));
}

TEST(Ulid, RejectsWrongSizeBinary) {
  Ulid out;
  EXPECT_FALSE(Ulid::FromBinary(Slice("short"), &out));
  EXPECT_FALSE(Ulid::FromBinary(Slice(std::string(17, 'x')), &out));
}

TEST(Ulid, TimestampIsRecoverable) {
  const uint64_t ts = 1755000000000ULL;
  const Ulid id = Ulid::FromParts(ts, 0xABCD, 0x0123456789ABCDEFULL);
  EXPECT_EQ(ts, id.TimestampMs());
}

// THE defining property. Byte order equals creation order, which is why
// entities created together land together on disk and share a key prefix the
// block format can compress.
TEST(Ulid, ByteOrderMatchesTimeOrder) {
  const Ulid earlier = Ulid::FromParts(1000, 0, 0);
  const Ulid later = Ulid::FromParts(2000, 0, 0);

  EXPECT_LT(earlier, later);
  EXPECT_LT(earlier.AsSlice().compare(later.AsSlice()), 0)
      << "raw bytes must sort in the same order as timestamps";
}

TEST(Ulid, TimestampDominatesRandomness) {
  // An earlier id with the largest possible random component must still sort
  // below a later id with the smallest. Otherwise ordering is not by time.
  const Ulid earlier = Ulid::FromParts(1000, 0xFFFF, ~uint64_t{0});
  const Ulid later = Ulid::FromParts(1001, 0, 0);
  EXPECT_LT(earlier, later);
}

TEST(Ulid, GeneratedIdsAreMonotonic) {
  // A tight loop generates many ids inside one millisecond. Without the
  // increment-on-collision rule they would be ordered at random.
  std::vector<Ulid> ids;
  for (int i = 0; i < 10000; ++i) ids.push_back(Ulid::Generate());

  for (size_t i = 1; i < ids.size(); ++i) {
    EXPECT_LT(ids[i - 1], ids[i])
        << "id " << i << " (" << ids[i].ToString() << ") did not exceed "
        << ids[i - 1].ToString();
  }
}

TEST(Ulid, GeneratedIdsAreUnique) {
  std::set<std::string> seen;
  for (int i = 0; i < 50000; ++i) {
    const std::string text = Ulid::Generate().ToString();
    EXPECT_TRUE(seen.insert(text).second) << "duplicate id: " << text;
  }
}

TEST(Ulid, GenerationIsThreadSafe) {
  // Generation takes a lock precisely so this holds. Under TSan this also
  // proves there is no data race on the shared counter.
  static constexpr int kThreads = 4;
  static constexpr int kPerThread = 5000;

  std::vector<std::vector<std::string>> results(kThreads);
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&results, t] {
      results[static_cast<size_t>(t)].reserve(kPerThread);
      for (int i = 0; i < kPerThread; ++i) {
        results[static_cast<size_t>(t)].push_back(Ulid::Generate().ToString());
      }
    });
  }
  for (auto& th : threads) th.join();

  std::set<std::string> all;
  for (const auto& per_thread : results) {
    for (const auto& id : per_thread) {
      EXPECT_TRUE(all.insert(id).second) << "duplicate across threads: " << id;
    }
  }
  EXPECT_EQ(static_cast<size_t>(kThreads * kPerThread), all.size());
}

TEST(Ulid, DefaultIsAllZeroAndSortsFirst) {
  const Ulid zero;
  EXPECT_EQ(0u, zero.TimestampMs());
  EXPECT_LT(zero, Ulid::Generate());
}
