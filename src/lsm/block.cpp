#include "block.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

#include "coding.h"

namespace sextant::lsm {

uint32_t Block::NumRestarts() const {
  assert(size_ >= sizeof(uint32_t));
  return DecodeFixed32BE(data_ + size_ - sizeof(uint32_t));
}

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_(contents.heap_allocated) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // malformed
    return;
  }
  const size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
  if (NumRestarts() > max_restarts_allowed) {
    size_ = 0;  // the restart count cannot fit; the block is corrupt
    return;
  }
  restart_offset_ =
      static_cast<uint32_t>(size_ - (1 + NumRestarts()) * sizeof(uint32_t));
}

Block::~Block() {
  if (owned_) delete[] data_;
}

namespace {

// Decode one entry header. The fast path exploits the fact that in the
// overwhelming majority of entries all three lengths fit in a single byte, so
// the three varints can be read with three array accesses and no branching.
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                        uint32_t* non_shared, uint32_t* value_length) {
  if (limit - p < 3) return nullptr;
  *shared = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p)[0]);
  *non_shared = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p)[1]);
  *value_length = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p)[2]);

  if ((*shared | *non_shared | *value_length) < 128) {
    p += 3;  // all three are single-byte varints
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }

  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return nullptr;
  }
  return p;
}

}  // namespace

class Block::Iter final : public Iterator {
 public:
  Iter(const InternalKeyComparator& comparator, const char* data,
       uint32_t restarts, uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts_),
        restart_index_(num_restarts_) {
    assert(num_restarts_ > 0);
  }

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }

  Slice key() const override {
    assert(Valid());
    return Slice(key_);
  }
  Slice value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void Prev() override {
    assert(Valid());
    // No back pointers. Walk back to the restart point before the current
    // entry, then decode forward until we land just before where we were.
    // Prev is rare; paying O(restart_interval) for it beats one extra pointer
    // per entry in the on-disk format.
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        current_ = restarts_;  // moved before the first entry
        restart_index_ = num_restarts_;
        return;
      }
      --restart_index_;
    }
    SeekToRestartPoint(restart_index_);
    do {
    } while (ParseNextKey() && NextEntryOffset() < original);
  }

  void Seek(const Slice& target) override {
    // Binary search the restart array for the last restart point whose key is
    // <= target. Restart keys are stored uncompressed, so each probe is a
    // single decode with no dependency on any other entry.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;

    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr =
          DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                      &non_shared, &value_length);
      if (key_ptr == nullptr || (shared != 0)) {
        CorruptionError();
        return;
      }
      const Slice mid_key(key_ptr, non_shared);
      if (comparator_.Compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    // Then scan forward from that restart point. At most restart_interval
    // entries get decoded.
    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) return;
      if (comparator_.Compare(Slice(key_), target) >= 0) return;
    }
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restarts_) {
    }
  }

 private:
  const InternalKeyComparator comparator_;
  const char* const data_;
  const uint32_t restarts_;      // offset of the restart array
  const uint32_t num_restarts_;

  uint32_t current_;             // offset of the current entry
  uint32_t restart_index_;       // restart block the current entry lives in
  std::string key_;
  Slice value_;
  Status status_;

  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  uint32_t GetRestartPoint(uint32_t index) const {
    assert(index < num_restarts_);
    return DecodeFixed32BE(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    // ParseNextKey starts from value_'s end, so point value_ at a zero-length
    // slice sitting exactly at the restart offset.
    const uint32_t offset = GetRestartPoint(index);
    value_ = Slice(data_ + offset, 0);
  }

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_.clear();
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) {
      current_ = restarts_;  // past the last entry
      restart_index_ = num_restarts_;
      return false;
    }

    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }

    key_.resize(shared);                    // keep the shared prefix
    key_.append(p, non_shared);             // append the delta
    value_ = Slice(p + non_shared, value_length);

    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }
};

Iterator* Block::NewIterator(const InternalKeyComparator& comparator) {
  if (size_ < sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("block is too small"));
  }
  const uint32_t num_restarts = NumRestarts();
  if (num_restarts == 0) return NewEmptyIterator();
  return new Iter(comparator, data_, restart_offset_, num_restarts);
}

}  // namespace sextant::lsm
