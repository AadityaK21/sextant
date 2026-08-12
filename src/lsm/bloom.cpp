#include "bloom.h"

#include <cstring>

#include "coding.h"

namespace sextant::lsm {

uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  constexpr uint32_t m = 0xc6a4a793;
  constexpr uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = seed ^ (static_cast<uint32_t>(n) * m);

  // Four bytes at a time while we can.
  while (data + 4 <= limit) {
    uint32_t w;
    std::memcpy(&w, data, 4);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }

  switch (limit - data) {
    case 3:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[0]));
      h *= m;
      h ^= (h >> r);
      break;
  }
  return h;
}

BloomFilterPolicy::BloomFilterPolicy(int bits_per_key)
    : bits_per_key_(bits_per_key) {
  // k = (m/n) * ln 2.  0.69 is close enough to ln 2 and keeps this integer-ish.
  size_t k = static_cast<size_t>(static_cast<double>(bits_per_key_) * 0.69);
  if (k < 1) k = 1;
  if (k > 30) k = 30;
  k_ = k;
}

void BloomFilterPolicy::CreateFilter(const Slice* keys, int n,
                                     std::string* dst) const {
  size_t bits = static_cast<size_t>(n) * static_cast<size_t>(bits_per_key_);

  // For very small n the false-positive rate blows up, because the filter is
  // too small for the hashes to spread out. Clamp to a floor.
  if (bits < 64) bits = 64;

  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const size_t init_size = dst->size();
  dst->resize(init_size + bytes, 0);
  // The probe count is stored as the final byte, so a reader never has to be
  // configured to match the writer. Change bits_per_key and old files still
  // read correctly.
  dst->push_back(static_cast<char>(k_));

  char* array = dst->data() + init_size;
  for (int i = 0; i < n; ++i) {
    uint32_t h = BloomHash(keys[i]);
    // Kirsch-Mitzenmacher: derive every further probe from one hash by adding
    // a rotation of it. Rotating (rather than, say, adding a constant) keeps
    // the increments well distributed.
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k_; ++j) {
      const uint32_t bitpos = h % bits;
      array[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      h += delta;
    }
  }
}

bool BloomFilterPolicy::KeyMayMatch(const Slice& key, const Slice& filter) const {
  const size_t len = filter.size();
  if (len < 2) return false;  // malformed: treat as "not present"

  const char* array = filter.data();
  const size_t bits = (len - 1) * 8;

  const size_t k = static_cast<size_t>(static_cast<uint8_t>(array[len - 1]));
  if (k > 30) {
    // Reserved for future encodings. Answering "maybe" keeps an unknown filter
    // format correct, merely slow - a filter must never produce a false
    // NEGATIVE, because that would silently lose data.
    return true;
  }

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (size_t j = 0; j < k; ++j) {
    const uint32_t bitpos = h % bits;
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

}  // namespace sextant::lsm
