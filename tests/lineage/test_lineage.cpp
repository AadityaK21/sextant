// The lineage round-trip: the headline result of the whole project.
//
//   For every property of every entity: read the provenance, fetch the raw
//   source row it names, extract the exact cell, re-apply the transform chain
//   by id, and assert the result equals the stored value.
//
// THE TEST BELOW IT MATTERS AS MUCH
//
// A round-trip that passes at 100% is only worth something if it CAN fail.
// `TheRoundTripDetectsATransformThatChanged` deliberately corrupts a stored
// provenance record and asserts the check goes red. Without that, a bug in the
// checker would look exactly like a perfect result - which is the most
// dangerous shape a test can have.
//
// This was not hypothetical here. The first negative control I wrote broke
// `title_case` and the round trip still reported 100%, which looked like the
// checker was doing nothing. It was not: fusion picks UN/LOCODE for names by
// trust, and its NameWoDiacritics column is already title case, so breaking
// the transform genuinely changed nothing. Breaking `first_char` - which only
// the World Port Index feeds - produced 144 failures with exact diagnostics.
// A negative control that does not fail is a statement about the control.

#include "lineage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "corpus.h"

using namespace sextant::lineage;

namespace onto = sextant::ontology;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;
namespace resolve = sextant::resolve;

namespace {

using sextant::testsupport::Corpus;
using sextant::testsupport::SourceDir;

// The whole pipeline, built once. Lives in tests/support/corpus.h because the
// query suite needs exactly the same thing, and two copies of a 150-line setup
// is how two suites end up disagreeing about what "the corpus" is while both
// pass.
class LineageTest : public ::testing::Test {
 protected:
  static Corpus corpus_;

  static void SetUpTestSuite() {
    ASSERT_TRUE(sextant::testsupport::BuildCorpus("lineagetest", &corpus_));
    ASSERT_GT(corpus_.source_records, 600u);
  }

  static void TearDownTestSuite() {
    corpus_.store.reset();
    corpus_.Destroy();
  }
};

Corpus LineageTest::corpus_;

}  // namespace

// ============================================================================
// THE HEADLINE RESULT
// ============================================================================
TEST_F(LineageTest, EveryPropertyOfEveryEntityRoundTrips) {
  const LineageReader reader(corpus_.store.get(), &corpus_.bundle, SourceDir());
  RoundTripReport report;
  ASSERT_TRUE(reader.RoundTrip(&report).ok());

  std::printf("  %llu entities, %llu properties, %llu verified -> %.2f%%\n",
              static_cast<unsigned long long>(report.entities),
              static_cast<unsigned long long>(report.properties),
              static_cast<unsigned long long>(report.verified),
              report.rate() * 100.0);

  for (const auto& failure : report.failures) {
    std::printf("    FAIL %s: %s\n", RoundTripFailureName(failure.failure),
                failure.detail.c_str());
  }

  // The test would pass trivially on an empty store, so the size is asserted
  // first. This is the guard that turns "100%" into a claim about the data.
  EXPECT_GT(report.entities, 300u);
  EXPECT_GT(report.properties, 1500u);
  EXPECT_EQ(0u, report.failed);
  EXPECT_DOUBLE_EQ(1.0, report.rate());
}

// A round trip that cannot fail is not a test. This corrupts a stored
// provenance record and asserts the checker notices.
TEST_F(LineageTest, TheRoundTripDetectsACorruptedProvenanceRecord) {
  const LineageReader reader(corpus_.store.get(), &corpus_.bundle, SourceDir());

  // Find a property whose provenance points at a real raw cell, and rewrite the
  // record so its origin names a row that does not exist.
  const onto::EntityTypeDef* port = corpus_.bundle.ontology().Type("Port");
  ASSERT_NE(nullptr, port);

  codec::Ulid victim;
  codec::PropId prop = 0;
  bool found = false;
  for (auto it = corpus_.store->ScanEntities(port->id); it->Valid(); it->Next()) {
    codec::TypeId type = 0;
    codec::Ulid id;
    if (!codec::DecodeEntityKey(it->key(), &type, &id)) continue;
    std::vector<Explanation> explanations;
    if (!reader.ExplainAll(port->id, id, &explanations).ok()) continue;
    for (const auto& explanation : explanations) {
      if (explanation.replay_matches) {
        victim = id;
        prop = explanation.prop;
        found = true;
        break;
      }
    }
    if (found) break;
  }
  ASSERT_TRUE(found) << "no verifiable property to corrupt";

  // Rewrite the provenance so its SourceRef names a row that was never
  // ingested. A checker that is not actually fetching the raw row will not
  // notice.
  resolve::Provenance corrupted;
  {
    auto it = corpus_.store->ScanProvenance(victim, prop);
    ASSERT_TRUE(it->Valid());
    lsm::Slice value = it->value();
    ASSERT_TRUE(resolve::Provenance::DecodeFrom(&value, &corrupted));
  }
  corrupted.origin.row_seq = 999999;
  std::string encoded;
  corrupted.EncodeTo(&encoded);

  codec::EntityWriter writer = corpus_.store->EditEntity(port->id, victim);
  writer.AddProvenance(prop, 1, lsm::Slice(encoded));
  ASSERT_TRUE(writer.Commit().ok());

  RoundTripReport report;
  ASSERT_TRUE(reader.RoundTrip(&report).ok());
  EXPECT_GT(report.failed, 0u)
      << "the round trip did not notice a provenance record pointing at a row"
         " that does not exist, which means it is not fetching the raw row at"
         " all";

  bool saw_missing_row = false;
  for (const auto& failure : report.failures) {
    if (failure.failure == RoundTripFailure::kRawRowMissing) saw_missing_row = true;
  }
  EXPECT_TRUE(saw_missing_row);
}

// --- the explanation itself -------------------------------------------------

TEST_F(LineageTest, AnExplanationCarriesTheWholeChain) {
  const LineageReader reader(corpus_.store.get(), &corpus_.bundle, SourceDir());
  const onto::EntityTypeDef* port = corpus_.bundle.ontology().Type("Port");

  int checked = 0, with_rejections = 0, merged = 0;
  for (auto it = corpus_.store->ScanEntities(port->id); it->Valid() && checked < 40;
       it->Next()) {
    codec::TypeId type = 0;
    codec::Ulid id;
    if (!codec::DecodeEntityKey(it->key(), &type, &id)) continue;

    std::vector<Explanation> explanations;
    ASSERT_TRUE(reader.ExplainAll(port->id, id, &explanations).ok());
    for (const auto& e : explanations) {
      ++checked;
      // Everything a lineage panel needs, present on every explanation.
      EXPECT_FALSE(e.property_name.empty());
      EXPECT_FALSE(e.source_key.empty());
      EXPECT_FALSE(e.column.empty());
      EXPECT_FALSE(e.raw_row.empty()) << e.property_name;
      EXPECT_FALSE(e.rule.empty());
      EXPECT_FALSE(e.chain_changed);
      // Every transform in the chain resolved to a name rather than a number.
      for (const auto& name : e.transform_names) {
        EXPECT_EQ(std::string::npos, name.find("unknown:")) << name;
      }
      if (!e.rejected.empty()) {
        ++with_rejections;
        // A loser without a reason is the assertion this project exists to
        // replace with an answer.
        EXPECT_FALSE(e.rejected.front().reason.empty());
        EXPECT_FALSE(e.rejected.front().column.empty());
      }
      if (e.cluster_size > 1) ++merged;
    }
  }
  EXPECT_GT(checked, 30);
  EXPECT_GT(with_rejections, 0) << "no property had a rejected alternative, so"
                                   " the fusion evidence is never exercised";
  EXPECT_GT(merged, 0);
}

TEST_F(LineageTest, RawCellExtractionHandlesBothConnectorShapes) {
  const LineageReader reader(corpus_.store.get(), &corpus_.bundle, SourceDir());

  // CSV: the column name is turned into a position using the source's header,
  // and the row is parsed with the same reader the connector used - so quoting
  // and embedded separators survive.
  const onto::SourceSpec* wpi = corpus_.bundle.Source("wpi");
  ASSERT_NE(nullptr, wpi);
  std::string cell;
  ASSERT_TRUE(reader
                  .RawCell(*wpi,
                           "10000,North Sea,ROTTERDAM,\"A; B\",NLRTM,NL,North "
                           "Sea,Large,Coastal Natural,51.9,4.5",
                           "Alternate Port Name", &cell)
                  .ok());
  EXPECT_EQ("A; B", cell) << "the quoted field was split on its semicolon";

  ASSERT_TRUE(reader.RawCell(*wpi, "10000,North Sea,ROTTERDAM,\"A; B\",NLRTM,NL,"
                                   "North Sea,Large,Coastal Natural,51.9,4.5",
                             "UN/LOCODE", &cell)
                  .ok());
  EXPECT_EQ("NLRTM", cell);

  // A multi-column property records its columns joined with '+', and the cell
  // is rebuilt the same way the mapper built it.
  const onto::SourceSpec* unlocode = corpus_.bundle.Source("unlocode");
  ASSERT_TRUE(reader
                  .RawCell(*unlocode,
                           ",NL,RTM,Rotterdam,Rotterdam,,AI,1-3-----,0401,,"
                           "5155N 00429E,",
                           "Country+Location", &cell)
                  .ok());
  EXPECT_EQ(std::string("NL") + '\x1f' + "RTM", cell);

  // JSON: the same dotted path the mapping used.
  const onto::SourceSpec* digitraffic = corpus_.bundle.Source("digitraffic");
  ASSERT_TRUE(reader
                  .RawCell(*digitraffic,
                           R"({"portCallId":1,"portAreaDetails":[{"ata":"2026-04-03T07:15:00+03:00"}]})",
                           "portAreaDetails[0].ata", &cell)
                  .ok());
  EXPECT_EQ("2026-04-03T07:15:00+03:00", cell);

  EXPECT_FALSE(reader.RawCell(*wpi, "1,2,3", "No Such Column", &cell).ok());
}

// --- links ------------------------------------------------------------------

TEST_F(LineageTest, LinksResolvedIntoEdgesAndTheTimeIndex) {
  EXPECT_GT(corpus_.links.references_seen, 500u);
  EXPECT_EQ(0u, corpus_.links.orphaned)
      << "a source record with links did not end up in any entity";
  EXPECT_GT(corpus_.links.edges_written, 500u);
  EXPECT_GT(corpus_.links.time_indexed, 300u);

  // All three link types the ontology declares.
  EXPECT_GT(corpus_.links.by_link_type["arrives_at"], 100u);
  EXPECT_GT(corpus_.links.by_link_type["departs_from"], 100u);
  EXPECT_GT(corpus_.links.by_link_type["operated_by"], 100u);
}

// THE HEADLINE QUERY, over real resolved data rather than a synthetic fixture.
//
// "Everything that arrived at this port in this window" has to be a seek plus a
// sequential read. `keys_scanned` proves it: the scan touches only the matching
// keys, so nothing is examined and rejected.
TEST_F(LineageTest, TheQuarterQueryIsARangeScanOverResolvedData) {
  const onto::LinkTypeDef* arrives = corpus_.bundle.ontology().Link("arrives_at");
  ASSERT_NE(nullptr, arrives);
  const onto::EntityTypeDef* port = corpus_.bundle.ontology().Type("Port");

  // Find the port with the most arrivals, so the window actually selects
  // something.
  codec::Ulid busiest;
  uint64_t most = 0;
  for (auto it = corpus_.store->ScanEntities(port->id); it->Valid(); it->Next()) {
    codec::TypeId type = 0;
    codec::Ulid id;
    if (!codec::DecodeEntityKey(it->key(), &type, &id)) continue;
    uint64_t arrivals = 0;
    for (auto in = corpus_.store->ScanIncoming(id, arrives->id); in->Valid(); in->Next()) {
      ++arrivals;
    }
    if (arrivals > most) {
      most = arrivals;
      busiest = id;
    }
  }
  ASSERT_GT(most, 2u) << "no port has enough arrivals to make this meaningful";

  // 2026-04-01 to 2026-07-01 UTC.
  int64_t q2 = 0, q3 = 0;
  ASSERT_TRUE(onto::ParseIso8601("2026-04-01T00:00:00Z", &q2));
  ASSERT_TRUE(onto::ParseIso8601("2026-07-01T00:00:00Z", &q3));

  auto scan = corpus_.store->ScanTimeRange(arrives->id, busiest, q2, q3);
  uint64_t matched = 0;
  for (; scan->Valid(); scan->Next()) {
    codec::LinkTypeId link_type = 0;
    codec::Ulid anchor, entity;
    int64_t timestamp = 0;
    ASSERT_TRUE(codec::DecodeTimeIndexKey(scan->key(), &link_type, &anchor,
                                          &timestamp, &entity));
    EXPECT_EQ(arrives->id, link_type);
    EXPECT_EQ(busiest, anchor);
    EXPECT_GE(timestamp, q2);
    EXPECT_LT(timestamp, q3);
    ++matched;
  }

  std::printf("  quarter query: %llu arrivals, %llu keys scanned\n",
              static_cast<unsigned long long>(matched),
              static_cast<unsigned long long>(scan->keys_scanned()));

  EXPECT_GT(matched, 0u);
  EXPECT_EQ(matched, scan->keys_scanned())
      << "the scan touched " << scan->keys_scanned() << " keys to return "
      << matched << " - that is a filter, not a range scan";
}
