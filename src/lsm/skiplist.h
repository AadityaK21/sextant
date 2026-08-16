// Concurrent skiplist: one writer, many lock-free readers.
//
// WHY A SKIPLIST AND NOT A RED-BLACK TREE:
//
//   1. Concurrency.  A balanced tree rebalances by ROTATING, which mutates
//      pointers that readers are actively walking.  You would need a lock or
//      an RCU scheme.  A skiplist only ever splices a new node in, so a reader
//      following forward pointers can never observe a broken structure.
//   2. Simplicity.  Insert is "pick a random height, splice".  There are no
//      rebalancing cases and no colour invariants.  ~200 lines versus a
//      correct concurrent RB-tree, which is considerably more.
//   3. Range scans are trivial - walk level 0.
//
//   Cost: O(log n) is probabilistic rather than worst-case guaranteed, and
//   there is pointer overhead of 1/(1-p) = 1.33 pointers per node at p = 1/4.
//
// THE MEMORY-ORDERING ARGUMENT (be ready to give this one precisely):
//
//   A node is fully constructed - key written, next pointers set - BEFORE it
//   is published.  Publication is a single release-store into the predecessor's
//   next pointer.  A reader that acquire-loads that pointer and sees the new
//   node is therefore guaranteed to see the fully-initialised node.  That
//   release/acquire pair is the entire synchronisation protocol; there is no
//   lock on the read path at all.
//
//   Readers may run concurrently with the writer.  Multiple writers are NOT
//   supported - that is the DB's job, and it holds a mutex over the write path.
//   This is deliberate: the write path is already serialised by the WAL append,
//   so a lock-free writer would buy nothing.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>      // placement new
#include <random>

#include "arena.h"

namespace sextant::lsm {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Insert key. REQUIRES: nothing equal to key is currently in the list.
  // Caller must be the single writer.
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  class Iterator {
   public:
    explicit Iterator(const SkipList* list);

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Prev();
    void Seek(const Key& target);
    void SeekToFirst();
    void SeekToLast();

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  enum { kMaxHeight = 12 };

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }
  bool KeyIsAfterNode(const Key& key, Node* n) const;

  // Lowest node with a key >= the target. If prev is non-null, fill it with
  // the predecessor at every level - that is exactly what Insert needs.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
  Node* FindLessThan(const Key& key) const;
  Node* FindLast() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;

  // Only ever modified by the writer; read racily by readers, which is safe
  // because a stale (smaller) height only costs a slower search, never a wrong
  // answer.
  std::atomic<int> max_height_;

  std::mt19937 rnd_;
};

// --- Node ------------------------------------------------------------------

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  Node* Next(int n) {
    assert(n >= 0);
    // Acquire: pairs with the release-store in SetNext. Guarantees that if we
    // observe this node, we observe its fully-initialised contents.
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_release);
  }

  // Used only while the node is still private to the writer (not yet linked
  // into the list), so no ordering guarantees are required.
  Node* NoBarrier_Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }
  void NoBarrier_SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_relaxed);
  }

 private:
  // Flexible array: allocated with (height) entries. next_[0] is the lowest
  // level, which forms the fully-linked sorted list.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (static_cast<size_t>(height) - 1));
  return new (node_memory) Node(key);
}

// --- Iterator --------------------------------------------------------------

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list)
    : list_(list), node_(nullptr) {}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
  // No back pointers - walk forward from the head instead. Prev is rare
  // (reverse iteration), so paying O(log n) for it beats one extra pointer per
  // node for every node in the list.
  assert(Valid());
  node_ = list_->FindLessThan(node_->key);
  if (node_ == list_->head_) node_ = nullptr;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
  node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) node_ = nullptr;
}

// --- SkipList --------------------------------------------------------------

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // p = 1/4. Expected pointers per node = 1/(1-p) = 1.33.
  static constexpr unsigned int kBranching = 4;
  int height = 1;
  while (height < kMaxHeight && (rnd_() % kBranching) == 0) {
    ++height;
  }
  assert(height > 0 && height <= kMaxHeight);
  return height;
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
  return (n != nullptr) && (compare_(n->key, key) < 0);
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key, Node** prev) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep going right at this level
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;  // drop down
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    assert(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    if (next == nullptr || compare_(next->key, key) >= 0) {
      if (level == 0) return x;
      --level;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) return x;
      --level;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(Key{}, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; ++i) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);

  // Duplicate internal keys are impossible: the sequence number is part of the
  // key and is unique per write.
  assert(x == nullptr || !Equal(key, x->key));

  const int height = RandomHeight();
  if (height > max_height_.load(std::memory_order_relaxed)) {
    for (int i = max_height_.load(std::memory_order_relaxed); i < height; ++i) {
      prev[i] = head_;
    }
    // Racy with concurrent readers, and deliberately so. A reader that sees the
    // old (smaller) height simply searches from a lower level: slower, never
    // wrong. A reader that sees the new height finds head_->next[i] as either
    // nullptr (fine, it stops) or the new node (fine, it is fully built).
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // NoBarrier is safe here: x is not reachable by any reader yet.
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    // THIS is the publication point. Release-store: everything written to x
    // above happens-before any reader that acquire-loads this pointer.
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace sextant::lsm
