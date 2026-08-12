#include "cache.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bloom.h"  // Hash

namespace sextant::lsm {
namespace {

struct Entry {
  std::string key;
  void* value = nullptr;
  void (*deleter)(const Slice&, void*) = nullptr;
  size_t charge = 0;
  uint32_t refs = 0;   // number of outstanding handles (cache's own ref excluded)
  bool in_cache = false;
};

// std::list guarantees iterator and reference stability across insert/erase of
// other elements, which is what lets us hand out `Entry*` as a Handle.
using EntryList = std::list<Entry>;

class LRUCacheShard {
 public:
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  ~LRUCacheShard() {
    for (auto& entry : lru_) {
      if (entry.deleter) entry.deleter(Slice(entry.key), entry.value);
    }
  }

  Entry* Insert(const Slice& key, void* value, size_t charge,
                void (*deleter)(const Slice&, void*),
                std::atomic<uint64_t>* evictions) {
    std::lock_guard<std::mutex> lock(mutex_);

    EraseLocked(key);

    lru_.push_front(Entry{});
    Entry& e = lru_.front();
    e.key.assign(key.data(), key.size());
    e.value = value;
    e.deleter = deleter;
    e.charge = charge;
    e.refs = 1;  // the handle we are about to return
    e.in_cache = true;

    table_[e.key] = lru_.begin();
    usage_ += charge;

    EvictLocked(evictions);
    return &e;
  }

  Entry* Lookup(const Slice& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = table_.find(std::string(key.data(), key.size()));
    if (it == table_.end()) return nullptr;

    // Move to the front: this is the "recently used" part of LRU.
    lru_.splice(lru_.begin(), lru_, it->second);
    it->second = lru_.begin();

    Entry& e = lru_.front();
    ++e.refs;
    return &e;
  }

  void Release(Entry* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(handle->refs > 0);
    --handle->refs;
    if (!handle->in_cache && handle->refs == 0) {
      // Already evicted while pinned; now safe to destroy.
      FinishErase(handle);
    }
  }

  void Erase(const Slice& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    EraseLocked(key);
  }

  size_t Usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
  }

 private:
  void EraseLocked(const Slice& key) {
    const auto it = table_.find(std::string(key.data(), key.size()));
    if (it == table_.end()) return;

    Entry* e = &(*it->second);
    table_.erase(it);
    usage_ -= e->charge;
    e->in_cache = false;
    if (e->refs == 0) FinishErase(e);
  }

  // Destroy an entry that is no longer in the table and no longer pinned.
  void FinishErase(Entry* e) {
    if (e->deleter) e->deleter(Slice(e->key), e->value);
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
      if (&(*it) == e) {
        lru_.erase(it);
        return;
      }
    }
  }

  void EvictLocked(std::atomic<uint64_t>* evictions) {
    auto it = lru_.end();
    while (usage_ > capacity_ && it != lru_.begin()) {
      --it;
      Entry& e = *it;
      if (!e.in_cache || e.refs > 0) {
        continue;  // pinned by a live iterator; skip it
      }
      const std::string key = e.key;
      auto next = it;
      ++next;
      table_.erase(key);
      usage_ -= e.charge;
      e.in_cache = false;
      if (e.deleter) e.deleter(Slice(e.key), e.value);
      lru_.erase(it);
      it = next;
      evictions->fetch_add(1, std::memory_order_relaxed);
    }
  }

  mutable std::mutex mutex_;
  size_t capacity_ = 0;
  size_t usage_ = 0;
  EntryList lru_;  // front = most recently used
  std::unordered_map<std::string, EntryList::iterator> table_;
};

static constexpr int kNumShardBits = 4;
static constexpr int kNumShards = 1 << kNumShardBits;  // 16

class ShardedLRUCache final : public Cache {
 public:
  explicit ShardedLRUCache(size_t capacity) {
    // Round up so the shards together are never smaller than the request.
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (auto& shard : shards_) shard.SetCapacity(per_shard);
  }

  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice&, void*)) override {
    return reinterpret_cast<Handle*>(
        Shard(key).Insert(key, value, charge, deleter, &evictions_));
  }

  Handle* Lookup(const Slice& key) override {
    Entry* e = Shard(key).Lookup(key);
    if (e != nullptr) {
      hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
      misses_.fetch_add(1, std::memory_order_relaxed);
    }
    return reinterpret_cast<Handle*>(e);
  }

  void Release(Handle* handle) override {
    auto* e = reinterpret_cast<Entry*>(handle);
    Shard(Slice(e->key)).Release(e);
  }

  void* Value(Handle* handle) override {
    return reinterpret_cast<Entry*>(handle)->value;
  }

  void Erase(const Slice& key) override { Shard(key).Erase(key); }

  uint64_t NewId() override {
    return last_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  size_t TotalCharge() const override {
    size_t total = 0;
    for (const auto& shard : shards_) total += shard.Usage();
    return total;
  }

  uint64_t Hits() const override { return hits_.load(std::memory_order_relaxed); }
  uint64_t Misses() const override { return misses_.load(std::memory_order_relaxed); }
  uint64_t Evictions() const override {
    return evictions_.load(std::memory_order_relaxed);
  }

 private:
  LRUCacheShard& Shard(const Slice& key) {
    return shards_[Hash(key.data(), key.size(), 0) & (kNumShards - 1)];
  }
  const LRUCacheShard& Shard(const Slice& key) const {
    return shards_[Hash(key.data(), key.size(), 0) & (kNumShards - 1)];
  }

  LRUCacheShard shards_[kNumShards];
  std::atomic<uint64_t> last_id_{0};
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> evictions_{0};
};

}  // namespace

std::unique_ptr<Cache> Cache::NewLRUCache(size_t capacity) {
  return std::make_unique<ShardedLRUCache>(capacity);
}

}  // namespace sextant::lsm
