#include "sextant/lsm/iterator.h"

#include <cassert>

namespace sextant::lsm {

Iterator::Iterator() { cleanup_head_.function = nullptr; }

Iterator::~Iterator() {
  if (!cleanup_head_.IsEmpty()) {
    cleanup_head_.Run();
    for (CleanupNode* node = cleanup_head_.next; node != nullptr;) {
      node->Run();
      CleanupNode* next = node->next;
      delete node;
      node = next;
    }
  }
}

void Iterator::RegisterCleanup(CleanupFunction func, void* arg1, void* arg2) {
  CleanupNode* node;
  if (cleanup_head_.IsEmpty()) {
    node = &cleanup_head_;
  } else {
    node = new CleanupNode();
    node->next = cleanup_head_.next;
    cleanup_head_.next = node;
  }
  node->function = func;
  node->arg1 = arg1;
  node->arg2 = arg2;
}

namespace {

class EmptyIterator : public Iterator {
 public:
  explicit EmptyIterator(const Status& s) : status_(s) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice&) override {}
  void Next() override { assert(false); }
  void Prev() override { assert(false); }
  Slice key() const override {
    assert(false);
    return Slice();
  }
  Slice value() const override {
    assert(false);
    return Slice();
  }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

Iterator* NewEmptyIterator() { return new EmptyIterator(Status::OK()); }

Iterator* NewErrorIterator(const Status& status) { return new EmptyIterator(status); }

}  // namespace sextant::lsm
