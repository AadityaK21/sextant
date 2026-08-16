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

RangeIterator::RangeIterator(std::unique_ptr<lsm::Iterator> iter, std::string until,
                             lsm::ReadStats* stats)
    : iter_(std::move(iter)), prefix_(std::move(until)), stats_(stats) {}

bool RangeIterator::Valid() const {
  if (!iter_->Valid()) return false;
  if (prefix_.empty()) return true;  // no upper bound: scan to the end
  return iter_->key().compare(Slice(prefix_)) < 0;
}

void RangeIterator::Next() {
  ++scanned_;
  // Counted here rather than by the caller so that keys_scanned means one
  // thing: entries the engine actually stepped over. A query cannot inflate or
  // deflate it by choosing when to stop reading.
  if (stats_ != nullptr) ++stats_->keys_scanned;
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
                                                       const std::string& until,
                                                       const ReadContext& ctx) {
  auto iter = db_->NewIterator(ctx.ToReadOptions());
  iter->Seek(Slice(from));
  return std::make_unique<RangeIterator>(std::move(iter), until, ctx.stats);
}

std::unique_ptr<RangeIterator> Store::NewPrefixIterator(const std::string& prefix,
                                                        const ReadContext& ctx) {
  return NewRangeIterator(prefix, PrefixUpperBound(prefix), ctx);
}

// --- entities ---

Status Store::GetEntity(TypeId type, const Ulid& id, std::string* payload,
                        const ReadContext& ctx) {
  return db_->Get(ctx.ToReadOptions(), Slice(EncodeEntityKey(type, id)), payload);
}

std::unique_ptr<RangeIterator> Store::ScanEntities(TypeId type,
                                                   const ReadContext& ctx) {
  // Entity ids are ULIDs, so this yields entities in CREATION ORDER for free.
  return NewPrefixIterator(EntityTypePrefix(type), ctx);
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
  return NewPrefixIterator(RawBatchPrefix(source, batch), ReadContext{});
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
  return NewPrefixIterator(SourceRecordPrefix(source), ReadContext{});
}

// --- ingest manifests ---

Status Store::PutIngestManifest(SourceId source, BatchId batch,
                                const Slice& bytes) {
  return db_->Put(lsm::WriteOptions{}, Slice(EncodeIngestKey(source, batch)),
                  bytes);
}

std::unique_ptr<RangeIterator> Store::ScanIngest(SourceId source) {
  return NewPrefixIterator(IngestPrefix(source), ReadContext{});
}

// --- graph traversal ---

std::unique_ptr<RangeIterator> Store::ScanOutgoing(const Ulid& src, LinkTypeId type,
                                                   const ReadContext& ctx) {
  return NewPrefixIterator(LinkOutPrefix(src, type), ctx);
}

std::unique_ptr<RangeIterator> Store::ScanIncoming(const Ulid& dst, LinkTypeId type,
                                                   const ReadContext& ctx) {
  return NewPrefixIterator(LinkInPrefix(dst, type), ctx);
}

std::unique_ptr<RangeIterator> Store::ScanOutgoing(const Ulid& src,
                                                   const ReadContext& ctx) {
  return NewPrefixIterator(LinkOutPrefix(src), ctx);
}

// --- the headline query ---

std::unique_ptr<RangeIterator> Store::ScanTimeRange(LinkTypeId link_type,
                                                    const Ulid& anchor,
                                                    int64_t from_inclusive,
                                                    int64_t to_exclusive,
                                                    const ReadContext& ctx) {
  // Both bounds are just encoded keys. Because the timestamp is sign-flipped
  // big-endian and sits immediately after the anchor, the window is one
  // contiguous byte range - nothing outside it is ever touched.
  return NewRangeIterator(TimeIndexBound(link_type, anchor, from_inclusive),
                          TimeIndexBound(link_type, anchor, to_exclusive), ctx);
}

// --- lineage ---

std::unique_ptr<RangeIterator> Store::ScanProvenance(const Ulid& entity,
                                                     const ReadContext& ctx) {
  return NewPrefixIterator(ProvenancePrefix(entity), ctx);
}

std::unique_ptr<RangeIterator> Store::ScanProvenance(const Ulid& entity, PropId prop,
                                                     const ReadContext& ctx) {
  return NewPrefixIterator(ProvenancePrefix(entity, prop), ctx);
}

// --- secondary index ---

std::unique_ptr<RangeIterator> Store::LookupString(TypeId type, PropId prop,
                                                   const Slice& value,
                                                   const ReadContext& ctx) {
  return NewPrefixIterator(IndexPrefixString(type, prop, value), ctx);
}

std::unique_ptr<RangeIterator> Store::RangeInt(TypeId type, PropId prop, int64_t from,
                                               int64_t to_exclusive,
                                               const ReadContext& ctx) {
  return NewRangeIterator(IndexBoundInt(type, prop, from),
                          IndexBoundInt(type, prop, to_exclusive), ctx);
}

std::unique_ptr<RangeIterator> Store::RangeDouble(TypeId type, PropId prop, double from,
                                                  double to_exclusive,
                                                  const ReadContext& ctx) {
  return NewRangeIterator(IndexBoundDouble(type, prop, from),
                          IndexBoundDouble(type, prop, to_exclusive), ctx);
}

std::unique_ptr<RangeIterator> Store::ScanIndex(TypeId type, PropId prop,
                                                const ReadContext& ctx) {
  return NewPrefixIterator(IndexPrefix(type, prop), ctx);
}

std::unique_ptr<RangeIterator> Store::PrefixString(TypeId type, PropId prop,
                                                   const Slice& value,
                                                   const ReadContext& ctx) {
  // An empty prefix would otherwise produce a bound of the property prefix
  // with its last byte incremented, which is right, but saying so explicitly
  // is cheaper than making a reader verify it.
  if (value.empty()) return ScanIndex(type, prop, ctx);
  return NewPrefixIterator(IndexPrefixStringPartial(type, prop, value), ctx);
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
  return NewPrefixIterator(BlockingPrefix(block_key_hash), ReadContext{});
}

Status Store::PutCandidate(double score, uint64_t pair_hash, const Slice& payload) {
  return db_->Put(lsm::WriteOptions{}, Slice(EncodeCandidateKey(score, pair_hash)),
                  payload);
}

Status Store::ClearKeyspace(Keyspace keyspace, uint64_t* deleted) {
  switch (keyspace) {
    case Keyspace::kRaw:
    case Keyspace::kSourceRecord:
    case Keyspace::kIngest:
      // These three are the record of what was ingested. Resolution recomputes
      // from them and must never be able to remove them, so this refuses
      // rather than trusting every future caller to remember.
      return Status::InvalidArgument(
          std::string("refusing to clear ") + KeyspaceName(keyspace) +
          ": it is ingested data, not derived");
    default:
      break;
  }

  std::string prefix(1, static_cast<char>(keyspace));
  const std::string until = PrefixUpperBound(prefix);
  uint64_t removed = 0;

  // Batched rather than one Delete per key: a tombstone per key is unavoidable,
  // but a WAL record per key is not, and this runs over every entity in the
  // store. Bounded so a large database does not build one enormous batch in
  // memory before anything reaches disk.
  constexpr int kBatchSize = 4096;
  while (true) {
    lsm::WriteBatch batch;
    {
      auto iter = db_->NewIterator(lsm::ReadOptions{});
      iter->Seek(Slice(prefix));
      for (; iter->Valid(); iter->Next()) {
        if (iter->key().compare(Slice(until)) >= 0) break;
        batch.Delete(iter->key());
        if (batch.Count() >= kBatchSize) break;
      }
      if (!iter->status().ok()) return iter->status();
    }
    if (batch.Count() == 0) break;
    removed += static_cast<uint64_t>(batch.Count());
    const Status s = db_->Write(lsm::WriteOptions{}, &batch);
    if (!s.ok()) return s;
  }

  if (deleted != nullptr) *deleted = removed;
  return Status::OK();
}

std::unique_ptr<RangeIterator> Store::ScanCandidates(const ReadContext& ctx) {
  // Scores are stored negated, so a forward scan is highest-score-first: the
  // pairs sitting closest to the decision boundary, which are the ones a human
  // should look at.
  return NewPrefixIterator(CandidatePrefix(), ctx);
}

}  // namespace sextant::codec
