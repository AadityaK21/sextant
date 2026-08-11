#include "table_builder.h"

#include <cassert>

#include "coding.h"
#include "crc32c.h"

namespace sextant::lsm {

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options), file_(file) {}

TableBuilder::~TableBuilder() { assert(closed_); }

void TableBuilder::Add(const Slice& key, const Slice& value) {
  assert(!closed_);
  if (!ok()) return;

  if (num_entries_ > 0) {
    assert(comparator_.Compare(key, Slice(last_key_)) > 0);
  }

  // The previous data block is complete, and we now know a key that is greater
  // than everything in it - so its index entry can finally be written.
  //
  // Using the FIRST key of the new block as the separator (rather than the last
  // key of the old one) keeps the index entry as small as it can be while still
  // satisfying the invariant "index key >= every key in the block it names".
  if (pending_index_entry_) {
    assert(data_block_.empty());
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  data_block_.Add(key, value);

  if (data_block_.CurrentSizeEstimate() >= kDefaultBlockSize) {
    Flush();
  }
}

void TableBuilder::Flush() {
  assert(!closed_);
  if (!ok() || data_block_.empty()) return;

  assert(!pending_index_entry_);
  WriteBlock(&data_block_, &pending_handle_);
  if (ok()) {
    pending_index_entry_ = true;
    status_ = file_->Flush();
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  const Slice raw = block->Finish();
  // Compression hooks in here later; the type byte already exists in the
  // trailer so adding it is a format-compatible change.
  WriteRawBlock(raw, kNoCompression, handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents, CompressionType type,
                                 BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(block_contents.size());  // size EXCLUDES the trailer

  status_ = file_->Append(block_contents);
  if (!status_.ok()) return;

  // Trailer: compression type, then a CRC over contents + type byte.
  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(type);
  uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
  crc = crc32c::Extend(crc, trailer, 1);
  EncodeFixed32BE(trailer + 1, crc32c::Mask(crc));

  status_ = file_->Append(Slice(trailer, kBlockTrailerSize));
  if (status_.ok()) {
    offset_ += block_contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::Finish() {
  Flush();
  assert(!closed_);
  closed_ = true;

  // Metaindex: empty for now. Day 3 puts the bloom filter handle here. Writing
  // the (empty) block today means adding filters later does not change the
  // file layout.
  BlockHandle metaindex_block_handle;
  BlockHandle index_block_handle;

  if (ok()) {
    BlockBuilder meta_index_block;
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  if (ok()) {
    if (pending_index_entry_) {
      std::string handle_encoding;
      pending_handle_.EncodeTo(&handle_encoding);
      index_block_.Add(Slice(last_key_), Slice(handle_encoding));
      pending_index_entry_ = false;
    }
    WriteBlock(&index_block_, &index_block_handle);
  }

  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);

    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    status_ = file_->Append(Slice(footer_encoding));
    if (status_.ok()) offset_ += footer_encoding.size();
  }

  if (ok()) status_ = file_->Flush();
  return status_;
}

void TableBuilder::Abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace sextant::lsm
