#include "arena.h"

#include <cassert>
#include <cstdint>

namespace sextant::lsm {

Arena::~Arena() {
  for (char* block : blocks_) delete[] block;
}

char* Arena::AllocateFallback(size_t bytes) {
  // A request larger than a quarter of a block gets its own exact-sized block,
  // so that one big allocation cannot waste most of a fresh 4 KB block.
  if (bytes > kBlockSize / 4) {
    return AllocateNewBlock(bytes);
  }
  // Otherwise abandon whatever is left in the current block (at most
  // kBlockSize/4 bytes of waste) and start a fresh one.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {
  constexpr size_t kAlign = alignof(std::max_align_t) > 8 ? alignof(std::max_align_t) : 8;
  static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of two");

  const size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  const size_t slop = (current_mod == 0) ? 0 : (kAlign - current_mod);
  const size_t needed = bytes + slop;

  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback returns freshly-allocated (therefore aligned) memory.
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.store(MemoryUsage() + block_bytes + sizeof(char*),
                      std::memory_order_relaxed);
  return result;
}

}  // namespace sextant::lsm
