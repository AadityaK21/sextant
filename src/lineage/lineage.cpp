#include "lineage.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "csv.h"
#include "json_source.h"

namespace sextant::lineage {
namespace {

namespace onto = sextant::ontology;
namespace conn = sextant::connectors;

std::string Join(const std::vector<std::string>& parts, const char* separator) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += separator;
    out += parts[i];
  }
  return out;
}

}  // namespace

const char* RoundTripFailureName(RoundTripFailure failure) {
  switch (failure) {
    case RoundTripFailure::kNone: return "ok";
    case RoundTripFailure::kNoProvenance: return "no provenance";
    case RoundTripFailure::kRawRowMissing: return "raw row missing";
    case RoundTripFailure::kColumnMissing: return "column missing";
    case RoundTripFailure::kValueMismatch: return "value mismatch";
    case RoundTripFailure::kTransformChanged: return "transform changed";
  }
  return "unknown";
}

std::string Explanation::Render() const {
  std::string out;
  char buf[256];

  std::snprintf(buf, sizeof(buf), "%s = %s\n", property_name.c_str(),
                stored_value.c_str());
  out += buf;

  std::snprintf(buf, sizeof(buf), "  rule       %s (confidence %.2f)\n",
                rule.c_str(), confidence);
  out += buf;

  std::snprintf(buf, sizeof(buf), "  origin     %s batch %llu row %llu, column %s\n",
                source_key.c_str(), static_cast<unsigned long long>(batch_id),
                static_cast<unsigned long long>(row_seq), column.c_str());
  out += buf;

  out += "  raw cell   \"" + raw_cell + "\"\n";
  if (!transform_names.empty()) {
    out += "  chain      " + Join(transform_names, " -> ") + "\n";
  }
  out += "  replay     \"" + replayed_value + "\"  " +
         (replay_matches ? "MATCHES" : "DOES NOT MATCH") + "\n";
  if (!replay_error.empty()) out += "  error      " + replay_error + "\n";
  if (chain_changed) {
    out += "  warning    a transform in this chain has been revised since this"
           " value was written\n";
  }

  if (!rejected.empty()) {
    out += "  rejected\n";
    for (const auto& r : rejected) {
      std::snprintf(buf, sizeof(buf), "    \"%s\" from source %u row %llu (%s)\n",
                    r.value.c_str(), r.source_id,
                    static_cast<unsigned long long>(r.row_seq), r.column.c_str());
      out += buf;
      out += "      " + r.reason + "\n";
    }
  }
  if (cluster_size > 1) {
    std::snprintf(buf, sizeof(buf), "  cluster    %llu source records\n",
                  static_cast<unsigned long long>(cluster_size));
    out += buf;
    for (const auto& evidence : merge_evidence) {
      out += "    " + evidence + "\n";
    }
  }

  out += "  raw row    " + raw_row + "\n";
  return out;
}

LineageReader::LineageReader(codec::Store* store, const SchemaBundle* bundle,
                             std::string data_root)
    : store_(store), bundle_(bundle), data_root_(std::move(data_root)) {}

std::string LineageReader::SourcePath(const ontology::SourceSpec& spec) const {
  if (spec.uri.empty()) return spec.uri;
  // Already absolute, or no root to resolve against.
  if (spec.uri.front() == '/' || spec.uri.front() == '\\') return spec.uri;
  if (spec.uri.size() > 1 && spec.uri[1] == ':') return spec.uri;  // C:\...
  if (data_root_.empty() || data_root_ == ".") return spec.uri;
  return data_root_ + "/" + spec.uri;
}

const std::vector<std::string>* LineageReader::HeaderFor(
    const ontology::SourceSpec& spec) const {
  for (const auto& [id, header] : headers_) {
    if (id == spec.id) return &header;
  }
  std::unique_ptr<conn::CsvReader> reader;
  std::vector<std::string> header;
  if (conn::CsvReader::Open(SourcePath(spec), &reader).ok()) {
    header = reader->header();
  }
  headers_.emplace_back(spec.id, std::move(header));
  return &headers_.back().second;
}

Status LineageReader::RawCell(const ontology::SourceSpec& spec,
                              const std::string& raw_row,
                              const std::string& column, std::string* out) const {
  // A multi-column property records its columns joined with '+'. The raw value
  // it stored is the cells joined by the same unit separator the natural key
  // uses, so rebuild it the same way rather than trying to reverse the join.
  if (column.find('+') != std::string::npos) {
    std::vector<std::string> names;
    std::string current;
    for (const char c : column) {
      if (c == '+') {
        names.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    names.push_back(current);

    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
      std::string cell;
      const Status s = RawCell(spec, raw_row, names[i], &cell);
      if (!s.ok()) return s;
      if (i != 0) joined.push_back('\x1f');
      joined += cell;
    }
    *out = std::move(joined);
    return Status::OK();
  }

  switch (spec.connector) {
    case onto::ConnectorKind::kCsv: {
      // The verbatim row is one CSV record, so parse it with the same reader
      // the connector used - quoting and embedded separators included - and use
      // the source's header to turn the column name into a position.
      const std::vector<std::string>* header = HeaderFor(spec);
      if (header == nullptr || header->empty()) {
        return Status::NotFound(
            "cannot read the header of " + SourcePath(spec) +
            " - lineage needs it to turn a column name into a position."
            " Pass --data-root if the source files are not below the working"
            " directory");
      }
      int index = -1;
      for (size_t i = 0; i < header->size(); ++i) {
        if ((*header)[i] == column) {
          index = static_cast<int>(i);
          break;
        }
      }
      if (index < 0) {
        return Status::NotFound("source " + spec.key + " has no column \"" +
                                column + "\"");
      }

      std::unique_ptr<conn::CsvReader> reader;
      const std::string document = Join(*header, ",") + "\n" + raw_row + "\n";
      const Status s = conn::CsvReader::OpenFromString(document, &reader);
      if (!s.ok()) return s;
      if (!reader->Next()) {
        return Status::Corruption("the raw row did not parse as a CSV record");
      }
      const auto& fields = reader->fields();
      *out = static_cast<size_t>(index) < fields.size()
                 ? fields[static_cast<size_t>(index)]
                 : std::string();
      return Status::OK();
    }

    case onto::ConnectorKind::kHttp: {
      // The verbatim row is one JSON object, addressed by the same dotted path
      // the mapping used.
      if (!conn::JsonPathLookup(raw_row, column, out)) {
        return Status::NotFound("no value at \"" + column + "\" in the raw row");
      }
      return Status::OK();
    }

    case onto::ConnectorKind::kPostgres: {
      // A SQL row has no original text form, so the connector defined one:
      // tab-separated in column order. Without the query's column list there is
      // nothing to index into, which is a real gap and is stated rather than
      // faked.
      return Status::NotSupported(
          "lineage cannot name a column in a Postgres row - the connector"
          " stores tab-separated values with no header");
    }
  }
  return Status::NotSupported("unknown connector");
}

Status LineageReader::LoadProvenance(const codec::Ulid& entity,
                                     codec::PropId prop, Provenance* out) const {
  // PROV is keyed entity | prop | version, with version descending inside the
  // engine's ordering, so the first key in the prefix is the newest record.
  auto it = store_->ScanProvenance(entity, prop);
  if (!it->Valid()) {
    return Status::NotFound("no provenance for property " + std::to_string(prop));
  }
  Slice value = it->value();
  if (!Provenance::DecodeFrom(&value, out)) {
    return Status::Corruption("undecodable provenance record");
  }
  return it->status();
}

Status LineageReader::Explain(codec::TypeId type, const codec::Ulid& entity,
                              codec::PropId prop, Explanation* out) const {
  *out = Explanation{};
  out->entity_id = entity;
  out->entity_type = type;
  out->prop = prop;

  const onto::EntityTypeDef* type_def = bundle_->ontology().Type(type);
  const onto::PropertyDef* prop_def =
      type_def == nullptr ? nullptr : type_def->Property(prop);
  out->property_name = prop_def == nullptr ? std::to_string(prop) : prop_def->name;

  // LOOKUP ONE: the provenance record.
  Provenance provenance;
  Status s = LoadProvenance(entity, prop, &provenance);
  if (!s.ok()) return s;
  out->provenance_found = true;

  out->batch_id = provenance.origin.batch_id;
  out->row_seq = provenance.origin.row_seq;
  out->column = provenance.origin.column;
  out->chain = provenance.chain;
  out->chain_fingerprint = provenance.chain_fingerprint;
  out->rule = onto::FusionRuleName(provenance.rule);
  out->confidence = provenance.confidence;
  out->rejected = provenance.rejected;
  out->cluster_size = provenance.cluster_size;
  out->merge_evidence = provenance.merge_evidence;
  out->stored_value = provenance.emitted_value;

  const onto::SourceSpec* spec = bundle_->Source(provenance.origin.source_id);
  if (spec == nullptr) {
    return Status::NotFound("provenance names source " +
                            std::to_string(provenance.origin.source_id) +
                            ", which the schema no longer declares");
  }
  out->source_key = spec->key;

  for (const auto id : provenance.chain) {
    const onto::Transform* transform = bundle_->transforms().ById(id);
    out->transform_names.push_back(transform == nullptr
                                       ? "unknown:" + std::to_string(id)
                                       : transform->name);
  }
  // A version bump changes the fingerprint. Detecting it here means the round
  // trip can say "this transform was revised" instead of reporting the data as
  // corrupt - a very different bug, deserving a different message.
  out->chain_changed =
      bundle_->transforms().ChainFingerprint(provenance.chain) !=
      provenance.chain_fingerprint;

  // LOOKUP TWO: the verbatim source row.
  s = store_->GetRawRecord(provenance.origin.source_id, provenance.origin.batch_id,
                           provenance.origin.row_seq, &out->raw_row);
  if (!s.ok()) return s;
  out->raw_row_found = true;

  s = RawCell(*spec, out->raw_row, provenance.origin.column, &out->raw_cell);
  if (!s.ok()) {
    out->replay_error = s.ToString();
    return Status::OK();  // the provenance is still worth showing
  }

  // THE REPLAY.
  std::string error;
  TValue input = TValue::String(out->raw_cell);
  if (out->raw_cell.find('\x1f') != std::string::npos) {
    // A multi-column property's chain expects a list, not a joined string.
    std::vector<std::string> parts;
    std::string current;
    for (const char c : out->raw_cell) {
      if (c == '\x1f') {
        parts.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    parts.push_back(current);
    input = TValue::StringList(std::move(parts));
  }

  TValue replayed = bundle_->transforms().Apply(provenance.chain, input, &error);
  if (prop_def != nullptr && !replayed.IsNull() &&
      prop_def->type == onto::ValueType::kStringList &&
      replayed.type() == onto::ValueType::kString) {
    replayed = TValue::StringList({replayed.AsString()});
  }
  out->replayed_value = replayed.ToDisplay();
  out->replay_error = error;

  // A UNION PROPERTY IS CHECKED BY CONTAINMENT, NOT EQUALITY.
  //
  // `alt_names` is the merge of every alias every source contributed, so no
  // single raw cell can reproduce the whole list. The honest question is
  // whether what THIS row contributed survived into the stored value.
  //
  // This rule used to live only in RoundTrip(), which meant the two callers
  // disagreed: the round trip counted a union property as verified and
  // /api/entities/{id} reported `verified: false` for the same property at the
  // same moment. The UI would have drawn a red mark on a value that is fine,
  // right next to a README claiming 100%. One definition, in one place.
  out->replay_is_union = prop_def != nullptr &&
                         prop_def->fuse == onto::FusionRule::kUnion;
  if (out->replay_is_union) {
    out->replay_matches = out->replayed_value.empty() ||
                          out->stored_value.find(out->replayed_value) !=
                              std::string::npos;
  } else {
    out->replay_matches = out->replayed_value == out->stored_value;
  }
  return Status::OK();
}

Status LineageReader::ExplainAll(codec::TypeId type, const codec::Ulid& entity,
                                 std::vector<Explanation>* out) const {
  out->clear();
  std::string payload;
  Status s = store_->GetEntity(type, entity, &payload);
  if (!s.ok()) return s;

  Slice slice(payload);
  ResolvedEntity decoded;
  if (!ResolvedEntity::DecodeFrom(&slice, &decoded)) {
    return Status::Corruption("undecodable entity");
  }

  for (const auto& [prop, value] : decoded.properties) {
    Explanation explanation;
    const Status es = Explain(type, entity, prop, &explanation);
    if (!es.ok()) continue;
    out->push_back(std::move(explanation));
  }
  return Status::OK();
}

Status LineageReader::RoundTrip(RoundTripReport* report,
                                size_t max_failures_reported) const {
  *report = RoundTripReport{};

  for (const auto& type_def : bundle_->ontology().types()) {
    auto it = store_->ScanEntities(type_def.id);
    for (; it->Valid(); it->Next()) {
      codec::TypeId type = 0;
      codec::Ulid id;
      if (!codec::DecodeEntityKey(it->key(), &type, &id)) continue;

      Slice value = it->value();
      ResolvedEntity entity;
      if (!ResolvedEntity::DecodeFrom(&value, &entity)) continue;
      ++report->entities;

      for (const auto& [prop, stored] : entity.properties) {
        ++report->properties;
        const onto::PropertyDef* def = type_def.Property(prop);

        RoundTripResult result;
        result.entity_id = id;
        result.entity_type = type;
        result.prop = prop;
        result.stored_value = stored.ToDisplay();

        Explanation explanation;
        const Status s = Explain(type, id, prop, &explanation);
        if (!s.ok()) {
          // Which lookup failed, not which status code came back. Both are
          // NotFound, and "this value has no provenance" is a very different
          // bug from "its provenance points at a row that is not there".
          result.failure = explanation.provenance_found
                               ? RoundTripFailure::kRawRowMissing
                               : RoundTripFailure::kNoProvenance;
          result.detail = s.ToString();
        } else if (!explanation.replay_error.empty() &&
                   explanation.replayed_value.empty()) {
          result.failure = RoundTripFailure::kColumnMissing;
          result.detail = explanation.replay_error;
        } else if (explanation.chain_changed) {
          result.failure = RoundTripFailure::kTransformChanged;
          result.detail = "the chain's version fingerprint no longer matches";
        } else if (explanation.replay_is_union) {
          // Containment rather than equality, decided in Explain() so that this
          // report and the per-property API can never disagree about whether a
          // union property verified.
          ++report->union_properties;
          if (!explanation.replay_matches) {
            result.failure = RoundTripFailure::kValueMismatch;
            result.replayed_value = explanation.replayed_value;
            result.detail = "the replayed value is not part of the union";
          }
        } else if (!explanation.replay_matches) {
          result.failure = RoundTripFailure::kValueMismatch;
          result.replayed_value = explanation.replayed_value;
          result.detail = "replaying " +
                          std::to_string(explanation.chain.size()) +
                          " transforms over \"" + explanation.raw_cell +
                          "\" gave \"" + explanation.replayed_value +
                          "\", stored is \"" + result.stored_value + "\"";
        }

        if (result.failure == RoundTripFailure::kNone) {
          ++report->verified;
        } else {
          ++report->failed;
          if (report->failures.size() < max_failures_reported) {
            report->failures.push_back(std::move(result));
          }
        }
      }
    }
    const Status s = it->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

}  // namespace sextant::lineage
