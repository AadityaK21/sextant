// ULID: a 128-bit identifier that sorts by creation time.
//
//   ┌───────────────────────────┬─────────────────────────────────┐
//   │ 48 bits: ms since epoch   │ 80 bits: randomness             │
//   │ big-endian                │                                 │
//   └───────────────────────────┴─────────────────────────────────┘
//
// WHY NOT UUIDv4. A random UUID has no ordering, so entities created together
// land in completely unrelated parts of the keyspace. That destroys two things
// this system depends on:
//
//   1. LOCALITY. Entities resolved in the same batch are read together. With a
//      time-ordered id they share a key prefix, so they sit in the same
//      SSTable blocks and the block cache holds them as a unit.
//   2. PREFIX COMPRESSION. Adjacent keys sharing a long prefix is exactly what
//      the block format exploits. Random ids share nothing.
//
// WHY NOT AN AUTOINCREMENT COUNTER. It requires coordination - a lock or a
// round trip - on every id. A ULID needs neither: the clock and a CSPRNG are
// enough, so id generation stays a local, lock-free operation that would still
// work if this were sharded across machines.
//
// MONOTONICITY. Two ULIDs generated in the same millisecond would otherwise be
// ordered at random. The spec's answer, implemented here: within a millisecond,
// increment the previous random component instead of drawing a new one. Ids
// then strictly increase even under a burst faster than the clock resolution.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "sextant/lsm/slice.h"

namespace sextant::codec {

using lsm::Slice;

class Ulid {
 public:
  static constexpr size_t kBinarySize = 16;
  static constexpr size_t kTextSize = 26;  // base32 of 128 bits

  Ulid() : bytes_{} {}
  explicit Ulid(const std::array<uint8_t, kBinarySize>& b) : bytes_(b) {}

  // Generate from the system clock. Thread-safe and monotonic.
  static Ulid Generate();

  // Deterministic construction, for tests and for replaying a batch.
  static Ulid FromParts(uint64_t timestamp_ms, uint64_t random_hi, uint64_t random_lo);

  static bool FromBinary(const Slice& in, Ulid* out);
  static bool FromString(const std::string& text, Ulid* out);

  Slice AsSlice() const {
    return Slice(reinterpret_cast<const char*>(bytes_.data()), kBinarySize);
  }
  const uint8_t* data() const { return bytes_.data(); }

  // Crockford base32: 32 symbols excluding I, L, O and U so that a human
  // transcribing an id cannot confuse them with 1 and 0.
  std::string ToString() const;

  uint64_t TimestampMs() const;

  bool operator==(const Ulid& other) const { return bytes_ == other.bytes_; }
  bool operator!=(const Ulid& other) const { return !(*this == other); }
  // Byte order equals creation order - the whole point of the layout.
  bool operator<(const Ulid& other) const { return bytes_ < other.bytes_; }

 private:
  std::array<uint8_t, kBinarySize> bytes_;
};

}  // namespace sextant::codec
