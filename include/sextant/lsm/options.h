#pragma once

#include <cstddef>
#include <cstdint>

namespace sextant::lsm {

class Snapshot;
class Cache;
class BloomFilterPolicy;

struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = true;

  // Fail Open() if the database already exists.
  bool error_if_exists = false;

  // Verify checksums on every read. Costs a little CPU; catches bit rot.
  bool paranoid_checks = true;

  // Freeze the memtable and flush once it exceeds this. Bigger buffers mean
  // fewer, larger SSTables and less write amplification, at the cost of more
  // RAM and a longer WAL replay after a crash.
  size_t write_buffer_size = 4u * 1024 * 1024;

  // Target size of an uncompressed data block. Smaller blocks make point
  // lookups cheaper (less to read and decode per key) and scans slightly worse
  // (more index entries, more block boundaries to cross).
  size_t block_size = 4096;

  // Keys between restart points inside a block. Smaller means faster seeks and
  // weaker prefix compression.
  int block_restart_interval = 16;

  // Bits per key in each SSTable's bloom filter. 10 gives a 0.82% false
  // positive rate at 7 probes. 0 disables filters entirely.
  //
  // Set on the DB; the DB constructs the policy and points filter_policy at it.
  int bloom_bits_per_key = 10;

  // Bytes of decoded data blocks to keep in memory. 0 disables the cache and
  // falls back on the OS page cache alone.
  size_t block_cache_size = 8u * 1024 * 1024;

  // Not owned. The DB creates and owns these, then hands them to each Table.
  // They are here rather than as Table::Open parameters so that one cache and
  // one policy are shared across every table in the database, which is the
  // entire reason a shared cache is useful.
  Cache* block_cache = nullptr;
  const BloomFilterPolicy* filter_policy = nullptr;
};

// Per-read cost accounting.
//
// WHY THIS IS NOT THE GLOBAL Stats STRUCT
//
// DB::GetStats() returns process-lifetime totals. A query could diff them
// before and after and call the difference its own cost, and that is what a
// first attempt did - but the numbers were wrong the moment a second request
// ran concurrently, because a background compaction and another reader both
// move the same counters. The bug does not show up in a single-threaded test
// and does show up under load, which is the worst combination.
//
// So a read carries its own sink. The engine increments through the pointer
// when it is set and does nothing when it is null, and the cost reported by a
// query is that query's cost even with a compaction running underneath it.
//
// Not atomic, and deliberately so: one ReadStats belongs to one request, and
// one request is served by one thread. Sharing a ReadStats across threads is a
// caller error, not something to pay for on every block read.
struct ReadStats {
  uint64_t keys_scanned = 0;      // entries the iterators actually stepped over
  uint64_t blocks_read = 0;       // data blocks decoded, cache misses only
  uint64_t block_cache_hits = 0;  // blocks that were already in memory
  uint64_t bloom_rejections = 0;  // lookups a filter answered without any I/O
  uint64_t range_rejections = 0;  // files skipped on their key range alone
  uint64_t sstables_probed = 0;
  uint64_t memtable_hits = 0;

  void Add(const ReadStats& other) {
    keys_scanned += other.keys_scanned;
    blocks_read += other.blocks_read;
    block_cache_hits += other.block_cache_hits;
    bloom_rejections += other.bloom_rejections;
    range_rejections += other.range_rejections;
    sstables_probed += other.sstables_probed;
    memtable_hits += other.memtable_hits;
  }
};

struct ReadOptions {
  bool verify_checksums = true;

  // False means "read this block but do not pollute the cache with it". Set
  // during compaction, which streams through every block exactly once and
  // would otherwise evict the working set of live reads.
  bool fill_cache = true;

  // Read the database as of this snapshot. nullptr means "as of now".
  //
  // This matters more here than in a general-purpose store: a multi-hop graph
  // traversal touches thousands of keys over several milliseconds, and without
  // a pinned sequence number a concurrent ingest could show an entity at hop 1
  // that has been merged away by hop 2 - inconsistent results, no error.
  const Snapshot* snapshot = nullptr;

  // Where to record what this read cost. Not owned, and null by default so
  // that every existing caller keeps working and pays nothing.
  ReadStats* stats = nullptr;
};

struct WriteOptions {
  // false : the write reaches the OS page cache. Survives a process crash,
  //         not a power cut. Fast.
  // true  : fsync before returning. Survives power loss. Costs a device
  //         round-trip, which is why group commit exists.
  //
  // Bulk ingest runs with sync=false and one explicit sync at the end of each
  // batch; that is the right trade when the source data is replayable.
  bool sync = false;
};

}  // namespace sextant::lsm
