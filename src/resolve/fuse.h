// Fusion: one cluster of source records becomes one entity.
//
// The cluster says these five records describe Rotterdam. They disagree about
// its name, its coordinates and its harbour size. Fusion picks a winner per
// property and - the part that matters - records why, along with what lost.
//
// WHY THE RULE IS DECLARED PER PROPERTY IN THE ONTOLOGY
//
// There is no single right answer. The correct rule depends on what the
// property means:
//
//   locode        most_trusted    UN/LOCODE is the code authority; when it
//                                 disagrees with the World Port Index, it wins
//   name          most_trusted    same argument, different authority
//   lat, lon      numeric_median  resists one bad coordinate entirely, where a
//                                 mean would let it drag the answer
//   flag          most_recent     a reflagged vessel genuinely changed flag,
//                                 so the newest observation is the true one
//   alt_names     union           a list property loses information under any
//                                 rule that picks one value
//
// Hardcoding one rule for everything would be wrong four times out of five,
// and the schema is where a domain expert can change it without a code review.
//
// WHAT GETS WRITTEN, AND WHY IT IS ONE BATCH
//
// Per entity: the ENTITY record, an XREF for every source row that fed it, an
// IDX entry per indexed property, and a PROV record per property carrying the
// winner, its source row, its transform chain, the rule that chose it, and
// every rejected alternative with the reason it lost.
//
// All of it in a single lsm::WriteBatch. A crash landing in the middle would
// leave an entity whose provenance is missing, or an index pointing at nothing
// - which is the one state that makes the lineage question unanswerable.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blocking.h"
#include "bundle.h"
#include "cluster.h"
#include "record.h"
#include "sextant/lsm/status.h"
#include "store.h"
#include "ulid.h"

namespace sextant::resolve {

using lsm::Slice;
using ontology::FusionRule;
using ontology::SchemaBundle;
using ontology::SourceRecord;
using ontology::TValue;

// A value that did not win, kept so the lineage panel can show what lost.
//
// Recording the losers is what turns "the name is Rotterdam" into "the name is
// Rotterdam, because UN/LOCODE says so and it is more trusted than the World
// Port Index, which said ROTTERDAM". The second is an answer; the first is an
// assertion.
struct RejectedValue {
  codec::SourceId source_id = 0;
  codec::BatchId batch_id = 0;
  codec::RowSeq row_seq = 0;
  std::string column;
  std::string value;
  std::string reason;
};

// The provenance of one property of one entity.
struct Provenance {
  codec::PropId prop = 0;

  // The winner and where it came from.
  ontology::SourceRef origin;
  std::vector<ontology::TransformId> chain;
  uint64_t chain_fingerprint = 0;
  std::string raw_value;
  std::string emitted_value;

  // The decision.
  FusionRule rule = FusionRule::kMostTrusted;
  double confidence = 0.0;
  std::vector<RejectedValue> rejected;

  // How the entity came to exist at all.
  uint64_t cluster_size = 0;
  std::vector<std::string> merge_evidence;

  void EncodeTo(std::string* dst) const;
  static bool DecodeFrom(Slice* input, Provenance* out);
};

struct ResolvedEntity {
  codec::Ulid id;
  codec::TypeId type = 0;
  std::vector<std::pair<codec::PropId, TValue>> properties;
  std::vector<Provenance> provenance;
  std::vector<RecordRef> members;

  const TValue* Property(codec::PropId prop) const;
  void EncodeTo(std::string* dst) const;
  static bool DecodeFrom(Slice* input, ResolvedEntity* out);
};

// Applies the fusion rule declared in the ontology for each property.
class Fuser {
 public:
  Fuser(const SchemaBundle* bundle, const ResolverProperties* props);

  // `records` are the source records of one cluster, in cluster order. The
  // merge evidence is carried in from the scorer so the provenance can say
  // which features justified the merge.
  ResolvedEntity Fuse(const std::vector<const SourceRecord*>& records,
                      const std::vector<RecordRef>& members,
                      const std::vector<std::string>& merge_evidence) const;

 private:
  const SchemaBundle* bundle_;
  const ResolverProperties* props_;
};

// Writes one entity, its cross-references, its indexes and its provenance as a
// single atomic batch.
Status WriteEntity(codec::Store* store, const SchemaBundle& bundle,
                   const ResolvedEntity& entity, int* operations = nullptr);

}  // namespace sextant::resolve
