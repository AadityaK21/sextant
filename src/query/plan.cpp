#include "plan.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace sextant::query {
namespace {

using ontology::EntityTypeDef;
using ontology::LinkTypeDef;
using ontology::PropertyDef;
using ontology::ValueType;

// How much better one indexed predicate looks than another, with no statistics
// to go on. Higher wins. The absolute numbers mean nothing; only the order
// does, and the order encodes three claims that are true of this schema:
//
//   a unique_hint property splits the space far harder than a plain indexed one
//   equality is at least as selective as a prefix, never less
//   a numeric range is the weakest of the three and can still be a huge win
//     over a full scan
//
// This is a heuristic, and the plan says so rather than dressing it up as an
// estimate.
int Selectivity(const PropertyDef& prop, CompareOp op) {
  int score = 0;
  switch (op) {
    case CompareOp::kEq:
      score = 100;
      break;
    case CompareOp::kStartsWith:
      score = 60;
      break;
    case CompareOp::kGt:
    case CompareOp::kGte:
    case CompareOp::kLt:
    case CompareOp::kLte:
      score = 30;
      break;
    default:
      return 0;  // kNe and kContains cannot be a key range at all
  }
  if (prop.unique_hint) score += 50;
  if (prop.title) score += 5;
  return score;
}

// Can this predicate become a key range on IDX, rather than a filter?
bool IsIndexable(const PropertyDef& prop, const Predicate& pred) {
  if (!prop.indexed) return false;
  switch (pred.op) {
    case CompareOp::kEq:
      return true;
    case CompareOp::kStartsWith:
      // Only strings have a prefix in the ordered encoding. A "starts with" on
      // a double is not a range over anything.
      return prop.type == ValueType::kString;
    case CompareOp::kLt:
    case CompareOp::kLte:
    case CompareOp::kGt:
    case CompareOp::kGte:
      return prop.type == ValueType::kInt || prop.type == ValueType::kDouble ||
             prop.type == ValueType::kTimestamp;
    default:
      return false;
  }
}

AccessPath PathFor(const PropertyDef& prop, CompareOp op) {
  if (op == CompareOp::kEq) return AccessPath::kIndexExact;
  if (op == CompareOp::kStartsWith) return AccessPath::kIndexPrefix;
  (void)prop;
  return AccessPath::kIndexRange;
}

// Which of two access paths is the more interesting thing to report as THE
// index this query used. TIDX outranks everything because it is the one that
// turns a filter into a range; a scan outranks nothing.
int Rank(AccessPath path) {
  switch (path) {
    case AccessPath::kTimeIndex:
      return 5;
    case AccessPath::kIndexExact:
      return 4;
    case AccessPath::kIndexRange:
    case AccessPath::kIndexPrefix:
      return 3;
    case AccessPath::kPointLookup:
      return 2;
    case AccessPath::kLinkOut:
    case AccessPath::kLinkIn:
      return 1;
    case AccessPath::kEntityScan:
      return 0;
  }
  return 0;
}

}  // namespace

const char* AccessPathName(AccessPath path) {
  switch (path) {
    case AccessPath::kPointLookup:
      return "POINT";
    case AccessPath::kIndexExact:
      return "IDX";
    case AccessPath::kIndexRange:
      return "IDX_RANGE";
    case AccessPath::kIndexPrefix:
      return "IDX_PREFIX";
    case AccessPath::kEntityScan:
      return "SCAN";
    case AccessPath::kLinkOut:
      return "LINKOUT";
    case AccessPath::kLinkIn:
      return "LINKIN";
    case AccessPath::kTimeIndex:
      return "TIDX";
  }
  return "UNKNOWN";
}

const char* AccessPathKeyspace(AccessPath path) {
  switch (path) {
    case AccessPath::kPointLookup:
    case AccessPath::kEntityScan:
      return "ENTITY";
    case AccessPath::kIndexExact:
    case AccessPath::kIndexRange:
    case AccessPath::kIndexPrefix:
      return "IDX";
    case AccessPath::kLinkOut:
      return "LINKOUT";
    case AccessPath::kLinkIn:
      return "LINKIN";
    case AccessPath::kTimeIndex:
      return "TIDX";
  }
  return "?";
}

std::string PlanStep::Render() const {
  std::string out = std::to_string(ordinal) + ". " + description + "\n";
  out += "     via " + std::string(AccessPathName(path)) + " (" +
         AccessPathKeyspace(path) + ")\n";
  out += "     " + reason + "\n";
  if (window.present) out += "     window " + window.Describe() + "\n";
  if (!residuals.empty()) {
    out += "     then filter:";
    for (const auto& pred : residuals) out += " " + pred.Describe();
    out += "\n";
  }
  return out;
}

std::string Plan::Render() const {
  std::string out;
  for (const auto& step : steps) out += step.Render();
  if (!warnings.empty()) {
    out += "  warnings\n";
    for (const auto& warning : warnings) out += "     ! " + warning + "\n";
  }
  return out;
}

// --- planning ---------------------------------------------------------------

Status Planner::PlanStart(const StartSet& start, PlanStep* step,
                          std::vector<std::string>* warnings) const {
  const EntityTypeDef* type = bundle_->ontology().Type(start.type);
  if (type == nullptr) {
    return Status::InvalidArgument("unknown entity type: " + start.type);
  }

  step->ordinal = 1;
  step->type = type->name;
  step->description = "resolve start set of " + type->name;

  // Explicit ids beat everything. Nothing to search for.
  if (!start.ids.empty()) {
    step->path = AccessPath::kPointLookup;
    step->reason = std::to_string(start.ids.size()) +
                   " id(s) given, so each is one ENTITY point lookup";
    step->residuals = start.filter;
    return Status::OK();
  }

  if (start.filter.empty()) {
    step->path = AccessPath::kEntityScan;
    step->reason = "no filter given, so every " + type->name + " is in scope";
    return Status::OK();
  }

  // Pick the single best indexable predicate. Everything else is a residual.
  int best_index = -1;
  int best_score = 0;
  for (size_t i = 0; i < start.filter.size(); ++i) {
    const PropertyDef* prop = type->Property(start.filter[i].property);
    if (prop == nullptr) {
      return Status::InvalidArgument("unknown property " + start.filter[i].property +
                                     " on " + type->name);
    }
    if (!IsIndexable(*prop, start.filter[i])) continue;
    const int score = Selectivity(*prop, start.filter[i].op);
    if (score > best_score) {
      best_score = score;
      best_index = static_cast<int>(i);
    }
  }

  if (best_index < 0) {
    step->path = AccessPath::kEntityScan;
    step->residuals = start.filter;
    // Name the property, because "add an index" is the fix and the reader
    // needs to know which one.
    step->reason = "no filter is servable from IDX (" +
                   start.filter[0].property + " is " +
                   (type->Property(start.filter[0].property)->indexed
                        ? "indexed but the operator cannot be a key range"
                        : "not indexed") +
                   "), so this is a full scan of " + type->name;
    warnings->push_back("full scan of " + type->name + ": no usable index for " +
                        start.filter[0].Describe());
    return Status::OK();
  }

  const Predicate& chosen = start.filter[best_index];
  const PropertyDef* prop = type->Property(chosen.property);
  step->path = PathFor(*prop, chosen.op);
  step->property = chosen.property;
  step->reason = "IDX on " + type->name + "." + chosen.property +
                 " serves " + chosen.Describe() + " as a key range" +
                 (prop->unique_hint ? "; the property is unique-hinted, so this"
                                      " is expected to be one entity"
                                    : "");

  for (size_t i = 0; i < start.filter.size(); ++i) {
    if (static_cast<int>(i) != best_index) step->residuals.push_back(start.filter[i]);
  }
  if (!step->residuals.empty()) {
    step->reason += "; " + std::to_string(step->residuals.size()) +
                    " further predicate(s) applied after materialisation "
                    "(one index range beats intersecting two)";
  }
  return Status::OK();
}

Status Planner::PlanHop(const Hop& hop, int index, const EntityTypeDef& from,
                        const EntityTypeDef** to, PlanStep* step,
                        std::vector<std::string>* warnings) const {
  // Either name resolves. Naming the inverse ("arrivals") is itself a
  // statement of direction, so it implies reverse without a separate flag; an
  // explicit hop.reverse still forces it, for the forward name.
  bool named_inverse = false;
  const LinkTypeDef* link = bundle_->ontology().LinkOrInverse(hop.link, &named_inverse);
  if (link == nullptr) {
    return Status::InvalidArgument("unknown link type: " + hop.link);
  }
  const bool reverse = named_inverse || hop.reverse;

  // Direction determines both which end we must currently be standing on and
  // which keyspace answers the question.
  const ontology::TypeId expected = reverse ? link->to : link->from;
  const ontology::TypeId arriving = reverse ? link->from : link->to;

  if (from.id != expected) {
    const EntityTypeDef* want = bundle_->ontology().Type(expected);
    return Status::InvalidArgument(
        "hop " + std::to_string(index) + " over '" + hop.link + "' starts at " +
        (want != nullptr ? want->name : std::string("?")) + ", but the previous step "
        "produced " + from.name);
  }

  const EntityTypeDef* target = bundle_->ontology().Type(arriving);
  if (target == nullptr) {
    return Status::Corruption("link " + hop.link + " points at an unknown type");
  }
  *to = target;

  step->ordinal = index + 1;
  step->link = link->name;
  step->type = target->name;
  step->window = hop.when;
  step->description = "hop " + std::to_string(index) + ": " + from.name +
                      (reverse ? " <- " : " -> ") + link->name + " -> " +
                      target->name;

  // THE DECISION THIS WHOLE KEYSPACE EXISTS FOR.
  //
  // TIDX is anchored on the TARGET of the timed link - the port that events
  // point at - because "what arrived here in this window" is the question the
  // index was built for. So it only helps walking BACKWARDS along the link,
  // from the anchor to the events. Going forward, from one voyage to its port,
  // there is at most one neighbour and no window to range over anyway.
  const bool tidx_applies = hop.when.present && link->HasTimeIndex() && reverse;

  if (tidx_applies) {
    const PropertyDef* time_prop = bundle_->ontology().Property(link->time_index);
    step->path = AccessPath::kTimeIndex;
    step->property = time_prop != nullptr ? time_prop->name : std::string();
    step->reason =
        "the hop has a time predicate and '" + link->name +
        "' carries a time_index on " + step->property +
        ", so TIDX turns the window into one contiguous range: seek to the "
        "start and read forward, with no candidate examined and rejected";
  } else if (hop.when.present && link->HasTimeIndex()) {
    step->path = AccessPath::kLinkOut;
    step->reason =
        "'" + link->name +
        "' has a time_index, but TIDX is anchored on " + target->name +
        " and this hop walks away from the anchor, so the window is applied "
        "as a filter instead";
  } else if (hop.when.present) {
    step->path = reverse ? AccessPath::kLinkIn : AccessPath::kLinkOut;
    step->reason = "the hop has a time predicate but '" + link->name +
                   "' has no time_index, so every neighbour is read and then "
                   "filtered on time";
    warnings->push_back(
        "time filter on '" + link->name +
        "' cannot use TIDX: the link has no time_index in the ontology");
  } else {
    step->path = reverse ? AccessPath::kLinkIn : AccessPath::kLinkOut;
    step->reason = std::string("no time predicate, so this is a prefix scan of ") +
                   AccessPathKeyspace(step->path) + " per entity in the frontier";
  }

  // Predicates on the far entity are always residual: the adjacency keyspaces
  // store an id and an edge payload, not the target's properties, so there is
  // nothing to push down into.
  for (const auto& pred : hop.filter) {
    if (target->Property(pred.property) == nullptr) {
      return Status::InvalidArgument("unknown property " + pred.property + " on " +
                                     target->name);
    }
    step->residuals.push_back(pred);
  }
  if (!step->residuals.empty()) {
    step->reason +=
        "; predicates on " + target->name +
        " are applied after the entity is read, because the link keyspaces "
        "hold ids rather than properties";
  }

  return Status::OK();
}

Status Planner::Build(const Query& query, Plan* out) const {
  *out = Plan{};

  PlanStep start;
  Status s = PlanStart(query.start, &start, &out->warnings);
  if (!s.ok()) return s;
  out->steps.push_back(start);

  const EntityTypeDef* current = bundle_->ontology().Type(query.start.type);
  for (size_t i = 0; i < query.hops.size(); ++i) {
    PlanStep step;
    const EntityTypeDef* next = nullptr;
    s = PlanHop(query.hops[i], static_cast<int>(i) + 1, *current, &next, &step,
                &out->warnings);
    if (!s.ok()) return s;
    out->steps.push_back(step);
    current = next;
  }

  out->headline = out->steps.empty() ? AccessPath::kEntityScan : out->steps[0].path;
  for (const auto& step : out->steps) {
    if (Rank(step.path) > Rank(out->headline)) out->headline = step.path;
  }
  return Status::OK();
}

}  // namespace sextant::query
