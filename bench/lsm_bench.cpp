// Storage engine benchmarks.
//
// Numbers from this program go straight into docs/BENCH.md and the README.
// Run it, record the output, and re-run it after each milestone so you can
// show what SSTables and compaction cost you. "Writes got 3x slower when I
// added leveled compaction, and here is why" is a far better interview answer
// than a single number with no history.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "env.h"
#include "sextant/lsm/db.h"

using namespace sextant::lsm;
using Clock = std::chrono::steady_clock;

namespace {

double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void DestroyDB(const std::string& name) {
  std::vector<std::string> children;
  if (GetChildren(name, &children).ok()) {
    for (const auto& c : children) {
      if (c == "." || c == "..") continue;
      RemoveFile(name + "/" + c);
    }
  }
  std::remove(name.c_str());
}

std::string MakeKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%016d", i);
  return std::string(buf);
}

void ReportThroughput(const char* name, int ops, double seconds, size_t bytes) {
  std::printf("%-28s %10.0f ops/sec  %8.2f MB/sec  (%d ops in %.3fs)\n", name,
              ops / seconds, (static_cast<double>(bytes) / (1024 * 1024)) / seconds,
              ops, seconds);
}

void ReportLatency(const char* name, std::vector<double>& samples_us) {
  std::sort(samples_us.begin(), samples_us.end());
  const auto pct = [&](double p) {
    const size_t idx = static_cast<size_t>(p * (samples_us.size() - 1));
    return samples_us[idx];
  };
  const double mean =
      std::accumulate(samples_us.begin(), samples_us.end(), 0.0) / samples_us.size();
  std::printf("%-28s mean %7.2f us   p50 %7.2f   p95 %7.2f   p99 %7.2f   max %7.2f\n",
              name, mean, pct(0.50), pct(0.95), pct(0.99), samples_us.back());
}

}  // namespace

int main(int argc, char** argv) {
  const int num = (argc > 1) ? std::atoi(argv[1]) : 200000;
  const int value_size = (argc > 2) ? std::atoi(argv[2]) : 100;
  const std::string dbname = "bench_db";

  // Deliberately small so the run actually flushes. A buffer large enough to
  // hold everything would benchmark the memtable and quietly report nothing
  // about the storage layer.
  Options options;
  options.write_buffer_size = 4u * 1024 * 1024;

  std::printf("sextant LSM benchmark\n");
  std::printf("  entries      : %d\n", num);
  std::printf("  value size   : %d bytes\n", value_size);
  std::printf("  write buffer : %.1f MB\n",
              static_cast<double>(options.write_buffer_size) / (1024 * 1024));
  std::printf("  bloom        : %d bits/key\n", options.bloom_bits_per_key);
  std::printf("  block cache  : %.1f MB\n",
              static_cast<double>(options.block_cache_size) / (1024 * 1024));
  std::printf("  milestone    : day 3 (bloom filters + block cache + range pruning)\n\n");

  DestroyDB(dbname);

  std::unique_ptr<DB> db;
  if (!DB::Open(options, dbname, &db).ok()) {
    std::fprintf(stderr, "failed to open db\n");
    return 1;
  }

  const std::string value(static_cast<size_t>(value_size), 'v');
  const size_t bytes_per_op = 16 + static_cast<size_t>(value_size);

  // --- sequential writes, no fsync ------------------------------------------
  {
    const auto start = Clock::now();
    for (int i = 0; i < num; ++i) {
      db->Put(WriteOptions{}, Slice(MakeKey(i)), Slice(value));
    }
    ReportThroughput("fillseq (sync=false)", num, SecondsSince(start),
                     static_cast<size_t>(num) * bytes_per_op);
  }

  // --- batched writes -------------------------------------------------------
  {
    static constexpr int kBatch = 1000;
    const auto start = Clock::now();
    for (int i = 0; i < num; i += kBatch) {
      WriteBatch batch;
      for (int j = 0; j < kBatch && i + j < num; ++j) {
        batch.Put(Slice(MakeKey(num + i + j)), Slice(value));
      }
      db->Write(WriteOptions{}, &batch);
    }
    ReportThroughput("fillbatch (1000/batch)", num, SecondsSince(start),
                     static_cast<size_t>(num) * bytes_per_op);
  }

  // --- durable writes: this is what an fsync actually costs ------------------
  {
    const int sync_ops = std::min(num, 2000);
    const auto start = Clock::now();
    for (int i = 0; i < sync_ops; ++i) {
      WriteOptions wo;
      wo.sync = true;
      db->Put(wo, Slice(MakeKey(2 * num + i)), Slice(value));
    }
    ReportThroughput("fillsync (sync=true)", sync_ops, SecondsSince(start),
                     static_cast<size_t>(sync_ops) * bytes_per_op);
  }

  // --- random point reads ---------------------------------------------------
  {
    std::mt19937 rnd(42);
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(num));

    const auto start = Clock::now();
    for (int i = 0; i < num; ++i) {
      const std::string key = MakeKey(static_cast<int>(rnd() % static_cast<unsigned>(num)));
      const auto t0 = Clock::now();
      std::string got;
      db->Get(ReadOptions{}, Slice(key), &got);
      samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    ReportThroughput("readrandom", num, SecondsSince(start),
                     static_cast<size_t>(num) * bytes_per_op);
    ReportLatency("readrandom latency", samples);
  }

  // --- misses ---------------------------------------------------------------
  //
  // This is the line day 3 is aimed at. Right now a miss must be proven absent
  // by probing EVERY L0 table, since nothing prunes them. Bloom filters turn
  // most of those probes into a bitmap check that never touches the disk, so
  // expect this number to move sharply.
  {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(num));
    const auto start = Clock::now();
    for (int i = 0; i < num; ++i) {
      const auto t0 = Clock::now();
      std::string got;
      db->Get(ReadOptions{}, Slice("absent" + std::to_string(i)), &got);
      samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }
    ReportThroughput("readmissing", num, SecondsSince(start), 0);
    ReportLatency("readmissing latency", samples);
  }

  // Capture before the reopen: Stats are per-session and reset on recovery.
  const Stats session = db->GetStats();

  // --- recovery -------------------------------------------------------------
  {
    db->SyncWAL();
    db.reset();

    const auto start = Clock::now();
    if (!DB::Open(options, dbname, &db).ok()) {
      std::fprintf(stderr, "reopen failed\n");
      return 1;
    }
    const double seconds = SecondsSince(start);
    const Stats s = db->GetStats();
    std::printf("\n%-28s %.3fs  (%llu WAL records, sequence %llu)\n", "recovery (WAL replay)",
                seconds, static_cast<unsigned long long>(s.wal_records_replayed),
                static_cast<unsigned long long>(s.sequence));
  }

  const Stats after = db->GetStats();
  std::printf("\nsession stats (before reopen)\n");
  std::printf("  writes         : %llu\n", static_cast<unsigned long long>(session.writes));
  std::printf("  reads          : %llu\n", static_cast<unsigned long long>(session.reads));
  std::printf("  bytes written  : %.2f MB\n",
              static_cast<double>(session.bytes_written) / (1024 * 1024));
  std::printf("  sequence       : %llu\n",
              static_cast<unsigned long long>(session.sequence));

  std::printf("\nstorage\n");
  std::printf("  flushes        : %llu\n",
              static_cast<unsigned long long>(session.flushes));
  std::printf("  bytes flushed  : %.2f MB\n",
              static_cast<double>(session.bytes_flushed) / (1024 * 1024));
  std::printf("  live sstables  : %llu\n",
              static_cast<unsigned long long>(session.num_sstables));
  std::printf("  memtable bytes : %.2f MB\n",
              static_cast<double>(session.memtable_bytes) / (1024 * 1024));

  std::printf("\nread path\n");
  std::printf("  memtable hits  : %llu\n",
              static_cast<unsigned long long>(session.memtable_hits));
  std::printf("  sstable hits   : %llu\n",
              static_cast<unsigned long long>(session.sstable_hits));
  std::printf("  sstables probed: %llu",
              static_cast<unsigned long long>(session.sstables_probed));
  if (session.reads > 0) {
    // Tables consulted per read. Today this rises with the number of L0 files,
    // because nothing prunes them. Bloom filters (day 3) and leveled
    // compaction (day 4) are both attacks on exactly this number.
    std::printf("   (%.2f per read)\n",
                static_cast<double>(session.sstables_probed) /
                    static_cast<double>(session.reads));
  } else {
    std::printf("\n");
  }

  std::printf("\nread-path filters (each rejection is a disk read avoided)\n");
  std::printf("  range rejects  : %llu\n",
              static_cast<unsigned long long>(session.range_rejections));
  std::printf("  bloom rejects  : %llu\n",
              static_cast<unsigned long long>(session.filter_rejections));
  std::printf("  cache hits     : %llu\n",
              static_cast<unsigned long long>(session.cache_hits));
  std::printf("  cache misses   : %llu\n",
              static_cast<unsigned long long>(session.cache_misses));
  if (session.cache_hits + session.cache_misses > 0) {
    std::printf("  cache hit rate : %.1f%%\n",
                100.0 * static_cast<double>(session.cache_hits) /
                    static_cast<double>(session.cache_hits + session.cache_misses));
  }
  std::printf("  cache evictions: %llu\n",
              static_cast<unsigned long long>(session.cache_evictions));
  std::printf("  cache bytes    : %.2f MB\n",
              static_cast<double>(session.cache_bytes) / (1024 * 1024));

  std::printf("\n  sequence after recovery : %llu  (must match %llu)\n",
              static_cast<unsigned long long>(after.sequence),
              static_cast<unsigned long long>(session.sequence));
  std::printf("  sstables after recovery : %llu\n",
              static_cast<unsigned long long>(after.num_sstables));

  db.reset();
  DestroyDB(dbname);
  return 0;
}
