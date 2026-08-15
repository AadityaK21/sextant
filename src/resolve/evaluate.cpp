#include "evaluate.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "hash.h"

namespace sextant::resolve {
namespace {

// Which weight block a scored pair belongs to. Ports and vessels are tuned
// separately, so the tuner has to be able to tell them apart from the cached
// feature vector alone.
bool IsVesselPair(const ScoredPair& pair) {
  for (const auto& feature : pair.score.features) {
    if (feature.name == "imo_exact" || feature.name == "mmsi_exact" ||
        feature.name == "callsign_exact" || feature.name == "flag_match") {
      return true;
    }
  }
  return false;
}

}  // namespace

double ConfusionMatrix::precision() const {
  const uint64_t predicted = true_positive + false_positive;
  return predicted == 0 ? 0.0
                        : static_cast<double>(true_positive) /
                              static_cast<double>(predicted);
}

double ConfusionMatrix::recall() const {
  const uint64_t actual = true_positive + false_negative;
  return actual == 0 ? 0.0
                     : static_cast<double>(true_positive) /
                           static_cast<double>(actual);
}

double ConfusionMatrix::f1() const {
  const double p = precision();
  const double r = recall();
  return (p + r) == 0.0 ? 0.0 : 2.0 * p * r / (p + r);
}

double ConfusionMatrix::accuracy() const {
  const uint64_t n = total();
  return n == 0 ? 0.0
                : static_cast<double>(true_positive + true_negative) /
                      static_cast<double>(n);
}

Split SplitFor(const PairRef& pair, int holdout_percent) {
  // Hash the pair, not its position. Adding labels later moves the new pairs
  // into a split and leaves every existing one alone, so a number measured
  // today stays comparable with one measured after the golden set grows.
  std::string material;
  material.reserve(24);
  for (const RecordRef& ref : {pair.a, pair.b}) {
    for (int i = 0; i < 4; ++i) {
      material.push_back(static_cast<char>((ref.source >> (8 * i)) & 0xFF));
    }
    for (int i = 0; i < 8; ++i) {
      material.push_back(static_cast<char>((ref.key_hash >> (8 * i)) & 0xFF));
    }
  }
  const uint64_t h = codec::Hash64(lsm::Slice(material));
  return (h % 100) < static_cast<uint64_t>(holdout_percent) ? Split::kHoldout
                                                            : Split::kTrain;
}

ConfusionMatrix Evaluate(const std::vector<ScoredPair>& pairs,
                         const ScorerConfig& config,
                         const ResolverProperties& props, Split split) {
  (void)props;
  ConfusionMatrix matrix;
  for (const auto& pair : pairs) {
    if (pair.split != split) continue;

    PairScore rescored = pair.score;
    PairScorer::Rescore(IsVesselPair(pair) ? config.vessel : config.port,
                        &rescored);

    // Only a MATCH counts as a positive prediction. A pair in the review band
    // is one a human has not confirmed, so counting it as a match would be
    // claiming credit for work not done.
    const bool predicted = rescored.decision == Decision::kMatch;

    if (pair.is_match && predicted) {
      ++matrix.true_positive;
    } else if (!pair.is_match && predicted) {
      ++matrix.false_positive;
    } else if (!pair.is_match && !predicted) {
      ++matrix.true_negative;
    } else {
      ++matrix.false_negative;
      if (rescored.decision == Decision::kReview) ++matrix.missed_to_review;
    }
  }
  return matrix;
}

EvaluationReport EvaluateAll(std::vector<ScoredPair>& pairs,
                             const ScorerConfig& config,
                             const ResolverProperties& props,
                             size_t worst_examples) {
  EvaluationReport report;

  for (auto& pair : pairs) {
    PairScorer::Rescore(IsVesselPair(pair) ? config.vessel : config.port,
                        &pair.score);
    if (pair.score.vetoed) ++report.vetoed;
  }

  report.train = Evaluate(pairs, config, props, Split::kTrain);
  report.holdout = Evaluate(pairs, config, props, Split::kHoldout);

  report.overall.true_positive =
      report.train.true_positive + report.holdout.true_positive;
  report.overall.false_positive =
      report.train.false_positive + report.holdout.false_positive;
  report.overall.true_negative =
      report.train.true_negative + report.holdout.true_negative;
  report.overall.false_negative =
      report.train.false_negative + report.holdout.false_negative;
  report.overall.missed_to_review =
      report.train.missed_to_review + report.holdout.missed_to_review;
  report.pairs_scored = pairs.size();

  // The mistakes worth reading: the ones the scorer was most confident about.
  // A false positive at 12.0 is a bug in a feature; one at 6.1 is a threshold.
  std::vector<const ScoredPair*> fps, fns;
  for (const auto& pair : pairs) {
    const bool predicted = pair.score.decision == Decision::kMatch;
    if (!pair.is_match && predicted) fps.push_back(&pair);
    if (pair.is_match && !predicted) fns.push_back(&pair);
  }
  std::sort(fps.begin(), fps.end(),
            [](const ScoredPair* a, const ScoredPair* b) {
              return a->score.score > b->score.score;
            });
  std::sort(fns.begin(), fns.end(),
            [](const ScoredPair* a, const ScoredPair* b) {
              return a->score.score < b->score.score;
            });
  if (fps.size() > worst_examples) fps.resize(worst_examples);
  if (fns.size() > worst_examples) fns.resize(worst_examples);
  report.worst_false_positives = std::move(fps);
  report.worst_false_negatives = std::move(fns);

  return report;
}

// --- tuning -----------------------------------------------------------------

TuningResult TuneWeights(const std::vector<ScoredPair>& pairs,
                         const ScorerConfig& start,
                         const ResolverProperties& props,
                         const std::string& entity, int max_passes,
                         double min_improvement) {
  TuningResult result;
  result.config = start;

  const bool vessel = entity == "Vessel";

  // Only the pairs of the type being tuned. Ports and vessels are scored by
  // separate weight blocks, and a single objective over both would trade one
  // against the other for no reason.
  std::vector<ScoredPair> subset;
  for (const auto& pair : pairs) {
    if (IsVesselPair(pair) == vessel) subset.push_back(pair);
  }

  auto weights_of = [&](ScorerConfig* config) -> ScorerConfig::TypeWeights& {
    return vessel ? config->vessel : config->port;
  };
  auto train_f1 = [&](const ScorerConfig& config) {
    return Evaluate(subset, config, props, Split::kTrain).f1();
  };

  result.train_f1_before = train_f1(result.config);
  double best = result.train_f1_before;

  // Coordinate ascent: try each weight in turn, keep an improvement, move on.
  // Deliberately not a good optimiser - a good one would be a day's work and
  // the plan is explicit that this is the project's most seductive time sink.
  // This one terminates.
  const double steps[] = {2.0, 1.0, 0.5, 0.25};

  for (int pass = 0; pass < max_passes; ++pass) {
    const double before_pass = best;
    ++result.passes;

    std::vector<std::string> names;
    for (const auto& [name, value] : weights_of(&result.config).weights) {
      names.push_back(name);
    }

    for (const auto& name : names) {
      for (const double step : steps) {
        for (const double direction : {1.0, -1.0}) {
          ScorerConfig candidate = result.config;
          auto& block = weights_of(&candidate);
          const double current = block.Weight(name);
          const double proposed = current + direction * step;
          // Negative weights are excluded. A feature whose agreement is
          // evidence AGAINST identity is not a weight problem, it is a
          // modelling mistake, and letting the optimiser find one would hide it.
          if (proposed < 0.0) continue;
          block.SetWeight(name, proposed);

          const double f1 = train_f1(candidate);
          if (f1 > best + 1e-12) {
            best = f1;
            result.config = candidate;
          }
        }
      }
    }

    // Thresholds too - they matter as much as any weight and are the only knob
    // that trades precision against recall directly.
    for (const double step : {1.0, 0.5, 0.25}) {
      for (const double direction : {1.0, -1.0}) {
        ScorerConfig candidate = result.config;
        auto& block = weights_of(&candidate);
        block.match_threshold += direction * step;
        if (block.match_threshold < block.review_threshold) continue;
        const double f1 = train_f1(candidate);
        if (f1 > best + 1e-12) {
          best = f1;
          result.config = candidate;
        }
      }
    }

    if (best - before_pass < min_improvement) {
      result.converged = true;
      break;
    }
  }

  result.train_f1_after = best;
  return result;
}

}  // namespace sextant::resolve
