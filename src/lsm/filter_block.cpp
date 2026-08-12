#include "filter_block.h"

#include "coding.h"

namespace sextant::lsm {

FilterBlockBuilder::FilterBlockBuilder(const BloomFilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  const uint64_t filter_index = block_offset / kFilterBase;
  assert(filter_index >= filter_offsets_.size());
  // A data block larger than kFilterBase skips several filter slots. Emit
  // empty filters for the gap so that index arithmetic stays a plain division
  // rather than a search.
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& user_key) {
  start_.push_back(keys_.size());
  keys_.append(user_key.data(), user_key.size());
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // No keys landed in this range. Record the current position so the slot
    // exists and decodes as a zero-length filter.
    filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
    return;
  }

  // Rebuild the key list as slices into keys_.
  start_.push_back(keys_.size());  // sentinel so the last length is computable
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; ++i) {
    const char* base = keys_.data() + start_[i];
    const size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
  policy_->CreateFilter(tmp_keys_.data(), static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

Slice FilterBlockBuilder::Finish() {
  if (!start_.empty()) GenerateFilter();

  const uint32_t array_offset = static_cast<uint32_t>(result_.size());
  for (uint32_t offset : filter_offsets_) {
    PutFixed32BE(&result_, offset);
  }
  PutFixed32BE(&result_, array_offset);
  result_.push_back(static_cast<char>(kFilterBaseLg));
  return Slice(result_);
}

FilterBlockReader::FilterBlockReader(const BloomFilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy) {
  const size_t n = contents.size();
  if (n < 5) return;  // 1 byte base_lg + 4 byte array offset, minimum

  base_lg_ = static_cast<size_t>(static_cast<uint8_t>(contents[n - 1]));
  const uint32_t last_word = DecodeFixed32BE(contents.data() + n - 5);
  if (last_word > n - 5) return;  // malformed

  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset,
                                    const Slice& user_key) const {
  const uint64_t index = block_offset >> base_lg_;
  if (index >= num_) {
    // No filter covers this offset. Answer "maybe" - a filter may only ever
    // produce false POSITIVES. A false negative would silently lose data.
    return true;
  }

  const uint32_t start = DecodeFixed32BE(offset_ + index * 4);
  const uint32_t limit = DecodeFixed32BE(offset_ + index * 4 + 4);

  if (start <= limit && limit <= static_cast<uint32_t>(offset_ - data_)) {
    const Slice filter(data_ + start, limit - start);
    return policy_->KeyMayMatch(user_key, filter);
  }
  if (start == limit) {
    return false;  // an empty filter means no keys in this range at all
  }
  return true;  // malformed; fail open
}

}  // namespace sextant::lsm
