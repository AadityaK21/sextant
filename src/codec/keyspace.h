// The eleven keyspaces.
//
// This file is where the storage engine stops being a generic key-value store
// and becomes an ontology engine. Everything above it - entities, links,
// provenance, indexes - is bytes in ONE ordered keyspace, laid out so that the
// query each one serves is a contiguous range scan rather than a scan plus a
// filter.
//
//   prefix  name      key layout                                    value
//   ------  --------  --------------------------------------------  --------------
//   0x01    RAW       src(4) batch(8) row(8)                        original bytes
//   0x02    SRCREC    src(4) natural_key_hash(8)                    SourceRecord
//   0x03    ENTITY    type(2) eid(16)                               ResolvedEntity
//   0x04    LINKOUT   src_eid(16) link_type(2) dst_eid(16)          edge payload
//   0x05    LINKIN    dst_eid(16) link_type(2) src_eid(16)          edge payload
//   0x06    PROV      eid(16) prop(2) version(8)                    Provenance
//   0x07    XREF      src(4) src_pk_hash(8)                         eid(16)
//   0x08    BLOCK     block_key_hash(8) src(4) rec(8)               empty
//   0x09    IDX       type(2) prop(2) <ordered value> eid(16)       empty
//   0x0A    TIDX      link_type(2) anchor_eid(16) ts_be(8) eid(16)  empty
//   0x0B    CAND      score_inv(4) pair_hash(8)                     candidate pair
//   0x0C    INGEST    src(4) batch(8)                               BatchManifest
//
// THREE LAYOUT DECISIONS WORTH DEFENDING
//
// 1. LINKS ARE STORED TWICE. LINKOUT is keyed by source, LINKIN by
//    destination. Cost: 2x link storage, and both writes must be in the same
//    WriteBatch or the graph can be left inconsistent. Benefit: "which voyages
//    arrived AT this port" is a prefix scan rather than a full table scan, and
//    that is the most common query in this domain. In an ordered KV store you
//    denormalise by writing the same fact under every ordering you query by.
//
// 2. TIDX PUTS A BIG-ENDIAN TIMESTAMP AFTER THE ANCHOR. This is what makes
//    "all voyages into Rotterdam between April and July" a seek plus a
//    sequential read, with no candidate examined and rejected. Little-endian
//    would make byte order meaningless and degrade it to a full scan.
//
// 3. CAND STORES AN INVERTED SCORE FIRST. Scanning the review queue from the
//    start therefore yields the most uncertain pairs first, for free.

#pragma once

#include <cstdint>
#include <string>

#include "ordered.h"
#include "sextant/lsm/slice.h"
#include "ulid.h"

namespace sextant::codec {

using lsm::Slice;

// One byte, so a prefix scan over an entire keyspace is a single-byte seek.
enum class Keyspace : uint8_t {
  kRaw = 0x01,
  kSourceRecord = 0x02,
  kEntity = 0x03,
  kLinkOut = 0x04,
  kLinkIn = 0x05,
  kProvenance = 0x06,
  kCrossRef = 0x07,
  kBlocking = 0x08,
  kIndex = 0x09,
  kTimeIndex = 0x0A,
  kCandidate = 0x0B,
  kIngest = 0x0C,
};

using SourceId = uint32_t;
using BatchId = uint64_t;
using RowSeq = uint64_t;
using TypeId = uint16_t;
using LinkTypeId = uint16_t;
using PropId = uint16_t;

// --- RAW: the immutable original bytes of one source row --------------------
//
// Lineage points here. Because batches are append-only and this is never
// overwritten, "show me the row this value came from" is a point lookup that
// stays valid forever.
std::string EncodeRawKey(SourceId source, BatchId batch, RowSeq row);
bool DecodeRawKey(const Slice& key, SourceId* source, BatchId* batch, RowSeq* row);
std::string RawBatchPrefix(SourceId source, BatchId batch);

// --- SRCREC: a normalised record extracted from one source row --------------
std::string EncodeSourceRecordKey(SourceId source, uint64_t natural_key_hash);
bool DecodeSourceRecordKey(const Slice& key, SourceId* source, uint64_t* hash);
std::string SourceRecordPrefix(SourceId source);

// --- ENTITY: one resolved real-world thing ----------------------------------
std::string EncodeEntityKey(TypeId type, const Ulid& id);
bool DecodeEntityKey(const Slice& key, TypeId* type, Ulid* id);
std::string EntityTypePrefix(TypeId type);

// --- LINKOUT / LINKIN: the graph, written in both directions ----------------
std::string EncodeLinkOutKey(const Ulid& src, LinkTypeId type, const Ulid& dst);
std::string EncodeLinkInKey(const Ulid& dst, LinkTypeId type, const Ulid& src);
bool DecodeLinkOutKey(const Slice& key, Ulid* src, LinkTypeId* type, Ulid* dst);
bool DecodeLinkInKey(const Slice& key, Ulid* dst, LinkTypeId* type, Ulid* src);

// Prefix for "every outgoing link of this type" - one contiguous range.
std::string LinkOutPrefix(const Ulid& src, LinkTypeId type);
std::string LinkInPrefix(const Ulid& dst, LinkTypeId type);
// Prefix for every link regardless of type.
std::string LinkOutPrefix(const Ulid& src);
std::string LinkInPrefix(const Ulid& dst);

// --- PROV: cell-level lineage, versioned by LSM sequence number -------------
std::string EncodeProvenanceKey(const Ulid& entity, PropId prop, uint64_t version);
bool DecodeProvenanceKey(const Slice& key, Ulid* entity, PropId* prop,
                         uint64_t* version);
std::string ProvenancePrefix(const Ulid& entity);
std::string ProvenancePrefix(const Ulid& entity, PropId prop);

// --- XREF: which entity did this source row end up in? ----------------------
std::string EncodeCrossRefKey(SourceId source, uint64_t source_pk_hash);
bool DecodeCrossRefKey(const Slice& key, SourceId* source, uint64_t* hash);

// --- BLOCK: the entity-resolution blocking index ----------------------------
//
// Candidate generation scans one block key and gets every record that hashed
// into it, which is what turns O(n^2) pair comparison into something feasible.
std::string EncodeBlockingKey(uint64_t block_key_hash, SourceId source,
                              uint64_t record);
bool DecodeBlockingKey(const Slice& key, uint64_t* block_key_hash,
                       SourceId* source, uint64_t* record);
std::string BlockingPrefix(uint64_t block_key_hash);

// --- IDX: secondary index on an entity property -----------------------------
//
// The value is order-preserving encoded (see ordered.h) so that a range scan
// over the index answers "ports with latitude between 50 and 52" directly.
std::string EncodeIndexKeyString(TypeId type, PropId prop, const Slice& value,
                                 const Ulid& entity);
std::string EncodeIndexKeyInt(TypeId type, PropId prop, int64_t value,
                              const Ulid& entity);
std::string EncodeIndexKeyDouble(TypeId type, PropId prop, double value,
                                 const Ulid& entity);

// Prefix for an exact-match lookup on a string value.
std::string IndexPrefixString(TypeId type, PropId prop, const Slice& value);
// Prefix for the whole property, used as the start of a range scan.
std::string IndexPrefix(TypeId type, PropId prop);
// Lower bound for a numeric range scan.
std::string IndexBoundInt(TypeId type, PropId prop, int64_t value);
std::string IndexBoundDouble(TypeId type, PropId prop, double value);

// Recover the entity id from an index key. The value is variable-length, so
// this reads from the END of the key rather than parsing forward.
bool DecodeIndexKeyEntity(const Slice& key, Ulid* entity);

// --- TIDX: time-ordered traversal index -------------------------------------
//
// THE key that makes the headline query fast. See the file header.
std::string EncodeTimeIndexKey(LinkTypeId link_type, const Ulid& anchor,
                               int64_t timestamp, const Ulid& entity);
bool DecodeTimeIndexKey(const Slice& key, LinkTypeId* link_type, Ulid* anchor,
                        int64_t* timestamp, Ulid* entity);
// Everything anchored here, in time order.
std::string TimeIndexPrefix(LinkTypeId link_type, const Ulid& anchor);
// Seek target for the start of a time window.
std::string TimeIndexBound(LinkTypeId link_type, const Ulid& anchor,
                           int64_t timestamp);

// --- CAND: entity-resolution pairs awaiting human review --------------------
//
// Keyed by inverted score so a scan returns the most uncertain pairs first.
std::string EncodeCandidateKey(double score, uint64_t pair_hash);
bool DecodeCandidateKey(const Slice& key, double* score, uint64_t* pair_hash);
std::string CandidatePrefix();

// --- INGEST: one record per ingest run --------------------------------------
//
// Added in the connector milestone. RAW is append-only, which is what makes
// lineage permanent, but it also means "have I loaded this file already?" has
// no answer inside RAW itself - every re-ingest just appends another batch.
// This keyspace records what each batch was, including a fingerprint of the
// input bytes, so a repeated ingest of an unchanged file is a no-op while a
// changed file gets a new batch and never overwrites the old one.
//
// Batch ids ascend, so scanning the prefix backwards from the upper bound
// gives the most recent batch for a source.
std::string EncodeIngestKey(SourceId source, BatchId batch);
bool DecodeIngestKey(const Slice& key, SourceId* source, BatchId* batch);
std::string IngestPrefix(SourceId source);

// --- helpers ----------------------------------------------------------------

// The exclusive upper bound for a prefix scan: the prefix with its last byte
// incremented. Iterating from `prefix` until a key is >= this covers exactly
// the prefix and nothing else.
std::string PrefixUpperBound(const std::string& prefix);

Keyspace KeyspaceOf(const Slice& key);
const char* KeyspaceName(Keyspace ks);

}  // namespace sextant::codec
