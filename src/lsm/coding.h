// Byte-level encoding primitives.
//
// THE RULE THAT GOVERNS THIS FILE: fixed-width integers are stored
// BIG-ENDIAN, so that lexicographic byte order (memcmp) equals numeric order.
//
// That single property is what makes the whole system work.  It is why
//   TIDX | link_type | port_id | timestamp_BE | voyage_id
// answers "all voyages into Rotterdam between April and July" as one
// contiguous range scan instead of a scan-and-filter.  If timestamps were
// little-endian, byte order would be meaningless and that query would degrade
// to a full scan.  See docs/ARCHITECTURE.md §5.
//
// Varints are a different tool for a different job: they are used for LENGTHS
// inside records, where compactness matters and ordering does not.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "sextant/lsm/slice.h"

namespace sextant::lsm {

// --- fixed-width, big-endian ----------------------------------------------

inline void EncodeFixed32BE(char* buf, uint32_t value) {
  buf[0] = static_cast<char>((value >> 24) & 0xff);
  buf[1] = static_cast<char>((value >> 16) & 0xff);
  buf[2] = static_cast<char>((value >> 8) & 0xff);
  buf[3] = static_cast<char>(value & 0xff);
}

inline void EncodeFixed64BE(char* buf, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xff);
  }
}

inline uint32_t DecodeFixed32BE(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

inline uint64_t DecodeFixed64BE(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
  return v;
}

inline void PutFixed32BE(std::string* dst, uint32_t value) {
  char buf[4];
  EncodeFixed32BE(buf, value);
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64BE(std::string* dst, uint64_t value) {
  char buf[8];
  EncodeFixed64BE(buf, value);
  dst->append(buf, sizeof(buf));
}

// --- varints ---------------------------------------------------------------

inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    ++len;
  }
  return len;
}

char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);

// --- length-prefixed slices ------------------------------------------------

void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

}  // namespace sextant::lsm
