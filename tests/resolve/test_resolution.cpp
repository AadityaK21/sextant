// Scoring, clustering and fusion, end to end over the ingested corpus.
//
// The three assertions that matter:
//
//   1. The IMO veto holds. Two records with different IMO numbers must not
//      merge however much else agrees - and the corpus contains three pairs
//      where the MMSI matches exactly and the answer is still no.
//   2. Held-out F1 clears its threshold. This is a regression gate on the
//      number in the README, measured on the split the weights never saw.
//   3. Veto-constrained clustering beats transitive union-find on precision.
//      That comparison is the point of implementing both.

#include <gtest/gtest.h>

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
#include "evaluate.h"
#include "fuse.h"
#include "golden.h"
#include "ingest.h"
#include "json_source.h"
#include "scorer.h"
#include "store.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace sextant::resolve;

namespace onto = sextant::ontology;
namespace conn = sextant::connectors;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;

namespace {

std::string SourceDir() { return std::string(SEXTANT_SOURCE_DIR); }

unsigned long ProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

class ResolutionTest : public ::testing::Test {
 protected:
  static std::string dbname_;
  static std::unique_ptr<codec::Store> store_;
  static onto::SchemaBundle bundle_;
  static ResolverProperties props_;
  static ScorerConfig config_;
  static std::unordered_map<RecordRef, onto::SourceRecord, RecordRefHash> records_;
  static std::vector<ScoredEdge> edges_;

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
    // Per process, not per test: ctest runs each TEST_F as its own process and
    // a shared directory name means the second one to start finds the LOCK held.
    dbname_ = "resolutiontest_db_" + std::to_string(ProcessId());
    Destroy();

    lsm::Options options;
    options.create_if_missing = true;
    const Status open = codec::Store::Open(options, dbname_, &store_);
    ASSERT_TRUE(open.ok()) << open.ToString();
    ASSERT_TRUE(
        onto::SchemaBundle::LoadFromDir(SourceDir() + "/schema", &bundle_).ok());
    ASSERT_TRUE(ResolverProperties::Resolve(bundle_.ontology(), &props_).ok());

    // The committed weights, not the defaults - this is a regression gate on
    // what the repository actually ships.
    const Status cs = ScorerConfig::LoadFromFile(
        SourceDir() + "/schema/resolver.yaml", &config_);
    ASSERT_TRUE(cs.ok()) << cs.ToString();

    conn::Ingestor ingestor(store_.get(), &bundle_.ontology(),
                            &bundle_.transforms());
    for (const char* key : {"wpi", "unlocode"}) {
      const onto::SourceSpec* spec = bundle_.Source(key);
      std::unique_ptr<conn::CsvReader> reader;
      ASSERT_TRUE(conn::CsvReader::Open(SourceDir() + "/" + spec->uri, &reader).ok());
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

    for (const auto& spec : bundle_.sources()) {
      auto it = store_->ScanSourceRecords(spec.id);
      for (; it->Valid(); it->Next()) {
        lsm::Slice value = it->value();
        onto::SourceRecord record;
        if (!onto::SourceRecord::DecodeFrom(&value, &record)) continue;
        records_[RecordRef{record.source_id, record.natural_key_hash}] =
            std::move(record);
      }
    }
    ASSERT_GT(records_.size(), 400u);

    Blocker blocker(store_.get(), &bundle_, &props_);
    BlockingReport report;
    ASSERT_TRUE(blocker.IndexAll(&report).ok());
    std::vector<CandidatePairRef> candidates;
    ASSERT_TRUE(blocker.GenerateCandidates({}, &candidates, &report).ok());

    const PairScorer scorer(&config_, &props_);
    for (const auto& candidate : candidates) {
      const auto a = records_.find(candidate.pair.a);
      const auto b = records_.find(candidate.pair.b);
      if (a == records_.end() || b == records_.end()) continue;
      const PairScore score = scorer.Score(a->second, b->second);
      ScoredEdge edge;
      edge.pair = candidate.pair;
      edge.score = score.score;
      edge.decision = score.decision;
      edge.vetoed = score.vetoed;
      edge.veto_reason = score.veto_reason;
      edges_.push_back(std::move(edge));
    }
    ASSERT_FALSE(edges_.empty());
  }

  static void TearDownTestSuite() {
    records_.clear();
    edges_.clear();
    store_.reset();
    Destroy();
  }

  static Status LoadGolden(const char* file, GoldenSet* out) {
    return GoldenSet::LoadFromFile(SourceDir() + "/eval/" + file, bundle_, out);
  }

  static std::vector<ScoredPair> ScoreGolden(const GoldenSet& golden) {
    const PairScorer scorer(&config_, &props_);
    std::vector<ScoredPair> scored;
    for (const auto& labeled : golden.pairs()) {
      const auto a = records_.find(labeled.pair.a);
      const auto b = records_.find(labeled.pair.b);
      if (a == records_.end() || b == records_.end()) continue;
      ScoredPair pair;
      pair.pair = labeled.pair;
      pair.text_a = labeled.text_a;
      pair.text_b = labeled.text_b;
      pair.is_match = labeled.is_match;
      pair.split = SplitFor(labeled.pair);
      pair.score = scorer.Score(a->second, b->second);
      scored.push_back(std::move(pair));
    }
    return scored;
  }
};

std::string ResolutionTest::dbname_;
std::unique_ptr<codec::Store> ResolutionTest::store_;
onto::SchemaBundle ResolutionTest::bundle_;
ResolverProperties ResolutionTest::props_;
ScorerConfig ResolutionTest::config_;
std::unordered_map<RecordRef, onto::SourceRecord, RecordRefHash>
    ResolutionTest::records_;
std::vector<ScoredEdge> ResolutionTest::edges_;

}  // namespace

// --- config -----------------------------------------------------------------

TEST_F(ResolutionTest, ShippedConfigIsCoherent) {
  EXPECT_GT(config_.port.match_threshold, config_.port.review_threshold);
  EXPECT_GT(config_.vessel.match_threshold, config_.vessel.review_threshold);
  // An exact UN/LOCODE match must clear the threshold on its own - it is the
  // code authority agreeing with itself.
  EXPECT_GE(config_.port.Weight("locode_exact"), config_.port.match_threshold);
  EXPECT_GE(config_.vessel.Weight("imo_exact"), config_.vessel.match_threshold);
  // The IMO must outweigh the MMSI, which is a licence rather than a hull.
  EXPECT_GT(config_.vessel.Weight("imo_exact"), config_.vessel.Weight("mmsi_exact"));

  // A review band above the match threshold would make every uncertain pair a
  // match, which the loader rejects.
  ScorerConfig broken;
  const Status s = ScorerConfig::LoadFromString(R"YAML(
Port:
  weights: { locode_exact: 6.0 }
  match_threshold: 4.0
  review_threshold: 6.0
Vessel:
  weights: { imo_exact: 8.0 }
  match_threshold: 5.0
  review_threshold: 3.0
)YAML",
                                                &broken, "<test>");
  EXPECT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("review_threshold"));
}

// --- the veto ---------------------------------------------------------------

// THE ASSERTION THIS WHOLE MECHANISM EXISTS FOR.
//
// An MMSI belongs to the radio licence and is reissued on reflagging, so two
// records can share one exactly and still be different hulls. The IMO settles
// it, and it settles it as a RULE - not as a large negative weight that enough
// other evidence could outvote.
TEST_F(ResolutionTest, ConflictingImoVetoesEvenWhenTheMmsiMatchesExactly) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_vessels.csv", &golden).ok());

  const PairScorer scorer(&config_, &props_);
  int reassignments = 0;
  for (const auto& labeled : golden.pairs()) {
    if (labeled.is_match) continue;
    // The forced hard negatives: identical natural key across two sources.
    const std::string key_a = labeled.text_a.substr(labeled.text_a.find(':') + 1);
    const std::string key_b = labeled.text_b.substr(labeled.text_b.find(':') + 1);
    if (key_a != key_b) continue;

    const auto a = records_.find(labeled.pair.a);
    const auto b = records_.find(labeled.pair.b);
    ASSERT_NE(records_.end(), a);
    ASSERT_NE(records_.end(), b);

    const PairScore score = scorer.Score(a->second, b->second);
    ++reassignments;
    EXPECT_TRUE(score.vetoed) << labeled.text_a << " <-> " << labeled.text_b;
    EXPECT_EQ(Decision::kNonMatch, score.decision);
    EXPECT_NE(std::string::npos, score.veto_reason.find("IMO"))
        << score.veto_reason;
    // The reason has to be readable, because it is what a reviewer sees.
    EXPECT_NE(std::string::npos, score.veto_reason.find("permanent"));
  }
  EXPECT_EQ(3, reassignments)
      << "the corpus lost its MMSI reassignment cases, and this test with them";
}

TEST_F(ResolutionTest, ConflictingCodesAreVetoed) {
  int locode_conflicts = 0, country_conflicts = 0, distance_vetoes = 0;
  for (const auto& edge : edges_) {
    if (!edge.vetoed) continue;
    if (edge.veto_reason.find("locode conflict") != std::string::npos) {
      ++locode_conflicts;
    }
    if (edge.veto_reason.find("country conflict") != std::string::npos) {
      ++country_conflicts;
    }
    if (edge.veto_reason.find("km apart") != std::string::npos) {
      ++distance_vetoes;
    }
  }
  EXPECT_GT(locode_conflicts, 0)
      << "two records carrying different UN/LOCODEs claim to be different places";
  EXPECT_GT(country_conflicts, 0);

  // THE DISTANCE VETO NEVER FIRES ON THIS CORPUS, and that is worth asserting
  // rather than hoping.
  //
  // It is checked third, after the locode and country vetoes, and blocking only
  // proposes pairs that already share a code, a name or a geographic cell. By
  // the time a pair reaches the distance check, everything it would have caught
  // has been rejected for a more specific reason.
  //
  // So it is a redundant guard on today's blocking scheme. It stays because
  // that scheme is configuration - widen the geohash precision or add a key and
  // it becomes load-bearing again - but a rule that never runs is a rule nobody
  // is testing, which is why the next assertion exercises it directly.
  EXPECT_EQ(0, distance_vetoes)
      << "the distance veto fired, which means the earlier vetoes stopped"
         " covering it - the comment above is now out of date";
}

// The distance rule, exercised directly since the corpus never reaches it.
TEST_F(ResolutionTest, DistantPortsAreVetoedWhenNothingElseRejectsThemFirst) {
  const onto::EntityTypeDef* port = bundle_.ontology().Type("Port");
  const codec::PropId name = port->Property("name")->id;
  const codec::PropId country = port->Property("country")->id;
  const codec::PropId lat = port->Property("lat")->id;
  const codec::PropId lon = port->Property("lon")->id;

  // Same country, same name, no UN/LOCODE on either - so neither of the
  // earlier vetoes applies - but 4,000 km apart.
  auto make = [&](double latitude, double longitude) {
    onto::SourceRecord record;
    record.type = port->id;
    record.source_id = 1;
    for (const auto& [prop, value] :
         std::vector<std::pair<codec::PropId, onto::TValue>>{
             {name, onto::TValue::String("PORTSMOUTH")},
             {country, onto::TValue::String("US")},
             {lat, onto::TValue::Double(latitude)},
             {lon, onto::TValue::Double(longitude)}}) {
      onto::PropertyCell cell;
      cell.prop = prop;
      cell.value = value;
      cell.origin.source_id = 1;
      cell.origin.column = "test";
      record.properties.push_back(std::move(cell));
    }
    return record;
  };

  const onto::SourceRecord here = make(43.07, -70.76);       // New Hampshire
  const onto::SourceRecord nearby = make(43.07, -69.60);     // about 94 km east
  const onto::SourceRecord virginia = make(36.84, -76.29);   // 837 km away
  const onto::SourceRecord hawaii = make(21.31, -157.86);    // 8,000 km away

  const PairScorer scorer(&config_, &props_);

  // Under the limit: two records for a port complex that spans a bay. The
  // scorer has to weigh the evidence rather than refuse to look.
  const PairScore close = scorer.Score(here, nearby);
  EXPECT_FALSE(close.vetoed) << close.veto_reason;

  // Portsmouth, New Hampshire and Portsmouth, Virginia are both real, share a
  // name and a country, and are 837 km apart. This is the case the rule exists
  // for - name similarity alone would happily merge them.
  const PairScore distant = scorer.Score(here, virginia);
  EXPECT_TRUE(distant.vetoed);
  EXPECT_EQ(Decision::kNonMatch, distant.decision);
  EXPECT_NE(std::string::npos, distant.veto_reason.find("km apart"))
      << distant.veto_reason;

  const PairScore very_distant = scorer.Score(here, hawaii);
  EXPECT_TRUE(very_distant.vetoed);
}

// --- the headline number ----------------------------------------------------

// A regression gate on the figure in the README, measured on the split the
// weights were never fitted to.
TEST_F(ResolutionTest, HeldOutF1ClearsTheThresholdForPorts) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_ports.csv", &golden).ok());
  std::vector<ScoredPair> scored = ScoreGolden(golden);
  ASSERT_GT(scored.size(), 1000u);

  const EvaluationReport report = EvaluateAll(scored, config_, props_);
  std::printf("  ports held-out: P %.4f R %.4f F1 %.4f  (TP %llu FP %llu FN %llu)\n",
              report.holdout.precision(), report.holdout.recall(),
              report.holdout.f1(),
              static_cast<unsigned long long>(report.holdout.true_positive),
              static_cast<unsigned long long>(report.holdout.false_positive),
              static_cast<unsigned long long>(report.holdout.false_negative));

  EXPECT_GT(report.holdout.total(), 200u) << "the held-out split is too small"
                                             " to mean anything";
  EXPECT_GT(report.holdout.f1(), 0.90);
  EXPECT_GT(report.holdout.precision(), 0.90);
  EXPECT_GT(report.holdout.recall(), 0.90);
}

TEST_F(ResolutionTest, HeldOutF1ClearsTheThresholdForVessels) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_vessels.csv", &golden).ok());
  std::vector<ScoredPair> scored = ScoreGolden(golden);

  const EvaluationReport report = EvaluateAll(scored, config_, props_);
  EXPECT_GT(report.holdout.f1(), 0.90);
  // Vessels are the easier task, and it is worth saying why rather than
  // claiming credit: when both records carry an IMO, one exact comparison
  // settles it, and the only interesting cases are the reassignments the veto
  // handles.
  EXPECT_EQ(0u, report.holdout.false_positive);
}

// The split has to be stable as the golden set grows, or a number measured
// today stops being comparable with one measured next week.
TEST_F(ResolutionTest, TheSplitIsDeterministicAndRoughlyEightyTwenty) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_ports.csv", &golden).ok());

  int train = 0, holdout = 0;
  for (const auto& labeled : golden.pairs()) {
    const Split split = SplitFor(labeled.pair);
    EXPECT_EQ(split, SplitFor(labeled.pair)) << "not deterministic";
    if (split == Split::kTrain) ++train; else ++holdout;
  }
  const double fraction = static_cast<double>(holdout) /
                          static_cast<double>(train + holdout);
  EXPECT_NEAR(0.20, fraction, 0.05) << holdout << " of " << train + holdout;
}

// --- clustering -------------------------------------------------------------

TEST(UnionFindTest, MergesAndFindsRoots) {
  UnionFind uf;
  const RecordRef a{1, 1}, b{1, 2}, c{1, 3}, d{2, 1};
  for (const auto& ref : {a, b, c, d}) uf.Add(ref);

  EXPECT_TRUE(uf.Union(a, b));
  EXPECT_FALSE(uf.Union(a, b)) << "already together";
  EXPECT_TRUE(uf.Union(b, c));
  EXPECT_EQ(uf.Find(a), uf.Find(c)) << "transitive by construction";
  EXPECT_NE(uf.Find(a), uf.Find(d));

  const auto clusters = uf.Clusters();
  EXPECT_EQ(2u, clusters.size());
  EXPECT_EQ(4u, uf.size());
}

// THE COMPARISON. Matching is not transitive, and plain union-find welds
// clusters together through chains of individually reasonable decisions.
TEST_F(ResolutionTest, VetoConstrainedClusteringBeatsTransitiveOnPrecision) {
  const ClusterSet plain = ClusterTransitive(edges_);
  const ClusterSet constrained = ClusterVetoConstrained(edges_);

  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_ports.csv", &golden).ok());
  const ClusterMetrics a = MeasureClusters(plain, golden);
  const ClusterMetrics b = MeasureClusters(constrained, golden);

  std::printf("  transitive       clusters %llu largest %llu  P %.4f R %.4f F1 %.4f\n",
              static_cast<unsigned long long>(a.clusters),
              static_cast<unsigned long long>(a.largest), a.precision(),
              a.recall(), a.f1());
  std::printf("  veto-constrained clusters %llu largest %llu  P %.4f R %.4f F1 %.4f"
              "  (%llu refused)\n",
              static_cast<unsigned long long>(b.clusters),
              static_cast<unsigned long long>(b.largest), b.precision(),
              b.recall(), b.f1(),
              static_cast<unsigned long long>(b.refused_merges));

  EXPECT_GT(b.refused_merges, 0u)
      << "no merge was refused, so the two algorithms are indistinguishable"
         " on this corpus and the comparison proves nothing";
  EXPECT_GT(b.precision(), a.precision())
      << "the constraint is supposed to buy precision";
  EXPECT_GE(b.f1(), a.f1());
  // Refusing merges cannot invent clusters that were not there.
  EXPECT_GE(b.clusters, a.clusters);
  EXPECT_LE(b.largest, a.largest);
}

TEST_F(ResolutionTest, ClusteringIsDeterministic) {
  const ClusterSet a = ClusterVetoConstrained(edges_);
  const ClusterSet b = ClusterVetoConstrained(edges_);
  ASSERT_EQ(a.clusters.size(), b.clusters.size());
  for (size_t i = 0; i < a.clusters.size(); ++i) {
    EXPECT_EQ(a.clusters[i], b.clusters[i]) << "at cluster " << i;
  }
}

// --- fusion -----------------------------------------------------------------

TEST_F(ResolutionTest, FusionAppliesTheRuleDeclaredInTheOntology) {
  const ClusterSet clusters = ClusterVetoConstrained(edges_);
  const Fuser fuser(&bundle_, &props_);
  const onto::EntityTypeDef* port = bundle_.ontology().Type("Port");

  int merged_entities = 0, checked_union = 0, checked_trust = 0;
  for (const auto& cluster : clusters.clusters) {
    if (cluster.size() < 2) continue;
    std::vector<const onto::SourceRecord*> records;
    for (const auto& member : cluster) {
      const auto it = records_.find(member);
      if (it != records_.end()) records.push_back(&it->second);
    }
    if (records.empty() || records.front()->type != port->id) continue;
    ++merged_entities;

    const ResolvedEntity entity = fuser.Fuse(records, cluster, {"test"});
    EXPECT_EQ(port->id, entity.type);
    EXPECT_EQ(cluster.size(), entity.members.size());
    EXPECT_FALSE(entity.properties.empty());
    // Every property must carry provenance. A value with no account of where
    // it came from is the thing this project exists to prevent.
    EXPECT_EQ(entity.properties.size(), entity.provenance.size());

    for (const auto& provenance : entity.provenance) {
      EXPECT_NE(0u, provenance.origin.source_id);
      EXPECT_FALSE(provenance.origin.column.empty());
      EXPECT_EQ(cluster.size(), provenance.cluster_size);

      const onto::PropertyDef* def = port->Property(provenance.prop);
      ASSERT_NE(nullptr, def);
      EXPECT_EQ(def->fuse, provenance.rule);

      if (def->fuse == onto::FusionRule::kUnion) {
        ++checked_union;
        const onto::TValue* value = entity.Property(provenance.prop);
        ASSERT_NE(nullptr, value);
        // A list fused by union keeps everything; picking one value would
        // discard information the schema explicitly said to keep.
        EXPECT_EQ(onto::ValueType::kStringList, value->type());
      }
      if (def->fuse == onto::FusionRule::kMostTrusted &&
          !provenance.rejected.empty()) {
        ++checked_trust;
        // The losers carry a reason, which is what makes the lineage panel an
        // answer rather than an assertion.
        EXPECT_NE(std::string::npos,
                  provenance.rejected.front().reason.find("trust"))
            << provenance.rejected.front().reason;
        EXPECT_FALSE(provenance.rejected.front().column.empty());
      }
    }
  }
  EXPECT_GT(merged_entities, 20);
  EXPECT_GT(checked_union, 0);
  EXPECT_GT(checked_trust, 0);
}

TEST_F(ResolutionTest, MedianResistsOneBadCoordinate) {
  // Three records for one port, one of which reports a coordinate in the wrong
  // place. The median moves by one position; a mean would move halfway to it.
  const onto::EntityTypeDef* port = bundle_.ontology().Type("Port");
  const codec::PropId lat = port->Property("lat")->id;
  ASSERT_EQ(onto::FusionRule::kNumericMedian, port->Property("lat")->fuse);

  std::vector<onto::SourceRecord> records;
  for (const double value : {51.92, 51.94, 0.0}) {
    onto::SourceRecord record;
    record.type = port->id;
    record.source_id = 1;
    onto::PropertyCell cell;
    cell.prop = lat;
    cell.value = onto::TValue::Double(value);
    cell.origin.source_id = 1;
    cell.origin.column = "Latitude";
    record.properties.push_back(std::move(cell));
    records.push_back(std::move(record));
  }
  std::vector<const onto::SourceRecord*> pointers;
  for (const auto& record : records) pointers.push_back(&record);

  const Fuser fuser(&bundle_, &props_);
  const ResolvedEntity entity = fuser.Fuse(pointers, {}, {});
  const onto::TValue* value = entity.Property(lat);
  ASSERT_NE(nullptr, value);
  EXPECT_DOUBLE_EQ(51.92, value->AsDouble())
      << "the median of {0.0, 51.92, 51.94} is 51.92; a mean would give 34.6";
}

TEST_F(ResolutionTest, EntitiesAndProvenanceRoundTripThroughStorage) {
  const ClusterSet clusters = ClusterVetoConstrained(edges_);
  const Fuser fuser(&bundle_, &props_);

  int written = 0;
  for (const auto& cluster : clusters.clusters) {
    if (cluster.size() < 2) continue;
    std::vector<const onto::SourceRecord*> records;
    for (const auto& member : cluster) {
      const auto it = records_.find(member);
      if (it != records_.end()) records.push_back(&it->second);
    }
    if (records.empty()) continue;

    const ResolvedEntity entity = fuser.Fuse(records, cluster, {"merged"});
    int operations = 0;
    ASSERT_TRUE(WriteEntity(store_.get(), bundle_, entity, &operations).ok());
    // Entity + one cross-reference per member + indexes + provenance, all in
    // one batch. A crash between them would leave a value with no account of
    // where it came from.
    EXPECT_GE(operations, 1 + static_cast<int>(cluster.size()) +
                              static_cast<int>(entity.provenance.size()));

    std::string payload;
    ASSERT_TRUE(store_->GetEntity(entity.type, entity.id, &payload).ok());
    lsm::Slice slice(payload);
    ResolvedEntity decoded;
    ASSERT_TRUE(ResolvedEntity::DecodeFrom(&slice, &decoded));
    EXPECT_TRUE(slice.empty());
    EXPECT_EQ(entity.type, decoded.type);
    EXPECT_EQ(entity.properties.size(), decoded.properties.size());
    EXPECT_EQ(entity.members.size(), decoded.members.size());

    // Every source row that fed the entity can be found from it.
    for (const auto& member : entity.members) {
      codec::Ulid resolved;
      ASSERT_TRUE(
          store_->LookupCrossRef(member.source, member.key_hash, &resolved).ok());
      EXPECT_EQ(entity.id, resolved);
    }

    // And the provenance is readable back.
    int provenance_count = 0;
    for (auto it = store_->ScanProvenance(entity.id); it->Valid(); it->Next()) {
      lsm::Slice value = it->value();
      Provenance provenance;
      ASSERT_TRUE(Provenance::DecodeFrom(&value, &provenance));
      EXPECT_TRUE(value.empty());
      EXPECT_FALSE(provenance.emitted_value.empty());
      ++provenance_count;
    }
    EXPECT_EQ(static_cast<int>(entity.provenance.size()), provenance_count);

    if (++written >= 15) break;  // a sample is enough to catch a systematic fault
  }
  EXPECT_GT(written, 5);
}
