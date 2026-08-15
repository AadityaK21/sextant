// Similarity measures, checked against published values.
//
// These exist so the implementations are verified against the specification
// rather than against themselves. An encoding that only agrees with its own
// decoder proves nothing.

#include "similarity.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sextant::resolve;

// The canonical Jaro and Jaro-Winkler examples from the record-linkage
// literature.
TEST(Jaro, MatchesPublishedValues) {
  EXPECT_NEAR(0.9444, Jaro("MARTHA", "MARHTA"), 0.0005);
  EXPECT_NEAR(0.7667, Jaro("DIXON", "DICKSONX"), 0.0005);
  EXPECT_NEAR(0.8963, Jaro("JELLYFISH", "SMELLYFISH"), 0.0005);
}

TEST(JaroWinkler, MatchesPublishedValues) {
  // Jaro plus the prefix bonus: 3 shared characters for MARTHA/MARHTA, 2 for
  // DIXON/DICKSONX, 0 for JELLYFISH/SMELLYFISH.
  EXPECT_NEAR(0.9611, JaroWinkler("MARTHA", "MARHTA"), 0.0005);
  EXPECT_NEAR(0.8133, JaroWinkler("DIXON", "DICKSONX"), 0.0005);
  EXPECT_NEAR(0.8963, JaroWinkler("JELLYFISH", "SMELLYFISH"), 0.0005);
}

TEST(Jaro, EdgeCases) {
  EXPECT_DOUBLE_EQ(1.0, Jaro("", ""));
  EXPECT_DOUBLE_EQ(0.0, Jaro("ROTTERDAM", ""));
  EXPECT_DOUBLE_EQ(1.0, Jaro("ROTTERDAM", "ROTTERDAM"));
  EXPECT_DOUBLE_EQ(0.0, Jaro("ABC", "XYZ"));
  // Symmetric, which a windowed measure is not guaranteed to be by accident.
  EXPECT_NEAR(Jaro("ROTTERDAM", "ROTHERDAM"), Jaro("ROTHERDAM", "ROTTERDAM"),
              1e-12);
}

// The prefix bonus is only applied above a similarity floor. Without it, two
// unrelated names sharing a first letter get pulled upwards for no reason.
TEST(JaroWinkler, DoesNotBoostDissimilarPairs) {
  const std::string a = "ROTTERDAM";
  const std::string b = "RIO DE JANEIRO";
  EXPECT_DOUBLE_EQ(Jaro(a, b), JaroWinkler(a, b))
      << "a pair below the floor must get no prefix bonus";
  EXPECT_LT(JaroWinkler(a, b), 0.7);
}

// The case Jaro-Winkler is weak at, and the reason Jaccard is a separate
// feature rather than an alternative.
TEST(TokenJaccard, IsOrderIndependent) {
  const std::vector<std::string> a = {"PORT", "KLANG", "WESTPORT"};
  const std::vector<std::string> b = {"WESTPORT", "PORT", "KLANG"};
  EXPECT_DOUBLE_EQ(1.0, TokenJaccard(a, b));
  EXPECT_LT(JaroWinkler("PORT KLANG WESTPORT", "WESTPORT PORT KLANG"), 0.9)
      << "a character-level measure sees two quite different strings";
}

TEST(TokenJaccard, CountsIntersectionOverUnion) {
  EXPECT_DOUBLE_EQ(0.5, TokenJaccard({"ROTTERDAM"}, {"ROTTERDAM", "BOTLEK"}));
  EXPECT_DOUBLE_EQ(1.0, TokenJaccard({"A", "B"}, {"B", "A"}));
  EXPECT_DOUBLE_EQ(0.0, TokenJaccard({"A"}, {"B"}));
  EXPECT_DOUBLE_EQ(1.0, TokenJaccard({}, {}));
  EXPECT_DOUBLE_EQ(0.0, TokenJaccard({"A"}, {}));
  // Duplicates in the input are sets, not bags.
  EXPECT_DOUBLE_EQ(1.0, TokenJaccard({"A", "A"}, {"A"}));
}

// Containment is a better signal than the ratio for port sub-locations:
// Jaccard scores "Rotterdam" against "Rotterdam Botlek" only 0.5 because it
// counts the extra token against them.
TEST(TokensContained, RecognisesSubLocations) {
  EXPECT_TRUE(TokensContained({"ROTTERDAM"}, {"ROTTERDAM", "BOTLEK"}));
  EXPECT_TRUE(TokensContained({"ROTTERDAM", "BOTLEK"}, {"ROTTERDAM"}))
      << "containment is symmetric - the shorter list is the one tested";
  EXPECT_FALSE(TokensContained({"EUROPOORT"}, {"ROTTERDAM", "BOTLEK"}));
  EXPECT_FALSE(TokensContained({}, {"ROTTERDAM"}));
  EXPECT_TRUE(TokensContained({"A"}, {"A"}));
}
