#include "record.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coding.h"

namespace sextant::ontology {
namespace {

namespace lsmc = sextant::lsm;

void PutRef(std::string* dst, const SourceRef& ref) {
  lsmc::PutVarint32(dst, ref.source_id);
  lsmc::PutVarint64(dst, ref.batch_id);
  lsmc::PutVarint64(dst, ref.row_seq);
  lsmc::PutLengthPrefixedSlice(dst, Slice(ref.column));
}

bool GetRef(Slice* in, SourceRef* ref) {
  uint32_t src;
  Slice column;
  if (!lsmc::GetVarint32(in, &src)) return false;
  if (!lsmc::GetVarint64(in, &ref->batch_id)) return false;
  if (!lsmc::GetVarint64(in, &ref->row_seq)) return false;
  if (!lsmc::GetLengthPrefixedSlice(in, &column)) return false;
  ref->source_id = src;
  ref->column = column.ToString();
  return true;
}

// A record format needs a version byte from the first day it is written, not
// from the first day it changes. Adding one later means the old records have
// no way to say what they are.
constexpr uint8_t kRecordFormatVersion = 1;
constexpr uint8_t kManifestFormatVersion = 1;

}  // namespace

const PropertyCell* SourceRecord::Property(PropId id) const {
  for (const auto& p : properties) {
    if (p.prop == id) return &p;
  }
  return nullptr;
}

void SourceRecord::EncodeTo(std::string* dst) const {
  dst->push_back(static_cast<char>(kRecordFormatVersion));
  lsmc::PutVarint32(dst, source_id);
  lsmc::PutVarint64(dst, batch_id);
  lsmc::PutVarint64(dst, row_seq);
  lsmc::PutVarint32(dst, type);
  lsmc::PutLengthPrefixedSlice(dst, Slice(natural_key));
  lsmc::PutFixed64BE(dst, natural_key_hash);

  lsmc::PutVarint32(dst, static_cast<uint32_t>(properties.size()));
  for (const auto& p : properties) {
    lsmc::PutVarint32(dst, p.prop);
    p.value.EncodeTo(dst);
    PutRef(dst, p.origin);
    lsmc::PutVarint32(dst, static_cast<uint32_t>(p.chain.size()));
    for (const TransformId id : p.chain) lsmc::PutVarint32(dst, id);
    lsmc::PutFixed64BE(dst, p.chain_fingerprint);
    lsmc::PutLengthPrefixedSlice(dst, Slice(p.raw_value));
    lsmc::PutLengthPrefixedSlice(dst, Slice(p.error));
  }

  lsmc::PutVarint32(dst, static_cast<uint32_t>(links.size()));
  for (const auto& l : links) {
    lsmc::PutVarint32(dst, l.link_type);
    lsmc::PutVarint32(dst, l.target_type);
    lsmc::PutVarint32(dst, l.match_property);
    lsmc::PutLengthPrefixedSlice(dst, Slice(l.match_value));
    PutRef(dst, l.origin);
  }
}

bool SourceRecord::DecodeFrom(Slice* input, SourceRecord* out) {
  if (input->empty()) return false;
  if (static_cast<uint8_t>((*input)[0]) != kRecordFormatVersion) return false;
  input->remove_prefix(1);

  SourceRecord r;
  uint32_t u32;
  Slice s;
  if (!lsmc::GetVarint32(input, &u32)) return false;
  r.source_id = u32;
  if (!lsmc::GetVarint64(input, &r.batch_id)) return false;
  if (!lsmc::GetVarint64(input, &r.row_seq)) return false;
  if (!lsmc::GetVarint32(input, &u32)) return false;
  r.type = static_cast<TypeId>(u32);
  if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
  r.natural_key = s.ToString();
  if (input->size() < 8) return false;
  r.natural_key_hash = lsmc::DecodeFixed64BE(input->data());
  input->remove_prefix(8);

  uint32_t nprops;
  if (!lsmc::GetVarint32(input, &nprops)) return false;
  r.properties.reserve(nprops);
  for (uint32_t i = 0; i < nprops; ++i) {
    PropertyCell p;
    if (!lsmc::GetVarint32(input, &u32)) return false;
    p.prop = static_cast<PropId>(u32);
    if (!TValue::DecodeFrom(input, &p.value)) return false;
    if (!GetRef(input, &p.origin)) return false;
    uint32_t nchain;
    if (!lsmc::GetVarint32(input, &nchain)) return false;
    p.chain.reserve(nchain);
    for (uint32_t k = 0; k < nchain; ++k) {
      if (!lsmc::GetVarint32(input, &u32)) return false;
      p.chain.push_back(static_cast<TransformId>(u32));
    }
    if (input->size() < 8) return false;
    p.chain_fingerprint = lsmc::DecodeFixed64BE(input->data());
    input->remove_prefix(8);
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    p.raw_value = s.ToString();
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    p.error = s.ToString();
    r.properties.push_back(std::move(p));
  }

  uint32_t nlinks;
  if (!lsmc::GetVarint32(input, &nlinks)) return false;
  r.links.reserve(nlinks);
  for (uint32_t i = 0; i < nlinks; ++i) {
    LinkRef l;
    if (!lsmc::GetVarint32(input, &u32)) return false;
    l.link_type = static_cast<LinkTypeId>(u32);
    if (!lsmc::GetVarint32(input, &u32)) return false;
    l.target_type = static_cast<TypeId>(u32);
    if (!lsmc::GetVarint32(input, &u32)) return false;
    l.match_property = static_cast<PropId>(u32);
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    l.match_value = s.ToString();
    if (!GetRef(input, &l.origin)) return false;
    r.links.push_back(std::move(l));
  }

  *out = std::move(r);
  return true;
}

// --- BatchManifest ----------------------------------------------------------

void BatchManifest::EncodeTo(std::string* dst) const {
  dst->push_back(static_cast<char>(kManifestFormatVersion));
  lsmc::PutVarint32(dst, source_id);
  lsmc::PutVarint64(dst, batch_id);
  lsmc::PutLengthPrefixedSlice(dst, Slice(source_key));
  lsmc::PutLengthPrefixedSlice(dst, Slice(uri));
  lsmc::PutFixed64BE(dst, content_fingerprint);
  lsmc::PutVarint64(dst, static_cast<uint64_t>(started_ms));
  lsmc::PutVarint64(dst, static_cast<uint64_t>(finished_ms));
  lsmc::PutVarint64(dst, rows_read);
  lsmc::PutVarint64(dst, rows_filtered);
  lsmc::PutVarint64(dst, records_written);
  lsmc::PutVarint64(dst, properties_written);
  lsmc::PutVarint64(dst, properties_rejected);
}

bool BatchManifest::DecodeFrom(Slice* input, BatchManifest* out) {
  if (input->empty()) return false;
  if (static_cast<uint8_t>((*input)[0]) != kManifestFormatVersion) return false;
  input->remove_prefix(1);

  BatchManifest m;
  uint32_t u32;
  uint64_t u64;
  Slice s;
  if (!lsmc::GetVarint32(input, &u32)) return false;
  m.source_id = u32;
  if (!lsmc::GetVarint64(input, &m.batch_id)) return false;
  if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
  m.source_key = s.ToString();
  if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
  m.uri = s.ToString();
  if (input->size() < 8) return false;
  m.content_fingerprint = lsmc::DecodeFixed64BE(input->data());
  input->remove_prefix(8);
  if (!lsmc::GetVarint64(input, &u64)) return false;
  m.started_ms = static_cast<int64_t>(u64);
  if (!lsmc::GetVarint64(input, &u64)) return false;
  m.finished_ms = static_cast<int64_t>(u64);
  if (!lsmc::GetVarint64(input, &m.rows_read)) return false;
  if (!lsmc::GetVarint64(input, &m.rows_filtered)) return false;
  if (!lsmc::GetVarint64(input, &m.records_written)) return false;
  if (!lsmc::GetVarint64(input, &m.properties_written)) return false;
  if (!lsmc::GetVarint64(input, &m.properties_rejected)) return false;

  *out = std::move(m);
  return true;
}

}  // namespace sextant::ontology
