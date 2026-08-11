#include "block_builder.h"

#include <cassert>
#include <cstring>

#include "coding.h"

namespace sextant::lsm {

BlockBuilder::BlockBuilder(int restart_interval)
    : restart_interval_(restart_interval) {
  assert(restart_interval_ >= 1);
  restarts_.push_back(0);  // the first entry is always a restart point
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() +                       // entries
         restarts_.size() * sizeof(uint32_t) +  // restart array
         sizeof(uint32_t);                      // restart count
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  const Slice last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= restart_interval_);
  // Order under the InternalKeyComparator, NOT bytewise. Getting this wrong
  // costs nothing in a release build (asserts vanish) and fires immediately in
  // Debug the moment two versions of one user key land in the same block.
  assert(buffer_.empty() || comparator_.Compare(key, last_key_piece) > 0);

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    // How much of the previous key does this one reuse?
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while (shared < min_length && last_key_piece[shared] == key[shared]) {
      ++shared;
    }
  } else {
    // Restart point: store the key in full so it can be decoded standalone.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);

  ++counter_;
}

Slice BlockBuilder::Finish() {
  for (uint32_t restart : restarts_) {
    PutFixed32BE(&buffer_, restart);
  }
  PutFixed32BE(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

}  // namespace sextant::lsm
