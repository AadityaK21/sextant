#include "crc32c.h"

#include <array>

namespace sextant::lsm::crc32c {
namespace {

// Reflected Castagnoli polynomial.
constexpr uint32_t kPoly = 0x82F63B78u;

// Built at compile time — no static initialisation order concerns, no runtime
// table-generation pass.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  uint32_t c = init_crc ^ 0xFFFFFFFFu;
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace sextant::lsm::crc32c
