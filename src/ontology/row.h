// Row - one record from a source, before anything has been done to it.
//
// Three connectors, one interface. A CSV line, a JSON object and a Postgres
// tuple have nothing in common physically, but the mapping engine only ever
// needs two things from any of them: look up a named cell, and hand back the
// verbatim bytes for the RAW keyspace.
//
// That narrow interface is what keeps the connectors from leaking into the rest
// of the system. The mapper never learns whether it is reading a file or a
// cursor, which is why the same mapping YAML syntax works for all three, and
// why adding a fourth connector is a new Row implementation and nothing else.
//
// PATHS, NOT JUST COLUMN NAMES. Tabular sources use a plain column name. JSON
// sources use a dotted path with optional indices, because the shape of an API
// response is not flat:
//
//     portAreaDetails[0].ata
//
// Tabular Row implementations simply have no dots in their column names, so one
// syntax covers both without a mode flag.
//
// Raw() MUST BE VERBATIM. It is what gets written to RAW and what every lineage
// answer is eventually traced back to. A Row that re-serialises its parsed form
// instead of keeping the original bytes will drift from the source file in
// exactly the cases - odd quoting, unusual encodings - where a user is most
// likely to be looking.

#pragma once

#include <string>

namespace sextant::ontology {

class Row {
 public:
  virtual ~Row() = default;

  // Returns false if the path does not exist in this row. An existing but
  // empty cell returns true with an empty string: "blank" and "absent" are
  // different facts about a source and the mapper treats them differently.
  virtual bool Get(const std::string& path, std::string* out) const = 0;

  // The original bytes of this record, exactly as the source produced them.
  virtual std::string Raw() const = 0;
};

}  // namespace sextant::ontology
