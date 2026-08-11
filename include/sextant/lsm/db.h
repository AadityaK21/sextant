// The public storage-engine interface.
//
// Deliberately small: this is the contract every layer above codes against
// (codec, ontology, resolve, lineage, query). Keeping it narrow is what lets
// the engine's internals change - memtable-only today, SSTables and leveled
// compaction on day 4 - without touching a line of the ontology layer.
//
// MILESTONE STATUS (docs/EXECUTION_PLAN.md)
//   Day 1  ✅ memtable + WAL + recovery
//   Day 2  ✅ SSTable build/read, flush to L0   <- you are here
//   Day 3  ⬜ bloom filters, block cache, merging iterator
//   Day 4  ⬜ VersionSet/MANIFEST, leveled compaction

#pragma once

#include <memory>
#include <string>

#include "sextant/lsm/options.h"
#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"
#include "sextant/lsm/write_batch.h"

namespace sextant::lsm {

// An immutable view of the database at one point in the write history.
// Implemented as nothing more than a sequence number.
class Snapshot {
 public:
  virtual ~Snapshot() = default;
};

struct Stats {
  uint64_t writes = 0;
  uint64_t bytes_written = 0;
  uint64_t reads = 0;
  uint64_t memtable_hits = 0;
  uint64_t memtable_bytes = 0;
  uint64_t wal_records_replayed = 0;
  uint64_t sequence = 0;

  // Day 2
  uint64_t flushes = 0;          // memtables written out as L0 tables
  uint64_t bytes_flushed = 0;
  uint64_t num_sstables = 0;
  uint64_t sstable_hits = 0;     // reads satisfied from disk
  uint64_t sstables_probed = 0;  // tables consulted across all reads
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& name,
                     std::unique_ptr<DB>* dbptr);

  DB() = default;
  virtual ~DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions& opts, const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const WriteOptions& opts, const Slice& key) = 0;

  // Atomic multi-key write. This is the one an entity merge uses.
  virtual Status Write(const WriteOptions& opts, WriteBatch* updates) = 0;

  // Returns Status::NotFound if the key is absent or tombstoned.
  virtual Status Get(const ReadOptions& opts, const Slice& key, std::string* value) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  virtual Stats GetStats() const = 0;

  // Force the WAL to the device. Used at the end of an ingest batch.
  virtual Status SyncWAL() = 0;
};

}  // namespace sextant::lsm
