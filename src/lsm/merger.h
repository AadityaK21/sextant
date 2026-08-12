// K-way merging iterator.
//
// A logical scan of the database has to walk the memtable, the immutable
// memtable, and every L0 table at once, presenting them as one sorted stream.
// That is what this does.
//
// The invariant that makes it work: because internal keys sort user-key
// ascending then sequence DESCENDING, and because a newer write always has a
// higher sequence, the merged stream yields the newest version of each user key
// FIRST. Callers that want current values simply take the first entry per user
// key and skip the rest. No version bookkeeping is needed anywhere.
//
// The implementation is a linear scan across children rather than a heap.
// With N in the low tens - one memtable, one immutable, a handful of L0 files -
// a scan over a contiguous array beats a heap on cache behaviour, and it is far
// easier to get Prev() right. A heap becomes worthwhile at large N, which is
// what leveled compaction on day 4 exists to prevent.

#pragma once

#include <memory>
#include <vector>

#include "internal_key.h"
#include "sextant/lsm/iterator.h"

namespace sextant::lsm {

// Takes ownership of every child iterator.
Iterator* NewMergingIterator(const InternalKeyComparator& comparator,
                             std::vector<Iterator*> children);

}  // namespace sextant::lsm
