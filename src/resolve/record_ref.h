// How entity resolution names a source record.
//
// A record's identity is (source_id, natural_key_hash) - exactly the SRCREC
// key, so a reference is one point lookup away from the record it names and
// costs twelve bytes to carry around. Every stage from blocking to clustering
// passes these rather than whole records, which matters because the candidate
// set is the largest thing in the pipeline.
//
// The text form, "wpi:10000" or "unlocode:NL|RTM", exists for one reason: the
// golden set is a CSV a person has to be able to read and argue with. A file of
// hash pairs would be unreviewable, and an unreviewable ground truth is not
// ground truth.

#pragma once

#include <cstdint>
#include <string>

#include "keyspace.h"

namespace sextant::resolve {

struct RecordRef {
  codec::SourceId source = 0;
  uint64_t key_hash = 0;

  bool operator==(const RecordRef& o) const {
    return source == o.source && key_hash == o.key_hash;
  }
  bool operator!=(const RecordRef& o) const { return !(*this == o); }
  bool operator<(const RecordRef& o) const {
    if (source != o.source) return source < o.source;
    return key_hash < o.key_hash;
  }
};

struct RecordRefHash {
  size_t operator()(const RecordRef& r) const {
    // The key hash already has good avalanche; mixing the source in by
    // multiplication keeps records from two sources with the same natural key
    // out of the same bucket.
    return static_cast<size_t>(r.key_hash ^ (static_cast<uint64_t>(r.source) *
                                             0x9E3779B97F4A7C15ULL));
  }
};

// An unordered pair, canonicalised so that (a,b) and (b,a) are one thing.
struct PairRef {
  RecordRef a, b;

  PairRef() = default;
  PairRef(const RecordRef& x, const RecordRef& y) {
    if (y < x) {
      a = y;
      b = x;
    } else {
      a = x;
      b = y;
    }
  }

  bool operator==(const PairRef& o) const { return a == o.a && b == o.b; }
  bool operator<(const PairRef& o) const {
    if (!(a == o.a)) return a < o.a;
    return b < o.b;
  }
};

struct PairRefHash {
  size_t operator()(const PairRef& p) const {
    const RecordRefHash h;
    return h(p.a) * 31u + h(p.b);
  }
};

}  // namespace sextant::resolve
