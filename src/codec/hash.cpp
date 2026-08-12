#include "hash.h"

#include <cstdint>
#include <string>

namespace sextant::codec {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

// splitmix64's finalizer. Three multiply-xorshift rounds that take an input
// with weak diffusion and give every output bit a roughly even dependence on
// every input bit.
uint64_t Mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

uint64_t FnvStep(uint64_t h, const char* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    h ^= static_cast<uint8_t>(data[i]);
    h *= kFnvPrime;
  }
  return h;
}

}  // namespace

uint64_t Hash64(const Slice& data) {
  return Mix(FnvStep(kFnvOffsetBasis, data.data(), data.size()));
}

uint64_t Hash64Fields(const std::string* fields, size_t count) {
  uint64_t h = kFnvOffsetBasis;
  for (size_t i = 0; i < count; ++i) {
    // Feed the length before the bytes. Without this, ("ab", "c") and
    // ("a", "bc") hash identically, and natural keys are routinely built from
    // several columns whose boundaries carry meaning - UN/LOCODE splits its
    // code across a Country column and a Location column.
    const uint64_t len = fields[i].size();
    for (int b = 0; b < 8; ++b) {
      h ^= static_cast<uint8_t>((len >> (8 * b)) & 0xFF);
      h *= kFnvPrime;
    }
    h = FnvStep(h, fields[i].data(), fields[i].size());
  }
  return Mix(h);
}

}  // namespace sextant::codec
