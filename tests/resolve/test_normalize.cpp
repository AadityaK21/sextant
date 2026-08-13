// Normalization, and the tension it is built around.
//
// Every rule here trades recall against precision, so the tests come in pairs:
// what the rule is supposed to merge, and what it must not.

#include "normalize.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "phonetic.h"

using namespace sextant::resolve;

namespace {

std::string Port(const std::string& raw) {
  return NormalizePortName(raw).canonical;
}

std::string Vessel(const std::string& raw) {
  return NormalizeVesselName(raw).canonical;
}

}  // namespace

// The example from the brief: five ways of writing one port, one canonical
// form.
TEST(Normalize, CollapsesTheWaysSourcesWriteOnePort) {
  const std::string expected = "ROTTERDAM";
  EXPECT_EQ(expected, Port("Rotterdam"));
  EXPECT_EQ(expected, Port("ROTTERDAM"));
  EXPECT_EQ(expected, Port("  rotterdam  "));
  EXPECT_EQ(expected, Port("Port of Rotterdam"));
  EXPECT_EQ(expected, Port("Rotterdam Harbour"));
  EXPECT_EQ(expected, Port("Rotterdam, Haven"));
}

TEST(Normalize, FoldsDiacriticsTheSameWayIngestDoes) {
  EXPECT_EQ("GOTEBORG", Port("Göteborg"));
  EXPECT_EQ("GOTEBORG", Port("Goteborg"));
  EXPECT_EQ("ARHUS", Port("Århus"));
  EXPECT_EQ("KOBENHAVN", Port("København"));
  EXPECT_EQ("GDANSK", Port("Gdańsk"));
  EXPECT_EQ("KLAIPEDA", Port("Klaipėda"));
  EXPECT_EQ("VALPARAISO", Port("Valparaíso"));
}

// UN/LOCODE writes both names for a bilingual place in one cell. Discarding
// either half would lose a real name for the port.
TEST(Normalize, KeepsParentheticalAndCommaSeparatedAlternatives) {
  const NormalizedName helsinki = NormalizePortName("Helsinki (Helsingfors)");
  EXPECT_EQ("HELSINKI HELSINGFORS", helsinki.canonical);
  EXPECT_EQ(std::vector<std::string>({"HELSINKI", "HELSINGFORS"}),
            helsinki.tokens);

  EXPECT_EQ("TURKU ABO", Port("Turku (Åbo)"));
}

TEST(Normalize, ExpandsAbbreviationsBeforeDroppingNoise) {
  // "St Petersburg" and "Saint Petersburg" are the same city, and no string
  // similarity measure would tell you that.
  EXPECT_EQ(Port("Saint Petersburg"), Port("St Petersburg"));
  EXPECT_EQ(Port("Saint Petersburg"), Port("St. Petersburg"));
  // Pt expands to Port, which is then dropped as noise - so both forms reach
  // the same place.
  EXPECT_EQ(Port("Port Klang"), Port("Pt Klang"));
}

TEST(Normalize, DropsNoiseTokens) {
  EXPECT_TRUE(IsPortNoiseToken("PORT"));
  EXPECT_TRUE(IsPortNoiseToken("HARBOUR"));
  EXPECT_TRUE(IsPortNoiseToken("TERMINAL"));
  EXPECT_FALSE(IsPortNoiseToken("ROTTERDAM"));
  EXPECT_FALSE(IsPortNoiseToken("BAY")) << "Bay is part of real port names";

  const NormalizedName name = NormalizePortName("Port of Rotterdam Terminal");
  EXPECT_EQ("ROTTERDAM", name.canonical);
  // The dropped tokens are kept as evidence rather than thrown away, because
  // the scorer wants to know one name had them and the other did not.
  EXPECT_EQ(3u, name.dropped.size());
}

// The known hazard, asserted rather than left as a comment: dropping PORT and
// OF really does reduce "Port of Spain" to "SPAIN". It is accepted because
// normalization only decides who gets COMPARED, and the scorer sees the
// country code and the coordinates.
TEST(Normalize, PortOfSpainCollapsesAndThatIsAcceptedDeliberately) {
  EXPECT_EQ("SPAIN", Port("Port of Spain"));
  // The point of documenting it: this is a blocking key, not a decision. Two
  // records reaching the same canonical form are candidates, nothing more.
  EXPECT_NE(Port("Port of Spain"), Port("Port Said"));
}

// A name made entirely of noise keeps its noise. Returning empty would drop the
// record out of every block, silently excluding it from resolution.
TEST(Normalize, ANameOfNothingButNoiseSurvives) {
  EXPECT_EQ("PORT", Port("Port"));
  EXPECT_EQ("THE HARBOUR", Port("The Harbour"));
  EXPECT_TRUE(Port("").empty());
  EXPECT_TRUE(Port("   ---   ").empty());
}

TEST(Normalize, VesselNamesDropTypePrefixesInstead) {
  EXPECT_EQ("BALTIC TRADER", Vessel("MV Baltic Trader"));
  EXPECT_EQ("BALTIC TRADER", Vessel("M/V Baltic Trader"));
  EXPECT_EQ("BALTIC TRADER", Vessel("MT BALTIC TRADER"));
  EXPECT_EQ("BALTIC TRADER", Vessel("baltic trader"));

  // A vessel called "Port Adelaide" must keep its Port - the port noise list
  // does not apply to hulls.
  EXPECT_EQ("PORT ADELAIDE", Vessel("Port Adelaide"));
  EXPECT_TRUE(IsVesselNoiseToken("MV"));
  EXPECT_FALSE(IsVesselNoiseToken("PORT"));
}

TEST(Tokenize, SplitsOnEverythingThatIsNotAlphanumeric) {
  EXPECT_EQ(std::vector<std::string>({"ROTTERDAM", "BOTLEK"}),
            TokenizeUpper("Rotterdam-Botlek"));
  EXPECT_EQ(std::vector<std::string>({"PORT", "2000"}), TokenizeUpper("Port 2000"));
  EXPECT_EQ(std::vector<std::string>({"O", "BRIEN"}), TokenizeUpper("O'Brien"));
  EXPECT_TRUE(TokenizeUpper("").empty());
  EXPECT_TRUE(TokenizeUpper("---").empty());
}

// --- Soundex ----------------------------------------------------------------

// The published American Soundex examples. These exist so the implementation is
// checked against the specification rather than against itself.
TEST(Soundex, MatchesThePublishedExamples) {
  EXPECT_EQ("R163", Soundex("Robert"));
  EXPECT_EQ("R163", Soundex("Rupert"));
  EXPECT_EQ("A261", Soundex("Ashcraft"));
  EXPECT_EQ("A261", Soundex("Ashcroft"));
  EXPECT_EQ("T522", Soundex("Tymczak"));
  EXPECT_EQ("P236", Soundex("Pfister"));
  EXPECT_EQ("H555", Soundex("Honeyman"));
}

TEST(Soundex, PadsAndTruncatesToFour) {
  EXPECT_EQ("L000", Soundex("Lee"));
  EXPECT_EQ("K530", Soundex("Kant"));
  EXPECT_EQ(4u, Soundex("Rotterdam").size());
  EXPECT_TRUE(Soundex("").empty());
  EXPECT_TRUE(Soundex("1234").empty());
}

TEST(Soundex, EncodesEachWordOfAPhrase) {
  const std::string phrase = SoundexPhrase("ROTTERDAM BOTLEK");
  EXPECT_EQ(SoundexPhrase("ROTTERDAM") + " " + SoundexPhrase("BOTLEK"), phrase);
  // A multi-word name must not lose everything after the first word.
  EXPECT_NE(SoundexPhrase("ROTTERDAM"), phrase);
}

// The limitation named in phonetic.h, asserted so the claim is not just a
// comment: Soundex keeps the first letter, so it cannot bring together two
// spellings that start differently.
TEST(Soundex, CannotSeePastTheFirstLetterAndThatIsTheKnownCost) {
  EXPECT_NE(Soundex("Gothenburg"), Soundex("Jothenburg"));
  // And it over-merges: two different ports with similar consonant skeletons
  // share a key, which costs a wasted comparison and nothing more.
  EXPECT_EQ(Soundex("Rotterdam"), Soundex("Rotherdam"));
}
