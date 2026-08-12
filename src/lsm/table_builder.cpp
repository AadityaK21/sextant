#include "table_builder.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

#include "coding.h"
#include "crc32c.h"

namespace sextant::lsm {

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval),
      index_block_(options.block_restart_interval) {
  if (options_.filter_policy != nullptr) {
    filter_block_ = std::make_unique<FilterBlockBuilder>(options_.filter_policy);
    filter_block_->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() { assert(closed_); }

void TableBuilder::Add(const Slice& key, const Slice& value) {
  assert(!closed_);
  if (!ok()) return;

  if (num_entries_ > 0) {
    assert(comparator_.Compare(key, Slice(last_key_)) > 0);
  }

  // The previous data block is complete, and we now know a key that is greater
  // than everything in it - so its index entry can finally be written.
  if (pending_index_entry_) {
    assert(data_block_.empty());
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  // The filter indexes USER keys. A lookup asks "does this table hold user key
  // K?" without knowing which sequence numbers exist, so hashing the internal
  // key would make every probe miss.
  if (filter_block_) {
    filter_block_->AddKey(ExtractUserKey(key));
  }

  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  data_block_.Add(key, value);

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
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
    if (filter_block_) filter_block_->StartBlock(offset_);
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

  BlockHandle filter_block_handle;
  BlockHandle metaindex_block_handle;
  BlockHandle index_block_handle;

  // The filter block is written raw - it has its own internal structure and is
  // not a sorted key/value block, so it never goes through BlockBuilder.
  if (ok() && filter_block_) {
    WriteRawBlock(filter_block_->Finish(), kNoCompression, &filter_block_handle);
  }

  // The metaindex maps a metadata name to a handle. Today that is only the
  // filter; keeping it a real block means adding more later costs nothing.
  if (ok()) {
    BlockBuilder meta_index_block(options_.block_restart_interval);
    if (filter_block_) {
      // Metaindex keys must satisfy the InternalKeyComparator like any other
      // block key, so a trailer is appended to make it a well-formed internal
      // key rather than a bare string.
      std::string key;
      AppendInternalKey(&key, ParsedInternalKey(Slice(kFilterBlockKey),
                                                kMaxSequenceNumber, kTypeValue));
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(Slice(key), Slice(handle_encoding));
    }
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
