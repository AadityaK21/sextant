// Order-preserving encodings for values embedded inside composite keys.
//
// THE PROBLEM. A secondary index key looks like
//
//     IDX | type | prop | <value> | entity_id
//
// and it only works if sorting those bytes sorts by VALUE. Two things break
// that, and both have to be fixed explicitly.
//
// ---------------------------------------------------------------------------
// 1. VARIABLE-LENGTH VALUES NEED A TERMINATOR, NOT A LENGTH PREFIX
//
// The obvious encoding - length prefix, then bytes - destroys ordering: "b"
// (len 1) would sort before "aa" (len 2) because the length byte is compared
// first. So values are terminated instead.
//
// But a plain NUL terminator fails the moment a value contains a NUL, which it
// can: keys here are binary. The fix is the standard escape used by
// FoundationDB and CockroachDB:
//
//     0x00 inside the value  ->  0x00 0xFF
//     end of value           ->  0x00 0x00
//
// This preserves order because the terminator (0x00 0x00) compares less than
// an escaped NUL (0x00 0xFF) and less than any real byte, so a string sorts
// before every string that extends it.
//
// ---------------------------------------------------------------------------
// 2. SIGNED NUMBERS DO NOT SORT BYTEWISE
//
// Two's complement puts negatives above positives when compared as unsigned
// bytes: -1 is 0xFFFF... which looks larger than 1. Flipping the sign bit maps
// the signed range onto the unsigned range in order.
//
// IEEE 754 doubles are worse. Positives already sort correctly as unsigned
// integers, but negatives sort BACKWARDS because the magnitude bits ascend
// while the value descends. The standard transform:
//
//     positive (sign bit clear)  ->  set the sign bit
//     negative (sign bit set)    ->  flip every bit
//
// After that, memcmp orders doubles correctly across the whole range including
// negative zero and infinities.
//
// This is why lat/lon range queries on a Port index work at all.

#pragma once

#include <cstdint>
#include <string>

#include "sextant/lsm/slice.h"

namespace sextant::codec {

using lsm::Slice;

// --- strings ---------------------------------------------------------------

// Append value with NUL escaping and a two-byte terminator.
void EncodeOrderedString(std::string* dst, const Slice& value);

// Consume one encoded string from input, advancing it past the terminator.
bool DecodeOrderedString(Slice* input, std::string* out);

// --- integers --------------------------------------------------------------

void EncodeOrderedUint64(std::string* dst, uint64_t value);
bool DecodeOrderedUint64(Slice* input, uint64_t* out);

// Sign bit flipped so negatives sort below positives.
void EncodeOrderedInt64(std::string* dst, int64_t value);
bool DecodeOrderedInt64(Slice* input, int64_t* out);

// --- doubles ---------------------------------------------------------------

void EncodeOrderedDouble(std::string* dst, double value);
bool DecodeOrderedDouble(Slice* input, double* out);

}  // namespace sextant::codec
