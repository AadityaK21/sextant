#include "ulid.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <string>

namespace sextant::codec {
namespace {

// Crockford base32. Deliberately excludes I, L, O and U: the first three are
// confusable with 1 and 0 when read aloud or transcribed, and U is excluded to
// avoid accidentally spelling things.
constexpr char kEncoding[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int DecodeChar(char c) {
  // Crockford decoding is case-insensitive and treats I/L as 1 and O as 0,
  // which is the point: a human-transcribed id still parses.
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  switch (c) {
    case 'I': case 'L': return 1;
    case 'O': return 0;
    default: break;
  }
  for (int i = 10; i < 32; ++i) {
    if (kEncoding[i] == c) return i;
  }
  return -1;
}

struct Generator {
  std::mutex mutex;
  uint64_t last_ms = 0;
  uint64_t random_hi = 0;  // top 16 bits of the 80-bit random field
  uint64_t random_lo = 0;  // bottom 64 bits
  std::mt19937_64 rng{std::random_device{}()};
};

Generator& Gen() {
  static Generator g;
  return g;
}

}  // namespace

Ulid Ulid::FromParts(uint64_t timestamp_ms, uint64_t random_hi, uint64_t random_lo) {
  std::array<uint8_t, kBinarySize> b{};

  // 48-bit timestamp, big-endian, so byte order equals chronological order.
  for (int i = 0; i < 6; ++i) {
    b[static_cast<size_t>(i)] =
        static_cast<uint8_t>((timestamp_ms >> (40 - 8 * i)) & 0xFF);
  }
  // 16 high bits of randomness.
  b[6] = static_cast<uint8_t>((random_hi >> 8) & 0xFF);
  b[7] = static_cast<uint8_t>(random_hi & 0xFF);
  // 64 low bits.
  for (int i = 0; i < 8; ++i) {
    b[static_cast<size_t>(8 + i)] =
        static_cast<uint8_t>((random_lo >> (56 - 8 * i)) & 0xFF);
  }
  return Ulid(b);
}

Ulid Ulid::Generate() {
  auto& g = Gen();
  std::lock_guard<std::mutex> lock(g.mutex);

  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  if (now == g.last_ms) {
    // Same millisecond: increment rather than redraw, so ids stay strictly
    // increasing during a burst faster than the clock ticks.
    if (++g.random_lo == 0) ++g.random_hi;  // carry into the high 16 bits
  } else {
    g.last_ms = now;
    g.random_hi = g.rng() & 0xFFFF;
    g.random_lo = g.rng();
  }

  return FromParts(now, g.random_hi, g.random_lo);
}

bool Ulid::FromBinary(const Slice& in, Ulid* out) {
  if (in.size() != kBinarySize) return false;
  std::array<uint8_t, kBinarySize> b{};
  std::memcpy(b.data(), in.data(), kBinarySize);
  *out = Ulid(b);
  return true;
}

uint64_t Ulid::TimestampMs() const {
  uint64_t ts = 0;
  for (int i = 0; i < 6; ++i) ts = (ts << 8) | bytes_[static_cast<size_t>(i)];
  return ts;
}

std::string Ulid::ToString() const {
  // 128 bits does not divide evenly into 5-bit symbols. 26 symbols hold 130
  // bits, so the FIRST symbol carries only 3 real bits and the remaining 25
  // carry 125 - exactly 128 in total, with no trailing partial symbol.
  //
  // Getting this wrong by one bit is not a formatting nit: treating the first
  // symbol as 2 bits leaves 126 bits for 25 symbols, which needs a 27th to
  // flush the remainder and writes off the end of the string.
  std::string out(kTextSize, '0');

  out[0] = kEncoding[(bytes_[0] >> 5) & 0x07];  // top 3 bits

  uint32_t buffer = bytes_[0] & 0x1F;           // 5 bits carried forward
  int bits = 5;
  size_t pos = 1;

  for (size_t i = 1; i < kBinarySize; ++i) {
    buffer = (buffer << 8) | bytes_[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out[pos++] = kEncoding[(buffer >> bits) & 0x1F];
    }
  }
  assert(pos == kTextSize && bits == 0);
  return out;
}

bool Ulid::FromString(const std::string& text, Ulid* out) {
  if (text.size() != kTextSize) return false;

  std::array<uint8_t, kBinarySize> b{};
  uint32_t buffer = 0;
  int bits = 0;
  size_t pos = 0;

  // Mirror of ToString: the first symbol holds 3 bits, so anything above 7
  // would overflow 128 bits and cannot be a valid id.
  const int first = DecodeChar(text[0]);
  if (first < 0 || first > 7) return false;
  buffer = static_cast<uint32_t>(first);
  bits = 3;

  for (size_t i = 1; i < kTextSize; ++i) {
    const int v = DecodeChar(text[i]);
    if (v < 0) return false;
    buffer = (buffer << 5) | static_cast<uint32_t>(v);
    bits += 5;
    while (bits >= 8 && pos < kBinarySize) {
      bits -= 8;
      b[pos++] = static_cast<uint8_t>((buffer >> bits) & 0xFF);
    }
  }
  if (pos != kBinarySize) return false;

  *out = Ulid(b);
  return true;
}

}  // namespace sextant::codec
