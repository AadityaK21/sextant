#include "internal_key.h"

#include <cstdio>

namespace sextant::lsm {

std::string ParsedInternalKey::DebugString() const {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "' @ %llu : %d",
                static_cast<unsigned long long>(sequence),
                static_cast<int>(type));
  std::string result = "'";
  result += user_key.ToString();
  result += buf;
  return result;
}

bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
  const size_t n = internal_key.size();
  if (n < 8) return false;
  const uint64_t num = DecodeFixed64BE(internal_key.data() + n - 8);
  const auto c = static_cast<uint8_t>(num & 0xff);
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  return c <= static_cast<uint8_t>(kTypeValue);
}

int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
  // Primary: user key ascending, bytewise.
  int r = ExtractUserKey(akey).compare(ExtractUserKey(bkey));
  if (r == 0) {
    // Secondary: trailer DESCENDING, so the newest sequence sorts first.
    const uint64_t anum = ExtractTrailer(akey);
    const uint64_t bnum = ExtractTrailer(bkey);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

int InternalKeyComparator::Compare(const ParsedInternalKey& a,
                                   const ParsedInternalKey& b) const {
  int r = a.user_key.compare(b.user_key);
  if (r == 0) {
    const uint64_t anum = PackSequenceAndType(a.sequence, a.type);
    const uint64_t bnum = PackSequenceAndType(b.sequence, b.type);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

LookupKey::LookupKey(const Slice& user_key, SequenceNumber s) {
  const size_t usize = user_key.size();
  const size_t needed = usize + 13;  // varint32 (<=5) + user key + trailer (8)

  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64BE(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

}  // namespace sextant::lsm
