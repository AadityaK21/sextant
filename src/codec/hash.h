// A 64-bit non-cryptographic hash, used for natural keys and blocking keys.
//
// WHERE THIS GETS USED, AND WHY 64 BITS
//
//   SRCREC  src(4) | natural_key_hash(8)     - identity of a source row
//   XREF    src(4) | source_pk_hash(8)       - which entity a source row landed in
//   BLOCK   block_key_hash(8) | src | rec    - entity-resolution candidate blocks
//
// In the first two the hash IS the identity: a collision silently merges two
// unrelated source rows, which is a data-corruption bug that would surface much
// later as a nonsensical entity. That risk is what sets the width. With 64 bits
// and 10^6 records the birthday bound gives a collision probability around
// 3 x 10^-8; with 32 bits the same dataset collides with near certainty. This
// is the one place in the project where the hash width is a correctness
// argument rather than a performance one.
//
// BLOCK is the opposite case - collisions there merely put two records in the
// same candidate block, which costs a wasted comparison and nothing else.
//
// WHY NOT xxHash OR CITY. Both are faster and better distributed, and both are
// a dependency plus a page of constants to get subtly wrong. Hashing here is
// nowhere near the hot path: it runs once per source row during ingest, while
// the read path hashes nothing. FNV-1a is short enough to verify by reading it.
//
// The one real weakness of FNV-1a is poor avalanche - inputs differing in a
// single low bit produce outputs differing in few bits, which clusters badly
// when you then take the low bits for bucketing. The finalizer below is
// splitmix64's mixing function, which fixes exactly that. Cost is three
// multiplies once per hash.

#pragma once

#include <cstdint>
#include <string>

#include "sextant/lsm/slice.h"

namespace sextant::codec {

using lsm::Slice;

// FNV-1a over the bytes, then a splitmix64 finalizer for avalanche.
uint64_t Hash64(const Slice& data);

// Hash a list of fields as one unit. Fields are separated by a byte that cannot
// appear in the length-prefixed framing, so ("ab","c") and ("a","bc") differ -
// a plain concatenation would collapse them, and natural keys are exactly the
// kind of thing built out of several columns.
uint64_t Hash64Fields(const std::string* fields, size_t count);

}  // namespace sextant::codec
