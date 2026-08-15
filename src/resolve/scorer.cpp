#include "scorer.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "geohash.h"
#include "normalize.h"
#include "similarity.h"

namespace sextant::resolve {
namespace {

namespace onto = sextant::ontology;

const std::string* StringOf(const SourceRecord& record, codec::PropId prop) {
  const onto::PropertyCell* cell = record.Property(prop);
  if (cell == nullptr || cell->value.IsNull()) return nullptr;
  if (cell->value.type() != onto::ValueType::kString) return nullptr;
  return &cell->value.AsString();
}

bool DoubleOf(const SourceRecord& record, codec::PropId prop, double* out) {
  const onto::PropertyCell* cell = record.Property(prop);
  if (cell == nullptr || cell->value.IsNull()) return false;
  if (cell->value.type() != onto::ValueType::kDouble) return false;
  *out = cell->value.AsDouble();
  return true;
}

std::vector<std::string> StringListOf(const SourceRecord& record,
                                      codec::PropId prop) {
  const onto::PropertyCell* cell = record.Property(prop);
  if (cell == nullptr || cell->value.IsNull()) return {};
  if (cell->value.type() == onto::ValueType::kStringList) {
    return cell->value.AsStringList();
  }
  if (cell->value.type() == onto::ValueType::kString) {
    return {cell->value.AsString()};
  }
  return {};
}

void Add(PairScore* score, const ScorerConfig::TypeWeights& weights,
         const std::string& name, double value, std::string detail) {
  Feature feature;
  feature.name = name;
  feature.value = value;
  feature.weight = weights.Weight(name);
  feature.contribution = value * feature.weight;
  feature.detail = std::move(detail);
  score->score += feature.contribution;
  score->features.push_back(std::move(feature));
}

std::string Fmt(const char* format, double value) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), format, value);
  return buf;
}

void Decide(const ScorerConfig::TypeWeights& weights, PairScore* score) {
  if (score->vetoed) {
    score->decision = Decision::kNonMatch;
    return;
  }
  if (score->score >= weights.match_threshold) {
    score->decision = Decision::kMatch;
  } else if (score->score >= weights.review_threshold) {
    score->decision = Decision::kReview;
  } else {
    score->decision = Decision::kNonMatch;
  }
}

}  // namespace

const char* DecisionName(Decision decision) {
  switch (decision) {
    case Decision::kNonMatch: return "non_match";
    case Decision::kReview: return "review";
    case Decision::kMatch: return "match";
  }
  return "unknown";
}

double ScorerConfig::TypeWeights::Weight(const std::string& name) const {
  for (const auto& [key, value] : weights) {
    if (key == name) return value;
  }
  return 0.0;
}

void ScorerConfig::TypeWeights::SetWeight(const std::string& name, double value) {
  for (auto& [key, existing] : weights) {
    if (key == name) {
      existing = value;
      return;
    }
  }
  weights.emplace_back(name, value);
}

std::vector<Feature> PairScore::TopContributions(size_t n) const {
  std::vector<Feature> sorted = features;
  std::sort(sorted.begin(), sorted.end(), [](const Feature& a, const Feature& b) {
    return std::fabs(a.contribution) > std::fabs(b.contribution);
  });
  if (sorted.size() > n) sorted.resize(n);
  return sorted;
}

std::string PairScore::Explain() const {
  if (vetoed) return "VETO: " + veto_reason;
  std::string out;
  for (const auto& feature : TopContributions(4)) {
    if (feature.contribution == 0.0) continue;
    if (!out.empty()) out += ", ";
    out += feature.name + " " + Fmt("%+.2f", feature.contribution);
    if (!feature.detail.empty()) out += " (" + feature.detail + ")";
  }
  return out;
}

// --- config -----------------------------------------------------------------

ScorerConfig ScorerConfig::Defaults() {
  ScorerConfig config;

  // Starting weights, set by hand from what each feature is worth as evidence,
  // then fitted by `sextant eval --tune` on the training split. The committed
  // schema/resolver.yaml holds the fitted values; these are the fallback so the
  // scorer works with no config file at all.
  config.port.weights = {
      {"locode_exact", 6.0},   {"name_exact", 3.0},
      {"name_jaro_winkler", 3.0}, {"name_jaccard", 1.5},
      {"name_contained", 1.5}, {"alt_name_match", 2.5},
      {"country_match", 1.0},  {"geo_proximity", 2.5},
  };
  config.port.match_threshold = 6.0;
  config.port.review_threshold = 4.0;

  config.vessel.weights = {
      {"imo_exact", 8.0},        {"mmsi_exact", 3.0},
      {"callsign_exact", 2.0},   {"name_exact", 2.5},
      {"name_jaro_winkler", 3.0}, {"name_jaccard", 1.0},
      {"flag_match", 0.5},
  };
  config.vessel.match_threshold = 5.0;
  config.vessel.review_threshold = 3.0;

  return config;
}

namespace {

Status LoadWeights(const YAML::Node& node, ScorerConfig::TypeWeights* out,
                   const std::string& origin, const std::string& what) {
  if (!node || !node.IsMap()) {
    return Status::InvalidArgument(origin + ": missing `" + what + "` block");
  }
  const YAML::Node weights = node["weights"];
  if (!weights || !weights.IsMap()) {
    return Status::InvalidArgument(origin + ": " + what + " has no `weights`");
  }
  out->weights.clear();
  for (const auto& kv : weights) {
    out->weights.emplace_back(kv.first.as<std::string>(), kv.second.as<double>());
  }
  if (!node["match_threshold"] || !node["review_threshold"]) {
    return Status::InvalidArgument(origin + ": " + what +
                                   " needs match_threshold and review_threshold");
  }
  out->match_threshold = node["match_threshold"].as<double>();
  out->review_threshold = node["review_threshold"].as<double>();
  if (out->review_threshold > out->match_threshold) {
    return Status::InvalidArgument(
        origin + ": " + what +
        " has review_threshold above match_threshold, which would make the"
        " review band empty and every uncertain pair a match");
  }
  return Status::OK();
}

}  // namespace

Status ScorerConfig::LoadFromString(const std::string& yaml, ScorerConfig* out,
                                    const std::string& origin) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(origin + ": " + e.what());
  }

  ScorerConfig config = Defaults();
  try {
    Status s = LoadWeights(root["Port"], &config.port, origin, "Port");
    if (!s.ok()) return s;
    s = LoadWeights(root["Vessel"], &config.vessel, origin, "Vessel");
    if (!s.ok()) return s;

    if (root["port_max_km"]) config.port_max_km = root["port_max_km"].as<double>();
    if (root["port_geo_scale_km"]) {
      config.port_geo_scale_km = root["port_geo_scale_km"].as<double>();
    }
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(origin + ": " + e.what());
  }

  *out = std::move(config);
  return Status::OK();
}

Status ScorerConfig::LoadFromFile(const std::string& path, ScorerConfig* out) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
    return LoadFromString(YAML::Dump(root), out, path);
  } catch (const YAML::Exception& e) {
    return Status::InvalidArgument(path + ": " + e.what());
  }
}

std::string ScorerConfig::ToYaml() const {
  std::string out;
  auto emit = [&out](const char* name, const TypeWeights& weights) {
    out += std::string(name) + ":\n  weights:\n";
    for (const auto& [key, value] : weights.weights) {
      out += "    " + key + ": " + Fmt("%.2f", value) + "\n";
    }
    out += "  match_threshold: " + Fmt("%.2f", weights.match_threshold) + "\n";
    out += "  review_threshold: " + Fmt("%.2f", weights.review_threshold) + "\n\n";
  };
  emit("Port", port);
  emit("Vessel", vessel);
  out += "port_max_km: " + Fmt("%.1f", port_max_km) + "\n";
  out += "port_geo_scale_km: " + Fmt("%.1f", port_geo_scale_km) + "\n";
  return out;
}

// --- scoring ----------------------------------------------------------------

PairScorer::PairScorer(const ScorerConfig* config, const ResolverProperties* props)
    : config_(config), props_(props) {}

PairScore PairScorer::Score(const SourceRecord& a, const SourceRecord& b) const {
  if (a.type != b.type) {
    PairScore score;
    score.vetoed = true;
    score.veto_reason = "different entity types";
    score.decision = Decision::kNonMatch;
    return score;
  }
  if (a.type == props_->port_type) return ScorePorts(a, b);
  if (a.type == props_->vessel_type) return ScoreVessels(a, b);
  return PairScore{};
}

PairScore PairScorer::ScorePorts(const SourceRecord& a,
                                 const SourceRecord& b) const {
  const auto& weights = config_->port;
  PairScore score;

  // --- vetoes, before anything is computed ---------------------------------

  const std::string* locode_a = StringOf(a, props_->port_locode);
  const std::string* locode_b = StringOf(b, props_->port_locode);
  if (locode_a != nullptr && locode_b != nullptr && *locode_a != *locode_b) {
    // UN/LOCODE is the code authority. Two records carrying different codes are
    // making an explicit claim to be different places, and no amount of name
    // similarity outranks that.
    score.vetoed = true;
    score.veto_reason = "locode conflict: " + *locode_a + " vs " + *locode_b;
    score.decision = Decision::kNonMatch;
    return score;
  }

  const std::string* country_a = StringOf(a, props_->port_country);
  const std::string* country_b = StringOf(b, props_->port_country);
  if (country_a != nullptr && country_b != nullptr && *country_a != *country_b) {
    score.vetoed = true;
    score.veto_reason = "country conflict: " + *country_a + " vs " + *country_b;
    score.decision = Decision::kNonMatch;
    return score;
  }

  double lat_a = 0, lon_a = 0, lat_b = 0, lon_b = 0;
  const bool geo_a = DoubleOf(a, props_->port_lat, &lat_a) &&
                     DoubleOf(a, props_->port_lon, &lon_a);
  const bool geo_b = DoubleOf(b, props_->port_lat, &lat_b) &&
                     DoubleOf(b, props_->port_lon, &lon_b);
  double distance_km = -1.0;
  if (geo_a && geo_b) {
    distance_km = HaversineKm(lat_a, lon_a, lat_b, lon_b);
    if (distance_km > config_->port_max_km) {
      score.vetoed = true;
      score.veto_reason = Fmt("%.0f km apart", distance_km) + ", beyond the " +
                          Fmt("%.0f km", config_->port_max_km) + " limit";
      score.decision = Decision::kNonMatch;
      return score;
    }
  }

  // --- features ------------------------------------------------------------

  if (locode_a != nullptr && locode_b != nullptr) {
    Add(&score, weights, "locode_exact", 1.0, "both " + *locode_a);
  }

  const std::string* name_a = StringOf(a, props_->port_name);
  const std::string* name_b = StringOf(b, props_->port_name);
  if (name_a != nullptr && name_b != nullptr) {
    const NormalizedName na = NormalizePortName(*name_a);
    const NormalizedName nb = NormalizePortName(*name_b);

    if (na.canonical == nb.canonical) {
      Add(&score, weights, "name_exact", 1.0, na.canonical);
    } else {
      const double jw = JaroWinkler(na.canonical, nb.canonical);
      Add(&score, weights, "name_jaro_winkler", jw,
          na.canonical + " / " + nb.canonical + " " + Fmt("(%.2f)", jw));

      const double jaccard = TokenJaccard(na.tokens, nb.tokens);
      if (jaccard > 0.0) {
        Add(&score, weights, "name_jaccard", jaccard, Fmt("%.2f", jaccard));
      }
      // "Rotterdam" entirely inside "Rotterdam Botlek". Jaccard scores that 0.5
      // because it counts the extra token against the pair; for a port
      // sub-location, being wholly contained is much stronger evidence than the
      // ratio suggests.
      if (TokensContained(na.tokens, nb.tokens)) {
        Add(&score, weights, "name_contained", 1.0,
            "one name contains the other");
      }
    }

    // The World Port Index lists sub-locations as alternate names on the parent
    // row, which is the only link between records that share nothing else. This
    // is the feature that reaches the Europoort case day 8's blocking missed.
    const std::vector<std::string> alts_a = StringListOf(a, props_->port_alt_names);
    const std::vector<std::string> alts_b = StringListOf(b, props_->port_alt_names);
    bool alt_hit = false;
    std::string alt_detail;
    for (const auto& alt : alts_a) {
      if (NormalizePortName(alt).canonical == nb.canonical) {
        alt_hit = true;
        alt_detail = *name_b + " is an alternate name of " + *name_a;
      }
    }
    for (const auto& alt : alts_b) {
      if (NormalizePortName(alt).canonical == na.canonical) {
        alt_hit = true;
        alt_detail = *name_a + " is an alternate name of " + *name_b;
      }
    }
    if (alt_hit) Add(&score, weights, "alt_name_match", 1.0, alt_detail);
  }

  if (country_a != nullptr && country_b != nullptr) {
    Add(&score, weights, "country_match", 1.0, "both " + *country_a);
  }

  if (distance_km >= 0.0) {
    // Exponential decay rather than a linear ramp. Two port records 2 km apart
    // and 4 km apart are both "the same port"; 40 km and 80 km are both "far",
    // and a linear scale would spend most of its range distinguishing distances
    // that carry no information.
    const double proximity =
        std::exp(-distance_km / config_->port_geo_scale_km);
    Add(&score, weights, "geo_proximity", proximity,
        Fmt("%.1f km apart", distance_km));
  }

  Decide(weights, &score);
  return score;
}

PairScore PairScorer::ScoreVessels(const SourceRecord& a,
                                   const SourceRecord& b) const {
  const auto& weights = config_->vessel;
  PairScore score;

  const std::string* imo_a = StringOf(a, props_->vessel_imo);
  const std::string* imo_b = StringOf(b, props_->vessel_imo);

  // THE VETO THIS WHOLE MECHANISM EXISTS FOR.
  //
  // An IMO number is assigned to the hull when it is built and never changes -
  // not on sale, not on rename, not on reflagging. Two records carrying
  // different IMO numbers are different ships, and it does not matter that they
  // share an MMSI, a call sign and a name.
  //
  // That case is real: an MMSI belongs to the radio licence and is reissued
  // when a vessel reflags, so the AIS feed and the registry can legitimately
  // disagree about which hull an MMSI refers to. Three such pairs are in the
  // corpus. A large negative weight would let the rest of the evidence
  // outvote this, which is precisely the failure being prevented.
  if (imo_a != nullptr && imo_b != nullptr && *imo_a != *imo_b) {
    score.vetoed = true;
    score.veto_reason = "IMO conflict: " + *imo_a + " vs " + *imo_b +
                        " - an IMO is permanent, so these are different hulls";
    score.decision = Decision::kNonMatch;
    return score;
  }

  if (imo_a != nullptr && imo_b != nullptr) {
    Add(&score, weights, "imo_exact", 1.0, "both " + *imo_a);
  }

  const std::string* mmsi_a = StringOf(a, props_->vessel_mmsi);
  const std::string* mmsi_b = StringOf(b, props_->vessel_mmsi);
  if (mmsi_a != nullptr && mmsi_b != nullptr && *mmsi_a == *mmsi_b) {
    Add(&score, weights, "mmsi_exact", 1.0, "both " + *mmsi_a);
  }

  const std::string* call_a = StringOf(a, props_->vessel_call_sign);
  const std::string* call_b = StringOf(b, props_->vessel_call_sign);
  if (call_a != nullptr && call_b != nullptr && *call_a == *call_b) {
    Add(&score, weights, "callsign_exact", 1.0, "both " + *call_a);
  }

  const std::string* name_a = StringOf(a, props_->vessel_name);
  const std::string* name_b = StringOf(b, props_->vessel_name);
  if (name_a != nullptr && name_b != nullptr) {
    const NormalizedName na = NormalizeVesselName(*name_a);
    const NormalizedName nb = NormalizeVesselName(*name_b);
    if (na.canonical == nb.canonical) {
      Add(&score, weights, "name_exact", 1.0, na.canonical);
    } else {
      const double jw = JaroWinkler(na.canonical, nb.canonical);
      Add(&score, weights, "name_jaro_winkler", jw,
          na.canonical + " / " + nb.canonical + " " + Fmt("(%.2f)", jw));
      const double jaccard = TokenJaccard(na.tokens, nb.tokens);
      if (jaccard > 0.0) {
        Add(&score, weights, "name_jaccard", jaccard, Fmt("%.2f", jaccard));
      }
    }
  }

  const std::string* flag_a = StringOf(a, props_->vessel_flag);
  const std::string* flag_b = StringOf(b, props_->vessel_flag);
  if (flag_a != nullptr && flag_b != nullptr && *flag_a == *flag_b) {
    Add(&score, weights, "flag_match", 1.0, "both " + *flag_a);
  }

  Decide(weights, &score);
  return score;
}

void PairScorer::Rescore(const ScorerConfig::TypeWeights& weights,
                         PairScore* score) {
  if (score->vetoed) {
    score->decision = Decision::kNonMatch;
    return;
  }
  score->score = 0.0;
  for (auto& feature : score->features) {
    feature.weight = weights.Weight(feature.name);
    feature.contribution = feature.value * feature.weight;
    score->score += feature.contribution;
  }
  Decide(weights, score);
}

}  // namespace sextant::resolve
