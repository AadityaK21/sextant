// THE differential test.
//
// Run a long stream of random operations against both the engine and a
// reference std::map, asserting identical observable behaviour at every step.
// This is the single highest-value test in a storage engine, because it finds
// the bugs unit tests structurally cannot: wrong version visibility, tombstones
// that fail to shadow, sequence numbers that collide, recovery that reorders
// writes.
//
// When SSTables and compaction land on days 2-4, this test does not change —
// it just starts exercising a far larger state space. That is the point of
// writing it on day 1.

#include <gtest/gtest.h>

#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "env.h"
#include "sextant/lsm/db.h"

using namespace sextant::lsm;

namespace {

// A reference implementation with the same semantics, in ~30 lines.
class ReferenceDB {
 public:
  void Put(const std::string& k, const std::string& v) { map_[k] = v; }
  void Delete(const std::string& k) { map_.erase(k); }
  std::optional<std::string> Get(const std::string& k) const {
    const auto it = map_.find(k);
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }
  size_t size() const { return map_.size(); }
  const std::map<std::string, std::string>& data() const { return map_; }

 private:
  std::map<std::string, std::string> map_;
};

class DifferentialTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<DB> db_;
  ReferenceDB ref_;

  void SetUp() override {
    dbname_ = std::string("difftest_") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name();
    Destroy();
    ASSERT_TRUE(DB::Open(Options{}, dbname_, &db_).ok());
  }
  void TearDown() override {
    db_.reset();
    Destroy();
  }

  void Destroy() {
    std::vector<std::string> children;
    if (GetChildren(dbname_, &children).ok()) {
      for (const auto& c : children) {
        if (c == "." || c == "..") continue;
        DeleteFile(dbname_ + "/" + c);
      }
    }
    std::remove(dbname_.c_str());
  }

  void Reopen() {
    ASSERT_TRUE(db_->SyncWAL().ok());
    db_.reset();
    ASSERT_TRUE(DB::Open(Options{}, dbname_, &db_).ok());
  }

  // Assert the engine and the reference agree about every key in the space,
  // present or absent.
  void AssertAgreement(const std::vector<std::string>& key_space) {
    for (const auto& k : key_space) {
      std::string got;
      const Status s = db_->Get(ReadOptions{}, Slice(k), &got);
      const auto expected = ref_.Get(k);

      if (expected.has_value()) {
        ASSERT_TRUE(s.ok()) << "key '" << k << "' should exist but Get said: "
                            << s.ToString();
        ASSERT_EQ(*expected, got) << "value mismatch for key '" << k << "'";
      } else {
        ASSERT_TRUE(s.IsNotFound())
            << "key '" << k << "' should be absent but Get returned '" << got << "'";
      }
    }
  }
};

std::string RandomValue(std::mt19937& rnd, size_t max_len) {
  const size_t len = rnd() % max_len;
  std::string v;
  v.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    v.push_back(static_cast<char>('a' + (rnd() % 26)));
  }
  return v;
}

}  // namespace

TEST_F(DifferentialTest, RandomOperationsMatchStdMap) {
  static constexpr int kOps = 60000;
  static constexpr int kKeySpace = 800;

  std::vector<std::string> key_space;
  key_space.reserve(kKeySpace);
  for (int i = 0; i < kKeySpace; ++i) key_space.push_back("key_" + std::to_string(i));

  std::mt19937 rnd(20260809);

  for (int op = 0; op < kOps; ++op) {
    const std::string& key = key_space[rnd() % kKeySpace];

    switch (rnd() % 10) {
      case 0:
      case 1:
      case 2: {  // delete (30%)
        ASSERT_TRUE(db_->Delete(WriteOptions{}, Slice(key)).ok());
        ref_.Delete(key);
        break;
      }
      case 3: {  // atomic batch (10%)
        WriteBatch batch;
        const int n = 1 + static_cast<int>(rnd() % 5);
        for (int i = 0; i < n; ++i) {
          const std::string& bk = key_space[rnd() % kKeySpace];
          if (rnd() % 4 == 0) {
            batch.Delete(Slice(bk));
            ref_.Delete(bk);
          } else {
            const std::string v = RandomValue(rnd, 64);
            batch.Put(Slice(bk), Slice(v));
            ref_.Put(bk, v);
          }
        }
        ASSERT_TRUE(db_->Write(WriteOptions{}, &batch).ok());
        break;
      }
      default: {  // put (60%)
        const std::string v = RandomValue(rnd, 100);
        ASSERT_TRUE(db_->Put(WriteOptions{}, Slice(key), Slice(v)).ok());
        ref_.Put(key, v);
        break;
      }
    }

    // Spot-check continuously; sweep the whole key space periodically.
    {
      const std::string& probe = key_space[rnd() % kKeySpace];
      std::string got;
      const Status s = db_->Get(ReadOptions{}, Slice(probe), &got);
      const auto expected = ref_.Get(probe);
      if (expected.has_value()) {
        ASSERT_TRUE(s.ok()) << "op " << op << ": missing key " << probe;
        ASSERT_EQ(*expected, got) << "op " << op;
      } else {
        ASSERT_TRUE(s.IsNotFound()) << "op " << op << ": resurrected key " << probe;
      }
    }

    if (op % 10000 == 0) {
      ASSERT_NO_FATAL_FAILURE(AssertAgreement(key_space)) << "at op " << op;
    }
  }

  ASSERT_NO_FATAL_FAILURE(AssertAgreement(key_space));
}

// The same idea, but crossing a recovery boundary. Recovery must reconstruct
// the exact logical state, including tombstones and last-write-wins ordering.
TEST_F(DifferentialTest, StateSurvivesRepeatedReopen) {
  static constexpr int kRounds = 8;
  static constexpr int kOpsPerRound = 3000;
  static constexpr int kKeySpace = 300;

  std::vector<std::string> key_space;
  for (int i = 0; i < kKeySpace; ++i) key_space.push_back("k" + std::to_string(i));

  std::mt19937 rnd(777);

  for (int round = 0; round < kRounds; ++round) {
    for (int op = 0; op < kOpsPerRound; ++op) {
      const std::string& key = key_space[rnd() % kKeySpace];
      if (rnd() % 3 == 0) {
        ASSERT_TRUE(db_->Delete(WriteOptions{}, Slice(key)).ok());
        ref_.Delete(key);
      } else {
        const std::string v = RandomValue(rnd, 50);
        ASSERT_TRUE(db_->Put(WriteOptions{}, Slice(key), Slice(v)).ok());
        ref_.Put(key, v);
      }
    }

    ASSERT_NO_FATAL_FAILURE(Reopen()) << "round " << round;
    ASSERT_NO_FATAL_FAILURE(AssertAgreement(key_space))
        << "state diverged after reopen in round " << round;
  }
}

// Snapshots must behave like a frozen std::map taken at that instant.
TEST_F(DifferentialTest, SnapshotsMatchAFrozenReference) {
  static constexpr int kKeySpace = 200;
  std::vector<std::string> key_space;
  for (int i = 0; i < kKeySpace; ++i) key_space.push_back("s" + std::to_string(i));

  std::mt19937 rnd(31337);

  for (int i = 0; i < 2000; ++i) {
    const std::string& k = key_space[rnd() % kKeySpace];
    const std::string v = RandomValue(rnd, 40);
    ASSERT_TRUE(db_->Put(WriteOptions{}, Slice(k), Slice(v)).ok());
    ref_.Put(k, v);
  }

  // Freeze both.
  const Snapshot* snap = db_->GetSnapshot();
  const std::map<std::string, std::string> frozen = ref_.data();

  // Churn heavily afterwards.
  for (int i = 0; i < 4000; ++i) {
    const std::string& k = key_space[rnd() % kKeySpace];
    if (rnd() % 2 == 0) {
      ASSERT_TRUE(db_->Delete(WriteOptions{}, Slice(k)).ok());
      ref_.Delete(k);
    } else {
      const std::string v = RandomValue(rnd, 40);
      ASSERT_TRUE(db_->Put(WriteOptions{}, Slice(k), Slice(v)).ok());
      ref_.Put(k, v);
    }
  }

  ReadOptions ro;
  ro.snapshot = snap;
  for (const auto& k : key_space) {
    std::string got;
    const Status s = db_->Get(ro, Slice(k), &got);
    const auto it = frozen.find(k);
    if (it != frozen.end()) {
      ASSERT_TRUE(s.ok()) << "snapshot lost key " << k;
      ASSERT_EQ(it->second, got) << "snapshot value drifted for key " << k;
    } else {
      ASSERT_TRUE(s.IsNotFound()) << "snapshot saw a key written after it: " << k;
    }
  }

  db_->ReleaseSnapshot(snap);

  // And the live view still matches the live reference.
  ASSERT_NO_FATAL_FAILURE(AssertAgreement(key_space));
}
