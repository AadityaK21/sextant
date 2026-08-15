// The executor: run a plan, and account for what it cost.
//
// FRONTIER EXPANSION, NOT RECURSION
//
// A hop takes a set of entities and produces the set they reach. Doing that
// breadth-first, one hop at a time, rather than depth-first per entity, is what
// makes deduplication possible: two ports can both link to the same vessel, and
// a depth-first walk would visit it twice, materialise it twice and return it
// twice. The frontier is a set, so the vessel is read once.
//
// It also bounds memory in terms of the widest layer rather than the deepest
// path, which for a graph like this is the smaller of the two.
//
// EVERY SCAN IS BOUNDED, AND SAYS WHEN IT STOPPED
//
// A hub port in this corpus has thousands of arrivals. Two hops from one is
// enough to touch most of the graph. So each hop carries max_expand, and when
// it trips the executor sets `truncated` and records which step did it. A
// result that quietly dropped rows is worse than one that says it dropped them:
// the first looks like a data problem and the second looks like what it is.
//
// COST IS MEASURED, NOT ESTIMATED
//
// keys_scanned, blocks_read, bloom_rejections and the rest come from a
// ReadStats the request owns, threaded down into the storage engine (see
// lsm::ReadOptions::stats). Nothing here counts anything by hand. That matters
// because the number's whole purpose is to be checkable: the quarter query
// claims 24 keys scanned for 24 arrivals, and if the executor were doing its
// own arithmetic that claim would be worth nothing.
//
// SNAPSHOT AT THE TOP, RELEASED AT THE BOTTOM
//
// The whole traversal reads at one sequence number. Without that, an ingest
// running concurrently could show an entity at hop 1 that has been merged away
// by the time hop 2 asks for it - which produces a wrong answer and no error.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bundle.h"
#include "fuse.h"
#include "plan.h"
#include "query.h"
#include "sextant/lsm/options.h"
#include "sextant/lsm/status.h"
#include "store.h"

namespace sextant::query {

using lsm::Status;
using ontology::SchemaBundle;

// What a query cost. Everything except elapsed_us and entities_materialised
// comes straight from the engine.
struct QueryCost {
  uint64_t keys_scanned = 0;
  uint64_t blocks_read = 0;
  uint64_t block_cache_hits = 0;
  uint64_t bloom_rejections = 0;
  uint64_t range_rejections = 0;
  uint64_t sstables_probed = 0;
  uint64_t memtable_hits = 0;

  // Entities decoded into memory. Distinct from keys_scanned: a TIDX scan
  // touches one key per hit and then materialises each one, so a wide gap
  // between these two is the sign of an index doing its job.
  uint64_t entities_materialised = 0;

  uint64_t elapsed_us = 0;
  AccessPath index_used = AccessPath::kEntityScan;

  bool truncated = false;
  std::string truncated_at;  // which step hit its bound
};

// One entity in a result, already materialised.
struct ResultEntity {
  codec::Ulid id;
  codec::TypeId type = 0;
  std::string type_name;
  std::string display;
  std::vector<std::pair<std::string, ontology::TValue>> properties;

  // Which hop produced it. 0 is the start set.
  int depth = 0;
};

// An edge actually traversed. Returned so the frontend can draw the graph it
// walked rather than guessing the edges back from the node list.
struct ResultEdge {
  codec::Ulid from;
  codec::Ulid to;
  std::string link;
  bool reverse = false;
};

struct QueryResult {
  Plan plan;
  QueryCost cost;

  std::vector<ResultEntity> entities;  // the final hop, or every hop if asked
  std::vector<ResultEdge> edges;

  uint64_t total_before_limit = 0;
};

class Executor {
 public:
  Executor(codec::Store* store, const SchemaBundle* bundle)
      : store_(store), bundle_(bundle) {}

  Status Run(const Query& query, QueryResult* out) const;

  // Read one entity and turn it into a result row. Exposed because the single
  // entity route needs exactly this and nothing else.
  Status Materialise(codec::TypeId type, const codec::Ulid& id,
                     const codec::ReadContext& ctx, ResultEntity* out) const;

 private:
  // The set of entities a step is standing on.
  struct Frontier {
    codec::TypeId type = 0;
    std::vector<codec::Ulid> ids;
  };

  Status ResolveStart(const Query& query, const PlanStep& step,
                      const codec::ReadContext& ctx, Frontier* out,
                      QueryCost* cost) const;

  Status Expand(const Hop& hop, const PlanStep& step, const Frontier& in,
                const codec::ReadContext& ctx, Frontier* out,
                std::vector<ResultEdge>* edges, QueryCost* cost) const;

  codec::Store* store_;
  const SchemaBundle* bundle_;
};

}  // namespace sextant::query
