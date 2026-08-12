#include "version_edit.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "coding.h"

namespace sextant::lsm {
namespace {

// Tag values are part of the on-disk MANIFEST format and must never be
// reassigned. New fields get new tags.
enum Tag : uint32_t {
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
};

bool GetLevel(Slice* input, int* level) {
  uint32_t v;
  if (!GetVarint32(input, &v) || v >= static_cast<uint32_t>(kNumLevels)) {
    return false;
  }
  *level = static_cast<int>(v);
  return true;
}

}  // namespace

void VersionEdit::Clear() {
  log_number_ = 0;
  next_file_number_ = 0;
  last_sequence_ = 0;
  has_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  compact_pointers_.clear();
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }

  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kCompactPointer);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutLengthPrefixedSlice(dst, Slice(key));
  }

  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }

  for (const auto& [level, f] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutLengthPrefixedSlice(dst, Slice(f.smallest));
    PutLengthPrefixedSlice(dst, Slice(f.largest));
  }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  const char* msg = nullptr;
  uint32_t tag;

  int level;
  uint64_t number;
  FileMetaData f;
  Slice str;

  while (msg == nullptr && GetVarint32(&input, &tag)) {
    switch (tag) {
      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          msg = "log number";
        }
        break;

      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          msg = "next file number";
        }
        break;

      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          msg = "last sequence number";
        }
        break;

      case kCompactPointer:
        if (GetLevel(&input, &level) && GetLengthPrefixedSlice(&input, &str)) {
          compact_pointers_.emplace_back(level, str.ToString());
        } else {
          msg = "compaction pointer";
        }
        break;

      case kDeletedFile:
        if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
          deleted_files_.insert(std::make_pair(level, number));
        } else {
          msg = "deleted file";
        }
        break;

      case kNewFile: {
        Slice smallest, largest;
        if (GetLevel(&input, &level) && GetVarint64(&input, &f.number) &&
            GetVarint64(&input, &f.file_size) &&
            GetLengthPrefixedSlice(&input, &smallest) &&
            GetLengthPrefixedSlice(&input, &largest)) {
          f.smallest = smallest.ToString();
          f.largest = largest.ToString();
          new_files_.emplace_back(level, f);
        } else {
          msg = "new file";
        }
        break;
      }

      default:
        msg = "unknown tag";
        break;
    }
  }

  if (msg == nullptr && !input.empty()) msg = "invalid tag";
  if (msg != nullptr) return Status::Corruption("VersionEdit", msg);
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string r = "VersionEdit {";
  char buf[128];
  if (has_log_number_) {
    std::snprintf(buf, sizeof(buf), "\n  LogNumber: %llu",
                  static_cast<unsigned long long>(log_number_));
    r += buf;
  }
  if (has_last_sequence_) {
    std::snprintf(buf, sizeof(buf), "\n  LastSeq: %llu",
                  static_cast<unsigned long long>(last_sequence_));
    r += buf;
  }
  for (const auto& [level, number] : deleted_files_) {
    std::snprintf(buf, sizeof(buf), "\n  RemoveFile: %d %llu", level,
                  static_cast<unsigned long long>(number));
    r += buf;
  }
  for (const auto& [level, f] : new_files_) {
    std::snprintf(buf, sizeof(buf), "\n  AddFile: %d %llu %llu bytes", level,
                  static_cast<unsigned long long>(f.number),
                  static_cast<unsigned long long>(f.file_size));
    r += buf;
  }
  r += "\n}\n";
  return r;
}

}  // namespace sextant::lsm
