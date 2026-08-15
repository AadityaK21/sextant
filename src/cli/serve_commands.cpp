#include "serve_commands.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bundle.h"
#include "env.h"
#include "execute.h"
#include "plan.h"
#include "query.h"
#include "server.h"
#include "sextant/lsm/status.h"
#include "store.h"
#include "value.h"

namespace sextant::cli {
namespace {

namespace onto = sextant::ontology;
using lsm::Status;

Status EnsureParents(const std::string& path) {
  std::string prefix;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '/' && path[i] != '\\') continue;
    const std::string parent = path.substr(0, i);
    if (parent.empty() || lsm::FileExists(parent)) continue;
    const Status s = lsm::CreateDir(parent);
    if (!s.ok() && !lsm::FileExists(parent)) return s;
  }
  return Status::OK();
}

bool Open(const Args& args, onto::SchemaBundle* bundle,
          std::unique_ptr<codec::Store>* store) {
  Status s = onto::SchemaBundle::LoadFromDir(args.Get("schema", "schema"), bundle);
  if (!s.ok()) {
    std::fprintf(stderr, "schema: %s\n", s.ToString().c_str());
    return false;
  }
  const std::string db = args.Get("db", "sextant-db");
  s = EnsureParents(db);
  if (s.ok()) {
    lsm::Options options;
    options.create_if_missing = true;
    s = codec::Store::Open(options, db, store);
  }
  if (!s.ok()) {
    std::fprintf(stderr, "store: %s\n", s.ToString().c_str());
    return false;
  }
  return true;
}

void PrintCost(const query::QueryCost& cost) {
  std::printf("\n  cost\n");
  std::printf("    index_used            %s\n",
              query::AccessPathName(cost.index_used));
  std::printf("    keys_scanned          %llu\n",
              static_cast<unsigned long long>(cost.keys_scanned));
  std::printf("    entities_materialised %llu\n",
              static_cast<unsigned long long>(cost.entities_materialised));
  std::printf("    blocks_read           %llu  (cache hits %llu)\n",
              static_cast<unsigned long long>(cost.blocks_read),
              static_cast<unsigned long long>(cost.block_cache_hits));
  std::printf("    bloom_rejections      %llu\n",
              static_cast<unsigned long long>(cost.bloom_rejections));
  std::printf("    elapsed_us            %llu\n",
              static_cast<unsigned long long>(cost.elapsed_us));
  if (cost.truncated) {
    std::printf("    TRUNCATED at %s\n", cost.truncated_at.c_str());
  }
}

}  // namespace

int CmdQuery(const Args& args) {
  onto::SchemaBundle bundle;
  std::unique_ptr<codec::Store> store;
  if (!Open(args, &bundle, &store)) return 1;

  query::Query query;

  // Two ways in. A JSON body is the same thing the HTTP route takes, so a
  // request can be developed here and pasted into the API unchanged. The flag
  // form is for the common case where typing JSON at a shell prompt is the
  // slowest part of asking the question.
  const std::string json_body = args.Get("json");
  if (!json_body.empty()) {
    const Status s = query::Query::FromJson(json_body, &query);
    if (!s.ok()) {
      std::fprintf(stderr, "query: %s\n", s.ToString().c_str());
      return 1;
    }
  } else {
    query.start.type = args.Get("type");
    if (query.start.type.empty()) {
      std::fprintf(stderr, "need --type NAME or --json '{...}'\n");
      return 2;
    }
    if (args.Has("where")) {
      // --where locode=NLRTM
      const std::string clause = args.Get("where");
      const size_t eq = clause.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "--where wants property=value\n");
        return 2;
      }
      query::Predicate pred;
      pred.property = clause.substr(0, eq);
      pred.op = query::CompareOp::kEq;
      pred.value = onto::TValue::String(clause.substr(eq + 1));
      query.start.filter.push_back(pred);
    }
    if (args.Has("link")) {
      query::Hop hop;
      hop.link = args.Get("link");
      hop.reverse = args.Has("reverse");
      const std::string from = args.Get("from");
      const std::string to = args.Get("to");
      if (!from.empty() || !to.empty()) {
        int64_t from_ms = 0, to_ms = 0;
        if (!from.empty() && !onto::ParseIso8601(from, &from_ms)) {
          std::fprintf(stderr, "--from is not an ISO 8601 timestamp: %s\n",
                       from.c_str());
          return 2;
        }
        if (!to.empty() && !onto::ParseIso8601(to, &to_ms)) {
          std::fprintf(stderr, "--to is not an ISO 8601 timestamp: %s\n", to.c_str());
          return 2;
        }
        hop.when.present = true;
        hop.when.from_inclusive = from.empty() ? INT64_MIN : from_ms;
        hop.when.to_exclusive = to.empty() ? INT64_MAX : to_ms;
      }
      query.hops.push_back(hop);
    }
    query.limit = args.GetU64("limit", 200);
  }

  query::Executor executor(store.get(), &bundle);
  query::QueryResult result;
  const Status s = executor.Run(query, &result);
  if (!s.ok()) {
    std::fprintf(stderr, "query: %s\n", s.ToString().c_str());
    return 1;
  }

  std::printf("\n%s\n\n", query.Describe().c_str());
  std::printf("  plan\n%s", result.plan.Render().c_str());

  std::printf("\n  %llu result(s)",
              static_cast<unsigned long long>(result.entities.size()));
  if (result.total_before_limit > result.entities.size()) {
    std::printf(" of %llu before the limit",
                static_cast<unsigned long long>(result.total_before_limit));
  }
  std::printf("\n");

  const uint64_t show = args.GetU64("show", 20);
  uint64_t shown = 0;
  for (const auto& entity : result.entities) {
    if (shown++ >= show) {
      std::printf("    ... %llu more\n",
                  static_cast<unsigned long long>(result.entities.size() - show));
      break;
    }
    std::printf("    %s  %s\n", entity.id.ToString().c_str(),
                entity.display.empty() ? entity.type_name.c_str()
                                       : entity.display.c_str());
  }

  PrintCost(result.cost);
  return 0;
}

int CmdServe(const Args& args) {
  onto::SchemaBundle bundle;
  std::unique_ptr<codec::Store> store;
  if (!Open(args, &bundle, &store)) return 1;

  api::ServerOptions options;
  options.host = args.Get("host", "127.0.0.1");
  options.port = static_cast<int>(args.GetU64("port", 8080));
  options.data_root = args.Get("data-root", ".");
  options.static_dir = args.Get("static");
  if (args.Has("no-cors")) options.cors_origin.clear();
  else options.cors_origin = args.Get("cors", "http://localhost:5173");

  api::Server server(store.get(), &bundle, options);

  std::printf("\nsextant serving on http://%s:%d\n", options.host.c_str(),
              options.port);
  std::printf("  GET  /api/ontology\n");
  std::printf("  GET  /api/entities?type=Port&q=rott\n");
  std::printf("  GET  /api/entities/{id}\n");
  std::printf("  GET  /api/entities/{id}/links?type=arrivals\n");
  std::printf("  POST /api/traverse\n");
  std::printf("  POST /api/explain\n");
  std::printf("  GET  /api/lineage/{id}/{property}\n");
  std::printf("  GET  /api/raw/{source}/{batch}/{row}\n");
  std::printf("  GET  /api/review?limit=50\n");
  std::printf("  POST /api/review/{pair_id}\n");
  std::printf("  GET  /api/stats\n");
  if (!options.static_dir.empty()) {
    std::printf("  GET  /  (static files from %s)\n", options.static_dir.c_str());
  }
  std::printf("\n");
  std::fflush(stdout);

  const Status s = server.Listen();
  if (!s.ok()) {
    std::fprintf(stderr, "serve: %s\n", s.ToString().c_str());
    return 1;
  }
  return 0;
}

}  // namespace sextant::cli
