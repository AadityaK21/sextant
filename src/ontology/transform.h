// The transform registry.
//
// THE RULE THIS FILE EXISTS TO ENFORCE
//
//   Every transform is a pure function with a stable numeric id and a version.
//   Lineage records ids, never names and never code.
//
// That is the difference between lineage that is a comment and lineage that is
// an invariant. Because a chain is a list of ids and every function is pure,
// the round-trip test on day 11 can take a stored provenance record, fetch the
// raw source row it names, re-run the chain, and assert the result still equals
// the stored value. Nothing else in this project proves the lineage is real.
//
// Three consequences follow, and all three are constraints on how you edit
// this file:
//
//   * IDS ARE FOREVER. They are written into provenance records on disk. Never
//     reuse one, never renumber, and never change what an existing id computes.
//   * BEHAVIOUR CHANGES BUMP THE VERSION. Old provenance stays readable and the
//     round-trip check reports "the transform changed since this was written"
//     instead of failing as though the data were corrupt. The chain fingerprint
//     stored alongside is what detects this.
//   * NO HIDDEN STATE. No clocks, no locales, no globals. A transform that
//     reads the current time cannot be replayed, and a chain that cannot be
//     replayed is not lineage.
//
// A transform can also FAIL, which is different from producing null. A blank
// UN/LOCODE column is a null; a UN/LOCODE of "XX" is a failure with a reason,
// and the reason is what the lineage panel shows when a user asks why a port
// has no code.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "value.h"

namespace sextant::ontology {

using TransformId = uint16_t;

// Ids are grouped by kind so a chain is readable in a hex dump:
//   0x01xx  string shaping
//   0x02xx  type conversion
//   0x03xx  maritime domain rules
namespace transform_id {
constexpr TransformId kTrim = 0x0101;
constexpr TransformId kCollapseWs = 0x0102;
constexpr TransformId kUpper = 0x0103;
constexpr TransformId kLower = 0x0104;
constexpr TransformId kTitleCase = 0x0105;
constexpr TransformId kStripDiacritics = 0x0106;
constexpr TransformId kFirstChar = 0x0107;
constexpr TransformId kNullIfBlank = 0x0108;
constexpr TransformId kConcat = 0x0109;
constexpr TransformId kSplitSemicolon = 0x010A;

constexpr TransformId kToString = 0x0201;
constexpr TransformId kToInt = 0x0202;
constexpr TransformId kToDouble = 0x0203;
constexpr TransformId kParseTimestamp = 0x0204;

constexpr TransformId kValidateLocode = 0x0301;
constexpr TransformId kValidateImo = 0x0302;
constexpr TransformId kStripImoPrefix = 0x0303;
constexpr TransformId kMidToCountry = 0x0304;
constexpr TransformId kNormalizeShipType = 0x0305;
constexpr TransformId kDdmmToDecimalLat = 0x0306;
constexpr TransformId kDdmmToDecimalLon = 0x0307;
}  // namespace transform_id

// Carries a failure reason out of a transform. Failure is a first-class
// outcome: the value becomes null and the reason is recorded in lineage.
struct TransformContext {
  bool failed = false;
  std::string error;

  void Fail(std::string why) {
    failed = true;
    error = std::move(why);
  }
};

// A plain function pointer rather than std::function, on purpose. It cannot
// capture, so a transform physically cannot close over mutable state, and the
// purity rule above becomes something the type system helps enforce.
using TransformFn = TValue (*)(const TValue&, TransformContext&);

struct Transform {
  TransformId id;
  const char* name;
  uint16_t version;
  TransformFn fn;
  const char* doc;
};

class TransformRegistry {
 public:
  // Registers every built-in. There is no way to add one at runtime, because a
  // transform that is not in the binary cannot be replayed by a later build.
  TransformRegistry();

  const Transform* ByName(const std::string& name) const;
  const Transform* ById(TransformId id) const;
  const std::vector<Transform>& All() const { return transforms_; }

  // Run a chain left to right. Stops at the first failure, sets *error, and
  // returns null. An input that is already null passes through untouched:
  // "there was nothing here" is not something a transform should turn into an
  // error.
  TValue Apply(const std::vector<TransformId>& chain, const TValue& input,
               std::string* error) const;

  // A hash over the (id, version) pairs of a chain, stored beside the chain in
  // every provenance record. If somebody bumps a transform's version, old
  // records no longer match and the round-trip test can say "this transform
  // changed" rather than reporting the data as broken.
  uint64_t ChainFingerprint(const std::vector<TransformId>& chain) const;

  // Resolve a chain of names from a mapping file. Returns false and names the
  // offending entry if any transform is unknown, which is what turns a typo in
  // YAML into a startup error rather than a silently missing property.
  bool ResolveChain(const std::vector<std::string>& names,
                    std::vector<TransformId>* out, std::string* error) const;

 private:
  void Register(TransformId id, const char* name, uint16_t version,
                TransformFn fn, const char* doc);

  std::vector<Transform> transforms_;
  std::unordered_map<std::string, size_t> by_name_;
  std::unordered_map<TransformId, size_t> by_id_;
};

// Fold Latin-1 and Latin Extended-A accents to ASCII. Exposed because entity
// resolution needs exactly the same folding the ingest transform applies: if
// normalization folded differently from `strip_diacritics`, two records would
// be compared under one set of rules and stored under another.
std::string FoldDiacritics(const std::string& text);

// Exposed for testing: the MMSI Maritime Identification Digit table. Returns an
// empty string for an unassigned MID.
std::string MidToCountryCode(int mid);

// Exposed for testing: the IMO check digit rule.
bool ImoChecksumValid(const std::string& digits);

}  // namespace sextant::ontology
