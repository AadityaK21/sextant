// The pair scorer: does this pair of records describe the same thing?
//
// THE OUTPUT IS A FEATURE VECTOR, NOT A NUMBER
//
// A scorer that returns 0.87 is useless to the thing this project is for. The
// lineage panel has to answer "why did these two merge", and the only honest
// answer is the per-feature breakdown:
//
//     locode_exact       +6.0   both NLRTM
//     name_jaro_winkler  +1.8   Rotterdam / ROTTERDAM  (0.94)
//     geo_distance       +1.1   2.3 km apart
//     country_match      +0.5   both NL
//                        -----
//                        +9.4   MATCH
//
// That is also what makes the weights tunable by a person rather than only by
// an optimiser: you can look at a wrong answer and see which feature carried
// it.
//
// WHY A WEIGHTED SUM AND NOT A LEARNED MODEL
//
// This is Fellegi-Sunter in shape: independent features, additive evidence,
// two thresholds. A logistic regression or a gradient-boosted tree would score
// better with enough labels. With a few hundred it would overfit, and it would
// destroy the explanation above - "the model said so" is not a lineage record.
// The trade is stated in the README's non-goals and it is a real one: give me
// 100,000 labels and I would use a learned model and keep the feature vector
// as the explanation layer.
//
// VETOES ARE NOT NEGATIVE WEIGHTS
//
// Some disagreements are not evidence to be outweighed - they are proof of
// non-identity. Two vessels with different IMO numbers are different hulls, no
// matter how identical everything else is, because an IMO is assigned to the
// hull for life. Encoding that as a large negative weight would let a
// sufficiently strong pile of other evidence overcome it, which is exactly
// wrong.
//
// So a veto short-circuits: the score is not computed, the decision is
// NON_MATCH, and the reason is recorded. The corpus contains three pairs where
// the MMSI matches exactly and the IMO does not, precisely so that anything
// treating this as a weight rather than a rule fails visibly.
//
// THREE OUTCOMES, NOT TWO
//
// Above the upper threshold is a match, below the lower is not, and between
// them is REVIEW - written to the CAND keyspace for a human. Forcing a binary
// decision on genuinely ambiguous evidence is how a resolver ends up confidently
// wrong. The review band is also where the F1 number gets honest: pairs in it
// count as non-matches for the metric, so parking a hard pair in review costs
// recall rather than hiding it.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blocking.h"
#include "record.h"
#include "sextant/lsm/status.h"

namespace sextant::resolve {

using ontology::SourceRecord;

enum class Decision { kNonMatch, kReview, kMatch };
const char* DecisionName(Decision decision);

// One feature's contribution to a decision. `detail` is human-readable and
// goes straight into the lineage panel, so it names the values compared rather
// than only the outcome.
struct Feature {
  std::string name;
  double value = 0.0;         // the raw measure, normally 0 to 1
  double weight = 0.0;
  double contribution = 0.0;  // value * weight
  std::string detail;
};

struct PairScore {
  std::vector<Feature> features;
  double score = 0.0;
  Decision decision = Decision::kNonMatch;

  bool vetoed = false;
  std::string veto_reason;

  // Sorted by absolute contribution, largest first - the order a person wants
  // to read them in.
  std::vector<Feature> TopContributions(size_t n) const;
  std::string Explain() const;
};

// Weights and thresholds, loaded from schema/resolver.yaml.
//
// In a file rather than in code because they are the one part of this system
// that is genuinely fitted to data. Recompiling to change a number that came
// out of an optimiser is how the number in the README stops matching the
// number in the binary.
struct ScorerConfig {
  struct TypeWeights {
    std::vector<std::pair<std::string, double>> weights;
    double match_threshold = 0.0;
    double review_threshold = 0.0;

    double Weight(const std::string& name) const;
    void SetWeight(const std::string& name, double value);
  };

  TypeWeights port;
  TypeWeights vessel;

  // A pair of ports further apart than this cannot be the same place, whatever
  // their names say.
  double port_max_km = 150.0;
  // Distance at which the geographic feature has decayed to roughly a third.
  double port_geo_scale_km = 25.0;

  static Status LoadFromFile(const std::string& path, ScorerConfig* out);
  static Status LoadFromString(const std::string& yaml, ScorerConfig* out,
                               const std::string& origin = "<string>");
  static ScorerConfig Defaults();

  std::string ToYaml() const;
};

class PairScorer {
 public:
  PairScorer(const ScorerConfig* config, const ResolverProperties* props);

  PairScore Score(const SourceRecord& a, const SourceRecord& b) const;

  // Exposed so the tuner can rescore a cached feature vector without
  // recomputing every string comparison - the inner loop of coordinate ascent
  // runs thousands of times over the same pairs.
  static void Rescore(const ScorerConfig::TypeWeights& weights, PairScore* score);

 private:
  PairScore ScorePorts(const SourceRecord& a, const SourceRecord& b) const;
  PairScore ScoreVessels(const SourceRecord& a, const SourceRecord& b) const;

  const ScorerConfig* config_;
  const ResolverProperties* props_;
};

}  // namespace sextant::resolve
