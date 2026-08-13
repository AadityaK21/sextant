// Blocking, end to end over the ingested corpus.
//
// The two assertions that matter are the reduction ratio and the pair
// completeness, and of the two, PAIR COMPLETENESS IS THE ONE TO WATCH. It is a
// ceiling on the recall of everything downstream: a true pair that blocking
// misses is never scored, never clustered and never merged, however good the
// scorer becomes.
//
// So the threshold here is a real regression gate, not a formality. If a change
// to the normalizer or the keys drops PC, this test fails before anyone gets to
// wonder why the F1 moved.

#include "blocking.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "bundle.h"
#include "csv.h"
#include "env.h"
#include "golden.h"
#include "ingest.h"
#include "json_source.h"
#include "store.h"

using namespace sextant::resolve;

namespace onto = sextant::ontology;
namespace conn = sextant::connectors;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;

namespace {

std::string SourceDir() { return std::string(SEXTANT_SOURCE_DIR); }

// Portable enough for a test fixture, and the only property that matters is
// that two concurrently running processes disagree.
unsigned long GetCurrentProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

// Ingests the whole committed corpus once, then blocks it. Shared across the
// tests in this file because ingesting five sources per test would dominate the
// runtime for no extra coverage.
class BlockingTest : public ::testing::Test {
 protected:
  static std::string dbname_;
  static std::unique_ptr<codec::Store> store_;
  static onto::SchemaBundle bundle_;
  static ResolverProperties props_;
  static std::vector<CandidatePairRef> pairs_;
  static BlockingReport report_;

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
    // A name unique to this PROCESS, not to this test.
    //
    // gtest_discover_tests registers every TEST_F as its own ctest entry, and
    // ctest -j runs them in parallel as separate processes - each of which runs
    // this whole fixture. A shared directory name means the second process to
    // start finds the LOCK held, Store::Open fails, and every test in it
    // segfaults on a null store_. That looks like a memory bug in the blocker
    // and is really a filename collision.
    dbname_ = "blockingtest_db_" + std::to_string(GetCurrentProcessId());
    Destroy();

    lsm::Options options;
    options.create_if_missing = true;
    const Status open = codec::Store::Open(options, dbname_, &store_);
    ASSERT_TRUE(open.ok()) << open.ToString();
    ASSERT_TRUE(
        onto::SchemaBundle::LoadFromDir(SourceDir() + "/schema", &bundle_).ok());
    ASSERT_TRUE(ResolverProperties::Resolve(bundle_.ontology(), &props_).ok());

    conn::Ingestor ingestor(store_.get(), &bundle_.ontology(),
                            &bundle_.transforms());

    for (const char* key : {"wpi", "unlocode"}) {
      const onto::SourceSpec* spec = bundle_.Source(key);
      ASSERT_NE(nullptr, spec);
      const std::string path = SourceDir() + "/" + spec->uri;
      std::unique_ptr<conn::CsvReader> reader;
      ASSERT_TRUE(conn::CsvReader::Open(path, &reader).ok()) << path;
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
      const onto::SourceSpec* spec = bundle_.Source(source_key);
      ASSERT_NE(nullptr, spec) << source_key;
      std::string body;
      ASSERT_TRUE(fetcher.Fetch(endpoint, "/", &body).ok()) << endpoint;
      std::unique_ptr<conn::JsonRowSource> rows;
      ASSERT_TRUE(conn::JsonRowSource::Open(body, "", endpoint, &rows).ok());
      conn::Ingestor::Result result;
      ASSERT_TRUE(ingestor.Run(*spec, rows.get(), 0, {}, &result).ok());
    }

    Blocker blocker(store_.get(), &bundle_, &props_);
    ASSERT_TRUE(blocker.IndexAll(&report_).ok());
    ASSERT_TRUE(blocker.GenerateCandidates({}, &pairs_, &report_).ok());
  }

  static void TearDownTestSuite() {
    pairs_.clear();
    store_.reset();
    Destroy();
  }

  static Status LoadGolden(const char* file, GoldenSet* out) {
    return GoldenSet::LoadFromFile(SourceDir() + "/eval/" + file, bundle_, out);
  }
};

std::string BlockingTest::dbname_;
std::unique_ptr<codec::Store> BlockingTest::store_;
onto::SchemaBundle BlockingTest::bundle_;
ResolverProperties BlockingTest::props_;
std::vector<CandidatePairRef> BlockingTest::pairs_;
BlockingReport BlockingTest::report_;

}  // namespace

TEST_F(BlockingTest, ResolvesEveryPropertyItNeedsFromTheOntology) {
  EXPECT_NE(0, props_.port_type);
  EXPECT_NE(0, props_.port_locode);
  EXPECT_NE(0, props_.port_lat);
  EXPECT_NE(0, props_.vessel_type);
  EXPECT_NE(0, props_.vessel_imo);
  EXPECT_NE(0, props_.vessel_mmsi);

  // A schema missing a property the resolver needs must fail loudly. Silently
  // disabling one blocking key would only show up as a slightly lower pair
  // completeness, with nothing pointing at the cause.
  onto::Ontology minimal;
  ASSERT_TRUE(onto::Ontology::LoadFromString(R"YAML(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string } }
  Vessel:
    id: 2
    properties: { name: { id: 2, type: string } }
)YAML",
                                             &minimal, "<test>")
                  .ok());
  ResolverProperties broken;
  const Status s = ResolverProperties::Resolve(minimal, &broken);
  EXPECT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("locode")) << s.ToString();
}

TEST_F(BlockingTest, IndexedEveryPortAndVessel) {
  // Voyages produce no blocking keys - they are resolved through their links,
  // not by comparing attributes - so the indexed count is ports plus vessels.
  EXPECT_GT(report_.records_indexed, 400u);
  EXPECT_GT(report_.keys_written, report_.records_indexed)
      << "each record should produce several keys";
  EXPECT_GT(report_.blocks, 100u);
}

// Reduction ratio: how much of the quadratic work was avoided.
//
// Worth knowing before quoting it: RR is a function of corpus SIZE as much as
// of the blocking scheme. The same keys over the full 110,000-row UN/LOCODE
// download would report a much higher number simply because the denominator
// grows quadratically while the candidate set grows roughly linearly. It is
// meaningful as a comparison between schemes on one corpus, and misleading as
// an absolute figure across corpora.
TEST_F(BlockingTest, ReductionRatioIsHigh) {
  EXPECT_GT(report_.possible_pairs, 60000u);
  EXPECT_LT(report_.candidate_pairs, report_.possible_pairs / 50);
  EXPECT_GT(report_.reduction_ratio, 0.98)
      << report_.candidate_pairs << " candidates from " << report_.possible_pairs
      << " possible pairs";
}

// THE CEILING TEST. Everything downstream can only lose recall from here.
TEST_F(BlockingTest, PairCompletenessClearsTheThresholdForPorts) {
  GoldenSet golden;
  const Status s = LoadGolden("golden_ports.csv", &golden);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_GT(golden.match_count(), 250u);
  EXPECT_GT(golden.non_match_count(), 500u)
      << "a golden set of mostly matches cannot measure precision";

  BlockingReport measured = report_;
  MeasureAgainstGolden(pairs_, golden, 25, &measured);

  EXPECT_GT(measured.pair_completeness, 0.98)
      << measured.golden_matches_covered << " of " << measured.golden_matches
      << " true pairs were blocked together";

  // Every missed pair is recall the resolver can never recover, so the report
  // names them rather than only counting.
  for (const auto& [a, b] : measured.missed) {
    std::printf("    missed: %s <-> %s\n", a.c_str(), b.c_str());
  }
}

TEST_F(BlockingTest, PairCompletenessClearsTheThresholdForVessels) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_vessels.csv", &golden).ok());

  BlockingReport measured = report_;
  MeasureAgainstGolden(pairs_, golden, 25, &measured);
  EXPECT_GT(measured.pair_completeness, 0.98)
      << measured.golden_matches_covered << " of " << measured.golden_matches;
}

// The pairs whose MMSI disagrees by one digit are the ones a single-key scheme
// would lose. They are in the corpus specifically to prove the other keys pick
// them up.
TEST_F(BlockingTest, CatchesPairsWhoseStrongestWeakIdentifierDisagrees) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_vessels.csv", &golden).ok());

  int checked = 0;
  for (const auto& labeled : golden.pairs()) {
    if (!labeled.is_match) continue;
    // "digitraffic:230100168" vs "digitraffic_ais:230100169" - same hull, the
    // transmitter got a digit wrong.
    const std::string key_a = labeled.text_a.substr(labeled.text_a.find(':') + 1);
    const std::string key_b = labeled.text_b.substr(labeled.text_b.find(':') + 1);
    if (key_a == key_b) continue;
    ++checked;

    bool found = false;
    for (const auto& candidate : pairs_) {
      if (candidate.pair == labeled.pair) {
        found = true;
        // And it was NOT the MMSI key that caught it, since the MMSIs differ.
        for (const auto& via : candidate.via) {
          EXPECT_NE("mmsi_exact", via);
        }
        break;
      }
    }
    EXPECT_TRUE(found) << labeled.text_a << " <-> " << labeled.text_b;
  }
  EXPECT_GT(checked, 0) << "the corpus lost its MMSI transcription slips";
}

// Attribution. A key that never uniquely catches a true pair is a key that is
// paying for itself in candidate comparisons and returning nothing.
TEST_F(BlockingTest, ReportsWhichKeyEarnedWhichRecall) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_ports.csv", &golden).ok());
  BlockingReport measured = report_;
  MeasureAgainstGolden(pairs_, golden, 25, &measured);

  EXPECT_FALSE(measured.pairs_by_key.empty());
  EXPECT_GT(measured.matches_by_key["locode_exact"], 100u)
      << "the code is the strongest key when it is present";
  EXPECT_GT(measured.matches_by_key["name_prefix"], 100u);

  // Every candidate carries at least one generator name, or the attribution
  // table is quietly lying about where pairs came from.
  for (const auto& candidate : pairs_) {
    EXPECT_FALSE(candidate.via.empty());
  }
}

// Purging is the honest response to a key that stops discriminating: drop the
// block, and let the report say so, rather than spending quadratic time inside
// one key.
TEST_F(BlockingTest, PurgingSmallBlocksCostsRecallVisibly) {
  Blocker blocker(store_.get(), &bundle_, &props_);
  Blocker::Options tiny;
  tiny.max_block_size = 2;

  std::vector<CandidatePairRef> fewer;
  BlockingReport purged;
  ASSERT_TRUE(blocker.GenerateCandidates(tiny, &fewer, &purged).ok());

  EXPECT_GT(purged.purged_blocks, 0u);
  EXPECT_LT(fewer.size(), pairs_.size());

  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_ports.csv", &golden).ok());
  BlockingReport measured_full = report_, measured_purged = purged;
  MeasureAgainstGolden(pairs_, golden, 25, &measured_full);
  MeasureAgainstGolden(fewer, golden, 25, &measured_purged);

  // The point: the loss is measurable rather than silent.
  EXPECT_LT(measured_purged.pair_completeness, measured_full.pair_completeness);
}

TEST_F(BlockingTest, CandidateGenerationIsDeterministic) {
  Blocker blocker(store_.get(), &bundle_, &props_);
  std::vector<CandidatePairRef> again;
  BlockingReport report;
  ASSERT_TRUE(blocker.GenerateCandidates({}, &again, &report).ok());

  ASSERT_EQ(pairs_.size(), again.size());
  for (size_t i = 0; i < pairs_.size(); ++i) {
    EXPECT_TRUE(pairs_[i].pair == again[i].pair) << "at index " << i;
    EXPECT_EQ(pairs_[i].via, again[i].via);
  }
}

// --- the golden set itself --------------------------------------------------

TEST_F(BlockingTest, GoldenReferencesResolveToRealRecords) {
  GoldenSet golden;
  ASSERT_TRUE(LoadGolden("golden_ports.csv", &golden).ok());

  // A reference that does not correspond to an ingested record would make the
  // evaluation quietly measure nothing, so every one is checked against the
  // store.
  int checked = 0;
  for (const auto& labeled : golden.pairs()) {
    for (const RecordRef& ref : {labeled.pair.a, labeled.pair.b}) {
      std::string payload;
      ASSERT_TRUE(store_->GetSourceRecord(ref.source, ref.key_hash, &payload).ok())
          << "golden set names " << golden.Describe(ref)
          << ", which is not in the store";
      ++checked;
    }
    if (checked > 200) break;  // a sample is enough to catch a systematic error
  }
  EXPECT_GT(checked, 100);
}

TEST_F(BlockingTest, GoldenSetRejectsUnknownSources) {
  RecordRef ref;
  EXPECT_TRUE(GoldenSet::ParseRef(bundle_, "wpi:10000", &ref));
  EXPECT_EQ(bundle_.Source("wpi")->id, ref.source);

  // Multi-column natural keys are written with '|' and must hash exactly the
  // way the mapper hashes them.
  EXPECT_TRUE(GoldenSet::ParseRef(bundle_, "unlocode:NL|RTM", &ref));
  EXPECT_EQ(bundle_.Source("unlocode")->id, ref.source);

  EXPECT_FALSE(GoldenSet::ParseRef(bundle_, "nosuchsource:1", &ref));
  EXPECT_FALSE(GoldenSet::ParseRef(bundle_, "no-colon-here", &ref));
}
