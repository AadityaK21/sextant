#include "blocking.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "geohash.h"
#include "golden.h"
#include "hash.h"
#include "normalize.h"
#include "phonetic.h"

namespace sextant::resolve {
namespace {

namespace onto = sextant::ontology;

// Precision 4: a cell about 39 x 19.5 km, and with neighbours a reach of well
// over a hundred. Deliberately generous - see the note in geohash.h about
// UN/LOCODE's whole-minute coordinates and about large port complexes whose
// records legitimately sit 20 km apart.
constexpr int kGeoPrecision = 4;

// How many leading characters of a normalised name form a prefix key. Four is
// short enough to survive a suffix disagreement ("Rotterdam" vs "Rotterdam
// Botlek") and long enough that it is not putting a quarter of the corpus in
// one block.
constexpr size_t kNamePrefix = 4;

const std::string* StringValue(const SourceRecord& record, codec::PropId prop) {
  const onto::PropertyCell* cell = record.Property(prop);
  if (cell == nullptr || cell->value.IsNull()) return nullptr;
  if (cell->value.type() != onto::ValueType::kString) return nullptr;
  return &cell->value.AsString();
}

bool DoubleValue(const SourceRecord& record, codec::PropId prop, double* out) {
  const onto::PropertyCell* cell = record.Property(prop);
  if (cell == nullptr || cell->value.IsNull()) return false;
  if (cell->value.type() != onto::ValueType::kDouble) return false;
  *out = cell->value.AsDouble();
  return true;
}

void Add(std::vector<BlockingKey>* keys, const char* generator,
         std::string value) {
  if (value.empty()) return;
  keys->push_back(BlockingKey{generator, std::move(value)});
}

}  // namespace

Status ResolverProperties::Resolve(const onto::Ontology& ontology,
                                   ResolverProperties* out) {
  const onto::EntityTypeDef* port = ontology.Type("Port");
  const onto::EntityTypeDef* vessel = ontology.Type("Vessel");
  if (port == nullptr || vessel == nullptr) {
    return Status::InvalidArgument(
        "the resolver needs Port and Vessel entity types");
  }

  // Every lookup is checked. A silently missing property would not crash - it
  // would just disable one blocking key, and the only symptom would be a pair
  // completeness number slightly lower than it should be, with nothing pointing
  // at why.
  struct Binding {
    const onto::EntityTypeDef* type;
    const char* name;
    codec::PropId* slot;
  };
  const Binding bindings[] = {
      {port, "name", &out->port_name},
      {port, "locode", &out->port_locode},
      {port, "country", &out->port_country},
      {port, "lat", &out->port_lat},
      {port, "lon", &out->port_lon},
      {port, "alt_names", &out->port_alt_names},
      {vessel, "name", &out->vessel_name},
      {vessel, "imo", &out->vessel_imo},
      {vessel, "mmsi", &out->vessel_mmsi},
      {vessel, "call_sign", &out->vessel_call_sign},
      {vessel, "flag", &out->vessel_flag},
  };
  for (const auto& binding : bindings) {
    const onto::PropertyDef* def = binding.type->Property(binding.name);
    if (def == nullptr) {
      return Status::InvalidArgument(std::string("the resolver needs ") +
                                     binding.type->name + "." + binding.name +
                                     ", which the ontology does not declare");
    }
    *binding.slot = def->id;
  }
  out->port_type = port->id;
  out->vessel_type = vessel->id;
  return Status::OK();
}

// --- key generation ---------------------------------------------------------

std::vector<BlockingKey> PortBlockingKeys(const SourceRecord& record,
                                          const ResolverProperties& props) {
  std::vector<BlockingKey> keys;

  // 1. THE CODE. Nearly free and nearly perfect - but only about three quarters
  //    of World Port Index rows carry one, which is precisely why there are
  //    four more keys.
  if (const std::string* locode = StringValue(record, props.port_locode)) {
    Add(&keys, "locode_exact", *locode);
  }

  const std::string* raw_name = StringValue(record, props.port_name);
  const NormalizedName name =
      raw_name == nullptr ? NormalizedName{} : NormalizePortName(*raw_name);

  if (!name.empty()) {
    // 2. PHONETIC. Catches spelling and transliteration differences that no
    //    prefix or exact key survives.
    Add(&keys, "name_soundex", SoundexPhrase(name.canonical));

    // 3. PREFIX. Cheap, and the one that carries "Rotterdam" to "Rotterdam
    //    Botlek": both start ROTT.
    if (name.canonical.size() >= kNamePrefix) {
      Add(&keys, "name_prefix", name.canonical.substr(0, kNamePrefix));
    } else {
      Add(&keys, "name_prefix", name.canonical);
    }

    // 5. COUNTRY AND NAME. Two ports in different countries almost never share
    //    an identity, so pinning the country makes a short name prefix
    //    discriminating enough to be useful.
    if (const std::string* country = StringValue(record, props.port_country)) {
      Add(&keys, "country_name", *country + "|" + name.canonical);
    }
  }

  // 4. GEOGRAPHY. The key that works when the names disagree entirely - and the
  //    only one that would catch a port recorded under a local name in one
  //    source and an English one in another. Expanded to the eight surrounding
  //    cells, because a cell boundary does not care what is near it.
  double lat = 0.0, lon = 0.0;
  if (DoubleValue(record, props.port_lat, &lat) &&
      DoubleValue(record, props.port_lon, &lon)) {
    const std::string cell = GeohashEncode(lat, lon, kGeoPrecision);
    for (const auto& neighbour : GeohashCellAndNeighbours(cell)) {
      Add(&keys, "geo_cell", neighbour);
    }
  }

  return keys;
}

std::vector<BlockingKey> VesselBlockingKeys(const SourceRecord& record,
                                            const ResolverProperties& props) {
  std::vector<BlockingKey> keys;

  // The IMO is the only identifier here that actually proves anything: it is
  // assigned to the hull and never changes, even through sale, rename and
  // reflagging. When both records carry one, this key alone settles it.
  if (const std::string* imo = StringValue(record, props.vessel_imo)) {
    Add(&keys, "imo_exact", *imo);
  }

  // The MMSI belongs to the radio licence, not the hull, and is REASSIGNED when
  // a vessel reflags. So it is a blocking key - a good reason to compare two
  // records - and emphatically not a decision. The corpus contains reassigned
  // MMSIs precisely so that anything treating this as proof fails visibly.
  if (const std::string* mmsi = StringValue(record, props.vessel_mmsi)) {
    Add(&keys, "mmsi_exact", *mmsi);
  }

  if (const std::string* call_sign = StringValue(record, props.vessel_call_sign)) {
    Add(&keys, "callsign_exact", *call_sign);
  }

  if (const std::string* raw_name = StringValue(record, props.vessel_name)) {
    const NormalizedName name = NormalizeVesselName(*raw_name);
    if (!name.empty()) {
      Add(&keys, "name_soundex", SoundexPhrase(name.canonical));
      Add(&keys, "name_prefix",
          name.canonical.substr(0, std::min(kNamePrefix, name.canonical.size())));
    }
  }

  return keys;
}

std::vector<BlockingKey> BlockingKeysFor(const SourceRecord& record,
                                         const ResolverProperties& props) {
  if (record.type == props.port_type) return PortBlockingKeys(record, props);
  if (record.type == props.vessel_type) return VesselBlockingKeys(record, props);
  return {};
}

// --- indexing ---------------------------------------------------------------

Blocker::Blocker(codec::Store* store, const SchemaBundle* bundle,
                 const ResolverProperties* props)
    : store_(store), bundle_(bundle), props_(props) {}

namespace {

// The BLOCK key is `block_key_hash(8) | src(4) | rec(8)`, so the generator name
// has to travel in the value if the report is to attribute recall per key.
uint64_t BlockHash(const BlockingKey& key) {
  const std::string material = key.generator + "\x1f" + key.value;
  return codec::Hash64(lsm::Slice(material));
}

}  // namespace

Status Blocker::IndexAll(BlockingReport* report) {
  for (const auto& spec : bundle_->sources()) {
    auto it = store_->ScanSourceRecords(spec.id);
    for (; it->Valid(); it->Next()) {
      lsm::Slice value = it->value();
      SourceRecord record;
      if (!SourceRecord::DecodeFrom(&value, &record)) {
        return Status::Corruption("undecodable SRCREC in source " + spec.key);
      }

      const std::vector<BlockingKey> keys = BlockingKeysFor(record, *props_);
      if (keys.empty()) continue;
      ++report->records_indexed;

      for (const auto& key : keys) {
        const Status s = store_->PutBlockingKey(BlockHash(key), record.source_id,
                                                record.natural_key_hash);
        if (!s.ok()) return s;
        ++report->keys_written;
      }
    }
    const Status s = it->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Status Blocker::GenerateCandidates(const Options& options,
                                   std::vector<CandidatePairRef>* pairs,
                                   BlockingReport* report) {
  pairs->clear();

  // Rebuild the blocks in memory rather than re-deriving keys. Scanning the
  // BLOCK keyspace is the point of having written it: this is the step that
  // would run on a machine that did not ingest the data.
  //
  // Blocks arrive contiguously because the keyspace is ordered by block hash,
  // so one sequential pass groups them with no sorting and no random access.
  std::unordered_map<PairRef, std::vector<std::string>, PairRefHash> by_pair;
  std::map<codec::TypeId, std::set<RecordRef>> records_by_type;

  // One pass over SRCREC collects both things this needs:
  //
  //   type_of        which record is which kind, for the possible-pairs
  //                  denominator and to stop a hash collision from pairing a
  //                  Port with a Vessel
  //   generator_of   which key produced a given block hash, so the report can
  //                  attribute recall per key
  //
  // The BLOCK keyspace stores no value, so the generator name has to be
  // recovered by recomputing the keys. That is a deliberate trade: it keeps
  // every BLOCK entry to twenty bytes with no payload, and the recomputation
  // happens once per resolve run rather than anywhere near a hot path.
  std::unordered_map<RecordRef, codec::TypeId, RecordRefHash> type_of;
  std::unordered_map<uint64_t, std::string> generator_of;
  for (const auto& spec : bundle_->sources()) {
    auto it = store_->ScanSourceRecords(spec.id);
    for (; it->Valid(); it->Next()) {
      lsm::Slice value = it->value();
      SourceRecord record;
      if (!SourceRecord::DecodeFrom(&value, &record)) continue;
      const RecordRef ref{record.source_id, record.natural_key_hash};
      type_of[ref] = record.type;
      records_by_type[record.type].insert(ref);
      for (const auto& key : BlockingKeysFor(record, *props_)) {
        generator_of.emplace(BlockHash(key), key.generator);
      }
    }
    const Status s = it->status();
    if (!s.ok()) return s;
  }

  auto it = store_->db()->NewIterator(lsm::ReadOptions{});
  const std::string prefix(1, static_cast<char>(codec::Keyspace::kBlocking));
  it->Seek(lsm::Slice(prefix));

  uint64_t current_hash = 0;
  bool have_block = false;
  std::vector<RecordRef> members;

  auto flush_block = [&]() {
    if (!have_block) return;
    ++report->blocks;
    report->largest_block = std::max<uint64_t>(report->largest_block,
                                               members.size());

    // A block bigger than the limit is not narrowing anything - it is the
    // quadratic problem again, inside one key. Dropping it costs recall
    // visibly, which is far better than costing it invisibly by running out of
    // time.
    if (members.size() > options.max_block_size) {
      ++report->purged_blocks;
      members.clear();
      have_block = false;
      return;
    }

    const auto generator = generator_of.find(current_hash);
    const std::string name =
        generator == generator_of.end() ? std::string("unknown")
                                        : generator->second;

    for (size_t i = 0; i < members.size(); ++i) {
      for (size_t j = i + 1; j < members.size(); ++j) {
        if (options.cross_source_only && members[i].source == members[j].source) {
          continue;
        }
        // Records of different types never resolve to each other, and a hash
        // collision between a Port block and a Vessel block would otherwise
        // manufacture a nonsense pair.
        const auto ta = type_of.find(members[i]);
        const auto tb = type_of.find(members[j]);
        if (ta == type_of.end() || tb == type_of.end()) continue;
        if (ta->second != tb->second) continue;

        auto& via = by_pair[PairRef(members[i], members[j])];
        if (std::find(via.begin(), via.end(), name) == via.end()) {
          via.push_back(name);
        }
      }
    }
    members.clear();
    have_block = false;
  };

  for (; it->Valid(); it->Next()) {
    const lsm::Slice key = it->key();
    if (key.empty() ||
        static_cast<uint8_t>(key[0]) !=
            static_cast<uint8_t>(codec::Keyspace::kBlocking)) {
      break;
    }
    uint64_t block_hash = 0;
    codec::SourceId source = 0;
    uint64_t record = 0;
    if (!codec::DecodeBlockingKey(key, &block_hash, &source, &record)) continue;

    if (!have_block || block_hash != current_hash) {
      flush_block();
      current_hash = block_hash;
      have_block = true;
    }
    members.push_back(RecordRef{source, record});
  }
  flush_block();

  const Status s = it->status();
  if (!s.ok()) return s;

  pairs->reserve(by_pair.size());
  for (auto& [pair, via] : by_pair) {
    std::sort(via.begin(), via.end());
    for (const auto& generator : via) ++report->pairs_by_key[generator];
    pairs->push_back(CandidatePairRef{pair, std::move(via)});
  }
  // Sorted output, so two runs over the same data produce the same file and a
  // diff between them means something.
  std::sort(pairs->begin(), pairs->end(),
            [](const CandidatePairRef& a, const CandidatePairRef& b) {
              return a.pair < b.pair;
            });

  report->candidate_pairs = pairs->size();
  report->possible_pairs = 0;
  for (const auto& [type, refs] : records_by_type) {
    const uint64_t n = refs.size();
    report->possible_pairs += n * (n - 1) / 2;
  }
  report->reduction_ratio =
      report->possible_pairs == 0
          ? 0.0
          : 1.0 - static_cast<double>(report->candidate_pairs) /
                      static_cast<double>(report->possible_pairs);
  return Status::OK();
}

void MeasureAgainstGolden(const std::vector<CandidatePairRef>& pairs,
                          const GoldenSet& golden, size_t max_missed_reported,
                          BlockingReport* report) {
  std::unordered_map<PairRef, const std::vector<std::string>*, PairRefHash>
      via_of;
  for (const auto& candidate : pairs) {
    via_of.emplace(candidate.pair, &candidate.via);
  }

  report->golden_matches = 0;
  report->golden_matches_covered = 0;
  report->matches_by_key.clear();
  report->unique_matches_by_key.clear();
  report->missed.clear();

  for (const auto& labeled : golden.pairs()) {
    if (!labeled.is_match) continue;
    ++report->golden_matches;

    const auto found = via_of.find(labeled.pair);
    if (found == via_of.end()) {
      // A true pair no key produced. This is recall the resolver can never get
      // back, so the pairs themselves are listed rather than only counted -
      // looking at five of them usually says which key to add.
      if (report->missed.size() < max_missed_reported) {
        report->missed.emplace_back(labeled.text_a, labeled.text_b);
      }
      continue;
    }
    ++report->golden_matches_covered;

    const std::vector<std::string>& via = *found->second;
    for (const auto& generator : via) ++report->matches_by_key[generator];
    // A key that is the ONLY one to catch a true pair is a key that cannot be
    // dropped. One that never appears here is paying for itself in candidates
    // and returning nothing.
    if (via.size() == 1) ++report->unique_matches_by_key[via.front()];
  }

  report->pair_completeness =
      report->golden_matches == 0
          ? 0.0
          : static_cast<double>(report->golden_matches_covered) /
                static_cast<double>(report->golden_matches);
}

}  // namespace sextant::resolve
