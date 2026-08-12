// Opens SSTables on demand and keeps the hot ones open.
//
// Once compaction exists, a Version holds only lightweight FileMetaData - a
// number, a size, a key range. It does NOT hold open Table objects, because a
// Version can be created and destroyed on every flush and every compaction, and
// reopening a file on each one would be absurd.
//
// So lookups go through here: "give me a reader for file 41". A cache hit is a
// hash lookup; a miss opens the file, parses its footer, loads its index block
// and its bloom filter, then caches the whole thing.
//
// This is a second, separate cache from the block cache. They hold different
// things at different granularity: this one caches OPEN FILES (index + filter,
// maybe 1% of the file), the block cache caches DECODED DATA BLOCKS. Being able
// to explain why both exist is worth more than either one individually.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "cache.h"
#include "sextant/lsm/iterator.h"
#include "sextant/lsm/options.h"
#include "sextant/lsm/status.h"
#include "table.h"

namespace sextant::lsm {

class TableCache {
 public:
  TableCache(std::string dbname, const Options& options, int max_open_files);
  ~TableCache();

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Iterator over the whole file. If tableptr is non-null it receives the
  // underlying Table, which remains owned by the cache.
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number,
                        uint64_t file_size, Table** tableptr = nullptr);

  // Point lookup. Calls handle_result at most once.
  Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
             const Slice& k, void* arg,
             void (*handle_result)(void*, const Slice&, const Slice&));

  // Drop a file from the cache. Called when compaction deletes it, so the file
  // handle is closed before the file is unlinked.
  void Evict(uint64_t file_number);

  uint64_t Hits() const { return cache_->Hits(); }
  uint64_t Misses() const { return cache_->Misses(); }

  // Total filter rejections across every currently-open table.
  uint64_t FilterRejections() const { return filter_rejections_; }
  void AddFilterRejections(uint64_t n) { filter_rejections_ += n; }

 private:
  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

  const std::string dbname_;
  const Options options_;
  std::unique_ptr<Cache> cache_;
  mutable uint64_t filter_rejections_ = 0;
};

}  // namespace sextant::lsm
