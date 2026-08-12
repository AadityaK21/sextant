// The typed layer the ontology, resolver and query engine all code against.
//
// Nothing above this file ever encodes a key by hand. Store owns the mapping
// from domain concepts (entities, links, provenance) onto the eleven keyspaces,
// which means a layout change is one file rather than a search-and-replace.
//
// THE ATOMICITY GUARANTEE IS THE POINT OF EntityWriter.
//
// Committing one resolved entity is not one write. It is:
//
//     the ENTITY record
//   + one LINKOUT and one LINKIN per link          (two per edge)
//   + one XREF per source row that fed the entity
//   + one IDX entry per indexed property
//   + one PROV record per property
//   + one TIDX entry per time-indexed link
//
// If a crash lands in the middle of that, the ontology is left describing
// something that never existed: a link pointing at a missing entity, or a value
// with no provenance. EntityWriter accumulates the whole set into a single
// lsm::WriteBatch, so it reaches disk as one atomic unit or not at all.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "keyspace.h"
#include "sextant/lsm/db.h"
#include "sextant/lsm/iterator.h"
#include "sextant/lsm/status.h"
#include "ulid.h"

namespace sextant::codec {

using lsm::Status;

// Iterates a contiguous key range and stops at its end. Every traversal in the
// system - link following, index lookup, time-window scan - is one of these.
class RangeIterator {
 public:
  RangeIterator(std::unique_ptr<lsm::Iterator> iter, std::string prefix);

  bool Valid() const;
  void Next();

  Slice key() const { return iter_->key(); }
  Slice value() const { return iter_->value(); }
  Status status() const { return iter_->status(); }

  // How many keys the underlying engine actually visited. Reported so a query
  // can prove it did a range scan rather than a scan-and-filter.
  uint64_t keys_scanned() const { return scanned_; }

 private:
  std::unique_ptr<lsm::Iterator> iter_;
  std::string prefix_;
  uint64_t scanned_ = 0;
};

class Store;

// Accumulates every write belonging to one entity, then commits them together.
class EntityWriter {
 public:
  EntityWriter(Store* store, TypeId type, const Ulid& id);

  EntityWriter& SetPayload(const Slice& payload);

  // Writes BOTH directions of the edge. Doing it here rather than at the call
  // site is what stops a caller from accidentally producing a half-linked graph.
  EntityWriter& AddLink(LinkTypeId type, const Ulid& target,
                        const Slice& payload = Slice());

  // A link that is also time-indexed, so it can be range-scanned by timestamp.
  EntityWriter& AddTimedLink(LinkTypeId type, const Ulid& target, int64_t timestamp,
                             const Slice& payload = Slice());

  EntityWriter& AddIndexString(PropId prop, const Slice& value);
  EntityWriter& AddIndexInt(PropId prop, int64_t value);
  EntityWriter& AddIndexDouble(PropId prop, double value);

  EntityWriter& AddProvenance(PropId prop, uint64_t version, const Slice& record);

  // Record that a source row contributed to this entity, so lineage can go
  // backwards from entity to raw data.
  EntityWriter& AddCrossRef(SourceId source, uint64_t source_pk_hash);

  int operations() const { return batch_.Count(); }

  // Commit everything as one atomic unit.
  Status Commit(const lsm::WriteOptions& options = lsm::WriteOptions{});

 private:
  Store* store_;
  TypeId type_;
  Ulid id_;
  lsm::WriteBatch batch_;
};

// Everything produced by one input row, committed together.
//
// Ingest writes the verbatim bytes to RAW and one normalised SourceRecord per
// mapping that matched - a Digitraffic port call yields a Voyage, and other
// endpoints yield Ports and Vessels. Splitting those across separate writes
// would allow a crash to leave a normalised record whose raw row is missing,
// which is the one state that makes lineage unanswerable: the provenance points
// somewhere, and there is nothing there.
class RowWriter {
 public:
  RowWriter(Store* store, SourceId source, BatchId batch, RowSeq row);

  // The original bytes, exactly as the source produced them.
  RowWriter& SetRaw(const Slice& bytes);
  RowWriter& AddSourceRecord(uint64_t natural_key_hash, const Slice& bytes);

  int operations() const { return batch_.Count(); }
  Status Commit(const lsm::WriteOptions& options = lsm::WriteOptions{});

 private:
  Store* store_;
  SourceId source_;
  BatchId batch_id_;
  RowSeq row_;
  lsm::WriteBatch batch_;
};

class Store {
 public:
  static Status Open(const lsm::Options& options, const std::string& path,
                     std::unique_ptr<Store>* store);

  explicit Store(std::unique_ptr<lsm::DB> db) : db_(std::move(db)) {}

  lsm::DB* db() { return db_.get(); }

  // --- entities ---
  EntityWriter NewEntity(TypeId type) { return EntityWriter(this, type, Ulid::Generate()); }
  EntityWriter EditEntity(TypeId type, const Ulid& id) {
    return EntityWriter(this, type, id);
  }

  Status GetEntity(TypeId type, const Ulid& id, std::string* payload);
  std::unique_ptr<RangeIterator> ScanEntities(TypeId type);

  // --- raw records: the far end of every lineage chain ---
  Status PutRawRecord(SourceId source, BatchId batch, RowSeq row, const Slice& bytes);
  Status GetRawRecord(SourceId source, BatchId batch, RowSeq row, std::string* out);
  std::unique_ptr<RangeIterator> ScanRawBatch(SourceId source, BatchId batch);

  RowWriter NewRow(SourceId source, BatchId batch, RowSeq row) {
    return RowWriter(this, source, batch, row);
  }

  // --- source records: one normalised row, keyed by its natural key ---
  //
  // Unlike RAW, this IS overwritten by a later batch. RAW is the immutable
  // archive of what every ingest saw; SRCREC is the current normalised view of
  // each source row, which is what entity resolution consumes. Keeping both is
  // what lets a re-ingest update the picture without destroying the history.
  Status PutSourceRecord(SourceId source, uint64_t natural_key_hash,
                         const Slice& bytes);
  Status GetSourceRecord(SourceId source, uint64_t natural_key_hash,
                         std::string* out);
  std::unique_ptr<RangeIterator> ScanSourceRecords(SourceId source);

  // --- ingest manifests: what was loaded, when, and from what bytes ---
  Status PutIngestManifest(SourceId source, BatchId batch, const Slice& bytes);
  std::unique_ptr<RangeIterator> ScanIngest(SourceId source);

  // --- graph traversal ---
  std::unique_ptr<RangeIterator> ScanOutgoing(const Ulid& src, LinkTypeId type);
  std::unique_ptr<RangeIterator> ScanIncoming(const Ulid& dst, LinkTypeId type);
  std::unique_ptr<RangeIterator> ScanOutgoing(const Ulid& src);

  // --- THE headline query ---
  //
  // "All voyages through this port between two timestamps." A seek to the start
  // of the window followed by a sequential read, with no candidate examined and
  // rejected. Iteration stops as soon as a key exceeds the window.
  std::unique_ptr<RangeIterator> ScanTimeRange(LinkTypeId link_type, const Ulid& anchor,
                                               int64_t from_inclusive,
                                               int64_t to_exclusive);

  // --- lineage ---
  std::unique_ptr<RangeIterator> ScanProvenance(const Ulid& entity);
  std::unique_ptr<RangeIterator> ScanProvenance(const Ulid& entity, PropId prop);

  // --- secondary index ---
  std::unique_ptr<RangeIterator> LookupString(TypeId type, PropId prop,
                                              const Slice& value);
  std::unique_ptr<RangeIterator> RangeInt(TypeId type, PropId prop, int64_t from,
                                          int64_t to_exclusive);
  std::unique_ptr<RangeIterator> RangeDouble(TypeId type, PropId prop, double from,
                                             double to_exclusive);

  // --- cross reference: which entity did this source row land in? ---
  Status LookupCrossRef(SourceId source, uint64_t source_pk_hash, Ulid* entity);

  // --- entity resolution support ---
  Status PutBlockingKey(uint64_t block_key_hash, SourceId source, uint64_t record);
  std::unique_ptr<RangeIterator> ScanBlock(uint64_t block_key_hash);

  Status PutCandidate(double score, uint64_t pair_hash, const Slice& payload);
  std::unique_ptr<RangeIterator> ScanCandidates();  // most uncertain first

 private:
  friend class EntityWriter;
  friend class RowWriter;

  // Range-scan helper: seek to `from`, stop when a key reaches `until`.
  std::unique_ptr<RangeIterator> NewRangeIterator(const std::string& from,
                                                  const std::string& until);
  // Prefix scan, which is a range whose upper bound is the prefix successor.
  std::unique_ptr<RangeIterator> NewPrefixIterator(const std::string& prefix);

  std::unique_ptr<lsm::DB> db_;
};

}  // namespace sextant::codec
