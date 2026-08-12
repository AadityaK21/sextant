// The mapping engine: a source row becomes a SourceRecord.
//
// One mapping file per source, declaring which columns feed which ontology
// properties and which transform chain shapes each one. This is the layer that
// makes the ontology declarative in practice rather than in principle - the
// four sources in this project disagree about column names, casing, coordinate
// format, code coverage and vessel-type taxonomy, and every one of those
// disagreements is settled in YAML instead of in C++.
//
// The engine's real output is not the values. It is the values PLUS the
// evidence: for each property, which column it came from and which transforms
// ran, recorded at the moment the value is produced. Lineage reconstructed
// afterwards would be a guess.
//
// SOURCE IDS ARE NUMERIC AND WRITTEN DOWN, for the same reason type ids are.
// They appear in RAW, SRCREC and XREF keys, so `wpi` needs a number that never
// changes even if the file is renamed.
//
// FILTERS RUN BEFORE MAPPING. UN/LOCODE contains about 110,000 locations of
// which only a fraction are seaports, distinguished by a positional bitfield.
// Filtering at the connector boundary means the other 100,000 never become
// records, never get blocked, and never get compared - which is a much bigger
// win than any optimisation further down.

#pragma once

#include <cstdint>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "keyspace.h"
#include "record.h"
#include "row.h"
#include "schema.h"
#include "sextant/lsm/status.h"
#include "transform.h"

namespace sextant::ontology {

enum class ConnectorKind { kCsv, kHttp, kPostgres };

const char* ConnectorKindName(ConnectorKind kind);
bool ParseConnectorKind(const std::string& name, ConnectorKind* out);

struct PropertyMapping {
  PropId prop = 0;
  std::string prop_name;
  ValueType target_type = ValueType::kString;

  // Usually one column. Several means the cells are gathered into a string list
  // and handed to the chain, which is how UN/LOCODE builds a five-character
  // code out of its separate Country and Location columns.
  std::vector<std::string> from;

  std::vector<TransformId> chain;
  std::vector<std::string> chain_names;
  uint64_t chain_fingerprint = 0;
};

struct LinkMapping {
  LinkTypeId link_type = 0;
  std::string link_name;
  TypeId target_type = 0;
  PropId match_property = 0;
  std::string from;  // the column holding the value to match on
};

struct TypeMapping {
  TypeId target_type = 0;
  std::string target_type_name;
  // HTTP sources fan out over several endpoints; a mapping declares which one
  // it consumes. Empty for CSV and Postgres, which have a single stream.
  std::string from_endpoint;
  std::vector<std::string> natural_key;
  std::vector<PropertyMapping> properties;
  std::vector<LinkMapping> links;
};

// A row is kept only if every filter matches. `pattern` is an ECMAScript
// regular expression searched anywhere in the cell, so anchor it if you mean
// to.
struct FilterRule {
  std::string column;
  std::string pattern;
  std::shared_ptr<std::regex> compiled;
};

struct EndpointSpec {
  std::string id;
  std::string path;
  // Where the array of records lives in the response body. Empty means the
  // document is itself the array.
  std::string records_at;
};

struct SourceSpec {
  codec::SourceId id = 0;  // numeric, appears in every key
  std::string key;         // "wpi"
  std::string name;
  ConnectorKind connector = ConnectorKind::kCsv;

  std::string uri;          // csv: file path
  std::string base_url;     // http
  std::string snapshot_dir; // http: where responses are recorded and replayed
  std::vector<std::pair<std::string, std::string>> headers;
  std::string dsn;          // postgres
  std::string query;
  // Bound separately rather than interpolated into the SQL. The values here
  // come from a config file that a user edits, and a query built by string
  // concatenation from an editable file is an injection waiting to happen.
  std::vector<std::string> params;
  int cursor_size = 10000;

  // Source-level weight consumed by the most_trusted fusion rule. Encoding
  // "which source is right about what" as one number per source is a
  // simplification - UN/LOCODE is authoritative on codes and mediocre on
  // geography - and a production system would carry it per property.
  double trust = 0.5;

  std::vector<std::string> natural_key;
  std::vector<FilterRule> filters;
  std::vector<EndpointSpec> endpoints;
  std::vector<TypeMapping> mappings;

  const EndpointSpec* Endpoint(const std::string& id) const;

  // Loading resolves every name against the ontology and the transform
  // registry, so a typo in a property name or a transform is a startup error
  // rather than a property that quietly never appears.
  static Status LoadFromFile(const std::string& path, const Ontology& onto,
                             const TransformRegistry& transforms, SourceSpec* out);
  static Status LoadFromString(const std::string& yaml, const Ontology& onto,
                               const TransformRegistry& transforms, SourceSpec* out,
                               const std::string& origin = "<string>");
};

class Mapper {
 public:
  Mapper(const Ontology* ontology, const TransformRegistry* transforms,
         const SourceSpec* spec);

  // False if the row fails any filter, in which case it is not a record at all.
  bool Accepts(const Row& row) const;

  // Produces one SourceRecord per mapping that applies to this row. `endpoint`
  // selects among the mappings for HTTP sources and is ignored otherwise.
  //
  // A mapping yields nothing if the row has no value for any of its natural key
  // columns - that is how one endpoint's response is skipped by a mapping meant
  // for another shape, rather than producing a record with no identity.
  Status MapRow(const Row& row, codec::BatchId batch, codec::RowSeq row_seq,
                const std::string& endpoint,
                std::vector<SourceRecord>* out) const;

  const SourceSpec& spec() const { return *spec_; }

 private:
  const Ontology* ontology_;
  const TransformRegistry* transforms_;
  const SourceSpec* spec_;
};

}  // namespace sextant::ontology
