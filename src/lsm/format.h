// On-disk structural types: BlockHandle, Footer, and block reading.
//
// SSTABLE LAYOUT
//
//   ┌──────────────┬──────────────┬─────┬──────────────┐
//   │ data block 0 │ data block 1 │ ... │ data block N │
//   ├──────────────┴──────────────┴─────┴──────────────┤
//   │ metaindex block   (empty today; day 3 puts the   │
//   │                    bloom filter handle here)     │
//   ├──────────────────────────────────────────────────┤
//   │ index block       last key of block i -> handle  │
//   ├──────────────────────────────────────────────────┤
//   │ footer            fixed 48 bytes, ends with magic │
//   └──────────────────────────────────────────────────┘
//
// THE FOOTER IS FIXED-SIZE ON PURPOSE. To open a table you seek to
// (file_size - 48) and read forward. That gives you the index handle, which
// gives you the index block, which gives you every data block. One seek to
// bootstrap the entire file - you never scan looking for structure.
//
// Every block carries a 5-byte trailer: one compression-type byte and a
// CRC32C over (block contents + type byte). The handle's size EXCLUDES the
// trailer, so a handle describes logical content and the reader adds the
// trailer length itself.

#pragma once

#include <cstdint>
#include <string>

#include "env.h"
#include "sextant/lsm/options.h"
#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

// Points at a byte range within an SSTable.
class BlockHandle {
 public:
  BlockHandle() = default;

  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

  // Two varint64s, each at most 10 bytes.
  static constexpr int kMaxEncodedLength = 10 + 10;

 private:
  uint64_t offset_ = 0;
  uint64_t size_ = 0;
};

class Footer {
 public:
  Footer() = default;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

  // Two handles, zero-padded to a constant, then the magic number. Fixed size
  // is what makes the footer readable by seeking backwards from end-of-file.
  static constexpr int kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// Identifies a well-formed table and catches truncation or a wrong file type
// before any structural byte is trusted.
static constexpr uint64_t kTableMagicNumber = 0x53455854414E5401ull;  // "SEXTANT" + 1

enum CompressionType : uint8_t {
  kNoCompression = 0x0,
  // kSnappyCompression / kZstdCompression land with the compression milestone.
};

static constexpr size_t kBlockTrailerSize = 5;  // 1 byte type + 4 byte crc32c

struct BlockContents {
  Slice data;
  bool cachable = false;
  bool heap_allocated = false;  // caller must delete[] data.data() if true
};

// Read and verify the block named by handle.
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

}  // namespace sextant::lsm
