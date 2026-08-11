// CRC32C (Castagnoli, reflected polynomial 0x82F63B78).
//
// This is what makes a torn WAL tail *detectable* rather than fatal.  A crash
// mid-append leaves a partial record; on recovery the CRC fails, the reader
// stops cleanly at that point, and every write that was acknowledged before
// the crash is still intact.  Without it, recovery would happily replay
// garbage into the memtable.
//
// Castagnoli rather than the more familiar CRC32 (0xEDB88320) because it has
// better error-detection properties for short records and because x86-64 has a
// hardware instruction for it (SSE4.2 `crc32`) - a natural later optimisation,
// noted in docs/adr.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sextant::lsm::crc32c {

// Return the crc32c of data[0..n-1] concatenated onto an existing crc.
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// A CRC stored directly next to the bytes it protects can be confused with the
// data itself by a naive scan.  LevelDB masks stored CRCs by rotating; we do
// the same so that a stored CRC value can never be mistaken for a live one.
static constexpr uint32_t kMaskDelta = 0xa282ead8ul;

inline uint32_t Mask(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

inline uint32_t Unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

}  // namespace sextant::lsm::crc32c
