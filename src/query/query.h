// The query IR: what to ask, separate from how to answer it.
//
// WHY THERE IS AN IR AT ALL, RATHER THAN A FUNCTION PER QUESTION
//
// The obvious way to serve "arrivals at Rotterdam last quarter" is a function
// called ArrivalsAtPort(port, from, to) that does the scan. Write six of those
// and the access-path decisions are scattered across six functions, none of
// which can tell you why it chose what it chose. The moment a seventh question
// arrives you write a seventh function.
//
// Splitting the request from the plan buys three things this project needs:
//
//   1. The planner becomes one place where index selection lives, so "why did
//      this use TIDX" has an answer that is not "read the source".
//   2. The plan is a value, so it can be returned to the client and rendered.
//      The frontend shows the plan next to the results.
//   3. A new question is a new Query, not new code.
//
// This is not a SQL engine and does not want to be. There is no join reordering
// and no cardinality estimation, because the shape here is fixed: resolve a
// start set, then walk hops. What it does have is honest accounting of what
// each step cost, which is the part that is usually missing.
//
// TIME PREDICATES ARE FIRST CLASS, NOT JUST ANOTHER FILTER
//
// A `where` on a hop over a link that carries a time_index is the difference
// between a seek plus a sequential read and a full prefix scan with a filter.
// So the IR distinguishes a time predicate from a value predicate rather than
// treating both as generic filters, because the planner has to see it to use
// it. Everything else really is just a filter.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sextant/lsm/status.h"
#include "value.h"

namespace sextant::query {

using lsm::Status;
using ontology::TValue;

// --- predicates -------------------------------------------------------------

enum class CompareOp {
  kEq,
  kNe,
  kLt,
  kLte,
  kGt,
  kGte,
  kContains,    // case-insensitive substring, for the search box
  kStartsWith,  // the one that a string index can actually accelerate
};

const char* CompareOpName(CompareOp op);
bool ParseCompareOp(const std::string& name, CompareOp* out);

struct Predicate {
  std::string property;
  CompareOp op = CompareOp::kEq;
  TValue value;

  // Evaluate against an entity's value for `property`. A missing property is
  // false for every operator including kNe, because "not equal" over a value
  // that does not exist is a claim the data does not support.
  bool Matches(const TValue* actual) const;

  std::string Describe() const;
};

// A closed-open time window. Half-open on purpose: quarters and months tile
// exactly, so [Apr 1, Jul 1) and [Jul 1, Oct 1) share no instant and leave no
// gap. Inclusive upper bounds are where off-by-one-day bugs live.
struct TimeWindow {
  bool present = false;
  int64_t from_inclusive = 0;
  int64_t to_exclusive = 0;

  bool Contains(int64_t ts) const {
    return !present || (ts >= from_inclusive && ts < to_exclusive);
  }
  std::string Describe() const;
};

// --- the query --------------------------------------------------------------

// How the traversal starts. Exactly one of these three forms.
struct StartSet {
  std::string type;  // required: the entity type name

  // Explicit ids. Used by the frontend when the user has already clicked
  // something, and by /api/entities/{id}/links.
  std::vector<std::string> ids;

  // Filters over the start type. The planner picks at most one of these to
  // serve from IDX and applies the rest as residuals.
  std::vector<Predicate> filter;
};

struct Hop {
  std::string link;  // link type name

  // Which way to walk it. A link is stored in both directions, so this is a
  // choice of keyspace rather than a difference in cost.
  bool reverse = false;

  TimeWindow when;                  // on the link's time_index property
  std::vector<Predicate> filter;    // on the entity at the far end
  uint64_t max_expand = 100000;     // safety valve, see the executor
};

struct Query {
  StartSet start;
  std::vector<Hop> hops;

  // "Vessel.name", "Voyage.arrived_at". Empty means every property of every
  // entity in the result.
  std::vector<std::string> select;

  uint64_t limit = 200;

  // Return the entities touched at every hop rather than only the last. The
  // graph view needs the intermediate nodes to draw edges at all.
  bool include_path = false;

  static Status FromJson(const std::string& json, Query* out);
  std::string Describe() const;
};

}  // namespace sextant::query
