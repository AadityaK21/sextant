#pragma once

#include <cstddef>
#include <cstdint>

namespace sextant::lsm {

class Snapshot;

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
};

struct ReadOptions {
  bool verify_checksums = true;
  bool fill_cache = true;

  // Read the database as of this snapshot. nullptr means "as of now".
  //
  // This matters more here than in a general-purpose store: a multi-hop graph
  // traversal touches thousands of keys over several milliseconds, and without
  // a pinned sequence number a concurrent ingest could show an entity at hop 1
  // that has been merged away by hop 2 — inconsistent results, no error.
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
