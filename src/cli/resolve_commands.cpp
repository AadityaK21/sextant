#include "resolve_commands.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blocking.h"
#include "bundle.h"
#include "cluster.h"
#include "env.h"
#include "evaluate.h"
#include "fuse.h"
#include "golden.h"
#include "scorer.h"
#include "sextant/lsm/options.h"
#include "store.h"

namespace sextant::cli {
namespace {

namespace onto = sextant::ontology;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;
namespace resolve = sextant::resolve;

using lsm::Status;

Status EnsureParents(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] != '/' && path[i] != '\\') continue;
    const std::string parent = path.substr(0, i);
    if (parent.empty() || lsm::FileExists(parent)) continue;
    const Status s = lsm::CreateDir(parent);
    if (!s.ok() && !lsm::FileExists(parent)) return s;
  }
  return Status::OK();
}

// Everything the two commands need: schema, store, scorer config, and the
// source records loaded into memory so they can be scored repeatedly.
struct Context {
  onto::SchemaBundle bundle;
  std::unique_ptr<codec::Store> store;
  resolve::ResolverProperties props;
  resolve::ScorerConfig config;

  std::unordered_map<resolve::RecordRef, onto::SourceRecord,
                     resolve::RecordRefHash>
      records;

  std::vector<resolve::CandidatePairRef> candidates;
  resolve::BlockingReport blocking;
};

bool Setup(const Args& args, Context* ctx, bool with_candidates) {
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"),
                                             &ctx->bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return false;
  }

  const std::string db = args.Get("db", "sextant-db");
  s = EnsureParents(db);
  if (s.ok()) {
    lsm::Options options;
    options.create_if_missing = true;
    s = codec::Store::Open(options, db, &ctx->store);
  }
  if (!s.ok()) {
    std::fprintf(stderr, "store: %s\n", s.ToString().c_str());
    return false;
  }

  s = resolve::ResolverProperties::Resolve(ctx->bundle.ontology(), &ctx->props);
  if (!s.ok()) {
    std::fprintf(stderr, "resolver: %s\n", s.ToString().c_str());
    return false;
  }

  // A missing config is not an error - the built-in defaults are the same
  // numbers the committed file holds, and a fresh clone should still run.
  const std::string config_path =
      args.Get("config", args.Get("schema", "schema") + "/resolver.yaml");
  ctx->config = resolve::ScorerConfig::Defaults();
  const Status cs = resolve::ScorerConfig::LoadFromFile(config_path, &ctx->config);
  if (!cs.ok()) {
    std::fprintf(stderr, "  (using built-in weights: %s)\n", cs.ToString().c_str());
  }

  // Source records, held in memory. At corpus scale this is a few megabytes and
  // it makes the tuner's inner loop possible; at full scale it would have to
  // stream, which is a real limitation and is stated in docs/ER.md.
  for (const auto& spec : ctx->bundle.sources()) {
    auto it = ctx->store->ScanSourceRecords(spec.id);
    for (; it->Valid(); it->Next()) {
      lsm::Slice value = it->value();
      onto::SourceRecord record;
      if (!onto::SourceRecord::DecodeFrom(&value, &record)) continue;
      ctx->records[resolve::RecordRef{record.source_id, record.natural_key_hash}] =
          std::move(record);
    }
  }
  if (ctx->records.empty()) {
    std::fprintf(stderr,
                 "no source records in %s - run `sextant ingest` first\n",
                 db.c_str());
    return false;
  }

  if (with_candidates) {
    resolve::Blocker blocker(ctx->store.get(), &ctx->bundle, &ctx->props);
    s = blocker.IndexAll(&ctx->blocking);
    if (s.ok()) {
      s = blocker.GenerateCandidates({}, &ctx->candidates, &ctx->blocking);
    }
    if (!s.ok()) {
      std::fprintf(stderr, "blocking: %s\n", s.ToString().c_str());
      return false;
    }
  }
  return true;
}

void PrintMatrix(const char* label, const resolve::ConfusionMatrix& m) {
  std::printf("  %-9s  TP %-5llu FP %-5llu TN %-5llu FN %-5llu",
              label, static_cast<unsigned long long>(m.true_positive),
              static_cast<unsigned long long>(m.false_positive),
              static_cast<unsigned long long>(m.true_negative),
              static_cast<unsigned long long>(m.false_negative));
  std::printf("   P %.4f  R %.4f  F1 %.4f\n", m.precision(), m.recall(), m.f1());
}

// Score every labeled pair we can find records for.
std::vector<resolve::ScoredPair> ScoreGolden(const Context& ctx,
                                             const resolve::GoldenSet& golden,
                                             uint64_t* unscorable) {
  const resolve::PairScorer scorer(&ctx.config, &ctx.props);
  std::vector<resolve::ScoredPair> scored;
  scored.reserve(golden.pairs().size());

  for (const auto& labeled : golden.pairs()) {
    const auto a = ctx.records.find(labeled.pair.a);
    const auto b = ctx.records.find(labeled.pair.b);
    if (a == ctx.records.end() || b == ctx.records.end()) {
      ++*unscorable;
      continue;
    }
    resolve::ScoredPair pair;
    pair.pair = labeled.pair;
    pair.text_a = labeled.text_a;
    pair.text_b = labeled.text_b;
    pair.is_match = labeled.is_match;
    pair.split = resolve::SplitFor(labeled.pair);
    pair.score = scorer.Score(a->second, b->second);
    scored.push_back(std::move(pair));
  }
  return scored;
}

}  // namespace

bool Args::Has(const std::string& name) const { return flags.count(name) != 0; }

std::string Args::Get(const std::string& name, const std::string& fallback) const {
  const auto it = flags.find(name);
  return it == flags.end() ? fallback : it->second;
}

uint64_t Args::GetU64(const std::string& name, uint64_t fallback) const {
  const auto it = flags.find(name);
  if (it == flags.end() || it->second.empty()) return fallback;
  return std::strtoull(it->second.c_str(), nullptr, 10);
}

// --- eval -------------------------------------------------------------------

int CmdEval(const Args& args) {
  Context ctx;
  if (!Setup(args, &ctx, /*with_candidates=*/false)) return 1;

  const std::string eval_dir = args.Get("eval", "eval");
  const bool tune = args.Has("tune");
  resolve::ScorerConfig tuned = ctx.config;

  for (const auto& [file, entity] :
       {std::pair<const char*, const char*>{"golden_ports.csv", "Port"},
        std::pair<const char*, const char*>{"golden_vessels.csv", "Vessel"}}) {
    resolve::GoldenSet golden;
    const std::string path = eval_dir + "/" + file;
    const Status s = resolve::GoldenSet::LoadFromFile(path, ctx.bundle, &golden);
    if (!s.ok()) {
      std::fprintf(stderr, "%s: %s\n", file, s.ToString().c_str());
      continue;
    }

    uint64_t unscorable = 0;
    std::vector<resolve::ScoredPair> scored = ScoreGolden(ctx, golden, &unscorable);

    std::printf("\n%s  (%s)\n", file, entity);
    std::printf("  %zu labeled pairs, %llu scored, %llu unscorable\n",
                golden.pairs().size(),
                static_cast<unsigned long long>(scored.size()),
                static_cast<unsigned long long>(unscorable));

    if (tune) {
      const resolve::TuningResult result = resolve::TuneWeights(
          scored, ctx.config, ctx.props, entity);
      std::printf("  tuning: train F1 %.4f -> %.4f in %d passes%s\n",
                  result.train_f1_before, result.train_f1_after, result.passes,
                  result.converged ? " (converged)" : " (hit the pass limit)");
      if (std::string(entity) == "Port") {
        tuned.port = result.config.port;
      } else {
        tuned.vessel = result.config.vessel;
      }
    }

    const resolve::ScorerConfig& active = tune ? tuned : ctx.config;
    const resolve::EvaluationReport report =
        resolve::EvaluateAll(scored, active, ctx.props);

    PrintMatrix("train", report.train);
    PrintMatrix("HELD-OUT", report.holdout);
    PrintMatrix("overall", report.overall);
    std::printf("  %llu vetoed, %llu true pairs parked in review\n",
                static_cast<unsigned long long>(report.vetoed),
                static_cast<unsigned long long>(report.overall.missed_to_review));

    // The mistakes worth reading by hand. A false positive at 12.0 is a bug in
    // a feature; one at 6.1 is a threshold.
    if (!report.worst_false_positives.empty()) {
      std::printf("\n  worst false positives (merged and should not have been):\n");
      for (const auto* pair : report.worst_false_positives) {
        std::printf("    %.2f  %s <-> %s\n      %s\n", pair->score.score,
                    pair->text_a.c_str(), pair->text_b.c_str(),
                    pair->score.Explain().c_str());
      }
    }
    if (!report.worst_false_negatives.empty()) {
      std::printf("\n  worst false negatives (missed):\n");
      for (const auto* pair : report.worst_false_negatives) {
        std::printf("    %.2f  %s <-> %s\n      %s\n", pair->score.score,
                    pair->text_a.c_str(), pair->text_b.c_str(),
                    pair->score.Explain().c_str());
      }
    }
  }

  if (tune) {
    std::printf(
        "\n--- tuned weights, fitted on the training split only ---\n"
        "--- paste into schema/resolver.yaml ---\n\n%s\n",
        tuned.ToYaml().c_str());
  }
  return 0;
}

// --- resolve ----------------------------------------------------------------

int CmdResolve(const Args& args) {
  Context ctx;
  if (!Setup(args, &ctx, /*with_candidates=*/true)) return 1;

  std::printf("resolve\n");
  std::printf("  %zu source records, %llu candidate pairs\n", ctx.records.size(),
              static_cast<unsigned long long>(ctx.blocking.candidate_pairs));

  // Score every candidate pair the blocker proposed.
  const resolve::PairScorer scorer(&ctx.config, &ctx.props);
  std::vector<resolve::ScoredEdge> edges;
  edges.reserve(ctx.candidates.size());
  uint64_t matched = 0, review = 0, vetoed = 0;

  for (const auto& candidate : ctx.candidates) {
    const auto a = ctx.records.find(candidate.pair.a);
    const auto b = ctx.records.find(candidate.pair.b);
    if (a == ctx.records.end() || b == ctx.records.end()) continue;

    const resolve::PairScore score = scorer.Score(a->second, b->second);
    resolve::ScoredEdge edge;
    edge.pair = candidate.pair;
    edge.score = score.score;
    edge.decision = score.decision;
    edge.vetoed = score.vetoed;
    edge.veto_reason = score.veto_reason;
    edges.push_back(std::move(edge));

    if (score.vetoed) ++vetoed;
    if (score.decision == resolve::Decision::kMatch) ++matched;
    if (score.decision == resolve::Decision::kReview) ++review;

    // The review band goes to the CAND keyspace, keyed by inverted score so a
    // scan returns the most uncertain pairs first.
    if (score.decision == resolve::Decision::kReview) {
      const std::string explanation = score.Explain();
      ctx.store->PutCandidate(score.score,
                              candidate.pair.a.key_hash ^ candidate.pair.b.key_hash,
                              lsm::Slice(explanation));
    }
  }

  std::printf("  %llu match, %llu review, %llu vetoed\n",
              static_cast<unsigned long long>(matched),
              static_cast<unsigned long long>(review),
              static_cast<unsigned long long>(vetoed));

  // Both clusterings, so the comparison is produced by the same run that writes
  // the entities rather than by a separate experiment.
  const resolve::ClusterSet plain = resolve::ClusterTransitive(edges);
  const resolve::ClusterSet constrained = resolve::ClusterVetoConstrained(edges);

  std::printf("\n  %-22s %8s %8s %10s %8s\n", "clustering", "clusters", "largest",
              "singletons", "refused");
  std::printf("  %-22s %8zu %8zu %10zu %8s\n", "transitive union-find",
              plain.clusters.size(), plain.largest(), plain.singletons(), "-");
  std::printf("  %-22s %8zu %8zu %10zu %8zu\n", "veto-constrained",
              constrained.clusters.size(), constrained.largest(),
              constrained.singletons(), constrained.refused.size());

  const std::string eval_dir = args.Get("eval", "eval");
  for (const char* file : {"golden_ports.csv", "golden_vessels.csv"}) {
    resolve::GoldenSet golden;
    if (!resolve::GoldenSet::LoadFromFile(eval_dir + "/" + file, ctx.bundle,
                                          &golden)
             .ok()) {
      continue;
    }
    const resolve::ClusterMetrics a = resolve::MeasureClusters(plain, golden);
    const resolve::ClusterMetrics b = resolve::MeasureClusters(constrained, golden);
    std::printf("\n  %s - cluster-level, over the pairs the clustering IMPLIES\n",
                file);
    std::printf("  %-22s P %.4f  R %.4f  F1 %.4f\n", "transitive", a.precision(),
                a.recall(), a.f1());
    std::printf("  %-22s P %.4f  R %.4f  F1 %.4f\n", "veto-constrained",
                b.precision(), b.recall(), b.f1());
  }

  if (args.Has("dry-run")) {
    std::printf("\n  --dry-run: no entities written\n");
    return 0;
  }

  // Write the entities from the constrained clustering, which is the one the
  // system actually uses.
  const resolve::Fuser fuser(&ctx.bundle, &ctx.props);
  uint64_t written = 0, properties = 0;
  for (const auto& cluster : constrained.clusters) {
    std::vector<const onto::SourceRecord*> records;
    for (const auto& member : cluster) {
      const auto it = ctx.records.find(member);
      if (it != ctx.records.end()) records.push_back(&it->second);
    }
    if (records.empty()) continue;

    std::vector<std::string> evidence;
    if (cluster.size() > 1) {
      evidence.push_back("merged from " + std::to_string(cluster.size()) +
                         " source records");
    }
    const resolve::ResolvedEntity entity =
        fuser.Fuse(records, cluster, evidence);
    const Status s = resolve::WriteEntity(ctx.store.get(), ctx.bundle, entity);
    if (!s.ok()) {
      std::fprintf(stderr, "  write: %s\n", s.ToString().c_str());
      return 1;
    }
    ++written;
    properties += entity.properties.size();
  }

  std::printf("\n  %llu entities written, %llu properties, %llu provenance"
              " records\n",
              static_cast<unsigned long long>(written),
              static_cast<unsigned long long>(properties),
              static_cast<unsigned long long>(properties));
  const double dedup =
      ctx.records.empty()
          ? 0.0
          : 1.0 - static_cast<double>(written) /
                      static_cast<double>(ctx.records.size());
  std::printf("  dedup ratio %.4f  (%zu records -> %llu entities)\n", dedup,
              ctx.records.size(), static_cast<unsigned long long>(written));
  return 0;
}

}  // namespace sextant::cli
