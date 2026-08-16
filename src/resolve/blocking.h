// Blocking: turning O(n^2) comparisons into something that finishes.
//
// THE ARITHMETIC THAT MAKES THIS NECESSARY
//
// Comparing every pair of records is quadratic. At the 376 port records in the
// committed corpus that is 70,500 pairs, which is nothing. At the 110,000
// locations in the full UN/LOCODE download it is six billion, and at any real
// scale it is simply not a thing you do. Blocking replaces "compare everything"
// with "compare records that share a cheap key", and the whole art is choosing
// keys that are cheap to compute and rarely wrong to skip.
//
// TWO NUMBERS DESCRIBE A BLOCKING SCHEME, AND YOU MUST REPORT BOTH
//
//   Reduction Ratio    1 - (candidate pairs / all possible pairs)
//                      How much work you avoided. Trivially maximised by
//                      blocking on nothing, so it is meaningless alone.
//
//   Pair Completeness  (true pairs in the candidate set) / (true pairs)
//                      The recall ceiling for the entire resolver. A pair that
//                      blocking misses is never scored, never clustered and
//                      never merged, no matter how good the scorer is.
//
// They trade against each other and either one alone can be gamed. Quoting RR
// without PC is the classic way to make a blocking scheme sound good.
//
// PC IS A CEILING, NOT A SCORE. Everything downstream can only lose recall
// from here, never regain it. If PC is
// 0.94, then 6% of true duplicates are already gone before the scorer has seen
// a single feature, and no amount of weight tuning will get them back.
//
// FIVE KEYS, NOT ONE
//
// Each key catches a different failure. An exact UN/LOCODE match is nearly
// free and nearly perfect - when the code is present, which for the World Port
// Index is only about three quarters of the time. The geographic cell catches
// records whose names disagree. The phonetic and prefix keys catch records with
// no code and imprecise coordinates. Recall is the union; precision is the
// intersection's problem, and the scorer's.
//
// BLOCK PURGING. A key that puts thousands of records in one block does not
// reduce anything - it reintroduces the quadratic blowup inside a single block.
// Blocks above the size limit are dropped whole, which costs recall in a
// visible, measurable way rather than silently melting the machine.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bundle.h"
#include "golden.h"
#include "record.h"
#include "record_ref.h"
#include "sextant/lsm/status.h"
#include "store.h"

namespace sextant::resolve {

using lsm::Status;
using ontology::SchemaBundle;
using ontology::SourceRecord;

// One blocking key: which generator produced it, and the value records must
// share to be blocked together.
struct BlockingKey {
  std::string generator;
  std::string value;
};

// The property names entity resolution needs to find in the ontology.
//
// This is where the declarative layer stops. Storage, indexing and traversal
// are fully schema-driven; resolution is not, because knowing that an IMO is a
// permanent hull identifier while an MMSI is a reassignable radio licence is
// domain knowledge, not schema. Naming those properties in one struct rather
// than scattering string literals through the resolver at least keeps the
// coupling in a single place, and makes loading it from a config file on day 9
// a small change instead of a rewrite.
struct ResolverProperties {
  codec::TypeId port_type = 0;
  codec::PropId port_name = 0;
  codec::PropId port_locode = 0;
  codec::PropId port_country = 0;
  codec::PropId port_lat = 0;
  codec::PropId port_lon = 0;
  codec::PropId port_alt_names = 0;

  codec::TypeId vessel_type = 0;
  codec::PropId vessel_name = 0;
  codec::PropId vessel_imo = 0;
  codec::PropId vessel_mmsi = 0;
  codec::PropId vessel_call_sign = 0;
  codec::PropId vessel_flag = 0;

  // Resolves every name against the ontology, failing loudly if one is missing
  // rather than silently disabling the feature that needed it.
  static Status Resolve(const ontology::Ontology& ontology,
                        ResolverProperties* out);
};

// Produces the blocking keys for one normalised record.
std::vector<BlockingKey> PortBlockingKeys(const SourceRecord& record,
                                          const ResolverProperties& props);
std::vector<BlockingKey> VesselBlockingKeys(const SourceRecord& record,
                                            const ResolverProperties& props);
std::vector<BlockingKey> BlockingKeysFor(const SourceRecord& record,
                                         const ResolverProperties& props);

struct BlockingReport {
  uint64_t records_indexed = 0;
  uint64_t keys_written = 0;
  uint64_t blocks = 0;
  uint64_t purged_blocks = 0;
  uint64_t largest_block = 0;

  uint64_t candidate_pairs = 0;
  uint64_t possible_pairs = 0;
  double reduction_ratio = 0.0;

  // Against the golden set, when one is supplied.
  uint64_t golden_matches = 0;
  uint64_t golden_matches_covered = 0;
  double pair_completeness = 0.0;

  // Which generator produced how many candidates, and how many true pairs it
  // was alone in catching. The second column is what tells you whether a key is
  // earning its cost - a key that contributes no unique recall can be dropped.
  std::map<std::string, uint64_t> pairs_by_key;
  std::map<std::string, uint64_t> matches_by_key;
  std::map<std::string, uint64_t> unique_matches_by_key;

  // Golden matches that no key put together, as readable references. These are
  // the recall the resolver can never recover, so they are worth looking at
  // one by one rather than only counting.
  std::vector<std::pair<std::string, std::string>> missed;
};

// A candidate pair plus which keys produced it. The attribution is not
// decoration: it is what lets the report say which of the five keys is worth
// its cost.
struct CandidatePairRef {
  PairRef pair;
  std::vector<std::string> via;
};

class Blocker {
 public:
  struct Options {
    // A block larger than this is dropped whole. 200 members is 19,900 pairs
    // from a single key - past that a "block" is not narrowing anything.
    size_t max_block_size = 200;
    // Only pairs from different sources, or within a source too? Duplicates do
    // occur inside one source - the World Port Index lists Rotterdam and
    // Rotterdam Botlek separately - so this defaults off.
    bool cross_source_only = false;
    size_t max_missed_reported = 25;
  };

  Blocker(codec::Store* store, const SchemaBundle* bundle,
          const ResolverProperties* props);

  // Walk every source's SRCREC range and write BLOCK entries.
  Status IndexAll(BlockingReport* report);

  // Read the BLOCK keyspace back and emit candidate pairs.
  Status GenerateCandidates(const Options& options,
                            std::vector<CandidatePairRef>* pairs,
                            BlockingReport* report);

 private:
  codec::Store* store_;
  const SchemaBundle* bundle_;
  const ResolverProperties* props_;
};

// Fill in the golden-set half of a report: pair completeness, per-key recall,
// and the list of true pairs no key put together.
//
// Kept separate from candidate generation on purpose. Blocking has to work
// without a golden set - it is what production would run - and measuring it is
// a thing you do afterwards with labels you happen to have.
void MeasureAgainstGolden(const std::vector<CandidatePairRef>& pairs,
                          const GoldenSet& golden, size_t max_missed_reported,
                          BlockingReport* report);

}  // namespace sextant::resolve
