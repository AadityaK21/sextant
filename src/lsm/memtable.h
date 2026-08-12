// The in-memory write buffer.
//
// Entry encoding inside the skiplist (one contiguous arena allocation):
//
//   varint32  internal_key_size      (= user_key.size() + 8)
//   bytes     user_key
//   uint64BE  (sequence << 8) | type
//   varint32  value_size
//   bytes     value
//
// The skiplist is keyed on `const char*` pointing at the start of that record,
// and the comparator decodes the length prefix before delegating to the
// InternalKeyComparator.  Storing key and value in one allocation means an
// insert touches one contiguous region rather than two.

#pragma once

#include <string>

#include "arena.h"
#include "internal_key.h"
#include "skiplist.h"
#include "sextant/lsm/iterator.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

class MemTable {
 private:
  // Compares two length-prefixed memtable entries by their internal keys.
  struct KeyComparator {
    InternalKeyComparator comparator;
    KeyComparator() = default;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

 public:
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // REFERENCE COUNTED, for the same reason Version is.
  //
  // An open iterator points directly into this memtable's arena. Meanwhile a
  // writer can freeze it (mem_ becomes imm_) and the background thread can
  // flush and then destroy it. Without a refcount that is a use-after-free
  // that only appears when a scan happens to overlap a flush - rare, timing
  // dependent, and extremely unpleasant to debug.
  //
  // The destructor is private so the only way to release one is Unref().
  void Ref() { ++refs_; }
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) delete this;
  }

  // Bytes of arena in use.  The DB compares this against
  // Options::write_buffer_size to decide when to freeze and flush.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  size_t NumEntries() const { return num_entries_; }

  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // Returns true if an entry for key was found - INCLUDING a tombstone, in
  // which case *s is set to NotFound.  Returning true for a tombstone is what
  // stops the caller falling through to older levels and resurrecting a
  // deleted value.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

  // Ordered iteration over internal keys.  Used by the flush path and tests.
  class Iterator {
   public:
    explicit Iterator(const MemTable* mem) : iter_(&mem->table_) {}

    bool Valid() const { return iter_.Valid(); }
    void SeekToFirst() { iter_.SeekToFirst(); }
    void SeekToLast() { iter_.SeekToLast(); }
    void Next() { iter_.Next(); }
    void Prev() { iter_.Prev(); }
    void Seek(const Slice& internal_key);

    Slice key() const;    // the internal key (user_key || trailer)
    Slice value() const;

   private:
    Table::Iterator iter_;
    std::string tmp_;     // scratch for Seek's length-prefixed target
  };

  // Heap-allocated adapter implementing the shared Iterator interface, so a
  // memtable can be fed to NewMergingIterator alongside SSTable iterators.
  // Caller owns the result; the memtable must outlive it.
  lsm::Iterator* NewIterator() const;

 private:
  ~MemTable() { assert(refs_ == 0); }

  int refs_ = 0;
  KeyComparator key_comparator_;
  Arena arena_;
  Table table_;
  size_t num_entries_ = 0;
};

}  // namespace sextant::lsm
