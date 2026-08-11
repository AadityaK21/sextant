#include "format.h"

#include "coding.h"
#include "crc32c.h"

namespace sextant::lsm {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  // Pad so the footer is a constant size regardless of how large the varints
  // turned out. Without this you could not find the footer from the file end.
  dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength, '\0');
  PutFixed64BE(dst, kTableMagicNumber);
  assert(dst->size() == original_size + kEncodedLength);
  (void)original_size;
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() < static_cast<size_t>(kEncodedLength)) {
    return Status::Corruption("file is too short to be an sstable");
  }

  const char* magic_ptr = input->data() + kEncodedLength - 8;
  if (DecodeFixed64BE(magic_ptr) != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }

  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) result = index_handle_.DecodeFrom(input);
  if (result.ok()) {
    // Skip whatever padding remains before the magic.
    const char* end = magic_ptr + 8;
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result) {
  result->data = Slice();
  result->cachable = false;
  result->heap_allocated = false;

  const size_t n = static_cast<size_t>(handle.size());
  std::unique_ptr<char[]> buf(new char[n + kBlockTrailerSize]);

  Slice contents;
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf.get());
  if (!s.ok()) return s;
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  const char* data = contents.data();

  if (options.verify_checksums) {
    // The stored CRC covers the block bytes AND the compression-type byte, so
    // a flipped type byte is caught too.
    const uint32_t expected = crc32c::Unmask(DecodeFixed32BE(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != expected) {
      return Status::Corruption("block checksum mismatch");
    }
  }

  switch (static_cast<CompressionType>(data[n])) {
    case kNoCompression:
      if (data != buf.get()) {
        // The file returned a pointer into its own mapped memory; no copy and
        // nothing for us to free.
        result->data = Slice(data, n);
        result->heap_allocated = false;
        result->cachable = false;
      } else {
        result->data = Slice(buf.release(), n);
        result->heap_allocated = true;
        result->cachable = true;
      }
      break;
    default:
      return Status::Corruption("unknown block compression type");
  }

  return Status::OK();
}

}  // namespace sextant::lsm
