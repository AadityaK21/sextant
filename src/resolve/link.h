// Link resolution: the second pass that turns references into edges.
//
// WHY THIS CANNOT HAPPEN DURING INGEST
//
// Digitraffic says a port call arrived at "NLRTM". That is a reference to a
// Port, but which Port entity it is depends on entity resolution having run -
// and at ingest time it has not. So the mapper records an unresolved LinkRef
// carrying the match property and the value, and this file resolves it later.
//
// The resolution itself is a secondary-index lookup:
//
//     IDX | Port | locode | "NLRTM" | <entity id>
//
// which is why `mapping.cpp` refuses to load a link whose match property is not
// declared `indexed: true`. Without the index this would be a full scan of
// every Port for every link, and there are more links than entities.
//
// WHY IT IS A SEPARATE PASS RATHER THAN PART OF FUSION
//
// A link can point forwards. A Voyage ingested before its Port has been
// resolved would find nothing, so every entity has to exist before any edge is
// written. Two passes is the honest structure: build the nodes, then the edges.
//
// WHAT GETS WRITTEN, AND THE ONE THAT MATTERS
//
//   LINKOUT   src | type | dst      forward traversal
//   LINKIN    dst | type | src      reverse traversal, the denormalisation
//   TIDX      type | dst | ts | src time-ordered, and ONLY when the ontology
//                                   declares `time_index:` on the link
//
// That last one is the whole reason the quarter-query is a range scan. The
// declaration lives in the schema, so which links get a time index is a
// decision a domain expert makes in YAML rather than one buried here.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "blocking.h"
#include "bundle.h"
#include "record_ref.h"
#include "sextant/lsm/status.h"
#include "store.h"

namespace sextant::resolve {

struct LinkReport {
  uint64_t references_seen = 0;
  uint64_t edges_written = 0;
  uint64_t time_indexed = 0;
  // A reference whose target does not exist. Common and not an error: the
  // Digitraffic feed names ports that are not in the port list, and a link to
  // nothing is better dropped than invented.
  uint64_t unresolved = 0;
  // A reference whose SOURCE record never made it into an entity, which would
  // be a bug rather than missing data.
  uint64_t orphaned = 0;

  std::unordered_map<std::string, uint64_t> by_link_type;
};

// Reads every SourceRecord's links, finds both endpoints, and writes the edges.
// Must run after every entity has been written.
Status ResolveLinks(codec::Store* store, const ontology::SchemaBundle& bundle,
                    const ResolverProperties& props, LinkReport* report);

}  // namespace sextant::resolve
