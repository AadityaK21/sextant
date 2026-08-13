// Normalization: the first stage of entity resolution.
//
// The job is to collapse the ways four sources write the same real-world name
// into one canonical form, so that later stages compare meaning rather than
// formatting. In this dataset the same port arrives as:
//
//     "ROTTERDAM"              World Port Index, all caps
//     "Rotterdam"              UN/LOCODE, NameWoDiacritics
//     "Port of Rotterdam"      a source that writes the full title
//     "Rotterdam, Haven van"   inverted, with punctuation
//     "Göteborg" / "Goteborg"  the same place, one with a diacritic
//
// All five must normalise to the same string, and none of them may collide with
// a genuinely different port.
//
// THE TENSION THAT DECIDES EVERY RULE HERE
//
// Normalization is lossy on purpose, and every token it throws away buys recall
// at the cost of precision. Dropping "PORT" and "OF" merges "Port of Spain"
// with... "Spain", which is wrong. Keeping them leaves "Port of Rotterdam" and
// "Rotterdam" as strangers, which is also wrong.
//
// The resolution is that normalization does NOT decide identity. It produces a
// blocking key - a cheap, deliberately over-generous grouping - and the scorer
// then looks at the full evidence: codes, coordinates, country, the unnormalised
// names. So the rule is: **when in doubt, over-merge here**. A false candidate
// pair costs one comparison. A missed pair is invisible and permanent.
//
// This is why the noise list is short and conservative rather than exhaustive,
// and why `NormalizedName` keeps the token list around: the scorer wants
// "Rotterdam Botlek" and "Rotterdam" to block together AND to be told that one
// has an extra token the other does not.

#pragma once

#include <string>
#include <vector>

namespace sextant::resolve {

// The canonical form plus the pieces that produced it. The scorer uses the
// tokens directly for a Jaccard overlap, so they are computed once here rather
// than re-split downstream.
struct NormalizedName {
  std::string canonical;               // "ROTTERDAM BOTLEK"
  std::vector<std::string> tokens;     // {"ROTTERDAM", "BOTLEK"}
  std::vector<std::string> dropped;    // noise tokens removed, kept as evidence

  bool empty() const { return canonical.empty(); }
};

// Fold diacritics, uppercase, strip punctuation, drop noise tokens, collapse
// whitespace. Parenthesised and comma-separated alternatives are kept as
// tokens: UN/LOCODE writes "Helsinki (Helsingfors)" and both halves are real
// names for the place.
NormalizedName NormalizePortName(const std::string& raw);

// Vessel names carry a different kind of noise: type prefixes (MV, M/V, MT,
// SS), and owners who write the name with a suffix in brackets. IMO and MMSI do
// most of the identification work, so this is deliberately lighter than the
// port normalizer.
NormalizedName NormalizeVesselName(const std::string& raw);

// Exposed for tests and for the scorer's own tokenisation.
std::vector<std::string> TokenizeUpper(const std::string& folded);

// True if the token is one this project treats as carrying no identifying
// information for a port. Public so a test can enumerate the list rather than
// trusting a comment.
bool IsPortNoiseToken(const std::string& upper_token);
bool IsVesselNoiseToken(const std::string& upper_token);

}  // namespace sextant::resolve
