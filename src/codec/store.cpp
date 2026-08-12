#include "store.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace sextant::codec {

// --- RangeIterator ----------------------------------------------------------
//
// `prefix_` is used as an EXCLUSIVE UPPER BOUND, not as a starts-with test.
// That distinction matters: comparing against a precomputed bound is one
// memcmp per key and terminates the scan the instant it leaves the range,
// whereas a starts-with test would keep walking and discarding.

RangeIterator::RangeIterator(std::unique_ptr<lsm::Iterator> iter, std::string until)
    : iter_(std::move(iter)), prefix_(std::move(until)) {}

bool RangeIterator::Valid() const {
  if (!iter_->Valid()) return false;
  if (prefix_.empty()) return true;  // no upper bound: scan to the end
  return iter_->key().compare(Slice(prefix_)) < 0;
}

void RangeIterator::Next() {
  ++scanned_;
  iter_->Next();
}

// --- EntityWriter -----------------------------------------------------------

EntityWriter::EntityWriter(Store* store, TypeId type, const Ulid& id)
    : store_(store), type_(type), id_(id) {}

EntityWriter& EntityWriter::SetPayload(const Slice& payload) {
  batch_.Put(Slice(EncodeEntityKey(type_, id_)), payload);
  return *this;
}

EntityWriter& EntityWriter::AddLink(LinkTypeId type, const Ulid& target,
                                    const Slice& payload) {
  // Both directions, always, in the same batch. Writing only one would leave a
  // graph that traverses correctly forwards and silently loses edges backwards.
  batch_.Put(Slice(EncodeLinkOutKey(id_, type, target)), payload);
  batch_.Put(Slice(EncodeLinkInKey(target, type, id_)), payload);
  return *this;
}

EntityWriter& EntityWriter::AddTimedLink(LinkTypeId type, const Ulid& target,
                                         int64_t timestamp, const Slice& payload) {
  AddLink(type, target, payload);
  // Anchored on the TARGET: the question is "what arrived at this port in this
  // window", so the port is what the scan is anchored to.
  batch_.Put(Slice(EncodeTimeIndexKey(type, target, timestamp, id_)), Slice());
  return *this;
}

EntityWriter& EntityWriter::AddIndexString(PropId prop, const Slice& value) {
  batch_.Put(Slice(EncodeIndexKeyString(type_, prop, value, id_)), Slice());
  return *this;
}

EntityWriter& EntityWriter::AddIndexInt(PropId prop, int64_t value) {
  batch_.Put(Slice(EncodeIndexKeyInt(type_, prop, value, id_)), Slice());
  return *this;
}

EntityWriter& EntityWriter::AddIndexDouble(PropId prop, double value) {
  batch_.Put(Slice(EncodeIndexKeyDouble(type_, prop, value, id_)), Slice());
  return *this;
}

EntityWriter& EntityWriter::AddProvenance(PropId prop, uint64_t version,
                                          const Slice& record) {
  batch_.Put(Slice(EncodeProvenanceKey(id_, prop, version)), record);
  return *this;
}

EntityWriter& EntityWriter::AddCrossRef(SourceId source, uint64_t source_pk_hash) {
  batch_.Put(Slice(EncodeCrossRefKey(source, source_pk_hash)), id_.AsSlice());
  return *this;
}

Status EntityWriter::Commit(const lsm::WriteOptions& options) {
  return store_->db_->Write(options, &batch_);
}

// --- RowWriter --------------------------------------------------------------

RowWriter::RowWriter(Store* store, SourceId source, BatchId batch, RowSeq row)
    : store_(store), source_(source), batch_id_(batch), row_(row) {}

RowWriter& RowWriter::SetRaw(const Slice& bytes) {
  batch_.Put(Slice(EncodeRawKey(source_, batch_id_, row_)), bytes);
  return *this;
}

RowWriter& RowWriter::AddSourceRecord(uint64_t natural_key_hash,
                                      const Slice& bytes) {
  batch_.Put(Slice(EncodeSourceRecordKey(source_, natural_key_hash)), bytes);
  return *this;
}

Status RowWriter::Commit(const lsm::WriteOptions& options) {
  return store_->db_->Write(options, &batch_);
}

// --- Store ------------------------------------------------------------------

Status Store::Open(const lsm::Options& options, const std::string& path,
                   std::unique_ptr<Store>* store) {
  std::unique_ptr<lsm::DB> db;
  Status s = lsm::DB::Open(options, path, &db);
  if (!s.ok()) return s;
  *store = std::make_unique<Store>(std::move(db));
  return Status::OK();
}

std::unique_ptr<RangeIterator> Store::NewRangeIterator(const std::string& from,
                                                       const std::string& until) {
  auto iter = db_->NewIterator(lsm::ReadOptions{});
  iter->Seek(Slice(from));
  return std::make_unique<RangeIterator>(std::move(iter), until);
}

std::unique_ptr<RangeIterator> Store::NewPrefixIterator(const std::string& prefix) {
  return NewRangeIterator(prefix, PrefixUpperBound(prefix));
}

// --- entities ---

Status Store::GetEntity(TypeId type, const Ulid& id, std::string* payload) {
  return db_->Get(lsm::ReadOptions{}, Slice(EncodeEntityKey(type, id)), payload);
}

std::unique_ptr<RangeIterator> Store::ScanEntities(TypeId type) {
  // Entity ids are ULIDs, so this yields entities in CREATION ORDER for free.
  return NewPrefixIterator(EntityTypePrefix(type));
}

// --- raw records ---

Status Store::PutRawRecord(SourceId source, BatchId batch, RowSeq row,
                           const Slice& bytes) {
  return db_->Put(lsm::WriteOptions{}, Slice(EncodeRawKey(source, batch, row)), bytes);
}

Status Store::GetRawRecord(SourceId source, BatchId batch, RowSeq row,
                           std::string* out) {
  return db_->Get(lsm::ReadOptions{}, Slice(EncodeRawKey(source, batch, row)), out);
}

std::unique_ptr<RangeIterator> Store::ScanRawBatch(SourceId source, BatchId batch) {
  return NewPrefixIterator(RawBatchPrefix(source, batch));
}

// --- source records ---

Status Store::PutSourceRecord(SourceId source, uint64_t natural_key_hash,
                              const Slice& bytes) {
  return db_->Put(lsm::WriteOptions{},
                  Slice(EncodeSourceRecordKey(source, natural_key_hash)), bytes);
}

Status Store::GetSourceRecord(SourceId source, uint64_t natural_key_hash,
                              std::string* out) {
  return db_->Get(lsm::ReadOptions{},
                  Slice(EncodeSourceRecordKey(source, natural_key_hash)), out);
}

std::unique_ptr<RangeIterator> Store::ScanSourceRecords(SourceId source) {
  return NewPrefixIterator(SourceRecordPrefix(source));
}

// --- ingest manifests ---

Status Store::PutIngestManifest(SourceId source, BatchId batch,
                                const Slice& bytes) {
  return db_->Put(lsm::WriteOptions{}, Slice(EncodeIngestKey(source, batch)),
                  bytes);
}

std::unique_ptr<RangeIterator> Store::ScanIngest(SourceId source) {
  return NewPrefixIterator(IngestPrefix(source));
}

// --- graph traversal ---

std::unique_ptr<RangeIterator> Store::ScanOutgoing(const Ulid& src, LinkTypeId type) {
  return NewPrefixIterator(LinkOutPrefix(src, type));
}

std::unique_ptr<RangeIterator> Store::ScanIncoming(const Ulid& dst, LinkTypeId type) {
  return NewPrefixIterator(LinkInPrefix(dst, type));
}

std::unique_ptr<RangeIterator> Store::ScanOutgoing(const Ulid& src) {
  return NewPrefixIterator(LinkOutPrefix(src));
}

// --- the headline query ---

std::unique_ptr<RangeIterator> Store::ScanTimeRange(LinkTypeId link_type,
                                                    const Ulid& anchor,
                                                    int64_t from_inclusive,
                                                    int64_t to_exclusive) {
  // Both bounds are just encoded keys. Because the timestamp is sign-flipped
  // big-endian and sits immediately after the anchor, the window is one
  // contiguous byte range - nothing outside it is ever touched.
  return NewRangeIterator(TimeIndexBound(link_type, anchor, from_inclusive),
                          TimeIndexBound(link_type, anchor, to_exclusive));
}

// --- lineage ---

std::unique_ptr<RangeIterator> Store::ScanProvenance(const Ulid& entity) {
  return NewPrefixIterator(ProvenancePrefix(entity));
}

std::unique_ptr<RangeIterator> Store::ScanProvenance(const Ulid& entity, PropId prop) {
  return NewPrefixIterator(ProvenancePrefix(entity, prop));
}

// --- secondary index ---

std::unique_ptr<RangeIterator> Store::LookupString(TypeId type, PropId prop,
                                                   const Slice& value) {
  return NewPrefixIterator(IndexPrefixString(type, prop, value));
}

std::unique_ptr<RangeIterator> Store::RangeInt(TypeId type, PropId prop, int64_t from,
                                               int64_t to_exclusive) {
  return NewRangeIterator(IndexBoundInt(type, prop, from),
                          IndexBoundInt(type, prop, to_exclusive));
}

std::unique_ptr<RangeIterator> Store::RangeDouble(TypeId type, PropId prop, double from,
                                                  double to_exclusive) {
  return NewRangeIterator(IndexBoundDouble(type, prop, from),
                          IndexBoundDouble(type, prop, to_exclusive));
}

// --- cross reference ---

Status Store::LookupCrossRef(SourceId source, uint64_t source_pk_hash, Ulid* entity) {
  std::string value;
  Status s = db_->Get(lsm::ReadOptions{},
                      Slice(EncodeCrossRefKey(source, source_pk_hash)), &value);
  if (!s.ok()) return s;
  if (!Ulid::FromBinary(Slice(value), entity)) {
    return Status::Corruption("XREF value is not a 16-byte entity id");
  }
  return Status::OK();
}

// --- entity resolution support ---

Status Store::PutBlockingKey(uint64_t block_key_hash, SourceId source,
                             uint64_t record) {
  return db_->Put(lsm::WriteOptions{},
                  Slice(EncodeBlockingKey(block_key_hash, source, record)), Slice());
}

std::unique_ptr<RangeIterator> Store::ScanBlock(uint64_t block_key_hash) {
  return NewPrefixIterator(BlockingPrefix(block_key_hash));
}

Status Store::PutCandidate(double score, uint64_t pair_hash, const Slice& payload) {
  return db_->Put(lsm::WriteOptions{}, Slice(EncodeCandidateKey(score, pair_hash)),
                  payload);
}

std::unique_ptr<RangeIterator> Store::ScanCandidates() {
  // Scores are stored negated, so a forward scan is highest-score-first: the
  // pairs sitting closest to the decision boundary, which are the ones a human
  // should look at.
  return NewPrefixIterator(CandidatePrefix());
}

}  // namespace sextant::codec
