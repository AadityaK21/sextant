// TValue - one property value, typed.
//
// This is the currency of the whole layer above storage. A source row arrives
// as text; a transform chain turns it into a TValue; the ontology says what
// type that value is allowed to be; the index encoder turns it back into
// order-preserving bytes.
//
// THREE DESIGN POINTS
//
// 1. NULL IS A TYPE, NOT AN ABSENCE. Real source data is full of blanks, and
//    the difference between "this port has no UN/LOCODE" and "we never looked"
//    matters to the resolver. A transform that fails - a bad IMO checksum, an
//    unparseable coordinate - produces a null with a reason attached, which is
//    recorded in lineage rather than dropped silently.
//
// 2. TIMESTAMP IS A SEPARATE TYPE FROM INT even though both hold an int64.
//    They index identically but display and validate differently, and an
//    ontology that cannot distinguish "arrived at" from "gross tonnage" is not
//    describing anything. The discriminator is therefore stored explicitly
//    rather than inferred from the variant alternative.
//
// 3. THE ENCODING IS SELF-DESCRIBING. SRCREC values are written once and read
//    back by a later stage that has no schema in hand, so the type tag travels
//    with the bytes. Costs one byte per value.
//
// Timestamps are milliseconds since the Unix epoch, UTC, always. Every source
// in this project reports local time with an offset, and normalising at the
// transform boundary is the only way to make a time range query mean one
// thing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sextant/lsm/slice.h"

namespace sextant::ontology {

using lsm::Slice;

enum class ValueType : uint8_t {
  kNull = 0,
  kString = 1,
  kInt = 2,
  kDouble = 3,
  kBool = 4,
  kTimestamp = 5,  // milliseconds since the Unix epoch, UTC
  kStringList = 6,
};

const char* ValueTypeName(ValueType type);

// Parse a type name as it appears in ontology.yaml. "enum" maps to kString:
// the allowed set is a validation rule, not a storage concern.
bool ParseValueType(const std::string& name, ValueType* out);

class TValue {
 public:
  TValue() = default;  // null

  static TValue Null();
  static TValue String(std::string v);
  static TValue Int(int64_t v);
  static TValue Double(double v);
  static TValue Bool(bool v);
  static TValue Timestamp(int64_t epoch_ms);
  static TValue StringList(std::vector<std::string> v);

  ValueType type() const { return type_; }
  bool IsNull() const { return type_ == ValueType::kNull; }

  // Accessors. Calling one whose type does not match is a programming error and
  // is asserted; use type() first, or ToDisplay() if you just want text.
  const std::string& AsString() const;
  int64_t AsInt() const;
  double AsDouble() const;
  bool AsBool() const;
  int64_t AsTimestamp() const;
  const std::vector<std::string>& AsStringList() const;

  // Text suitable for a UI or a log. Timestamps render as ISO 8601 in UTC.
  std::string ToDisplay() const;

  // Append this value to a composite key such that byte order equals logical
  // order within the type. Null and list values are not orderable and are
  // encoded as an empty string, so they cluster at the head of the index
  // rather than being scattered through it.
  void EncodeOrdered(std::string* dst) const;

  // Self-describing serialization for SRCREC and ENTITY payloads.
  void EncodeTo(std::string* dst) const;
  static bool DecodeFrom(Slice* input, TValue* out);

  bool operator==(const TValue& other) const;
  bool operator!=(const TValue& other) const { return !(*this == other); }

 private:
  ValueType type_ = ValueType::kNull;
  std::string str_;                  // kString
  std::vector<std::string> list_;    // kStringList
  int64_t i_ = 0;                    // kInt, kTimestamp, kBool
  double d_ = 0.0;                   // kDouble
};

// --- timestamp helpers ------------------------------------------------------
//
// Deliberately hand-rolled rather than reaching for strptime or timegm: neither
// exists on MSVC, and std::chrono's parsing needs a C++20 standard library that
// GCC only shipped in 13. The civil-date arithmetic below is exact and is the
// same algorithm the standard library uses internally.

// Days since 1970-01-01 for a proleptic Gregorian date. Valid for any year.
int64_t DaysFromCivil(int64_t year, unsigned month, unsigned day);
void CivilFromDays(int64_t days, int64_t* year, unsigned* month, unsigned* day);

// Parse ISO 8601. Accepts a date alone, a date and time, an optional fractional
// second, and a trailing Z or +HH:MM / -HH:MM offset. A timestamp with no zone
// is read as UTC, which is what all three sources here actually emit.
bool ParseIso8601(const std::string& text, int64_t* epoch_ms);
std::string FormatIso8601(int64_t epoch_ms);

}  // namespace sextant::ontology
