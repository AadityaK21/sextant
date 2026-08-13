#include "phonetic.h"

#include <string>
#include <vector>

#include "normalize.h"

namespace sextant::resolve {
namespace {

char UpperAscii(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// The six Soundex consonant classes. 0 means "not coded": vowels plus H, W and
// Y, which are treated differently from each other below.
char SoundexCode(char upper) {
  switch (upper) {
    case 'B': case 'F': case 'P': case 'V':
      return '1';
    case 'C': case 'G': case 'J': case 'K': case 'Q': case 'S': case 'X': case 'Z':
      return '2';
    case 'D': case 'T':
      return '3';
    case 'L':
      return '4';
    case 'M': case 'N':
      return '5';
    case 'R':
      return '6';
    default:
      return '0';
  }
}

bool IsLetter(char c) { return c >= 'A' && c <= 'Z'; }

}  // namespace

std::string Soundex(const std::string& word) {
  // Skip anything that is not a letter, so digits in a name do not shift the
  // encoding. UN/LOCODE has entries like "Port 2".
  size_t start = 0;
  while (start < word.size() && !IsLetter(UpperAscii(word[start]))) ++start;
  if (start >= word.size()) return {};

  std::string out;
  out.push_back(UpperAscii(word[start]));

  char previous = SoundexCode(UpperAscii(word[start]));

  for (size_t i = start + 1; i < word.size() && out.size() < 4; ++i) {
    const char c = UpperAscii(word[i]);
    if (!IsLetter(c)) continue;
    const char code = SoundexCode(c);

    if (code != '0' && code != previous) {
      out.push_back(code);
    }

    // H and W are transparent: two consonants of the same class separated by
    // one of them still count as a single sound, so the previous code is
    // carried forward unchanged. Every other letter - including vowels and Y -
    // breaks the run, which is why "Tymczak" is T522 and not T520.
    if (c != 'H' && c != 'W') {
      previous = code;
    }
  }

  while (out.size() < 4) out.push_back('0');
  return out;
}

std::string SoundexPhrase(const std::string& normalized_name) {
  const std::vector<std::string> tokens = TokenizeUpper(normalized_name);
  std::string out;
  for (const auto& token : tokens) {
    const std::string code = Soundex(token);
    if (code.empty()) continue;
    if (!out.empty()) out.push_back(' ');
    out += code;
  }
  return out;
}

}  // namespace sextant::resolve
