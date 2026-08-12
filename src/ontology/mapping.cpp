#include "mapping.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "hash.h"
#include "value.h"

namespace sextant::ontology {
namespace {

Status Bad(const std::string& origin, const std::string& what) {
  return Status::InvalidArgument(origin + ": " + what);
}

std::string OptString(const YAML::Node& n, const char* key,
                      const std::string& fallback = {}) {
  const YAML::Node v = n[key];
  return v && v.IsScalar() ? v.as<std::string>() : fallback;
}

// A YAML value that may be a single scalar or a sequence of them. `from` and
// `transform` both accept either form, because writing a one-element list for
// the common case is noise.
std::vector<std::string> StringOrList(const YAML::Node& n) {
  std::vector<std::string> out;
  if (!n) return out;
  if (n.IsScalar()) {
    out.push_back(n.as<std::string>());
  } else if (n.IsSequence()) {
    for (const auto& v : n) out.push_back(v.as<std::string>());
  }
  return out;
}

// The separator between natural key fields. Unit Separator, chosen because it
// cannot appear in a CSV cell or a JSON string that came from real data, so the
// joined key is unambiguous and readable in a debugger.
constexpr char kKeySeparator = '\x1f';

}  // namespace

const char* ConnectorKindName(ConnectorKind kind) {
  switch (kind) {
    case ConnectorKind::kCsv: return "csv";
    case ConnectorKind::kHttp: return "http";
    case ConnectorKind::kPostgres: return "postgres";
  }
  return "unknown";
}

bool ParseConnectorKind(const std::string& name, ConnectorKind* out) {
  if (name == "csv") { *out = ConnectorKind::kCsv; return true; }
  if (name == "http") { *out = ConnectorKind::kHttp; return true; }
  if (name == "postgres") { *out = ConnectorKind::kPostgres; return true; }
  return false;
}

const EndpointSpec* SourceSpec::Endpoint(const std::string& eid) const {
  for (const auto& e : endpoints) {
    if (e.id == eid) return &e;
  }
  return nullptr;
}

// --- loading ----------------------------------------------------------------

Status SourceSpec::LoadFromFile(const std::string& path, const Ontology& onto,
                                const TransformRegistry& transforms,
                                SourceSpec* out) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
    return LoadFromString(YAML::Dump(root), onto, transforms, out, path);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(path + ": " + e.what());
  }
}

Status SourceSpec::LoadFromString(const std::string& yaml, const Ontology& onto,
                                  const TransformRegistry& transforms,
                                  SourceSpec* out, const std::string& origin) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(origin + ": " + e.what());
  }

  SourceSpec spec;
  try {
    const YAML::Node src = root["source"];
    if (!src || !src.IsMap()) return Bad(origin, "missing `source` block");

    spec.key = OptString(src, "id");
    if (spec.key.empty()) return Bad(origin, "source has no `id`");

    const YAML::Node sid = src["source_id"];
    if (!sid || !sid.IsScalar()) {
      return Bad(origin, "source " + spec.key +
                             " needs a numeric `source_id`; it is written into"
                             " every RAW, SRCREC and XREF key, so it cannot be"
                             " derived from the file name");
    }
    const int64_t numeric = sid.as<int64_t>();
    if (numeric <= 0 || numeric > 0xFFFFFFFFLL) {
      return Bad(origin, "source " + spec.key + " has `source_id` out of range");
    }
    spec.id = static_cast<codec::SourceId>(numeric);

    spec.name = OptString(src, "name", spec.key);
    const std::string kind = OptString(src, "connector");
    if (!ParseConnectorKind(kind, &spec.connector)) {
      return Bad(origin, "source " + spec.key + " has unknown connector \"" +
                             kind + "\"");
    }
    spec.uri = OptString(src, "uri");
    spec.base_url = OptString(src, "base_url");
    spec.snapshot_dir = OptString(src, "snapshot_dir");
    spec.dsn = OptString(src, "dsn");
    spec.query = OptString(src, "query");
    spec.params = StringOrList(src["params"]);
    if (src["cursor_size"] && src["cursor_size"].IsScalar()) {
      spec.cursor_size = src["cursor_size"].as<int>();
    }
    if (src["trust"] && src["trust"].IsScalar()) {
      spec.trust = src["trust"].as<double>();
    }
    if (spec.trust < 0.0 || spec.trust > 1.0) {
      return Bad(origin, "source " + spec.key + " has `trust` outside 0..1");
    }
    if (src["headers"] && src["headers"].IsMap()) {
      for (const auto& kv : src["headers"]) {
        spec.headers.emplace_back(kv.first.as<std::string>(),
                                  kv.second.as<std::string>());
      }
    }
    spec.natural_key = StringOrList(src["natural_key"]);

    if (src["filter"] && src["filter"].IsSequence()) {
      for (const auto& f : src["filter"]) {
        FilterRule rule;
        rule.column = OptString(f, "column");
        rule.pattern = OptString(f, "matches");
        if (rule.column.empty() || rule.pattern.empty()) {
          return Bad(origin, "a filter needs both `column` and `matches`");
        }
        try {
          rule.compiled = std::make_shared<std::regex>(rule.pattern);
        } catch (const std::regex_error& e) {
          return Bad(origin, "filter on " + rule.column + " has a bad pattern \"" +
                                 rule.pattern + "\": " + e.what());
        }
        spec.filters.push_back(std::move(rule));
      }
    }

    if (root["endpoints"] && root["endpoints"].IsSequence()) {
      for (const auto& e : root["endpoints"]) {
        EndpointSpec ep;
        ep.id = OptString(e, "id");
        ep.path = OptString(e, "path");
        ep.records_at = OptString(e, "records_at");
        if (ep.id.empty() || ep.path.empty()) {
          return Bad(origin, "an endpoint needs both `id` and `path`");
        }
        spec.endpoints.push_back(std::move(ep));
      }
    }

    const YAML::Node mappings = root["mappings"];
    if (!mappings || !mappings.IsSequence() || mappings.size() == 0) {
      return Bad(origin, "source " + spec.key + " declares no `mappings`");
    }

    for (const auto& m : mappings) {
      TypeMapping tm;
      tm.target_type_name = OptString(m, "target_type");
      const EntityTypeDef* type = onto.Type(tm.target_type_name);
      if (type == nullptr) {
        return Bad(origin, "mapping targets unknown entity type \"" +
                               tm.target_type_name + "\"");
      }
      tm.target_type = type->id;
      tm.from_endpoint = OptString(m, "from_endpoint");
      if (!tm.from_endpoint.empty() && spec.Endpoint(tm.from_endpoint) == nullptr) {
        return Bad(origin, "mapping for " + tm.target_type_name +
                               " reads endpoint \"" + tm.from_endpoint +
                               "\", which is not declared");
      }
      tm.natural_key = StringOrList(m["natural_key"]);
      if (tm.natural_key.empty()) tm.natural_key = spec.natural_key;
      if (tm.natural_key.empty()) {
        // Without one there is no stable identity for the row, so a re-ingest
        // would create a second SRCREC for the same real-world thing instead of
        // replacing the first.
        return Bad(origin, "mapping for " + tm.target_type_name +
                               " has no `natural_key`, and the source declares"
                               " no default");
      }

      const YAML::Node props = m["properties"];
      if (!props || !props.IsMap()) {
        return Bad(origin, "mapping for " + tm.target_type_name +
                               " has no `properties`");
      }
      for (const auto& pkv : props) {
        PropertyMapping pm;
        pm.prop_name = pkv.first.as<std::string>();
        const PropertyDef* def = type->Property(pm.prop_name);
        if (def == nullptr) {
          return Bad(origin, tm.target_type_name + " has no property \"" +
                                 pm.prop_name + "\"");
        }
        pm.prop = def->id;
        pm.target_type = def->type;

        pm.from = StringOrList(pkv.second["from"]);
        if (pm.from.empty()) {
          return Bad(origin, tm.target_type_name + "." + pm.prop_name +
                                 " has no `from`");
        }
        pm.chain_names = StringOrList(pkv.second["transform"]);
        std::string err;
        if (!transforms.ResolveChain(pm.chain_names, &pm.chain, &err)) {
          return Bad(origin, tm.target_type_name + "." + pm.prop_name + ": " + err);
        }
        pm.chain_fingerprint = transforms.ChainFingerprint(pm.chain);
        tm.properties.push_back(std::move(pm));
      }

      if (m["links"] && m["links"].IsSequence()) {
        for (const auto& l : m["links"]) {
          LinkMapping lm;
          lm.link_name = OptString(l, "type");
          const LinkTypeDef* link = onto.Link(lm.link_name);
          if (link == nullptr) {
            return Bad(origin, "unknown link type \"" + lm.link_name + "\"");
          }
          if (link->from != tm.target_type) {
            return Bad(origin, "link \"" + lm.link_name + "\" starts at " +
                                   std::to_string(link->from) + " but is declared"
                                   " on a mapping for " + tm.target_type_name);
          }
          lm.link_type = link->id;
          lm.target_type = link->to;

          const YAML::Node via = l["via"];
          if (!via || !via.IsMap()) {
            return Bad(origin, "link \"" + lm.link_name + "\" has no `via`");
          }
          lm.from = OptString(via, "from");
          const std::string match = OptString(via, "match_property");
          const EntityTypeDef* target = onto.Type(link->to);
          const PropertyDef* mp =
              target == nullptr ? nullptr : target->Property(match);
          if (lm.from.empty() || mp == nullptr) {
            return Bad(origin, "link \"" + lm.link_name +
                                   "\" needs `via.from` and a `via.match_property`"
                                   " that exists on the target type");
          }
          // Matching on an unindexed property would turn every link resolution
          // into a full scan of the target type.
          if (!mp->indexed) {
            return Bad(origin, "link \"" + lm.link_name + "\" matches on " +
                                   target->name + "." + match +
                                   ", which is not `indexed: true`");
          }
          lm.match_property = mp->id;
          tm.links.push_back(std::move(lm));
        }
      }

      spec.mappings.push_back(std::move(tm));
    }
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(origin + ": " + e.what());
  }

  *out = std::move(spec);
  return Status::OK();
}

// --- mapping ----------------------------------------------------------------

Mapper::Mapper(const Ontology* ontology, const TransformRegistry* transforms,
               const SourceSpec* spec)
    : ontology_(ontology), transforms_(transforms), spec_(spec) {}

bool Mapper::Accepts(const Row& row) const {
  for (const auto& f : spec_->filters) {
    std::string cell;
    // A row missing the filter column is rejected. The alternative - letting it
    // through - would mean a renamed column silently disables the filter and
    // quietly multiplies the record count.
    if (!row.Get(f.column, &cell)) return false;
    if (!std::regex_search(cell, *f.compiled)) return false;
  }
  return true;
}

Status Mapper::MapRow(const Row& row, codec::BatchId batch, codec::RowSeq row_seq,
                      const std::string& endpoint,
                      std::vector<SourceRecord>* out) const {
  for (const auto& tm : spec_->mappings) {
    if (!tm.from_endpoint.empty() && tm.from_endpoint != endpoint) continue;

    // The natural key first: no identity, no record.
    std::vector<std::string> key_parts;
    key_parts.reserve(tm.natural_key.size());
    bool have_key = false;
    for (const auto& col : tm.natural_key) {
      std::string cell;
      if (row.Get(col, &cell) && !cell.empty()) have_key = true;
      key_parts.push_back(cell);
    }
    if (!have_key) continue;

    SourceRecord rec;
    rec.source_id = spec_->id;
    rec.batch_id = batch;
    rec.row_seq = row_seq;
    rec.type = tm.target_type;
    for (size_t i = 0; i < key_parts.size(); ++i) {
      if (i != 0) rec.natural_key.push_back(kKeySeparator);
      rec.natural_key += key_parts[i];
    }
    rec.natural_key_hash =
        codec::Hash64Fields(key_parts.data(), key_parts.size());

    for (const auto& pm : tm.properties) {
      // Gather the source cells. One column gives a string; several give a
      // string list, which is what `concat` is for.
      std::vector<std::string> cells;
      bool any_present = false;
      for (const auto& col : pm.from) {
        std::string cell;
        if (row.Get(col, &cell)) any_present = true;
        cells.push_back(cell);
      }
      if (!any_present) continue;  // the source simply does not carry this

      PropertyCell out_cell;
      out_cell.prop = pm.prop;
      out_cell.chain = pm.chain;
      out_cell.chain_fingerprint = pm.chain_fingerprint;
      out_cell.origin.source_id = spec_->id;
      out_cell.origin.batch_id = batch;
      out_cell.origin.row_seq = row_seq;

      TValue input;
      if (cells.size() == 1) {
        out_cell.origin.column = pm.from[0];
        out_cell.raw_value = cells[0];
        input = TValue::String(cells[0]);
      } else {
        // Several columns: record all of them, joined, so the lineage panel can
        // highlight every cell that contributed rather than an arbitrary one.
        for (size_t i = 0; i < pm.from.size(); ++i) {
          if (i != 0) {
            out_cell.origin.column += "+";
            out_cell.raw_value.push_back(kKeySeparator);
          }
          out_cell.origin.column += pm.from[i];
          out_cell.raw_value += cells[i];
        }
        input = TValue::StringList(cells);
      }

      std::string error;
      TValue value = transforms_->Apply(pm.chain, input, &error);
      out_cell.error = error;

      // Coerce to the declared type where the intent is unambiguous. A scalar
      // reaching a list-valued property becomes a one-element list, which is
      // how a source with a single alternate-name column feeds a property that
      // other sources supply several values for.
      if (!value.IsNull()) {
        if (pm.target_type == ValueType::kStringList &&
            value.type() == ValueType::kString) {
          value = TValue::StringList({value.AsString()});
        } else if (pm.target_type != ValueType::kStringList &&
                   value.type() == ValueType::kStringList) {
          const auto& items = value.AsStringList();
          value = items.empty() ? TValue::Null() : TValue::String(items.front());
        }
      }

      // A property declared as an enum but carrying a value outside its set is
      // recorded with the reason rather than dropped, because "the source said
      // something we do not recognise" is exactly the kind of thing the review
      // queue exists to surface.
      const EntityTypeDef* type = ontology_->Type(tm.target_type);
      const PropertyDef* def = type == nullptr ? nullptr : type->Property(pm.prop);
      if (def != nullptr && def->IsEnum() && value.type() == ValueType::kString &&
          !def->Permits(value.AsString())) {
        out_cell.error = "\"" + value.AsString() + "\" is not one of " +
                         tm.target_type_name + "." + pm.prop_name + "'s"
                         " permitted values";
        value = TValue::Null();
      }

      out_cell.value = std::move(value);
      rec.properties.push_back(std::move(out_cell));
    }

    for (const auto& lm : tm.links) {
      std::string cell;
      if (!row.Get(lm.from, &cell) || cell.empty()) continue;
      LinkRef ref;
      ref.link_type = lm.link_type;
      ref.target_type = lm.target_type;
      ref.match_property = lm.match_property;
      ref.match_value = cell;
      ref.origin.source_id = spec_->id;
      ref.origin.batch_id = batch;
      ref.origin.row_seq = row_seq;
      ref.origin.column = lm.from;
      rec.links.push_back(std::move(ref));
    }

    out->push_back(std::move(rec));
  }
  return Status::OK();
}

}  // namespace sextant::ontology
