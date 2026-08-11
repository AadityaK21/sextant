// Reads a block produced by BlockBuilder.
//
// The iterator is where the restart array earns its keep. Seek() binary
// searches the restart offsets to find the last restart point whose key is
// <= target, then decodes forward at most restart_interval entries. That is
// O(log(n/16) + 16) rather than O(n).

#pragma once

#include <cstddef>
#include <cstdint>

#include "format.h"
#include "internal_key.h"
#include "sextant/lsm/iterator.h"

namespace sextant::lsm {

class Block {
 public:
  // Takes ownership of contents.data() if contents.heap_allocated.
  explicit Block(const BlockContents& contents);
  ~Block();

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return size_; }

  // The returned iterator orders keys with the supplied comparator, which for
  // data and index blocks is always the InternalKeyComparator.
  Iterator* NewIterator(const InternalKeyComparator& comparator);

 private:
  class Iter;

  uint32_t NumRestarts() const;

  const char* data_;
  size_t size_;
  uint32_t restart_offset_ = 0;  // where the restart array begins
  bool owned_ = false;
};

}  // namespace sextant::lsm
