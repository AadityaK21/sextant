// Writes an SSTable.
//
// Keys must arrive in sorted order, which is free here: the only caller is the
// memtable flush, and a skiplist walk is already sorted. That single constraint
// is what allows the whole file to be written in one forward pass with no
// seeking and no rewriting.
//
// THE INDEX BLOCK is the interesting part. After each data block is written, an
// entry is added to the index mapping (a key >= every key in that block) to the
// block's handle. To find the block that could contain key K, binary search the
// index for the first entry whose key is >= K. Since the index has one entry per
// block rather than one per key, it is roughly 1/1000th the size of the data and
// stays resident in memory.
//
// This is why an SSTable read is one seek and not a scan: footer -> index ->
// data block, three reads regardless of file size.

#pragma once

#include <memory>
#include <string>

#include "block_builder.h"
#include "env.h"
#include "format.h"
#include "internal_key.h"
#include "sextant/lsm/options.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

class TableBuilder {
 public:
  TableBuilder(const Options& options, WritableFile* file);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: key is strictly greater than every key already added.
  void Add(const Slice& key, const Slice& value);

  // Finish the current data block early. Rarely needed by callers.
  void Flush();

  Status status() const { return status_; }

  // Write the metaindex block, index block and footer. No further calls after.
  Status Finish();

  // Abandon the table. The caller is responsible for deleting the file.
  void Abandon();

  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }

  // Cut a data block once it reaches roughly this many bytes.
  static constexpr size_t kDefaultBlockSize = 4096;

 private:
  bool ok() const { return status_.ok(); }
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(const Slice& data, CompressionType type, BlockHandle* handle);

  Options options_;
  WritableFile* file_;
  uint64_t offset_ = 0;
  Status status_;

  InternalKeyComparator comparator_{};
  BlockBuilder data_block_;
  BlockBuilder index_block_;

  std::string last_key_;
  uint64_t num_entries_ = 0;
  bool closed_ = false;

  // An index entry can only be written once the NEXT data block starts,
  // because until then we do not know this block is finished. So the pending
  // handle is carried forward one step.
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
};

}  // namespace sextant::lsm
