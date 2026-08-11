// Status: the engine's error type.
//
// Design note for the interview: this is deliberately not exceptions.  A
// storage engine's hot path (Get, iterator Next) must be allocation-free and
// branch-predictable, and "key not found" is an ordinary control-flow outcome,
// not an exceptional one.  Status costs one pointer when OK - the common case
// allocates nothing at all.
//
// The encoding is the LevelDB trick: state_ is nullptr for OK, otherwise a
// heap block laid out as
//     [ message length : 4 bytes ][ code : 1 byte ][ message : length bytes ]

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "sextant/lsm/slice.h"

namespace sextant::lsm {

class Status {
 public:
  enum class Code : uint8_t {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5,
  };

  Status() noexcept : state_(nullptr) {}
  ~Status() { delete[] state_; }

  Status(const Status& rhs) : state_(CopyState(rhs.state_)) {}
  Status& operator=(const Status& rhs) {
    if (state_ != rhs.state_) {
      delete[] state_;
      state_ = CopyState(rhs.state_);
    }
    return *this;
  }
  Status(Status&& rhs) noexcept : state_(rhs.state_) { rhs.state_ = nullptr; }
  Status& operator=(Status&& rhs) noexcept {
    std::swap(state_, rhs.state_);
    return *this;
  }

  static Status OK() { return Status(); }
  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kNotFound, msg, msg2);
  }
  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kCorruption, msg, msg2);
  }
  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kInvalidArgument, msg, msg2);
  }
  static Status IOError(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(Code::kIOError, msg, msg2);
  }

  bool ok() const noexcept { return state_ == nullptr; }
  bool IsNotFound() const noexcept { return code() == Code::kNotFound; }
  bool IsCorruption() const noexcept { return code() == Code::kCorruption; }
  bool IsNotSupported() const noexcept { return code() == Code::kNotSupported; }
  bool IsInvalidArgument() const noexcept { return code() == Code::kInvalidArgument; }
  bool IsIOError() const noexcept { return code() == Code::kIOError; }

  std::string ToString() const;

 private:
  Code code() const noexcept {
    return state_ == nullptr ? Code::kOk : static_cast<Code>(state_[4]);
  }

  Status(Code code, const Slice& msg, const Slice& msg2);
  static const char* CopyState(const char* s);

  const char* state_;
};

}  // namespace sextant::lsm
