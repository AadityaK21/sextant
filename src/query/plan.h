// The planner: choose an access path for every step, and say why.
//
// THE REASON STRING IS NOT DECORATION
//
// Every step carries a sentence explaining the choice. That is there because
// the failure mode of a system like this is silent degradation: a query that
// used to hit TIDX starts doing a full scan because someone removed a
// time_index from the schema, and nothing anywhere says so. It still returns
// the right answer, just a hundred times slower, and you find out in
// production.
//
// With the reason on the plan and the plan on the response, the frontend shows
// "full scan of Port: no index on `name`" in the same panel as the results,
// and the regression is visible the moment it happens.
//
// WHAT THE PLANNER ACTUALLY DECIDES
//
// Three choices, in order:
//
//   1. START SET. Ids given -> point lookups. Otherwise pick ONE indexed
//      equality or prefix predicate to serve from IDX and leave the rest as
//      residuals. If nothing is indexable, an entity-type scan, admitted as
//      such.
//
//   2. EACH HOP. A time window on a link that carries a time_index means TIDX:
//      a seek to the start of the window and a sequential read. No window, or
//      no time_index, means the plain adjacency keyspace, LINKOUT or LINKIN
//      depending on direction.
//
//   3. RESIDUALS. Everything not pushed into a key range gets applied to
//      materialised entities, and the plan records which predicates those were,
//      so nobody has to guess whether a filter was cheap or expensive.
//
// WHY ONE INDEX AND NOT AN INTERSECTION
//
// Two indexed equality predicates could in principle be served by scanning both
// index ranges and intersecting the entity ids. At this data size that is
// slower than scanning the more selective one and filtering, because the second
// scan costs real I/O while the filter costs a comparison against a value
// already in memory. Recording the decision here means it can be revisited when
// the data justifies it, rather than being invisible.
//
// SELECTIVITY IS A HEURISTIC, AND IT SAYS SO
//
// With no statistics, "most selective predicate" is guesswork. The planner
// ranks by what the schema knows - a unique_hint property beats a plain indexed
// one, equality beats prefix - and the plan says the ranking was a heuristic
// rather than an estimate. Claiming a cardinality estimate here without
// histograms would be a lie that an interviewer could catch in one question.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bundle.h"
#include "query.h"
#include "schema.h"
#include "sextant/lsm/status.h"

namespace sextant::query {

using lsm::Status;
using ontology::SchemaBundle;

enum class AccessPath {
  kPointLookup,   // ENTITY, key known exactly
  kIndexExact,    // IDX prefix on a fully known string value
  kIndexRange,    // IDX range on an ordered numeric value
  kIndexPrefix,   // IDX prefix on a known leading substring
  kEntityScan,    // ENTITY type prefix, every entity of a type
  kLinkOut,       // LINKOUT prefix
  kLinkIn,        // LINKIN prefix
  kTimeIndex,     // TIDX range, the one worth having
};

const char* AccessPathName(AccessPath path);
// The keyspace an access path reads. Named separately because the frontend
// draws the keyspace, and a reader should be able to check the mapping.
const char* AccessPathKeyspace(AccessPath path);

struct PlanStep {
  int ordinal = 0;
  std::string description;  // "resolve start set", "hop 1: arrivals"

  AccessPath path = AccessPath::kEntityScan;
  std::string reason;

  // Filled for the steps where they mean something.
  std::string type;      // entity type name
  std::string link;      // link type name
  std::string property;  // the indexed property, when one was chosen
  TimeWindow window;

  // Predicates that could not be pushed into the key range and will be
  // evaluated against materialised entities.
  std::vector<Predicate> residuals;

  std::string Render() const;
};

struct Plan {
  std::vector<PlanStep> steps;

  // Problems that did not stop planning: a filter on a property with no index,
  // a hop that will fan out badly. The response carries them so a slow query
  // explains itself.
  std::vector<std::string> warnings;

  // The single headline for the whole query: the most interesting index used,
  // which is what `_stats.index_used` reports.
  AccessPath headline = AccessPath::kEntityScan;

  std::string Render() const;
};

class Planner {
 public:
  explicit Planner(const SchemaBundle* bundle) : bundle_(bundle) {}

  // Fails only on things that make the query meaningless: an unknown type, an
  // unknown link, a hop whose link does not start where the previous one
  // ended. A query that will merely be slow plans successfully with a warning,
  // because refusing to run it would be worse than running it visibly.
  Status Build(const Query& query, Plan* out) const;

 private:
  Status PlanStart(const StartSet& start, PlanStep* step,
                   std::vector<std::string>* warnings) const;
  Status PlanHop(const Hop& hop, int index, const ontology::EntityTypeDef& from,
                 const ontology::EntityTypeDef** to, PlanStep* step,
                 std::vector<std::string>* warnings) const;

  const SchemaBundle* bundle_;
};

}  // namespace sextant::query
