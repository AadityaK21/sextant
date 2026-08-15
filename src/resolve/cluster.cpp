#include "cluster.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sextant::resolve {

void UnionFind::Add(const RecordRef& ref) {
  if (parent_.find(ref) == parent_.end()) {
    parent_[ref] = ref;
    rank_[ref] = 0;
  }
}

RecordRef UnionFind::Find(const RecordRef& ref) {
  auto it = parent_.find(ref);
  if (it == parent_.end()) {
    Add(ref);
    return ref;
  }
  // Path compression, iterative. A recursive version is prettier and blows the
  // stack on a long chain, which is exactly the shape a bad over-merge makes.
  RecordRef root = ref;
  while (!(parent_[root] == root)) root = parent_[root];
  RecordRef walk = ref;
  while (!(parent_[walk] == walk)) {
    const RecordRef next = parent_[walk];
    parent_[walk] = root;
    walk = next;
  }
  return root;
}

bool UnionFind::Union(const RecordRef& a, const RecordRef& b) {
  const RecordRef ra = Find(a);
  const RecordRef rb = Find(b);
  if (ra == rb) return false;

  if (rank_[ra] < rank_[rb]) {
    parent_[ra] = rb;
  } else if (rank_[rb] < rank_[ra]) {
    parent_[rb] = ra;
  } else {
    parent_[rb] = ra;
    ++rank_[ra];
  }
  return true;
}

std::vector<std::vector<RecordRef>> UnionFind::Clusters() {
  std::map<RecordRef, std::vector<RecordRef>> grouped;
  for (const auto& [ref, _] : parent_) {
    grouped[Find(ref)].push_back(ref);
  }
  std::vector<std::vector<RecordRef>> out;
  out.reserve(grouped.size());
  for (auto& [root, members] : grouped) {
    std::sort(members.begin(), members.end());
    out.push_back(std::move(members));
  }
  // Deterministic order, so two runs produce the same output and a diff between
  // them means something.
  std::sort(out.begin(), out.end(),
            [](const std::vector<RecordRef>& a, const std::vector<RecordRef>& b) {
              return a.front() < b.front();
            });
  return out;
}

size_t ClusterSet::largest() const {
  size_t max = 0;
  for (const auto& cluster : clusters) max = std::max(max, cluster.size());
  return max;
}

size_t ClusterSet::singletons() const {
  size_t n = 0;
  for (const auto& cluster : clusters) {
    if (cluster.size() == 1) ++n;
  }
  return n;
}

namespace {

// Every record mentioned by any edge, so records that matched nothing still
// become singleton clusters rather than disappearing.
void SeedMembers(const std::vector<ScoredEdge>& edges, UnionFind* uf) {
  for (const auto& edge : edges) {
    uf->Add(edge.pair.a);
    uf->Add(edge.pair.b);
  }
}

}  // namespace

ClusterSet ClusterTransitive(const std::vector<ScoredEdge>& edges) {
  UnionFind uf;
  SeedMembers(edges, &uf);
  for (const auto& edge : edges) {
    if (edge.decision != Decision::kMatch) continue;
    uf.Union(edge.pair.a, edge.pair.b);
  }
  ClusterSet out;
  out.clusters = uf.Clusters();
  return out;
}

ClusterSet ClusterVetoConstrained(const std::vector<ScoredEdge>& edges) {
  UnionFind uf;
  SeedMembers(edges, &uf);

  // Every pair the scorer refused outright. A veto is not only a statement
  // about those two records - it constrains the whole chain, because after any
  // sequence of merges these two must still be in different clusters.
  std::unordered_set<PairRef, PairRefHash> vetoed;
  for (const auto& edge : edges) {
    if (edge.vetoed) vetoed.insert(edge.pair);
  }

  // Descending score. Highest confidence first, so the merges most likely to be
  // right are made while there is still room to make them - once two clusters
  // are joined, every veto spanning them blocks nothing, because the damage is
  // already done.
  std::vector<const ScoredEdge*> matches;
  for (const auto& edge : edges) {
    if (edge.decision == Decision::kMatch) matches.push_back(&edge);
  }
  std::sort(matches.begin(), matches.end(),
            [](const ScoredEdge* a, const ScoredEdge* b) {
              if (a->score != b->score) return a->score > b->score;
              return a->pair < b->pair;  // stable, for determinism
            });

  // Members of each root, maintained so a proposed merge can be checked against
  // every veto spanning the two clusters rather than only the edge's endpoints.
  std::unordered_map<RecordRef, std::vector<RecordRef>, RecordRefHash> members;
  for (const auto& cluster : uf.Clusters()) {
    members[cluster.front()] = cluster;
  }

  ClusterSet out;
  for (const ScoredEdge* edge : matches) {
    const RecordRef ra = uf.Find(edge->pair.a);
    const RecordRef rb = uf.Find(edge->pair.b);
    if (ra == rb) continue;

    // Would this merge put two records that must not share a cluster together?
    std::string reason;
    for (const RecordRef& x : members[ra]) {
      for (const RecordRef& y : members[rb]) {
        const PairRef candidate(x, y);
        if (vetoed.count(candidate) != 0) {
          reason = "merging these clusters would join a vetoed pair";
          break;
        }
      }
      if (!reason.empty()) break;
    }
    if (!reason.empty()) {
      out.refused.emplace_back(edge->pair, reason);
      continue;
    }

    uf.Union(edge->pair.a, edge->pair.b);
    const RecordRef root = uf.Find(edge->pair.a);
    std::vector<RecordRef> merged = members[ra];
    merged.insert(merged.end(), members[rb].begin(), members[rb].end());
    members.erase(ra);
    members.erase(rb);
    members[root] = std::move(merged);
  }

  out.clusters = uf.Clusters();
  return out;
}

// --- measurement ------------------------------------------------------------

double ClusterMetrics::precision() const {
  const uint64_t predicted = true_positive + false_positive;
  return predicted == 0 ? 0.0
                        : static_cast<double>(true_positive) /
                              static_cast<double>(predicted);
}

double ClusterMetrics::recall() const {
  const uint64_t actual = true_positive + false_negative;
  return actual == 0 ? 0.0
                     : static_cast<double>(true_positive) /
                           static_cast<double>(actual);
}

double ClusterMetrics::f1() const {
  const double p = precision();
  const double r = recall();
  return (p + r) == 0.0 ? 0.0 : 2.0 * p * r / (p + r);
}

ClusterMetrics MeasureClusters(const ClusterSet& clusters,
                               const GoldenSet& golden) {
  ClusterMetrics metrics;
  metrics.clusters = clusters.clusters.size();
  metrics.largest = clusters.largest();
  metrics.singletons = clusters.singletons();
  metrics.refused_merges = clusters.refused.size();

  // The pairs the CLUSTERING implies, not the edges the scorer decided. This is
  // the distinction that makes cluster metrics worth computing: an over-merge
  // arriving through a chain of individually reasonable decisions shows up here
  // and is invisible in pairwise F1.
  std::unordered_set<PairRef, PairRefHash> implied;
  for (const auto& cluster : clusters.clusters) {
    for (size_t i = 0; i < cluster.size(); ++i) {
      for (size_t j = i + 1; j < cluster.size(); ++j) {
        implied.insert(PairRef(cluster[i], cluster[j]));
      }
    }
  }
  metrics.implied_pairs = implied.size();

  // Scored only against labeled pairs. The golden set covers a sample, so an
  // implied pair with no label is neither credited nor penalised.
  for (const auto& labeled : golden.pairs()) {
    const bool together = implied.count(labeled.pair) != 0;
    if (labeled.is_match && together) {
      ++metrics.true_positive;
    } else if (!labeled.is_match && together) {
      ++metrics.false_positive;
    } else if (labeled.is_match && !together) {
      ++metrics.false_negative;
    }
  }
  return metrics;
}

}  // namespace sextant::resolve
