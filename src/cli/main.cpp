// sextant - the command line.
//
// Four subcommands for now: schema, ingest, stats and lineage. Resolve, query
// and serve arrive with the milestones that implement them.
//
// The argument parser is hand-written and about sixty lines. CLI11 would be
// nicer and is one more dependency to fetch, configure and explain, for a
// surface that is four verbs and six flags. It can be added the day the CLI is
// large enough to justify it.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// For SetConsoleOutputCP. NOMINMAX and WIN32_LEAN_AND_MEAN are set globally in
// CMakeLists.txt for the reason documented in env.cpp: <windows.h> defines a
// pile of ordinary-looking names as macros, and a macro does not respect
// namespaces.
#include <windows.h>
#endif

#include "blocking.h"
#include "bundle.h"
#include "csv.h"
#include "env.h"
#include "golden.h"
#include "ingest.h"
#include "json_source.h"
#include "postgres.h"
#include "record.h"
#include "resolve_commands.h"
#include "serve_commands.h"
#include "sextant/lsm/options.h"
#include "sextant/lsm/status.h"
#include "store.h"

namespace {

namespace onto = sextant::ontology;
namespace conn = sextant::connectors;
namespace codec = sextant::codec;
namespace lsm = sextant::lsm;
namespace resolve = sextant::resolve;

using lsm::Status;

struct Args {
  std::string command;
  std::map<std::string, std::string> flags;
  std::vector<std::string> positional;

  bool Has(const std::string& name) const { return flags.count(name) != 0; }
  std::string Get(const std::string& name, const std::string& fallback = {}) const {
    const auto it = flags.find(name);
    return it == flags.end() ? fallback : it->second;
  }
  uint64_t GetU64(const std::string& name, uint64_t fallback) const {
    const auto it = flags.find(name);
    if (it == flags.end() || it->second.empty()) return fallback;
    return std::strtoull(it->second.c_str(), nullptr, 10);
  }
};

// Accepts `--name value`, `--name=value` and bare `--flag`.
Args ParseArgs(int argc, char** argv) {
  Args args;
  if (argc > 1) args.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--", 0) != 0) {
      args.positional.push_back(std::move(arg));
      continue;
    }
    arg = arg.substr(2);
    const size_t eq = arg.find('=');
    if (eq != std::string::npos) {
      args.flags[arg.substr(0, eq)] = arg.substr(eq + 1);
      continue;
    }
    if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
      args.flags[arg] = argv[++i];
    } else {
      args.flags[arg] = "";
    }
  }
  return args;
}

void Usage() {
  std::printf(R"(sextant - ontology, entity resolution and cell-level lineage

usage:
  sextant schema  [--schema DIR]
      Load and validate the ontology and every mapping, then print them.

  sextant ingest  --source ID [--db DIR] [--schema DIR] [--limit N] [--force]
      Read a source and write RAW and SRCREC records. Re-ingesting an
      unchanged input is detected and skipped unless --force is given.

  sextant stats   [--db DIR] [--schema DIR]
      Record counts per source, and the batches behind them.

  sextant block   [--db DIR] [--schema DIR] [--eval DIR] [--max-block N]
      Build the blocking index and report reduction ratio and pair
      completeness against the golden sets in eval/.

  sextant eval    [--db DIR] [--schema DIR] [--eval DIR] [--tune]
      Score the golden set and report the confusion matrix, precision,
      recall and F1 on a held-out split. --tune fits the weights on the
      training split only and prints them for schema/resolver.yaml.

  sextant resolve [--db DIR] [--schema DIR] [--eval DIR] [--dry-run]
      Score every candidate pair, cluster it both ways, and write the
      resolved entities with their provenance.

  sextant explain [--db DIR] [--entity ULID --type NAME] [--verify]
      With no --entity, runs the lineage round-trip over every property of
      every entity: read the provenance, fetch the raw row it names, replay
      the transform chain, assert the result equals the stored value.
      With --entity, shows the full chain for one entity's properties.

  sextant lineage --source ID --batch N --row N [--db DIR]
      Print the verbatim source row a lineage reference points at.

  sextant query   --type NAME [--where prop=value] [--link NAME [--reverse]]
                  [--from ISO8601] [--to ISO8601] [--limit N] [--show N]
                  [--json '{...}'] [--db DIR] [--schema DIR]
      Run a traversal and print the plan, the results and what it cost.
      --json takes the same body POST /api/traverse does.

  sextant serve   [--host H] [--port N] [--static DIR] [--cors ORIGIN]
                  [--no-cors] [--data-root DIR] [--db DIR] [--schema DIR]
      Serve the query API over HTTP.

defaults:
  --db      ./sextant-db
  --schema  ./schema
  --host    127.0.0.1
  --port    8080
)");
}

// The engine creates its own directory, but not the path leading to it: a
// storage engine has no business doing recursive mkdir on a path it was handed.
// The command line does, because that is where a path a person typed arrives,
// and `--db /tmp/demo/db` failing with "no such file or directory" is a poor
// first impression.
Status EnsureParents(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] != '/' && path[i] != '\\') continue;
    const std::string parent = path.substr(0, i);
    if (parent.empty() || lsm::FileExists(parent)) continue;
    const Status s = lsm::CreateDir(parent);
    // A race with another process creating the same directory is fine; the
    // subsequent Open will fail properly if it really is not there.
    if (!s.ok() && !lsm::FileExists(parent)) return s;
  }
  return Status::OK();
}

Status OpenStore(const Args& args, std::unique_ptr<codec::Store>* store) {
  const std::string path = args.Get("db", "sextant-db");
  const Status s = EnsureParents(path);
  if (!s.ok()) return s;

  lsm::Options options;
  options.create_if_missing = true;
  return codec::Store::Open(options, path, store);
}

// --- schema -----------------------------------------------------------------

int CmdSchema(const Args& args) {
  onto::SchemaBundle bundle;
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"), &bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return 1;
  }

  const onto::Ontology& o = bundle.ontology();
  std::printf("ontology  namespace=%s version=%d\n\n", o.name_space().c_str(),
              o.version());

  for (const auto& t : o.types()) {
    std::printf("  %s (id %u)\n", t.name.c_str(), t.id);
    for (const auto& p : t.properties) {
      std::printf("    %-14s id=%-3u %-10s%s%s%s fuse=%s\n", p.name.c_str(), p.id,
                  onto::ValueTypeName(p.type), p.title ? " title" : "",
                  p.indexed ? " indexed" : "", p.unique_hint ? " unique" : "",
                  onto::FusionRuleName(p.fuse));
    }
  }

  std::printf("\n  links\n");
  for (const auto& l : o.links()) {
    const auto* from = o.Type(l.from);
    const auto* to = o.Type(l.to);
    std::printf("    %-14s id=%-3u %s -> %s  %s%s\n", l.name.c_str(), l.id,
                from == nullptr ? "?" : from->name.c_str(),
                to == nullptr ? "?" : to->name.c_str(),
                onto::CardinalityName(l.cardinality),
                l.HasTimeIndex() ? "  [time-indexed]" : "");
  }

  std::printf("\n  transforms\n");
  for (const auto& t : bundle.transforms().All()) {
    std::printf("    0x%04X v%-2u %-22s %s\n", t.id, t.version, t.name, t.doc);
  }

  std::printf("\n  sources\n");
  for (const auto& src : bundle.sources()) {
    std::printf("    %-16s id=%-3u %-9s trust=%.2f  %s\n", src.key.c_str(), src.id,
                onto::ConnectorKindName(src.connector), src.trust,
                src.uri.empty() ? src.base_url.c_str() : src.uri.c_str());
    for (const auto& m : src.mappings) {
      std::printf("      -> %-8s %zu properties, %zu links%s\n",
                  m.target_type_name.c_str(), m.properties.size(), m.links.size(),
                  m.from_endpoint.empty() ? ""
                                          : (" from " + m.from_endpoint).c_str());
    }
  }
  return 0;
}

// --- ingest -----------------------------------------------------------------

void PrintResult(const conn::Ingestor::Result& r, const std::string& label) {
  if (r.skipped) {
    std::printf("  %-24s unchanged since batch %llu, nothing written\n",
                label.c_str(), static_cast<unsigned long long>(r.existing_batch));
    return;
  }
  const auto& m = r.manifest;
  std::printf(
      "  %-24s batch %-3llu  %llu rows -> %llu records, %llu values"
      " (%llu rejected, %llu filtered) in %lld ms\n",
      label.c_str(), static_cast<unsigned long long>(r.batch),
      static_cast<unsigned long long>(m.rows_read),
      static_cast<unsigned long long>(m.records_written),
      static_cast<unsigned long long>(m.properties_written),
      static_cast<unsigned long long>(m.properties_rejected),
      static_cast<unsigned long long>(m.rows_filtered),
      static_cast<long long>(m.finished_ms - m.started_ms));
}

int CmdIngest(const Args& args) {
  const std::string source_key = args.Get("source");
  if (source_key.empty()) {
    std::fprintf(stderr, "ingest: --source is required\n");
    return 2;
  }

  onto::SchemaBundle bundle;
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"), &bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return 1;
  }
  const onto::SourceSpec* spec = bundle.Source(source_key);
  if (spec == nullptr) {
    std::fprintf(stderr, "ingest: no source \"%s\"; try `sextant schema`\n",
                 source_key.c_str());
    return 2;
  }

  std::unique_ptr<codec::Store> store;
  s = OpenStore(args, &store);
  if (!s.ok()) {
    std::fprintf(stderr, "store: %s\n", s.ToString().c_str());
    return 1;
  }

  conn::Ingestor ingestor(store.get(), &bundle.ontology(), &bundle.transforms());
  conn::Ingestor::Options options;
  options.force = args.Has("force");
  options.limit = args.GetU64("limit", 0);

  std::printf("ingest %s (%s)\n", spec->key.c_str(),
              onto::ConnectorKindName(spec->connector));

  switch (spec->connector) {
    case onto::ConnectorKind::kCsv: {
      uint64_t fingerprint = 0;
      s = conn::Ingestor::FingerprintFile(spec->uri, &fingerprint);
      if (!s.ok()) {
        std::fprintf(stderr, "  %s\n  run data/fetch.sh to download the source\n",
                     s.ToString().c_str());
        return 1;
      }
      std::unique_ptr<conn::CsvReader> reader;
      s = conn::CsvReader::Open(spec->uri, &reader);
      if (!s.ok()) {
        std::fprintf(stderr, "  %s\n", s.ToString().c_str());
        return 1;
      }
      conn::Ingestor::Result result;
      s = ingestor.Run(*spec, reader.get(), fingerprint, options, &result);
      if (!s.ok()) {
        std::fprintf(stderr, "  %s\n", s.ToString().c_str());
        return 1;
      }
      PrintResult(result, spec->uri);
      break;
    }

    case onto::ConnectorKind::kHttp: {
      // One batch per endpoint. A batch is one input stream, and five endpoints
      // sharing a batch id would make the row numbers in RAW ambiguous.
      conn::SnapshotFetcher fetcher(spec->snapshot_dir);
      bool any = false;
      for (const auto& endpoint : spec->endpoints) {
        bool used = false;
        for (const auto& m : spec->mappings) {
          if (m.from_endpoint == endpoint.id) used = true;
        }
        if (!used) continue;

        std::string body;
        s = fetcher.Fetch(endpoint.id, endpoint.path, &body);
        if (!s.ok()) {
          std::fprintf(stderr, "  %-24s %s\n", endpoint.id.c_str(),
                       s.ToString().c_str());
          continue;
        }
        std::unique_ptr<conn::JsonRowSource> rows;
        s = conn::JsonRowSource::Open(body, endpoint.records_at, endpoint.id, &rows);
        if (!s.ok()) {
          std::fprintf(stderr, "  %-24s %s\n", endpoint.id.c_str(),
                       s.ToString().c_str());
          continue;
        }
        conn::Ingestor::Options endpoint_options = options;
        endpoint_options.uri_label = spec->base_url + endpoint.path;
        conn::Ingestor::Result result;
        s = ingestor.Run(*spec, rows.get(),
                         conn::Ingestor::FingerprintBytes(body), endpoint_options,
                         &result);
        if (!s.ok()) {
          std::fprintf(stderr, "  %-24s %s\n", endpoint.id.c_str(),
                       s.ToString().c_str());
          return 1;
        }
        PrintResult(result, endpoint.id);
        any = true;
      }
      if (!any) {
        std::fprintf(stderr,
                     "  no recorded responses under %s\n"
                     "  see the note in schema/mappings/%s.yaml\n",
                     spec->snapshot_dir.c_str(), spec->key.c_str());
        return 1;
      }
      break;
    }

    case onto::ConnectorKind::kPostgres: {
      std::unique_ptr<conn::PostgresSource> rows;
      s = conn::PostgresSource::Open(spec->dsn, spec->query, spec->params, &rows);
      if (!s.ok()) {
        std::fprintf(stderr, "  %s\n", s.ToString().c_str());
        return 1;
      }
      // A query cannot be hashed before it is run, so the idempotency check is
      // off for this connector and every run is a new batch. Saying so is
      // better than pretending otherwise.
      conn::Ingestor::Result result;
      s = ingestor.Run(*spec, rows.get(), /*content_fingerprint=*/0, options,
                       &result);
      if (!s.ok()) {
        std::fprintf(stderr, "  %s\n", s.ToString().c_str());
        return 1;
      }
      PrintResult(result, spec->dsn);
      break;
    }
  }
  return 0;
}

// --- stats ------------------------------------------------------------------

int CmdStats(const Args& args) {
  onto::SchemaBundle bundle;
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"), &bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return 1;
  }
  std::unique_ptr<codec::Store> store;
  s = OpenStore(args, &store);
  if (!s.ok()) {
    std::fprintf(stderr, "store: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("%-16s %8s %10s %10s %8s\n", "source", "batches", "raw", "records",
              "latest");
  uint64_t total_raw = 0, total_records = 0;
  for (const auto& spec : bundle.sources()) {
    conn::SourceStats stats;
    s = conn::CollectSourceStats(store.get(), spec, &stats);
    if (!s.ok()) {
      std::fprintf(stderr, "stats: %s\n", s.ToString().c_str());
      return 1;
    }
    std::printf("%-16s %8llu %10llu %10llu %8llu\n", stats.key.c_str(),
                static_cast<unsigned long long>(stats.batches),
                static_cast<unsigned long long>(stats.raw_records),
                static_cast<unsigned long long>(stats.source_records),
                static_cast<unsigned long long>(stats.latest_batch));
    total_raw += stats.raw_records;
    total_records += stats.source_records;
  }
  std::printf("%-16s %8s %10llu %10llu\n", "total", "",
              static_cast<unsigned long long>(total_raw),
              static_cast<unsigned long long>(total_records));
  return 0;
}

// --- block ------------------------------------------------------------------

int CmdBlock(const Args& args) {
  onto::SchemaBundle bundle;
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"), &bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return 1;
  }
  std::unique_ptr<codec::Store> store;
  s = OpenStore(args, &store);
  if (!s.ok()) {
    std::fprintf(stderr, "store: %s\n", s.ToString().c_str());
    return 1;
  }

  resolve::ResolverProperties props;
  s = resolve::ResolverProperties::Resolve(bundle.ontology(), &props);
  if (!s.ok()) {
    std::fprintf(stderr, "resolver: %s\n", s.ToString().c_str());
    return 1;
  }

  resolve::Blocker blocker(store.get(), &bundle, &props);
  resolve::BlockingReport report;

  s = blocker.IndexAll(&report);
  if (!s.ok()) {
    std::fprintf(stderr, "index: %s\n", s.ToString().c_str());
    return 1;
  }

  resolve::Blocker::Options options;
  options.max_block_size = static_cast<size_t>(args.GetU64("max-block", 200));

  std::vector<resolve::CandidatePairRef> pairs;
  s = blocker.GenerateCandidates(options, &pairs, &report);
  if (!s.ok()) {
    std::fprintf(stderr, "candidates: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("blocking\n");
  std::printf("  %llu records indexed, %llu keys written\n",
              static_cast<unsigned long long>(report.records_indexed),
              static_cast<unsigned long long>(report.keys_written));
  std::printf("  %llu blocks, largest %llu, %llu purged over %zu\n",
              static_cast<unsigned long long>(report.blocks),
              static_cast<unsigned long long>(report.largest_block),
              static_cast<unsigned long long>(report.purged_blocks),
              options.max_block_size);
  std::printf("  %llu candidate pairs out of %llu possible\n",
              static_cast<unsigned long long>(report.candidate_pairs),
              static_cast<unsigned long long>(report.possible_pairs));
  std::printf("  reduction ratio    %.5f\n", report.reduction_ratio);

  // Both golden sets, if they are there. A missing one is not an error - the
  // blocking itself does not need labels, and production would not have any.
  const std::string eval_dir = args.Get("eval", "eval");
  bool measured = false;
  for (const char* file : {"golden_ports.csv", "golden_vessels.csv"}) {
    resolve::GoldenSet golden;
    const std::string path = eval_dir + "/" + file;
    const Status gs = resolve::GoldenSet::LoadFromFile(path, bundle, &golden);
    if (!gs.ok()) {
      std::fprintf(stderr, "  %s: %s\n", file, gs.ToString().c_str());
      continue;
    }
    resolve::BlockingReport measured_report = report;
    resolve::MeasureAgainstGolden(pairs, golden, options.max_missed_reported,
                                  &measured_report);
    measured = true;

    std::printf("\n  %s: %llu labeled pairs, %llu matches\n", file,
                static_cast<unsigned long long>(golden.pairs().size()),
                static_cast<unsigned long long>(golden.match_count()));
    std::printf("  pair completeness  %.4f  (%llu of %llu true pairs blocked"
                " together)\n",
                measured_report.pair_completeness,
                static_cast<unsigned long long>(
                    measured_report.golden_matches_covered),
                static_cast<unsigned long long>(measured_report.golden_matches));

    std::printf("\n  %-16s %10s %10s %8s\n", "key", "candidates", "true pairs",
                "unique");
    for (const auto& [key, count] : measured_report.pairs_by_key) {
      const auto matched = measured_report.matches_by_key.find(key);
      const auto unique = measured_report.unique_matches_by_key.find(key);
      std::printf("  %-16s %10llu %10llu %8llu\n", key.c_str(),
                  static_cast<unsigned long long>(count),
                  static_cast<unsigned long long>(
                      matched == measured_report.matches_by_key.end()
                          ? 0
                          : matched->second),
                  static_cast<unsigned long long>(
                      unique == measured_report.unique_matches_by_key.end()
                          ? 0
                          : unique->second));
    }

    if (!measured_report.missed.empty()) {
      // The recall nothing downstream can recover. Worth printing rather than
      // only counting: five of these usually name the key that is missing.
      std::printf("\n  true pairs no key produced:\n");
      for (const auto& [a, b] : measured_report.missed) {
        std::printf("    %s  <->  %s\n", a.c_str(), b.c_str());
      }
    }
  }
  if (!measured) {
    std::fprintf(stderr,
                 "\n  no golden set found under %s\n"
                 "  run `python3 eval/make_corpus.py` to build one\n",
                 eval_dir.c_str());
  }
  return 0;
}

// --- lineage ----------------------------------------------------------------

int CmdLineage(const Args& args) {
  onto::SchemaBundle bundle;
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"), &bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return 1;
  }
  const onto::SourceSpec* spec = bundle.Source(args.Get("source"));
  if (spec == nullptr) {
    std::fprintf(stderr, "lineage: --source is required\n");
    return 2;
  }

  std::unique_ptr<codec::Store> store;
  s = OpenStore(args, &store);
  if (!s.ok()) {
    std::fprintf(stderr, "store: %s\n", s.ToString().c_str());
    return 1;
  }

  const auto batch = args.GetU64("batch", 1);
  const auto row = args.GetU64("row", 1);
  std::string raw;
  s = store->GetRawRecord(spec->id, batch, row, &raw);
  if (!s.ok()) {
    std::fprintf(stderr, "lineage: %s\n", s.ToString().c_str());
    return 1;
  }
  // This is the far end of every lineage answer: the bytes the source actually
  // produced, unchanged since the day they were read.
  std::printf("RAW %s batch %llu row %llu\n%s\n", spec->key.c_str(),
              static_cast<unsigned long long>(batch),
              static_cast<unsigned long long>(row), raw.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // Every string this program prints is UTF-8: source rows come off disk as
  // bytes and are written out unchanged, which is the whole point of a lineage
  // view. A Windows console defaults to a legacy code page, so "Göteborg"
  // renders as "G├╢teborg" - the bytes are right and the terminal is guessing
  // wrong. Setting the output code page once at startup fixes the display
  // without touching the data, which is the correct place to fix it: the
  // encoding of a source row is not something a CLI should be re-interpreting.
  SetConsoleOutputCP(CP_UTF8);
#endif

  const Args args = ParseArgs(argc, argv);
  if (args.command.empty() || args.command == "help" || args.Has("help")) {
    Usage();
    return args.command.empty() ? 2 : 0;
  }
  if (args.command == "schema") return CmdSchema(args);
  if (args.command == "ingest") return CmdIngest(args);
  if (args.command == "stats") return CmdStats(args);
  if (args.command == "block") return CmdBlock(args);
  if (args.command == "eval" || args.command == "resolve" ||
      args.command == "explain" || args.command == "query" ||
      args.command == "serve") {
    // These live in their own translation units - between them they are longer
    // than the rest of the CLI put together.
    sextant::cli::Args forwarded;
    forwarded.command = args.command;
    forwarded.flags = args.flags;
    forwarded.positional = args.positional;
    if (args.command == "eval") return sextant::cli::CmdEval(forwarded);
    if (args.command == "resolve") return sextant::cli::CmdResolve(forwarded);
    if (args.command == "query") return sextant::cli::CmdQuery(forwarded);
    if (args.command == "serve") return sextant::cli::CmdServe(forwarded);
    return sextant::cli::CmdExplain(forwarded);
  }
  if (args.command == "lineage") return CmdLineage(args);

  std::fprintf(stderr, "unknown command \"%s\"\n\n", args.command.c_str());
  Usage();
  return 2;
}
