#include "bundle.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "env.h"

namespace sextant::ontology {

const SourceSpec* SchemaBundle::Source(const std::string& key) const {
  for (const auto& s : sources_) {
    if (s.key == key) return &s;
  }
  return nullptr;
}

const SourceSpec* SchemaBundle::Source(codec::SourceId id) const {
  for (const auto& s : sources_) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

Status SchemaBundle::LoadFromDir(const std::string& dir, SchemaBundle* out) {
  SchemaBundle bundle;

  const std::string ontology_path = dir + "/ontology.yaml";
  Status s = Ontology::LoadFromFile(ontology_path, &bundle.ontology_);
  if (!s.ok()) return s;

  const std::string mappings_dir = dir + "/mappings";
  std::vector<std::string> children;
  s = lsm::GetChildren(mappings_dir, &children);
  if (!s.ok()) {
    return Status::NotFound("no mapping directory at " + mappings_dir);
  }

  // Sorted, so the order sources are loaded in does not depend on how the
  // filesystem happens to return them. Anything that reports per-source numbers
  // is easier to diff between runs when the order is stable.
  std::sort(children.begin(), children.end());

  std::set<codec::SourceId> ids;
  std::set<std::string> keys;
  for (const auto& child : children) {
    if (child.size() < 6) continue;
    if (child.compare(child.size() - 5, 5, ".yaml") != 0) continue;

    SourceSpec spec;
    s = SourceSpec::LoadFromFile(mappings_dir + "/" + child, bundle.ontology_,
                                 bundle.transforms_, &spec);
    if (!s.ok()) return s;

    // Two sources sharing a numeric id would write into each other's RAW and
    // SRCREC ranges, which is silent corruption rather than a visible failure.
    if (!ids.insert(spec.id).second) {
      return Status::InvalidArgument(
          "two sources share source_id " + std::to_string(spec.id) +
          " (found again in " + child + "); ids must be unique across mappings");
    }
    if (!keys.insert(spec.key).second) {
      return Status::InvalidArgument("two sources share the id \"" + spec.key +
                                     "\" (found again in " + child + ")");
    }
    bundle.sources_.push_back(std::move(spec));
  }

  if (bundle.sources_.empty()) {
    return Status::NotFound("no mapping files found in " + mappings_dir);
  }

  *out = std::move(bundle);
  return Status::OK();
}

}  // namespace sextant::ontology
