// SourceRecord - one source row, normalised, with lineage already attached.
//
// This is the output of ingestion and the input to entity resolution, and it is
// where cell-level lineage is actually born. Every property carries the column
// it came from, the transform chain that shaped it, and the literal bytes
// before any of that happened. Nothing downstream has to reconstruct that
// history, because nothing downstream could.
//
// THE POINT OF STORING raw_value NEXT TO value
//
// It looks redundant - the raw bytes are already in the RAW keyspace, one
// point lookup away. It is there because the round-trip test on day 11 needs
// to distinguish two failures that otherwise look identical:
//
//   * the transform chain no longer produces the stored value
//   * the SourceRef points at the wrong row or the wrong column
//
// With the raw cell recorded here, the test can re-run the chain on the value
// it was actually given AND separately check that fetching the source row
// yields that same cell. One assertion catches a broken transform, the other
// catches broken lineage, and the error message says which.
//
// A REJECTED PROPERTY IS STILL A PROPERTY. When validate_imo throws out a bad
// check digit, the cell survives with a null value and the reason attached.
// Deleting it would lose the only evidence that the source said anything at
// all, and "why does this vessel have no IMO" is a question the lineage panel
// is supposed to answer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "keyspace.h"
#include "sextant/lsm/slice.h"
#include "transform.h"
#include "value.h"

namespace sextant::ontology {

using codec::BatchId;
using codec::LinkTypeId;
using codec::PropId;
using codec::RowSeq;
using codec::SourceId;
using codec::TypeId;

// Where a value came from, precisely enough to fetch it again.
struct SourceRef {
  SourceId source_id = 0;
  BatchId batch_id = 0;
  RowSeq row_seq = 0;
  std::string column;  // the exact source column, or a JSON path

  bool operator==(const SourceRef& o) const {
    return source_id == o.source_id && batch_id == o.batch_id &&
           row_seq == o.row_seq && column == o.column;
  }
};

struct PropertyCell {
  PropId prop = 0;
  TValue value;

  SourceRef origin;
  std::vector<TransformId> chain;
  uint64_t chain_fingerprint = 0;

  // The literal source cell, before any transform ran.
  std::string raw_value;
  // Empty unless a transform rejected the value, in which case `value` is null
  // and this says which rule rejected it and why.
  std::string error;

  bool rejected() const { return !error.empty(); }
};

// An edge the source asserts but cannot resolve on its own. Digitraffic says a
// port call arrived at "NLRTM"; which Port entity that is depends on entity
// resolution having run, so the reference is carried by value until day 10 can
// turn it into a real link.
struct LinkRef {
  LinkTypeId link_type = 0;
  TypeId target_type = 0;
  PropId match_property = 0;
  std::string match_value;
  SourceRef origin;
};

struct SourceRecord {
  SourceId source_id = 0;
  BatchId batch_id = 0;
  RowSeq row_seq = 0;
  TypeId type = 0;

  // The source's own identifier for this row, joined from its natural key
  // columns, plus the hash that forms the SRCREC key.
  std::string natural_key;
  uint64_t natural_key_hash = 0;

  std::vector<PropertyCell> properties;
  std::vector<LinkRef> links;

  const PropertyCell* Property(PropId id) const;

  void EncodeTo(std::string* dst) const;
  static bool DecodeFrom(Slice* input, SourceRecord* out);
};

// --- INGEST manifest --------------------------------------------------------
//
// One per ingest run. `content_fingerprint` is what makes re-ingesting an
// unchanged file a no-op: RAW is append-only by design, so without a record of
// what was already loaded, running the same command twice would double every
// count and quietly corrupt any statistic computed over the source.
struct BatchManifest {
  SourceId source_id = 0;
  BatchId batch_id = 0;
  std::string source_key;   // "wpi"
  std::string uri;          // file path, URL or DSN
  uint64_t content_fingerprint = 0;  // 0 when the input cannot be pre-hashed
  int64_t started_ms = 0;
  int64_t finished_ms = 0;
  uint64_t rows_read = 0;
  uint64_t rows_filtered = 0;
  uint64_t records_written = 0;
  uint64_t properties_written = 0;
  uint64_t properties_rejected = 0;

  void EncodeTo(std::string* dst) const;
  static bool DecodeFrom(Slice* input, BatchManifest* out);
};

}  // namespace sextant::ontology
