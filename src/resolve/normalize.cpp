#include "normalize.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "transform.h"

namespace sextant::resolve {
namespace {

char UpperAscii(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool IsAlnum(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Abbreviations expanded BEFORE the noise list runs, so that "Pt. Rotterdam"
// and "Port of Rotterdam" reach the same place. Saint is the one that actually
// matters in this dataset - "St Petersburg" and "Saint Petersburg" are the same
// city and no amount of string similarity will tell you that.
const std::unordered_map<std::string, std::string>& Expansions() {
  static const std::unordered_map<std::string, std::string> kMap = {
      {"ST", "SAINT"},  {"STE", "SAINTE"}, {"PT", "PORT"},
      {"HBR", "HARBOR"}, {"IS", "ISLAND"},  {"MT", "MOUNT"},
  };
  return kMap;
}

// KEPT SHORT ON PURPOSE. Every entry here buys recall and costs precision, and
// the failure it causes is the invisible kind.
//
// The known hazard: dropping PORT and OF turns "Port of Spain" into "SPAIN".
// That is a real port in Trinidad and this normalizer will happily block it
// with anything else whose name reduces to SPAIN. It is accepted because
// normalization does not decide identity - it only decides who gets compared.
// The scorer sees the unnormalised names, the country code and the coordinates,
// and rejects the pair. A missed pair, by contrast, is never looked at again.
const std::unordered_set<std::string>& PortNoise() {
  static const std::unordered_set<std::string> kNoise = {
      "PORT",   "PORTS",     "OF",        "THE",   "AND",
      "HARBOR", "HARBOUR",   "HAVEN",     "TERMINAL", "TERMINALS",
      "ANCHORAGE", "ROADS",  "ROADSTEAD", "DOCK",  "DOCKS",
      "WHARF",  "QUAY",      "JETTY",
  };
  return kNoise;
}

// Vessel type prefixes. A hull's identity lives in its IMO number; the name is
// a weak signal that changes on sale, so this list stays minimal.
const std::unordered_set<std::string>& VesselNoise() {
  static const std::unordered_set<std::string> kNoise = {
      "MV", "MS", "MT", "SS", "MSV", "MY", "RV", "FV", "TS",
  };
  return kNoise;
}

NormalizedName Build(const std::string& raw,
                     const std::unordered_set<std::string>& noise,
                     bool expand_abbreviations) {
  NormalizedName out;

  // Fold through exactly the same routine `strip_diacritics` uses at ingest.
  // Two different folding implementations would mean records are compared under
  // one set of rules and stored under another.
  std::string folded = ontology::FoldDiacritics(raw);

  // Vessel type prefixes are written both ways - "MV Baltic Trader" and
  // "M/V Baltic Trader" - and the tokenizer would split the second into two
  // single letters that no noise list can match. Dropping the slash first turns
  // one problem into the other, which is already solved.
  if (!expand_abbreviations) {
    std::string without_slashes;
    without_slashes.reserve(folded.size());
    for (const char c : folded) {
      if (c != '/') without_slashes.push_back(c);
    }
    folded = std::move(without_slashes);
  }

  const std::vector<std::string> raw_tokens = TokenizeUpper(folded);

  std::vector<std::string> kept;
  for (const auto& token : raw_tokens) {
    // Abbreviation expansion is a PORT rule and must not run on vessel names.
    // The two vocabularies collide: MT means Mount on a coastline and Motor
    // Tanker on a hull, and expanding it for a vessel turns "MT Baltic Trader"
    // into "Mount Baltic Trader" - a prefix that then survives the noise list
    // because it is no longer spelled like one.
    const auto expansion =
        expand_abbreviations ? Expansions().find(token) : Expansions().end();
    const std::string word =
        expansion == Expansions().end() ? token : expansion->second;
    if (noise.count(word) != 0) {
      out.dropped.push_back(word);
    } else {
      kept.push_back(word);
    }
  }

  // A name made entirely of noise keeps its noise. "Port" on its own is a
  // terrible name, but it is the only name that record has, and returning an
  // empty canonical form would drop the record out of every block - silently
  // excluding it from resolution altogether.
  if (kept.empty()) {
    kept = std::move(out.dropped);
    out.dropped.clear();
  }

  out.tokens = std::move(kept);
  for (size_t i = 0; i < out.tokens.size(); ++i) {
    if (i != 0) out.canonical.push_back(' ');
    out.canonical += out.tokens[i];
  }
  return out;
}

}  // namespace

std::vector<std::string> TokenizeUpper(const std::string& folded) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char c : folded) {
    // Everything that is not alphanumeric is a separator, including the
    // parentheses in "Helsinki (Helsingfors)" and the comma in "Rotterdam,
    // Haven van". Both halves are real names for the place, so both become
    // tokens rather than one being discarded.
    if (IsAlnum(c)) {
      current.push_back(UpperAscii(c));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

bool IsPortNoiseToken(const std::string& upper_token) {
  return PortNoise().count(upper_token) != 0;
}

bool IsVesselNoiseToken(const std::string& upper_token) {
  return VesselNoise().count(upper_token) != 0;
}

NormalizedName NormalizePortName(const std::string& raw) {
  return Build(raw, PortNoise(), /*expand_abbreviations=*/true);
}

NormalizedName NormalizeVesselName(const std::string& raw) {
  return Build(raw, VesselNoise(), /*expand_abbreviations=*/false);
}

}  // namespace sextant::resolve
