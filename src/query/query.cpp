#include "query.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sextant::query {
namespace {

using ontology::ValueType;

std::string Lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return out;
}

// Compare two values that are not necessarily the same type.
//
// Returns false in `comparable` when the comparison is meaningless, which the
// caller turns into "no match" rather than into an ordering. Silently coercing
// a string to a number here would make `{"imo": {"gte": "abc"}}` return
// everything, which is worse than returning nothing.
int Compare(const TValue& a, const TValue& b, bool* comparable) {
  *comparable = true;

  const bool a_num = a.type() == ValueType::kInt || a.type() == ValueType::kDouble ||
                     a.type() == ValueType::kTimestamp;
  const bool b_num = b.type() == ValueType::kInt || b.type() == ValueType::kDouble ||
                     b.type() == ValueType::kTimestamp;

  if (a_num && b_num) {
    const double x = a.type() == ValueType::kDouble
                         ? a.AsDouble()
                         : static_cast<double>(a.type() == ValueType::kTimestamp
                                                   ? a.AsTimestamp()
                                                   : a.AsInt());
    const double y = b.type() == ValueType::kDouble
                         ? b.AsDouble()
                         : static_cast<double>(b.type() == ValueType::kTimestamp
                                                   ? b.AsTimestamp()
                                                   : b.AsInt());
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
  }

  if (a.type() == ValueType::kString && b.type() == ValueType::kString) {
    return a.AsString().compare(b.AsString());
  }

  if (a.type() == ValueType::kBool && b.type() == ValueType::kBool) {
    return static_cast<int>(a.AsBool()) - static_cast<int>(b.AsBool());
  }

  *comparable = false;
  return 0;
}

Status ParsePredicates(const nlohmann::json& node, std::vector<Predicate>* out);

// A JSON scalar becomes a TValue. Timestamps arrive as ISO 8601 strings and are
// parsed here rather than at the comparison site, so that a malformed date is
// an error on the request instead of a filter that silently matches nothing.
Status ParseValue(const nlohmann::json& node, TValue* out) {
  if (node.is_null()) {
    *out = TValue::Null();
  } else if (node.is_boolean()) {
    *out = TValue::Bool(node.get<bool>());
  } else if (node.is_number_integer()) {
    *out = TValue::Int(node.get<int64_t>());
  } else if (node.is_number_float()) {
    *out = TValue::Double(node.get<double>());
  } else if (node.is_string()) {
    *out = TValue::String(node.get<std::string>());
  } else if (node.is_array()) {
    std::vector<std::string> items;
    for (const auto& item : node) {
      if (!item.is_string()) {
        return Status::InvalidArgument("list values must be strings");
      }
      items.push_back(item.get<std::string>());
    }
    *out = TValue::StringList(std::move(items));
  } else {
    return Status::InvalidArgument("unsupported value in filter");
  }
  return Status::OK();
}

Status ParsePredicates(const nlohmann::json& node, std::vector<Predicate>* out) {
  if (!node.is_object()) {
    return Status::InvalidArgument("filter must be an object");
  }
  for (const auto& [property, spec] : node.items()) {
    if (spec.is_object()) {
      // {"arrived_at": {"gte": "...", "lt": "..."}}
      for (const auto& [op_name, value] : spec.items()) {
        Predicate pred;
        pred.property = property;
        if (!ParseCompareOp(op_name, &pred.op)) {
          return Status::InvalidArgument("unknown operator: " + op_name);
        }
        Status s = ParseValue(value, &pred.value);
        if (!s.ok()) return s;
        out->push_back(std::move(pred));
      }
    } else {
      // {"locode": "NLRTM"} is shorthand for equality.
      Predicate pred;
      pred.property = property;
      pred.op = CompareOp::kEq;
      Status s = ParseValue(spec, &pred.value);
      if (!s.ok()) return s;
      out->push_back(std::move(pred));
    }
  }
  return Status::OK();
}

}  // namespace

const char* CompareOpName(CompareOp op) {
  switch (op) {
    case CompareOp::kEq:
      return "eq";
    case CompareOp::kNe:
      return "ne";
    case CompareOp::kLt:
      return "lt";
    case CompareOp::kLte:
      return "lte";
    case CompareOp::kGt:
      return "gt";
    case CompareOp::kGte:
      return "gte";
    case CompareOp::kContains:
      return "contains";
    case CompareOp::kStartsWith:
      return "starts_with";
  }
  return "?";
}

bool ParseCompareOp(const std::string& name, CompareOp* out) {
  if (name == "eq" || name == "=") *out = CompareOp::kEq;
  else if (name == "ne" || name == "!=") *out = CompareOp::kNe;
  else if (name == "lt" || name == "<") *out = CompareOp::kLt;
  else if (name == "lte" || name == "<=") *out = CompareOp::kLte;
  else if (name == "gt" || name == ">") *out = CompareOp::kGt;
  else if (name == "gte" || name == ">=") *out = CompareOp::kGte;
  else if (name == "contains") *out = CompareOp::kContains;
  else if (name == "starts_with" || name == "prefix") *out = CompareOp::kStartsWith;
  else return false;
  return true;
}

bool Predicate::Matches(const TValue* actual) const {
  // A property the entity does not carry fails every operator, kNe included.
  // "Not equal to X" over a value that does not exist is not something the data
  // supports, and answering true would quietly include every entity that simply
  // never had the field populated.
  if (actual == nullptr || actual->IsNull()) return false;

  // A list matches if any element does. That makes `aliases contains "rott"`
  // behave the way a person expects without a separate operator.
  if (actual->type() == ValueType::kStringList) {
    for (const auto& item : actual->AsStringList()) {
      const TValue element = TValue::String(item);
      if (Matches(&element)) return true;
    }
    return false;
  }

  if (op == CompareOp::kContains || op == CompareOp::kStartsWith) {
    if (actual->type() != ValueType::kString || value.type() != ValueType::kString) {
      return false;
    }
    const std::string haystack = Lower(actual->AsString());
    const std::string needle = Lower(value.AsString());
    if (op == CompareOp::kStartsWith) return haystack.rfind(needle, 0) == 0;
    return haystack.find(needle) != std::string::npos;
  }

  bool comparable = false;
  const int cmp = Compare(*actual, value, &comparable);
  if (!comparable) return false;

  switch (op) {
    case CompareOp::kEq:
      return cmp == 0;
    case CompareOp::kNe:
      return cmp != 0;
    case CompareOp::kLt:
      return cmp < 0;
    case CompareOp::kLte:
      return cmp <= 0;
    case CompareOp::kGt:
      return cmp > 0;
    case CompareOp::kGte:
      return cmp >= 0;
    default:
      return false;
  }
}

std::string Predicate::Describe() const {
  return property + " " + CompareOpName(op) + " " + value.ToDisplay();
}

std::string TimeWindow::Describe() const {
  if (!present) return "(none)";
  return "[" + ontology::FormatIso8601(from_inclusive) + ", " +
         ontology::FormatIso8601(to_exclusive) + ")";
}

std::string Query::Describe() const {
  std::string out = "start " + start.type;
  if (!start.ids.empty()) out += " ids=" + std::to_string(start.ids.size());
  for (const auto& pred : start.filter) out += " [" + pred.Describe() + "]";
  for (const auto& hop : hops) {
    out += " -> " + hop.link;
    if (hop.reverse) out += "(reverse)";
    if (hop.when.present) out += " " + hop.when.Describe();
  }
  out += " limit " + std::to_string(limit);
  return out;
}

Status Query::FromJson(const std::string& text, Query* out) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error& e) {
    return Status::InvalidArgument(std::string("malformed JSON: ") + e.what());
  }
  if (!root.is_object()) return Status::InvalidArgument("request must be an object");

  *out = Query{};

  if (!root.contains("start") || !root["start"].is_object()) {
    return Status::InvalidArgument("request needs a 'start' object");
  }
  const auto& start = root["start"];
  if (!start.contains("type") || !start["type"].is_string()) {
    return Status::InvalidArgument("start needs a 'type'");
  }
  out->start.type = start["type"].get<std::string>();

  if (start.contains("ids")) {
    if (!start["ids"].is_array()) return Status::InvalidArgument("ids must be an array");
    for (const auto& id : start["ids"]) {
      if (!id.is_string()) return Status::InvalidArgument("ids must be strings");
      out->start.ids.push_back(id.get<std::string>());
    }
  }
  if (start.contains("filter")) {
    Status s = ParsePredicates(start["filter"], &out->start.filter);
    if (!s.ok()) return s;
  }

  if (root.contains("hops")) {
    if (!root["hops"].is_array()) return Status::InvalidArgument("hops must be an array");
    for (const auto& node : root["hops"]) {
      if (!node.is_object()) return Status::InvalidArgument("each hop is an object");
      Hop hop;
      if (!node.contains("link") || !node["link"].is_string()) {
        return Status::InvalidArgument("each hop needs a 'link'");
      }
      hop.link = node["link"].get<std::string>();
      if (node.contains("reverse")) hop.reverse = node["reverse"].get<bool>();
      if (node.contains("max_expand")) {
        hop.max_expand = node["max_expand"].get<uint64_t>();
      }

      if (node.contains("where")) {
        std::vector<Predicate> predicates;
        Status s = ParsePredicates(node["where"], &predicates);
        if (!s.ok()) return s;

        // Split the time bounds out of the general filter. The planner needs to
        // SEE a window to choose TIDX; leaving it as an ordinary predicate on a
        // timestamp property would work and would be quietly slow.
        for (auto& pred : predicates) {
          const bool is_lower = pred.op == CompareOp::kGte || pred.op == CompareOp::kGt;
          const bool is_upper = pred.op == CompareOp::kLt || pred.op == CompareOp::kLte;
          if ((is_lower || is_upper) && pred.value.type() == ValueType::kString) {
            int64_t epoch_ms = 0;
            if (ontology::ParseIso8601(pred.value.AsString(), &epoch_ms)) {
              hop.when.present = true;
              if (is_lower) {
                // gt is gte at the next millisecond. Representing both as one
                // inclusive lower bound keeps the key encoding to one case.
                hop.when.from_inclusive =
                    pred.op == CompareOp::kGt ? epoch_ms + 1 : epoch_ms;
              } else {
                hop.when.to_exclusive =
                    pred.op == CompareOp::kLte ? epoch_ms + 1 : epoch_ms;
              }
              continue;
            }
          }
          hop.filter.push_back(std::move(pred));
        }

        // A one-sided window is still a window: the missing end becomes the end
        // of representable time rather than a full scan.
        if (hop.when.present && hop.when.to_exclusive == 0) {
          hop.when.to_exclusive = INT64_MAX;
        }
        if (hop.when.present && hop.when.from_inclusive == 0) {
          hop.when.from_inclusive = INT64_MIN;
        }
      }
      out->hops.push_back(std::move(hop));
    }
  }

  if (root.contains("select")) {
    for (const auto& field : root["select"]) {
      if (!field.is_string()) return Status::InvalidArgument("select entries are strings");
      out->select.push_back(field.get<std::string>());
    }
  }
  if (root.contains("limit")) out->limit = root["limit"].get<uint64_t>();
  if (root.contains("include_path")) {
    out->include_path = root["include_path"].get<bool>();
  }
  if (out->limit == 0 || out->limit > 10000) {
    // A limit of zero is almost always a client bug rather than a request for
    // no rows, and an unbounded one lets a single request pull the database
    // into memory.
    return Status::InvalidArgument("limit must be between 1 and 10000");
  }
  return Status::OK();
}

}  // namespace sextant::query
