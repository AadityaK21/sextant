// Sharded LRU cache for decoded data blocks.
//
// WHY SHARDING IS THE WHOLE POINT. A cache is consulted on every single read,
// so a single global mutex around it becomes the bottleneck the moment more
// than one thread reads. Splitting into 16 independent shards, each with its
// own lock, chosen by hash(key) & 15, means 16 threads can usually proceed
// without ever contending. This is the standard fix and it is worth being able
// to name.
//
// WHY REFERENCE COUNTING. A block iterator points directly INTO the cached
// block's bytes. If eviction freed that memory while an iterator was walking
// it, you would get a use-after-free that appears only under memory pressure -
// the worst class of bug to debug. So Lookup() pins the entry, the caller must
// Release() it, and eviction skips anything still pinned.
//
// Consequence, stated honestly: if every entry is pinned, usage can exceed
// capacity until the holders let go. Capacity is a target, not a hard bound.

#pragma once

#include <cstdint>
#include <memory>

#include "sextant/lsm/slice.h"

namespace sextant::lsm {

class Cache {
 public:
  // Opaque pinned reference to a cached value.
  struct Handle {};

  static std::unique_ptr<Cache> NewLRUCache(size_t capacity);

  virtual ~Cache() = default;

  // Insert and pin. The caller must Release() the returned handle.
  // deleter runs when the entry is evicted and no longer referenced.
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // Returns nullptr on a miss. A hit is pinned and must be Released.
  virtual Handle* Lookup(const Slice& key) = 0;

  virtual void Release(Handle* handle) = 0;
  virtual void* Value(Handle* handle) = 0;
  virtual void Erase(const Slice& key) = 0;

  // Unique id used to namespace one table's blocks from another's inside a
  // shared cache.
  virtual uint64_t NewId() = 0;

  virtual size_t TotalCharge() const = 0;

  // Diagnostics; surfaced through DB stats so the hit rate is observable.
  virtual uint64_t Hits() const = 0;
  virtual uint64_t Misses() const = 0;
  virtual uint64_t Evictions() const = 0;
};

}  // namespace sextant::lsm
