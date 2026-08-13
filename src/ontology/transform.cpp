#include "transform.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hash.h"
#include "value.h"

namespace sextant::ontology {
namespace {

// --- small helpers ----------------------------------------------------------

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ASCII only, and deliberately so. Case folding outside ASCII is
// locale-dependent and locale is hidden state, which the purity rule forbids.
// Non-ASCII text reaches these only after strip_diacritics has folded it, and
// anything still non-ASCII is left alone rather than mangled.
char UpperAscii(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}
char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string TrimStr(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && IsSpace(s[b])) ++b;
  while (e > b && IsSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string CollapseWsStr(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool in_space = false;
  for (const char c : s) {
    if (IsSpace(c)) {
      in_space = true;
    } else {
      if (in_space && !out.empty()) out.push_back(' ');
      in_space = false;
      out.push_back(c);
    }
  }
  return out;
}

// Apply a string->string function elementwise over a list, or once over a
// scalar. Mapping files routinely put [trim, upper] in front of a property
// whose ontology type is string[]; without this the chain would have to be
// written twice.
template <typename Fn>
TValue MapStrings(const TValue& in, Fn fn) {
  if (in.type() == ValueType::kString) return TValue::String(fn(in.AsString()));
  if (in.type() == ValueType::kStringList) {
    std::vector<std::string> out;
    out.reserve(in.AsStringList().size());
    for (const auto& s : in.AsStringList()) out.push_back(fn(s));
    return TValue::StringList(std::move(out));
  }
  return in;
}

// Text form of any value, used by transforms that need to work on whatever the
// connector produced. JSON numbers arrive as text already; this covers the case
// where an earlier transform in the chain produced a typed value.
std::string AsText(const TValue& v) { return v.ToDisplay(); }

// --- diacritic folding ------------------------------------------------------
//
// Covers U+00C0 to U+017F: Latin-1 Supplement plus Latin Extended-A. That is
// every accented character that appears in UN/LOCODE place names for Europe,
// which is where the diacritics in this dataset actually live. A codepoint
// outside the range is passed through unchanged rather than dropped, so a name
// in a script this table does not cover survives intact instead of becoming
// empty.
//
// nullptr means "leave this codepoint alone" and is used for the two symbols
// that sit inside the letter range (multiplication and division signs).
constexpr const char* kFold[] = {
    // U+00C0
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    // U+00D0
    "D", "N", "O", "O", "O", "O", "O", nullptr, "O", "U", "U", "U", "U", "Y", "TH", "ss",
    // U+00E0
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    // U+00F0
    "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", "th", "y",
    // U+0100
    "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d",
    // U+0110
    "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g",
    // U+0120
    "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i",
    // U+0130
    "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L",
    // U+0140
    "l", "L", "l", "N", "n", "N", "n", "N", "n", "n", "N", "n", "O", "o", "O", "o",
    // U+0150
    "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",
    // U+0160
    "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u",
    // U+0170
    "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s",
};
constexpr uint32_t kFoldFirst = 0x00C0;
constexpr uint32_t kFoldCount = sizeof(kFold) / sizeof(kFold[0]);

// Minimal UTF-8 decoder. Returns the codepoint and advances i, or returns -1
// for an invalid sequence, in which case the caller copies the byte through.
int32_t NextCodepoint(const std::string& s, size_t* i) {
  const auto b0 = static_cast<uint8_t>(s[*i]);
  auto cont = [&](size_t k) {
    return *i + k < s.size() &&
           (static_cast<uint8_t>(s[*i + k]) & 0xC0) == 0x80;
  };
  if (b0 < 0x80) {
    *i += 1;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0 && cont(1)) {
    const int32_t cp = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(s[*i + 1]) & 0x3F);
    *i += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    const int32_t cp = ((b0 & 0x0F) << 12) |
                       ((static_cast<uint8_t>(s[*i + 1]) & 0x3F) << 6) |
                       (static_cast<uint8_t>(s[*i + 2]) & 0x3F);
    *i += 3;
    return cp;
  }
  if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    const int32_t cp = ((b0 & 0x07) << 18) |
                       ((static_cast<uint8_t>(s[*i + 1]) & 0x3F) << 12) |
                       ((static_cast<uint8_t>(s[*i + 2]) & 0x3F) << 6) |
                       (static_cast<uint8_t>(s[*i + 3]) & 0x3F);
    *i += 4;
    return cp;
  }
  *i += 1;
  return -1;
}

void AppendUtf8(std::string* out, int32_t cp) {
  if (cp < 0) return;
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string StripDiacriticsStr(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const size_t start = i;
    const int32_t cp = NextCodepoint(s, &i);
    if (cp < 0) {
      out.append(s, start, i - start);  // invalid byte: pass it through
      continue;
    }
    const auto u = static_cast<uint32_t>(cp);
    if (u >= kFoldFirst && u < kFoldFirst + kFoldCount) {
      const char* rep = kFold[u - kFoldFirst];
      if (rep != nullptr) {
        out.append(rep);
        continue;
      }
    }
    AppendUtf8(&out, cp);
  }
  return out;
}

// --- title case -------------------------------------------------------------

// Lowercased inside a name but capitalised when first. This is a display
// nicety, not a matching rule: entity resolution normalises to uppercase
// before it compares anything, so nothing here can change which records match.
// It exists because "Rio De Janeiro" looks wrong on a screen and "Rio de
// Janeiro" does not.
bool IsMinorWord(const std::string& w) {
  static const char* kMinor[] = {"of", "the", "and", "de",  "du",  "da", "di",
                                 "del", "la", "le",  "les", "van", "von",
                                 "der", "den", "el",  "al",  "sur", "sous"};
  for (const char* m : kMinor) {
    if (w == m) return true;
  }
  return false;
}

std::string TitleCaseStr(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool at_boundary = true;
  bool seen_word = false;
  size_t word_start = 0;

  auto flush_minor = [&]() {
    // Lowercase the word just written if it is a minor word and not the first.
    if (!seen_word) return;
    const std::string w = out.substr(word_start);
    std::string lowered;
    lowered.reserve(w.size());
    for (const char c : w) lowered.push_back(LowerAscii(c));
    if (IsMinorWord(lowered) && word_start != 0) {
      out.replace(word_start, w.size(), lowered);
    }
  };

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    // Apostrophes and hyphens start a new word - "O'Brien", "Saint-Nazaire" -
    // but a word starting after one of them is never treated as minor, because
    // "Saint-de" is not a thing and the check would only misfire.
    const bool boundary_char = IsSpace(c) || c == '-' || c == '/' || c == '(' ||
                               c == '.' || c == ',' || c == '\'';
    if (boundary_char) {
      if (IsSpace(c)) flush_minor();
      out.push_back(c);
      at_boundary = true;
      continue;
    }
    if (at_boundary) {
      flush_minor();
      word_start = out.size();
      seen_word = true;
      out.push_back(UpperAscii(c));
      at_boundary = false;
    } else {
      out.push_back(LowerAscii(c));
    }
  }
  flush_minor();
  return out;
}

// --- numeric parsing --------------------------------------------------------

// strtod and strtoll are locale-sensitive for the decimal separator. The
// project never calls setlocale, so the C locale is in force and '.' is the
// separator everywhere. This is the reason the transform rule bans hidden
// state: a stray setlocale in some future main() would silently change what
// every stored coordinate decodes to.
bool ParseInt64(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str()) return false;
  while (*end != '\0' && IsSpace(*end)) ++end;
  if (*end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool ParseDouble(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  while (*end != '\0' && IsSpace(*end)) ++end;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

// --- degree-minute coordinates ----------------------------------------------
//
// UN/LOCODE writes coordinates as "5155N 00430E": degrees and minutes with no
// separator and a hemisphere letter, latitude first. Minutes are whole, so the
// resolution is about 1.8 km - fine for blocking a port to a geographic cell,
// nowhere near enough to distinguish two berths, which is worth knowing before
// you weight a distance feature in the scorer.
bool ParseDdmm(const std::string& text, bool want_lat, double* out) {
  std::string a, b;
  size_t i = 0;
  while (i < text.size() && IsSpace(text[i])) ++i;
  while (i < text.size() && !IsSpace(text[i])) a.push_back(text[i++]);
  while (i < text.size() && IsSpace(text[i])) ++i;
  while (i < text.size() && !IsSpace(text[i])) b.push_back(text[i++]);
  while (i < text.size() && IsSpace(text[i])) ++i;
  if (i != text.size()) return false;

  const std::string& token = want_lat ? a : b;
  const size_t digits = want_lat ? 4 : 5;
  if (token.size() != digits + 1) return false;
  for (size_t k = 0; k < digits; ++k) {
    if (token[k] < '0' || token[k] > '9') return false;
  }
  const char hemi = UpperAscii(token[digits]);
  if (want_lat) {
    if (hemi != 'N' && hemi != 'S') return false;
  } else {
    if (hemi != 'E' && hemi != 'W') return false;
  }

  const int deg = std::stoi(token.substr(0, digits - 2));
  const int min = std::stoi(token.substr(digits - 2, 2));
  if (min > 59) return false;
  double v = deg + min / 60.0;
  if (hemi == 'S' || hemi == 'W') v = -v;
  if (want_lat && (v < -90.0 || v > 90.0)) return false;
  if (!want_lat && (v < -180.0 || v > 180.0)) return false;
  *out = v;
  return true;
}

// --- the transforms ---------------------------------------------------------

TValue TrimFn(const TValue& in, TransformContext&) {
  return MapStrings(in, TrimStr);
}

TValue CollapseWsFn(const TValue& in, TransformContext&) {
  return MapStrings(in, CollapseWsStr);
}

TValue UpperFn(const TValue& in, TransformContext&) {
  return MapStrings(in, [](const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (const char c : s) o.push_back(UpperAscii(c));
    return o;
  });
}

TValue LowerFn(const TValue& in, TransformContext&) {
  return MapStrings(in, [](const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (const char c : s) o.push_back(LowerAscii(c));
    return o;
  });
}

TValue TitleCaseFn(const TValue& in, TransformContext&) {
  return MapStrings(in, TitleCaseStr);
}

TValue StripDiacriticsFn(const TValue& in, TransformContext&) {
  return MapStrings(in, StripDiacriticsStr);
}

TValue FirstCharFn(const TValue& in, TransformContext& ctx) {
  const std::string s = AsText(in);
  if (s.empty()) return TValue::Null();
  if (static_cast<uint8_t>(s[0]) >= 0x80) {
    ctx.Fail("first_char on a multi-byte character would split a codepoint");
    return TValue::Null();
  }
  return TValue::String(std::string(1, UpperAscii(s[0])));
}

TValue NullIfBlankFn(const TValue& in, TransformContext&) {
  if (in.type() != ValueType::kString) return in;
  const std::string t = TrimStr(in.AsString());
  if (t.empty()) return TValue::Null();
  return in;
}

TValue ConcatFn(const TValue& in, TransformContext&) {
  if (in.type() != ValueType::kStringList) return in;
  std::string out;
  for (const auto& s : in.AsStringList()) out += s;
  return TValue::String(std::move(out));
}

TValue SplitSemicolonFn(const TValue& in, TransformContext&) {
  if (in.type() != ValueType::kString) return in;
  std::vector<std::string> parts;
  std::string cur;
  for (const char c : in.AsString()) {
    if (c == ';') {
      const std::string t = TrimStr(cur);
      if (!t.empty()) parts.push_back(t);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  const std::string t = TrimStr(cur);
  if (!t.empty()) parts.push_back(t);
  if (parts.empty()) return TValue::Null();
  return TValue::StringList(std::move(parts));
}

TValue ToStringFn(const TValue& in, TransformContext&) {
  if (in.type() == ValueType::kString || in.type() == ValueType::kStringList) {
    return in;
  }
  return TValue::String(AsText(in));
}

TValue ToIntFn(const TValue& in, TransformContext& ctx) {
  if (in.type() == ValueType::kInt) return in;
  const std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  int64_t v;
  if (!ParseInt64(s, &v)) {
    ctx.Fail("not an integer: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::Int(v);
}

TValue ToDoubleFn(const TValue& in, TransformContext& ctx) {
  if (in.type() == ValueType::kDouble) return in;
  const std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  double v;
  if (!ParseDouble(s, &v)) {
    ctx.Fail("not a number: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::Double(v);
}

TValue ParseTimestampFn(const TValue& in, TransformContext& ctx) {
  if (in.type() == ValueType::kTimestamp) return in;
  const std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  int64_t ms;
  if (!ParseIso8601(s, &ms)) {
    ctx.Fail("not an ISO 8601 timestamp: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::Timestamp(ms);
}

TValue ValidateLocodeFn(const TValue& in, TransformContext& ctx) {
  std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  // Some sources separate the country and location parts with a space. Removing
  // internal whitespace before validating means those rows keep their code
  // rather than being rejected over formatting.
  std::string c;
  for (const char ch : s) {
    if (!IsSpace(ch)) c.push_back(UpperAscii(ch));
  }
  bool ok = c.size() == 5;
  for (size_t i = 0; ok && i < 2; ++i) {
    ok = c[i] >= 'A' && c[i] <= 'Z';
  }
  // The location part uses A-Z and 2-9. Digits 0 and 1 are excluded by the
  // standard precisely because they are confusable with O and I, which is the
  // same reason ULIDs use Crockford base32.
  for (size_t i = 2; ok && i < 5; ++i) {
    ok = (c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= '2' && c[i] <= '9');
  }
  if (!ok) {
    ctx.Fail("not a UN/LOCODE: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::String(std::move(c));
}

TValue StripImoPrefixFn(const TValue& in, TransformContext&) {
  if (in.type() != ValueType::kString) return in;
  std::string s = TrimStr(in.AsString());
  if (s.size() > 3) {
    const std::string head = {UpperAscii(s[0]), UpperAscii(s[1]), UpperAscii(s[2])};
    if (head == "IMO") {
      s = TrimStr(s.substr(3));
    }
  }
  return TValue::String(std::move(s));
}

TValue ValidateImoFn(const TValue& in, TransformContext& ctx) {
  const std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  if (!ImoChecksumValid(s)) {
    ctx.Fail("failed IMO check digit: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::String(s);
}

TValue MidToCountryFn(const TValue& in, TransformContext&) {
  const std::string s = TrimStr(AsText(in));
  if (s.size() < 3) return TValue::Null();
  for (size_t i = 0; i < 3; ++i) {
    if (s[i] < '0' || s[i] > '9') return TValue::Null();
  }
  const std::string code = MidToCountryCode(std::stoi(s.substr(0, 3)));
  // An unassigned MID is not an error. Roughly one AIS transmitter in twenty
  // broadcasts a malformed MMSI, and treating that as a failure would fill the
  // lineage with noise about a field that is merely a weak hint to the
  // resolver.
  if (code.empty()) return TValue::Null();
  return TValue::String(code);
}

TValue NormalizeShipTypeFn(const TValue& in, TransformContext& ctx) {
  if (in.IsNull()) return in;
  int64_t v;
  if (in.type() == ValueType::kInt) {
    v = in.AsInt();
  } else {
    const std::string s = TrimStr(AsText(in));
    if (s.empty()) return TValue::Null();
    if (!ParseInt64(s, &v)) {
      ctx.Fail("not a ship type code: \"" + s + "\"");
      return TValue::Null();
    }
  }
  // ITU-R M.1371 ship-and-cargo type codes occupy 0 to 99. Anything outside
  // that came from a different taxonomy and is dropped with a reason rather
  // than guessed at: inventing a mapping between two vessel-type vocabularies
  // would put fabricated data into the ontology and lineage would faithfully
  // record where the fabrication happened, which is worse than a null.
  if (v < 0 || v > 99) {
    ctx.Fail("ship type " + std::to_string(v) + " is outside the AIS range 0-99");
    return TValue::Null();
  }
  return TValue::Int(v);
}

TValue DdmmLatFn(const TValue& in, TransformContext& ctx) {
  const std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  double v;
  if (!ParseDdmm(s, /*want_lat=*/true, &v)) {
    ctx.Fail("not a DDMM coordinate pair: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::Double(v);
}

TValue DdmmLonFn(const TValue& in, TransformContext& ctx) {
  const std::string s = TrimStr(AsText(in));
  if (s.empty()) return TValue::Null();
  double v;
  if (!ParseDdmm(s, /*want_lat=*/false, &v)) {
    ctx.Fail("not a DDMM coordinate pair: \"" + s + "\"");
    return TValue::Null();
  }
  return TValue::Double(v);
}

}  // namespace

std::string FoldDiacritics(const std::string& text) {
  return StripDiacriticsStr(text);
}

// --- IMO check digit --------------------------------------------------------

bool ImoChecksumValid(const std::string& digits) {
  if (digits.size() != 7) return false;
  for (const char c : digits) {
    if (c < '0' || c > '9') return false;
  }
  // Each of the first six digits is multiplied by its position weight, 7 down
  // to 2, and the last digit of the sum must equal the seventh digit.
  int sum = 0;
  for (int i = 0; i < 6; ++i) {
    sum += (digits[static_cast<size_t>(i)] - '0') * (7 - i);
  }
  return (sum % 10) == (digits[6] - '0');
}

// --- MMSI Maritime Identification Digits ------------------------------------
//
// The first three digits of an MMSI are assigned by the ITU to a flag state.
// Deriving the flag from the MMSI and comparing it with the flag the source
// declares is a free consistency signal for the resolver, and a disagreement is
// a strong hint that two records describing the same name are different hulls.
//
// Ranges follow the ITU allocation: 2xx Europe, 3xx North and Central America
// and the Caribbean, 4xx Asia, 5xx Oceania, 6xx Africa, 7xx South America.
// Several states hold more than one MID, which is why Panama and the United
// States appear repeatedly - both operate large open registries.
std::string MidToCountryCode(int mid) {
  static const std::unordered_map<int, const char*> kMid = {
      // Europe
      {201, "AL"}, {202, "AD"}, {203, "AT"}, {204, "PT"}, {205, "BE"},
      {206, "BY"}, {207, "BG"}, {208, "VA"}, {209, "CY"}, {210, "CY"},
      {211, "DE"}, {212, "CY"}, {213, "GE"}, {214, "MD"}, {215, "MT"},
      {216, "AM"}, {218, "DE"}, {219, "DK"}, {220, "DK"}, {224, "ES"},
      {225, "ES"}, {226, "FR"}, {227, "FR"}, {228, "FR"}, {229, "MT"},
      {230, "FI"}, {231, "FO"}, {232, "GB"}, {233, "GB"}, {234, "GB"},
      {235, "GB"}, {236, "GI"}, {237, "GR"}, {238, "HR"}, {239, "GR"},
      {240, "GR"}, {241, "GR"}, {242, "MA"}, {243, "HU"}, {244, "NL"},
      {245, "NL"}, {246, "NL"}, {247, "IT"}, {248, "MT"}, {249, "MT"},
      {250, "IE"}, {251, "IS"}, {252, "LI"}, {253, "LU"}, {254, "MC"},
      {255, "PT"}, {256, "MT"}, {257, "NO"}, {258, "NO"}, {259, "NO"},
      {261, "PL"}, {262, "ME"}, {263, "PT"}, {264, "RO"}, {265, "SE"},
      {266, "SE"}, {267, "SK"}, {268, "SM"}, {269, "CH"}, {270, "CZ"},
      {271, "TR"}, {272, "UA"}, {273, "RU"}, {274, "MK"}, {275, "LV"},
      {276, "EE"}, {277, "LT"}, {278, "SI"}, {279, "RS"},
      // North and Central America, the Caribbean
      {301, "AI"}, {303, "US"}, {304, "AG"}, {305, "AG"}, {306, "CW"},
      {307, "AW"}, {308, "BS"}, {309, "BS"}, {310, "BM"}, {311, "BS"},
      {312, "BZ"}, {314, "BB"}, {316, "CA"}, {319, "KY"}, {321, "CR"},
      {323, "CU"}, {325, "DM"}, {327, "DO"}, {329, "GP"}, {330, "GD"},
      {331, "GL"}, {332, "GT"}, {334, "HN"}, {336, "HT"}, {338, "US"},
      {339, "JM"}, {341, "KN"}, {343, "LC"}, {345, "MX"}, {347, "MQ"},
      {348, "MS"}, {350, "NI"}, {351, "PA"}, {352, "PA"}, {353, "PA"},
      {354, "PA"}, {355, "PA"}, {356, "PA"}, {357, "PA"}, {358, "PR"},
      {359, "SV"}, {361, "PM"}, {362, "TT"}, {364, "TC"}, {366, "US"},
      {367, "US"}, {368, "US"}, {369, "US"}, {370, "PA"}, {371, "PA"},
      {372, "PA"}, {373, "PA"}, {374, "PA"}, {375, "VC"}, {376, "VC"},
      {377, "VC"}, {378, "VG"}, {379, "VI"},
      // Asia
      {401, "AF"}, {403, "SA"}, {405, "BD"}, {408, "BH"}, {410, "BT"},
      {412, "CN"}, {413, "CN"}, {414, "CN"}, {416, "TW"}, {417, "LK"},
      {419, "IN"}, {422, "IR"}, {423, "AZ"}, {425, "IQ"}, {428, "IL"},
      {431, "JP"}, {432, "JP"}, {434, "TM"}, {436, "KZ"}, {437, "UZ"},
      {438, "JO"}, {440, "KR"}, {441, "KR"}, {443, "PS"}, {445, "KP"},
      {447, "KW"}, {450, "LB"}, {451, "KG"}, {453, "MO"}, {455, "MV"},
      {457, "MN"}, {459, "NP"}, {461, "OM"}, {463, "PK"}, {466, "QA"},
      {468, "SY"}, {470, "AE"}, {471, "AE"}, {472, "TJ"}, {473, "YE"},
      {475, "YE"}, {477, "HK"}, {478, "BA"},
      // Oceania
      {501, "TF"}, {503, "AU"}, {506, "MM"}, {508, "BN"}, {510, "FM"},
      {511, "PW"}, {512, "NZ"}, {514, "KH"}, {515, "KH"}, {516, "CX"},
      {518, "CK"}, {520, "FJ"}, {523, "CC"}, {525, "ID"}, {529, "KI"},
      {531, "LA"}, {533, "MY"}, {536, "MP"}, {538, "MH"}, {540, "NC"},
      {542, "NU"}, {544, "NR"}, {546, "PF"}, {548, "PH"}, {553, "PG"},
      {555, "PN"}, {557, "SB"}, {559, "AS"}, {561, "WS"}, {563, "SG"},
      {564, "SG"}, {565, "SG"}, {566, "SG"}, {567, "TH"}, {570, "TO"},
      {572, "TV"}, {574, "VN"}, {576, "VU"}, {577, "VU"}, {578, "WF"},
      // Africa
      {601, "ZA"}, {603, "AO"}, {605, "DZ"}, {607, "TF"}, {608, "SH"},
      {609, "BI"}, {610, "BJ"}, {611, "BW"}, {612, "CF"}, {613, "CM"},
      {615, "CG"}, {616, "KM"}, {617, "CV"}, {618, "TF"}, {619, "CI"},
      {620, "KM"}, {621, "DJ"}, {622, "EG"}, {624, "ET"}, {625, "ER"},
      {626, "GA"}, {627, "GH"}, {629, "GM"}, {630, "GW"}, {631, "GQ"},
      {632, "GN"}, {633, "BF"}, {634, "KE"}, {635, "TF"}, {636, "LR"},
      {637, "LR"}, {638, "SS"}, {642, "LY"}, {644, "LS"}, {645, "MU"},
      {647, "MG"}, {649, "ML"}, {650, "MZ"}, {654, "MR"}, {655, "MW"},
      {656, "NE"}, {657, "NG"}, {659, "NA"}, {660, "RE"}, {661, "RW"},
      {662, "SD"}, {663, "SN"}, {664, "SC"}, {665, "SH"}, {666, "SO"},
      {667, "SL"}, {668, "ST"}, {669, "SZ"}, {670, "TD"}, {671, "TG"},
      {672, "TN"}, {674, "TZ"}, {675, "UG"}, {676, "CD"}, {677, "TZ"},
      {678, "ZM"}, {679, "ZW"},
      // South America
      {701, "AR"}, {710, "BR"}, {720, "BO"}, {725, "CL"}, {730, "CO"},
      {735, "EC"}, {740, "FK"}, {745, "GF"}, {750, "GY"}, {755, "PY"},
      {760, "PE"}, {765, "SR"}, {770, "UY"}, {775, "VE"},
  };
  const auto it = kMid.find(mid);
  return it == kMid.end() ? std::string() : std::string(it->second);
}

// --- the registry -----------------------------------------------------------

void TransformRegistry::Register(TransformId id, const char* name,
                                 uint16_t version, TransformFn fn,
                                 const char* doc) {
  const size_t index = transforms_.size();
  transforms_.push_back(Transform{id, name, version, fn, doc});
  by_name_[name] = index;
  by_id_[id] = index;
}

TransformRegistry::TransformRegistry() {
  namespace t = transform_id;
  Register(t::kTrim, "trim", 1, TrimFn,
           "strip leading and trailing whitespace");
  Register(t::kCollapseWs, "collapse_ws", 1, CollapseWsFn,
           "collapse internal whitespace runs to one space");
  Register(t::kUpper, "upper", 1, UpperFn, "ASCII uppercase");
  Register(t::kLower, "lower", 1, LowerFn, "ASCII lowercase");
  Register(t::kTitleCase, "title_case", 1, TitleCaseFn,
           "capitalise each word, lowercasing minor words after the first");
  Register(t::kStripDiacritics, "strip_diacritics", 1, StripDiacriticsFn,
           "fold Latin-1 and Latin Extended-A accents to ASCII");
  Register(t::kFirstChar, "first_char", 1, FirstCharFn,
           "keep the first character, uppercased");
  Register(t::kNullIfBlank, "null_if_blank", 1, NullIfBlankFn,
           "an all-whitespace string becomes null");
  Register(t::kConcat, "concat", 1, ConcatFn,
           "join a multi-column value into one string");
  Register(t::kSplitSemicolon, "split_semicolon", 1, SplitSemicolonFn,
           "split on ';' into a string list");

  Register(t::kToString, "to_string", 1, ToStringFn, "render as text");
  Register(t::kToInt, "to_int", 1, ToIntFn, "parse a base-10 integer");
  Register(t::kToDouble, "to_double", 1, ToDoubleFn, "parse a decimal number");
  Register(t::kParseTimestamp, "parse_timestamp", 1, ParseTimestampFn,
           "parse ISO 8601 into epoch milliseconds UTC");

  Register(t::kValidateLocode, "validate_locode", 1, ValidateLocodeFn,
           "check the 5-character UN/LOCODE shape");
  Register(t::kValidateImo, "validate_imo", 1, ValidateImoFn,
           "check the IMO number's check digit");
  Register(t::kStripImoPrefix, "strip_imo_prefix", 1, StripImoPrefixFn,
           "drop a leading \"IMO\"");
  Register(t::kMidToCountry, "mid_to_country", 1, MidToCountryFn,
           "derive the flag state from the MMSI's first three digits");
  Register(t::kNormalizeShipType, "normalize_ship_type", 1, NormalizeShipTypeFn,
           "keep AIS ship-and-cargo type codes 0-99, drop anything else");
  Register(t::kDdmmToDecimalLat, "ddmm_to_decimal_lat", 1, DdmmLatFn,
           "latitude from a \"5155N 00430E\" pair");
  Register(t::kDdmmToDecimalLon, "ddmm_to_decimal_lon", 1, DdmmLonFn,
           "longitude from a \"5155N 00430E\" pair");
}

const Transform* TransformRegistry::ByName(const std::string& name) const {
  const auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : &transforms_[it->second];
}

const Transform* TransformRegistry::ById(TransformId id) const {
  const auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : &transforms_[it->second];
}

TValue TransformRegistry::Apply(const std::vector<TransformId>& chain,
                                const TValue& input, std::string* error) const {
  // Cleared up front rather than only written on failure. A caller that reuses
  // one string across several applications would otherwise see the previous
  // failure attached to a value that succeeded.
  if (error != nullptr) error->clear();

  TValue current = input;
  for (const TransformId id : chain) {
    // A null passes straight through. A transform's job is to shape a value,
    // not to have an opinion about a value that was never there, and running
    // validate_imo over an absent IMO would report a failure for every vessel
    // in the AIS feed that simply does not carry one.
    if (current.IsNull()) return current;

    const Transform* t = ById(id);
    if (t == nullptr) {
      // Only reachable when replaying provenance written by a build that had a
      // transform this one does not. Saying so precisely is the whole point of
      // storing ids.
      if (error != nullptr) {
        *error = "unknown transform id 0x" + std::to_string(id);
      }
      return TValue::Null();
    }

    TransformContext ctx;
    current = t->fn(current, ctx);
    if (ctx.failed) {
      if (error != nullptr) *error = std::string(t->name) + ": " + ctx.error;
      return TValue::Null();
    }
  }
  return current;
}

uint64_t TransformRegistry::ChainFingerprint(
    const std::vector<TransformId>& chain) const {
  std::string buf;
  buf.reserve(chain.size() * 4);
  for (const TransformId id : chain) {
    const Transform* t = ById(id);
    const uint16_t version = t == nullptr ? 0 : t->version;
    buf.push_back(static_cast<char>((id >> 8) & 0xFF));
    buf.push_back(static_cast<char>(id & 0xFF));
    buf.push_back(static_cast<char>((version >> 8) & 0xFF));
    buf.push_back(static_cast<char>(version & 0xFF));
  }
  return codec::Hash64(Slice(buf));
}

bool TransformRegistry::ResolveChain(const std::vector<std::string>& names,
                                     std::vector<TransformId>* out,
                                     std::string* error) const {
  out->clear();
  out->reserve(names.size());
  for (const auto& name : names) {
    const Transform* t = ByName(name);
    if (t == nullptr) {
      if (error != nullptr) *error = "unknown transform \"" + name + "\"";
      return false;
    }
    out->push_back(t->id);
  }
  return true;
}

}  // namespace sextant::ontology
