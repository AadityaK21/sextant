#include "sextant/lsm/write_batch.h"

#include "coding.h"
#include "internal_key.h"
#include "memtable.h"

#include <cassert>
#include <cstdint>

namespace sextant::lsm {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeaderSize);
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

int WriteBatch::Count() const {
  return static_cast<int>(DecodeFixed32BE(rep_.data() + 8));
}

void WriteBatch::SetCount(int n) {
  EncodeFixed32BE(rep_.data() + 8, static_cast<uint32_t>(n));
}

uint64_t WriteBatch::Sequence() const { return DecodeFixed64BE(rep_.data()); }

void WriteBatch::SetSequence(uint64_t seq) { EncodeFixed64BE(rep_.data(), seq); }

void WriteBatch::SetContents(const Slice& contents) {
  assert(contents.size() >= kHeaderSize);
  rep_.assign(contents.data(), contents.size());
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeaderSize) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  input.remove_prefix(kHeaderSize);

  Slice key, value;
  int found = 0;
  while (!input.empty()) {
    ++found;
    const auto tag = static_cast<ValueType>(static_cast<uint8_t>(input[0]));
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, value);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != Count()) {
    return Status::Corruption("WriteBatch has wrong count");
  }
  return Status::OK();
}

namespace {

// Assigns consecutive sequence numbers within the batch. Every record in a
// batch gets its OWN sequence, not a shared one - otherwise two writes to the
// same key inside one batch would collide in the skiplist, and last-write-wins
// ordering inside the batch would be undefined.
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_ = 0;
  MemTable* mem_ = nullptr;

  void Put(const Slice& key, const Slice& value) override {
    mem_->Add(sequence_, kTypeValue, key, value);
    ++sequence_;
  }
  void Delete(const Slice& key) override {
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    ++sequence_;
  }
};

}  // namespace

Status WriteBatch::InsertInto(MemTable* memtable) const {
  MemTableInserter inserter;
  inserter.sequence_ = Sequence();
  inserter.mem_ = memtable;
  return Iterate(&inserter);
}

}  // namespace sextant::lsm
