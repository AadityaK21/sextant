// The REST/JSON connector.
//
// THE SPLIT THAT MATTERS: FETCHING IS NOT PARSING
//
// A connector that calls an HTTP client directly is a connector that cannot be
// tested, cannot be replayed, and produces lineage pointing at a response
// nobody can produce again. So this file knows nothing about the network. It
// consumes a `Fetcher`, and there are two:
//
//   SnapshotFetcher - reads a recorded response from data/snapshots/
//   HttpFetcher     - performs the request AND records it as a snapshot
//
// The default is the snapshot. Fetching is an explicit, separate step
// (`sextant fetch`), which buys three things at once:
//
//   * the test suite and the demo run with no network at all
//   * lineage stays honest - the bytes a provenance record points at are on
//     disk, so "show me the source row" works a year later
//   * Digitraffic's rate limits are hit once rather than on every run
//
// That is not a workaround for lacking an HTTP client. Reproducibility is the
// reason, and it is the same reason RAW is append-only.
//
// PATHS INTO NESTED JSON. An API response is not a table. The mapping files
// address cells with a dotted path that may contain indices:
//
//     portAreaDetails[0].ata
//
// Which is a deliberate simplification: taking the first port area of a call
// covers the overwhelming majority and ignores the rest. That is a real
// modelling decision and it is worth saying out loud rather than pretending
// the shape is flat.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "row.h"
#include "sextant/lsm/status.h"
#include "source.h"

namespace sextant::connectors {

// Where a JSON document comes from. Implementations return the raw response
// body; nothing here parses it.
class Fetcher {
 public:
  virtual ~Fetcher() = default;

  // `endpoint_id` names the endpoint in the mapping file; `path` is its URL
  // path. Implementations that replay from disk use the id; implementations
  // that hit the network use the path.
  virtual Status Fetch(const std::string& endpoint_id, const std::string& path,
                       std::string* body) = 0;

  virtual std::string Describe() const = 0;
};

// Replays a response recorded under <dir>/<endpoint_id>.json.
class SnapshotFetcher : public Fetcher {
 public:
  explicit SnapshotFetcher(std::string dir) : dir_(std::move(dir)) {}

  Status Fetch(const std::string& endpoint_id, const std::string& path,
               std::string* body) override;
  std::string Describe() const override { return "snapshot:" + dir_; }

  static std::string PathFor(const std::string& dir, const std::string& endpoint_id);

 private:
  std::string dir_;
};

// Streams the records out of one JSON document.
//
// `records_at` names the array inside the response. Digitraffic wraps port
// calls in {"portCalls": [...]} while other endpoints return a bare array, so
// an empty records_at means "the document is the array".
class JsonRowSource : public RowSource {
 public:
  // Parses `body`. A document that is not an array, or whose records_at path
  // does not lead to one, is a hard error rather than an empty stream: an API
  // that changed shape should stop the ingest, not silently load nothing.
  static Status Open(std::string body, const std::string& records_at,
                     const std::string& endpoint_id,
                     std::unique_ptr<JsonRowSource>* out);

  ~JsonRowSource() override;

  bool Next() override;
  const Row& current() const override;
  uint64_t row_seq() const override { return row_seq_; }
  std::string endpoint() const override { return endpoint_id_; }
  Status status() const override { return status_; }

  uint64_t size() const;

 private:
  JsonRowSource();

  // The nlohmann types are kept out of this header so that including it does
  // not pull a 900 KB header into every translation unit that ingests.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::string endpoint_id_;
  uint64_t row_seq_ = 0;
  Status status_;
};

// Exposed for testing: resolve a dotted path with optional indices against a
// JSON document. Returns false if any step is missing.
bool JsonPathLookup(const std::string& json, const std::string& path,
                    std::string* out);

}  // namespace sextant::connectors
