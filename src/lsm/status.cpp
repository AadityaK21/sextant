#include "sextant/lsm/status.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace sextant::lsm {

const char* Status::CopyState(const char* s) {
  if (s == nullptr) return nullptr;
  uint32_t size;
  std::memcpy(&size, s, sizeof(size));
  char* result = new char[size + 5];
  std::memcpy(result, s, size + 5);
  return result;
}

Status::Status(Code code, const Slice& msg, const Slice& msg2) {
  const uint32_t len1 = static_cast<uint32_t>(msg.size());
  const uint32_t len2 = static_cast<uint32_t>(msg2.size());
  const uint32_t size = len1 + (len2 ? (2 + len2) : 0);

  char* result = new char[size + 5];
  std::memcpy(result, &size, sizeof(size));
  result[4] = static_cast<char>(code);
  std::memcpy(result + 5, msg.data(), len1);
  if (len2) {
    result[5 + len1] = ':';
    result[6 + len1] = ' ';
    std::memcpy(result + 7 + len1, msg2.data(), len2);
  }
  state_ = result;
}

std::string Status::ToString() const {
  if (state_ == nullptr) return "OK";

  const char* type;
  char tmp[32];
  switch (code()) {
    case Code::kOk:              type = "OK"; break;
    case Code::kNotFound:        type = "NotFound: "; break;
    case Code::kCorruption:      type = "Corruption: "; break;
    case Code::kNotSupported:    type = "Not implemented: "; break;
    case Code::kInvalidArgument: type = "Invalid argument: "; break;
    case Code::kIOError:         type = "IO error: "; break;
    default:
      std::snprintf(tmp, sizeof(tmp), "Unknown code(%d): ",
                    static_cast<int>(code()));
      type = tmp;
      break;
  }

  std::string result(type);
  uint32_t length;
  std::memcpy(&length, state_, sizeof(length));
  result.append(state_ + 5, length);
  return result;
}

}  // namespace sextant::lsm
