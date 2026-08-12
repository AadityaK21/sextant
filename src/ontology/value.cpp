#include "value.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "coding.h"
#include "ordered.h"

namespace sextant::ontology {
namespace {

namespace lsmc = sextant::lsm;
namespace codec = sextant::codec;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Read exactly n digits starting at pos, advancing pos. Returns false rather
// than parsing a short field, because "2026-4-1" and "2026-04-01" mean the same
// date but only one of them is ISO 8601 and accepting both quietly hides
// upstream format drift.
bool ReadFixedDigits(const std::string& s, size_t* pos, int n, int64_t* out) {
  if (*pos + static_cast<size_t>(n) > s.size()) return false;
  int64_t v = 0;
  for (int i = 0; i < n; ++i) {
    const char c = s[*pos + static_cast<size_t>(i)];
    if (!IsDigit(c)) return false;
    v = v * 10 + (c - '0');
  }
  *pos += static_cast<size_t>(n);
  *out = v;
  return true;
}

}  // namespace

const char* ValueTypeName(ValueType type) {
  switch (type) {
    case ValueType::kNull: return "null";
    case ValueType::kString: return "string";
    case ValueType::kInt: return "int";
    case ValueType::kDouble: return "double";
    case ValueType::kBool: return "bool";
    case ValueType::kTimestamp: return "timestamp";
    case ValueType::kStringList: return "string[]";
  }
  return "unknown";
}

bool ParseValueType(const std::string& name, ValueType* out) {
  if (name == "string") { *out = ValueType::kString; return true; }
  if (name == "int") { *out = ValueType::kInt; return true; }
  if (name == "double") { *out = ValueType::kDouble; return true; }
  if (name == "bool") { *out = ValueType::kBool; return true; }
  if (name == "timestamp") { *out = ValueType::kTimestamp; return true; }
  if (name == "string[]") { *out = ValueType::kStringList; return true; }
  // An enum is a string with a validation rule attached. Giving it its own
  // storage type would mean the on-disk format changed whenever somebody added
  // a permitted value, which is exactly the kind of coupling the declarative
  // schema exists to avoid.
  if (name == "enum") { *out = ValueType::kString; return true; }
  return false;
}

// --- construction -----------------------------------------------------------

TValue TValue::Null() { return TValue(); }

TValue TValue::String(std::string v) {
  TValue t;
  t.type_ = ValueType::kString;
  t.str_ = std::move(v);
  return t;
}

TValue TValue::Int(int64_t v) {
  TValue t;
  t.type_ = ValueType::kInt;
  t.i_ = v;
  return t;
}

TValue TValue::Double(double v) {
  TValue t;
  t.type_ = ValueType::kDouble;
  t.d_ = v;
  return t;
}

TValue TValue::Bool(bool v) {
  TValue t;
  t.type_ = ValueType::kBool;
  t.i_ = v ? 1 : 0;
  return t;
}

TValue TValue::Timestamp(int64_t epoch_ms) {
  TValue t;
  t.type_ = ValueType::kTimestamp;
  t.i_ = epoch_ms;
  return t;
}

TValue TValue::StringList(std::vector<std::string> v) {
  TValue t;
  t.type_ = ValueType::kStringList;
  t.list_ = std::move(v);
  return t;
}

// --- accessors --------------------------------------------------------------

const std::string& TValue::AsString() const {
  assert(type_ == ValueType::kString);
  return str_;
}

int64_t TValue::AsInt() const {
  assert(type_ == ValueType::kInt);
  return i_;
}

double TValue::AsDouble() const {
  assert(type_ == ValueType::kDouble);
  return d_;
}

bool TValue::AsBool() const {
  assert(type_ == ValueType::kBool);
  return i_ != 0;
}

int64_t TValue::AsTimestamp() const {
  assert(type_ == ValueType::kTimestamp);
  return i_;
}

const std::vector<std::string>& TValue::AsStringList() const {
  assert(type_ == ValueType::kStringList);
  return list_;
}

// --- display ----------------------------------------------------------------

std::string TValue::ToDisplay() const {
  switch (type_) {
    case ValueType::kNull:
      return "";
    case ValueType::kString:
      return str_;
    case ValueType::kInt:
      return std::to_string(i_);
    case ValueType::kBool:
      return i_ != 0 ? "true" : "false";
    case ValueType::kTimestamp:
      return FormatIso8601(i_);
    case ValueType::kDouble: {
      // %.17g round-trips every finite double exactly, which matters because
      // this text is what a lineage panel shows next to the stored value. A
      // shorter format would make a correct round-trip look like a mismatch.
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", d_);
      return buf;
    }
    case ValueType::kStringList: {
      std::string out;
      for (size_t i = 0; i < list_.size(); ++i) {
        if (i != 0) out += "; ";
        out += list_[i];
      }
      return out;
    }
  }
  return {};
}

// --- ordered encoding -------------------------------------------------------

void TValue::EncodeOrdered(std::string* dst) const {
  switch (type_) {
    case ValueType::kString:
      codec::EncodeOrderedString(dst, Slice(str_));
      return;
    case ValueType::kInt:
    case ValueType::kTimestamp:
      codec::EncodeOrderedInt64(dst, i_);
      return;
    case ValueType::kBool:
      codec::EncodeOrderedInt64(dst, i_);
      return;
    case ValueType::kDouble:
      codec::EncodeOrderedDouble(dst, d_);
      return;
    case ValueType::kNull:
    case ValueType::kStringList:
      // Neither has a meaningful position in a total order. Encoding them as
      // the empty string groups them at the head of the index, where a range
      // scan steps over them once, rather than interleaving them with real
      // values where every scan would have to test for them.
      codec::EncodeOrderedString(dst, Slice());
      return;
  }
}

// --- serialization ----------------------------------------------------------

void TValue::EncodeTo(std::string* dst) const {
  dst->push_back(static_cast<char>(type_));
  switch (type_) {
    case ValueType::kNull:
      return;
    case ValueType::kString:
      lsmc::PutLengthPrefixedSlice(dst, Slice(str_));
      return;
    case ValueType::kInt:
    case ValueType::kTimestamp:
    case ValueType::kBool:
      // Zigzag, so a small negative number costs one byte rather than ten.
      // Departure timestamps before 1970 and negative longitudes both hit this.
      lsmc::PutVarint64(dst, (static_cast<uint64_t>(i_) << 1) ^
                                 static_cast<uint64_t>(i_ >> 63));
      return;
    case ValueType::kDouble: {
      // Bit pattern, not text. This is a storage format, and a double that
      // survives a round trip through decimal text is a double that got lucky.
      uint64_t bits;
      std::memcpy(&bits, &d_, sizeof(bits));
      lsmc::PutFixed64BE(dst, bits);
      return;
    }
    case ValueType::kStringList:
      lsmc::PutVarint32(dst, static_cast<uint32_t>(list_.size()));
      for (const auto& s : list_) lsmc::PutLengthPrefixedSlice(dst, Slice(s));
      return;
  }
}

bool TValue::DecodeFrom(Slice* input, TValue* out) {
  if (input->empty()) return false;
  const auto tag = static_cast<uint8_t>((*input)[0]);
  if (tag > static_cast<uint8_t>(ValueType::kStringList)) return false;
  const auto type = static_cast<ValueType>(tag);
  input->remove_prefix(1);

  switch (type) {
    case ValueType::kNull:
      *out = TValue::Null();
      return true;
    case ValueType::kString: {
      Slice s;
      if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
      *out = TValue::String(s.ToString());
      return true;
    }
    case ValueType::kInt:
    case ValueType::kTimestamp:
    case ValueType::kBool: {
      uint64_t zz;
      if (!lsmc::GetVarint64(input, &zz)) return false;
      const int64_t v =
          static_cast<int64_t>((zz >> 1) ^ (~(zz & 1) + 1));
      if (type == ValueType::kInt) *out = TValue::Int(v);
      else if (type == ValueType::kTimestamp) *out = TValue::Timestamp(v);
      else *out = TValue::Bool(v != 0);
      return true;
    }
    case ValueType::kDouble: {
      if (input->size() < 8) return false;
      const uint64_t bits = lsmc::DecodeFixed64BE(input->data());
      input->remove_prefix(8);
      double d;
      std::memcpy(&d, &bits, sizeof(d));
      *out = TValue::Double(d);
      return true;
    }
    case ValueType::kStringList: {
      uint32_t n;
      if (!lsmc::GetVarint32(input, &n)) return false;
      std::vector<std::string> items;
      items.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        Slice s;
        if (!lsmc::GetLengthPrefixedSlice(input, &s)) return false;
        items.push_back(s.ToString());
      }
      *out = TValue::StringList(std::move(items));
      return true;
    }
  }
  return false;
}

bool TValue::operator==(const TValue& other) const {
  if (type_ != other.type_) return false;
  switch (type_) {
    case ValueType::kNull: return true;
    case ValueType::kString: return str_ == other.str_;
    case ValueType::kInt:
    case ValueType::kTimestamp:
    case ValueType::kBool: return i_ == other.i_;
    case ValueType::kDouble:
      // Bitwise, not numeric. Two values are equal here if they serialize
      // identically; NaN == NaN has to hold or the lineage round-trip test
      // would fail on a value that was faithfully preserved.
      return std::memcmp(&d_, &other.d_, sizeof(d_)) == 0;
    case ValueType::kStringList: return list_ == other.list_;
  }
  return false;
}

// --- civil date arithmetic --------------------------------------------------
//
// Howard Hinnant's days_from_civil. The trick is to shift the year so that it
// starts in March, which puts the leap day at the END of the year and removes
// every special case from the month-length arithmetic.

int64_t DaysFromCivil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<uint64_t>(year - era * 400);            // [0, 399]
  const uint64_t doy =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;        // [0, 365]
  const uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t days, int64_t* year, unsigned* month, unsigned* day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<uint64_t>(days - era * 146097);         // [0, 146096]
  const uint64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;           // [0, 399]
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);        // [0, 365]
  const uint64_t mp = (5 * doy + 2) / 153;                             // [0, 11]
  const uint64_t d = doy - (153 * mp + 2) / 5 + 1;                     // [1, 31]
  const uint64_t m = mp + (mp < 10 ? 3 : -9);                          // [1, 12]
  *year = y + (m <= 2);
  *month = static_cast<unsigned>(m);
  *day = static_cast<unsigned>(d);
}

bool ParseIso8601(const std::string& text, int64_t* epoch_ms) {
  size_t p = 0;
  int64_t y = 0, mo = 0, d = 0;
  if (!ReadFixedDigits(text, &p, 4, &y)) return false;
  if (p >= text.size() || text[p] != '-') return false;
  ++p;
  if (!ReadFixedDigits(text, &p, 2, &mo)) return false;
  if (p >= text.size() || text[p] != '-') return false;
  ++p;
  if (!ReadFixedDigits(text, &p, 2, &d)) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;

  int64_t hh = 0, mm = 0, ss = 0, ms = 0;
  if (p < text.size() && (text[p] == 'T' || text[p] == ' ')) {
    ++p;
    if (!ReadFixedDigits(text, &p, 2, &hh)) return false;
    if (p >= text.size() || text[p] != ':') return false;
    ++p;
    if (!ReadFixedDigits(text, &p, 2, &mm)) return false;
    if (p < text.size() && text[p] == ':') {
      ++p;
      if (!ReadFixedDigits(text, &p, 2, &ss)) return false;
    }
    if (p < text.size() && (text[p] == '.' || text[p] == ',')) {
      ++p;
      // Take at most three fractional digits and discard the rest. Digitraffic
      // emits microseconds on some endpoints; truncating rather than rounding
      // keeps the value monotone with the source, which is what an ordering
      // index needs.
      int taken = 0;
      while (p < text.size() && IsDigit(text[p])) {
        if (taken < 3) ms = ms * 10 + (text[p] - '0');
        ++taken;
        ++p;
      }
      for (; taken < 3; ++taken) ms *= 10;
    }
    if (hh > 23 || mm > 59 || ss > 60) return false;  // 60 for a leap second
  }

  int64_t offset_seconds = 0;
  if (p < text.size()) {
    const char z = text[p];
    if (z == 'Z' || z == 'z') {
      ++p;
    } else if (z == '+' || z == '-') {
      ++p;
      int64_t oh = 0, om = 0;
      if (!ReadFixedDigits(text, &p, 2, &oh)) return false;
      if (p < text.size() && text[p] == ':') ++p;
      if (p < text.size() && IsDigit(text[p])) {
        if (!ReadFixedDigits(text, &p, 2, &om)) return false;
      }
      offset_seconds = (oh * 3600 + om * 60) * (z == '-' ? -1 : 1);
    }
  }
  if (p != text.size()) return false;  // trailing junk

  const int64_t days =
      DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  const int64_t secs = days * 86400 + hh * 3600 + mm * 60 + ss - offset_seconds;
  *epoch_ms = secs * 1000 + ms;
  return true;
}

std::string FormatIso8601(int64_t epoch_ms) {
  // Floor division, so timestamps before 1970 do not round towards zero and
  // land on the wrong day.
  int64_t secs = epoch_ms / 1000;
  int64_t ms = epoch_ms % 1000;
  if (ms < 0) {
    ms += 1000;
    --secs;
  }
  int64_t days = secs / 86400;
  int64_t rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  int64_t y;
  unsigned mo, d;
  CivilFromDays(days, &y, &mo, &d);

  char buf[48];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<long long>(y), mo, d,
                static_cast<long long>(rem / 3600),
                static_cast<long long>((rem / 60) % 60),
                static_cast<long long>(rem % 60), static_cast<long long>(ms));
  return buf;
}

}  // namespace sextant::ontology
