// An atomic group of mutations.
//
// WHY THIS EXISTS, in the language of this project rather than of LevelDB:
//
// An entity merge is not one write. It writes the ENTITY record, both
// directions of every LINK, the XREF entries mapping each source row to the
// resolved entity, the secondary index entries, and one PROV record per
// property. If a crash lands in the middle of that, the ontology is left
// internally inconsistent — a link pointing at an entity that does not exist,
// or a value with no provenance.
//
// WriteBatch makes the whole merge one atomic unit: one WAL record, one
// sequence range, all-or-nothing on recovery. It is also the fast path, since
// N logical writes cost one fsync rather than N.
//
// Serialised format:
//
//   uint64BE  sequence        sequence number of the FIRST record
//   uint32BE  count           number of records
//   record*                   count records, each:
//       kTypeValue    : 1 byte || varstring key || varstring value
//       kTypeDeletion : 1 byte || varstring key

#pragma once

#include <cstdint>
#include <string>

#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

class MemTable;

class WriteBatch {
 public:
  WriteBatch();

  void Put(const Slice& key, const Slice& value);
  void Delete(const Slice& key);
  void Clear();

  int Count() const;
  size_t ApproximateSize() const { return rep_.size(); }

  // Handler for iterating a batch. The memtable insert path is one
  // implementation; a replication or CDC sink would be another.
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  Status Iterate(Handler* handler) const;

  // --- internals used by the DB write path ---
  const std::string& Contents() const { return rep_; }
  void SetContents(const Slice& contents);
  uint64_t Sequence() const;
  void SetSequence(uint64_t seq);
  void SetCount(int n);

  static constexpr size_t kHeaderSize = 12;  // 8-byte sequence + 4-byte count

  Status InsertInto(MemTable* memtable) const;

 private:
  std::string rep_;
};

}  // namespace sextant::lsm
