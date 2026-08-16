// Bloom filter.
//
// A bloom filter answers one question: "is this key definitely NOT in this
// table?" It can say "definitely not" or "maybe". It never says "definitely
// yes". That asymmetry is exactly what an LSM read path needs, because the
// expensive case is reading a 4 KB block off disk to discover the key was never
// there.
//
// THE MATH, which you should be able to reproduce on a whiteboard.
//
//   m = bits in the filter, n = keys inserted, k = hash functions
//
//   optimal k       = (m/n) * ln 2
//   false positives ~ (1 - e^(-kn/m))^k
//
// At m/n = 10:  k = 6.93 -> 7, and the false-positive rate is 0.82%.
// Ten bits per key to avoid ~99% of pointless disk reads is one of the best
// trades available in systems work.
//
// IMPLEMENTATION NOTE: we do not compute
// seven independent hashes. Kirsch-Mitzenmacher ("Less Hashing, Same
// Performance", 2006) showed that g_i(x) = h1(x) + i*h2(x) has no asymptotic
// penalty in false-positive rate. So we take ONE 32-bit hash and derive the
// rest by repeatedly adding a rotation of it - seven times cheaper.
//
// THE FILTER IS BUILT ON USER KEYS, not internal keys. A lookup asks "does
// this table contain user key K?" without knowing which sequence numbers exist,
// so hashing the internal key would make every probe miss.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "sextant/lsm/slice.h"

namespace sextant::lsm {

// Murmur-style hash. Cheap, well-distributed enough for filters.
uint32_t Hash(const char* data, size_t n, uint32_t seed);

inline uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

class BloomFilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key = 10);

  const char* Name() const { return "sextant.BuiltinBloomFilter"; }

  // Append a filter covering keys[0..n-1] to dst.
  void CreateFilter(const Slice* keys, int n, std::string* dst) const;

  // False means the key is definitely absent. True means "maybe present".
  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const;

  int bits_per_key() const { return bits_per_key_; }
  size_t num_probes() const { return k_; }

 private:
  int bits_per_key_;
  size_t k_;  // number of probes, derived from bits_per_key
};

}  // namespace sextant::lsm
