// Lineage: answering "why does this say Rotterdam?"
//
// THE CLAIM THIS FILE MAKES GOOD ON
//
// Every value in the ontology can be traced to the exact source row and
// transform chain that produced it, and the trace can be REPLAYED to check that
// the value really is what that row and that chain produce.
//
// Everything before this was setup for that sentence. Transforms have stable
// numeric ids because a chain has to survive being written to disk. Batches are
// append-only because a reference has to stay valid. RAW keeps verbatim bytes
// because a re-serialised row would drift from the source in exactly the cases
// somebody is looking at it.
//
// TWO POINT LOOKUPS, BOTH O(log n)
//
//     PROV | entity | prop            -> the provenance record
//     RAW  | source | batch | row     -> the verbatim original row
//
// No scan, no join, no separate lineage database. That is the payoff of the
// keyspace layout: lineage is not a feature bolted on beside the data, it is
// the same ordered keyspace read with a different prefix.
//
// WHAT MAKES THIS DIFFERENT FROM DECORATIVE LINEAGE
//
// Most systems that claim lineage store a string: "derived from wpi.csv". That
// is a comment. It cannot be checked, it goes stale silently, and the first
// time someone relies on it they discover it was wrong six months ago.
//
// Here the chain is a list of transform ids and every transform is a pure
// function, so the claim is executable:
//
//     replay(chain, raw_cell) == stored_value
//
// `RoundTrip` runs that over every property of every entity. If it passes, the
// lineage is not a description of what the pipeline does - it is a proof.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bundle.h"
#include "fuse.h"
#include "record.h"
#include "sextant/lsm/status.h"
#include "store.h"
#include "ulid.h"

namespace sextant::lineage {

using lsm::Slice;
using lsm::Status;
using ontology::SchemaBundle;
using ontology::TValue;
using resolve::Provenance;
using resolve::ResolvedEntity;

// The full answer to "why does this property have this value".
struct Explanation {
  codec::Ulid entity_id;
  codec::TypeId entity_type = 0;
  codec::PropId prop = 0;
  std::string property_name;

  // Which of the two lookups succeeded. Both fail with NotFound, so the status
  // code alone cannot say whether the property has no provenance at all or has
  // provenance pointing at a row that is not there - and those are completely
  // different bugs.
  bool provenance_found = false;
  bool raw_row_found = false;

  std::string stored_value;

  // Where it came from.
  std::string source_key;   // "unlocode"
  codec::BatchId batch_id = 0;
  codec::RowSeq row_seq = 0;
  std::string column;

  // The verbatim row, and the one cell of it that this value came from.
  std::string raw_row;
  std::string raw_cell;

  // The chain, by name, in order.
  std::vector<std::string> transform_names;
  std::vector<ontology::TransformId> chain;
  uint64_t chain_fingerprint = 0;
  bool chain_changed = false;  // the stored fingerprint no longer matches

  // The decision.
  std::string rule;
  double confidence = 0.0;
  std::vector<resolve::RejectedValue> rejected;
  uint64_t cluster_size = 0;
  std::vector<std::string> merge_evidence;

  // The replay.
  std::string replayed_value;
  bool replay_matches = false;
  std::string replay_error;

  // True when `replay_matches` was decided by containment rather than equality,
  // because the property is fused by union and no single source row can
  // reproduce the merged list. Exposed so the UI can say which check it passed
  // instead of implying every green tick means the same thing.
  bool replay_is_union = false;

  std::string Render() const;
};

// Why a round-trip failed, which is a more useful thing than a count.
enum class RoundTripFailure {
  kNone,
  kNoProvenance,     // a property with no account of where it came from
  kRawRowMissing,    // the SourceRef points at a row that is not there
  kColumnMissing,    // the row exists but has no such column
  kValueMismatch,    // the chain no longer produces the stored value
  kTransformChanged, // a transform's version was bumped since this was written
};

const char* RoundTripFailureName(RoundTripFailure failure);

struct RoundTripResult {
  codec::Ulid entity_id;
  codec::TypeId entity_type = 0;
  codec::PropId prop = 0;
  RoundTripFailure failure = RoundTripFailure::kNone;
  std::string stored_value;
  std::string replayed_value;
  std::string detail;
};

struct RoundTripReport {
  uint64_t entities = 0;
  uint64_t properties = 0;
  uint64_t verified = 0;
  uint64_t failed = 0;
  // List-valued properties fused by `union` are checked differently: the
  // replayed value must be CONTAINED in the stored list rather than equal to
  // it, because the stored value is the union of several sources and no single
  // raw cell can reproduce it.
  uint64_t union_properties = 0;

  std::vector<RoundTripResult> failures;

  double rate() const {
    return properties == 0 ? 0.0
                           : static_cast<double>(verified) /
                                 static_cast<double>(properties);
  }
};

class LineageReader {
 public:
  // `data_root` is what a source's relative `uri` is resolved against.
  //
  // Reading a CSV cell by NAME needs the source's header, and the header lives
  // in the source file - which the mapping names relatively, because a schema
  // that hardcodes absolute paths is not portable. So the reader has to be told
  // where the data lives, rather than assuming the process happens to have been
  // started from the repository root. Getting this wrong is silent: every CSV
  // property fails with "cannot read the header" and the round trip drops to
  // half without anything being actually wrong with the lineage.
  LineageReader(codec::Store* store, const SchemaBundle* bundle,
                std::string data_root = ".");

  // The two point lookups plus the replay.
  Status Explain(codec::TypeId type, const codec::Ulid& entity,
                 codec::PropId prop, Explanation* out) const;

  // Every property of one entity.
  Status ExplainAll(codec::TypeId type, const codec::Ulid& entity,
                    std::vector<Explanation>* out) const;

  // THE TEST. Every property of every entity, replayed.
  Status RoundTrip(RoundTripReport* report,
                   size_t max_failures_reported = 25) const;

  // Extract one named cell from a verbatim source row. CSV needs the source's
  // header to turn a column name into a position; JSON can be addressed
  // directly. Public because the round-trip test is the main user and it is
  // worth being able to test the extractor on its own.
  Status RawCell(const ontology::SourceSpec& spec, const std::string& raw_row,
                 const std::string& column, std::string* out) const;

 private:
  Status LoadProvenance(const codec::Ulid& entity, codec::PropId prop,
                        Provenance* out) const;

  std::string SourcePath(const ontology::SourceSpec& spec) const;

  codec::Store* store_;
  const SchemaBundle* bundle_;
  std::string data_root_;

  // CSV headers, read once per source. Cached because the round-trip test asks
  // for a cell from every property of every entity, and reopening the file each
  // time would make an O(properties) test into an O(properties) file open.
  mutable std::vector<std::pair<codec::SourceId, std::vector<std::string>>>
      headers_;
  const std::vector<std::string>* HeaderFor(
      const ontology::SourceSpec& spec) const;
};

}  // namespace sextant::lineage
