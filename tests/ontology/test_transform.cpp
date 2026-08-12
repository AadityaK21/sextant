// The transform registry.
//
// Two kinds of test here. The first is ordinary: each transform does what it
// says. The second is the one that matters - the properties the lineage
// round-trip depends on. Transforms must be pure, ids must be stable, and a
// chain must be replayable from ids alone.

#include "transform.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "value.h"

using namespace sextant::ontology;

namespace {

class TransformTest : public ::testing::Test {
 protected:
  TransformRegistry registry_;

  TValue Run(const std::vector<std::string>& names, const std::string& input,
             std::string* error = nullptr) {
    std::vector<TransformId> chain;
    std::string resolve_error;
    EXPECT_TRUE(registry_.ResolveChain(names, &chain, &resolve_error))
        << resolve_error;
    std::string local;
    TValue out = registry_.Apply(chain, TValue::String(input),
                                 error == nullptr ? &local : error);
    return out;
  }

  std::string Text(const std::vector<std::string>& names,
                   const std::string& input) {
    return Run(names, input).ToDisplay();
  }
};

}  // namespace

// --- string shaping ---------------------------------------------------------

TEST_F(TransformTest, WhitespaceShaping) {
  EXPECT_EQ("Rotterdam", Text({"trim"}, "  Rotterdam \t\n"));
  EXPECT_EQ("Port of Rotterdam", Text({"collapse_ws"}, "Port   of\t\tRotterdam"));
  EXPECT_EQ("Port of Rotterdam",
            Text({"trim", "collapse_ws"}, "  Port   of  Rotterdam  "));
  EXPECT_EQ("", Text({"trim"}, "   "));
}

TEST_F(TransformTest, CaseFolding) {
  EXPECT_EQ("ROTTERDAM", Text({"upper"}, "Rotterdam"));
  EXPECT_EQ("rotterdam", Text({"lower"}, "ROTTERDAM"));
  // Non-ASCII is left alone rather than mangled: case folding outside ASCII is
  // locale-dependent, and locale is hidden state.
  EXPECT_EQ("G\xC3\xB6TEBORG", Text({"upper"}, "G\xC3\xB6teborg"));
}

TEST_F(TransformTest, TitleCase) {
  EXPECT_EQ("Rotterdam", Text({"title_case"}, "ROTTERDAM"));
  EXPECT_EQ("New York", Text({"title_case"}, "NEW YORK"));
  EXPECT_EQ("Saint-Nazaire", Text({"title_case"}, "SAINT-NAZAIRE"));
  EXPECT_EQ("O'Brien Wharf", Text({"title_case"}, "O'BRIEN WHARF"));
  // Minor words are lowercased inside a name and capitalised at the start.
  EXPECT_EQ("Port of Rotterdam", Text({"title_case"}, "PORT OF ROTTERDAM"));
  EXPECT_EQ("Rio de Janeiro", Text({"title_case"}, "RIO DE JANEIRO"));
  EXPECT_EQ("De Haven", Text({"title_case"}, "DE HAVEN"));
}

TEST_F(TransformTest, StripDiacritics) {
  EXPECT_EQ("Goteborg", Text({"strip_diacritics"}, "G\xC3\xB6teborg"));
  EXPECT_EQ("Arhus", Text({"strip_diacritics"}, "\xC3\x85rhus"));
  EXPECT_EQ("Valparaiso", Text({"strip_diacritics"}, "Valpara\xC3\xADso"));
  EXPECT_EQ("Turku (Abo)", Text({"strip_diacritics"}, "Turku (\xC3\x85""bo)"));
  // Multi-character expansions.
  EXPECT_EQ("AErosund", Text({"strip_diacritics"}, "\xC3\x86rosund"));
  EXPECT_EQ("strasse", Text({"strip_diacritics"}, "stra\xC3\x9F""e"));
  // Latin Extended-A, which is where the Central European port names live.
  EXPECT_EQ("Gdansk", Text({"strip_diacritics"}, "Gda\xC5\x84sk"));
  EXPECT_EQ("Swinoujscie",
            Text({"strip_diacritics"}, "\xC5\x9Awinouj\xC5\x9B""cie"));
  // A script the table does not cover survives intact rather than vanishing.
  EXPECT_EQ("\xD0\x9C\xD1\x83\xD1\x80\xD0\xBC\xD0\xB0\xD0\xBD\xD1\x81\xD0\xBA",
            Text({"strip_diacritics"},
                 "\xD0\x9C\xD1\x83\xD1\x80\xD0\xBC\xD0\xB0\xD0\xBD\xD1\x81\xD0\xBA"));
}

TEST_F(TransformTest, ListsAndScalars) {
  const TValue split = Run({"split_semicolon"}, "Europoort; Maasvlakte ;Botlek");
  ASSERT_EQ(ValueType::kStringList, split.type());
  EXPECT_EQ(std::vector<std::string>({"Europoort", "Maasvlakte", "Botlek"}),
            split.AsStringList());

  // String transforms map elementwise over a list, so a mapping does not have
  // to write the chain twice for a list-valued property.
  std::vector<TransformId> chain;
  std::string err;
  ASSERT_TRUE(registry_.ResolveChain({"upper"}, &chain, &err));
  const TValue upper = registry_.Apply(chain, split, &err);
  EXPECT_EQ(std::vector<std::string>({"EUROPOORT", "MAASVLAKTE", "BOTLEK"}),
            upper.AsStringList());

  // An empty list is a null, not an empty list: "the source had nothing here".
  EXPECT_TRUE(Run({"split_semicolon"}, " ; ; ").IsNull());
}

TEST_F(TransformTest, ConcatJoinsMultipleColumns) {
  std::vector<TransformId> chain;
  std::string err;
  ASSERT_TRUE(registry_.ResolveChain({"concat", "upper"}, &chain, &err));
  const TValue out =
      registry_.Apply(chain, TValue::StringList({"NL", "RTM"}), &err);
  EXPECT_EQ("NLRTM", out.AsString());
}

TEST_F(TransformTest, FirstCharAndNullIfBlank) {
  EXPECT_EQ("L", Text({"first_char"}, "Large"));
  EXPECT_EQ("V", Text({"first_char"}, "very small"));
  EXPECT_TRUE(Run({"first_char"}, "").IsNull());
  EXPECT_TRUE(Run({"null_if_blank"}, "   ").IsNull());
  EXPECT_EQ("x", Text({"null_if_blank"}, "x"));
}

// --- conversion -------------------------------------------------------------

TEST_F(TransformTest, NumericParsing) {
  EXPECT_EQ(ValueType::kInt, Run({"to_int"}, "  42 ").type());
  EXPECT_EQ(42, Run({"to_int"}, "  42 ").AsInt());
  EXPECT_EQ(-7, Run({"to_int"}, "-7").AsInt());
  EXPECT_DOUBLE_EQ(51.9225, Run({"to_double"}, "51.9225").AsDouble());
  EXPECT_DOUBLE_EQ(-74.0333, Run({"to_double"}, "-74.0333").AsDouble());

  // A blank cell is a null, not a failure. Real exports are full of them and
  // reporting each one would drown the genuine problems.
  std::string error;
  EXPECT_TRUE(Run({"to_int"}, "", &error).IsNull());
  EXPECT_TRUE(error.empty());

  // A non-blank cell that is not a number IS a failure, with a reason.
  EXPECT_TRUE(Run({"to_int"}, "N/A", &error).IsNull());
  EXPECT_NE(std::string::npos, error.find("to_int"));
  EXPECT_NE(std::string::npos, error.find("N/A"));

  EXPECT_TRUE(Run({"to_double"}, "51.9 degrees", &error).IsNull());
  EXPECT_FALSE(error.empty()) << "trailing junk must not be silently ignored";
}

// --- maritime domain rules --------------------------------------------------

TEST_F(TransformTest, LocodeValidation) {
  EXPECT_EQ("NLRTM", Text({"validate_locode"}, "nlrtm"));
  EXPECT_EQ("NLRTM", Text({"validate_locode"}, " NL RTM "));
  EXPECT_EQ("US2N9", Text({"validate_locode"}, "US2N9"));

  std::string error;
  EXPECT_TRUE(Run({"validate_locode"}, "XX", &error).IsNull());
  EXPECT_NE(std::string::npos, error.find("validate_locode"));
  // 0 and 1 are excluded from the location part by the standard, for the same
  // reason Crockford base32 excludes them.
  EXPECT_TRUE(Run({"validate_locode"}, "NLRT0").IsNull());
  EXPECT_TRUE(Run({"validate_locode"}, "NLRT1").IsNull());
  EXPECT_TRUE(Run({"validate_locode"}, "1LRTM").IsNull());
  EXPECT_TRUE(Run({"validate_locode"}, "NLRTMX").IsNull());
  // Blank stays a quiet null: most World Port Index rows have no code at all.
  EXPECT_TRUE(Run({"validate_locode"}, "", &error).IsNull());
  EXPECT_TRUE(error.empty());
}

TEST(ImoChecksum, MatchesTheStandardWeighting) {
  // 7,6,5,4,3,2 against the first six digits; the last digit of the sum is the
  // check digit.
  EXPECT_TRUE(ImoChecksumValid("9074729"));
  EXPECT_TRUE(ImoChecksumValid("9321483"));
  EXPECT_TRUE(ImoChecksumValid("9345673"));
  EXPECT_TRUE(ImoChecksumValid("9111113"));
  EXPECT_TRUE(ImoChecksumValid("9501239"));
  EXPECT_TRUE(ImoChecksumValid("9700005"));

  EXPECT_FALSE(ImoChecksumValid(""));
  EXPECT_FALSE(ImoChecksumValid("907472"));
  EXPECT_FALSE(ImoChecksumValid("90747290"));
  EXPECT_FALSE(ImoChecksumValid("IMO90747"));
}

// The IMO check digit does NOT catch every single-digit typo, and the exact
// pattern of what it misses is a property of the weights.
//
// A change of d in the digit at weight w shifts the weighted sum by w*d, and is
// invisible when w*d is a multiple of 10. Weights 7 and 3 are coprime to 10, so
// those two positions catch everything. Weights 6, 4 and 2 share a factor of 2
// and miss a shift of five; weight 5 shares a factor of 5 and misses every even
// shift, which is four of the nine possible typos in that position.
//
// This matters beyond arithmetic. The resolver treats an IMO match as strong
// evidence of identity, and it is worth knowing that a transcription error can
// still produce a number that validates - so the check digit narrows the space,
// it does not close it.
TEST(ImoChecksum, MissesExactlyTheErrorsItsWeightsCannotSee) {
  // Positions 1 to 6 carry these weights; position 7 is the check digit itself,
  // where any change breaks the comparison directly.
  const int weights[7] = {7, 6, 5, 4, 3, 2, 1};
  const std::string good = "9074729";
  ASSERT_TRUE(ImoChecksumValid(good));

  int caught = 0, missed = 0;
  for (size_t i = 0; i < good.size(); ++i) {
    for (char c = '0'; c <= '9'; ++c) {
      if (c == good[i]) continue;
      std::string bad = good;
      bad[i] = c;

      const int delta = c - good[i];
      const bool should_catch = (weights[i] * delta) % 10 != 0;
      EXPECT_EQ(should_catch, !ImoChecksumValid(bad))
          << bad << " (position " << i + 1 << ", weight " << weights[i] << ")";
      if (should_catch) ++caught; else ++missed;
    }
  }

  // Seven positions, nine possible typos each. For this particular number the
  // weight-5 position loses four (its even shifts) and the weight-6, weight-4
  // and weight-2 positions lose one apiece (a shift of five, where the digit
  // allows it). The weight-7 and weight-3 positions and the check digit lose
  // none.
  EXPECT_EQ(63, caught + missed);
  EXPECT_EQ(7, missed);
  EXPECT_EQ(56, caught);
}

TEST_F(TransformTest, ImoTransforms) {
  EXPECT_EQ("9074729", Text({"strip_imo_prefix", "validate_imo"}, "IMO 9074729"));
  EXPECT_EQ("9074729", Text({"strip_imo_prefix", "validate_imo"}, "imo9074729"));

  std::string error;
  EXPECT_TRUE(Run({"validate_imo"}, "9074720", &error).IsNull());
  EXPECT_NE(std::string::npos, error.find("check digit"));
}

TEST(MidTable, MapsTheFlagsThisProjectActuallySees) {
  EXPECT_EQ("FI", MidToCountryCode(230));
  EXPECT_EQ("NL", MidToCountryCode(244));
  EXPECT_EQ("DE", MidToCountryCode(211));
  EXPECT_EQ("SE", MidToCountryCode(265));
  EXPECT_EQ("GB", MidToCountryCode(232));
  EXPECT_EQ("US", MidToCountryCode(366));
  // The two big open registries, both of which hold several MIDs.
  EXPECT_EQ("PA", MidToCountryCode(351));
  EXPECT_EQ("PA", MidToCountryCode(373));
  EXPECT_EQ("LR", MidToCountryCode(636));
  EXPECT_EQ("LR", MidToCountryCode(637));
  EXPECT_EQ("SG", MidToCountryCode(563));
  // Unassigned.
  EXPECT_EQ("", MidToCountryCode(999));
  EXPECT_EQ("", MidToCountryCode(100));
}

TEST_F(TransformTest, MidToCountryIsQuietAboutJunk) {
  EXPECT_EQ("FI", Text({"mid_to_country"}, "230123456"));

  // A malformed MMSI is common in AIS and is a null, not an error: the flag is
  // a weak hint to the resolver, and filling the lineage with complaints about
  // it would bury the failures that matter.
  std::string error;
  EXPECT_TRUE(Run({"mid_to_country"}, "999888777", &error).IsNull());
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(Run({"mid_to_country"}, "abc", &error).IsNull());
  EXPECT_TRUE(error.empty());
}

TEST_F(TransformTest, ShipTypeStaysInsideTheAisRange) {
  EXPECT_EQ(70, Run({"to_int", "normalize_ship_type"}, "70").AsInt());
  EXPECT_EQ(0, Run({"to_int", "normalize_ship_type"}, "0").AsInt());
  EXPECT_EQ(99, Run({"to_int", "normalize_ship_type"}, "99").AsInt());

  std::string error;
  EXPECT_TRUE(Run({"to_int", "normalize_ship_type"}, "1012", &error).IsNull());
  EXPECT_NE(std::string::npos, error.find("0-99"));
}

TEST_F(TransformTest, DegreeMinuteCoordinates) {
  // UN/LOCODE writes "DDMMH DDDMMH", latitude first.
  EXPECT_NEAR(51.9167, Run({"ddmm_to_decimal_lat"}, "5155N 00429E").AsDouble(),
              1e-4);
  EXPECT_NEAR(4.4833, Run({"ddmm_to_decimal_lon"}, "5155N 00429E").AsDouble(),
              1e-4);
  // Southern and western hemispheres are negative.
  EXPECT_NEAR(-33.0333, Run({"ddmm_to_decimal_lat"}, "3302S 07138W").AsDouble(),
              1e-4);
  EXPECT_NEAR(-71.6333, Run({"ddmm_to_decimal_lon"}, "3302S 07138W").AsDouble(),
              1e-4);

  std::string error;
  EXPECT_TRUE(Run({"ddmm_to_decimal_lat"}, "51.9225", &error).IsNull());
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(Run({"ddmm_to_decimal_lat"}, "5199N 00429E").IsNull())
      << "99 minutes is not a coordinate";
  EXPECT_TRUE(Run({"ddmm_to_decimal_lat"}, "5155X 00429E").IsNull());
  EXPECT_TRUE(Run({"ddmm_to_decimal_lat"}, "", &error).IsNull());
  EXPECT_TRUE(error.empty());
}

// --- the properties lineage depends on --------------------------------------

// Ids are written into provenance records on disk. Two transforms sharing one
// would make an old record ambiguous, and there would be no way to tell after
// the fact.
TEST_F(TransformTest, IdsAndNamesAreUnique) {
  std::set<TransformId> ids;
  std::set<std::string> names;
  for (const auto& t : registry_.All()) {
    EXPECT_TRUE(ids.insert(t.id).second) << "duplicate transform id " << t.id;
    EXPECT_TRUE(names.insert(t.name).second) << "duplicate name " << t.name;
    EXPECT_GT(t.version, 0u);
    EXPECT_NE(nullptr, t.doc);
  }
  EXPECT_EQ(ids.size(), registry_.All().size());
}

// The ids the mapping files and stored provenance both depend on. If a change
// to this file renumbers one, this test fails rather than the data silently
// decoding as a different transform.
TEST_F(TransformTest, IdsArePinned) {
  const std::pair<const char*, TransformId> pinned[] = {
      {"trim", 0x0101},           {"collapse_ws", 0x0102},
      {"upper", 0x0103},          {"lower", 0x0104},
      {"title_case", 0x0105},     {"strip_diacritics", 0x0106},
      {"first_char", 0x0107},     {"null_if_blank", 0x0108},
      {"concat", 0x0109},         {"split_semicolon", 0x010A},
      {"to_string", 0x0201},      {"to_int", 0x0202},
      {"to_double", 0x0203},      {"parse_timestamp", 0x0204},
      {"validate_locode", 0x0301}, {"validate_imo", 0x0302},
      {"strip_imo_prefix", 0x0303}, {"mid_to_country", 0x0304},
      {"normalize_ship_type", 0x0305}, {"ddmm_to_decimal_lat", 0x0306},
      {"ddmm_to_decimal_lon", 0x0307},
  };
  for (const auto& [name, id] : pinned) {
    const Transform* t = registry_.ByName(name);
    ASSERT_NE(nullptr, t) << name << " has disappeared from the registry";
    EXPECT_EQ(id, t->id) << name << " was renumbered; ids are on disk forever";
  }
}

// Purity, stated as a test: the same chain over the same input gives the same
// answer, every time, in any order. This is the assumption the day 11
// round-trip test rests on, so it is worth asserting rather than believing.
TEST_F(TransformTest, TransformsArePure) {
  const std::vector<std::string> inputs = {
      "  ROTTERDAM  ", "G\xC3\xB6teborg", "5155N 00429E", "9074729",
      "230123456",     "",                "N/A",          "Europoort; Botlek"};
  for (const auto& t : registry_.All()) {
    std::vector<TransformId> chain{t.id};
    for (const auto& input : inputs) {
      std::string e1, e2;
      const TValue first = registry_.Apply(chain, TValue::String(input), &e1);
      for (int repeat = 0; repeat < 3; ++repeat) {
        const TValue again = registry_.Apply(chain, TValue::String(input), &e2);
        ASSERT_EQ(first, again)
            << t.name << " gave a different answer on repeat for \"" << input
            << "\"";
        ASSERT_EQ(e1, e2) << t.name << " gave a different error on repeat";
      }
    }
  }
}

// A chain is replayable from ids alone, which is exactly what a provenance
// record stores.
TEST_F(TransformTest, ChainsReplayFromIdsAlone) {
  namespace t = transform_id;
  const std::vector<TransformId> chain = {t::kTrim, t::kCollapseWs, t::kTitleCase};
  std::string error;
  const TValue direct =
      registry_.Apply(chain, TValue::String("  PORT   OF ROTTERDAM "), &error);
  EXPECT_EQ("Port of Rotterdam", direct.AsString());

  // A fresh registry - as a later process would have - produces the same value.
  TransformRegistry other;
  const TValue replayed =
      other.Apply(chain, TValue::String("  PORT   OF ROTTERDAM "), &error);
  EXPECT_EQ(direct, replayed);
  EXPECT_EQ(registry_.ChainFingerprint(chain), other.ChainFingerprint(chain));
}

TEST_F(TransformTest, FingerprintDistinguishesChains) {
  namespace t = transform_id;
  const uint64_t a = registry_.ChainFingerprint({t::kTrim, t::kUpper});
  const uint64_t b = registry_.ChainFingerprint({t::kUpper, t::kTrim});
  const uint64_t c = registry_.ChainFingerprint({t::kTrim});
  EXPECT_NE(a, b) << "order must matter - upper then trim is not trim then upper";
  EXPECT_NE(a, c);
  EXPECT_NE(0u, registry_.ChainFingerprint({}));
}

TEST_F(TransformTest, UnknownTransformsAreNamedRatherThanIgnored) {
  std::vector<TransformId> chain;
  std::string error;
  EXPECT_FALSE(registry_.ResolveChain({"trim", "uscg_to_ais_type"}, &chain, &error));
  EXPECT_NE(std::string::npos, error.find("uscg_to_ais_type"));

  // Replaying a chain written by a build that had a transform this one does not
  // must say so rather than silently skipping the step.
  const TValue out = registry_.Apply({0xFFFF}, TValue::String("x"), &error);
  EXPECT_TRUE(out.IsNull());
  EXPECT_NE(std::string::npos, error.find("unknown transform"));
}

// A null means "the source had nothing here". Running validate_imo over it
// would report a failure for every AIS vessel that simply does not carry one.
TEST_F(TransformTest, NullPassesThroughUntouched) {
  std::string error;
  for (const auto& t : registry_.All()) {
    const TValue out = registry_.Apply({t.id}, TValue::Null(), &error);
    EXPECT_TRUE(out.IsNull()) << t.name;
    EXPECT_TRUE(error.empty()) << t.name << " complained about a null";
  }
}
