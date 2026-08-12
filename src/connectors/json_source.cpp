#include "json_source.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "env.h"

namespace sextant::connectors {
namespace {

using json = nlohmann::json;

// Render one JSON value as the text a transform chain expects.
//
// The important case is numbers. Digitraffic reports an IMO as the number
// 9074729, and nlohmann's dump() would render an integer-valued double as
// "9074729.0", which validate_imo would then reject for having eight
// characters. So integral values are printed as integers, and only genuinely
// fractional ones go through the floating-point path.
std::string ValueToText(const json& v) {
  if (v.is_null()) return {};
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
  if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
  if (v.is_number_float()) {
    const double d = v.get<double>();
    const auto as_int = static_cast<int64_t>(d);
    if (static_cast<double>(as_int) == d) return std::to_string(as_int);
    return v.dump();
  }
  // Objects and arrays reach here when a mapping addresses a subtree rather
  // than a leaf. Dumping them keeps the value visible instead of dropping it,
  // and the transform chain will reject it if it is not what was meant.
  return v.dump();
}

// Walk a path like `portAreaDetails[0].ata`. Returns nullptr if any step is
// missing, which the caller reports as "column absent" rather than "empty".
const json* Resolve(const json& root, const std::string& path) {
  const json* node = &root;
  size_t i = 0;
  const size_t n = path.size();

  while (i < n) {
    if (path[i] == '.') {
      ++i;
      continue;
    }
    if (path[i] == '[') {
      const size_t close = path.find(']', i);
      if (close == std::string::npos) return nullptr;
      const std::string digits = path.substr(i + 1, close - i - 1);
      if (digits.empty()) return nullptr;
      size_t index = 0;
      for (const char c : digits) {
        if (c < '0' || c > '9') return nullptr;
        index = index * 10 + static_cast<size_t>(c - '0');
      }
      if (!node->is_array() || index >= node->size()) return nullptr;
      node = &(*node)[index];
      i = close + 1;
      continue;
    }
    size_t j = i;
    while (j < n && path[j] != '.' && path[j] != '[') ++j;
    const std::string key = path.substr(i, j - i);
    if (!node->is_object()) return nullptr;
    const auto it = node->find(key);
    if (it == node->end()) return nullptr;
    node = &(*it);
    i = j;
  }
  return node;
}

}  // namespace

// --- SnapshotFetcher --------------------------------------------------------

std::string SnapshotFetcher::PathFor(const std::string& dir,
                                     const std::string& endpoint_id) {
  return dir + "/" + endpoint_id + ".json";
}

Status SnapshotFetcher::Fetch(const std::string& endpoint_id,
                              const std::string& path, std::string* body) {
  (void)path;
  const std::string file = PathFor(dir_, endpoint_id);
  std::unique_ptr<lsm::SequentialFile> f;
  Status s = lsm::SequentialFile::Open(file, &f);
  if (!s.ok()) {
    // Naming the command that would create the file turns a missing snapshot
    // from a dead end into an instruction.
    return Status::NotFound(
        "no recorded response at " + file +
        " - run `sextant fetch --source <id>` to record one, or `data/fetch.sh`");
  }

  body->clear();
  std::string scratch;
  scratch.resize(64 * 1024);
  while (true) {
    lsm::Slice chunk;
    s = f->Read(scratch.size(), &chunk, scratch.data());
    if (!s.ok()) return s;
    if (chunk.empty()) break;
    body->append(chunk.data(), chunk.size());
  }
  return Status::OK();
}

// --- JsonRowSource ----------------------------------------------------------

struct JsonRowSource::Impl {
  json doc;
  const json* records = nullptr;
  size_t index = 0;  // index of the NEXT record to yield

  // Wraps the current element. Rebuilt on each Next() so `current()` stays
  // valid exactly as long as the interface promises.
  class Element : public ontology::Row {
   public:
    void Reset(const json* v) { value_ = v; }

    bool Get(const std::string& path, std::string* out) const override {
      if (value_ == nullptr) return false;
      const json* node = Resolve(*value_, path);
      if (node == nullptr) return false;
      // An explicit JSON null is present-but-empty, which is a different fact
      // from an absent key and is what null_if_blank is there to handle.
      *out = ValueToText(*node);
      return true;
    }

    std::string Raw() const override {
      return value_ == nullptr ? std::string() : value_->dump();
    }

   private:
    const json* value_ = nullptr;
  };

  Element element;
};

JsonRowSource::JsonRowSource() : impl_(std::make_unique<Impl>()) {}
JsonRowSource::~JsonRowSource() = default;

Status JsonRowSource::Open(std::string body, const std::string& records_at,
                           const std::string& endpoint_id,
                           std::unique_ptr<JsonRowSource>* out) {
  std::unique_ptr<JsonRowSource> src(new JsonRowSource());
  src->endpoint_id_ = endpoint_id;

  // Parsing without exceptions: a malformed body is data, not a bug, and a
  // truncated download should produce a diagnosable error rather than unwind
  // the stack out of an ingest loop.
  src->impl_->doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (src->impl_->doc.is_discarded()) {
    return Status::Corruption("response for endpoint \"" + endpoint_id +
                              "\" is not valid JSON");
  }

  const json* node = &src->impl_->doc;
  if (!records_at.empty()) {
    node = Resolve(src->impl_->doc, records_at);
    if (node == nullptr) {
      return Status::Corruption("endpoint \"" + endpoint_id +
                                "\" has no records at \"" + records_at +
                                "\" - the API shape changed");
    }
  }
  if (!node->is_array()) {
    return Status::Corruption("endpoint \"" + endpoint_id +
                              "\" did not yield an array of records");
  }
  src->impl_->records = node;
  *out = std::move(src);
  return Status::OK();
}

bool JsonRowSource::Next() {
  if (impl_->records == nullptr) return false;
  if (impl_->index >= impl_->records->size()) return false;
  impl_->element.Reset(&(*impl_->records)[impl_->index]);
  ++impl_->index;
  ++row_seq_;
  return true;
}

const Row& JsonRowSource::current() const { return impl_->element; }

uint64_t JsonRowSource::size() const {
  return impl_->records == nullptr ? 0
                                   : static_cast<uint64_t>(impl_->records->size());
}

bool JsonPathLookup(const std::string& text, const std::string& path,
                    std::string* out) {
  const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) return false;
  const json* node = Resolve(doc, path);
  if (node == nullptr) return false;
  *out = ValueToText(*node);
  return true;
}

}  // namespace sextant::connectors
