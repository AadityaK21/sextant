// Bump allocator for memtable nodes.
//
// Why: a memtable holds millions of tiny, immortal allocations (skiplist nodes
// and key bytes) whose lifetimes are all identical — they die together when
// the memtable is flushed.  That is the exact shape an arena is for.  We get
//   * one allocation per 4 KB block instead of one per node
//   * no per-node free, no fragmentation, no destructor pass
//   * dramatically better locality, since nodes inserted together sit together
//
// The cost is that you cannot free an individual object.  For a memtable that
// is not a limitation, it is the point.
//
// memory_usage_ is atomic because the flush decision ("is this memtable full?")
// is read by the background thread while the writer is still appending.

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sextant::lsm {

class Arena {
 public:
  Arena() = default;
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes);

  // Allocate with alignment suitable for the platform's largest scalar.
  // Skiplist nodes contain std::atomic<Node*> and must be properly aligned.
  char* AllocateAligned(size_t bytes);

  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  static constexpr size_t kBlockSize = 4096;

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace sextant::lsm
