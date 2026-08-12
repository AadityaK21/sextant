// Reads an SSTable written by TableBuilder.
//
// Opening is three steps and no scanning:
//   1. seek to (file_size - Footer::kEncodedLength), read the footer
//   2. the footer names the index block; read it (once, kept in memory)
//   3. every subsequent lookup is: binary search the index, read one data block
//
// NewIterator returns a TWO-LEVEL iterator: the outer level walks index
// entries, the inner level walks the data block each index entry points at.
// The inner iterator is created lazily and destroyed as the outer one moves, so
// scanning a 100 MB table holds one 4 KB block in memory at a time, not the
// whole file.

#pragma once

#include <memory>

#include "block.h"
#include "env.h"
#include "format.h"
#include "internal_key.h"
#include "sextant/lsm/iterator.h"
#include "sextant/lsm/options.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

class Table {
 public:
  // Takes ownership of file on success.
  static Status Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                     uint64_t file_size, std::unique_ptr<Table>* table);

  ~Table();
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  Iterator* NewIterator(const ReadOptions& options) const;

  // How many lookups this table answered from its bloom filter without
  // touching a data block. Surfaced in DB stats so the filter's value is
  // measurable rather than assumed.
  uint64_t FilterRejections() const;

  // Point lookup. Calls handle_result(arg, key, value) at most once, for the
  // first entry at or after key. Cheaper than building an iterator because it
  // does not have to support movement.
  //
  // Day 3 adds the bloom-filter check here - the whole point of a filter is to
  // return "definitely absent" before any data block is read.
  Status InternalGet(const ReadOptions& options, const Slice& key, void* arg,
                     void (*handle_result)(void* arg, const Slice& k, const Slice& v));

 private:
  struct Rep;

  explicit Table(Rep* rep) : rep_(rep) {}

  void ReadFilter(const ReadOptions& options);

  static Iterator* BlockReader(void* arg, const ReadOptions& options,
                               const Slice& index_value);

  Rep* rep_;
};

// Wraps an index iterator plus a function that turns an index value into a
// data-block iterator.
Iterator* NewTwoLevelIterator(
    Iterator* index_iter,
    Iterator* (*block_function)(void* arg, const ReadOptions& options,
                                const Slice& index_value),
    void* arg, const ReadOptions& options, const InternalKeyComparator& comparator);

}  // namespace sextant::lsm
