// The query engine, tested against the real pipeline output.
//
// WHAT THESE TESTS ARE ACTUALLY ASSERTING
//
// Not "the executor returns rows". Any implementation returns rows. The claims
// worth making, and the ones an interviewer would poke at, are:
//
//   1. The planner picks the index it says it picks, and picks a DIFFERENT one
//      when the schema stops supporting it.
//   2. The cost numbers are real. `keys_scanned` for the quarter query is close
//      to the number of results, which is only true if TIDX turned the window
//      into a range rather than a scan-and-filter.
//   3. The same question asked without the index gives the SAME ANSWER at a
//      much higher cost. That is the strongest form: it proves the index is an
//      optimisation and not a semantic filter that quietly drops rows.
//   4. A snapshot taken at the start of a request survives a concurrent write.
//
// Point 3 is the one that would catch a real bug. A time-range scan with a
// wrong bound returns fewer rows and looks fast, and every test that only
// checks "fast" and "non-empty" passes.

#include "execute.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "corpus.h"
#include "plan.h"
#include "query.h"

namespace {

namespace onto = sextant::ontology;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;
namespace query = sextant::query;

using sextant::testsupport::Corpus;

class QueryTest : public ::testing::Test {
 protected:
  static Corpus corpus_;

  static void SetUpTestSuite() {
    ASSERT_TRUE(sextant::testsupport::BuildCorpus("querytest", &corpus_));
    ASSERT_GT(corpus_.entities_written, 300u);
  }
  static void TearDownTestSuite() {
    corpus_.store.reset();
    corpus_.Destroy();
  }

  static query::Executor Exec() {
    return query::Executor(corpus_.store.get(), &corpus_.bundle);
  }

  // The port every arrival test hangs off. Resolved by locode rather than
  // hardcoded, because entity ids are ULIDs and change on every run.
  static bool RotterdamId(std::string* id) {
    query::Query q;
    q.start.type = "Port";
    query::Predicate pred;
    pred.property = "locode";
    pred.op = query::CompareOp::kEq;
    pred.value = onto::TValue::String("NLRTM");
    q.start.filter.push_back(pred);

    query::QueryResult result;
    if (!Exec().Run(q, &result).ok() || result.entities.empty()) return false;
    *id = result.entities[0].id.ToString();
    return true;
  }
};

Corpus QueryTest::corpus_;

// ============================================================================
// THE PLANNER
// ============================================================================

TEST_F(QueryTest, AnIndexedEqualityBecomesAnIndexRangeRatherThanAScan) {
  query::Query q;
  q.start.type = "Port";
  query::Predicate pred;
  pred.property = "locode";
  pred.op = query::CompareOp::kEq;
  pred.value = onto::TValue::String("NLRTM");
  q.start.filter.push_back(pred);

  query::Plan plan;
  ASSERT_TRUE(query::Planner(&corpus_.bundle).Build(q, &plan).ok());

  EXPECT_EQ(plan.steps.size(), 1u);
  EXPECT_EQ(plan.steps[0].path, query::AccessPath::kIndexExact);
  EXPECT_EQ(plan.steps[0].property, "locode");
  EXPECT_TRUE(plan.warnings.empty());
  // The reason has to name the property, because "add an index" is the fix and
  // a reason that does not say which one is not actionable.
  EXPECT_NE(plan.steps[0].reason.find("locode"), std::string::npos);
}

TEST_F(QueryTest, AFilterWithNoUsableIndexIsAdmittedAsAFullScan) {
  // `harbor_size` is a real property with no index in the ontology. A planner
  // that quietly scanned here would still be correct and would be a hundred
  // times slower with nothing to show for it.
  //
  // This test found the schema gap that put `indexed: true` on Port.name: the
  // first version used `name`, expecting an index, and the planner correctly
  // said there was not one. The search box was going to be a full scan.
  query::Query q;
  q.start.type = "Port";
  query::Predicate pred;
  pred.property = "harbor_size";
  pred.op = query::CompareOp::kEq;
  pred.value = onto::TValue::String("L");
  q.start.filter.push_back(pred);

  query::Plan plan;
  ASSERT_TRUE(query::Planner(&corpus_.bundle).Build(q, &plan).ok());

  EXPECT_EQ(plan.steps[0].path, query::AccessPath::kEntityScan);
  EXPECT_EQ(plan.steps[0].residuals.size(), 1u);
  ASSERT_FALSE(plan.warnings.empty());
  EXPECT_NE(plan.warnings[0].find("full scan"), std::string::npos);
}

TEST_F(QueryTest, OnlyOnePredicateIsPushedDownAndTheRestBecomeResiduals) {
  query::Query q;
  q.start.type = "Port";
  for (const auto& [property, value] :
       std::vector<std::pair<std::string, std::string>>{{"locode", "NLRTM"},
                                                        {"country", "NL"}}) {
    query::Predicate pred;
    pred.property = property;
    pred.op = query::CompareOp::kEq;
    pred.value = onto::TValue::String(value);
    q.start.filter.push_back(pred);
  }

  query::Plan plan;
  ASSERT_TRUE(query::Planner(&corpus_.bundle).Build(q, &plan).ok());

  EXPECT_EQ(plan.steps[0].path, query::AccessPath::kIndexExact);
  EXPECT_EQ(plan.steps[0].property, "locode");
  ASSERT_EQ(plan.steps[0].residuals.size(), 1u);
  EXPECT_EQ(plan.steps[0].residuals[0].property, "country");
}

TEST_F(QueryTest, ATimeWindowOnATimeIndexedLinkChoosesTheTimeIndex) {
  query::Query q;
  q.start.type = "Port";
  query::Hop hop;
  hop.link = "arrivals";
  hop.reverse = true;
  hop.when.present = true;
  ASSERT_TRUE(onto::ParseIso8601("2026-04-01T00:00:00Z", &hop.when.from_inclusive));
  ASSERT_TRUE(onto::ParseIso8601("2026-07-01T00:00:00Z", &hop.when.to_exclusive));
  q.hops.push_back(hop);

  query::Plan plan;
  ASSERT_TRUE(query::Planner(&corpus_.bundle).Build(q, &plan).ok());

  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[1].path, query::AccessPath::kTimeIndex);
  EXPECT_EQ(plan.headline, query::AccessPath::kTimeIndex);
  EXPECT_TRUE(plan.warnings.empty());
}

TEST_F(QueryTest, TheSameHopWithoutATimeWindowFallsBackToAdjacency) {
  query::Query q;
  q.start.type = "Port";
  query::Hop hop;
  hop.link = "arrivals";
  hop.reverse = true;
  q.hops.push_back(hop);

  query::Plan plan;
  ASSERT_TRUE(query::Planner(&corpus_.bundle).Build(q, &plan).ok());
  EXPECT_EQ(plan.steps[1].path, query::AccessPath::kLinkIn);
}

TEST_F(QueryTest, AHopThatDoesNotStartWhereThePreviousOneEndedIsRejected) {
  // Ports do not have an `operated_by` link; Voyages do. Catching this at plan
  // time rather than returning zero rows is the difference between an error
  // and a silent wrong answer.
  query::Query q;
  q.start.type = "Port";
  query::Hop hop;
  hop.link = "operated_by";
  q.hops.push_back(hop);

  query::Plan plan;
  const auto status = query::Planner(&corpus_.bundle).Build(q, &plan);
  EXPECT_TRUE(status.IsInvalidArgument()) << status.ToString();
  EXPECT_NE(status.ToString().find("Port"), std::string::npos);
}

TEST_F(QueryTest, AnUnknownPropertyIsARequestErrorRatherThanAnEmptyResult) {
  query::Query q;
  q.start.type = "Port";
  query::Predicate pred;
  pred.property = "not_a_property";
  pred.value = onto::TValue::String("x");
  q.start.filter.push_back(pred);

  query::Plan plan;
  EXPECT_TRUE(query::Planner(&corpus_.bundle).Build(q, &plan).IsInvalidArgument());
}

// ============================================================================
// THE EXECUTOR
// ============================================================================

TEST_F(QueryTest, AnIndexLookupReadsFarFewerKeysThanTheEquivalentScan) {
  query::Query indexed;
  indexed.start.type = "Port";
  query::Predicate by_locode;
  by_locode.property = "locode";
  by_locode.op = query::CompareOp::kEq;
  by_locode.value = onto::TValue::String("NLRTM");
  indexed.start.filter.push_back(by_locode);

  query::QueryResult fast;
  ASSERT_TRUE(Exec().Run(indexed, &fast).ok());
  ASSERT_EQ(fast.entities.size(), 1u);

  // The same question with no filter at all, so the executor has to scan.
  query::Query scanned;
  scanned.start.type = "Port";
  scanned.limit = 10000;
  query::QueryResult slow;
  ASSERT_TRUE(Exec().Run(scanned, &slow).ok());

  std::printf("  indexed: %llu keys for 1 result;  scan: %llu keys for %llu\n",
              static_cast<unsigned long long>(fast.cost.keys_scanned),
              static_cast<unsigned long long>(slow.cost.keys_scanned),
              static_cast<unsigned long long>(slow.entities.size()));

  EXPECT_GT(slow.entities.size(), 50u);
  // The point of the index, stated as a number.
  EXPECT_LT(fast.cost.keys_scanned, slow.cost.keys_scanned / 10);
}

TEST_F(QueryTest, TheQuarterQueryScansAboutAsManyKeysAsItReturns) {
  std::string rotterdam;
  ASSERT_TRUE(RotterdamId(&rotterdam));

  query::Query q;
  q.start.type = "Port";
  q.start.ids.push_back(rotterdam);

  query::Hop hop;
  hop.link = "arrivals";
  hop.reverse = true;
  hop.when.present = true;
  ASSERT_TRUE(onto::ParseIso8601("2026-04-01T00:00:00Z", &hop.when.from_inclusive));
  ASSERT_TRUE(onto::ParseIso8601("2026-07-01T00:00:00Z", &hop.when.to_exclusive));
  q.hops.push_back(hop);
  q.limit = 10000;

  query::QueryResult result;
  ASSERT_TRUE(Exec().Run(q, &result).ok());

  std::printf("  %llu arrivals, %llu keys scanned, %llu us, index %s\n",
              static_cast<unsigned long long>(result.entities.size()),
              static_cast<unsigned long long>(result.cost.keys_scanned),
              static_cast<unsigned long long>(result.cost.elapsed_us),
              query::AccessPathName(result.cost.index_used));

  ASSERT_GT(result.entities.size(), 0u);
  EXPECT_EQ(result.cost.index_used, query::AccessPath::kTimeIndex);

  // THE CLAIM. A range scan touches one key per result plus the point lookups
  // that materialise them. A scan-and-filter would touch every arrival at this
  // port in all time. The factor of 3 is slack for the ENTITY reads, not for
  // the index being wrong.
  EXPECT_LE(result.cost.keys_scanned, result.entities.size() * 3 + 10);

  // The DoD from the execution plan.
  EXPECT_LT(result.cost.elapsed_us, 50000u);
}

TEST_F(QueryTest, TheTimeIndexAndTheFullScanAgreeOnWhichVoyagesAreInTheWindow) {
  // The test that would actually catch a broken bound. A time range that is
  // off by one at either end returns a plausible number of rows quickly, and
  // every "is it fast and non-empty" test passes.
  std::string rotterdam;
  ASSERT_TRUE(RotterdamId(&rotterdam));

  int64_t from = 0, to = 0;
  ASSERT_TRUE(onto::ParseIso8601("2026-04-01T00:00:00Z", &from));
  ASSERT_TRUE(onto::ParseIso8601("2026-07-01T00:00:00Z", &to));

  query::Query windowed;
  windowed.start.type = "Port";
  windowed.start.ids.push_back(rotterdam);
  query::Hop fast_hop;
  fast_hop.link = "arrivals";
  fast_hop.reverse = true;
  fast_hop.when.present = true;
  fast_hop.when.from_inclusive = from;
  fast_hop.when.to_exclusive = to;
  windowed.hops.push_back(fast_hop);
  windowed.limit = 10000;

  query::QueryResult via_index;
  ASSERT_TRUE(Exec().Run(windowed, &via_index).ok());
  ASSERT_EQ(via_index.cost.index_used, query::AccessPath::kTimeIndex);

  // Every arrival ever, then filtered in the test rather than by the engine.
  query::Query everything;
  everything.start.type = "Port";
  everything.start.ids.push_back(rotterdam);
  query::Hop slow_hop;
  slow_hop.link = "arrivals";
  slow_hop.reverse = true;
  everything.hops.push_back(slow_hop);
  everything.limit = 10000;

  query::QueryResult via_scan;
  ASSERT_TRUE(Exec().Run(everything, &via_scan).ok());
  // `index_used` reports the most interesting path in the plan, and both
  // queries resolve their start set by id, so both report POINT. What matters
  // here is that this one did NOT get the time index.
  ASSERT_NE(via_scan.cost.index_used, query::AccessPath::kTimeIndex);
  ASSERT_EQ(via_scan.plan.steps[1].path, query::AccessPath::kLinkIn);

  std::set<std::string> expected;
  for (const auto& entity : via_scan.entities) {
    for (const auto& [name, value] : entity.properties) {
      if (name != "arrived_at") continue;
      const int64_t ts = value.AsTimestamp();
      if (ts >= from && ts < to) expected.insert(entity.id.ToString());
    }
  }
  std::set<std::string> actual;
  for (const auto& entity : via_index.entities) actual.insert(entity.id.ToString());

  std::printf("  index %zu, scan-and-filter %zu, all arrivals %zu (%llu keys vs %llu)\n",
              actual.size(), expected.size(), via_scan.entities.size(),
              static_cast<unsigned long long>(via_index.cost.keys_scanned),
              static_cast<unsigned long long>(via_scan.cost.keys_scanned));

  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(actual, expected);
  // Same answer, and the index really did do less work to get it.
  EXPECT_LT(via_index.cost.keys_scanned, via_scan.cost.keys_scanned);
}

TEST_F(QueryTest, TwoHopsReachVesselsThroughVoyages) {
  std::string rotterdam;
  ASSERT_TRUE(RotterdamId(&rotterdam));

  query::Query q;
  q.start.type = "Port";
  q.start.ids.push_back(rotterdam);

  query::Hop arrivals;
  arrivals.link = "arrivals";
  arrivals.reverse = true;
  q.hops.push_back(arrivals);

  query::Hop operated;
  operated.link = "operated_by";
  q.hops.push_back(operated);
  q.limit = 10000;

  query::QueryResult result;
  ASSERT_TRUE(Exec().Run(q, &result).ok());

  ASSERT_GT(result.entities.size(), 0u);
  for (const auto& entity : result.entities) {
    EXPECT_EQ(entity.type_name, "Vessel");
    EXPECT_EQ(entity.depth, 2);
  }
  std::printf("  %llu distinct vessels called at Rotterdam\n",
              static_cast<unsigned long long>(result.entities.size()));
}

TEST_F(QueryTest, TheFrontierDeduplicatesSoAVesselWithTwoVoyagesAppearsOnce) {
  std::string rotterdam;
  ASSERT_TRUE(RotterdamId(&rotterdam));

  query::Query q;
  q.start.type = "Port";
  q.start.ids.push_back(rotterdam);
  query::Hop arrivals;
  arrivals.link = "arrivals";
  arrivals.reverse = true;
  q.hops.push_back(arrivals);
  query::Hop operated;
  operated.link = "operated_by";
  q.hops.push_back(operated);
  q.limit = 10000;

  query::QueryResult result;
  ASSERT_TRUE(Exec().Run(q, &result).ok());

  std::set<std::string> unique;
  for (const auto& entity : result.entities) unique.insert(entity.id.ToString());
  EXPECT_EQ(unique.size(), result.entities.size());
}

TEST_F(QueryTest, AHopBoundStopsTheTraversalAndSaysSo) {
  std::string rotterdam;
  ASSERT_TRUE(RotterdamId(&rotterdam));

  query::Query q;
  q.start.type = "Port";
  q.start.ids.push_back(rotterdam);
  query::Hop hop;
  hop.link = "arrivals";
  hop.reverse = true;
  hop.max_expand = 3;  // deliberately tiny
  q.hops.push_back(hop);
  q.limit = 10000;

  query::QueryResult result;
  ASSERT_TRUE(Exec().Run(q, &result).ok());

  EXPECT_TRUE(result.cost.truncated);
  EXPECT_FALSE(result.cost.truncated_at.empty());
  EXPECT_LE(result.entities.size(), 3u);
}

TEST_F(QueryTest, APrefixSearchIsARangeScanRatherThanAScanAndFilter) {
  query::Query q;
  q.start.type = "Port";
  query::Predicate pred;
  pred.property = "name";
  pred.op = query::CompareOp::kStartsWith;
  pred.value = onto::TValue::String("Rott");
  q.start.filter.push_back(pred);
  q.limit = 100;

  query::QueryResult result;
  ASSERT_TRUE(Exec().Run(q, &result).ok());

  ASSERT_GT(result.entities.size(), 0u);
  EXPECT_EQ(result.cost.index_used, query::AccessPath::kIndexPrefix);
  for (const auto& entity : result.entities) {
    bool matched = false;
    for (const auto& [name, value] : entity.properties) {
      if (name == "name" && value.AsString().rfind("Rott", 0) == 0) matched = true;
    }
    EXPECT_TRUE(matched) << entity.display;
  }

  // A full scan of Port for comparison. If the prefix search were secretly a
  // scan, these two numbers would be the same.
  query::Query all;
  all.start.type = "Port";
  all.limit = 10000;
  query::QueryResult scanned;
  ASSERT_TRUE(Exec().Run(all, &scanned).ok());
  EXPECT_LT(result.cost.keys_scanned, scanned.cost.keys_scanned);
}

// ============================================================================
// COST ACCOUNTING
// ============================================================================

TEST_F(QueryTest, TheCostBelongsToTheQueryAndNotToTheProcess) {
  // Global stats would keep climbing across requests. A per-request sink means
  // the same query costs the same twice in a row, which is what makes the
  // number usable as a regression signal.
  query::Query q;
  q.start.type = "Port";
  query::Predicate pred;
  pred.property = "locode";
  pred.op = query::CompareOp::kEq;
  pred.value = onto::TValue::String("DEHAM");
  q.start.filter.push_back(pred);

  query::QueryResult first, second;
  ASSERT_TRUE(Exec().Run(q, &first).ok());
  ASSERT_TRUE(Exec().Run(q, &second).ok());

  EXPECT_EQ(first.cost.keys_scanned, second.cost.keys_scanned);
  EXPECT_EQ(first.cost.entities_materialised, second.cost.entities_materialised);
  EXPECT_GT(first.cost.entities_materialised, 0u);
}

TEST_F(QueryTest, AQueryForSomethingAbsentCostsAlmostNothing) {
  query::Query q;
  q.start.type = "Port";
  query::Predicate pred;
  pred.property = "locode";
  pred.op = query::CompareOp::kEq;
  pred.value = onto::TValue::String("ZZZZZ");
  q.start.filter.push_back(pred);

  query::QueryResult result;
  ASSERT_TRUE(Exec().Run(q, &result).ok());

  EXPECT_EQ(result.entities.size(), 0u);
  EXPECT_EQ(result.cost.entities_materialised, 0u);
  EXPECT_LT(result.cost.keys_scanned, 5u);
}

// ============================================================================
// SNAPSHOT ISOLATION
// ============================================================================

TEST_F(QueryTest, ASnapshotTakenAtTheStartOfARequestIgnoresALaterWrite) {
  // The reason a traversal pins a snapshot at all: a multi-hop walk takes
  // milliseconds, and an ingest running alongside it must not be able to make
  // hop 2 disagree with hop 1.
  codec::SnapshotHandle snapshot = corpus_.store->NewSnapshot();
  codec::ReadContext ctx;
  ctx.snapshot = snapshot.get();

  const onto::EntityTypeDef* port = corpus_.bundle.ontology().Type("Port");
  ASSERT_NE(port, nullptr);

  uint64_t before = 0;
  {
    auto iter = corpus_.store->ScanEntities(port->id, ctx);
    for (; iter->Valid(); iter->Next()) ++before;
  }
  ASSERT_GT(before, 0u);

  // Write a new Port after the snapshot was taken.
  const codec::Ulid intruder = codec::Ulid::Generate();
  sextant::resolve::ResolvedEntity entity;
  entity.id = intruder;
  entity.type = port->id;
  std::string payload;
  entity.EncodeTo(&payload);
  ASSERT_TRUE(corpus_.store->EditEntity(port->id, intruder)
                  .SetPayload(lsm::Slice(payload))
                  .Commit()
                  .ok());

  uint64_t after_on_snapshot = 0;
  {
    auto iter = corpus_.store->ScanEntities(port->id, ctx);
    for (; iter->Valid(); iter->Next()) ++after_on_snapshot;
  }
  EXPECT_EQ(after_on_snapshot, before) << "the snapshot saw a later write";

  uint64_t after_live = 0;
  {
    auto iter = corpus_.store->ScanEntities(port->id);
    for (; iter->Valid(); iter->Next()) ++after_live;
  }
  EXPECT_EQ(after_live, before + 1) << "a live read did not see the write";
}

}  // namespace
