// A non-owning view over a contiguous run of bytes.
//
// Why not std::string_view?  Two reasons.  First, keys and values in this
// engine are arbitrary bytes, not text, and string_view's char-oriented API
// (find, substr, operator<<) invites accidental text semantics.  Second, we
// want a single obvious place to hang engine-specific helpers such as
// starts_with and the bytewise three-way compare used by the comparator.
//
// A Slice is only valid while the memory it points at is.  Slices returned by
// iterators are invalidated by the next iterator movement; slices returned by
// Get() point into a caller-owned std::string.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace sextant::lsm {

class Slice {
 public:
  Slice() noexcept : data_(""), size_(0) {}
  Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}
  Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}
  Slice(std::string_view sv) noexcept : data_(sv.data()), size_(sv.size()) {}
  Slice(const char* s) noexcept : data_(s), size_(std::strlen(s)) {}

  const char* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

  const char* begin() const noexcept { return data_; }
  const char* end() const noexcept { return data_ + size_; }

  char operator[](size_t n) const noexcept {
    assert(n < size_);
    return data_[n];
  }

  void clear() noexcept {
    data_ = "";
    size_ = 0;
  }

  // Drop the first n bytes.
  void remove_prefix(size_t n) noexcept {
    assert(n <= size_);
    data_ += n;
    size_ -= n;
  }

  // Drop the last n bytes.
  void remove_suffix(size_t n) noexcept {
    assert(n <= size_);
    size_ -= n;
  }

  std::string ToString() const { return std::string(data_, size_); }
  std::string_view ToStringView() const noexcept { return {data_, size_}; }

  // Bytewise ordering: < 0, == 0, > 0 as *this is <, ==, > than b.
  // This is the total order the whole engine is built on.  It must be a plain
  // unsigned byte comparison - memcmp gives us exactly that, and it is why the
  // key encodings in src/codec use big-endian integers.
  int compare(const Slice& b) const noexcept {
    const size_t min_len = size_ < b.size_ ? size_ : b.size_;
    int r = std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) return -1;
      if (size_ > b.size_) return 1;
    }
    return r;
  }

  bool starts_with(const Slice& x) const noexcept {
    return size_ >= x.size_ && std::memcmp(data_, x.data_, x.size_) == 0;
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) noexcept {
  return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size()) == 0;
}
inline bool operator!=(const Slice& x, const Slice& y) noexcept { return !(x == y); }
inline bool operator<(const Slice& x, const Slice& y) noexcept { return x.compare(y) < 0; }

}  // namespace sextant::lsm
