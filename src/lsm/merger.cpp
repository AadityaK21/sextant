#include "merger.h"

#include <cassert>

namespace sextant::lsm {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(const InternalKeyComparator& comparator,
                  std::vector<Iterator*> children)
      : comparator_(comparator), current_(nullptr), direction_(Direction::kForward) {
    children_.reserve(children.size());
    for (Iterator* child : children) {
      children_.emplace_back(child);
    }
  }

  ~MergingIterator() override = default;

  bool Valid() const override { return current_ != nullptr; }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    FindSmallest();
    direction_ = Direction::kForward;
  }

  void SeekToLast() override {
    for (auto& child : children_) child->SeekToLast();
    FindLargest();
    direction_ = Direction::kReverse;
  }

  void Seek(const Slice& target) override {
    for (auto& child : children_) child->Seek(target);
    FindSmallest();
    direction_ = Direction::kForward;
  }

  void Next() override {
    assert(Valid());

    // Reversing direction is the subtle part. When moving backwards, every
    // child other than current_ sits at an entry <= the current key. To go
    // forward they must first be repositioned to the first entry > current key.
    if (direction_ != Direction::kForward) {
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->Seek(key());
        if (child->Valid() && comparator_.Compare(key(), child->key()) == 0) {
          child->Next();  // it landed exactly on our key; step past it
        }
      }
      direction_ = Direction::kForward;
    }

    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    assert(Valid());

    if (direction_ != Direction::kReverse) {
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->Seek(key());
        if (child->Valid()) {
          child->Prev();      // land just before our key
        } else {
          child->SeekToLast();  // key is past this child's end
        }
      }
      direction_ = Direction::kReverse;
    }

    current_->Prev();
    FindLargest();
  }

  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  Status status() const override {
    for (const auto& child : children_) {
      if (!child->status().ok()) return child->status();
    }
    return Status::OK();
  }

 private:
  enum class Direction { kForward, kReverse };

  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& child : children_) {
      if (!child->Valid()) continue;
      if (smallest == nullptr ||
          comparator_.Compare(child->key(), smallest->key()) < 0) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  void FindLargest() {
    Iterator* largest = nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
      if (!(*it)->Valid()) continue;
      if (largest == nullptr ||
          comparator_.Compare((*it)->key(), largest->key()) > 0) {
        largest = it->get();
      }
    }
    current_ = largest;
  }

  const InternalKeyComparator comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_;
  Direction direction_;
};

}  // namespace

Iterator* NewMergingIterator(const InternalKeyComparator& comparator,
                             std::vector<Iterator*> children) {
  if (children.empty()) return NewEmptyIterator();
  if (children.size() == 1) return children[0];
  return new MergingIterator(comparator, std::move(children));
}

}  // namespace sextant::lsm
