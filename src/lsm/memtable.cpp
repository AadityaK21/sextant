#include "memtable.h"

#include "coding.h"

namespace sextant::lsm {
namespace {

// Decode a length-prefixed slice written by PutLengthPrefixedSlice.
Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len = 0;
  const char* p = data;
  p = GetVarint32Ptr(p, p + 5, &len);  // +5: max varint32 length
  return Slice(p, len);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* aptr, const char* bptr) const {
  const Slice a = GetLengthPrefixedSlice(aptr);
  const Slice b = GetLengthPrefixedSlice(bptr);
  return comparator.Compare(a, b);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : key_comparator_(comparator), table_(key_comparator_, &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key,
                   const Slice& value) {
  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  const size_t encoded_len = static_cast<size_t>(VarintLength(internal_key_size)) +
                             internal_key_size +
                             static_cast<size_t>(VarintLength(val_size)) + val_size;

  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64BE(p, PackSequenceAndType(seq, type));
  p += 8;
  p = EncodeVarint32(p, static_cast<uint32_t>(val_size));
  std::memcpy(p, value.data(), val_size);
  assert(p + val_size == buf + encoded_len);

  table_.Insert(buf);
  ++num_entries_;
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  const Slice memkey = key.memtable_key();

  Table::Iterator iter(&table_);
  iter.Seek(memkey.data());

  if (!iter.Valid()) return false;

  // Seek landed on the first entry >= (user_key, seq, kValueTypeForSeek).
  // Because the trailer sorts descending, that is the NEWEST version of this
  // user key that is visible at this sequence number. We only have to check
  // that the user key actually matches - no version loop needed.
  const char* entry = iter.key();
  uint32_t key_length = 0;
  const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);

  const Slice found_user_key(key_ptr, key_length - 8);
  if (found_user_key.compare(key.user_key()) != 0) {
    return false;  // different user key entirely
  }

  const uint64_t tag = DecodeFixed64BE(key_ptr + key_length - 8);
  switch (static_cast<ValueType>(tag & 0xff)) {
    case kTypeValue: {
      const Slice v = GetLengthPrefixedSlice(key_ptr + key_length);
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case kTypeDeletion:
      // Found a tombstone. Report "handled" so the caller stops searching, but
      // report NotFound as the outcome.
      *s = Status::NotFound(Slice());
      return true;
  }
  return false;
}

namespace {

// Thin bridge from MemTable::Iterator (a concrete walker) to the polymorphic
// Iterator the merge path consumes.
class MemTableIteratorAdapter final : public lsm::Iterator {
 public:
  explicit MemTableIteratorAdapter(const MemTable* mem) : iter_(mem) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Seek(const Slice& target) override { iter_.Seek(target); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }
  Slice key() const override { return iter_.key(); }
  Slice value() const override { return iter_.value(); }
  Status status() const override { return Status::OK(); }

 private:
  MemTable::Iterator iter_;
};

}  // namespace

lsm::Iterator* MemTable::NewIterator() const {
  return new MemTableIteratorAdapter(this);
}

void MemTable::Iterator::Seek(const Slice& internal_key) {
  tmp_.clear();
  PutLengthPrefixedSlice(&tmp_, internal_key);
  iter_.Seek(tmp_.data());
}

Slice MemTable::Iterator::key() const {
  return GetLengthPrefixedSlice(iter_.key());
}

Slice MemTable::Iterator::value() const {
  const Slice k = GetLengthPrefixedSlice(iter_.key());
  return GetLengthPrefixedSlice(k.data() + k.size());
}

}  // namespace sextant::lsm
