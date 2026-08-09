#include "internal_key.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace sextant::lsm;

namespace {

std::string IKey(const std::string& user_key, SequenceNumber seq, ValueType t) {
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey(user_key, seq, t));
  return encoded;
}

}  // namespace

TEST(InternalKey, RoundTrip) {
  const std::vector<std::string> keys = {"", "k", "hello", std::string("bin\0ary", 7)};
  const std::vector<SequenceNumber> seqs = {1, 100, 1ull << 40, kMaxSequenceNumber};

  for (const std::string& key : keys) {
    for (SequenceNumber seq : seqs) {
      const std::string encoded = IKey(key, seq, kTypeValue);

      ParsedInternalKey decoded;
      ASSERT_TRUE(ParseInternalKey(Slice(encoded), &decoded));
      EXPECT_EQ(key, decoded.user_key.ToString());
      EXPECT_EQ(seq, decoded.sequence);
      EXPECT_EQ(kTypeValue, decoded.type);
    }
  }
}

TEST(InternalKey, RejectsTooShort) {
  ParsedInternalKey decoded;
  EXPECT_FALSE(ParseInternalKey(Slice("short"), &decoded));
}

TEST(InternalKey, RejectsUnknownType) {
  std::string encoded = "userkey";
  PutFixed64BE(&encoded, PackSequenceAndType(42, static_cast<ValueType>(7)));
  ParsedInternalKey decoded;
  EXPECT_FALSE(ParseInternalKey(Slice(encoded), &decoded));
}

// THE ordering property. User keys ascend; within one user key, sequence
// numbers DESCEND so the newest version sorts first. That is what lets Get()
// take the first hit on a forward scan and skip every older version.
TEST(InternalKey, NewestVersionSortsFirst) {
  const InternalKeyComparator cmp;

  const std::string newer = IKey("port", 200, kTypeValue);
  const std::string older = IKey("port", 100, kTypeValue);

  EXPECT_LT(cmp.Compare(Slice(newer), Slice(older)), 0)
      << "the newer sequence must sort BEFORE the older one";
}

TEST(InternalKey, UserKeysSortAscending) {
  const InternalKeyComparator cmp;
  const std::string a = IKey("aaa", 1, kTypeValue);
  const std::string b = IKey("bbb", 999999, kTypeValue);

  // A high sequence number must not let "bbb" jump ahead of "aaa" — user key
  // is always the primary sort field.
  EXPECT_LT(cmp.Compare(Slice(a), Slice(b)), 0);
}

TEST(InternalKey, SortOrderIsFullyDetermined) {
  const InternalKeyComparator cmp;

  std::vector<std::string> keys = {
      IKey("a", 1, kTypeValue),   IKey("a", 3, kTypeValue),
      IKey("a", 2, kTypeDeletion), IKey("b", 1, kTypeValue),
      IKey("ab", 5, kTypeValue),  IKey("", 9, kTypeValue),
  };

  std::sort(keys.begin(), keys.end(), [&](const std::string& x, const std::string& y) {
    return cmp.Compare(Slice(x), Slice(y)) < 0;
  });

  std::vector<std::pair<std::string, SequenceNumber>> got;
  for (const auto& k : keys) {
    ParsedInternalKey p;
    ASSERT_TRUE(ParseInternalKey(Slice(k), &p));
    got.emplace_back(p.user_key.ToString(), p.sequence);
  }

  const std::vector<std::pair<std::string, SequenceNumber>> expected = {
      {"", 9}, {"a", 3}, {"a", 2}, {"a", 1}, {"ab", 5}, {"b", 1},
  };
  EXPECT_EQ(expected, got);
}

// A tombstone and a value at the same sequence can never coexist (each write
// consumes a sequence number), but the comparator must still be a strict weak
// ordering if they did: type breaks the tie, value before deletion.
TEST(InternalKey, TypeBreaksTiesWithinASequence) {
  const InternalKeyComparator cmp;
  const std::string value = IKey("k", 5, kTypeValue);
  const std::string tomb = IKey("k", 5, kTypeDeletion);
  EXPECT_LT(cmp.Compare(Slice(value), Slice(tomb)), 0);
}

TEST(LookupKey, ExposesAllThreeViews) {
  const LookupKey lk(Slice("rotterdam"), 4242);

  EXPECT_EQ("rotterdam", lk.user_key().ToString());
  EXPECT_EQ(std::string("rotterdam").size() + 8, lk.internal_key().size());

  // memtable_key is the internal key with a varint32 length prefix.
  EXPECT_GT(lk.memtable_key().size(), lk.internal_key().size());

  ParsedInternalKey p;
  ASSERT_TRUE(ParseInternalKey(lk.internal_key(), &p));
  EXPECT_EQ("rotterdam", p.user_key.ToString());
  EXPECT_EQ(4242u, p.sequence);
  EXPECT_EQ(kValueTypeForSeek, p.type);
}

// Keys longer than the inline buffer must still work — this exercises the
// heap-allocation path.
TEST(LookupKey, HandlesKeysLargerThanInlineBuffer) {
  const std::string big(4096, 'z');
  const LookupKey lk(Slice(big), 7);
  EXPECT_EQ(big, lk.user_key().ToString());
}
