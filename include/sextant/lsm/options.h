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
