// TValue: typing, serialization and the two things that are easy to get wrong -
// order-preserving encoding and civil date arithmetic.

#include "value.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace sextant::ontology;

namespace {

TValue RoundTrip(const TValue& in) {
  std::string encoded;
  in.EncodeTo(&encoded);
  Slice slice(encoded);
  TValue out;
  EXPECT_TRUE(TValue::DecodeFrom(&slice, &out));
  EXPECT_TRUE(slice.empty()) << "decoder left " << slice.size() << " bytes behind";
  return out;
}

}  // namespace

TEST(TValue, EveryTypeRoundTrips) {
  EXPECT_EQ(TValue::Null(), RoundTrip(TValue::Null()));
  EXPECT_EQ(TValue::String("Rotterdam"), RoundTrip(TValue::String("Rotterdam")));
  EXPECT_EQ(TValue::Int(-42), RoundTrip(TValue::Int(-42)));
  EXPECT_EQ(TValue::Double(51.9225), RoundTrip(TValue::Double(51.9225)));
  EXPECT_EQ(TValue::Bool(true), RoundTrip(TValue::Bool(true)));
  EXPECT_EQ(TValue::Timestamp(1'743'465'600'000LL),
            RoundTrip(TValue::Timestamp(1'743'465'600'000LL)));
  EXPECT_EQ(TValue::StringList({"Europoort", "Maasvlakte"}),
            RoundTrip(TValue::StringList({"Europoort", "Maasvlakte"})));
}

// An int and a timestamp both hold an int64. If the discriminator were inferred
// from the payload rather than stored, these two would decode identically and
// the ontology could not tell a tonnage from an arrival time.
TEST(TValue, IntAndTimestampAreDistinguishable) {
  const TValue i = RoundTrip(TValue::Int(1'743'465'600'000LL));
  const TValue t = RoundTrip(TValue::Timestamp(1'743'465'600'000LL));
  EXPECT_EQ(ValueType::kInt, i.type());
  EXPECT_EQ(ValueType::kTimestamp, t.type());
  EXPECT_NE(i, t);
}

TEST(TValue, StringsSurviveEmbeddedNulsAndUtf8) {
  // Nine bytes with a NUL in the middle, then a two-byte UTF-8 codepoint.
  const std::string awkward = std::string("Go\0teborg", 9) + "\xC3\xA5";
  const TValue out = RoundTrip(TValue::String(awkward));
  EXPECT_EQ(awkward, out.AsString());
  EXPECT_EQ(11u, out.AsString().size())
      << "a length-prefixed encoding must not stop at the embedded NUL";
}

// Doubles are stored as their bit pattern, not as decimal text. A value that
// only round-trips through %g is a value the lineage test would flag as broken.
TEST(TValue, DoublesRoundTripExactly) {
  const double values[] = {0.0,
                           -0.0,
                           51.922500000000003,
                           1.0 / 3.0,
                           std::numeric_limits<double>::min(),
                           std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()};
  for (const double v : values) {
    const TValue out = RoundTrip(TValue::Double(v));
    const double back = out.AsDouble();
    EXPECT_EQ(0, std::memcmp(&v, &back, sizeof(double)))
        << "double " << v << " did not survive the round trip bit for bit";
  }
}

TEST(TValue, DecodeRejectsTruncatedAndUnknownInput) {
  std::string encoded;
  TValue::String("Rotterdam").EncodeTo(&encoded);
  for (size_t n = 1; n < encoded.size(); ++n) {
    Slice truncated(encoded.data(), n);
    TValue out;
    EXPECT_FALSE(TValue::DecodeFrom(&truncated, &out))
        << "a " << n << "-byte prefix was accepted";
  }

  const std::string bogus_tag(1, static_cast<char>(0x7F));
  Slice slice(bogus_tag);
  TValue out;
  EXPECT_FALSE(TValue::DecodeFrom(&slice, &out));
}

// --- ordered encoding -------------------------------------------------------

namespace {

std::string Ordered(const TValue& v) {
  std::string out;
  v.EncodeOrdered(&out);
  return out;
}

}  // namespace

TEST(TValue, ByteOrderEqualsNumericOrderForDoubles) {
  // Includes negatives, which sort backwards under a naive IEEE 754 comparison,
  // and both zeros. Longitudes west of Greenwich are negative, so this is the
  // path every lon index takes.
  const std::vector<double> ascending = {-180.0, -74.0333, -1.0,  -0.5, -0.0,
                                         0.0,    0.5,      4.4833, 51.9225, 180.0};
  for (size_t i = 1; i < ascending.size(); ++i) {
    const std::string lo = Ordered(TValue::Double(ascending[i - 1]));
    const std::string hi = Ordered(TValue::Double(ascending[i]));
    EXPECT_LE(lo, hi) << ascending[i - 1] << " did not sort below " << ascending[i];
  }
}

TEST(TValue, ByteOrderEqualsNumericOrderForTimestamps) {
  const std::vector<int64_t> ascending = {-2'208'988'800'000LL,  // 1900
                                          -1,
                                          0,
                                          1,
                                          1'743'465'600'000LL,
                                          4'102'444'800'000LL};
  for (size_t i = 1; i < ascending.size(); ++i) {
    EXPECT_LT(Ordered(TValue::Timestamp(ascending[i - 1])),
              Ordered(TValue::Timestamp(ascending[i])));
  }
}

TEST(TValue, NullAndListsClusterAtTheHeadOfAnIndex) {
  const std::string null_key = Ordered(TValue::Null());
  const std::string list_key = Ordered(TValue::StringList({"a", "b"}));
  const std::string real_key = Ordered(TValue::String("A"));
  EXPECT_EQ(null_key, list_key);
  EXPECT_LT(null_key, real_key)
      << "unorderable values must sort before real ones so a range scan steps"
         " over them once instead of testing for them at every key";
}

// --- dates ------------------------------------------------------------------

TEST(Iso8601, ParsesTheFormsTheSourcesActuallyEmit) {
  int64_t ms = 0;

  ASSERT_TRUE(ParseIso8601("1970-01-01T00:00:00Z", &ms));
  EXPECT_EQ(0, ms);

  ASSERT_TRUE(ParseIso8601("2026-04-01", &ms));
  EXPECT_EQ(1'775'001'600'000LL, ms);

  // Digitraffic reports Finnish local time with an offset. Getting the sign
  // wrong here would shift every arrival by the offset and quietly move calls
  // into the wrong quarter.
  int64_t utc = 0, helsinki = 0;
  ASSERT_TRUE(ParseIso8601("2026-04-03T04:15:00Z", &utc));
  ASSERT_TRUE(ParseIso8601("2026-04-03T07:15:00+03:00", &helsinki));
  EXPECT_EQ(utc, helsinki);

  int64_t behind = 0;
  ASSERT_TRUE(ParseIso8601("2026-04-02T23:15:00-05:00", &behind));
  ASSERT_TRUE(ParseIso8601("2026-04-03T04:15:00Z", &utc));
  EXPECT_EQ(utc, behind);

  // Fractional seconds, truncated rather than rounded so the value stays
  // monotone with the source.
  ASSERT_TRUE(ParseIso8601("2026-04-03T07:15:00.123456+03:00", &ms));
  EXPECT_EQ(helsinki + 123, ms);

  // A space instead of T, which Postgres emits.
  ASSERT_TRUE(ParseIso8601("2026-04-03 07:15:00", &ms));
  EXPECT_EQ(utc + 3 * 3600 * 1000, ms);
}

TEST(Iso8601, RejectsThingsThatAreNotTimestamps) {
  int64_t ms = 0;
  EXPECT_FALSE(ParseIso8601("", &ms));
  EXPECT_FALSE(ParseIso8601("not a timestamp", &ms));
  EXPECT_FALSE(ParseIso8601("2026-13-01", &ms));      // month 13
  EXPECT_FALSE(ParseIso8601("2026-04-32", &ms));      // day 32
  EXPECT_FALSE(ParseIso8601("2026-4-1", &ms));        // not zero padded
  EXPECT_FALSE(ParseIso8601("2026-04-01T25:00:00Z", &ms));
  EXPECT_FALSE(ParseIso8601("2026-04-01T00:00:00Z junk", &ms));
}

TEST(Iso8601, FormatsBackToWhatItParsed) {
  const char* samples[] = {"1970-01-01T00:00:00.000Z", "2026-04-01T00:00:00.000Z",
                           "1969-07-20T20:17:40.000Z", "2262-04-11T23:47:16.854Z"};
  for (const char* s : samples) {
    int64_t ms = 0;
    ASSERT_TRUE(ParseIso8601(s, &ms)) << s;
    EXPECT_EQ(s, FormatIso8601(ms));
  }
}

// Timestamps before 1970 are negative, and integer division truncates towards
// zero rather than flooring. Getting this wrong puts a 1969 date on the wrong
// day, which is the sort of bug that only ever shows up in one row of a
// historical dataset.
TEST(Iso8601, HandlesNegativeEpochsWithoutSlippingADay) {
  int64_t ms = 0;
  ASSERT_TRUE(ParseIso8601("1969-12-31T23:59:59.500Z", &ms));
  EXPECT_LT(ms, 0);
  EXPECT_EQ("1969-12-31T23:59:59.500Z", FormatIso8601(ms));
}

TEST(CivilDates, AgreeWithKnownDayNumbers) {
  EXPECT_EQ(0, DaysFromCivil(1970, 1, 1));
  EXPECT_EQ(-1, DaysFromCivil(1969, 12, 31));
  EXPECT_EQ(59, DaysFromCivil(1970, 3, 1));
  // 2000 is a leap year, 1900 and 2100 are not - the rule most hand-rolled
  // date code gets wrong.
  EXPECT_EQ(DaysFromCivil(2000, 3, 1), DaysFromCivil(2000, 2, 29) + 1);
  EXPECT_EQ(DaysFromCivil(1900, 3, 1), DaysFromCivil(1900, 2, 28) + 1);
  EXPECT_EQ(DaysFromCivil(2100, 3, 1), DaysFromCivil(2100, 2, 28) + 1);
}

TEST(CivilDates, RoundTripOverThreeCenturies) {
  for (int64_t day = DaysFromCivil(1900, 1, 1); day < DaysFromCivil(2200, 1, 1);
       day += 7) {
    int64_t y;
    unsigned m, d;
    CivilFromDays(day, &y, &m, &d);
    ASSERT_EQ(day, DaysFromCivil(y, m, d)) << "day " << day;
  }
}
