// String similarity measures.
//
// WHY THESE TWO AND NOT AN EDIT DISTANCE
//
// Levenshtein is the obvious choice and it is the wrong one here, for a reason
// worth being able to state. Edit distance treats every position equally, so
// "Rotterdam" against "Rotterdam Botlek" scores badly - seven insertions - even
// though a human reads them as obviously related. And it treats a transposition
// as two edits, when a transposition is the single most common typing error.
//
//   JARO-WINKLER handles both. It counts matching characters within a sliding
//   window rather than requiring alignment, so transpositions cost half an
//   edit, and it boosts pairs sharing a prefix - which is exactly the shape of
//   names that agree on the important part and disagree on a suffix.
//
//   TOKEN JACCARD handles the case Jaro-Winkler is weak at: word order. "Port
//   Klang Westport" and "Westport Port Klang" are the same place written two
//   ways, and character-level measures see two quite different strings.
//
// They fail in different directions, which is why both are features rather
// than one being chosen. Jaro-Winkler is fooled by short strings - any two
// four-letter names score suspiciously high - and Jaccard is fooled by a
// shared common word. The scorer weighs them against each other and against
// the hard identifiers.
//
// Both return 0 to 1, where 1 is identical. Both are ASCII-oriented and expect
// input that has already been through the normalizer.

#pragma once

#include <string>
#include <vector>

namespace sextant::resolve {

// Jaro similarity: matching characters within a window, discounted by
// transpositions.
double Jaro(const std::string& a, const std::string& b);

// Jaro-Winkler: Jaro plus a bonus for a shared prefix, up to four characters,
// scaled by `prefix_weight`. The standard value is 0.1, which is the most a
// four-character prefix can add: 0.4 of the remaining distance to 1.
double JaroWinkler(const std::string& a, const std::string& b,
                   double prefix_weight = 0.1);

// Intersection over union of the token sets. Order-independent by
// construction, which is the point.
double TokenJaccard(const std::vector<std::string>& a,
                    const std::vector<std::string>& b);

// True when every token of the shorter list appears in the longer one -
// "Rotterdam" inside "Rotterdam Botlek". Jaccard gives that pair only 0.5,
// because it counts the extra token against them; containment says the shorter
// name is entirely accounted for, which for port sub-locations is a much
// better signal than the ratio.
bool TokensContained(const std::vector<std::string>& a,
                     const std::vector<std::string>& b);

}  // namespace sextant::resolve
