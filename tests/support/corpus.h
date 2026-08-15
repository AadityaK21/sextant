// Build the whole pipeline once, so a test can ask questions of real output.
//
// WHY THIS IS SHARED RATHER THAN COPIED
//
// The lineage suite and the query suite both need the same thing: the committed
// corpus ingested, blocked, scored, clustered, fused, written and linked. That
// is about 150 lines of setup. Having it twice would mean a schema change fixes
// one suite and silently skews the other, and it would mean the two suites
// could disagree about what "the corpus" is while both passing.
//
// It also matters for what these tests are FOR. Neither suite is testing a
// function in isolation; both are asserting properties of the output of the
// real pipeline. A fixture that reimplemented any part of that pipeline would
// be testing the fixture.
//
// EVERY DATABASE NAME CARRIES THE PROCESS ID
//
// ctest -j4 runs the suites in parallel, in one working directory. The first
// version of this used a fixed directory name and produced eight segfaults that
// looked like a compaction bug and were two processes opening one database.

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "blocking.h"
#include "bundle.h"
#include "cluster.h"
#include "csv.h"
#include "env.h"
#include "fuse.h"
#include "ingest.h"
#include "json_source.h"
#include "link.h"
#include "record.h"
#include "scorer.h"
#include "store.h"

// DO NOT SORT THIS BLOCK. scripts/check_includes.py --fix reordered an #if/#else
// like this one twice and produced "#else without #if" both times.
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sextant::testsupport {

namespace conn = sextant::connectors;
namespace onto = sextant::ontology;

inline std::string SourceDir() { return std::string(SEXTANT_SOURCE_DIR); }

inline unsigned long ProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

// Everything one pipeline run produced.
struct Corpus {
  std::string dbname;
  std::unique_ptr<codec::Store> store;
  onto::SchemaBundle bundle;
  resolve::ResolverProperties props;
  resolve::LinkReport links;
  resolve::BlockingReport blocking;
  uint64_t source_records = 0;
  uint64_t entities_written = 0;

  void Destroy() {
    std::vector<std::string> children;
    if (lsm::GetChildren(dbname, &children).ok()) {
      for (const auto& child : children) {
        if (child == "." || child == "..") continue;
        lsm::RemoveFile(dbname + "/" + child);
      }
    }
    std::remove(dbname.c_str());
  }
};

// Returns false on the first failure rather than aborting, so the caller can
// use ASSERT_TRUE and get a line number in its own file.
inline bool BuildCorpus(const std::string& prefix, Corpus* out) {
  out->dbname = prefix + "_db_" + std::to_string(ProcessId());
  out->Destroy();

  lsm::Options options;
  options.create_if_missing = true;
  if (!codec::Store::Open(options, out->dbname, &out->store).ok()) return false;
  if (!onto::SchemaBundle::LoadFromDir(SourceDir() + "/schema", &out->bundle).ok()) {
    return false;
  }
  if (!resolve::ResolverProperties::Resolve(out->bundle.ontology(), &out->props).ok()) {
    return false;
  }

  conn::Ingestor ingestor(out->store.get(), &out->bundle.ontology(),
                          &out->bundle.transforms());

  for (const char* key : {"wpi", "unlocode"}) {
    const onto::SourceSpec* spec = out->bundle.Source(key);
    if (spec == nullptr) return false;
    std::unique_ptr<conn::CsvReader> reader;
    if (!conn::CsvReader::Open(SourceDir() + "/" + spec->uri, &reader).ok()) return false;
    conn::Ingestor::Result result;
    if (!ingestor.Run(*spec, reader.get(), 0, {}, &result).ok()) return false;
  }

  conn::SnapshotFetcher fetcher(SourceDir() + "/data/snapshots/digitraffic");
  const std::pair<const char*, const char*> endpoints[] = {
      {"digitraffic", "ports"},
      {"digitraffic", "vessel_details"},
      {"digitraffic_ais", "ais_vessels"},
  };
  for (const auto& [source_key, endpoint] : endpoints) {
    std::string body;
    if (!fetcher.Fetch(endpoint, "/", &body).ok()) return false;
    std::unique_ptr<conn::JsonRowSource> rows;
    if (!conn::JsonRowSource::Open(body, "", endpoint, &rows).ok()) return false;
    conn::Ingestor::Result result;
    if (!ingestor.Run(*out->bundle.Source(source_key), rows.get(), 0, {}, &result)
             .ok()) {
      return false;
    }
  }

  // Port calls: the Voyage entity, and the only thing in the corpus that
  // produces links or a time index. Without it the graph is empty and the
  // query tests would pass against nothing.
  {
    std::string body;
    if (!fetcher.Fetch("port_calls", "/", &body).ok()) return false;
    std::unique_ptr<conn::JsonRowSource> rows;
    if (!conn::JsonRowSource::Open(body, "portCalls", "port_calls", &rows).ok()) {
      return false;
    }
    conn::Ingestor::Result result;
    if (!ingestor.Run(*out->bundle.Source("digitraffic"), rows.get(), 0, {}, &result)
             .ok()) {
      return false;
    }
  }

  std::unordered_map<resolve::RecordRef, onto::SourceRecord, resolve::RecordRefHash>
      records;
  for (const auto& spec : out->bundle.sources()) {
    auto it = out->store->ScanSourceRecords(spec.id);
    for (; it->Valid(); it->Next()) {
      lsm::Slice value = it->value();
      onto::SourceRecord record;
      if (!onto::SourceRecord::DecodeFrom(&value, &record)) continue;
      records[resolve::RecordRef{record.source_id, record.natural_key_hash}] =
          std::move(record);
    }
  }
  out->source_records = records.size();

  resolve::ScorerConfig config;
  if (!resolve::ScorerConfig::LoadFromFile(SourceDir() + "/schema/resolver.yaml",
                                           &config)
           .ok()) {
    return false;
  }

  resolve::Blocker blocker(out->store.get(), &out->bundle, &out->props);
  if (!blocker.IndexAll(&out->blocking).ok()) return false;
  std::vector<resolve::CandidatePairRef> candidates;
  if (!blocker.GenerateCandidates({}, &candidates, &out->blocking).ok()) return false;

  const resolve::PairScorer scorer(&config, &out->props);
  std::vector<resolve::ScoredEdge> edges;
  for (const auto& candidate : candidates) {
    const auto a = records.find(candidate.pair.a);
    const auto b = records.find(candidate.pair.b);
    if (a == records.end() || b == records.end()) continue;
    const resolve::PairScore score = scorer.Score(a->second, b->second);
    resolve::ScoredEdge edge;
    edge.pair = candidate.pair;
    edge.score = score.score;
    edge.decision = score.decision;
    edge.vetoed = score.vetoed;
    edges.push_back(std::move(edge));
  }

  // Seeded from EVERY record, not just the ones blocking produced a pair for.
  // Voyages produce no blocking keys - they are resolved through their links
  // rather than by comparing attributes - so seeding from the edges alone
  // drops them and leaves the graph with no nodes to link.
  std::vector<resolve::RecordRef> all;
  all.reserve(records.size());
  for (const auto& [ref, record] : records) all.push_back(ref);

  const resolve::ClusterSet clusters = resolve::ClusterVetoConstrained(edges, all);
  const resolve::Fuser fuser(&out->bundle, &out->props);
  for (const auto& cluster : clusters.clusters) {
    std::vector<const onto::SourceRecord*> members;
    for (const auto& member : cluster) {
      const auto it = records.find(member);
      if (it != records.end()) members.push_back(&it->second);
    }
    if (members.empty()) continue;
    const resolve::ResolvedEntity entity = fuser.Fuse(members, cluster, {});
    if (!resolve::WriteEntity(out->store.get(), out->bundle, entity).ok()) return false;
    ++out->entities_written;
  }

  return resolve::ResolveLinks(out->store.get(), out->bundle, out->props, &out->links)
      .ok();
}

}  // namespace sextant::testsupport
