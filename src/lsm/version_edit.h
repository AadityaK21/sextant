// A delta to the set of live files.
//
// WHY AN EDIT LOG RATHER THAN A SNAPSHOT. Day 2 and 3 wrote a DESCRIPTOR file
// listing every live file, rewritten in full on every flush. That is O(files)
// work per flush and it grows without bound. A VersionEdit instead records
// only what CHANGED - "file 41 was added to level 1, files 12 and 19 were
// removed from level 0" - and the MANIFEST is an append-only log of those
// records. Recovery replays them.
//
// This is also what makes compaction cheap to commit. A compaction that merges
// eight files into three appends one small record; it does not restate the
// other four hundred files in the database.
//
// ENCODING is a sequence of (varint tag, payload) pairs. Unknown tags cause a
// clean error rather than a misparse, and adding a field later does not
// invalidate old MANIFESTs.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "internal_key.h"
#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

// Seven levels is the conventional depth. With a 10x size ratio and a 4 MB L1
// that reaches 4 TB, which is far past anything this project will hold.
static constexpr int kNumLevels = 7;

struct FileMetaData {
  int refs = 0;
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // smallest internal key in the file
  std::string largest;   // largest internal key in the file
};

class VersionEdit {
 public:
  VersionEdit() { Clear(); }

  void Clear();

  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetCompactPointer(int level, const Slice& key) {
    compact_pointers_.emplace_back(level, key.ToString());
  }

  void AddFile(int level, uint64_t file, uint64_t file_size, const Slice& smallest,
               const Slice& largest) {
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest.ToString();
    f.largest = largest.ToString();
    new_files_.emplace_back(level, std::move(f));
  }

  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  // --- accessors used by VersionSet ---
  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }
  bool has_next_file_number() const { return has_next_file_number_; }
  uint64_t next_file_number() const { return next_file_number_; }
  bool has_last_sequence() const { return has_last_sequence_; }
  SequenceNumber last_sequence() const { return last_sequence_; }

  const std::set<std::pair<int, uint64_t>>& deleted_files() const {
    return deleted_files_;
  }
  const std::vector<std::pair<int, FileMetaData>>& new_files() const {
    return new_files_;
  }
  const std::vector<std::pair<int, std::string>>& compact_pointers() const {
    return compact_pointers_;
  }

  std::string DebugString() const;

 private:
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::vector<std::pair<int, std::string>> compact_pointers_;
  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace sextant::lsm
