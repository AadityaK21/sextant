#include "fuse.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "coding.h"

namespace sextant::resolve {
namespace {

namespace onto = sextant::ontology;
namespace lsmc = sextant::lsm;

constexpr uint8_t kProvenanceVersion = 1;
constexpr uint8_t kEntityVersion = 1;

// One candidate value for a property, with everything needed to judge it.
struct Candidate {
  const SourceRecord* record = nullptr;
  const onto::PropertyCell* cell = nullptr;
  double trust = 0.0;
};

RejectedValue MakeRejected(const Candidate& candidate, std::string reason) {
  RejectedValue rejected;
  rejected.source_id = candidate.cell->origin.source_id;
  rejected.batch_id = candidate.cell->origin.batch_id;
  rejected.row_seq = candidate.cell->origin.row_seq;
  rejected.column = candidate.cell->origin.column;
  rejected.value = candidate.cell->value.ToDisplay();
  rejected.reason = std::move(reason);
  return rejected;
}

std::string Trust(double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", value);
  return buf;
}

}  // namespace

const TValue* ResolvedEntity::Property(codec::PropId prop) const {
  for (const auto& [id, value] : properties) {
    if (id == prop) return &value;
  }
  return nullptr;
}

// --- serialization ----------------------------------------------------------

void Provenance::EncodeTo(std::string* dst) const {
  dst->push_back(static_cast<char>(kProvenanceVersion));
  lsmc::PutVarint32(dst, prop);
  lsmc::PutVarint32(dst, origin.source_id);
  lsmc::PutVarint64(dst, origin.batch_id);
  lsmc::PutVarint64(dst, origin.row_seq);
  lsmc::PutLengthPrefixedSlice(dst, Slice(origin.column));

  lsmc::PutVarint32(dst, static_cast<uint32_t>(chain.size()));
  for (const auto id : chain) lsmc::PutVarint32(dst, id);
  lsmc::PutFixed64BE(dst, chain_fingerprint);

  lsmc::PutLengthPrefixedSlice(dst, Slice(raw_value));
  lsmc::PutLengthPrefixedSlice(dst, Slice(emitted_value));
  lsmc::PutVarint32(dst, static_cast<uint32_t>(rule));

  uint64_t bits;
  std::memcpy(&bits, &confidence, sizeof(bits));
  lsmc::PutFixed64BE(dst, bits);

  lsmc::PutVarint32(dst, static_cast<uint32_t>(rejected.size()));
  for (const auto& r : rejected) {
    lsmc::PutVarint32(dst, r.source_id);
    lsmc::PutVarint64(dst, r.batch_id);
    lsmc::PutVarint64(dst, r.row_seq);
    lsmc::PutLengthPrefixedSlice(dst, Slice(r.column));
    lsmc::PutLengthPrefixedSlice(dst, Slice(r.value));
    lsmc::PutLengthPrefixedSlice(dst, Slice(r.reason));
  }

  lsmc::PutVarint64(dst, cluster_size);
  lsmc::PutVarint32(dst, static_cast<uint32_t>(merge_evidence.size()));
  for (const auto& e : merge_evidence) {
    lsmc::PutLengthPrefixedSlice(dst, Slice(e));
  }
}

bool Provenance::DecodeFrom(Slice* input, Provenance* out) {
  if (input->empty()) return false;
  if (static_cast<uint8_t>((*input)[0]) != kProvenanceVersion) return false;
  input->remove_prefix(1);

  Provenance p;
  uint32_t u32;
  Slice s;
  if (!lsmc::GetVarint32(input, &u32)) return false;
  p.prop = static_cast<codec::PropId>(u32);
  if (!lsmc::GetVarint32(input, &u32)) return false;
  p.origin.source_id = u32;
  if (!lsmc::GetVarint64(input, &p.origin.batch_id)) return false;
  if (!lsmc::GetVarint64(input, &p.origin.row_seq)) return false;
  if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
  p.origin.column = s.ToString();

  uint32_t n;
  if (!lsmc::GetVarint32(input, &n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    if (!lsmc::GetVarint32(input, &u32)) return false;
    p.chain.push_back(static_cast<onto::TransformId>(u32));
  }
  if (input->size() < 8) return false;
  p.chain_fingerprint = lsmc::DecodeFixed64BE(input->data());
  input->remove_prefix(8);

  if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
  p.raw_value = s.ToString();
  if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
  p.emitted_value = s.ToString();
  if (!lsmc::GetVarint32(input, &u32)) return false;
  p.rule = static_cast<FusionRule>(u32);

  if (input->size() < 8) return false;
  const uint64_t bits = lsmc::DecodeFixed64BE(input->data());
  input->remove_prefix(8);
  std::memcpy(&p.confidence, &bits, sizeof(p.confidence));

  if (!lsmc::GetVarint32(input, &n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    RejectedValue r;
    if (!lsmc::GetVarint32(input, &u32)) return false;
    r.source_id = u32;
    if (!lsmc::GetVarint64(input, &r.batch_id)) return false;
    if (!lsmc::GetVarint64(input, &r.row_seq)) return false;
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    r.column = s.ToString();
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    r.value = s.ToString();
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    r.reason = s.ToString();
    p.rejected.push_back(std::move(r));
  }

  if (!lsmc::GetVarint64(input, &p.cluster_size)) return false;
  if (!lsmc::GetVarint32(input, &n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
    p.merge_evidence.push_back(s.ToString());
  }

  *out = std::move(p);
  return true;
}

void ResolvedEntity::EncodeTo(std::string* dst) const {
  dst->push_back(static_cast<char>(kEntityVersion));
  lsmc::PutVarint32(dst, type);
  lsmc::PutVarint32(dst, static_cast<uint32_t>(properties.size()));
  for (const auto& [prop, value] : properties) {
    lsmc::PutVarint32(dst, prop);
    value.EncodeTo(dst);
  }
  lsmc::PutVarint32(dst, static_cast<uint32_t>(members.size()));
  for (const auto& member : members) {
    lsmc::PutVarint32(dst, member.source);
    lsmc::PutFixed64BE(dst, member.key_hash);
  }
}

bool ResolvedEntity::DecodeFrom(Slice* input, ResolvedEntity* out) {
  if (input->empty()) return false;
  if (static_cast<uint8_t>((*input)[0]) != kEntityVersion) return false;
  input->remove_prefix(1);

  ResolvedEntity e;
  uint32_t u32;
  if (!lsmc::GetVarint32(input, &u32)) return false;
  e.type = static_cast<codec::TypeId>(u32);

  uint32_t n;
  if (!lsmc::GetVarint32(input, &n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    if (!lsmc::GetVarint32(input, &u32)) return false;
    TValue value;
    if (!TValue::DecodeFrom(input, &value)) return false;
    e.properties.emplace_back(static_cast<codec::PropId>(u32), std::move(value));
  }

  if (!lsmc::GetVarint32(input, &n)) return false;
  for (uint32_t i = 0; i < n; ++i) {
    RecordRef ref;
    if (!lsmc::GetVarint32(input, &u32)) return false;
    ref.source = u32;
    if (input->size() < 8) return false;
    ref.key_hash = lsmc::DecodeFixed64BE(input->data());
    input->remove_prefix(8);
    e.members.push_back(ref);
  }

  *out = std::move(e);
  return true;
}

// --- fusion -----------------------------------------------------------------

Fuser::Fuser(const SchemaBundle* bundle, const ResolverProperties* props)
    : bundle_(bundle), props_(props) {}

ResolvedEntity Fuser::Fuse(const std::vector<const SourceRecord*>& records,
                           const std::vector<RecordRef>& members,
                           const std::vector<std::string>& merge_evidence) const {
  ResolvedEntity entity;
  entity.members = members;
  if (records.empty()) return entity;

  entity.type = records.front()->type;
  entity.id = codec::Ulid::Generate();

  const onto::EntityTypeDef* type = bundle_->ontology().Type(entity.type);
  if (type == nullptr) return entity;

  for (const auto& def : type->properties) {
    // Gather every non-null candidate for this property.
    std::vector<Candidate> candidates;
    for (const SourceRecord* record : records) {
      const onto::PropertyCell* cell = record->Property(def.id);
      if (cell == nullptr || cell->value.IsNull()) continue;
      const onto::SourceSpec* spec = bundle_->Source(record->source_id);
      candidates.push_back(
          Candidate{record, cell, spec == nullptr ? 0.0 : spec->trust});
    }
    if (candidates.empty()) continue;

    Provenance provenance;
    provenance.prop = def.id;
    provenance.rule = def.fuse;
    provenance.cluster_size = records.size();
    provenance.merge_evidence = merge_evidence;

    TValue winner;
    const Candidate* chosen = nullptr;
    std::string why;

    switch (def.fuse) {
      case FusionRule::kUnion: {
        // A list property loses information under any rule that picks one
        // value, which is the whole reason `union` exists. Every source's
        // values are kept, deduplicated, in a stable order.
        std::vector<std::string> merged;
        for (const auto& candidate : candidates) {
          std::vector<std::string> values;
          if (candidate.cell->value.type() == onto::ValueType::kStringList) {
            values = candidate.cell->value.AsStringList();
          } else {
            values.push_back(candidate.cell->value.ToDisplay());
          }
          for (auto& value : values) {
            if (std::find(merged.begin(), merged.end(), value) == merged.end()) {
              merged.push_back(std::move(value));
            }
          }
        }
        std::sort(merged.begin(), merged.end());
        winner = TValue::StringList(std::move(merged));
        chosen = &candidates.front();
        why = "union of " + std::to_string(candidates.size()) + " sources";
        break;
      }

      case FusionRule::kNumericMedian: {
        // The median, not the mean. One source reporting a coordinate in the
        // wrong hemisphere would drag a mean halfway across the world; it moves
        // a median by one position.
        std::vector<std::pair<double, const Candidate*>> values;
        for (const auto& candidate : candidates) {
          if (candidate.cell->value.type() == onto::ValueType::kDouble) {
            values.emplace_back(candidate.cell->value.AsDouble(), &candidate);
          } else if (candidate.cell->value.type() == onto::ValueType::kInt) {
            values.emplace_back(
                static_cast<double>(candidate.cell->value.AsInt()), &candidate);
          }
        }
        if (values.empty()) continue;
        std::sort(values.begin(), values.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        const auto& median = values[values.size() / 2];
        winner = def.type == onto::ValueType::kInt
                     ? TValue::Int(static_cast<int64_t>(median.first))
                     : TValue::Double(median.first);
        chosen = median.second;
        why = "median of " + std::to_string(values.size()) + " values";
        break;
      }

      case FusionRule::kMostFrequent: {
        std::map<std::string, int> counts;
        for (const auto& candidate : candidates) {
          ++counts[candidate.cell->value.ToDisplay()];
        }
        int best = 0;
        std::string best_value;
        for (const auto& [value, count] : counts) {
          if (count > best) {
            best = count;
            best_value = value;
          }
        }
        for (const auto& candidate : candidates) {
          if (candidate.cell->value.ToDisplay() == best_value) {
            chosen = &candidate;
            break;
          }
        }
        why = std::to_string(best) + " of " + std::to_string(candidates.size()) +
              " sources agree";
        break;
      }

      case FusionRule::kMostRecent: {
        // Recency is the batch, then the row within it. A reflagged vessel
        // genuinely changed flag, so the newest observation is the true one -
        // which is the opposite of what most_trusted would conclude.
        for (const auto& candidate : candidates) {
          if (chosen == nullptr ||
              std::make_pair(candidate.cell->origin.batch_id,
                             candidate.cell->origin.row_seq) >
                  std::make_pair(chosen->cell->origin.batch_id,
                                 chosen->cell->origin.row_seq)) {
            chosen = &candidate;
          }
        }
        why = "most recent observation";
        break;
      }

      case FusionRule::kLongest: {
        for (const auto& candidate : candidates) {
          if (chosen == nullptr || candidate.cell->value.ToDisplay().size() >
                                       chosen->cell->value.ToDisplay().size()) {
            chosen = &candidate;
          }
        }
        why = "longest value";
        break;
      }

      case FusionRule::kMostTrusted:
      default: {
        for (const auto& candidate : candidates) {
          if (chosen == nullptr || candidate.trust > chosen->trust) {
            chosen = &candidate;
          }
        }
        why = "highest source trust (" + Trust(chosen->trust) + ")";
        break;
      }
    }

    if (chosen == nullptr) continue;
    if (winner.IsNull()) winner = chosen->cell->value;

    provenance.origin = chosen->cell->origin;
    provenance.chain = chosen->cell->chain;
    provenance.chain_fingerprint = chosen->cell->chain_fingerprint;
    provenance.raw_value = chosen->cell->raw_value;
    provenance.emitted_value = winner.ToDisplay();
    provenance.confidence =
        static_cast<double>(candidates.size() > 1 ? chosen->trust : 1.0);

    // Everything that lost, with the reason. This is what turns "the name is
    // Rotterdam" into an answer rather than an assertion.
    for (const auto& candidate : candidates) {
      if (&candidate == chosen) continue;
      if (candidate.cell->value.ToDisplay() == provenance.emitted_value) continue;
      std::string reason;
      switch (def.fuse) {
        case FusionRule::kMostTrusted:
          reason = "lower source trust (" + Trust(candidate.trust) + " < " +
                   Trust(chosen->trust) + ")";
          break;
        case FusionRule::kNumericMedian:
          reason = "not the median";
          break;
        case FusionRule::kMostRecent:
          reason = "an earlier observation";
          break;
        case FusionRule::kMostFrequent:
          reason = "a minority value";
          break;
        case FusionRule::kLongest:
          reason = "shorter than the chosen value";
          break;
        default:
          reason = "not selected by " + std::string(onto::FusionRuleName(def.fuse));
          break;
      }
      provenance.rejected.push_back(MakeRejected(candidate, std::move(reason)));
    }

    entity.properties.emplace_back(def.id, std::move(winner));
    entity.provenance.push_back(std::move(provenance));
  }

  return entity;
}

// --- writing ----------------------------------------------------------------

Status WriteEntity(codec::Store* store, const SchemaBundle& bundle,
                   const ResolvedEntity& entity, int* operations) {
  const onto::EntityTypeDef* type = bundle.ontology().Type(entity.type);
  if (type == nullptr) {
    return Status::InvalidArgument("entity has an unknown type");
  }

  codec::EntityWriter writer = store->EditEntity(entity.type, entity.id);

  std::string payload;
  entity.EncodeTo(&payload);
  writer.SetPayload(Slice(payload));

  // Which source rows fed this entity, so lineage can go backwards from the
  // entity as well as forwards from a row.
  for (const auto& member : entity.members) {
    writer.AddCrossRef(member.source, member.key_hash);
  }

  for (const auto& [prop, value] : entity.properties) {
    const onto::PropertyDef* def = type->Property(prop);
    if (def == nullptr || !def->indexed || value.IsNull()) continue;
    switch (value.type()) {
      case onto::ValueType::kString:
        writer.AddIndexString(prop, Slice(value.AsString()));
        break;
      case onto::ValueType::kDouble:
        writer.AddIndexDouble(prop, value.AsDouble());
        break;
      case onto::ValueType::kInt:
        writer.AddIndexInt(prop, value.AsInt());
        break;
      case onto::ValueType::kTimestamp:
        writer.AddIndexInt(prop, value.AsTimestamp());
        break;
      default:
        break;  // lists and nulls have no meaningful position in an index
    }
  }

  // Provenance encoded once per property, all in the same batch as the entity
  // it explains. A crash between them would leave a value with no account of
  // where it came from, which is the one state that makes the lineage question
  // unanswerable.
  std::vector<std::string> encoded;
  encoded.reserve(entity.provenance.size());
  for (const auto& provenance : entity.provenance) {
    encoded.emplace_back();
    provenance.EncodeTo(&encoded.back());
    writer.AddProvenance(provenance.prop, 1, Slice(encoded.back()));
  }

  if (operations != nullptr) *operations = writer.operations();
  return writer.Commit();
}

}  // namespace sextant::resolve
