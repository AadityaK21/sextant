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

  std::printf("sextant LSM benchmark\n");
  std::printf("  entries      : %d\n", num);
  std::printf("  value size   : %d bytes\n", value_size);
  std::printf("  milestone    : day 1 (memtable + WAL, no SSTables)\n\n");

  DestroyDB(dbname);

  Options options;
  options.write_buffer_size = 64u * 1024 * 1024;

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

  // --- misses (this is where bloom filters will show up on day 3) ----------
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
  std::printf("  memtable bytes : %.2f MB\n",
              static_cast<double>(session.memtable_bytes) / (1024 * 1024));
  std::printf("  sequence       : %llu\n",
              static_cast<unsigned long long>(session.sequence));
  std::printf("  sequence after recovery : %llu  (must match)\n",
              static_cast<unsigned long long>(after.sequence));

  db.reset();
  DestroyDB(dbname);
  return 0;
}
