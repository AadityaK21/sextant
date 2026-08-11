// Write-ahead log.
//
// The log is a sequence of fixed-size 32 KB blocks. A block contains one or
// more records; a record never spans a block boundary without being split,
// which is what the FIRST/MIDDLE/LAST types are for.
//
//   record := header || payload
//   header := crc32c(masked, 4 bytes BE) || length(2 bytes BE) || type(1 byte)
//
// WHY BLOCKS AT ALL?  Because a corrupt or torn region should cost you the
// records inside one 32 KB block, not the rest of the file. A reader that hits
// a bad CRC can skip to the next block boundary and resynchronise. Without
// framing you cannot tell where the next record starts.
//
// WHY THE CRC?  It is what makes a torn tail DETECTABLE rather than fatal. A
// crash mid-append leaves a partial final record; on recovery its CRC fails,
// the reader stops cleanly there, and every write acknowledged before the
// crash is intact. This is the property the crash test asserts.
//
// WHAT DURABILITY ACTUALLY MEANS HERE: Append() alone only reaches the OS page
// cache - it survives a process crash. Only Sync() survives power loss, and it
// costs a real device round-trip, so the DB batches many writes behind a
// single Sync (group commit). WriteOptions::sync selects the guarantee.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "env.h"
#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm::wal {

enum RecordType : uint8_t {
  kZeroType = 0,   // preallocated / padding
  kFullType = 1,   // the whole record fits in this block

  // A record split across blocks.
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

static constexpr int kMaxRecordType = kLastType;
static constexpr int kBlockSize = 32768;
static constexpr int kHeaderSize = 4 + 2 + 1;  // crc + length + type

class Writer {
 public:
  // dest must be freshly opened (offset 0) unless dest_length is given.
  explicit Writer(WritableFile* dest);
  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  Status AddRecord(const Slice& slice);
  Status Sync() { return dest_->Sync(); }
  Status Flush() { return dest_->Flush(); }

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;  // offset within the current block
};

class Reader {
 public:
  // Reports corruption without aborting; recovery treats it as end-of-log.
  class Reporter {
   public:
    virtual ~Reporter() = default;
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  Reader(SequentialFile* file, Reporter* reporter, bool checksum);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Reads the next record into *record; scratch backs fragmented records.
  // Returns false at end of input.
  bool ReadRecord(Slice* record, std::string* scratch);

 private:
  // Sentinels returned by ReadPhysicalRecord in place of a real type.
  enum {
    kEof = kMaxRecordType + 1,
    kBadRecord = kMaxRecordType + 2,
  };

  unsigned int ReadPhysicalRecord(Slice* result);
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  SequentialFile* const file_;
  Reporter* const reporter_;
  const bool checksum_;
  std::unique_ptr<char[]> backing_store_;
  Slice buffer_;
  bool eof_ = false;
};

}  // namespace sextant::lsm::wal
