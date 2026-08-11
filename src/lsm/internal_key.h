// The internal key encoding - the single most load-bearing idea in the engine.
//
//     internal_key = user_key || trailer
//     trailer      = big-endian uint64 of  (sequence << 8) | value_type
//
// The comparator orders by user_key ASCENDING, then trailer DESCENDING.
// Because the sequence number occupies the high 56 bits of the trailer,
// "trailer descending" means "newest version first".
//
// Four capabilities fall out of this one encoding:
//
//   MVCC        Multiple versions of a key coexist as distinct internal keys.
//   Read path   A forward scan hits the newest version first, so Get() takes
//               the first match and skips the rest.  No version bookkeeping.
//   Deletes     A delete is a tombstone (kTypeDeletion), not a mutation.  This
//               is what lets an append-only, immutable-file design support
//               deletion at all.
//   Snapshots   A snapshot is just a sequence number; a read skips any entry
//               whose sequence exceeds it.  Nearly free - and load-bearing for
//               graph traversal, which must see a consistent graph across
//               thousands of key lookups.
//
// Interview trap to be ready for: why can't a tombstone be dropped as soon as
// compaction sees it?  Because an older version of the same user key may still
// live in a lower level.  Drop the tombstone above it and the deleted value
// RESURRECTS.  Tombstones may only be dropped when compacting into the bottom
// level, or when no older version can exist below.

#pragma once

#include <cassert>
#include <cstdint>
#include <string>

#include "coding.h"
#include "sextant/lsm/slice.h"

namespace sextant::lsm {

using SequenceNumber = uint64_t;

// 56 bits of sequence: 7.2e16 writes.  The remaining byte holds the type, so
// the packed trailer fits one aligned 64-bit load.
static constexpr SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

// When seeking we want to land on the newest version of a user key.  Because
// the trailer sorts descending and kTypeValue (1) > kTypeDeletion (0), seeking
// with the largest possible trailer for a given sequence puts us at or before
// every entry for that key.
static constexpr ValueType kValueTypeForSeek = kTypeValue;

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;

  ParsedInternalKey() = default;
  ParsedInternalKey(const Slice& u, SequenceNumber seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}

  std::string DebugString() const;
};

inline uint64_t PackSequenceAndType(SequenceNumber seq, ValueType t) {
  return (seq << 8) | static_cast<uint64_t>(t);
}

inline void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64BE(result, PackSequenceAndType(key.sequence, key.type));
}

inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

inline uint64_t ExtractTrailer(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return DecodeFixed64BE(internal_key.data() + internal_key.size() - 8);
}

bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result);

// Orders internal keys.  User keys are compared bytewise (memcmp); this is why
// every multi-field key in src/codec encodes its integers big-endian.
class InternalKeyComparator {
 public:
  int Compare(const Slice& a, const Slice& b) const;
  int Compare(const ParsedInternalKey& a, const ParsedInternalKey& b) const;
  const char* Name() const { return "sextant.InternalKeyComparator"; }
};

// A LookupKey packs the three shapes a Get() needs into one arena-free buffer:
//   memtable_key()  varint32(internal_key_size) || internal_key   (skiplist)
//   internal_key()  user_key || trailer                           (sstable)
//   user_key()      user_key                                      (comparison)
//
// Small keys stay in the inline buffer, so a point lookup allocates nothing.
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber sequence);
  ~LookupKey();

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  Slice memtable_key() const {
    return Slice(start_, static_cast<size_t>(end_ - start_));
  }
  Slice internal_key() const {
    return Slice(kstart_, static_cast<size_t>(end_ - kstart_));
  }
  Slice user_key() const {
    return Slice(kstart_, static_cast<size_t>(end_ - kstart_ - 8));
  }

 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  char space_[200];
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace sextant::lsm
