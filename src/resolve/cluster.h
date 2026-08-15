// Clustering: turning pairwise decisions into entities.
//
// THE TRAP THIS FILE EXISTS TO DEMONSTRATE
//
// The obvious algorithm is union-find over the MATCH edges: if A matches B and
// B matches C, then A, B and C are one entity. It is one page of code, it is
// what everyone writes first, and it has a failure mode that gets worse the
// better your scorer is.
//
// Matching is not transitive. A and B can be similar, B and C can be similar,
// and A and C can be obviously different - "Rotterdam" and "Rotterdam Botlek"
// and "Botlek Terminal 3" walk a chain from a port to a berth. Plain union-find
// merges the whole chain, and one over-merge does not produce one wrong entity:
// it welds two clusters together permanently, and every record in both is now
// wrong.
//
// Worse, the damage is invisible in pairwise metrics. Precision and recall are
// computed over PAIRS, and a chain of three good pairwise decisions can produce
// a cluster that is entirely wrong as an entity. This is why the pairwise F1 in
// docs/ER.md is not the last word, and why cluster-level numbers are reported
// alongside it.
//
// THE FIX: VETO EDGES
//
// The scorer already knows about pairs that CANNOT be the same - two vessels
// with different IMO numbers, two ports with different UN/LOCODEs. Those are
// vetoes, and a veto between A and C is information about the whole chain:
// whatever the intermediate similarities say, A and C must not end up in one
// cluster.
//
// So the constrained algorithm processes match edges in DESCENDING SCORE ORDER
// and refuses any merge that would put two vetoed records together. Highest
// confidence first means the merges most likely to be right are made before
// the budget of possible merges is spent.
//
// Both are implemented, both are measured, and the comparison is in
// docs/ER.md. That table is the most informative artifact in the project,
// because it is a number attached to a design decision rather than an opinion
// about one.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "golden.h"
#include "record_ref.h"
#include "scorer.h"

namespace sextant::resolve {

// Union-find with path compression and union by rank. Exposed because the
// tests want to check it directly rather than only through a clusterer.
class UnionFind {
 public:
  void Add(const RecordRef& ref);
  RecordRef Find(const RecordRef& ref);
  // False if the two were already together.
  bool Union(const RecordRef& a, const RecordRef& b);

  std::vector<std::vector<RecordRef>> Clusters();
  size_t size() const { return parent_.size(); }

 private:
  std::unordered_map<RecordRef, RecordRef, RecordRefHash> parent_;
  std::unordered_map<RecordRef, int, RecordRefHash> rank_;
};

// One decided pair, the input to clustering.
struct ScoredEdge {
  PairRef pair;
  double score = 0.0;
  Decision decision = Decision::kNonMatch;
  bool vetoed = false;
  std::string veto_reason;
};

struct ClusterSet {
  std::vector<std::vector<RecordRef>> clusters;
  // Merges the veto constraint refused, with the reason. Empty for plain
  // union-find, and the difference between the two algorithms in one number.
  std::vector<std::pair<PairRef, std::string>> refused;

  size_t largest() const;
  size_t singletons() const;
};

// A matches B, B matches C, so A, B and C are one entity. Fast, simple, and
// wrong in a way that compounds.
ClusterSet ClusterTransitive(const std::vector<ScoredEdge>& edges);

// The same, in descending score order, refusing any merge that would place two
// vetoed records in one cluster.
ClusterSet ClusterVetoConstrained(const std::vector<ScoredEdge>& edges);

// Cluster-level quality against the golden set.
//
// Pairwise F1 cannot see an over-merge that arrives through a chain, so these
// are computed over the pairs IMPLIED by the clustering - every pair of records
// sharing a cluster - rather than over the decided edges.
struct ClusterMetrics {
  uint64_t clusters = 0;
  uint64_t largest = 0;
  uint64_t singletons = 0;
  uint64_t refused_merges = 0;

  uint64_t implied_pairs = 0;
  uint64_t true_positive = 0;
  uint64_t false_positive = 0;
  uint64_t false_negative = 0;

  double precision() const;
  double recall() const;
  double f1() const;
};

ClusterMetrics MeasureClusters(const ClusterSet& clusters,
                               const GoldenSet& golden);

}  // namespace sextant::resolve
