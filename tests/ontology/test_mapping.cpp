// The mapping engine.
//
// This is where lineage is born, so the tests are mostly about the evidence
// rather than the values: does each property record the column it came from,
// the chain that shaped it, and the raw bytes it started as - and does a
// rejected value survive with its reason attached instead of vanishing.

#include "mapping.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bundle.h"
#include "record.h"
#include "schema.h"
#include "transform.h"

using namespace sextant::ontology;
namespace codec = sextant::codec;

namespace {

// A row backed by a map, so a test can state exactly what the source said.
class FakeRow : public Row {
 public:
  explicit FakeRow(std::map<std::string, std::string> cells)
      : cells_(std::move(cells)) {}

  bool Get(const std::string& path, std::string* out) const override {
    const auto it = cells_.find(path);
    if (it == cells_.end()) return false;
    *out = it->second;
    return true;
  }
  std::string Raw() const override { return "<raw>"; }

 private:
  std::map<std::string, std::string> cells_;
};

constexpr const char* kOntology = R"(
version: 1
namespace: maritime
entity_types:
  Port:
    id: 1
    properties:
      name:        { id: 1, type: string, title: true }
      locode:      { id: 2, type: string, indexed: true }
      alt_names:   { id: 3, type: "string[]", fuse: union }
      lat:         { id: 4, type: double }
      harbor_size: { id: 5, type: enum, values: [V, L, M, S] }
  Voyage:
    id: 2
    properties:
      voyage_ref: { id: 10, type: string, title: true }
      arrived_at: { id: 11, type: timestamp }
link_types:
  arrives_at: { id: 1, from: Voyage, to: Port, time_index: arrived_at }
)";

constexpr const char* kMapping = R"(
source:
  id: testsrc
  source_id: 77
  name: A test source
  connector: csv
  uri: /dev/null
  natural_key: ["PortId"]
  trust: 0.5
  filter:
    - column: "Function"
      matches: "^1"
mappings:
  - target_type: Port
    properties:
      name:        { from: "PortName", transform: [trim, collapse_ws, title_case] }
      locode:      { from: ["Country", "Location"], transform: [concat, upper, validate_locode] }
      alt_names:   { from: "Alternates", transform: [trim, split_semicolon] }
      lat:         { from: "Coordinates", transform: [ddmm_to_decimal_lat] }
      harbor_size: { from: "Size", transform: [trim, upper, first_char] }
)";

class MappingTest : public ::testing::Test {
 protected:
  Ontology ontology_;
  TransformRegistry transforms_;
  SourceSpec spec_;

  void SetUp() override {
    ASSERT_TRUE(Ontology::LoadFromString(kOntology, &ontology_, "<onto>").ok());
    const Status s = SourceSpec::LoadFromString(kMapping, ontology_, transforms_,
                                                &spec_, "<mapping>");
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  std::vector<SourceRecord> Map(std::map<std::string, std::string> cells,
                                codec::RowSeq row = 1) {
    Mapper mapper(&ontology_, &transforms_, &spec_);
    const FakeRow row_obj(std::move(cells));
    std::vector<SourceRecord> out;
    EXPECT_TRUE(mapper.MapRow(row_obj, /*batch=*/9, row, "", &out).ok());
    return out;
  }
};

const PropertyCell* Cell(const SourceRecord& rec, codec::PropId prop) {
  return rec.Property(prop);
}

}  // namespace

TEST_F(MappingTest, LoadsAndResolvesNamesAgainstTheOntology) {
  EXPECT_EQ(77u, spec_.id);
  EXPECT_EQ("testsrc", spec_.key);
  EXPECT_EQ(ConnectorKind::kCsv, spec_.connector);
  ASSERT_EQ(1u, spec_.mappings.size());

  const TypeMapping& m = spec_.mappings.front();
  EXPECT_EQ(1, m.target_type);
  ASSERT_EQ(5u, m.properties.size());

  // Names became numeric ids at load time, which is what lets everything below
  // this layer be schema-agnostic.
  bool found = false;
  for (const auto& p : m.properties) {
    if (p.prop_name != "locode") continue;
    found = true;
    EXPECT_EQ(2, p.prop);
    EXPECT_EQ(std::vector<std::string>({"Country", "Location"}), p.from);
    EXPECT_EQ(3u, p.chain.size());
    EXPECT_NE(0u, p.chain_fingerprint);
  }
  EXPECT_TRUE(found);
}

TEST_F(MappingTest, EveryValueCarriesItsOwnLineage) {
  const auto records = Map({{"PortId", "14370"},
                            {"PortName", "  PORT   OF ROTTERDAM "},
                            {"Country", "nl"},
                            {"Location", "rtm"},
                            {"Alternates", "Europoort; Botlek"},
                            {"Coordinates", "5155N 00429E"},
                            {"Size", "large"},
                            {"Function", "1-3-----"}});
  ASSERT_EQ(1u, records.size());
  const SourceRecord& rec = records.front();

  EXPECT_EQ(77u, rec.source_id);
  EXPECT_EQ(9u, rec.batch_id);
  EXPECT_EQ(1u, rec.row_seq);
  EXPECT_EQ(1, rec.type);
  EXPECT_EQ("14370", rec.natural_key);
  EXPECT_NE(0u, rec.natural_key_hash);

  const PropertyCell* name = Cell(rec, 1);
  ASSERT_NE(nullptr, name);
  EXPECT_EQ("Port of Rotterdam", name->value.AsString());
  // The evidence, recorded at the moment the value was produced.
  EXPECT_EQ("PortName", name->origin.column);
  EXPECT_EQ("  PORT   OF ROTTERDAM ", name->raw_value);
  EXPECT_EQ(77u, name->origin.source_id);
  EXPECT_EQ(9u, name->origin.batch_id);
  EXPECT_EQ(1u, name->origin.row_seq);
  EXPECT_EQ(3u, name->chain.size());
  EXPECT_FALSE(name->rejected());

  // Replaying the recorded chain over the recorded raw value reproduces the
  // stored value. This is the day 11 round-trip in miniature, and it is the
  // only reason to store both.
  std::string error;
  const TValue replayed =
      transforms_.Apply(name->chain, TValue::String(name->raw_value), &error);
  EXPECT_EQ(name->value, replayed) << error;
}

TEST_F(MappingTest, MultipleColumnsRecordAllOfThem) {
  const auto records = Map({{"PortId", "1"},
                            {"Country", "nl"},
                            {"Location", "rtm"},
                            {"Function", "1"}});
  ASSERT_EQ(1u, records.size());

  const PropertyCell* locode = Cell(records.front(), 2);
  ASSERT_NE(nullptr, locode);
  EXPECT_EQ("NLRTM", locode->value.AsString());
  // Both contributing columns are named, so the lineage panel can highlight
  // both cells rather than picking one arbitrarily.
  EXPECT_EQ("Country+Location", locode->origin.column);
  EXPECT_NE(std::string::npos, locode->raw_value.find("nl"));
  EXPECT_NE(std::string::npos, locode->raw_value.find("rtm"));
}

TEST_F(MappingTest, TypeCoercionFollowsTheSchema) {
  const auto records = Map({{"PortId", "1"},
                            {"Alternates", "Just One"},
                            {"Coordinates", "5155N 00429E"},
                            {"Size", "medium"},
                            {"Function", "1"}});
  ASSERT_EQ(1u, records.size());
  const SourceRecord& rec = records.front();

  // A scalar reaching a list-valued property becomes a one-element list, which
  // is how a source with a single alternate-name column feeds a property other
  // sources supply several values for.
  const PropertyCell* alt = Cell(rec, 3);
  ASSERT_NE(nullptr, alt);
  ASSERT_EQ(ValueType::kStringList, alt->value.type());
  EXPECT_EQ(std::vector<std::string>({"Just One"}), alt->value.AsStringList());

  const PropertyCell* lat = Cell(rec, 4);
  ASSERT_NE(nullptr, lat);
  EXPECT_EQ(ValueType::kDouble, lat->value.type());
  EXPECT_NEAR(51.9167, lat->value.AsDouble(), 1e-4);

  const PropertyCell* size = Cell(rec, 5);
  ASSERT_NE(nullptr, size);
  EXPECT_EQ("M", size->value.AsString());
}

// A rejected value keeps its cell. Deleting it would lose the only evidence
// that the source said anything, and "why does this port have no code" is a
// question the lineage panel is supposed to answer.
TEST_F(MappingTest, RejectedValuesSurviveWithTheirReason) {
  const auto records = Map({{"PortId", "1"},
                            {"Country", "zz"},
                            {"Location", "1"},
                            {"Function", "1"}});
  ASSERT_EQ(1u, records.size());

  const PropertyCell* locode = Cell(records.front(), 2);
  ASSERT_NE(nullptr, locode);
  EXPECT_TRUE(locode->value.IsNull());
  EXPECT_TRUE(locode->rejected());
  EXPECT_NE(std::string::npos, locode->error.find("validate_locode"));
  // The raw bytes are still there, so a reviewer can see what was rejected.
  EXPECT_NE(std::string::npos, locode->raw_value.find("zz"));
}

TEST_F(MappingTest, EnumViolationsAreRecordedNotDropped) {
  const auto records =
      Map({{"PortId", "1"}, {"Size", "enormous"}, {"Function", "1"}});
  ASSERT_EQ(1u, records.size());

  const PropertyCell* size = Cell(records.front(), 5);
  ASSERT_NE(nullptr, size);
  EXPECT_TRUE(size->value.IsNull());
  EXPECT_TRUE(size->rejected());
  EXPECT_NE(std::string::npos, size->error.find("permitted values"));
}

TEST_F(MappingTest, AbsentColumnsAndBlankCellsAreDifferent) {
  // A column the source does not have at all produces no cell.
  const auto missing = Map({{"PortId", "1"}, {"Function", "1"}});
  ASSERT_EQ(1u, missing.size());
  EXPECT_EQ(nullptr, Cell(missing.front(), 1));

  // A column that exists but is blank produces a cell with a null value and no
  // error. "The source has no name for this" is a fact worth recording.
  const auto blank = Map({{"PortId", "1"}, {"PortName", ""}, {"Function", "1"}});
  ASSERT_EQ(1u, blank.size());
  const PropertyCell* name = Cell(blank.front(), 1);
  ASSERT_NE(nullptr, name);
  EXPECT_EQ("", name->value.AsString());
  EXPECT_FALSE(name->rejected());
}

TEST_F(MappingTest, RowsWithNoIdentityProduceNoRecord) {
  EXPECT_TRUE(Map({{"PortName", "Rotterdam"}, {"Function", "1"}}).empty());
  EXPECT_TRUE(Map({{"PortId", ""}, {"PortName", "Rotterdam"}, {"Function", "1"}})
                  .empty());
}

TEST_F(MappingTest, FiltersRunBeforeAnythingElse) {
  Mapper mapper(&ontology_, &transforms_, &spec_);
  auto accepts = [&](std::map<std::string, std::string> cells) {
    const FakeRow row(std::move(cells));
    return mapper.Accepts(row);
  };

  EXPECT_TRUE(accepts({{"Function", "1-3-----"}}));
  EXPECT_TRUE(accepts({{"Function", "12345---"}}));
  EXPECT_FALSE(accepts({{"Function", "-2------"}}));
  EXPECT_FALSE(accepts({{"Function", "--3-----"}}));
  // A row missing the filter column is rejected rather than let through: a
  // renamed column must not silently disable the filter.
  EXPECT_FALSE(accepts({{"PortId", "1"}}));
}

TEST_F(MappingTest, NaturalKeysDistinguishFieldBoundaries) {
  const auto a = Map({{"PortId", "14370"}, {"Function", "1"}});
  const auto b = Map({{"PortId", "1437"}, {"Function", "1"}});
  ASSERT_EQ(1u, a.size());
  ASSERT_EQ(1u, b.size());
  EXPECT_NE(a.front().natural_key_hash, b.front().natural_key_hash);

  // The same key from a different row is the same record identity, which is
  // what makes a re-ingest replace rather than duplicate.
  const auto again = Map({{"PortId", "14370"}, {"Function", "1"}}, /*row=*/500);
  EXPECT_EQ(a.front().natural_key_hash, again.front().natural_key_hash);
}

TEST_F(MappingTest, RecordsRoundTripThroughSerialization) {
  const auto records = Map({{"PortId", "14370"},
                            {"PortName", "ROTTERDAM"},
                            {"Country", "nl"},
                            {"Location", "rtm"},
                            {"Alternates", "Europoort; Botlek"},
                            {"Coordinates", "5155N 00429E"},
                            {"Size", "large"},
                            {"Function", "1"}});
  ASSERT_EQ(1u, records.size());
  const SourceRecord& original = records.front();

  std::string encoded;
  original.EncodeTo(&encoded);
  Slice slice(encoded);
  SourceRecord decoded;
  ASSERT_TRUE(SourceRecord::DecodeFrom(&slice, &decoded));
  EXPECT_TRUE(slice.empty());

  EXPECT_EQ(original.source_id, decoded.source_id);
  EXPECT_EQ(original.batch_id, decoded.batch_id);
  EXPECT_EQ(original.row_seq, decoded.row_seq);
  EXPECT_EQ(original.type, decoded.type);
  EXPECT_EQ(original.natural_key, decoded.natural_key);
  EXPECT_EQ(original.natural_key_hash, decoded.natural_key_hash);
  ASSERT_EQ(original.properties.size(), decoded.properties.size());

  for (size_t i = 0; i < original.properties.size(); ++i) {
    const auto& a = original.properties[i];
    const auto& b = decoded.properties[i];
    EXPECT_EQ(a.prop, b.prop);
    EXPECT_EQ(a.value, b.value);
    EXPECT_EQ(a.origin, b.origin);
    EXPECT_EQ(a.chain, b.chain);
    EXPECT_EQ(a.chain_fingerprint, b.chain_fingerprint);
    EXPECT_EQ(a.raw_value, b.raw_value);
    EXPECT_EQ(a.error, b.error);
  }

  for (size_t n = 1; n < encoded.size(); ++n) {
    Slice truncated(encoded.data(), n);
    SourceRecord out;
    EXPECT_FALSE(SourceRecord::DecodeFrom(&truncated, &out))
        << "a " << n << "-byte prefix decoded as a record";
  }
}

// --- rejections -------------------------------------------------------------

namespace {

void ExpectMappingRejected(const std::string& yaml, const std::string& mentions) {
  Ontology onto;
  ASSERT_TRUE(Ontology::LoadFromString(kOntology, &onto, "<onto>").ok());
  TransformRegistry transforms;
  SourceSpec spec;
  const Status s =
      SourceSpec::LoadFromString(yaml, onto, transforms, &spec, "<mapping>");
  ASSERT_FALSE(s.ok()) << "this mapping should not have loaded";
  EXPECT_NE(std::string::npos, s.ToString().find(mentions)) << s.ToString();
}

std::string WithMapping(const std::string& body) {
  return R"(
source:
  id: t
  source_id: 1
  connector: csv
  uri: /dev/null
  natural_key: ["k"]
mappings:
)" + body;
}

}  // namespace

TEST(MappingRejections, NamesAreCheckedAgainstTheSchema) {
  ExpectMappingRejected(
      WithMapping("  - target_type: Harbour\n    properties: { name: { from: n } }"),
      "unknown entity type");
  ExpectMappingRejected(
      WithMapping("  - target_type: Port\n    properties: { nmae: { from: n } }"),
      "has no property");
  ExpectMappingRejected(
      WithMapping(
          "  - target_type: Port\n"
          "    properties: { name: { from: n, transform: [trim, uscg_to_ais_type] } }"),
      "unknown transform");
}

TEST(MappingRejections, SourceIdsAreRequiredAndNumeric) {
  ExpectMappingRejected(R"(
source:
  id: t
  connector: csv
  uri: /dev/null
  natural_key: ["k"]
mappings:
  - target_type: Port
    properties: { name: { from: n } }
)",
                        "numeric `source_id`");
}

// Without a natural key a re-ingest creates a second record for the same
// real-world thing instead of replacing the first.
TEST(MappingRejections, NaturalKeysAreRequired) {
  ExpectMappingRejected(R"(
source:
  id: t
  source_id: 1
  connector: csv
  uri: /dev/null
mappings:
  - target_type: Port
    properties: { name: { from: n } }
)",
                        "natural_key");
}

// Matching a link on an unindexed property would turn every link resolution
// into a full scan of the target type, and nothing would report it as slow -
// it would just be slow.
TEST(MappingRejections, LinksMustMatchOnAnIndexedProperty) {
  ExpectMappingRejected(WithMapping(R"(  - target_type: Voyage
    properties: { voyage_ref: { from: id } }
    links:
      - type: arrives_at
        to_type: Port
        via: { from: "port", match_property: name }
)"),
                        "not `indexed: true`");
}

TEST(MappingRejections, EndpointsMustBeDeclared) {
  ExpectMappingRejected(R"(
source:
  id: t
  source_id: 1
  connector: http
  base_url: https://example.invalid
  natural_key: ["k"]
endpoints:
  - { id: real, path: /a }
mappings:
  - target_type: Port
    from_endpoint: imaginary
    properties: { name: { from: n } }
)",
                        "not declared");
}

// --- the real mapping files -------------------------------------------------

TEST(RealMappings, EveryShippedMappingLoadsAndResolves) {
  SchemaBundle bundle;
  const Status s =
      SchemaBundle::LoadFromDir(std::string(SEXTANT_SOURCE_DIR) + "/schema", &bundle);
  ASSERT_TRUE(s.ok()) << s.ToString();

  for (const auto& spec : bundle.sources()) {
    for (const auto& m : spec.mappings) {
      const EntityTypeDef* type = bundle.ontology().Type(m.target_type);
      ASSERT_NE(nullptr, type) << spec.key;
      EXPECT_FALSE(m.natural_key.empty()) << spec.key << " -> " << m.target_type_name;
      for (const auto& p : m.properties) {
        EXPECT_NE(nullptr, type->Property(p.prop))
            << spec.key << ": " << p.prop_name << " is not on " << type->name;
        EXPECT_FALSE(p.from.empty());
        // Every chain must resolve, which is what makes lineage replayable.
        for (const TransformId id : p.chain) {
          EXPECT_NE(nullptr, bundle.transforms().ById(id))
              << spec.key << ": " << p.prop_name << " uses an unknown transform";
        }
      }
    }
  }
}

// The Digitraffic mapping addresses a nested JSON path. If the syntax for that
// ever changes, this is where it shows up.
TEST(RealMappings, DigitrafficUsesNestedPaths) {
  SchemaBundle bundle;
  ASSERT_TRUE(
      SchemaBundle::LoadFromDir(std::string(SEXTANT_SOURCE_DIR) + "/schema", &bundle)
          .ok());

  const SourceSpec* spec = bundle.Source("digitraffic");
  ASSERT_NE(nullptr, spec);
  bool found = false;
  for (const auto& m : spec->mappings) {
    if (m.target_type_name != "Voyage") continue;
    EXPECT_EQ("port_calls", m.from_endpoint);
    EXPECT_EQ(3u, m.links.size()) << "a port call implies three edges";
    for (const auto& p : m.properties) {
      if (p.prop_name == "arrived_at") {
        found = true;
        EXPECT_EQ("portAreaDetails[0].ata", p.from.front());
      }
    }
  }
  EXPECT_TRUE(found);

  // The endpoint that wraps its array, which is why records_at exists.
  const EndpointSpec* endpoint = spec->Endpoint("port_calls");
  ASSERT_NE(nullptr, endpoint);
  EXPECT_EQ("portCalls", endpoint->records_at);
}
