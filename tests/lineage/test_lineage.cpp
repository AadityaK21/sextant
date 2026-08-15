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
#include <unordered_map>
#include <utility>
#include <vector>

#include "blocking.h"
#include "bundle.h"
#include "cluster.h"
#include "csv.h"
#include "env.h"
#include "fuse.h"
#include "ingest.h"
#include "json_source.h"
#include "link.h"
#include "scorer.h"
#include "store.h"

// DO NOT SORT THIS BLOCK. scripts/check_includes.py --fix reordered these
// across the #if/#else once and produced "#else without #if" - the same failure
// env.cpp hit on day 6, for the same reason. Both files are in SKIP_FIX now.
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace sextant::lineage;

namespace onto = sextant::ontology;
namespace conn = sextant::connectors;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;
namespace resolve = sextant::resolve;

namespace {

std::string SourceDir() { return std::string(SEXTANT_SOURCE_DIR); }

unsigned long ProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

// Ingests, resolves and links the whole committed corpus once, so the tests
// below run against the same pipeline output the CLI produces.
class LineageTest : public ::testing::Test {
 protected:
  static std::string dbname_;
  static std::unique_ptr<codec::Store> store_;
  static onto::SchemaBundle bundle_;
  static resolve::ResolverProperties props_;
  static resolve::LinkReport links_;
  static uint64_t entities_written_;

  static void Destroy() {
    std::vector<std::string> children;
    if (lsm::GetChildren(dbname_, &children).ok()) {
      for (const auto& c : children) {
        if (c == "." || c == "..") continue;
        lsm::RemoveFile(dbname_ + "/" + c);
      }
    }
    std::remove(dbname_.c_str());
  }

  static void SetUpTestSuite() {
    dbname_ = "lineagetest_db_" + std::to_string(ProcessId());
    Destroy();

    lsm::Options options;
    options.create_if_missing = true;
    ASSERT_TRUE(codec::Store::Open(options, dbname_, &store_).ok());
    ASSERT_TRUE(
        onto::SchemaBundle::LoadFromDir(SourceDir() + "/schema", &bundle_).ok());
    ASSERT_TRUE(
        resolve::ResolverProperties::Resolve(bundle_.ontology(), &props_).ok());

    conn::Ingestor ingestor(store_.get(), &bundle_.ontology(),
                            &bundle_.transforms());
    for (const char* key : {"wpi", "unlocode"}) {
      const onto::SourceSpec* spec = bundle_.Source(key);
      std::unique_ptr<conn::CsvReader> reader;
      ASSERT_TRUE(
          conn::CsvReader::Open(SourceDir() + "/" + spec->uri, &reader).ok());
      conn::Ingestor::Result result;
      ASSERT_TRUE(ingestor.Run(*spec, reader.get(), 0, {}, &result).ok());
    }
    conn::SnapshotFetcher fetcher(SourceDir() + "/data/snapshots/digitraffic");
    const std::pair<const char*, const char*> endpoints[] = {
        {"digitraffic", "ports"},
        {"digitraffic", "vessel_details"},
        {"digitraffic_ais", "ais_vessels"},
    };
    for (const auto& [source_key, endpoint] : endpoints) {
      std::string body;
      ASSERT_TRUE(fetcher.Fetch(endpoint, "/", &body).ok());
      std::unique_ptr<conn::JsonRowSource> rows;
      ASSERT_TRUE(conn::JsonRowSource::Open(body, "", endpoint, &rows).ok());
      conn::Ingestor::Result result;
      ASSERT_TRUE(
          ingestor.Run(*bundle_.Source(source_key), rows.get(), 0, {}, &result)
              .ok());
    }
    // Port calls, which are the Voyage entity and the only source of links.
    {
      std::string body;
      ASSERT_TRUE(fetcher.Fetch("port_calls", "/", &body).ok());
      std::unique_ptr<conn::JsonRowSource> rows;
      ASSERT_TRUE(
          conn::JsonRowSource::Open(body, "portCalls", "port_calls", &rows).ok());
      conn::Ingestor::Result result;
      ASSERT_TRUE(
          ingestor.Run(*bundle_.Source("digitraffic"), rows.get(), 0, {}, &result)
              .ok());
    }

    // Load, block, score, cluster, fuse, write, link - the whole pipeline.
    std::unordered_map<resolve::RecordRef, onto::SourceRecord,
                       resolve::RecordRefHash>
        records;
    for (const auto& spec : bundle_.sources()) {
      auto it = store_->ScanSourceRecords(spec.id);
      for (; it->Valid(); it->Next()) {
        lsm::Slice value = it->value();
        onto::SourceRecord record;
        if (!onto::SourceRecord::DecodeFrom(&value, &record)) continue;
        records[resolve::RecordRef{record.source_id, record.natural_key_hash}] =
            std::move(record);
      }
    }
    ASSERT_GT(records.size(), 600u);

    resolve::ScorerConfig config;
    ASSERT_TRUE(resolve::ScorerConfig::LoadFromFile(
                    SourceDir() + "/schema/resolver.yaml", &config)
                    .ok());

    resolve::Blocker blocker(store_.get(), &bundle_, &props_);
    resolve::BlockingReport report;
    ASSERT_TRUE(blocker.IndexAll(&report).ok());
    std::vector<resolve::CandidatePairRef> candidates;
    ASSERT_TRUE(blocker.GenerateCandidates({}, &candidates, &report).ok());

    const resolve::PairScorer scorer(&config, &props_);
    std::vector<resolve::ScoredEdge> edges;
    for (const auto& candidate : candidates) {
      const auto a = records.find(candidate.pair.a);
      const auto b = records.find(candidate.pair.b);
      if (a == records.end() || b == records.end()) continue;
      const resolve::PairScore score = scorer.Score(a->second, b->second);
      resolve::ScoredEdge edge;
      edge.pair = candidate.pair;
      edge.score = score.score;
      edge.decision = score.decision;
      edge.vetoed = score.vetoed;
      edges.push_back(std::move(edge));
    }

    std::vector<resolve::RecordRef> all;
    all.reserve(records.size());
    for (const auto& [ref, record] : records) all.push_back(ref);

    const resolve::ClusterSet clusters =
        resolve::ClusterVetoConstrained(edges, all);
    const resolve::Fuser fuser(&bundle_, &props_);
    for (const auto& cluster : clusters.clusters) {
      std::vector<const onto::SourceRecord*> members;
      for (const auto& member : cluster) {
        const auto it = records.find(member);
        if (it != records.end()) members.push_back(&it->second);
      }
      if (members.empty()) continue;
      const resolve::ResolvedEntity entity = fuser.Fuse(members, cluster, {});
      ASSERT_TRUE(resolve::WriteEntity(store_.get(), bundle_, entity).ok());
      ++entities_written_;
    }
    ASSERT_TRUE(
        resolve::ResolveLinks(store_.get(), bundle_, props_, &links_).ok());
  }

  static void TearDownTestSuite() {
    store_.reset();
    Destroy();
  }
};

std::string LineageTest::dbname_;
std::unique_ptr<codec::Store> LineageTest::store_;
onto::SchemaBundle LineageTest::bundle_;
resolve::ResolverProperties LineageTest::props_;
resolve::LinkReport LineageTest::links_;
uint64_t LineageTest::entities_written_ = 0;

}  // namespace

// ============================================================================
// THE HEADLINE RESULT
// ============================================================================
TEST_F(LineageTest, EveryPropertyOfEveryEntityRoundTrips) {
  const LineageReader reader(store_.get(), &bundle_, SourceDir());
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
  const LineageReader reader(store_.get(), &bundle_, SourceDir());

  // Find a property whose provenance points at a real raw cell, and rewrite the
  // record so its origin names a row that does not exist.
  const onto::EntityTypeDef* port = bundle_.ontology().Type("Port");
  ASSERT_NE(nullptr, port);

  codec::Ulid victim;
  codec::PropId prop = 0;
  bool found = false;
  for (auto it = store_->ScanEntities(port->id); it->Valid(); it->Next()) {
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
    auto it = store_->ScanProvenance(victim, prop);
    ASSERT_TRUE(it->Valid());
    lsm::Slice value = it->value();
    ASSERT_TRUE(resolve::Provenance::DecodeFrom(&value, &corrupted));
  }
  corrupted.origin.row_seq = 999999;
  std::string encoded;
  corrupted.EncodeTo(&encoded);

  codec::EntityWriter writer = store_->EditEntity(port->id, victim);
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
  const LineageReader reader(store_.get(), &bundle_, SourceDir());
  const onto::EntityTypeDef* port = bundle_.ontology().Type("Port");

  int checked = 0, with_rejections = 0, merged = 0;
  for (auto it = store_->ScanEntities(port->id); it->Valid() && checked < 40;
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
  const LineageReader reader(store_.get(), &bundle_, SourceDir());

  // CSV: the column name is turned into a position using the source's header,
  // and the row is parsed with the same reader the connector used - so quoting
  // and embedded separators survive.
  const onto::SourceSpec* wpi = bundle_.Source("wpi");
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
  const onto::SourceSpec* unlocode = bundle_.Source("unlocode");
  ASSERT_TRUE(reader
                  .RawCell(*unlocode,
                           ",NL,RTM,Rotterdam,Rotterdam,,AI,1-3-----,0401,,"
                           "5155N 00429E,",
                           "Country+Location", &cell)
                  .ok());
  EXPECT_EQ(std::string("NL") + '\x1f' + "RTM", cell);

  // JSON: the same dotted path the mapping used.
  const onto::SourceSpec* digitraffic = bundle_.Source("digitraffic");
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
  EXPECT_GT(links_.references_seen, 500u);
  EXPECT_EQ(0u, links_.orphaned)
      << "a source record with links did not end up in any entity";
  EXPECT_GT(links_.edges_written, 500u);
  EXPECT_GT(links_.time_indexed, 300u);

  // All three link types the ontology declares.
  EXPECT_GT(links_.by_link_type["arrives_at"], 100u);
  EXPECT_GT(links_.by_link_type["departs_from"], 100u);
  EXPECT_GT(links_.by_link_type["operated_by"], 100u);
}

// THE HEADLINE QUERY, over real resolved data rather than a synthetic fixture.
//
// "Everything that arrived at this port in this window" has to be a seek plus a
// sequential read. `keys_scanned` proves it: the scan touches only the matching
// keys, so nothing is examined and rejected.
TEST_F(LineageTest, TheQuarterQueryIsARangeScanOverResolvedData) {
  const onto::LinkTypeDef* arrives = bundle_.ontology().Link("arrives_at");
  ASSERT_NE(nullptr, arrives);
  const onto::EntityTypeDef* port = bundle_.ontology().Type("Port");

  // Find the port with the most arrivals, so the window actually selects
  // something.
  codec::Ulid busiest;
  uint64_t most = 0;
  for (auto it = store_->ScanEntities(port->id); it->Valid(); it->Next()) {
    codec::TypeId type = 0;
    codec::Ulid id;
    if (!codec::DecodeEntityKey(it->key(), &type, &id)) continue;
    uint64_t arrivals = 0;
    for (auto in = store_->ScanIncoming(id, arrives->id); in->Valid(); in->Next()) {
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

  auto scan = store_->ScanTimeRange(arrives->id, busiest, q2, q3);
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
