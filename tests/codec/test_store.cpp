// The typed Store, exercised against the real engine.
//
// These tests use the maritime domain deliberately: they are the first place
// the project reads like the thing it is meant to be, rather than a key-value
// store with a test harness.

#include "store.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "env.h"

using namespace sextant::codec;
namespace lsm = sextant::lsm;

namespace {

constexpr TypeId kPort = 1;
constexpr TypeId kVessel = 2;
constexpr TypeId kVoyage = 3;

constexpr LinkTypeId kArrivesAt = 10;
constexpr LinkTypeId kDepartsFrom = 11;
constexpr LinkTypeId kOperatedBy = 12;

constexpr PropId kName = 1;
constexpr PropId kLocode = 2;
constexpr PropId kLatitude = 3;

// Milliseconds, so windows read like real quarters.
constexpr int64_t kQ2Start = 1'743'465'600'000LL;  // 2025-04-01
constexpr int64_t kQ3Start = 1'751'328'000'000LL;  // 2025-07-01

class StoreTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<Store> store_;

  void SetUp() override {
    dbname_ = std::string("storetest_") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name();
    Destroy();
    lsm::Options opts;
    opts.write_buffer_size = 64 * 1024;
    ASSERT_TRUE(Store::Open(opts, dbname_, &store_).ok());
  }

  void TearDown() override {
    store_.reset();
    Destroy();
  }

  void Destroy() {
    std::vector<std::string> children;
    if (lsm::GetChildren(dbname_, &children).ok()) {
      for (const auto& c : children) {
        if (c == "." || c == "..") continue;
        lsm::RemoveFile(dbname_ + "/" + c);
      }
    }
    std::remove(dbname_.c_str());
  }

  // Create a port with its name and code indexed.
  Ulid AddPort(const std::string& name, const std::string& locode, double lat) {
    auto writer = store_->NewEntity(kPort);
    const Ulid id = Ulid::Generate();
    auto w = store_->EditEntity(kPort, id);
    w.SetPayload(Slice(name))
        .AddIndexString(kName, Slice(name))
        .AddIndexString(kLocode, Slice(locode))
        .AddIndexDouble(kLatitude, lat);
    EXPECT_TRUE(w.Commit().ok());
    return id;
  }
};

}  // namespace

TEST_F(StoreTest, EntityRoundTrips) {
  const Ulid rotterdam = AddPort("Rotterdam", "NLRTM", 51.9225);

  std::string payload;
  ASSERT_TRUE(store_->GetEntity(kPort, rotterdam, &payload).ok());
  EXPECT_EQ("Rotterdam", payload);

  // A different type with the same id must not collide.
  EXPECT_TRUE(store_->GetEntity(kVessel, rotterdam, &payload).IsNotFound());
}

TEST_F(StoreTest, EntityScanReturnsCreationOrder) {
  const Ulid a = AddPort("Rotterdam", "NLRTM", 51.9);
  const Ulid b = AddPort("Hamburg", "DEHAM", 53.5);
  const Ulid c = AddPort("Antwerp", "BEANR", 51.2);

  std::vector<Ulid> found;
  for (auto it = store_->ScanEntities(kPort); it->Valid(); it->Next()) {
    TypeId type; Ulid id;
    ASSERT_TRUE(DecodeEntityKey(it->key(), &type, &id));
    EXPECT_EQ(kPort, type);
    found.push_back(id);
  }

  ASSERT_EQ(3u, found.size());
  EXPECT_EQ(a, found[0]);
  EXPECT_EQ(b, found[1]);
  EXPECT_EQ(c, found[2]) << "ULID ids should yield insertion order for free";
}

// A merge writes an entity, its links, its indexes and its provenance. If a
// crash could land in the middle, the ontology would describe something that
// never existed.
TEST_F(StoreTest, EntityCommitIsAtomic) {
  const Ulid port = AddPort("Rotterdam", "NLRTM", 51.9225);
  const Ulid vessel = Ulid::Generate();
  const Ulid voyage = Ulid::Generate();

  auto w = store_->EditEntity(kVoyage, voyage);
  w.SetPayload(Slice("voyage-1"))
      .AddTimedLink(kArrivesAt, port, kQ2Start + 1000, Slice("arrival"))
      .AddLink(kOperatedBy, vessel)
      .AddIndexString(kName, Slice("voyage-1"))
      .AddProvenance(kName, 1, Slice("wpi row 2841"))
      .AddCrossRef(/*source=*/7, /*source_pk_hash=*/0xABCD);

  // One entity, one timed link (which is 3 writes), one link (2 writes), one
  // index, one provenance record, one xref.
  EXPECT_EQ(9, w.operations()) << "everything must be in a single batch";
  ASSERT_TRUE(w.Commit().ok());

  // Every piece landed.
  std::string payload;
  EXPECT_TRUE(store_->GetEntity(kVoyage, voyage, &payload).ok());

  Ulid resolved;
  ASSERT_TRUE(store_->LookupCrossRef(7, 0xABCD, &resolved).ok());
  EXPECT_EQ(voyage, resolved);
}

// Links are written in both directions on purpose. Reverse traversal is the
// most common query in this domain and would otherwise be a full scan.
TEST_F(StoreTest, LinksTraverseBothWays) {
  const Ulid rotterdam = AddPort("Rotterdam", "NLRTM", 51.9225);
  const Ulid hamburg = AddPort("Hamburg", "DEHAM", 53.5);

  std::vector<Ulid> voyages;
  for (int i = 0; i < 5; ++i) {
    const Ulid v = Ulid::Generate();
    auto w = store_->EditEntity(kVoyage, v);
    w.SetPayload(Slice("v" + std::to_string(i)))
        .AddLink(kArrivesAt, rotterdam)
        .AddLink(kDepartsFrom, hamburg);
    ASSERT_TRUE(w.Commit().ok());
    voyages.push_back(v);
  }

  // Forward: what does this voyage arrive at?
  int forward = 0;
  for (auto it = store_->ScanOutgoing(voyages[0], kArrivesAt); it->Valid();
       it->Next()) {
    Ulid src, dst; LinkTypeId type;
    ASSERT_TRUE(DecodeLinkOutKey(it->key(), &src, &type, &dst));
    EXPECT_EQ(voyages[0], src);
    EXPECT_EQ(rotterdam, dst);
    ++forward;
  }
  EXPECT_EQ(1, forward);

  // Reverse: which voyages arrived at Rotterdam? This is the query LINKIN exists
  // for, and it is a prefix scan rather than a scan of every voyage.
  int reverse = 0;
  for (auto it = store_->ScanIncoming(rotterdam, kArrivesAt); it->Valid();
       it->Next()) {
    Ulid src, dst; LinkTypeId type;
    ASSERT_TRUE(DecodeLinkInKey(it->key(), &dst, &type, &src));
    EXPECT_EQ(rotterdam, dst);
    ++reverse;
  }
  EXPECT_EQ(5, reverse);

  // Departures from Hamburg are a separate link type and must not leak in.
  int departures = 0;
  for (auto it = store_->ScanIncoming(rotterdam, kDepartsFrom); it->Valid();
       it->Next()) {
    ++departures;
  }
  EXPECT_EQ(0, departures);
}

// THE headline query. "All voyages through Rotterdam last quarter" must be a
// seek plus a sequential read - not a scan of every voyage with a filter.
TEST_F(StoreTest, QuarterQueryIsAContiguousRangeScan) {
  const Ulid rotterdam = AddPort("Rotterdam", "NLRTM", 51.9225);
  const Ulid hamburg = AddPort("Hamburg", "DEHAM", 53.5);

  static constexpr int kInsideWindow = 40;
  static constexpr int kBeforeWindow = 500;
  static constexpr int kAfterWindow = 500;
  static constexpr int kOtherPort = 500;

  const int64_t day = 86'400'000LL;

  for (int i = 0; i < kBeforeWindow; ++i) {
    auto w = store_->EditEntity(kVoyage, Ulid::Generate());
    w.SetPayload(Slice("old")).AddTimedLink(kArrivesAt, rotterdam,
                                            kQ2Start - (i + 1) * day);
    ASSERT_TRUE(w.Commit().ok());
  }
  for (int i = 0; i < kInsideWindow; ++i) {
    auto w = store_->EditEntity(kVoyage, Ulid::Generate());
    w.SetPayload(Slice("hit")).AddTimedLink(kArrivesAt, rotterdam,
                                            kQ2Start + i * day);
    ASSERT_TRUE(w.Commit().ok());
  }
  for (int i = 0; i < kAfterWindow; ++i) {
    auto w = store_->EditEntity(kVoyage, Ulid::Generate());
    w.SetPayload(Slice("new")).AddTimedLink(kArrivesAt, rotterdam,
                                            kQ3Start + i * day);
    ASSERT_TRUE(w.Commit().ok());
  }
  for (int i = 0; i < kOtherPort; ++i) {
    auto w = store_->EditEntity(kVoyage, Ulid::Generate());
    w.SetPayload(Slice("other")).AddTimedLink(kArrivesAt, hamburg,
                                              kQ2Start + i * day);
    ASSERT_TRUE(w.Commit().ok());
  }

  auto it = store_->ScanTimeRange(kArrivesAt, rotterdam, kQ2Start, kQ3Start);

  int matched = 0;
  for (; it->Valid(); it->Next()) {
    LinkTypeId link; Ulid anchor, entity; int64_t ts;
    ASSERT_TRUE(DecodeTimeIndexKey(it->key(), &link, &anchor, &ts, &entity));
    EXPECT_EQ(kArrivesAt, link);
    EXPECT_EQ(rotterdam, anchor) << "another port leaked into the window";
    EXPECT_GE(ts, kQ2Start);
    EXPECT_LT(ts, kQ3Start);
    ++matched;
  }

  EXPECT_EQ(kInsideWindow, matched);

  // The point of the layout: the scan visited only the matching keys. It did
  // NOT walk the 1500 voyages outside the window and reject them.
  EXPECT_EQ(static_cast<uint64_t>(kInsideWindow), it->keys_scanned())
      << "scan touched " << it->keys_scanned() << " keys to return " << matched
      << " - that is a filter, not a range scan";
}

TEST_F(StoreTest, IndexLookupFindsEntitiesByValue) {
  const Ulid rotterdam = AddPort("Rotterdam", "NLRTM", 51.9225);
  AddPort("Hamburg", "DEHAM", 53.5);
  AddPort("Antwerp", "BEANR", 51.2);

  int hits = 0;
  Ulid found;
  for (auto it = store_->LookupString(kPort, kLocode, Slice("NLRTM")); it->Valid();
       it->Next()) {
    ASSERT_TRUE(DecodeIndexKeyEntity(it->key(), &found));
    ++hits;
  }

  EXPECT_EQ(1, hits);
  EXPECT_EQ(rotterdam, found);
}

TEST_F(StoreTest, NumericRangeQueryWorksAcrossTheEquator) {
  AddPort("Sydney", "AUSYD", -33.8688);
  AddPort("Singapore", "SGSIN", 1.3521);
  const Ulid antwerp = AddPort("Antwerp", "BEANR", 51.2194);
  const Ulid rotterdam = AddPort("Rotterdam", "NLRTM", 51.9225);
  AddPort("Bergen", "NOBGO", 60.3913);

  // Northern European band. A naive IEEE 754 key would misorder the southern
  // ports and this would return the wrong set.
  std::vector<Ulid> found;
  for (auto it = store_->RangeDouble(kPort, kLatitude, 50.0, 55.0); it->Valid();
       it->Next()) {
    Ulid id;
    ASSERT_TRUE(DecodeIndexKeyEntity(it->key(), &id));
    found.push_back(id);
  }

  ASSERT_EQ(2u, found.size());
  EXPECT_EQ(antwerp, found[0]) << "results should be ordered by latitude";
  EXPECT_EQ(rotterdam, found[1]);
}

TEST_F(StoreTest, RawRecordsAreRetrievableForLineage) {
  const std::string row = "3245,ROTTERDAM,NLRTM,51.9225,4.47917";
  ASSERT_TRUE(store_->PutRawRecord(/*source=*/1, /*batch=*/20260812, /*row=*/2841,
                                   Slice(row))
                  .ok());

  std::string fetched;
  ASSERT_TRUE(store_->GetRawRecord(1, 20260812, 2841, &fetched).ok());
  EXPECT_EQ(row, fetched) << "lineage depends on raw rows being byte-identical";
}

TEST_F(StoreTest, ProvenanceIsScannablePerProperty) {
  const Ulid port = Ulid::Generate();

  auto w = store_->EditEntity(kPort, port);
  w.SetPayload(Slice("Rotterdam"))
      .AddProvenance(kName, 100, Slice("wpi row 2841"))
      .AddProvenance(kName, 200, Slice("unlocode row 88213"))
      .AddProvenance(kLocode, 100, Slice("unlocode row 88213"));
  ASSERT_TRUE(w.Commit().ok());

  // Every property of this entity.
  int all = 0;
  for (auto it = store_->ScanProvenance(port); it->Valid(); it->Next()) ++all;
  EXPECT_EQ(3, all);

  // One property, with its versions in ascending order.
  std::vector<uint64_t> versions;
  for (auto it = store_->ScanProvenance(port, kName); it->Valid(); it->Next()) {
    Ulid entity; PropId prop; uint64_t version;
    ASSERT_TRUE(DecodeProvenanceKey(it->key(), &entity, &prop, &version));
    EXPECT_EQ(kName, prop);
    versions.push_back(version);
  }
  ASSERT_EQ(2u, versions.size());
  EXPECT_EQ(100u, versions[0]);
  EXPECT_EQ(200u, versions[1]);
}

TEST_F(StoreTest, BlockingIndexGroupsCandidates) {
  // Three records hashing into the same block, one into another.
  ASSERT_TRUE(store_->PutBlockingKey(0xAAAA, 1, 10).ok());
  ASSERT_TRUE(store_->PutBlockingKey(0xAAAA, 1, 11).ok());
  ASSERT_TRUE(store_->PutBlockingKey(0xAAAA, 2, 20).ok());
  ASSERT_TRUE(store_->PutBlockingKey(0xBBBB, 1, 30).ok());

  int members = 0;
  for (auto it = store_->ScanBlock(0xAAAA); it->Valid(); it->Next()) {
    uint64_t block; SourceId source; uint64_t record;
    ASSERT_TRUE(DecodeBlockingKey(it->key(), &block, &source, &record));
    EXPECT_EQ(0xAAAAu, block);
    ++members;
  }
  EXPECT_EQ(3, members) << "candidate generation reads exactly one block";
}

TEST_F(StoreTest, ReviewQueueReturnsMostConfidentPairsFirst) {
  ASSERT_TRUE(store_->PutCandidate(2.1, 1, Slice("weak")).ok());
  ASSERT_TRUE(store_->PutCandidate(4.9, 2, Slice("strong")).ok());
  ASSERT_TRUE(store_->PutCandidate(3.4, 3, Slice("middling")).ok());

  std::vector<double> scores;
  for (auto it = store_->ScanCandidates(); it->Valid(); it->Next()) {
    double score; uint64_t hash;
    ASSERT_TRUE(DecodeCandidateKey(it->key(), &score, &hash));
    scores.push_back(score);
  }

  ASSERT_EQ(3u, scores.size());
  EXPECT_DOUBLE_EQ(4.9, scores[0]);
  EXPECT_DOUBLE_EQ(3.4, scores[1]);
  EXPECT_DOUBLE_EQ(2.1, scores[2]);
}

// Everything must survive a restart, since the whole point of the engine
// underneath is durability.
TEST_F(StoreTest, StateSurvivesReopen) {
  const Ulid rotterdam = AddPort("Rotterdam", "NLRTM", 51.9225);
  const Ulid voyage = Ulid::Generate();

  auto w = store_->EditEntity(kVoyage, voyage);
  w.SetPayload(Slice("voyage-1"))
      .AddTimedLink(kArrivesAt, rotterdam, kQ2Start + 5000)
      .AddProvenance(kName, 1, Slice("digitraffic port_call 91"));
  ASSERT_TRUE(w.Commit(lsm::WriteOptions{true}).ok());

  store_.reset();
  lsm::Options opts;
  opts.write_buffer_size = 64 * 1024;
  ASSERT_TRUE(Store::Open(opts, dbname_, &store_).ok());

  std::string payload;
  EXPECT_TRUE(store_->GetEntity(kPort, rotterdam, &payload).ok());
  EXPECT_EQ("Rotterdam", payload);

  int arrivals = 0;
  for (auto it = store_->ScanTimeRange(kArrivesAt, rotterdam, kQ2Start, kQ3Start);
       it->Valid(); it->Next()) {
    ++arrivals;
  }
  EXPECT_EQ(1, arrivals);

  int provenance = 0;
  for (auto it = store_->ScanProvenance(voyage); it->Valid(); it->Next()) ++provenance;
  EXPECT_EQ(1, provenance);
}

// A prefix scan must stop at the end of its range even when the next keyspace
// begins immediately after it.
TEST_F(StoreTest, ScansDoNotLeakIntoAdjacentKeyspaces) {
  const Ulid port = AddPort("Rotterdam", "NLRTM", 51.9225);

  auto w = store_->EditEntity(kVoyage, Ulid::Generate());
  w.SetPayload(Slice("v")).AddLink(kArrivesAt, port);
  ASSERT_TRUE(w.Commit().ok());

  // Ports exist in ENTITY; links exist in LINKOUT/LINKIN. Scanning one type of
  // entity must not run on into the link keyspace.
  int entities = 0;
  for (auto it = store_->ScanEntities(kVessel); it->Valid(); it->Next()) ++entities;
  EXPECT_EQ(0, entities) << "an empty type scan must return nothing at all";

  int ports = 0;
  for (auto it = store_->ScanEntities(kPort); it->Valid(); it->Next()) ++ports;
  EXPECT_EQ(1, ports);
}
