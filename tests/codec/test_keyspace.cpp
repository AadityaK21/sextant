// The eleven keyspaces.
//
// Two things are tested for each: that keys round-trip, and - more importantly
// - that the ORDERING the layout was designed to produce actually holds. A
// keyspace whose keys decode correctly but sort wrongly turns every range scan
// into a silent wrong answer.

#include "keyspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace sextant::codec;

namespace {

Ulid MakeUlid(uint64_t ts, uint64_t lo) { return Ulid::FromParts(ts, 0, lo); }

const Ulid kRotterdam = MakeUlid(1000, 1);
const Ulid kHamburg = MakeUlid(1000, 2);
const Ulid kVoyageA = MakeUlid(2000, 10);
const Ulid kVoyageB = MakeUlid(2000, 11);

constexpr TypeId kPortType = 1;
constexpr TypeId kVesselType = 2;
constexpr LinkTypeId kArrivesAt = 7;
constexpr LinkTypeId kDepartsFrom = 8;
constexpr PropId kLocode = 3;
constexpr PropId kLatitude = 4;

}  // namespace

// --- every keyspace has a distinct one-byte prefix --------------------------

TEST(Keyspace, PrefixesAreDistinct) {
  const std::vector<std::string> keys = {
      EncodeRawKey(1, 2, 3),
      EncodeSourceRecordKey(1, 2),
      EncodeEntityKey(kPortType, kRotterdam),
      EncodeLinkOutKey(kVoyageA, kArrivesAt, kRotterdam),
      EncodeLinkInKey(kRotterdam, kArrivesAt, kVoyageA),
      EncodeProvenanceKey(kRotterdam, kLocode, 1),
      EncodeCrossRefKey(1, 2),
      EncodeBlockingKey(1, 2, 3),
      EncodeIndexKeyString(kPortType, kLocode, Slice("NLRTM"), kRotterdam),
      EncodeTimeIndexKey(kArrivesAt, kRotterdam, 1000, kVoyageA),
      EncodeCandidateKey(5.0, 42),
  };

  std::vector<uint8_t> prefixes;
  for (const auto& k : keys) prefixes.push_back(static_cast<uint8_t>(k[0]));

  std::sort(prefixes.begin(), prefixes.end());
  EXPECT_EQ(prefixes.end(), std::adjacent_find(prefixes.begin(), prefixes.end()))
      << "two keyspaces share a prefix - their key ranges would interleave";
  EXPECT_EQ(11u, prefixes.size());
}

// --- round trips ------------------------------------------------------------

TEST(Keyspace, RawRoundTrips) {
  const std::string key = EncodeRawKey(7, 12345, 999);
  SourceId source; BatchId batch; RowSeq row;
  ASSERT_TRUE(DecodeRawKey(Slice(key), &source, &batch, &row));
  EXPECT_EQ(7u, source);
  EXPECT_EQ(12345u, batch);
  EXPECT_EQ(999u, row);
}

TEST(Keyspace, EntityRoundTrips) {
  const std::string key = EncodeEntityKey(kPortType, kRotterdam);
  TypeId type; Ulid id;
  ASSERT_TRUE(DecodeEntityKey(Slice(key), &type, &id));
  EXPECT_EQ(kPortType, type);
  EXPECT_EQ(kRotterdam, id);
}

TEST(Keyspace, LinksRoundTripBothDirections) {
  {
    const std::string key = EncodeLinkOutKey(kVoyageA, kArrivesAt, kRotterdam);
    Ulid src, dst; LinkTypeId type;
    ASSERT_TRUE(DecodeLinkOutKey(Slice(key), &src, &type, &dst));
    EXPECT_EQ(kVoyageA, src);
    EXPECT_EQ(kArrivesAt, type);
    EXPECT_EQ(kRotterdam, dst);
  }
  {
    const std::string key = EncodeLinkInKey(kRotterdam, kArrivesAt, kVoyageA);
    Ulid src, dst; LinkTypeId type;
    ASSERT_TRUE(DecodeLinkInKey(Slice(key), &dst, &type, &src));
    EXPECT_EQ(kRotterdam, dst);
    EXPECT_EQ(kArrivesAt, type);
    EXPECT_EQ(kVoyageA, src);
  }
}

TEST(Keyspace, ProvenanceRoundTrips) {
  const std::string key = EncodeProvenanceKey(kRotterdam, kLocode, 987654321);
  Ulid entity; PropId prop; uint64_t version;
  ASSERT_TRUE(DecodeProvenanceKey(Slice(key), &entity, &prop, &version));
  EXPECT_EQ(kRotterdam, entity);
  EXPECT_EQ(kLocode, prop);
  EXPECT_EQ(987654321u, version);
}

TEST(Keyspace, TimeIndexRoundTrips) {
  const std::string key = EncodeTimeIndexKey(kArrivesAt, kRotterdam, -86400000, kVoyageA);
  LinkTypeId link; Ulid anchor, entity; int64_t ts;
  ASSERT_TRUE(DecodeTimeIndexKey(Slice(key), &link, &anchor, &ts, &entity));
  EXPECT_EQ(kArrivesAt, link);
  EXPECT_EQ(kRotterdam, anchor);
  EXPECT_EQ(-86400000, ts) << "pre-epoch timestamps must survive";
  EXPECT_EQ(kVoyageA, entity);
}

TEST(Keyspace, CandidateRoundTrips) {
  const std::string key = EncodeCandidateKey(4.75, 0xDEADBEEF);
  double score; uint64_t hash;
  ASSERT_TRUE(DecodeCandidateKey(Slice(key), &score, &hash));
  EXPECT_DOUBLE_EQ(4.75, score);
  EXPECT_EQ(0xDEADBEEFu, hash);
}

TEST(Keyspace, DecodersRejectWrongKeyspace) {
  const std::string entity_key = EncodeEntityKey(kPortType, kRotterdam);
  SourceId source; BatchId batch; RowSeq row;
  EXPECT_FALSE(DecodeRawKey(Slice(entity_key), &source, &batch, &row))
      << "a decoder must not accept a key from another keyspace";
}

TEST(Keyspace, DecodersRejectTruncatedKeys) {
  std::string key = EncodeEntityKey(kPortType, kRotterdam);
  key.resize(key.size() - 1);
  TypeId type; Ulid id;
  EXPECT_FALSE(DecodeEntityKey(Slice(key), &type, &id));
}

// --- the orderings the layouts exist to produce -----------------------------

// A prefix scan over LINKOUT must return exactly one entity's links of one
// type, contiguously. This is the entire graph traversal engine.
TEST(Keyspace, LinkOutKeysGroupBySourceThenType) {
  const std::string a = EncodeLinkOutKey(kVoyageA, kArrivesAt, kRotterdam);
  const std::string b = EncodeLinkOutKey(kVoyageA, kArrivesAt, kHamburg);
  const std::string c = EncodeLinkOutKey(kVoyageA, kDepartsFrom, kRotterdam);
  const std::string d = EncodeLinkOutKey(kVoyageB, kArrivesAt, kRotterdam);

  const std::string prefix = LinkOutPrefix(kVoyageA, kArrivesAt);

  // Both arrives_at links of voyage A share the prefix.
  EXPECT_EQ(0, a.compare(0, prefix.size(), prefix));
  EXPECT_EQ(0, b.compare(0, prefix.size(), prefix));
  // A different link type does not.
  EXPECT_NE(0, c.compare(0, prefix.size(), prefix));
  // Nor does a different source entity.
  EXPECT_NE(0, d.compare(0, prefix.size(), prefix));

  // And everything sharing the prefix is contiguous: nothing outside the group
  // sorts between two members of it.
  const std::string upper = PrefixUpperBound(prefix);
  EXPECT_LT(a, upper);
  EXPECT_LT(b, upper);
  EXPECT_GE(c, upper);
}

// THE headline claim: a time window is one contiguous byte range.
TEST(Keyspace, TimeIndexKeysSortChronologicallyWithinAnAnchor) {
  std::vector<int64_t> times = {-1000, -1, 0, 1, 1000, 1'700'000'000'000LL};

  std::vector<std::string> keys;
  for (int64_t t : times) {
    keys.push_back(EncodeTimeIndexKey(kArrivesAt, kRotterdam, t, kVoyageA));
  }

  for (size_t i = 1; i < keys.size(); ++i) {
    EXPECT_LT(keys[i - 1], keys[i])
        << "time ordering broke between " << times[i - 1] << " and " << times[i];
  }
}

TEST(Keyspace, TimeWindowIsAContiguousRange) {
  // Build keys across a wide span, then check that exactly the ones inside the
  // window fall between the two bounds - no filtering required.
  const int64_t kFrom = 1000;
  const int64_t kTo = 2000;

  const std::string lower = TimeIndexBound(kArrivesAt, kRotterdam, kFrom);
  const std::string upper = TimeIndexBound(kArrivesAt, kRotterdam, kTo);

  const std::vector<int64_t> inside = {1000, 1001, 1500, 1999};
  const std::vector<int64_t> outside = {-5000, 0, 999, 2000, 2001, 99999};

  for (int64_t t : inside) {
    const std::string k = EncodeTimeIndexKey(kArrivesAt, kRotterdam, t, kVoyageA);
    EXPECT_GE(k, lower) << "t=" << t << " should be inside the window";
    EXPECT_LT(k, upper) << "t=" << t << " should be inside the window";
  }
  for (int64_t t : outside) {
    const std::string k = EncodeTimeIndexKey(kArrivesAt, kRotterdam, t, kVoyageA);
    EXPECT_TRUE(k < lower || k >= upper) << "t=" << t << " leaked into the window";
  }
}

TEST(Keyspace, TimeIndexSeparatesAnchors) {
  // A window on Rotterdam must never pick up Hamburg's voyages, whatever the
  // timestamps are.
  const std::string lower = TimeIndexBound(kArrivesAt, kRotterdam, 0);
  const std::string upper = TimeIndexBound(kArrivesAt, kRotterdam, 1'000'000);

  const std::string hamburg =
      EncodeTimeIndexKey(kArrivesAt, kHamburg, 500, kVoyageA);
  EXPECT_TRUE(hamburg < lower || hamburg >= upper);
}

// Index keys must sort by VALUE, which is what makes a range query possible.
TEST(Keyspace, IndexKeysSortByValueNotByEntityId) {
  // Deliberately pair the smallest value with the largest entity id, so a
  // layout that sorted by id would fail this.
  const std::string low_value_high_id =
      EncodeIndexKeyString(kPortType, kLocode, Slice("AAAAA"), MakeUlid(9999, 9999));
  const std::string high_value_low_id =
      EncodeIndexKeyString(kPortType, kLocode, Slice("ZZZZZ"), MakeUlid(1, 1));

  EXPECT_LT(low_value_high_id, high_value_low_id);
}

TEST(Keyspace, NumericIndexSupportsRangeScans) {
  // Latitudes spanning the equator: the case that breaks naive IEEE 754 keys.
  const std::vector<double> lats = {-45.0, -0.5, 0.0, 0.5, 51.9225, 60.0};

  std::vector<std::string> keys;
  for (double lat : lats) {
    keys.push_back(EncodeIndexKeyDouble(kPortType, kLatitude, lat, kRotterdam));
  }
  for (size_t i = 1; i < keys.size(); ++i) {
    EXPECT_LT(keys[i - 1], keys[i]) << "latitude index misordered at " << lats[i];
  }

  // A bound between two values separates them cleanly.
  const std::string bound = IndexBoundDouble(kPortType, kLatitude, 0.25);
  EXPECT_LT(keys[2], bound);  // 0.0
  EXPECT_GT(keys[3], bound);  // 0.5
}

TEST(Keyspace, IndexKeyEntityIsRecoverable) {
  // The value is variable-length, so the entity id has to be read from the end.
  const std::string key = EncodeIndexKeyString(
      kPortType, kLocode, Slice(std::string("odd\0value", 9)), kRotterdam);

  Ulid entity;
  ASSERT_TRUE(DecodeIndexKeyEntity(Slice(key), &entity));
  EXPECT_EQ(kRotterdam, entity);
}

TEST(Keyspace, IndexSeparatesPropertiesAndTypes) {
  const std::string a =
      EncodeIndexKeyString(kPortType, kLocode, Slice("X"), kRotterdam);
  const std::string b =
      EncodeIndexKeyString(kPortType, kLatitude, Slice("X"), kRotterdam);
  const std::string c =
      EncodeIndexKeyString(kVesselType, kLocode, Slice("X"), kRotterdam);

  const std::string prefix = IndexPrefix(kPortType, kLocode);
  EXPECT_EQ(0, a.compare(0, prefix.size(), prefix));
  EXPECT_NE(0, b.compare(0, prefix.size(), prefix));
  EXPECT_NE(0, c.compare(0, prefix.size(), prefix));
}

// Candidates are stored with a negated score so a forward scan yields the
// pairs closest to the decision boundary first.
TEST(Keyspace, CandidatesScanHighestScoreFirst) {
  const std::string high = EncodeCandidateKey(4.9, 1);
  const std::string mid = EncodeCandidateKey(3.0, 2);
  const std::string low = EncodeCandidateKey(2.1, 3);

  EXPECT_LT(high, mid);
  EXPECT_LT(mid, low);
}

TEST(Keyspace, EntityKeysSortByCreationTimeWithinAType) {
  const std::string older = EncodeEntityKey(kPortType, MakeUlid(1000, 0));
  const std::string newer = EncodeEntityKey(kPortType, MakeUlid(2000, 0));
  EXPECT_LT(older, newer)
      << "ULID ids should make an entity scan yield creation order for free";
}

// --- prefix bound arithmetic ------------------------------------------------

TEST(PrefixUpperBound, IncrementsTheLastByte) {
  EXPECT_EQ(std::string("ab\x02", 3), PrefixUpperBound(std::string("ab\x01", 3)));
}

TEST(PrefixUpperBound, CarriesPastTrailingFF) {
  EXPECT_EQ(std::string("b"), PrefixUpperBound(std::string("a\xFF", 2)));
  EXPECT_EQ(std::string("b"), PrefixUpperBound(std::string("a\xFF\xFF", 3)));
}

TEST(PrefixUpperBound, AllFFHasNoSuccessor) {
  // An empty bound means "scan to the end", which is the correct answer when
  // the prefix is the largest possible.
  EXPECT_TRUE(PrefixUpperBound(std::string("\xFF\xFF", 2)).empty());
}

TEST(Keyspace, NamesAreReportedForDiagnostics) {
  EXPECT_STREQ("ENTITY", KeyspaceName(KeyspaceOf(
      Slice(EncodeEntityKey(kPortType, kRotterdam)))));
  EXPECT_STREQ("TIDX", KeyspaceName(KeyspaceOf(
      Slice(EncodeTimeIndexKey(kArrivesAt, kRotterdam, 0, kVoyageA)))));
}
