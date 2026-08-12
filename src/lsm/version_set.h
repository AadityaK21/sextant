// Versions, the MANIFEST, and compaction planning.
//
// A VERSION is an immutable snapshot of which files exist at which level. It is
// reference counted. Nothing ever mutates a Version; applying an edit produces
// a NEW Version, and the old one stays alive until every reader holding it lets
// go.
//
// THAT REFCOUNT IS THE POINT. It fixes the limitation left open on day 3: an
// iterator can now hold a Version for as long as it likes while compaction runs
// concurrently, deletes files, and installs new Versions. The files the
// iterator is reading are not unlinked until its Version's refcount hits zero.
// Without this, a compaction finishing mid-scan would delete a file out from
// under an open reader.
//
// LEVELED LAYOUT
//
//   L0   files OVERLAP - they are flushed memtables, in arrival order.
//        A lookup must check every L0 file whose key range contains the key,
//        newest first.
//   L1+  files are non-overlapping and globally sorted within the level.
//        A lookup binary-searches the level and checks AT MOST ONE file.
//
// That distinction is the entire reason compaction exists: it converts the
// unbounded, overlapping L0 into bounded, sorted levels, which turns read
// amplification from O(number of flushes) into O(number of levels).

#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "env.h"
#include "internal_key.h"
#include "table_cache.h"
#include "version_edit.h"
#include "wal.h"

namespace sextant::lsm {

class Compaction;
class VersionSet;

// Size targets. L1 is 10 MB and each level is 10x the one above.
//
// The 10x ratio is the classic knob. Larger means fewer levels (better reads)
// but more bytes rewritten per level (worse writes). 10 is the conventional
// balance and matches LevelDB and RocksDB defaults.
static constexpr int kL0CompactionTrigger = 4;   // start compacting at 4 files
static constexpr int kL0SlowdownWritesTrigger = 8;  // throttle writers
static constexpr int kL0StopWritesTrigger = 12;     // block writers entirely
static constexpr uint64_t kMaxFileSize = 2u * 1024 * 1024;

double MaxBytesForLevel(int level);

class Version {
 public:
  // Per-lookup accounting, aggregated into DB Stats. Reported rather than
  // inferred, so the effect of each read-path filter is measurable.
  struct GetStats {
    uint64_t range_rejections = 0;  // files skipped on key range alone
    uint64_t files_probed = 0;      // files actually opened and searched
  };

  void Ref();
  void Unref();

  // Point lookup across all levels. Returns NotFound if absent or tombstoned.
  Status Get(const ReadOptions& options, const LookupKey& key, std::string* value,
             GetStats* stats);

  // Append one iterator per file (concatenating per level for L1+).
  void AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters);

  // Files in a level whose key range intersects [begin, end] on user keys.
  void GetOverlappingInputs(int level, const Slice* begin, const Slice* end,
                            std::vector<FileMetaData*>* inputs);

  int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }
  const std::vector<FileMetaData*>& files(int level) const { return files_[level]; }
  int64_t NumBytes(int level) const;

  std::string DebugString() const;

 private:
  friend class VersionSet;
  friend class Compaction;

  explicit Version(VersionSet* vset) : vset_(vset) {}
  ~Version();

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  VersionSet* vset_;
  Version* next_ = nullptr;  // doubly-linked list of live versions
  Version* prev_ = nullptr;
  int refs_ = 0;

  std::vector<FileMetaData*> files_[kNumLevels];
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options& options, TableCache* table_cache);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Apply edit to the current version, append it to the MANIFEST, and install
  // the result. REQUIRES: mu is held; it is released during the file write so
  // that other threads can proceed while we do I/O.
  Status LogAndApply(VersionEdit* edit, std::mutex* mu);

  Status Recover(bool* save_manifest);

  Version* current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_++; }
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t LogNumber() const { return log_number_; }
  void SetLogNumber(uint64_t num) { log_number_ = num; }

  SequenceNumber LastSequence() const { return last_sequence_; }
  void SetLastSequence(SequenceNumber s) { last_sequence_ = s; }

  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) next_file_number_ = number + 1;
  }

  int NumLevelFiles(int level) const;
  int64_t NumLevelBytes(int level) const;

  // Which level a freshly flushed memtable should land in. Pushing it past L0
  // when it does not overlap anything avoids a pointless immediate compaction.
  int PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                 const Slice& largest_user_key);

  bool NeedsCompaction() const;
  Compaction* PickCompaction();

  // Every file referenced by any live version. Anything else on disk is
  // garbage and can be deleted.
  void AddLiveFiles(std::set<uint64_t>* live);

  Iterator* MakeInputIterator(Compaction* c);

  std::string LevelSummary() const;

 private:
  friend class Compaction;
  friend class Version;

  class Builder;

  void AppendVersion(Version* v);
  void Finalize(Version* v);
  Status WriteSnapshot(wal::Writer* log);
  void SetupOtherInputs(Compaction* c);

  const std::string dbname_;
  const Options options_;
  TableCache* const table_cache_;
  const InternalKeyComparator icmp_{};

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;

  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<wal::Writer> descriptor_log_;

  Version dummy_versions_{this};  // head of the circular list
  Version* current_ = nullptr;

  // Where the next compaction of each level should begin. Without this, a level
  // would repeatedly compact the same key range and starve the rest.
  std::string compact_pointer_[kNumLevels];

  double compaction_score_ = -1;
  int compaction_level_ = -1;
};

// One compaction job: inputs from `level` and `level+1`, output to `level+1`.
class Compaction {
 public:
  int level() const { return level_; }
  VersionEdit* edit() { return &edit_; }

  int num_input_files(int which) const {
    return static_cast<int>(inputs_[which].size());
  }
  FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // A single L0 file that overlaps nothing below can simply be MOVED to the
  // next level: a metadata edit with no data rewritten at all.
  bool IsTrivialMove() const;

  void AddInputDeletions(VersionEdit* edit);

  // True when no level below level_+1 can contain user_key. Only then is it
  // safe to drop a tombstone - otherwise the delete would stop shadowing an
  // older value and the key would come back to life.
  bool IsBaseLevelForKey(const Slice& user_key);

  void ReleaseInputs();

  // Public so callers can hold a Compaction in a unique_ptr. Construction stays
  // private: only VersionSet::PickCompaction knows how to assemble a valid one.
  ~Compaction();

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const Options& options, int level);

  int level_;
  uint64_t max_output_file_size_;
  Version* input_version_ = nullptr;
  VersionEdit edit_;

  std::vector<FileMetaData*> inputs_[2];  // inputs_[0] = level_, [1] = level_+1

  // Per-level cursor used by IsBaseLevelForKey, which is called with
  // monotonically increasing keys during a compaction, so it can resume rather
  // than rescan.
  size_t level_ptrs_[kNumLevels];
};

}  // namespace sextant::lsm
