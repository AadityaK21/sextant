// A phonetic blocking key.
//
// WHY SOUNDEX AND NOT DOUBLE METAPHONE
//
// The plan called for Double Metaphone, and this is Soundex instead. That is a
// deliberate downgrade and worth defending rather than hiding.
//
// Double Metaphone is roughly two thousand lines of hand-written rules covering
// Germanic, Slavic, Romance and Chinese name patterns, with a primary and an
// alternate encoding per word. It is genuinely better - it would encode
// "Gothenburg" and "Goteborg" closer together than Soundex does. It is also not
// something you write from memory correctly, so the honest options were to
// vendor a copy or to use something smaller that can be verified against its
// published test vectors.
//
// What Soundex costs, concretely: it keeps the first letter, so "Gothenburg"
// (G) and "Jotunheim" (J) never share a key even when they sound alike, and it
// collapses aggressively after four characters, so "Rotterdam" and "Rotherham"
// collide. The first is a recall loss, the second a precision loss.
//
// Neither is fatal HERE, because of what this key is for. It is one of five
// blocking keys, and blocking is a recall problem with a cheap failure mode: a
// spurious candidate pair costs one comparison in the scorer, which then looks
// at the code, the country and the coordinates and rejects it. The keys that
// carry most of the recall in this dataset are the exact UN/LOCODE match and
// the geohash cell; the phonetic key exists to catch the residue where a port
// has no code and imprecise coordinates.
//
// The execution plan lists "drop the Double Metaphone blocking key" as cut
// number five for exactly this reason. This is that cut, taken early and with
// a cheaper substitute left in place rather than nothing.
//
// If the blocking report shows pair completeness short of target and the misses
// are name-driven, this is the first thing to upgrade.

#pragma once

#include <string>

namespace sextant::resolve {

// American Soundex: initial letter, then three digits from the consonant
// classes, zero-padded. Returns an empty string for input with no letters.
std::string Soundex(const std::string& word);

// Soundex of each token, joined - so "ROTTERDAM BOTLEK" gives "R363 B432" and
// a multi-word name does not lose everything after the first word.
std::string SoundexPhrase(const std::string& normalized_name);

}  // namespace sextant::resolve
