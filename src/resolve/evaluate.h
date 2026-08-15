// Measuring the resolver, and tuning it without lying to yourself.
//
// THE SPLIT IS THE WHOLE POINT
//
// Weights fitted to a set of labels will score well on those labels. That is
// what fitting means, and it says nothing about whether the resolver works.
// So the golden set is split deterministically into 80% training and 20%
// held-out, the tuner only ever sees the training half, and the number quoted
// in the README comes from the half it never saw.
//
// The split is by hash of the pair rather than by shuffling with a seed, for a
// specific reason: it is stable when the golden set GROWS. Adding a hundred
// new labels moves a hundred pairs into one split or the other and leaves
// every existing pair where it was, so today's held-out number stays comparable
// with last week's. A seeded shuffle reassigns everything and quietly makes the
// comparison meaningless.
//
// WHAT COUNTS AS A POSITIVE
//
// Only a MATCH decision. A pair parked in the REVIEW band counts as a
// non-match for the metric, so sending a hard pair to review costs recall
// rather than hiding it. That is the honest accounting: a pair a human has not
// looked at yet is not a resolved pair, and a resolver that pushes everything
// difficult into review should not be rewarded for it.
//
// THE STOPPING RULE
//
// The execution plan calls weight tuning "the single most seductive time sink
// in the project", and it is right. Coordinate ascent here stops at a fixed
// iteration count or when a full pass improves training F1 by less than a
// threshold. It is deliberately not a good optimiser - it is a good enough one
// that terminates.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "golden.h"
#include "record_ref.h"
#include "scorer.h"

namespace sextant::resolve {

enum class Split { kTrain, kHoldout };

// Deterministic, and stable as the golden set grows.
Split SplitFor(const PairRef& pair, int holdout_percent = 20);

struct ConfusionMatrix {
  uint64_t true_positive = 0;
  uint64_t false_positive = 0;
  uint64_t true_negative = 0;
  uint64_t false_negative = 0;

  // How many of the false negatives were parked in review rather than rejected
  // outright. Worth separating: a pair a human would have confirmed is a very
  // different failure from one the scorer confidently got wrong.
  uint64_t missed_to_review = 0;

  uint64_t total() const {
    return true_positive + false_positive + true_negative + false_negative;
  }
  double precision() const;
  double recall() const;
  double f1() const;
  double accuracy() const;
};

// One labeled pair, scored once. The feature vector is cached so the tuner can
// rescore thousands of times without recomputing string comparisons.
struct ScoredPair {
  PairRef pair;
  std::string text_a;
  std::string text_b;
  bool is_match = false;
  Split split = Split::kTrain;
  PairScore score;
};

struct EvaluationReport {
  ConfusionMatrix train;
  ConfusionMatrix holdout;
  ConfusionMatrix overall;

  uint64_t pairs_scored = 0;
  uint64_t pairs_unscorable = 0;  // labeled but not in the store
  uint64_t vetoed = 0;

  // The pairs worth looking at by hand: confident mistakes in both directions.
  std::vector<const ScoredPair*> worst_false_positives;
  std::vector<const ScoredPair*> worst_false_negatives;
};

// Apply the current weights to already-scored pairs and count outcomes.
ConfusionMatrix Evaluate(const std::vector<ScoredPair>& pairs,
                         const ScorerConfig& config,
                         const ResolverProperties& props, Split split);

// Both splits plus the mistake lists.
EvaluationReport EvaluateAll(std::vector<ScoredPair>& pairs,
                             const ScorerConfig& config,
                             const ResolverProperties& props,
                             size_t worst_examples = 5);

struct TuningResult {
  ScorerConfig config;
  double train_f1_before = 0.0;
  double train_f1_after = 0.0;
  int passes = 0;
  bool converged = false;
};

// Coordinate ascent over weights and thresholds, on the TRAINING split only.
//
// `entity` selects which weight block is tuned - "Port" or "Vessel" - because
// they are scored independently and a single objective over both would trade
// one against the other for no reason.
TuningResult TuneWeights(const std::vector<ScoredPair>& pairs,
                         const ScorerConfig& start,
                         const ResolverProperties& props,
                         const std::string& entity, int max_passes = 12,
                         double min_improvement = 0.0005);

}  // namespace sextant::resolve
