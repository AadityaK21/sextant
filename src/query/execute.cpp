#include "execute.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "keyspace.h"

namespace sextant::query {
namespace {

using codec::Ulid;
using ontology::EntityTypeDef;
using ontology::PropertyDef;
using ontology::TValue;
using ontology::ValueType;

uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Deduplicate while preserving the order entities were discovered in.
//
// Order matters more than it looks: the frontier is built from a TIDX scan,
// which is already in time order, and a std::set would throw that away and
// return arrivals sorted by entity id. Sorting to deduplicate and then having
// to sort back is worse than paying for one lookup set alongside the vector.
class OrderedIdSet {
 public:
  bool Add(const Ulid& id) {
    if (!seen_.insert(id).second) return false;
    order_.push_back(id);
    return true;
  }
  const std::vector<Ulid>& ids() const { return order_; }
  size_t size() const { return order_.size(); }

 private:
  std::set<Ulid> seen_;
  std::vector<Ulid> order_;
};

}  // namespace

Status Executor::Materialise(codec::TypeId type, const Ulid& id,
                             const codec::ReadContext& ctx, ResultEntity* out) const {
  std::string payload;
  Status s = store_->GetEntity(type, id, &payload, ctx);
  if (!s.ok()) return s;

  resolve::ResolvedEntity entity;
  lsm::Slice input(payload);
  if (!resolve::ResolvedEntity::DecodeFrom(&input, &entity)) {
    return Status::Corruption("entity " + id.ToString() + " did not decode");
  }

  const EntityTypeDef* def = bundle_->ontology().Type(type);
  out->id = id;
  out->type = type;
  out->type_name = def != nullptr ? def->name : std::string();

  std::vector<std::pair<codec::PropId, std::string>> display_values;
  for (const auto& [prop_id, value] : entity.properties) {
    const PropertyDef* prop = bundle_->ontology().Property(prop_id);
    if (prop == nullptr) continue;  // a property the schema no longer declares
    out->properties.emplace_back(prop->name, value);
    display_values.emplace_back(prop_id, value.ToDisplay());
  }
  if (def != nullptr) {
    out->display = bundle_->ontology().RenderDisplay(*def, display_values);
  }
  return Status::OK();
}

Status Executor::ResolveStart(const Query& query, const PlanStep& step,
                              const codec::ReadContext& ctx, Frontier* out,
                              QueryCost* cost) const {
  const EntityTypeDef* type = bundle_->ontology().Type(query.start.type);
  if (type == nullptr) return Status::InvalidArgument("unknown type " + query.start.type);
  out->type = type->id;

  OrderedIdSet found;

  switch (step.path) {
    case AccessPath::kPointLookup: {
      for (const auto& text : query.start.ids) {
        Ulid id;
        if (!Ulid::FromString(text, &id)) {
          return Status::InvalidArgument("not a valid entity id: " + text);
        }
        found.Add(id);
      }
      break;
    }

    case AccessPath::kIndexExact:
    case AccessPath::kIndexPrefix: {
      const PropertyDef* prop = type->Property(step.property);
      const Predicate* chosen = nullptr;
      for (const auto& pred : query.start.filter) {
        if (pred.property == step.property) chosen = &pred;
      }
      if (prop == nullptr || chosen == nullptr) {
        return Status::Corruption("planner chose an index the query does not use");
      }
      // Both are a bounded scan of IDX. Exact match is the case where the
      // bound includes the value's terminator, so it matches that value and
      // nothing longer; a prefix search drops the terminator so that every
      // longer value starting the same way falls inside the range.
      const std::string& needle = chosen->value.type() == ValueType::kString
                                      ? chosen->value.AsString()
                                      : chosen->value.ToDisplay();
      auto iter = step.path == AccessPath::kIndexExact
                      ? store_->LookupString(type->id, prop->id,
                                             lsm::Slice(needle), ctx)
                      : store_->PrefixString(type->id, prop->id,
                                             lsm::Slice(needle), ctx);
      for (; iter->Valid(); iter->Next()) {
        Ulid id;
        if (codec::DecodeIndexKeyEntity(iter->key(), &id)) found.Add(id);
      }
      if (!iter->status().ok()) return iter->status();
      break;
    }

    case AccessPath::kIndexRange: {
      const PropertyDef* prop = type->Property(step.property);
      if (prop == nullptr) return Status::Corruption("indexed property vanished");

      double lower = -1e308, upper = 1e308;
      for (const auto& pred : query.start.filter) {
        if (pred.property != step.property) continue;
        const double v = pred.value.type() == ValueType::kDouble
                             ? pred.value.AsDouble()
                             : static_cast<double>(pred.value.type() ==
                                                           ValueType::kTimestamp
                                                       ? pred.value.AsTimestamp()
                                                       : pred.value.AsInt());
        if (pred.op == CompareOp::kGt || pred.op == CompareOp::kGte) lower = v;
        if (pred.op == CompareOp::kLt || pred.op == CompareOp::kLte) upper = v;
      }

      auto iter = prop->type == ValueType::kDouble
                      ? store_->RangeDouble(type->id, prop->id, lower, upper, ctx)
                      : store_->RangeInt(type->id, prop->id,
                                         static_cast<int64_t>(lower),
                                         static_cast<int64_t>(upper), ctx);
      for (; iter->Valid(); iter->Next()) {
        Ulid id;
        if (codec::DecodeIndexKeyEntity(iter->key(), &id)) found.Add(id);
      }
      if (!iter->status().ok()) return iter->status();
      break;
    }

    case AccessPath::kEntityScan: {
      auto iter = store_->ScanEntities(type->id, ctx);
      for (; iter->Valid(); iter->Next()) {
        codec::TypeId decoded_type = 0;
        Ulid id;
        if (codec::DecodeEntityKey(iter->key(), &decoded_type, &id)) found.Add(id);
      }
      if (!iter->status().ok()) return iter->status();
      break;
    }

    default:
      return Status::Corruption("planner produced a start path the executor "
                                "cannot run");
  }

  out->ids = found.ids();
  (void)cost;
  return Status::OK();
}

Status Executor::Expand(const Hop& hop, const PlanStep& step, const Frontier& in,
                        const codec::ReadContext& ctx, Frontier* out,
                        std::vector<ResultEdge>* edges, QueryCost* cost) const {
  bool named_inverse = false;
  const ontology::LinkTypeDef* link =
      bundle_->ontology().LinkOrInverse(hop.link, &named_inverse);
  if (link == nullptr) return Status::InvalidArgument("unknown link " + hop.link);

  // The direction comes from the PLAN, not from the hop. The planner has
  // already reconciled an explicit reverse flag with a hop that named the
  // link's inverse, and having the executor redo that reasoning is how the two
  // drift apart and the plan starts describing something else.
  const bool reverse = step.path == AccessPath::kLinkIn;
  const codec::TypeId arriving = (named_inverse || hop.reverse) ? link->from : link->to;
  out->type = arriving;

  OrderedIdSet reached;
  uint64_t expanded = 0;

  for (const Ulid& anchor : in.ids) {
    if (expanded >= hop.max_expand) {
      cost->truncated = true;
      cost->truncated_at = step.description;
      break;
    }

    std::unique_ptr<codec::RangeIterator> iter;
    if (step.path == AccessPath::kTimeIndex) {
      // TIDX is anchored on the target of the timed link, which is the entity
      // the events point AT. That is the port in "arrivals at Rotterdam", and
      // it is why this is a seek rather than a scan.
      iter = store_->ScanTimeRange(link->id, anchor, hop.when.from_inclusive,
                                   hop.when.to_exclusive, ctx);
    } else if (reverse) {
      iter = store_->ScanIncoming(anchor, link->id, ctx);
    } else {
      iter = store_->ScanOutgoing(anchor, link->id, ctx);
    }

    for (; iter->Valid(); iter->Next()) {
      Ulid neighbour;
      bool decoded = false;

      if (step.path == AccessPath::kTimeIndex) {
        codec::LinkTypeId decoded_link = 0;
        Ulid decoded_anchor;
        int64_t ts = 0;
        decoded = codec::DecodeTimeIndexKey(iter->key(), &decoded_link,
                                            &decoded_anchor, &ts, &neighbour);
      } else if (reverse) {
        Ulid dst;
        codec::LinkTypeId decoded_link = 0;
        decoded = codec::DecodeLinkInKey(iter->key(), &dst, &decoded_link, &neighbour);
      } else {
        Ulid src;
        codec::LinkTypeId decoded_link = 0;
        decoded = codec::DecodeLinkOutKey(iter->key(), &src, &decoded_link, &neighbour);
      }
      if (!decoded) continue;

      if (reached.Add(neighbour)) ++expanded;
      edges->push_back(ResultEdge{anchor, neighbour, link->name, reverse});

      if (expanded >= hop.max_expand) {
        cost->truncated = true;
        cost->truncated_at = step.description;
        break;
      }
    }
    if (!iter->status().ok()) return iter->status();
  }

  out->ids = reached.ids();
  return Status::OK();
}

Status Executor::Run(const Query& query, QueryResult* out) const {
  const uint64_t started = NowMicros();

  Planner planner(bundle_);
  Status s = planner.Build(query, &out->plan);
  if (!s.ok()) return s;

  // One snapshot and one stats sink for the entire traversal. See the header:
  // both are about the request, not about the process.
  codec::SnapshotHandle snapshot = store_->NewSnapshot();
  lsm::ReadStats stats;
  codec::ReadContext ctx;
  ctx.snapshot = snapshot.get();
  ctx.stats = &stats;

  Frontier frontier;
  s = ResolveStart(query, out->plan.steps[0], ctx, &frontier, &out->cost);
  if (!s.ok()) return s;

  // Residual predicates on the start set need the entity in memory, so this is
  // where a start-set filter that no index could serve gets paid for.
  const std::vector<Predicate>& start_residuals = out->plan.steps[0].residuals;
  std::vector<ResultEntity> materialised_start;
  if (!start_residuals.empty() || query.hops.empty() || query.include_path) {
    std::vector<Ulid> kept;
    for (const Ulid& id : frontier.ids) {
      ResultEntity entity;
      Status get = Materialise(frontier.type, id, ctx, &entity);
      if (get.IsNotFound()) continue;  // an index entry outliving its entity
      if (!get.ok()) return get;
      ++out->cost.entities_materialised;

      bool keep = true;
      for (const auto& pred : start_residuals) {
        const TValue* value = nullptr;
        for (const auto& [name, v] : entity.properties) {
          if (name == pred.property) value = &v;
        }
        if (!pred.Matches(value)) {
          keep = false;
          break;
        }
      }
      if (!keep) continue;
      kept.push_back(id);
      entity.depth = 0;
      materialised_start.push_back(std::move(entity));
    }
    frontier.ids = std::move(kept);
  }

  if (query.include_path) {
    for (auto& entity : materialised_start) out->entities.push_back(std::move(entity));
  }

  // Walk the hops.
  for (size_t i = 0; i < query.hops.size(); ++i) {
    const PlanStep& step = out->plan.steps[i + 1];
    Frontier next;
    s = Expand(query.hops[i], step, frontier, ctx, &next, &out->edges, &out->cost);
    if (!s.ok()) return s;

    // A window the planner could not turn into a key range still has to be
    // ENFORCED. Choosing not to use TIDX is a performance decision; dropping
    // the predicate would be a wrong answer. This is the line that keeps the
    // two apart, and the test that compares the indexed and unindexed paths
    // for equal results is what holds it honest.
    const Hop& hop = query.hops[i];
    const ontology::LinkTypeDef* link =
        bundle_->ontology().LinkOrInverse(hop.link, nullptr);
    const bool filter_on_time = hop.when.present &&
                                step.path != AccessPath::kTimeIndex &&
                                link != nullptr && link->HasTimeIndex();
    const ontology::PropertyDef* time_prop =
        filter_on_time ? bundle_->ontology().Property(link->time_index) : nullptr;

    const bool last = (i + 1 == query.hops.size());
    const bool need_entities =
        last || query.include_path || !step.residuals.empty() || filter_on_time;

    if (need_entities) {
      std::vector<Ulid> kept;
      std::vector<ResultEntity> rows;
      for (const Ulid& id : next.ids) {
        ResultEntity entity;
        Status get = Materialise(next.type, id, ctx, &entity);
        if (get.IsNotFound()) continue;
        if (!get.ok()) return get;
        ++out->cost.entities_materialised;

        bool keep = true;
        if (time_prop != nullptr) {
          const TValue* value = nullptr;
          for (const auto& [name, v] : entity.properties) {
            if (name == time_prop->name) value = &v;
          }
          keep = value != nullptr && !value->IsNull() &&
                 hop.when.Contains(value->AsTimestamp());
        }
        for (const auto& pred : step.residuals) {
          if (!keep) break;
          const TValue* value = nullptr;
          for (const auto& [name, v] : entity.properties) {
            if (name == pred.property) value = &v;
          }
          if (!pred.Matches(value)) {
            keep = false;
            break;
          }
        }
        if (!keep) continue;

        entity.depth = static_cast<int>(i) + 1;
        kept.push_back(id);
        rows.push_back(std::move(entity));
      }
      next.ids = std::move(kept);

      if (last || query.include_path) {
        for (auto& row : rows) out->entities.push_back(std::move(row));
      }
    }
    frontier = std::move(next);
  }

  // The start set IS the result when there are no hops.
  if (query.hops.empty() && !query.include_path) {
    out->entities = std::move(materialised_start);
  }

  out->total_before_limit = out->entities.size();
  if (out->entities.size() > query.limit) {
    out->entities.resize(query.limit);
    // Not the same flag as a hop bound. This one means "there is more of the
    // answer", the other means "the traversal stopped early and the answer may
    // be incomplete in ways the limit does not describe".
  }

  // Drop edges that no longer connect anything returned, so the frontend never
  // draws an edge to a node it was not given.
  if (!out->edges.empty()) {
    std::set<Ulid> present;
    for (const auto& entity : out->entities) present.insert(entity.id);
    std::vector<ResultEdge> kept;
    for (const auto& edge : out->edges) {
      if (present.count(edge.to) > 0 || present.count(edge.from) > 0) {
        kept.push_back(edge);
      }
    }
    out->edges = std::move(kept);
  }

  out->cost.keys_scanned = stats.keys_scanned;
  out->cost.blocks_read = stats.blocks_read;
  out->cost.block_cache_hits = stats.block_cache_hits;
  out->cost.bloom_rejections = stats.bloom_rejections;
  out->cost.range_rejections = stats.range_rejections;
  out->cost.sstables_probed = stats.sstables_probed;
  out->cost.memtable_hits = stats.memtable_hits;
  out->cost.index_used = out->plan.headline;
  out->cost.elapsed_us = NowMicros() - started;
  return Status::OK();
}

}  // namespace sextant::query
