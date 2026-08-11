#include "wal.h"

#include <cstring>

#include "coding.h"
#include "crc32c.h"

namespace sextant::lsm::wal {

// --- Writer ----------------------------------------------------------------

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(static_cast<int>(dest_length % kBlockSize)) {}

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  Status s;
  bool begin = true;
  do {
    const int leftover = kBlockSize - block_offset_;
    assert(leftover >= 0);

    if (leftover < kHeaderSize) {
      // Not enough room for even a header: pad the block out with zeroes and
      // start a new one. Keeping headers block-aligned is what lets a reader
      // resynchronise after corruption.
      if (leftover > 0) {
        static constexpr char kPadding[kHeaderSize] = {0, 0, 0, 0, 0, 0, 0};
        s = dest_->Append(Slice(kPadding, static_cast<size_t>(leftover)));
        if (!s.ok()) return s;
      }
      block_offset_ = 0;
    }

    const size_t avail = static_cast<size_t>(kBlockSize - block_offset_ - kHeaderSize);
    const size_t fragment_length = (left < avail) ? left : avail;

    const bool end = (left == fragment_length);
    RecordType type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);

  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
  assert(length <= 0xffff);
  assert(block_offset_ + kHeaderSize + static_cast<int>(length) <= kBlockSize);

  char buf[kHeaderSize];
  // crc goes in first but is computed last; fill bytes 4..6 first.
  buf[4] = static_cast<char>((length >> 8) & 0xff);
  buf[5] = static_cast<char>(length & 0xff);
  buf[6] = static_cast<char>(t);

  // CRC covers the type byte AND the payload, so a corrupted type is caught.
  uint32_t crc = crc32c::Extend(0, &buf[6], 1);
  crc = crc32c::Extend(crc, ptr, length);
  EncodeFixed32BE(buf, crc32c::Mask(crc));

  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) s = dest_->Flush();
  }
  block_offset_ += kHeaderSize + static_cast<int>(length);
  return s;
}

// --- Reader ----------------------------------------------------------------

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]) {}

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != nullptr) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;

  Slice fragment;
  while (true) {
    const unsigned int record_type = ReadPhysicalRecord(&fragment);

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "partial record without end(1)");
        }
        scratch->clear();
        *record = fragment;
        return true;

      case kFirstType:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "partial record without end(2)");
        }
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "missing start of fragmented record(1)");
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "missing start of fragmented record(2)");
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(*scratch);
          return true;
        }
        break;

      case kEof:
        // A truncated fragmented record is exactly what a crash mid-append
        // looks like. Drop it silently - the write was never acknowledged.
        if (in_fragmented_record) scratch->clear();
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default: {
        ReportCorruption(fragment.size() + (in_fragmented_record ? scratch->size() : 0),
                         "unknown record type");
        in_fragmented_record = false;
        scratch->clear();
        break;
      }
    }
  }
}

unsigned int Reader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    if (buffer_.size() < static_cast<size_t>(kHeaderSize)) {
      if (!eof_) {
        buffer_.clear();
        Slice read_result;
        const Status s = file_->Read(kBlockSize, &read_result, backing_store_.get());
        buffer_ = read_result;
        if (!s.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, s);
          eof_ = true;
          return kEof;
        }
        if (read_result.size() < static_cast<size_t>(kBlockSize)) eof_ = true;
        continue;
      }
      // eof_ with a sub-header remainder: a torn tail. Not an error.
      buffer_.clear();
      return kEof;
    }

    const char* header = buffer_.data();
    const uint32_t length = (static_cast<uint32_t>(static_cast<uint8_t>(header[4])) << 8) |
                            static_cast<uint32_t>(static_cast<uint8_t>(header[5]));
    const auto type = static_cast<unsigned int>(static_cast<uint8_t>(header[6]));

    if (static_cast<size_t>(kHeaderSize) + length > buffer_.size()) {
      const size_t drop_size = buffer_.size();
      buffer_.clear();
      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }
      return kEof;  // truncated tail
    }

    if (type == kZeroType && length == 0) {
      buffer_.clear();  // padding at the end of a block
      return kBadRecord;
    }

    if (checksum_) {
      const uint32_t expected = crc32c::Unmask(DecodeFixed32BE(header));
      const uint32_t actual = crc32c::Value(header + 6, 1 + length);
      if (actual != expected) {
        const size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    buffer_.remove_prefix(static_cast<size_t>(kHeaderSize) + length);
    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

}  // namespace sextant::lsm::wal
