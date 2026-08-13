#include "golden.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "csv.h"
#include "hash.h"

namespace sextant::resolve {
namespace {

std::vector<std::string> SplitFields(const std::string& text, char separator) {
  std::vector<std::string> out;
  std::string current;
  for (const char c : text) {
    if (c == separator) {
      out.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  out.push_back(current);
  return out;
}

}  // namespace

bool GoldenSet::ParseRef(const ontology::SchemaBundle& bundle,
                         const std::string& text, RecordRef* out) {
  const size_t colon = text.find(':');
  if (colon == std::string::npos) return false;

  const std::string source_key = text.substr(0, colon);
  const ontology::SourceSpec* spec = bundle.Source(source_key);
  if (spec == nullptr) return false;

  // The natural key hash has to be computed exactly the way the mapper computes
  // it - field by field, length-prefixed - or every lookup misses and the
  // evaluation silently reports zero coverage.
  const std::vector<std::string> fields =
      SplitFields(text.substr(colon + 1), '|');
  out->source = spec->id;
  out->key_hash = codec::Hash64Fields(fields.data(), fields.size());
  return true;
}

std::string GoldenSet::Describe(const RecordRef& ref) const {
  const auto it = text_of_.find(ref);
  if (it != text_of_.end()) return it->second;
  return "<" + std::to_string(ref.source) + ":" + std::to_string(ref.key_hash) + ">";
}

const GoldenPair* GoldenSet::Find(const PairRef& pair) const {
  const auto it = index_.find(pair);
  return it == index_.end() ? nullptr : &pairs_[it->second];
}

Status GoldenSet::LoadFromFile(const std::string& path,
                               const ontology::SchemaBundle& bundle,
                               GoldenSet* out) {
  std::unique_ptr<connectors::CsvReader> reader;
  Status s = connectors::CsvReader::Open(path, &reader);
  if (!s.ok()) {
    return Status::NotFound("no golden set at " + path +
                            " - run `python3 eval/make_corpus.py` to build one");
  }

  GoldenSet set;
  while (reader->Next()) {
    std::string text_a, text_b, label, truth_id;
    if (!reader->current().Get("record_a", &text_a) ||
        !reader->current().Get("record_b", &text_b) ||
        !reader->current().Get("label", &label)) {
      return Status::Corruption(
          path + ": expected columns record_a, record_b, label");
    }
    reader->current().Get("truth_id", &truth_id);

    GoldenPair pair;
    RecordRef a, b;
    // A reference naming a source the schema does not declare is a hard error,
    // not a skipped row. The failure mode it prevents is a renamed source
    // turning the whole evaluation into "zero pairs, therefore no misses,
    // therefore perfect recall".
    if (!ParseRef(bundle, text_a, &a)) {
      return Status::InvalidArgument(path + ": cannot resolve \"" + text_a +
                                     "\" - is its source still in schema/?");
    }
    if (!ParseRef(bundle, text_b, &b)) {
      return Status::InvalidArgument(path + ": cannot resolve \"" + text_b +
                                     "\" - is its source still in schema/?");
    }

    pair.pair = PairRef(a, b);
    pair.is_match = label == "match";
    pair.truth_id = truth_id;
    pair.text_a = text_a;
    pair.text_b = text_b;

    set.text_of_[a] = text_a;
    set.text_of_[b] = text_b;
    if (pair.is_match) ++set.matches_;
    set.index_.emplace(pair.pair, set.pairs_.size());
    set.pairs_.push_back(std::move(pair));
  }
  s = reader->status();
  if (!s.ok()) return s;

  if (set.pairs_.empty()) {
    return Status::Corruption(path + " has no labeled pairs");
  }
  *out = std::move(set);
  return Status::OK();
}

}  // namespace sextant::resolve
