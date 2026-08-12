// RowSource - a stream of rows from somewhere.
//
// The ingestor is written once against this interface and works for a 15 MB
// CSV, a paginated JSON API and a Postgres cursor without knowing which it has.
//
// PULL, NOT PUSH, AND ONE ROW AT A TIME. The iterator shape is what makes
// streaming the default rather than an optimisation. UN/LOCODE has about
// 110,000 rows and an AIS extract can have millions; a `std::vector<Row>`
// return type would make holding the whole source in memory the path of least
// resistance, and the first thing anyone would hit is a source that does not
// fit.
//
// FAILURE IS SEPARATE FROM EXHAUSTION. Next() returning false means "no more
// rows", which happens both at a clean end of file and after an I/O error.
// status() is what distinguishes them, and the ingestor checks it before
// committing a batch - otherwise a truncated download would be indistinguishable
// from a complete one and would quietly commit a partial batch as though it
// were the whole source.

#pragma once

#include <cstdint>
#include <string>

#include "row.h"
#include "sextant/lsm/status.h"

namespace sextant::connectors {

using lsm::Status;
using ontology::Row;

class RowSource {
 public:
  virtual ~RowSource() = default;

  // Advance to the next row. False means the stream ended - check status() to
  // find out whether it ended well.
  virtual bool Next() = 0;

  // Valid until the next call to Next().
  virtual const Row& current() const = 0;

  // 1-based position in this stream. Written into RAW keys, so it must be
  // stable for a given input: this is what a lineage answer uses to find the
  // row again.
  virtual uint64_t row_seq() const = 0;

  // Which declared endpoint these rows came from. Empty for single-stream
  // sources; the HTTP connector uses it to select among mappings.
  virtual std::string endpoint() const { return {}; }

  virtual Status status() const = 0;
};

}  // namespace sextant::connectors
