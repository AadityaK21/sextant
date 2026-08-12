// Turns a stream of internal keys into a stream of live user keys.
//
// The merging iterator hands us every version of every key, newest first,
// including tombstones and including versions newer than our snapshot. Three
// rules collapse that into what a caller actually wants:
//
//   1. skip anything with sequence > snapshot        (snapshot isolation)
//   2. for each user key, take only the FIRST surviving entry (newest wins)
//   3. if that entry is a tombstone, emit nothing and move on
//
// Rule 2 is free because of the internal key ordering: within one user key,
// sequence numbers descend, so the newest visible version is simply the first
// one encountered. No comparison of versions, no bookkeeping.

#pragma once

#include <memory>

#include "internal_key.h"
#include "sextant/lsm/iterator.h"

namespace sextant::lsm {

// Takes ownership of internal_iter.
Iterator* NewDBIterator(const InternalKeyComparator& comparator,
                        Iterator* internal_iter, SequenceNumber snapshot);

}  // namespace sextant::lsm
