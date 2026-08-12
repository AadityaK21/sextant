// Everything declarative the system needs, loaded together and validated once.
//
// The ontology, the transform registry and every source mapping form one unit:
// a mapping is meaningless without the ontology it targets, and a schema
// nobody maps onto is unreachable. Loading them separately would let a process
// start with a mapping that references a property the ontology no longer has,
// and only discover it on the row that used it.
//
// So this loads the directory, resolves every name, and fails at startup if
// anything does not line up. A schema typo costs a startup error rather than a
// property that quietly never appears in the output.

#pragma once

#include <string>
#include <vector>

#include "mapping.h"
#include "schema.h"
#include "sextant/lsm/status.h"
#include "transform.h"

namespace sextant::ontology {

class SchemaBundle {
 public:
  // Expects <dir>/ontology.yaml and <dir>/mappings/*.yaml.
  static Status LoadFromDir(const std::string& dir, SchemaBundle* out);

  const Ontology& ontology() const { return ontology_; }
  const TransformRegistry& transforms() const { return transforms_; }
  const std::vector<SourceSpec>& sources() const { return sources_; }

  const SourceSpec* Source(const std::string& key) const;
  const SourceSpec* Source(codec::SourceId id) const;

 private:
  Ontology ontology_;
  TransformRegistry transforms_;
  std::vector<SourceSpec> sources_;
};

}  // namespace sextant::ontology
