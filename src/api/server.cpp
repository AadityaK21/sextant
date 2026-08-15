#include "server.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>

#include "json.h"
#include "keyspace.h"
#include "plan.h"
#include "query.h"

namespace sextant::api {
namespace {

using codec::Ulid;
using query::Executor;
using query::Query;
using query::QueryResult;

// Map an engine Status onto an HTTP code.
//
// The distinction that matters is 400 versus 500. An unknown property is the
// client's mistake and must not be reported as a server fault, or the client
// retries a request that can never work. A decode failure is the server's
// problem and must not be reported as the client's, or a corrupt database looks
// like a bad request forever.
int StatusToHttp(const Status& status) {
  if (status.ok()) return 200;
  if (status.IsNotFound()) return 404;
  if (status.IsInvalidArgument()) return 400;
  return 500;
}

const char* StatusCode(const Status& status) {
  if (status.IsNotFound()) return "not_found";
  if (status.IsInvalidArgument()) return "invalid_argument";
  if (status.IsCorruption()) return "corruption";
  return "internal";
}

void SendError(httplib::Response* res, const Status& status) {
  res->status = StatusToHttp(status);
  res->set_content(ErrorToJson(StatusCode(status), status.ToString()).dump(2),
                   "application/json");
}

void SendJson(httplib::Response* res, const json& body) {
  res->set_content(body.dump(2), "application/json");
}

}  // namespace

struct Server::Impl {
  codec::Store* store;
  const ontology::SchemaBundle* bundle;
  ServerOptions options;

  Executor executor;
  lineage::LineageReader lineage;

  httplib::Server http;
  std::thread thread;
  std::atomic<bool> running{false};

  Impl(codec::Store* s, const ontology::SchemaBundle* b, ServerOptions o)
      : store(s),
        bundle(b),
        options(std::move(o)),
        executor(s, b),
        lineage(s, b, options.data_root) {}

  // An entity id on the wire carries no type, but an ENTITY key is
  // type-then-id. Rather than add a second index, try each declared type.
  //
  // That sounds worse than it is: each attempt is a point lookup whose bloom
  // filter answers "definitely not" without reading a data block, so the cost
  // of the misses is a few bit tests. With six types in the ontology the
  // arithmetic is comfortable; with six hundred it would not be, and the fix
  // would be to put the type in the URL.
  Status FindEntity(const Ulid& id, const codec::ReadContext& ctx,
                    codec::TypeId* type) const {
    for (const auto& candidate : bundle->ontology().types()) {
      std::string payload;
      Status s = store->GetEntity(candidate.id, id, &payload, ctx);
      if (s.ok()) {
        *type = candidate.id;
        return Status::OK();
      }
      if (!s.IsNotFound()) return s;
    }
    return Status::NotFound("no entity with id " + id.ToString());
  }

  void Register();
  void RegisterOntology();
  void RegisterEntities();
  void RegisterTraverse();
  void RegisterLineage();
  void RegisterReview();
  void RegisterStats();
};

void Server::Impl::RegisterOntology() {
  // The schema itself. The frontend renders types it was never compiled
  // against by reading this, which is the whole reason the ontology is data
  // rather than C++ structs.
  http.Get("/api/ontology", [this](const httplib::Request&, httplib::Response& res) {
    SendJson(&res, OntologyToJson(bundle->ontology()));
  });
}

void Server::Impl::RegisterEntities() {
  // Search. `q` is a prefix on the type's title property, which is the one
  // shape a string index can actually serve as a range.
  http.Get("/api/entities", [this](const httplib::Request& req,
                                   httplib::Response& res) {
    Query query;
    query.start.type = req.has_param("type") ? req.get_param_value("type") : "";
    if (query.start.type.empty()) {
      SendError(&res, Status::InvalidArgument("type is required"));
      return;
    }
    const auto* type = bundle->ontology().Type(query.start.type);
    if (type == nullptr) {
      SendError(&res, Status::InvalidArgument("unknown type " + query.start.type));
      return;
    }

    if (req.has_param("q") && !req.get_param_value("q").empty()) {
      const auto* title = type->TitleProperty();
      if (title == nullptr) {
        SendError(&res, Status::InvalidArgument(query.start.type +
                                                " has no title property to search"));
        return;
      }
      query::Predicate pred;
      pred.property = title->name;
      pred.op = query::CompareOp::kStartsWith;
      pred.value = ontology::TValue::String(req.get_param_value("q"));
      query.start.filter.push_back(pred);
    }

    // Any other query parameter is an equality filter on a property of that
    // type. Unknown parameters are rejected rather than ignored, because a
    // typo that silently widens a filter is a bug that looks like a data
    // problem.
    for (const auto& [key, value] : req.params) {
      if (key == "type" || key == "q" || key == "limit") continue;
      if (type->Property(key) == nullptr) {
        SendError(&res, Status::InvalidArgument("unknown parameter or property: " + key));
        return;
      }
      query::Predicate pred;
      pred.property = key;
      pred.op = query::CompareOp::kEq;
      pred.value = ontology::TValue::String(value);
      query.start.filter.push_back(pred);
    }

    if (req.has_param("limit")) {
      query.limit = std::strtoull(req.get_param_value("limit").c_str(), nullptr, 10);
      if (query.limit == 0 || query.limit > 10000) {
        SendError(&res, Status::InvalidArgument("limit must be between 1 and 10000"));
        return;
      }
    }

    QueryResult result;
    Status s = executor.Run(query, &result);
    if (!s.ok()) {
      SendError(&res, s);
      return;
    }
    SendJson(&res, ResultToJson(result));
  });

  // One entity, with a provenance summary.
  http.Get(R"(/api/entities/([0-9A-HJKMNP-TV-Z]{26}))",
           [this](const httplib::Request& req, httplib::Response& res) {
             Ulid id;
             if (!Ulid::FromString(req.matches[1], &id)) {
               SendError(&res, Status::InvalidArgument("not a valid entity id"));
               return;
             }

             codec::SnapshotHandle snapshot = store->NewSnapshot();
             lsm::ReadStats stats;
             codec::ReadContext ctx;
             ctx.snapshot = snapshot.get();
             ctx.stats = &stats;

             codec::TypeId type = 0;
             Status s = FindEntity(id, ctx, &type);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }

             query::ResultEntity entity;
             s = executor.Materialise(type, id, ctx, &entity);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }

             json body = EntityToJson(entity);

             // A summary rather than the full chain: which source won each
             // property and how confident fusion was. The full chain is one
             // more request away at /api/lineage, and sending it here would
             // make the common case pay for the rare one.
             std::vector<lineage::Explanation> explanations;
             if (lineage.ExplainAll(type, id, &explanations).ok()) {
               json provenance = json::object();
               for (const auto& explanation : explanations) {
                 provenance[explanation.property_name] = {
                     {"source", explanation.source_key},
                     {"column", explanation.column},
                     {"rule", explanation.rule},
                     {"confidence", explanation.confidence},
                     {"rejected", explanation.rejected.size()},
                     {"cluster_size", explanation.cluster_size},
                     {"verified", explanation.replay_matches}};
               }
               body["_provenance"] = provenance;
             }

             query::QueryCost cost;
             cost.keys_scanned = stats.keys_scanned;
             cost.blocks_read = stats.blocks_read;
             cost.block_cache_hits = stats.block_cache_hits;
             cost.bloom_rejections = stats.bloom_rejections;
             cost.sstables_probed = stats.sstables_probed;
             cost.memtable_hits = stats.memtable_hits;
             cost.index_used = query::AccessPath::kPointLookup;
             body["_stats"] = CostToJson(cost);
             SendJson(&res, body);
           });

  // The links of one entity: a one-hop traverse in disguise.
  http.Get(R"(/api/entities/([0-9A-HJKMNP-TV-Z]{26})/links)",
           [this](const httplib::Request& req, httplib::Response& res) {
             Ulid id;
             if (!Ulid::FromString(req.matches[1], &id)) {
               SendError(&res, Status::InvalidArgument("not a valid entity id"));
               return;
             }
             if (!req.has_param("type")) {
               SendError(&res, Status::InvalidArgument("type (the link name) is required"));
               return;
             }

             codec::SnapshotHandle snapshot = store->NewSnapshot();
             codec::ReadContext ctx;
             ctx.snapshot = snapshot.get();

             codec::TypeId entity_type = 0;
             Status s = FindEntity(id, ctx, &entity_type);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }
             const auto* from = bundle->ontology().Type(entity_type);

             Query query;
             query.start.type = from != nullptr ? from->name : "";
             query.start.ids.push_back(id.ToString());

             query::Hop hop;
             hop.link = req.get_param_value("type");
             // Either name works: "arrives_at" from a Voyage, or its declared
             // inverse "arrivals" from a Port. The planner works the direction
             // out from which name was used, so the client never has to
             // re-derive the schema to ask a natural question.
             if (bundle->ontology().LinkOrInverse(hop.link, nullptr) == nullptr) {
               SendError(&res, Status::InvalidArgument("unknown link " + hop.link));
               return;
             }
             query.hops.push_back(hop);

             if (req.has_param("limit")) {
               query.limit = std::strtoull(req.get_param_value("limit").c_str(),
                                           nullptr, 10);
               if (query.limit == 0 || query.limit > 10000) {
                 SendError(&res, Status::InvalidArgument("limit must be 1 to 10000"));
                 return;
               }
             }

             QueryResult result;
             s = executor.Run(query, &result);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }
             SendJson(&res, ResultToJson(result));
           });
}

void Server::Impl::RegisterTraverse() {
  http.Post("/api/traverse", [this](const httplib::Request& req,
                                    httplib::Response& res) {
    Query query;
    Status s = Query::FromJson(req.body, &query);
    if (!s.ok()) {
      SendError(&res, s);
      return;
    }
    QueryResult result;
    s = executor.Run(query, &result);
    if (!s.ok()) {
      SendError(&res, s);
      return;
    }
    SendJson(&res, ResultToJson(result));
  });

  // Plan without running. Useful on its own and useful in a demo: it shows the
  // index choice for a query too expensive to actually execute.
  http.Post("/api/explain", [this](const httplib::Request& req,
                                   httplib::Response& res) {
    Query query;
    Status s = Query::FromJson(req.body, &query);
    if (!s.ok()) {
      SendError(&res, s);
      return;
    }
    query::Plan plan;
    query::Planner planner(bundle);
    s = planner.Build(query, &plan);
    if (!s.ok()) {
      SendError(&res, s);
      return;
    }
    SendJson(&res, {{"query", query.Describe()}, {"_plan", PlanToJson(plan)}});
  });
}

void Server::Impl::RegisterLineage() {
  http.Get(R"(/api/lineage/([0-9A-HJKMNP-TV-Z]{26})/([A-Za-z0-9_]+))",
           [this](const httplib::Request& req, httplib::Response& res) {
             Ulid id;
             if (!Ulid::FromString(req.matches[1], &id)) {
               SendError(&res, Status::InvalidArgument("not a valid entity id"));
               return;
             }
             const std::string property = req.matches[2];

             codec::TypeId type = 0;
             Status s = FindEntity(id, codec::ReadContext{}, &type);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }
             const auto* type_def = bundle->ontology().Type(type);
             const auto* prop = type_def != nullptr ? type_def->Property(property)
                                                    : nullptr;
             if (prop == nullptr) {
               SendError(&res, Status::InvalidArgument(
                                   "no property " + property + " on " +
                                   (type_def != nullptr ? type_def->name : "?")));
               return;
             }

             lineage::Explanation explanation;
             s = lineage.Explain(type, id, prop->id, &explanation);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }
             SendJson(&res, ExplanationToJson(explanation));
           });

  // The verbatim original row. The far end of every lineage chain, served as
  // bytes rather than as a parsed structure, because the point of it is that
  // it is what the source actually said.
  http.Get(R"(/api/raw/([A-Za-z0-9_-]+)/(\d+)/(\d+))",
           [this](const httplib::Request& req, httplib::Response& res) {
             const std::string source_key = req.matches[1];
             const auto* spec = bundle->Source(source_key);
             if (spec == nullptr) {
               SendError(&res, Status::NotFound("no source called " + source_key));
               return;
             }
             const auto batch = static_cast<codec::BatchId>(
                 std::strtoull(std::string(req.matches[2]).c_str(), nullptr, 10));
             const auto row = static_cast<codec::RowSeq>(
                 std::strtoull(std::string(req.matches[3]).c_str(), nullptr, 10));

             std::string raw;
             Status s = store->GetRawRecord(spec->id, batch, row, &raw);
             if (!s.ok()) {
               SendError(&res, s);
               return;
             }
             SendJson(&res, {{"source", source_key},
                             {"batch", batch},
                             {"row", row},
                             {"raw", raw}});
           });
}

void Server::Impl::RegisterReview() {
  // The entity-resolution review queue, most uncertain first. That ordering is
  // free: CAND is keyed by inverted score, so a forward scan is already sorted
  // by how close the pair sat to the decision boundary.
  http.Get("/api/review", [this](const httplib::Request& req,
                                 httplib::Response& res) {
    uint64_t limit = 50;
    if (req.has_param("limit")) {
      limit = std::strtoull(req.get_param_value("limit").c_str(), nullptr, 10);
      if (limit == 0 || limit > 1000) {
        SendError(&res, Status::InvalidArgument("limit must be between 1 and 1000"));
        return;
      }
    }

    codec::SnapshotHandle snapshot = store->NewSnapshot();
    lsm::ReadStats stats;
    codec::ReadContext ctx;
    ctx.snapshot = snapshot.get();
    ctx.stats = &stats;

    json pairs = json::array();
    auto iter = store->ScanCandidates(ctx);
    for (; iter->Valid() && pairs.size() < limit; iter->Next()) {
      double score = 0.0;
      uint64_t pair_hash = 0;
      if (!codec::DecodeCandidateKey(iter->key(), &score, &pair_hash)) continue;
      pairs.push_back({{"pair_id", std::to_string(pair_hash)},
                       {"score", score},
                       {"explanation", iter->value().ToString()}});
    }
    if (!iter->status().ok()) {
      SendError(&res, iter->status());
      return;
    }

    query::QueryCost cost;
    cost.keys_scanned = stats.keys_scanned;
    cost.blocks_read = stats.blocks_read;
    cost.block_cache_hits = stats.block_cache_hits;
    cost.index_used = query::AccessPath::kEntityScan;
    SendJson(&res, {{"pairs", pairs}, {"count", pairs.size()}, {"_stats", CostToJson(cost)}});
  });

  // Accept or reject a pair.
  //
  // A DEPARTURE FROM THE ARCHITECTURE DOC, WHICH SAID "decision edge".
  //
  // A decision is about a PAIR OF SOURCE RECORDS, and a pair of source records
  // is not an entity, so it has no entity id for an edge to point at. Writing
  // one would mean inventing entities for decisions, which is a bigger idea
  // than this needs. The decision is recorded on the candidate record instead,
  // which keeps it exactly where the next resolver run will look for it.
  //
  // What this does NOT do is re-run resolution. Accepting a pair records the
  // human's answer; the merge happens on the next `sextant resolve`. Doing it
  // live would mean re-clustering and re-fusing inside a request handler, and
  // the veto-constrained clustering that makes the result trustworthy is a
  // whole-graph operation.
  http.Post(R"(/api/review/(\d+))", [this](const httplib::Request& req,
                                           httplib::Response& res) {
    const uint64_t pair_hash =
        std::strtoull(std::string(req.matches[1]).c_str(), nullptr, 10);

    json body;
    try {
      body = json::parse(req.body);
    } catch (const json::parse_error& e) {
      SendError(&res, Status::InvalidArgument(std::string("malformed JSON: ") + e.what()));
      return;
    }
    if (!body.contains("decision") || !body["decision"].is_string()) {
      SendError(&res, Status::InvalidArgument("body needs a 'decision' of accept or reject"));
      return;
    }
    const std::string decision = body["decision"].get<std::string>();
    if (decision != "accept" && decision != "reject") {
      SendError(&res, Status::InvalidArgument("decision must be accept or reject"));
      return;
    }

    // Find the candidate so the score can be preserved: the key encodes it, and
    // rewriting under a different score would move the record and leave the
    // original in place.
    double score = 0.0;
    std::string explanation;
    bool found = false;
    auto iter = store->ScanCandidates();
    for (; iter->Valid(); iter->Next()) {
      double candidate_score = 0.0;
      uint64_t candidate_hash = 0;
      if (!codec::DecodeCandidateKey(iter->key(), &candidate_score, &candidate_hash)) {
        continue;
      }
      if (candidate_hash == pair_hash) {
        score = candidate_score;
        explanation = iter->value().ToString();
        found = true;
        break;
      }
    }
    if (!found) {
      SendError(&res, Status::NotFound("no candidate pair " + std::to_string(pair_hash)));
      return;
    }

    const std::string reviewer =
        body.value("reviewer", std::string("anonymous"));
    const std::string recorded = explanation + "\nDECISION " + decision + " by " +
                                 reviewer + "\n";
    Status s = store->PutCandidate(score, pair_hash, lsm::Slice(recorded));
    if (!s.ok()) {
      SendError(&res, s);
      return;
    }
    SendJson(&res, {{"pair_id", std::to_string(pair_hash)},
                    {"decision", decision},
                    {"reviewer", reviewer},
                    {"applied", false},
                    {"note", "recorded; the merge happens on the next resolve run"}});
  });
}

void Server::Impl::RegisterStats() {
  http.Get("/api/stats", [this](const httplib::Request&, httplib::Response& res) {
    codec::SnapshotHandle snapshot = store->NewSnapshot();
    codec::ReadContext ctx;
    ctx.snapshot = snapshot.get();

    json counts = json::object();
    uint64_t total_entities = 0;
    for (const auto& type : bundle->ontology().types()) {
      uint64_t n = 0;
      auto iter = store->ScanEntities(type.id, ctx);
      for (; iter->Valid(); iter->Next()) ++n;
      counts[type.name] = n;
      total_entities += n;
    }

    uint64_t source_records = 0;
    for (const auto& source : bundle->sources()) {
      auto iter = store->ScanSourceRecords(source.id);
      for (; iter->Valid(); iter->Next()) ++source_records;
    }

    const lsm::Stats engine = store->db()->GetStats();
    json levels = json::array();
    for (int i = 0; i < 7; ++i) {
      if (engine.files_per_level[i] == 0 && engine.bytes_per_level[i] == 0) continue;
      levels.push_back({{"level", i},
                        {"files", engine.files_per_level[i]},
                        {"bytes", engine.bytes_per_level[i]}});
    }

    SendJson(&res,
             {{"entities", counts},
              {"total_entities", total_entities},
              {"source_records", source_records},
              // The FRACTION OF RECORDS REMOVED by merging, which is the same
              // definition `sextant resolve` prints.
              //
              // The first version of this route returned entities/records
              // instead - the complement. Both are defensible; having two of
              // them under one name is not, and a demo where the CLI says
              // 0.1819 and the API says 0.8181 for the same database is a
              // question you do not want to be answering live.
              {"dedup_ratio",
               source_records == 0
                   ? 0.0
                   : 1.0 - static_cast<double>(total_entities) /
                               static_cast<double>(source_records)},
              {"engine",
               {{"writes", engine.writes},
                {"reads", engine.reads},
                {"sstables", engine.num_sstables},
                {"bytes_on_disk", engine.total_bytes_on_disk},
                {"compactions", engine.compactions},
                {"trivial_moves", engine.trivial_moves},
                {"keys_dropped", engine.keys_dropped},
                {"write_stalls", engine.write_stalls},
                {"cache_hits", engine.cache_hits},
                {"cache_misses", engine.cache_misses},
                {"filter_rejections", engine.filter_rejections},
                {"range_rejections", engine.range_rejections},
                {"levels", levels}}}});
  });

  http.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    SendJson(&res, {{"status", "ok"}});
  });
}

void Server::Impl::Register() {
  if (!options.cors_origin.empty()) {
    http.set_post_routing_handler(
        [this](const httplib::Request&, httplib::Response& res) {
          res.set_header("Access-Control-Allow-Origin", options.cors_origin);
          res.set_header("Access-Control-Allow-Headers", "Content-Type");
          res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        });
    // The browser sends a preflight OPTIONS before any POST with a JSON body.
    // Without this every traverse fails in the browser and works in curl,
    // which is a confusing hour if you have not met it before.
    http.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
      res.status = 204;
    });
  }

  RegisterOntology();
  RegisterEntities();
  RegisterTraverse();
  RegisterLineage();
  RegisterReview();
  RegisterStats();

  if (!options.static_dir.empty()) {
    http.set_mount_point("/", options.static_dir);
  }

  http.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string what = "unknown";
        try {
          std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          what = e.what();
        } catch (...) {
        }
        res.status = 500;
        res.set_content(ErrorToJson("internal", what).dump(2), "application/json");
      });

  if (options.log_requests) {
    http.set_logger([](const httplib::Request& req, const httplib::Response& res) {
      std::printf("  %s %s -> %d\n", req.method.c_str(), req.path.c_str(), res.status);
      std::fflush(stdout);
    });
  }
}

Server::Server(codec::Store* store, const ontology::SchemaBundle* bundle,
               ServerOptions options)
    : impl_(std::make_unique<Impl>(store, bundle, std::move(options))) {
  impl_->Register();
}

Server::~Server() { Stop(); }

Status Server::Listen() {
  impl_->running = true;
  if (!impl_->http.listen(impl_->options.host, impl_->options.port)) {
    impl_->running = false;
    return Status::IOError("could not bind " + impl_->options.host + ":" +
                           std::to_string(impl_->options.port));
  }
  return Status::OK();
}

Status Server::StartBackground(int* bound_port) {
  // Port 0 asks the OS for a free one, which is what makes the tests safe to
  // run in parallel. A fixed test port is a flaky test waiting for a second
  // job on the same machine.
  const int port = impl_->http.bind_to_any_port(impl_->options.host);
  if (port < 0) {
    return Status::IOError("could not bind " + impl_->options.host);
  }
  if (bound_port != nullptr) *bound_port = port;
  impl_->options.port = port;
  impl_->running = true;
  impl_->thread = std::thread([this] { impl_->http.listen_after_bind(); });
  impl_->http.wait_until_ready();
  return Status::OK();
}

void Server::Stop() {
  if (!impl_ || !impl_->running.exchange(false)) return;
  impl_->http.stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace sextant::api
