// Builds one block of sorted key/value pairs with prefix compression.
//
// BLOCK FORMAT
//
//   entry:  varint32 shared_len      bytes of prefix shared with previous key
//           varint32 unshared_len    bytes that differ
//           varint32 value_len
//           char[unshared_len]       the differing key suffix
//           char[value_len]          the value
//   ...
//   uint32BE restart_offset[0..R-1]  byte offset of each restart entry
//   uint32BE num_restarts
//
// WHY PREFIX COMPRESSION. Keys arrive sorted, so adjacent keys share long
// prefixes. Storing only the delta is a large saving - and it is an unusually
// large saving for THIS system specifically, because every key is a structured
// composite by construction:
//
//   LINKOUT | src_entity(16) | link_type(2) | dst_entity(16)
//
// All of one entity's outgoing links share the first 19 bytes. Being able to
// say "my key design makes my block compression effective" is a genuine
// observation about the whole architecture, not a generic claim about LSMs.
//
// WHY RESTART POINTS. Prefix compression destroys random access: to decode
// entry N you need entry N-1, and so on back to the start. Every 16th entry is
// therefore stored with shared_len = 0 - a full key, decodable on its own.
// Those positions are collected in the restart array at the end of the block,
// which the reader BINARY SEARCHES. So a lookup is O(log(entries/16)) restart
// comparisons plus at most 15 sequential decodes, rather than a full scan.
//
// The interval is the tunable: smaller means faster seeks and worse
// compression. 16 is the conventional balance.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "internal_key.h"
#include "sextant/lsm/slice.h"

namespace sextant::lsm {

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval = 16);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  void Reset();

  // REQUIRES: key is strictly greater than the previous key UNDER THE
  // InternalKeyComparator - which is not the same as bytewise greater. Within
  // one user key, sequence numbers DESCEND, so the second version of a key is
  // bytewise *smaller* than the first while still being correctly ordered.
  // REQUIRES: Finish() has not been called.
  void Add(const Slice& key, const Slice& value);

  // Append the restart array and return the finished block contents.
  Slice Finish();

  // Size the block would occupy if finished right now. The table builder uses
  // this to decide when to cut a block.
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

 private:
  const int restart_interval_;
  InternalKeyComparator comparator_{};

  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;          // entries emitted since the last restart
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace sextant::lsm
