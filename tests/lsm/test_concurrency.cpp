// Reader threads against a compacting engine.
//
// WHAT THIS EXISTS TO CATCH
//
// An iterator pins a Version and the two memtables so that a concurrent
// compaction cannot delete the files or arenas it is reading. Those references
// are released when the iterator is DESTROYED, which happens on whatever thread
// owns the iterator, at whatever moment it goes out of scope.
//
// That release is the dangerous part, and it is invisible to every
// single-threaded test in this suite. `Version::Unref` decrements a plain int
// and, at zero, runs a destructor that unlinks the version from the
// VersionSet's shared linked list and drops refcounts on FileMetaData shared
// with other versions. Doing any of that without the DB mutex, while the
// background compaction thread is running, is three separate races.
//
// The bug was real and shipped: see docs/BUGS.md. It survived 415 tests, a
// clean ASan run and a clean UBSan run, because none of those look at
// cross-thread ordering. It took either reading the code or running this file
// under ThreadSanitizer.
//
// HOW TO RUN IT THE WAY IT IS MEANT TO BE RUN
//
//     cmake -S . -B build-tsan -DSEXTANT_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
//     cmake --build build-tsan -j
//     ./build-tsan/tests/test_concurrency
//
// Without TSan these tests still exercise the paths and will catch a crash or a
// corrupted result, which is worth something. With TSan they catch the race
// itself, which is worth much more - an unsynchronised refcount usually does
// not crash, it just occasionally frees something twice under load.

#include "sextant/lsm/db.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "env.h"

using namespace sextant::lsm;

namespace {

class ConcurrencyTest : public ::testing::Test {
 protected:
  std::string dbname_;
  std::unique_ptr<DB> db_;

  void SetUp() override {
    dbname_ = std::string("conctest_") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name();
    Destroy();
    Options opts;
    // Small buffer so flushes and compactions happen constantly during the
    // test rather than once at the end. The whole point is to have background
    // work in flight while readers come and go.
    opts.write_buffer_size = 32 * 1024;
    ASSERT_TRUE(DB::Open(opts, dbname_, &db_).ok());
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
        RemoveFile(dbname_ + "/" + c);
      }
    }
    std::remove(dbname_.c_str());
  }

  static std::string Key(int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key%06d", i);
    return buf;
  }
};

}  // namespace

// THE ONE THAT FOUND THE BUG.
//
// Iterators are created and destroyed on reader threads while a writer thread
// drives flushes and compactions. Every destruction releases a Version
// reference and two MemTable references from a thread that is not the one
// doing the compaction.
TEST_F(ConcurrencyTest, IteratorsCreatedAndDestroyedWhileCompactionRuns) {
  static constexpr int kWriterOps = 4000;
  static constexpr int kReaders = 4;
  static constexpr int kIterationsPerReader = 400;

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> iterators_built{0};
  std::atomic<uint64_t> keys_seen{0};

  std::thread writer([&] {
    for (int i = 0; i < kWriterOps; ++i) {
      const std::string value(120, static_cast<char>('a' + (i % 26)));
      db_->Put(WriteOptions{}, Slice(Key(i % 900)), Slice(value));
      // Deletes as well as puts, so compaction has tombstones to reclaim and
      // versions turn over faster.
      if (i % 7 == 0) db_->Delete(WriteOptions{}, Slice(Key((i * 13) % 900)));
    }
    stop = true;
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 rnd(static_cast<uint32_t>(1000 + r));
      for (int n = 0; n < kIterationsPerReader && !stop; ++n) {
        // The iterator is deliberately scoped tightly. Its destruction here is
        // the operation under test.
        auto iter = db_->NewIterator(ReadOptions{});
        iter->SeekToFirst();

        // Walk a random distance, so iterators die at every stage of their
        // life rather than always at the end of a full scan.
        const int steps = static_cast<int>(rnd() % 40);
        int walked = 0;
        while (iter->Valid() && walked < steps) {
          keys_seen.fetch_add(1, std::memory_order_relaxed);
          iter->Next();
          ++walked;
        }
        EXPECT_TRUE(iter->status().ok()) << iter->status().ToString();
        iterators_built.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  std::printf("  %llu iterators built and destroyed across %d threads, "
              "%llu keys walked\n",
              static_cast<unsigned long long>(iterators_built.load()), kReaders,
              static_cast<unsigned long long>(keys_seen.load()));

  EXPECT_GT(iterators_built.load(), 100u)
      << "the readers barely ran; this test proves little";

  // The engine must still be coherent afterwards.
  db_->WaitForBackgroundWork();
  std::string value;
  const Status s = db_->Get(ReadOptions{}, Slice(Key(1)), &value);
  EXPECT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
}

// Snapshots are the other refcounted thing a reader holds across a compaction.
// A snapshot is only a sequence number, so this is cheaper than the iterator
// case, but it exercises the same acquire and release path from other threads.
TEST_F(ConcurrencyTest, SnapshotsTakenAndReleasedWhileCompactionRuns) {
  static constexpr int kWriterOps = 3000;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> snapshots{0};

  std::thread writer([&] {
    for (int i = 0; i < kWriterOps; ++i) {
      const std::string value(100, 'x');
      db_->Put(WriteOptions{}, Slice(Key(i % 700)), Slice(value));
    }
    stop = true;
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (!stop) {
        const Snapshot* snap = db_->GetSnapshot();
        ReadOptions ro;
        ro.snapshot = snap;
        std::string value;
        for (int i = 0; i < 20; ++i) {
          const Status s = db_->Get(ro, Slice(Key(i)), &value);
          EXPECT_TRUE(s.ok() || s.IsNotFound()) << s.ToString();
        }
        db_->ReleaseSnapshot(snap);
        snapshots.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  std::printf("  %llu snapshots taken and released during compaction\n",
              static_cast<unsigned long long>(snapshots.load()));
  EXPECT_GT(snapshots.load(), 10u);
}

// Point lookups from several threads while the version set turns over. Get()
// refs the current Version, reads, and unrefs - all under the mutex, which is
// what the iterator path failed to do.
TEST_F(ConcurrencyTest, ConcurrentReadersSeeConsistentValues) {
  static constexpr int kKeys = 400;

  // Seed a known state, then verify readers never observe anything but the
  // value written for a key or its absence - never a torn or foreign value.
  for (int i = 0; i < kKeys; ++i) {
    ASSERT_TRUE(
        db_->Put(WriteOptions{}, Slice(Key(i)), Slice("v" + std::to_string(i)))
            .ok());
  }

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};

  std::thread writer([&] {
    for (int round = 0; round < 8; ++round) {
      for (int i = 0; i < kKeys; ++i) {
        const std::string padding(200, static_cast<char>('a' + round));
        db_->Put(WriteOptions{}, Slice("filler" + std::to_string(i)),
                 Slice(padding));
      }
    }
    stop = true;
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      while (!stop) {
        for (int i = 0; i < kKeys; ++i) {
          std::string value;
          const Status s = db_->Get(ReadOptions{}, Slice(Key(i)), &value);
          ASSERT_TRUE(s.ok()) << "key " << i << ": " << s.ToString();
          ASSERT_EQ("v" + std::to_string(i), value)
              << "key " << i << " came back as something else entirely";
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();

  std::printf("  %llu point lookups verified during compaction\n",
              static_cast<unsigned long long>(reads.load()));
  EXPECT_GT(reads.load(), 1000u);
}
