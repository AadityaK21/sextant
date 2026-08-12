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

#include <cstdint>
#include <memory>
#include <string>

#include "sextant/lsm/iterator.h"
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

  // Day 3. These three are the whole story of the read-path optimisation:
  // every rejection is a data block that was never read off disk.
  uint64_t range_rejections = 0;   // file skipped: key outside its [smallest,largest]
  uint64_t filter_rejections = 0;  // block skipped: bloom filter said "definitely not"
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_evictions = 0;
  uint64_t cache_bytes = 0;

  // Day 4
  uint64_t compactions = 0;
  uint64_t trivial_moves = 0;            // files relabelled without rewriting bytes
  uint64_t compaction_bytes_written = 0;
  uint64_t keys_dropped = 0;             // versions and tombstones reclaimed
  uint64_t files_deleted = 0;
  uint64_t write_stalls = 0;             // writers throttled to let compaction catch up
  uint64_t total_bytes_on_disk = 0;
  uint64_t files_per_level[7] = {};
  uint64_t bytes_per_level[7] = {};
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

  // Ordered scan over live user keys, merging the memtable, the immutable
  // memtable and every SSTable. Tombstoned and shadowed versions are hidden.
  //
  // FORWARD ONLY for now: SeekToFirst, Seek and Next. Reverse iteration is not
  // implemented because nothing in this project needs it - compaction streams
  // forward, and the graph traversals in src/query are forward range scans.
  // Calling Prev() returns a NotSupported status rather than pretending.
  //
  // This is the primitive the whole system above the engine is built on: a
  // LINKOUT prefix scan and a TIDX time-range scan are both just Seek + Next.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  virtual Stats GetStats() const = 0;

  // Force the WAL to the device. Used at the end of an ingest batch.
  virtual Status SyncWAL() = 0;

  // Block until background flush and compaction are idle.
  //
  // Exposed because compaction is asynchronous: without this, a test asserting
  // "L0 has at most 4 files" would be racing the background thread and would
  // fail intermittently. Also useful before taking a benchmark reading, so the
  // measurement is not distorted by work still in flight.
  virtual void WaitForBackgroundWork() = 0;
};

}  // namespace sextant::lsm
