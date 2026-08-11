// The common iteration abstraction.
//
// Why this exists as an interface rather than a concrete class: by day 3 a
// single logical scan has to walk the memtable, the immutable memtable, and one
// iterator per SSTable, merging them on the fly. Every one of those has a
// different underlying representation, but the merge code should not care.
//
// The cleanup registration is the part that looks odd until you need it. A
// block iterator points *into* a heap-allocated block; when the iterator dies,
// something has to release that block. Registering a callback lets the iterator
// own that responsibility without the caller having to know it exists.

#pragma once

#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

class Iterator {
 public:
  Iterator();
  virtual ~Iterator();

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  // An iterator is either positioned at a key/value pair, or invalid.
  virtual bool Valid() const = 0;

  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;

  // Position at the first key at or after target. If no such key exists the
  // iterator becomes invalid - it does not wrap.
  virtual void Seek(const Slice& target) = 0;

  virtual void Next() = 0;
  virtual void Prev() = 0;

  // REQUIRES: Valid(). The returned slices are invalidated by any subsequent
  // movement of this iterator.
  virtual Slice key() const = 0;
  virtual Slice value() const = 0;

  virtual Status status() const = 0;

  // Run function(arg1, arg2) when this iterator is destroyed. Callbacks run in
  // registration order. Used to release blocks and drop cache handles.
  using CleanupFunction = void (*)(void* arg1, void* arg2);
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  struct CleanupNode {
    CleanupFunction function = nullptr;
    void* arg1 = nullptr;
    void* arg2 = nullptr;
    CleanupNode* next = nullptr;

    bool IsEmpty() const { return function == nullptr; }
    void Run() { function(arg1, arg2); }
  };

  // The first node is inline: the overwhelmingly common case is exactly one
  // cleanup, and paying an allocation for it would be silly.
  CleanupNode cleanup_head_;
};

Iterator* NewEmptyIterator();
Iterator* NewErrorIterator(const Status& status);

}  // namespace sextant::lsm
