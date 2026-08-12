#include "db_iter.h"

#include <cassert>
#include <memory>
#include <string>

namespace sextant::lsm {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(const InternalKeyComparator& comparator, Iterator* internal_iter,
         SequenceNumber snapshot)
      : comparator_(comparator), iter_(internal_iter), sequence_(snapshot) {}

  ~DBIter() override = default;

  bool Valid() const override { return valid_; }

  Slice key() const override {
    assert(valid_);
    return Slice(saved_key_);
  }

  Slice value() const override {
    assert(valid_);
    return saved_value_;
  }

  Status status() const override {
    if (!status_.ok()) return status_;
    return iter_->status();
  }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

  void Seek(const Slice& target) override {
    // Seek to (target, snapshot, kValueTypeForSeek). Because the trailer sorts
    // descending, that lands at or before the newest version of target that is
    // visible at this snapshot - never on a version we are not allowed to see.
    //
    // The internal key goes in its own scratch buffer. saved_key_ holds a USER
    // key at all times; mixing the two representations in one field is exactly
    // the bug this comment exists to prevent a repeat of.
    seek_scratch_.clear();
    AppendInternalKey(&seek_scratch_,
                      ParsedInternalKey(target, sequence_, kValueTypeForSeek));
    iter_->Seek(Slice(seek_scratch_));
    FindNextUserEntry(/*skipping=*/false);
  }

  void Next() override {
    assert(valid_);
    // saved_key_ currently holds the user key we just returned; skip every
    // remaining (older) version of it.
    FindNextUserEntry(/*skipping=*/true);
  }

  void SeekToLast() override {
    status_ = Status::NotSupported("DBIter is forward-only; see db.h");
    valid_ = false;
  }

  void Prev() override {
    status_ = Status::NotSupported("DBIter is forward-only; see db.h");
    valid_ = false;
  }

 private:
  // Advance until a live, visible entry is found.
  //
  // `skipping` means "we have already emitted saved_key_, so discard any
  // further versions of it". That is also how a tombstone works: on seeing a
  // deletion we remember the key and set skipping, which swallows every older
  // version below it. The tombstone hides the value without anything having to
  // compare sequence numbers.
  void FindNextUserEntry(bool skipping) {
    // saved_key_ is already a user key - do NOT run ExtractUserKey on it.
    std::string skip_key;
    if (skipping) skip_key = saved_key_;

    while (iter_->Valid()) {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(iter_->key(), &parsed)) {
        status_ = Status::Corruption("corrupt internal key in iterator");
        valid_ = false;
        return;
      }

      if (parsed.sequence <= sequence_) {  // visible at our snapshot
        if (skipping && parsed.user_key.compare(Slice(skip_key)) == 0) {
          // An older version of a key we have already dealt with.
        } else if (parsed.type == kTypeDeletion) {
          // A tombstone: remember the key and skip everything older.
          skip_key = parsed.user_key.ToString();
          skipping = true;
        } else {
          saved_key_ = parsed.user_key.ToString();
          saved_value_ = iter_->value();
          valid_ = true;
          return;
        }
      }
      iter_->Next();
    }

    saved_key_.clear();
    valid_ = false;
  }

  const InternalKeyComparator comparator_;
  std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;

  std::string saved_key_;    // the USER key currently exposed
  std::string seek_scratch_; // internal-key buffer used only by Seek
  Slice saved_value_;
  bool valid_ = false;
  Status status_;
};

}  // namespace

Iterator* NewDBIterator(const InternalKeyComparator& comparator,
                        Iterator* internal_iter, SequenceNumber snapshot) {
  return new DBIter(comparator, internal_iter, snapshot);
}

}  // namespace sextant::lsm
