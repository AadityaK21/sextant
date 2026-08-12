// The Postgres connector.
//
// OPTIONAL AT BUILD TIME, ON PURPOSE. libpq is a system package, not something
// FetchContent can pin, and making it mandatory would mean the project fails to
// build on a clean Windows machine over a source that is a demonstration of
// connector variety rather than the point of the project. When Postgres is
// absent, Open() returns NotSupported with a message that says so.
//
// The part that would otherwise be untestable - turning a tuple into a Row -
// is not conditional. `MemoryTableSource` implements exactly the same shape
// over an in-memory table and is what the tests exercise, so the only code path
// that depends on a database being present is the libpq calls themselves.
//
// SINGLE-ROW MODE RATHER THAN A CURSOR. libpq normally buffers the entire
// result set in the client before returning, which for an AIS table means
// loading millions of rows into RAM to write them out one at a time.
// PQsetSingleRowMode makes libpq hand back one row at a time as they arrive off
// the socket, which is genuine streaming with no server-side cursor to declare,
// hold open, or clean up on failure.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "row.h"
#include "sextant/lsm/status.h"
#include "source.h"

namespace sextant::connectors {

// True when the build found libpq. The CLI uses this to print something
// helpful rather than a bare NotSupported.
bool PostgresAvailable();

class PostgresSource : public RowSource {
 public:
  static Status Open(const std::string& dsn, const std::string& query,
                     const std::vector<std::string>& params,
                     std::unique_ptr<PostgresSource>* out);

  ~PostgresSource() override;

  bool Next() override;
  const Row& current() const override;
  uint64_t row_seq() const override { return row_seq_; }
  Status status() const override { return status_; }

 private:
  PostgresSource();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint64_t row_seq_ = 0;
  Status status_;
};

// A table held in memory, exposed through the same interface. Used by the tests
// to exercise every path a Postgres ingest takes except the wire protocol, and
// usable as a fixture for any tabular source.
class MemoryTableSource : public RowSource {
 public:
  MemoryTableSource(std::vector<std::string> columns,
                    std::vector<std::vector<std::string>> rows);
  ~MemoryTableSource() override;

  bool Next() override;
  const Row& current() const override;
  uint64_t row_seq() const override { return row_seq_; }
  Status status() const override { return Status::OK(); }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint64_t row_seq_ = 0;
};

}  // namespace sextant::connectors
