// The ontology: entity types, their properties, and the links between them.
//
// This is the declarative layer the whole brief is really about. Adding a
// `Berth` type should require editing schema/ontology.yaml and nothing else -
// no C++, no frontend code, no migration. The way that is achieved is that
// nothing above this file ever names a property in source: keys carry numeric
// ids, the query planner looks up whether a property is indexed, and the React
// app fetches the schema from /api/ontology and renders whatever it finds.
//
// The loader's real job is therefore VALIDATION. A schema that loads is a
// schema the rest of the system can trust, so every check that would otherwise
// become a null pointer three layers up happens here:
//
//   * ids are unique and non-zero            - zero is reserved for "none"
//   * link endpoints name types that exist
//   * a time_index names a real property of the right entity type, and that
//     property is a timestamp
//   * at most one title property per type
//   * a display template only references properties that exist
//
// All of these fail at startup with a message naming the offending line's
// subject, which is the difference between a schema typo costing thirty
// seconds and costing an afternoon.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "keyspace.h"
#include "sextant/lsm/status.h"
#include "value.h"

namespace sextant::ontology {

using lsm::Status;

using codec::LinkTypeId;
using codec::PropId;
using codec::TypeId;

// How the fused value for a property is chosen when several sources disagree.
// Consumed on day 10; declared here because it belongs to the schema.
enum class FusionRule {
  kMostTrusted,    // highest source trust wins
  kMostRecent,     // latest observation wins
  kMostFrequent,   // majority vote across sources
  kNumericMedian,  // median, which resists one bad coordinate
  kLongest,        // the most complete-looking string
  kUnion,          // keep everything, for list-valued properties
};

const char* FusionRuleName(FusionRule rule);
bool ParseFusionRule(const std::string& name, FusionRule* out);

enum class Cardinality { kManyToOne, kOneToMany, kManyToMany, kOneToOne };

const char* CardinalityName(Cardinality c);
bool ParseCardinality(const std::string& name, Cardinality* out);

struct PropertyDef {
  PropId id = 0;
  std::string name;
  ValueType type = ValueType::kString;

  // The property shown as the entity's label. One per type.
  bool title = false;
  // Populates the IDX keyspace. Without this a lookup by value is a full scan.
  bool indexed = false;
  // A hint to the resolver that an exact match on this property is strong
  // evidence of identity. Not a uniqueness constraint: sources violate it, and
  // a constraint that rejects real data would just mean discarding the rows
  // that most need resolving.
  bool unique_hint = false;

  FusionRule fuse = FusionRule::kMostTrusted;
  // Non-empty only for enum properties. Storage type is still string.
  std::vector<std::string> enum_values;

  bool IsEnum() const { return !enum_values.empty(); }
  bool Permits(const std::string& value) const;
};

struct EntityTypeDef {
  TypeId id = 0;
  std::string name;
  std::string description;
  std::string display;  // e.g. "{name} ({locode})"
  std::vector<PropertyDef> properties;

  const PropertyDef* Property(const std::string& name) const;
  const PropertyDef* Property(PropId id) const;
  const PropertyDef* TitleProperty() const;
};

struct LinkTypeDef {
  LinkTypeId id = 0;
  std::string name;
  TypeId from = 0;
  TypeId to = 0;
  Cardinality cardinality = Cardinality::kManyToOne;
  std::string inverse;  // the name this link is browsed under from the target

  // The property whose timestamp orders this link in the TIDX keyspace. Zero
  // means the link is not time-indexed, and a time-bounded traversal over it
  // has to fall back to scanning LINKIN and filtering.
  PropId time_index = 0;
  bool HasTimeIndex() const { return time_index != 0; }
};

class Ontology {
 public:
  static Status LoadFromFile(const std::string& path, Ontology* out);
  static Status LoadFromString(const std::string& yaml, Ontology* out,
                               const std::string& origin = "<string>");

  int version() const { return version_; }
  const std::string& name_space() const { return namespace_; }

  const std::vector<EntityTypeDef>& types() const { return types_; }
  const std::vector<LinkTypeDef>& links() const { return links_; }

  const EntityTypeDef* Type(const std::string& name) const;
  const EntityTypeDef* Type(TypeId id) const;
  const LinkTypeDef* Link(const std::string& name) const;
  const LinkTypeDef* Link(LinkTypeId id) const;

  // Property ids are unique across the schema, so a provenance record can be
  // read without knowing which entity type it came from.
  const PropertyDef* Property(PropId id) const;
  const EntityTypeDef* TypeOfProperty(PropId id) const;

  // Render an entity's display template. Missing properties render empty.
  std::string RenderDisplay(const EntityTypeDef& type,
                            const std::vector<std::pair<PropId, std::string>>&
                                values) const;

 private:
  Status Validate(const std::string& origin);

  int version_ = 0;
  std::string namespace_;
  std::vector<EntityTypeDef> types_;
  std::vector<LinkTypeDef> links_;
};

}  // namespace sextant::ontology
