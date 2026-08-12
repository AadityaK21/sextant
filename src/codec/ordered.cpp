#include "ordered.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace sextant::codec {
namespace {

constexpr char kEscape = '\x00';
constexpr char kEscaped = '\xFF';  // 0x00 0xFF means "a literal NUL"
constexpr char kTerminator = '\x00';  // 0x00 0x00 means "end of value"

void PutBigEndian64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
  }
  dst->append(buf, 8);
}

bool GetBigEndian64(Slice* input, uint64_t* out) {
  if (input->size() < 8) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(input->data());
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  *out = v;
  input->remove_prefix(8);
  return true;
}

}  // namespace

// --- strings ---------------------------------------------------------------

void EncodeOrderedString(std::string* dst, const Slice& value) {
  dst->reserve(dst->size() + value.size() + 2);
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    dst->push_back(c);
    if (c == kEscape) dst->push_back(kEscaped);
  }
  dst->push_back(kEscape);
  dst->push_back(kTerminator);
}

bool DecodeOrderedString(Slice* input, std::string* out) {
  out->clear();
  size_t i = 0;
  while (i < input->size()) {
    const char c = (*input)[i];
    if (c != kEscape) {
      out->push_back(c);
      ++i;
      continue;
    }
    // A NUL: look at the next byte to decide terminator vs escaped literal.
    if (i + 1 >= input->size()) return false;  // truncated
    const char next = (*input)[i + 1];
    if (next == kTerminator) {
      input->remove_prefix(i + 2);
      return true;
    }
    if (next == kEscaped) {
      out->push_back(kEscape);
      i += 2;
      continue;
    }
    return false;  // malformed escape sequence
  }
  return false;  // no terminator found
}

// --- integers --------------------------------------------------------------

void EncodeOrderedUint64(std::string* dst, uint64_t value) {
  PutBigEndian64(dst, value);  // already sorts correctly as unsigned bytes
}

bool DecodeOrderedUint64(Slice* input, uint64_t* out) {
  return GetBigEndian64(input, out);
}

void EncodeOrderedInt64(std::string* dst, int64_t value) {
  // Flip the sign bit: this maps [INT64_MIN, INT64_MAX] onto
  // [0, UINT64_MAX] monotonically, so negatives now sort below positives.
  const uint64_t biased =
      static_cast<uint64_t>(value) ^ (uint64_t{1} << 63);
  PutBigEndian64(dst, biased);
}

bool DecodeOrderedInt64(Slice* input, int64_t* out) {
  uint64_t biased;
  if (!GetBigEndian64(input, &biased)) return false;
  *out = static_cast<int64_t>(biased ^ (uint64_t{1} << 63));
  return true;
}

// --- doubles ---------------------------------------------------------------

void EncodeOrderedDouble(std::string* dst, double value) {
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
  std::memcpy(&bits, &value, sizeof(bits));

  // Positive doubles already ascend as unsigned integers, but they sit below
  // negatives (whose sign bit is set). Negatives ascend in magnitude while
  // descending in value, so they must be reversed as well.
  //
  //   sign bit clear (>= 0)  ->  set it, lifting positives above negatives
  //   sign bit set   (<  0)  ->  flip everything, reversing their order
  if ((bits & (uint64_t{1} << 63)) == 0) {
    bits |= (uint64_t{1} << 63);
  } else {
    bits = ~bits;
  }
  PutBigEndian64(dst, bits);
}

bool DecodeOrderedDouble(Slice* input, double* out) {
  uint64_t bits;
  if (!GetBigEndian64(input, &bits)) return false;

  if ((bits & (uint64_t{1} << 63)) != 0) {
    bits &= ~(uint64_t{1} << 63);  // was non-negative
  } else {
    bits = ~bits;                  // was negative
  }
  std::memcpy(out, &bits, sizeof(*out));
  return true;
}

}  // namespace sextant::codec
