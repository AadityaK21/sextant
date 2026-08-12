// A streaming RFC 4180 CSV reader.
//
// WHY NOT getline AND split ON COMMAS
//
// Because both of the CSV sources in this project break that immediately. Port
// names contain commas inside quotes; the World Port Index has fields with
// embedded newlines; UN/LOCODE writes names with doubled quotes inside quoted
// fields. A splitter that handles none of those does not fail loudly - it
// silently shifts every column after the offending one, and the result is a
// port whose country code is half its name. That class of bug survives all the
// way to entity resolution, where it looks like a data quality problem rather
// than a parsing one.
//
// So the parser is a proper state machine over the four cases RFC 4180
// actually specifies, and there is a test for each.
//
// STREAMING, WITH ONE DELIBERATE ALLOCATION. Bytes are pulled in 64 KB chunks
// and a record is assembled into a reusable buffer, so memory is bounded by the
// widest record rather than by the file. The `fields()` vector is reused across
// rows for the same reason.
//
// THE RAW BYTES ARE KEPT. Raw() returns the exact bytes of the record as they
// appeared in the file, quotes and all, because that is what gets written to
// the RAW keyspace and shown in the lineage panel. Re-serialising the parsed
// fields would produce something that looks right and is not what the file
// says.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "env.h"
#include "row.h"
#include "sextant/lsm/status.h"
#include "source.h"

namespace sextant::connectors {

class CsvReader;

// A view onto the reader's current record. Cheap to construct, invalidated by
// the next Next().
class CsvRow : public ontology::Row {
 public:
  explicit CsvRow(const CsvReader* reader) : reader_(reader) {}

  bool Get(const std::string& path, std::string* out) const override;
  std::string Raw() const override;

 private:
  const CsvReader* reader_;
};

class CsvReader : public RowSource {
 public:
  static Status Open(const std::string& path, std::unique_ptr<CsvReader>* out);

  // Parse from memory. Used by the tests and by the HTTP connector when a
  // response happens to be CSV rather than JSON.
  static Status OpenFromString(std::string contents,
                               std::unique_ptr<CsvReader>* out);

  bool Next() override;
  const Row& current() const override { return row_; }
  uint64_t row_seq() const override { return row_seq_; }
  Status status() const override { return status_; }

  const std::vector<std::string>& header() const { return header_; }
  const std::vector<std::string>& fields() const { return fields_; }
  const std::string& raw_record() const { return raw_record_; }

  // Index of a column by name, or -1. Lookups go through a map because a wide
  // file such as the World Port Index has around a hundred columns and a linear
  // scan per property per row is the difference between seconds and minutes.
  int ColumnIndex(const std::string& name) const;

 private:
  CsvReader() : row_(this) {}

  // Reads one record into fields_ and raw_record_. False at end of input.
  bool ReadRecord();
  bool Fill();  // pull the next chunk from the file

  std::unique_ptr<lsm::SequentialFile> file_;
  std::string memory_;      // whole input, when opened from a string
  bool from_memory_ = false;

  std::vector<char> buffer_;
  size_t pos_ = 0;
  size_t end_ = 0;
  bool eof_ = false;

  std::vector<std::string> header_;
  std::unordered_map<std::string, int> column_index_;
  std::vector<std::string> fields_;
  std::string raw_record_;
  uint64_t row_seq_ = 0;

  CsvRow row_;
  Status status_;
};

}  // namespace sextant::connectors
