#include "postgres.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(SEXTANT_WITH_POSTGRES)
#include <libpq-fe.h>
#endif

namespace sextant::connectors {
namespace {

// One tuple, addressed by column name. Shared by the live connector and the
// in-memory fixture, which is what makes the fixture a real test of the
// mapping path rather than a stand-in for it.
class TupleRow : public ontology::Row {
 public:
  void Reset(const std::unordered_map<std::string, int>* index,
             const std::vector<std::string>* values) {
    index_ = index;
    values_ = values;
  }

  bool Get(const std::string& path, std::string* out) const override {
    if (index_ == nullptr || values_ == nullptr) return false;
    const auto it = index_->find(path);
    if (it == index_->end()) return false;
    const auto i = static_cast<size_t>(it->second);
    *out = i < values_->size() ? (*values_)[i] : std::string();
    return true;
  }

  // Tab-separated, in column order. A SQL row has no original text form, so
  // "verbatim" has to be defined rather than preserved; this is the closest
  // honest answer and it round-trips through the column list.
  std::string Raw() const override {
    if (values_ == nullptr) return {};
    std::string out;
    for (size_t i = 0; i < values_->size(); ++i) {
      if (i != 0) out.push_back('\t');
      out += (*values_)[i];
    }
    return out;
  }

 private:
  const std::unordered_map<std::string, int>* index_ = nullptr;
  const std::vector<std::string>* values_ = nullptr;
};

}  // namespace

bool PostgresAvailable() {
#if defined(SEXTANT_WITH_POSTGRES)
  return true;
#else
  return false;
#endif
}

// --- MemoryTableSource ------------------------------------------------------

struct MemoryTableSource::Impl {
  std::vector<std::string> columns;
  std::unordered_map<std::string, int> index;
  std::vector<std::vector<std::string>> rows;
  size_t next = 0;
  TupleRow row;
};

MemoryTableSource::MemoryTableSource(std::vector<std::string> columns,
                                     std::vector<std::vector<std::string>> rows)
    : impl_(std::make_unique<Impl>()) {
  impl_->columns = std::move(columns);
  impl_->rows = std::move(rows);
  for (size_t i = 0; i < impl_->columns.size(); ++i) {
    impl_->index.emplace(impl_->columns[i], static_cast<int>(i));
  }
}

MemoryTableSource::~MemoryTableSource() = default;

bool MemoryTableSource::Next() {
  if (impl_->next >= impl_->rows.size()) return false;
  impl_->row.Reset(&impl_->index, &impl_->rows[impl_->next]);
  ++impl_->next;
  ++row_seq_;
  return true;
}

const Row& MemoryTableSource::current() const { return impl_->row; }

// --- PostgresSource ---------------------------------------------------------

#if defined(SEXTANT_WITH_POSTGRES)

struct PostgresSource::Impl {
  PGconn* conn = nullptr;
  PGresult* result = nullptr;

  std::unordered_map<std::string, int> index;
  std::vector<std::string> values;
  TupleRow row;
  bool streaming = false;

  ~Impl() {
    if (result != nullptr) PQclear(result);
    if (conn != nullptr) {
      // Drain anything still in flight. Closing a connection mid-result leaves
      // the backend running the query until it notices, which on a large AIS
      // table can be a long time.
      while (PGresult* r = PQgetResult(conn)) PQclear(r);
      PQfinish(conn);
    }
  }
};

PostgresSource::PostgresSource() : impl_(std::make_unique<Impl>()) {}
PostgresSource::~PostgresSource() = default;

Status PostgresSource::Open(const std::string& dsn, const std::string& query,
                            const std::vector<std::string>& params,
                            std::unique_ptr<PostgresSource>* out) {
  std::unique_ptr<PostgresSource> src(new PostgresSource());
  src->impl_->conn = PQconnectdb(dsn.c_str());
  if (PQstatus(src->impl_->conn) != CONNECTION_OK) {
    return Status::IOError(std::string("postgres connect failed: ") +
                           PQerrorMessage(src->impl_->conn));
  }

  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& p : params) values.push_back(p.c_str());

  const int sent = PQsendQueryParams(
      src->impl_->conn, query.c_str(), static_cast<int>(params.size()),
      /*paramTypes=*/nullptr, values.empty() ? nullptr : values.data(),
      /*paramLengths=*/nullptr, /*paramFormats=*/nullptr, /*resultFormat=*/0);
  if (sent == 0) {
    return Status::IOError(std::string("postgres query failed: ") +
                           PQerrorMessage(src->impl_->conn));
  }
  // Without this libpq buffers the entire result set client-side before
  // returning a single row, which defeats the point of streaming.
  if (PQsetSingleRowMode(src->impl_->conn) == 0) {
    return Status::IOError("postgres refused single-row mode");
  }
  src->impl_->streaming = true;
  *out = std::move(src);
  return Status::OK();
}

bool PostgresSource::Next() {
  if (!impl_->streaming) return false;

  if (impl_->result != nullptr) {
    PQclear(impl_->result);
    impl_->result = nullptr;
  }
  impl_->result = PQgetResult(impl_->conn);
  if (impl_->result == nullptr) {
    impl_->streaming = false;
    return false;
  }

  const ExecStatusType st = PQresultStatus(impl_->result);
  if (st == PGRES_TUPLES_OK) {
    // The terminating empty result. Drain to the null that ends the stream so
    // the connection is left reusable.
    while (PGresult* r = PQgetResult(impl_->conn)) PQclear(r);
    impl_->streaming = false;
    return false;
  }
  if (st != PGRES_SINGLE_TUPLE) {
    status_ = Status::IOError(std::string("postgres stream failed: ") +
                              PQresultErrorMessage(impl_->result));
    impl_->streaming = false;
    return false;
  }

  if (impl_->index.empty()) {
    const int ncols = PQnfields(impl_->result);
    for (int i = 0; i < ncols; ++i) {
      impl_->index.emplace(PQfname(impl_->result, i), i);
    }
    impl_->values.resize(static_cast<size_t>(ncols));
  }

  const int ncols = PQnfields(impl_->result);
  for (int i = 0; i < ncols; ++i) {
    // A SQL NULL becomes an empty string. The distinction is preserved further
    // up by null_if_blank, and carrying a third state through the Row interface
    // for one connector would complicate all three.
    impl_->values[static_cast<size_t>(i)] =
        PQgetisnull(impl_->result, 0, i) ? std::string()
                                         : PQgetvalue(impl_->result, 0, i);
  }
  impl_->row.Reset(&impl_->index, &impl_->values);
  ++row_seq_;
  return true;
}

const Row& PostgresSource::current() const { return impl_->row; }

#else  // no libpq

struct PostgresSource::Impl {
  TupleRow row;
};

PostgresSource::PostgresSource() : impl_(std::make_unique<Impl>()) {}
PostgresSource::~PostgresSource() = default;

Status PostgresSource::Open(const std::string& dsn, const std::string& query,
                            const std::vector<std::string>& params,
                            std::unique_ptr<PostgresSource>* out) {
  (void)dsn;
  (void)query;
  (void)params;
  (void)out;
  return Status::NotSupported(
      "this build has no Postgres support - install libpq development headers"
      " and re-run cmake, or use `docker compose up` and build in the container");
}

bool PostgresSource::Next() { return false; }

const Row& PostgresSource::current() const { return impl_->row; }

#endif

}  // namespace sextant::connectors
