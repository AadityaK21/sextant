#include "coding.h"

namespace sextant::lsm {

char* EncodeVarint32(char* dst, uint32_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  static constexpr int B = 128;
  while (v >= B) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

char* EncodeVarint64(char* dst, uint64_t v) {
  auto* ptr = reinterpret_cast<uint8_t*>(dst);
  static constexpr int B = 128;
  while (v >= B) {
    *(ptr++) = static_cast<uint8_t>(v | B);
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  char* ptr = EncodeVarint32(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, static_cast<size_t>(ptr - buf));
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    auto byte = static_cast<uint32_t>(*reinterpret_cast<const uint8_t*>(p));
    ++p;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;  // truncated or over-long
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    auto byte = static_cast<uint64_t>(*reinterpret_cast<const uint8_t*>(p));
    ++p;
    if (byte & 128) {
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) return false;
  *input = Slice(q, static_cast<size_t>(limit - q));
  return true;
}

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len)) return false;
  if (input->size() < len) return false;
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

}  // namespace sextant::lsm
