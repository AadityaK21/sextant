#include "schema.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "value.h"

namespace sextant::ontology {
namespace {

Status Bad(const std::string& origin, const std::string& what) {
  return Status::InvalidArgument(origin + ": " + what);
}

// yaml-cpp returns a null node for a missing key rather than throwing, so
// optional fields read cleanly. Required fields are checked explicitly.
std::string OptString(const YAML::Node& n, const char* key,
                      const std::string& fallback = {}) {
  const YAML::Node v = n[key];
  return v && v.IsScalar() ? v.as<std::string>() : fallback;
}

bool OptBool(const YAML::Node& n, const char* key, bool fallback = false) {
  const YAML::Node v = n[key];
  return v && v.IsScalar() ? v.as<bool>() : fallback;
}

bool OptInt(const YAML::Node& n, const char* key, int64_t* out) {
  const YAML::Node v = n[key];
  if (!v || !v.IsScalar()) return false;
  *out = v.as<int64_t>();
  return true;
}

}  // namespace

const char* FusionRuleName(FusionRule rule) {
  switch (rule) {
    case FusionRule::kMostTrusted: return "most_trusted";
    case FusionRule::kMostRecent: return "most_recent";
    case FusionRule::kMostFrequent: return "most_frequent";
    case FusionRule::kNumericMedian: return "numeric_median";
    case FusionRule::kLongest: return "longest";
    case FusionRule::kUnion: return "union";
  }
  return "unknown";
}

bool ParseFusionRule(const std::string& name, FusionRule* out) {
  if (name == "most_trusted") { *out = FusionRule::kMostTrusted; return true; }
  if (name == "most_recent") { *out = FusionRule::kMostRecent; return true; }
  if (name == "most_frequent") { *out = FusionRule::kMostFrequent; return true; }
  if (name == "numeric_median") { *out = FusionRule::kNumericMedian; return true; }
  if (name == "longest") { *out = FusionRule::kLongest; return true; }
  if (name == "union") { *out = FusionRule::kUnion; return true; }
  return false;
}

const char* CardinalityName(Cardinality c) {
  switch (c) {
    case Cardinality::kManyToOne: return "many_to_one";
    case Cardinality::kOneToMany: return "one_to_many";
    case Cardinality::kManyToMany: return "many_to_many";
    case Cardinality::kOneToOne: return "one_to_one";
  }
  return "unknown";
}

bool ParseCardinality(const std::string& name, Cardinality* out) {
  if (name == "many_to_one") { *out = Cardinality::kManyToOne; return true; }
  if (name == "one_to_many") { *out = Cardinality::kOneToMany; return true; }
  if (name == "many_to_many") { *out = Cardinality::kManyToMany; return true; }
  if (name == "one_to_one") { *out = Cardinality::kOneToOne; return true; }
  return false;
}

// --- lookups ----------------------------------------------------------------

bool PropertyDef::Permits(const std::string& value) const {
  if (enum_values.empty()) return true;
  for (const auto& v : enum_values) {
    if (v == value) return true;
  }
  return false;
}

const PropertyDef* EntityTypeDef::Property(const std::string& n) const {
  for (const auto& p : properties) {
    if (p.name == n) return &p;
  }
  return nullptr;
}

const PropertyDef* EntityTypeDef::Property(PropId pid) const {
  for (const auto& p : properties) {
    if (p.id == pid) return &p;
  }
  return nullptr;
}

const PropertyDef* EntityTypeDef::TitleProperty() const {
  for (const auto& p : properties) {
    if (p.title) return &p;
  }
  return properties.empty() ? nullptr : &properties.front();
}

const EntityTypeDef* Ontology::Type(const std::string& n) const {
  for (const auto& t : types_) {
    if (t.name == n) return &t;
  }
  return nullptr;
}

const EntityTypeDef* Ontology::Type(TypeId id) const {
  for (const auto& t : types_) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

const LinkTypeDef* Ontology::Link(const std::string& n) const {
  for (const auto& l : links_) {
    if (l.name == n) return &l;
  }
  return nullptr;
}

const LinkTypeDef* Ontology::Link(LinkTypeId id) const {
  for (const auto& l : links_) {
    if (l.id == id) return &l;
  }
  return nullptr;
}

const PropertyDef* Ontology::Property(PropId id) const {
  for (const auto& t : types_) {
    if (const PropertyDef* p = t.Property(id)) return p;
  }
  return nullptr;
}

const EntityTypeDef* Ontology::TypeOfProperty(PropId id) const {
  for (const auto& t : types_) {
    if (t.Property(id) != nullptr) return &t;
  }
  return nullptr;
}

std::string Ontology::RenderDisplay(
    const EntityTypeDef& type,
    const std::vector<std::pair<PropId, std::string>>& values) const {
  std::string out;
  const std::string& tpl = type.display;
  for (size_t i = 0; i < tpl.size(); ++i) {
    if (tpl[i] != '{') {
      out.push_back(tpl[i]);
      continue;
    }
    const size_t close = tpl.find('}', i);
    if (close == std::string::npos) {
      out.append(tpl.substr(i));
      break;
    }
    const std::string name = tpl.substr(i + 1, close - i - 1);
    const PropertyDef* p = type.Property(name);
    if (p != nullptr) {
      for (const auto& [pid, text] : values) {
        if (pid == p->id) {
          out += text;
          break;
        }
      }
    }
    i = close;
  }
  return out;
}

// --- loading ----------------------------------------------------------------

Status Ontology::LoadFromFile(const std::string& path, Ontology* out) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(path + ": " + e.what());
  }
  // Parse and validate go through the same path as LoadFromString so there is
  // only one place where the schema's rules are enforced.
  try {
    return LoadFromString(YAML::Dump(root), out, path);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(path + ": " + e.what());
  }
}

Status Ontology::LoadFromString(const std::string& yaml, Ontology* out,
                                const std::string& origin) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(origin + ": " + e.what());
  }

  Ontology parsed;
  try {
    int64_t version = 0;
    if (!OptInt(root, "version", &version)) {
      return Bad(origin, "missing top-level `version`");
    }
    parsed.version_ = static_cast<int>(version);
    parsed.namespace_ = OptString(root, "namespace", "default");

    const YAML::Node types = root["entity_types"];
    if (!types || !types.IsMap()) {
      return Bad(origin, "missing or malformed `entity_types`");
    }

    for (const auto& kv : types) {
      EntityTypeDef t;
      t.name = kv.first.as<std::string>();
      const YAML::Node& body = kv.second;
      if (!body.IsMap()) return Bad(origin, "entity type " + t.name + " is not a map");

      int64_t id = 0;
      if (!OptInt(body, "id", &id) || id <= 0 || id > 0xFFFF) {
        return Bad(origin, "entity type " + t.name +
                               " needs an `id` in 1..65535 - see the comment at"
                               " the top of ontology.yaml for why it cannot be"
                               " derived");
      }
      t.id = static_cast<TypeId>(id);
      t.description = OptString(body, "description");
      t.display = OptString(body, "display");

      const YAML::Node props = body["properties"];
      if (!props || !props.IsMap()) {
        return Bad(origin, "entity type " + t.name + " has no `properties`");
      }
      for (const auto& pkv : props) {
        PropertyDef p;
        p.name = pkv.first.as<std::string>();
        const YAML::Node& pb = pkv.second;
        if (!pb.IsMap()) {
          return Bad(origin, t.name + "." + p.name + " is not a map");
        }

        int64_t pid = 0;
        if (!OptInt(pb, "id", &pid) || pid <= 0 || pid > 0xFFFF) {
          return Bad(origin, t.name + "." + p.name + " needs an `id` in 1..65535");
        }
        p.id = static_cast<PropId>(pid);

        const std::string type_name = OptString(pb, "type");
        if (type_name.empty()) {
          return Bad(origin, t.name + "." + p.name + " has no `type`");
        }
        if (!ParseValueType(type_name, &p.type)) {
          return Bad(origin, t.name + "." + p.name + " has unknown type \"" +
                                 type_name + "\"");
        }
        if (type_name == "enum") {
          const YAML::Node values = pb["values"];
          if (!values || !values.IsSequence() || values.size() == 0) {
            return Bad(origin, t.name + "." + p.name +
                                   " is an enum but declares no `values`");
          }
          for (const auto& v : values) p.enum_values.push_back(v.as<std::string>());
        }

        p.title = OptBool(pb, "title");
        p.indexed = OptBool(pb, "indexed");
        p.unique_hint = OptBool(pb, "unique_hint");

        const std::string fuse = OptString(pb, "fuse", "most_trusted");
        if (!ParseFusionRule(fuse, &p.fuse)) {
          return Bad(origin, t.name + "." + p.name + " has unknown fusion rule \"" +
                                 fuse + "\"");
        }
        // A list-valued property fused by anything but union would silently
        // throw away every value but one, which is never what the schema author
        // meant by declaring it a list in the first place.
        if (p.type == ValueType::kStringList && p.fuse != FusionRule::kUnion) {
          return Bad(origin, t.name + "." + p.name +
                                 " is a list but its fusion rule is \"" + fuse +
                                 "\"; lists must fuse with `union`");
        }

        t.properties.push_back(std::move(p));
      }
      parsed.types_.push_back(std::move(t));
    }

    const YAML::Node links = root["link_types"];
    if (links && links.IsMap()) {
      for (const auto& kv : links) {
        LinkTypeDef l;
        l.name = kv.first.as<std::string>();
        const YAML::Node& body = kv.second;
        if (!body.IsMap()) return Bad(origin, "link type " + l.name + " is not a map");

        int64_t id = 0;
        if (!OptInt(body, "id", &id) || id <= 0 || id > 0xFFFF) {
          return Bad(origin, "link type " + l.name + " needs an `id` in 1..65535");
        }
        l.id = static_cast<LinkTypeId>(id);

        const std::string from = OptString(body, "from");
        const std::string to = OptString(body, "to");
        const EntityTypeDef* ft = parsed.Type(from);
        const EntityTypeDef* tt = parsed.Type(to);
        if (ft == nullptr) {
          return Bad(origin, "link type " + l.name + " has unknown `from` type \"" +
                                 from + "\"");
        }
        if (tt == nullptr) {
          return Bad(origin, "link type " + l.name + " has unknown `to` type \"" +
                                 to + "\"");
        }
        l.from = ft->id;
        l.to = tt->id;

        const std::string card = OptString(body, "cardinality", "many_to_one");
        if (!ParseCardinality(card, &l.cardinality)) {
          return Bad(origin, "link type " + l.name + " has unknown cardinality \"" +
                                 card + "\"");
        }
        l.inverse = OptString(body, "inverse");

        const std::string ti = OptString(body, "time_index");
        if (!ti.empty()) {
          // This is the declaration that causes TIDX to be populated, so it is
          // worth being strict: an unnoticed typo here does not break anything
          // visibly, it just quietly makes the headline query a full scan.
          const PropertyDef* p = ft->Property(ti);
          if (p == nullptr) {
            return Bad(origin, "link type " + l.name + " time-indexes \"" + ti +
                                   "\", which is not a property of " + ft->name);
          }
          if (p->type != ValueType::kTimestamp) {
            return Bad(origin, "link type " + l.name + " time-indexes " + ft->name +
                                   "." + ti + ", which is a " +
                                   ValueTypeName(p->type) + " rather than a"
                                   " timestamp");
          }
          l.time_index = p->id;
        }
        parsed.links_.push_back(std::move(l));
      }
    }
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(origin + ": " + e.what());
  }

  const Status s = parsed.Validate(origin);
  if (!s.ok()) return s;
  *out = std::move(parsed);
  return Status::OK();
}

Status Ontology::Validate(const std::string& origin) {
  std::set<TypeId> type_ids;
  std::set<std::string> type_names;
  std::set<PropId> prop_ids;

  for (const auto& t : types_) {
    if (!type_ids.insert(t.id).second) {
      return Bad(origin, "duplicate entity type id " + std::to_string(t.id) +
                             " on " + t.name);
    }
    if (!type_names.insert(t.name).second) {
      return Bad(origin, "duplicate entity type name " + t.name);
    }

    int titles = 0;
    std::set<std::string> prop_names;
    for (const auto& p : t.properties) {
      // Global rather than per type. PROV keys carry a property id but no type
      // id, so an id that means `name` on Port and `imo` on Vessel would make a
      // provenance record impossible to interpret on its own.
      if (!prop_ids.insert(p.id).second) {
        return Bad(origin, "property id " + std::to_string(p.id) + " on " + t.name +
                               "." + p.name +
                               " is already used; property ids are unique across"
                               " the whole schema");
      }
      if (!prop_names.insert(p.name).second) {
        return Bad(origin, "duplicate property " + t.name + "." + p.name);
      }
      if (p.title) ++titles;
    }
    if (titles > 1) {
      return Bad(origin, t.name + " declares " + std::to_string(titles) +
                             " title properties; there can be at most one");
    }

    // A display template naming a property that does not exist renders as a
    // blank in the UI and is nearly impossible to spot from the outside.
    for (size_t i = 0; i < t.display.size(); ++i) {
      if (t.display[i] != '{') continue;
      const size_t close = t.display.find('}', i);
      if (close == std::string::npos) {
        return Bad(origin, t.name + " display template has an unclosed brace");
      }
      const std::string name = t.display.substr(i + 1, close - i - 1);
      if (t.Property(name) == nullptr) {
        return Bad(origin, t.name + " display template references \"" + name +
                               "\", which is not one of its properties");
      }
      i = close;
    }
  }

  std::set<LinkTypeId> link_ids;
  std::set<std::string> link_names;
  for (const auto& l : links_) {
    if (!link_ids.insert(l.id).second) {
      return Bad(origin, "duplicate link type id " + std::to_string(l.id) + " on " +
                             l.name);
    }
    if (!link_names.insert(l.name).second) {
      return Bad(origin, "duplicate link type name " + l.name);
    }
  }

  if (types_.empty()) return Bad(origin, "schema declares no entity types");
  return Status::OK();
}

}  // namespace sextant::ontology
