#include "keyspace.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace sextant::codec {
namespace {

void PutPrefix(std::string* dst, Keyspace ks) {
  dst->push_back(static_cast<char>(ks));
}

void PutU16(std::string* dst, uint16_t v) {
  dst->push_back(static_cast<char>((v >> 8) & 0xFF));
  dst->push_back(static_cast<char>(v & 0xFF));
}

void PutU32(std::string* dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    dst->push_back(static_cast<char>((v >> (24 - 8 * i)) & 0xFF));
  }
}

void PutU64(std::string* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    dst->push_back(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
  }
}

void PutUlid(std::string* dst, const Ulid& id) {
  dst->append(reinterpret_cast<const char*>(id.data()), Ulid::kBinarySize);
}

uint16_t GetU16(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

uint32_t GetU32(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}

uint64_t GetU64(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
  return v;
}

bool GetUlid(const char* p, Ulid* out) {
  return Ulid::FromBinary(Slice(p, Ulid::kBinarySize), out);
}

bool CheckPrefix(const Slice& key, Keyspace ks, size_t expected_size) {
  return key.size() == expected_size &&
         static_cast<uint8_t>(key[0]) == static_cast<uint8_t>(ks);
}

constexpr size_t kU = Ulid::kBinarySize;  // 16

}  // namespace

// --- RAW --------------------------------------------------------------------

std::string EncodeRawKey(SourceId source, BatchId batch, RowSeq row) {
  std::string k;
  k.reserve(1 + 4 + 8 + 8);
  PutPrefix(&k, Keyspace::kRaw);
  PutU32(&k, source);
  PutU64(&k, batch);
  PutU64(&k, row);
  return k;
}

bool DecodeRawKey(const Slice& key, SourceId* source, BatchId* batch, RowSeq* row) {
  if (!CheckPrefix(key, Keyspace::kRaw, 1 + 4 + 8 + 8)) return false;
  *source = GetU32(key.data() + 1);
  *batch = GetU64(key.data() + 5);
  *row = GetU64(key.data() + 13);
  return true;
}

std::string RawBatchPrefix(SourceId source, BatchId batch) {
  std::string k;
  PutPrefix(&k, Keyspace::kRaw);
  PutU32(&k, source);
  PutU64(&k, batch);
  return k;
}

// --- SRCREC -----------------------------------------------------------------

std::string EncodeSourceRecordKey(SourceId source, uint64_t natural_key_hash) {
  std::string k;
  PutPrefix(&k, Keyspace::kSourceRecord);
  PutU32(&k, source);
  PutU64(&k, natural_key_hash);
  return k;
}

bool DecodeSourceRecordKey(const Slice& key, SourceId* source, uint64_t* hash) {
  if (!CheckPrefix(key, Keyspace::kSourceRecord, 1 + 4 + 8)) return false;
  *source = GetU32(key.data() + 1);
  *hash = GetU64(key.data() + 5);
  return true;
}

std::string SourceRecordPrefix(SourceId source) {
  std::string k;
  PutPrefix(&k, Keyspace::kSourceRecord);
  PutU32(&k, source);
  return k;
}

// --- ENTITY -----------------------------------------------------------------

std::string EncodeEntityKey(TypeId type, const Ulid& id) {
  std::string k;
  k.reserve(1 + 2 + kU);
  PutPrefix(&k, Keyspace::kEntity);
  PutU16(&k, type);
  PutUlid(&k, id);
  return k;
}

bool DecodeEntityKey(const Slice& key, TypeId* type, Ulid* id) {
  if (!CheckPrefix(key, Keyspace::kEntity, 1 + 2 + kU)) return false;
  *type = GetU16(key.data() + 1);
  return GetUlid(key.data() + 3, id);
}

std::string EntityTypePrefix(TypeId type) {
  std::string k;
  PutPrefix(&k, Keyspace::kEntity);
  PutU16(&k, type);
  return k;
}

// --- LINKS ------------------------------------------------------------------

std::string EncodeLinkOutKey(const Ulid& src, LinkTypeId type, const Ulid& dst) {
  std::string k;
  k.reserve(1 + kU + 2 + kU);
  PutPrefix(&k, Keyspace::kLinkOut);
  PutUlid(&k, src);
  PutU16(&k, type);
  PutUlid(&k, dst);
  return k;
}

std::string EncodeLinkInKey(const Ulid& dst, LinkTypeId type, const Ulid& src) {
  std::string k;
  k.reserve(1 + kU + 2 + kU);
  PutPrefix(&k, Keyspace::kLinkIn);
  PutUlid(&k, dst);
  PutU16(&k, type);
  PutUlid(&k, src);
  return k;
}

bool DecodeLinkOutKey(const Slice& key, Ulid* src, LinkTypeId* type, Ulid* dst) {
  if (!CheckPrefix(key, Keyspace::kLinkOut, 1 + kU + 2 + kU)) return false;
  if (!GetUlid(key.data() + 1, src)) return false;
  *type = GetU16(key.data() + 1 + kU);
  return GetUlid(key.data() + 1 + kU + 2, dst);
}

bool DecodeLinkInKey(const Slice& key, Ulid* dst, LinkTypeId* type, Ulid* src) {
  if (!CheckPrefix(key, Keyspace::kLinkIn, 1 + kU + 2 + kU)) return false;
  if (!GetUlid(key.data() + 1, dst)) return false;
  *type = GetU16(key.data() + 1 + kU);
  return GetUlid(key.data() + 1 + kU + 2, src);
}

std::string LinkOutPrefix(const Ulid& src, LinkTypeId type) {
  std::string k;
  PutPrefix(&k, Keyspace::kLinkOut);
  PutUlid(&k, src);
  PutU16(&k, type);
  return k;
}

std::string LinkInPrefix(const Ulid& dst, LinkTypeId type) {
  std::string k;
  PutPrefix(&k, Keyspace::kLinkIn);
  PutUlid(&k, dst);
  PutU16(&k, type);
  return k;
}

std::string LinkOutPrefix(const Ulid& src) {
  std::string k;
  PutPrefix(&k, Keyspace::kLinkOut);
  PutUlid(&k, src);
  return k;
}

std::string LinkInPrefix(const Ulid& dst) {
  std::string k;
  PutPrefix(&k, Keyspace::kLinkIn);
  PutUlid(&k, dst);
  return k;
}

// --- PROVENANCE -------------------------------------------------------------

std::string EncodeProvenanceKey(const Ulid& entity, PropId prop, uint64_t version) {
  std::string k;
  k.reserve(1 + kU + 2 + 8);
  PutPrefix(&k, Keyspace::kProvenance);
  PutUlid(&k, entity);
  PutU16(&k, prop);
  PutU64(&k, version);
  return k;
}

bool DecodeProvenanceKey(const Slice& key, Ulid* entity, PropId* prop,
                         uint64_t* version) {
  if (!CheckPrefix(key, Keyspace::kProvenance, 1 + kU + 2 + 8)) return false;
  if (!GetUlid(key.data() + 1, entity)) return false;
  *prop = GetU16(key.data() + 1 + kU);
  *version = GetU64(key.data() + 1 + kU + 2);
  return true;
}

std::string ProvenancePrefix(const Ulid& entity) {
  std::string k;
  PutPrefix(&k, Keyspace::kProvenance);
  PutUlid(&k, entity);
  return k;
}

std::string ProvenancePrefix(const Ulid& entity, PropId prop) {
  std::string k;
  PutPrefix(&k, Keyspace::kProvenance);
  PutUlid(&k, entity);
  PutU16(&k, prop);
  return k;
}

// --- XREF -------------------------------------------------------------------

std::string EncodeCrossRefKey(SourceId source, uint64_t source_pk_hash) {
  std::string k;
  PutPrefix(&k, Keyspace::kCrossRef);
  PutU32(&k, source);
  PutU64(&k, source_pk_hash);
  return k;
}

bool DecodeCrossRefKey(const Slice& key, SourceId* source, uint64_t* hash) {
  if (!CheckPrefix(key, Keyspace::kCrossRef, 1 + 4 + 8)) return false;
  *source = GetU32(key.data() + 1);
  *hash = GetU64(key.data() + 5);
  return true;
}

// --- BLOCKING ---------------------------------------------------------------

std::string EncodeBlockingKey(uint64_t block_key_hash, SourceId source,
                              uint64_t record) {
  std::string k;
  PutPrefix(&k, Keyspace::kBlocking);
  PutU64(&k, block_key_hash);
  PutU32(&k, source);
  PutU64(&k, record);
  return k;
}

bool DecodeBlockingKey(const Slice& key, uint64_t* block_key_hash,
                       SourceId* source, uint64_t* record) {
  if (!CheckPrefix(key, Keyspace::kBlocking, 1 + 8 + 4 + 8)) return false;
  *block_key_hash = GetU64(key.data() + 1);
  *source = GetU32(key.data() + 9);
  *record = GetU64(key.data() + 13);
  return true;
}

std::string BlockingPrefix(uint64_t block_key_hash) {
  std::string k;
  PutPrefix(&k, Keyspace::kBlocking);
  PutU64(&k, block_key_hash);
  return k;
}

// --- SECONDARY INDEX --------------------------------------------------------

std::string IndexPrefix(TypeId type, PropId prop) {
  std::string k;
  PutPrefix(&k, Keyspace::kIndex);
  PutU16(&k, type);
  PutU16(&k, prop);
  return k;
}

std::string EncodeIndexKeyString(TypeId type, PropId prop, const Slice& value,
                                 const Ulid& entity) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedString(&k, value);  // escaped + terminated, so order survives
  PutUlid(&k, entity);
  return k;
}

std::string EncodeIndexKeyInt(TypeId type, PropId prop, int64_t value,
                              const Ulid& entity) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedInt64(&k, value);
  PutUlid(&k, entity);
  return k;
}

std::string EncodeIndexKeyDouble(TypeId type, PropId prop, double value,
                                 const Ulid& entity) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedDouble(&k, value);
  PutUlid(&k, entity);
  return k;
}

std::string IndexPrefixString(TypeId type, PropId prop, const Slice& value) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedString(&k, value);
  return k;
}

std::string IndexPrefixStringPartial(TypeId type, PropId prop, const Slice& value) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedStringPrefix(&k, value);
  return k;
}

std::string IndexBoundInt(TypeId type, PropId prop, int64_t value) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedInt64(&k, value);
  return k;
}

std::string IndexBoundDouble(TypeId type, PropId prop, double value) {
  std::string k = IndexPrefix(type, prop);
  EncodeOrderedDouble(&k, value);
  return k;
}

bool DecodeIndexKeyEntity(const Slice& key, Ulid* entity) {
  // The encoded value is variable-length, so the entity id is found from the
  // END of the key rather than by parsing forward past the value.
  if (key.size() < 1 + 2 + 2 + kU) return false;
  if (static_cast<uint8_t>(key[0]) != static_cast<uint8_t>(Keyspace::kIndex)) {
    return false;
  }
  return GetUlid(key.data() + key.size() - kU, entity);
}

// --- TIME INDEX -------------------------------------------------------------

std::string TimeIndexPrefix(LinkTypeId link_type, const Ulid& anchor) {
  std::string k;
  PutPrefix(&k, Keyspace::kTimeIndex);
  PutU16(&k, link_type);
  PutUlid(&k, anchor);
  return k;
}

std::string TimeIndexBound(LinkTypeId link_type, const Ulid& anchor,
                           int64_t timestamp) {
  std::string k = TimeIndexPrefix(link_type, anchor);
  // Sign-flipped big-endian, so byte order equals chronological order even for
  // timestamps before the epoch.
  EncodeOrderedInt64(&k, timestamp);
  return k;
}

std::string EncodeTimeIndexKey(LinkTypeId link_type, const Ulid& anchor,
                               int64_t timestamp, const Ulid& entity) {
  std::string k = TimeIndexBound(link_type, anchor, timestamp);
  PutUlid(&k, entity);
  return k;
}

bool DecodeTimeIndexKey(const Slice& key, LinkTypeId* link_type, Ulid* anchor,
                        int64_t* timestamp, Ulid* entity) {
  if (!CheckPrefix(key, Keyspace::kTimeIndex, 1 + 2 + kU + 8 + kU)) return false;
  *link_type = GetU16(key.data() + 1);
  if (!GetUlid(key.data() + 3, anchor)) return false;

  Slice ts(key.data() + 3 + kU, 8);
  if (!DecodeOrderedInt64(&ts, timestamp)) return false;

  return GetUlid(key.data() + 3 + kU + 8, entity);
}

// --- CANDIDATE PAIRS --------------------------------------------------------

std::string EncodeCandidateKey(double score, uint64_t pair_hash) {
  std::string k;
  PutPrefix(&k, Keyspace::kCandidate);
  // Negate so that a forward scan returns the HIGHEST scores first - the pairs
  // closest to the match threshold, which are the most valuable to review.
  EncodeOrderedDouble(&k, -score);
  PutU64(&k, pair_hash);
  return k;
}

bool DecodeCandidateKey(const Slice& key, double* score, uint64_t* pair_hash) {
  if (!CheckPrefix(key, Keyspace::kCandidate, 1 + 8 + 8)) return false;
  Slice s(key.data() + 1, 8);
  double negated;
  if (!DecodeOrderedDouble(&s, &negated)) return false;
  *score = -negated;
  *pair_hash = GetU64(key.data() + 9);
  return true;
}

std::string CandidatePrefix() {
  std::string k;
  PutPrefix(&k, Keyspace::kCandidate);
  return k;
}

// --- INGEST -----------------------------------------------------------------

std::string EncodeIngestKey(SourceId source, BatchId batch) {
  std::string k;
  k.reserve(1 + 4 + 8);
  PutPrefix(&k, Keyspace::kIngest);
  PutU32(&k, source);
  PutU64(&k, batch);
  return k;
}

bool DecodeIngestKey(const Slice& key, SourceId* source, BatchId* batch) {
  if (!CheckPrefix(key, Keyspace::kIngest, 1 + 4 + 8)) return false;
  *source = GetU32(key.data() + 1);
  *batch = GetU64(key.data() + 5);
  return true;
}

std::string IngestPrefix(SourceId source) {
  std::string k;
  k.reserve(1 + 4);
  PutPrefix(&k, Keyspace::kIngest);
  PutU32(&k, source);
  return k;
}

// --- helpers ----------------------------------------------------------------

std::string PrefixUpperBound(const std::string& prefix) {
  std::string limit = prefix;
  // Increment the last byte that is not 0xFF, dropping any trailing 0xFF.
  // An all-0xFF prefix has no successor, in which case an empty string means
  // "scan to the end".
  while (!limit.empty()) {
    auto& last = limit.back();
    if (static_cast<uint8_t>(last) != 0xFF) {
      last = static_cast<char>(static_cast<uint8_t>(last) + 1);
      return limit;
    }
    limit.pop_back();
  }
  return {};
}

Keyspace KeyspaceOf(const Slice& key) {
  if (key.empty()) return static_cast<Keyspace>(0);
  return static_cast<Keyspace>(static_cast<uint8_t>(key[0]));
}

const char* KeyspaceName(Keyspace ks) {
  switch (ks) {
    case Keyspace::kRaw: return "RAW";
    case Keyspace::kSourceRecord: return "SRCREC";
    case Keyspace::kEntity: return "ENTITY";
    case Keyspace::kLinkOut: return "LINKOUT";
    case Keyspace::kLinkIn: return "LINKIN";
    case Keyspace::kProvenance: return "PROV";
    case Keyspace::kCrossRef: return "XREF";
    case Keyspace::kBlocking: return "BLOCK";
    case Keyspace::kIndex: return "IDX";
    case Keyspace::kTimeIndex: return "TIDX";
    case Keyspace::kCandidate: return "CAND";
    case Keyspace::kIngest: return "INGEST";
  }
  return "UNKNOWN";
}

}  // namespace sextant::codec
