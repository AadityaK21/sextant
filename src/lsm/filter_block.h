// The per-SSTable filter block.
//
// One bloom filter for the whole table would work, but it would have to be
// loaded in full and would grow without bound. Instead the table is divided
// into 2 KB ranges of FILE OFFSET, and each range gets its own filter covering
// the keys in the data blocks that start there.
//
// So the lookup sequence becomes:
//
//   index block  ->  "key K would live in the data block at offset X"
//   filter block ->  "offset X's filter says K is definitely not present"
//   -> return immediately, having read no data block at all
//
// FILTER BLOCK FORMAT
//
//   [filter 0][filter 1] ... [filter N-1]     variable length
//   [offset of filter 0    : uint32 BE]
//   ...
//   [offset of filter N-1  : uint32 BE]
//   [offset of offset array: uint32 BE]
//   [base_lg               : 1 byte  ]       filters cover 2^base_lg bytes
//
// Reading it backwards from the end gives you the array position and the
// granularity, so the block is self-describing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bloom.h"
#include "sextant/lsm/slice.h"

namespace sextant::lsm {

// 2 KB per filter. Smaller means more filters and finer granularity; larger
// means fewer, coarser filters. 11 matches the 4 KB block size closely enough
// that most data blocks map to one or two filters.
static constexpr size_t kFilterBaseLg = 11;
static constexpr size_t kFilterBase = 1 << kFilterBaseLg;

class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const BloomFilterPolicy* policy);

  // Called by TableBuilder as each data block starts, with that block's offset.
  void StartBlock(uint64_t block_offset);

  // REQUIRES: the USER key, not the internal key. See bloom.h.
  void AddKey(const Slice& user_key);

  Slice Finish();

 private:
  void GenerateFilter();

  const BloomFilterPolicy* policy_;
  std::string keys_;              // flattened keys for the current filter
  std::vector<size_t> start_;     // offset of each key within keys_
  std::string result_;            // filter data accumulated so far
  std::vector<Slice> tmp_keys_;   // scratch, rebuilt per filter
  std::vector<uint32_t> filter_offsets_;
};

class FilterBlockReader {
 public:
  // contents must outlive the reader.
  FilterBlockReader(const BloomFilterPolicy* policy, const Slice& contents);

  // False means "definitely absent from the data block at this offset".
  bool KeyMayMatch(uint64_t block_offset, const Slice& user_key) const;

 private:
  const BloomFilterPolicy* policy_;
  const char* data_ = nullptr;    // start of the block
  const char* offset_ = nullptr;  // start of the offset array
  size_t num_ = 0;                // number of filters
  size_t base_lg_ = 0;
};

}  // namespace sextant::lsm
