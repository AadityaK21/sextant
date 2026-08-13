// The ontology loader.
//
// Most of these tests assert that a BAD schema is REJECTED, which is the
// loader's real job. A schema that loads is one the rest of the system takes on
// trust, so every check that would otherwise become a null pointer or a silently
// missing index has to happen here, at startup, with a message that names the
// thing that is wrong.

#include "schema.h"

#include <gtest/gtest.h>

#include <string>

#include "bundle.h"
#include "value.h"

using namespace sextant::ontology;
namespace codec = sextant::codec;

namespace {

// A minimal valid schema, used as the base every negative test mutates.
//
// The delimiter is R"YAML( rather than plain R"( because the display template
// below contains the two characters `)` and `"` next to each other, which would
// otherwise end the raw string in the middle of the schema.
constexpr const char* kValid = R"YAML(
version: 1
namespace: maritime
entity_types:
  Port:
    id: 1
    properties:
      name:   { id: 1, type: string, title: true }
      locode: { id: 2, type: string, indexed: true }
    display: "{name} ({locode})"
  Voyage:
    id: 2
    properties:
      voyage_ref: { id: 10, type: string, title: true }
      arrived_at: { id: 11, type: timestamp, indexed: true }
link_types:
  arrives_at:
    id: 1
    from: Voyage
    to: Port
    cardinality: many_to_one
    time_index: arrived_at
)YAML";

Status Load(const std::string& yaml, Ontology* out) {
  return Ontology::LoadFromString(yaml, out, "<test>");
}

// Assert that loading fails and that the message names the culprit. A rejection
// with an unhelpful message is only marginally better than no rejection.
void ExpectRejected(const std::string& yaml, const std::string& mentions) {
  Ontology onto;
  const Status s = Load(yaml, &onto);
  ASSERT_FALSE(s.ok()) << "this schema should not have loaded";
  EXPECT_NE(std::string::npos, s.ToString().find(mentions))
      << "message did not mention \"" << mentions << "\": " << s.ToString();
}

}  // namespace

TEST(Schema, LoadsAValidSchema) {
  Ontology onto;
  ASSERT_TRUE(Load(kValid, &onto).ok());

  EXPECT_EQ(1, onto.version());
  EXPECT_EQ("maritime", onto.name_space());
  ASSERT_EQ(2u, onto.types().size());

  const EntityTypeDef* port = onto.Type("Port");
  ASSERT_NE(nullptr, port);
  EXPECT_EQ(1, port->id);
  EXPECT_EQ(port, onto.Type(static_cast<codec::TypeId>(1)));

  const PropertyDef* locode = port->Property("locode");
  ASSERT_NE(nullptr, locode);
  EXPECT_EQ(2, locode->id);
  EXPECT_TRUE(locode->indexed);
  EXPECT_FALSE(locode->title);
  EXPECT_EQ("name", port->TitleProperty()->name);

  const LinkTypeDef* link = onto.Link("arrives_at");
  ASSERT_NE(nullptr, link);
  EXPECT_EQ(onto.Type("Voyage")->id, link->from);
  EXPECT_EQ(port->id, link->to);
  EXPECT_TRUE(link->HasTimeIndex());
  EXPECT_EQ(11, link->time_index);
}

// Property ids are unique across the whole schema, not per type, because a PROV
// key carries a property id but no type id.
TEST(Schema, PropertyIdsAreGloballyUnique) {
  Ontology onto;
  ASSERT_TRUE(Load(kValid, &onto).ok());
  EXPECT_EQ("locode", onto.Property(2)->name);
  EXPECT_EQ("Port", onto.TypeOfProperty(2)->name);
  EXPECT_EQ("Voyage", onto.TypeOfProperty(11)->name);
  EXPECT_EQ(nullptr, onto.Property(999));
}

TEST(Schema, RejectsMissingOrDuplicateIds) {
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    properties:
      name: { id: 1, type: string }
)",
                 "id");

  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties:
      name: { id: 1, type: string }
  Vessel:
    id: 1
    properties:
      name: { id: 2, type: string }
)",
                 "duplicate entity type id");

  // The one that would silently corrupt provenance if it got through.
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties:
      name: { id: 1, type: string }
  Vessel:
    id: 2
    properties:
      name: { id: 1, type: string }
)",
                 "unique across");
}

TEST(Schema, RejectsBrokenLinks) {
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string } }
link_types:
  arrives_at: { id: 1, from: Voyage, to: Port }
)",
                 "unknown `from` type");

  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string } }
link_types:
  arrives_at: { id: 1, from: Port, to: Port, cardinality: sometimes }
)",
                 "cardinality");
}

// A mistyped time_index does not break anything visibly. It just quietly stops
// TIDX being populated, and the headline query silently degrades from a range
// scan into a full scan. Which is exactly why the loader is strict about it.
TEST(Schema, RejectsATimeIndexThatWouldSilentlyDoNothing) {
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string } }
  Voyage:
    id: 2
    properties: { arrived_at: { id: 10, type: timestamp } }
link_types:
  arrives_at: { id: 1, from: Voyage, to: Port, time_index: arrived_ta }
)",
                 "not a property of Voyage");

  // Time-indexing something that is not a timestamp.
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string } }
  Voyage:
    id: 2
    properties: { cargo: { id: 10, type: string } }
link_types:
  arrives_at: { id: 1, from: Voyage, to: Port, time_index: cargo }
)",
                 "rather than a timestamp");
}

TEST(Schema, RejectsSchemasThatWouldRenderWrong) {
  // A display template naming a property that does not exist renders as a blank
  // in the UI and is nearly impossible to trace from the outside.
  ExpectRejected(R"YAML(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string } }
    display: "{name} ({locode})"
)YAML",
                 "display template references");

  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties:
      name:   { id: 1, type: string, title: true }
      locode: { id: 2, type: string, title: true }
)",
                 "title properties");
}

// A list fused by anything but union keeps one value and throws the rest away,
// which is never what declaring it a list meant.
TEST(Schema, RejectsAListThatWouldLoseValues) {
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties:
      alt_names: { id: 1, type: "string[]", fuse: most_trusted }
)",
                 "must fuse with `union`");
}

TEST(Schema, RejectsUnknownTypesAndRules) {
  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: geometry } }
)",
                 "unknown type");

  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { name: { id: 1, type: string, fuse: vibes } }
)",
                 "unknown fusion rule");

  ExpectRejected(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties: { size: { id: 1, type: enum } }
)",
                 "declares no `values`");
}

TEST(Schema, EnumsAreStringsWithAPermittedSet) {
  Ontology onto;
  ASSERT_TRUE(Load(R"(
version: 1
entity_types:
  Port:
    id: 1
    properties:
      harbor_size: { id: 1, type: enum, values: [V, L, M, S] }
)",
                   &onto)
                  .ok());
  const PropertyDef* p = onto.Type("Port")->Property("harbor_size");
  ASSERT_NE(nullptr, p);
  EXPECT_EQ(ValueType::kString, p->type);
  EXPECT_TRUE(p->IsEnum());
  EXPECT_TRUE(p->Permits("L"));
  EXPECT_FALSE(p->Permits("XL"));
}

TEST(Schema, RendersDisplayTemplates) {
  Ontology onto;
  ASSERT_TRUE(Load(kValid, &onto).ok());
  const EntityTypeDef* port = onto.Type("Port");
  EXPECT_EQ("Rotterdam (NLRTM)",
            onto.RenderDisplay(*port, {{1, "Rotterdam"}, {2, "NLRTM"}}));
  // A property with no value renders empty rather than leaving the placeholder.
  EXPECT_EQ("Rotterdam ()", onto.RenderDisplay(*port, {{1, "Rotterdam"}}));
}

TEST(Schema, RejectsMalformedYaml) {
  Ontology onto;
  EXPECT_FALSE(Load("version: 1\nentity_types: [not, a, map]", &onto).ok());
  EXPECT_FALSE(Load("this: is: not: yaml:", &onto).ok());
  EXPECT_FALSE(Load("", &onto).ok());
}

// The schema this project actually ships. If ontology.yaml stops loading, the
// binary stops working, so it is worth a test of its own rather than being
// discovered by running the CLI.
TEST(Schema, TheRealOntologyLoads) {
  Ontology onto;
  ASSERT_TRUE(
      Ontology::LoadFromFile(std::string(SEXTANT_SOURCE_DIR) + "/schema/ontology.yaml",
                             &onto)
          .ok());

  EXPECT_EQ("maritime", onto.name_space());
  ASSERT_NE(nullptr, onto.Type("Port"));
  ASSERT_NE(nullptr, onto.Type("Vessel"));
  ASSERT_NE(nullptr, onto.Type("Voyage"));

  // The declaration the headline query depends on.
  const LinkTypeDef* arrives = onto.Link("arrives_at");
  ASSERT_NE(nullptr, arrives);
  ASSERT_TRUE(arrives->HasTimeIndex());
  EXPECT_EQ(onto.Type("Voyage")->Property("arrived_at")->id, arrives->time_index);

  // Every property a link matches on must be indexed, or resolving that link
  // becomes a full scan of the target type.
  EXPECT_TRUE(onto.Type("Port")->Property("locode")->indexed);
  EXPECT_TRUE(onto.Type("Vessel")->Property("mmsi")->indexed);
  EXPECT_TRUE(onto.Type("Vessel")->Property("imo")->indexed);
}

// The whole declarative layer, loaded the way the binary loads it.
TEST(Schema, TheRealSchemaDirectoryLoads) {
  SchemaBundle bundle;
  const Status s =
      SchemaBundle::LoadFromDir(std::string(SEXTANT_SOURCE_DIR) + "/schema", &bundle);
  ASSERT_TRUE(s.ok()) << s.ToString();

  EXPECT_GE(bundle.sources().size(), 4u);
  for (const char* key :
       {"wpi", "unlocode", "digitraffic", "digitraffic_ais", "marinecadastre"}) {
    const SourceSpec* spec = bundle.Source(key);
    ASSERT_NE(nullptr, spec) << key << " is missing";
    EXPECT_GT(spec->id, 0u) << key << " has no numeric source_id";
    EXPECT_FALSE(spec->mappings.empty()) << key << " maps nothing";
  }

  // All three connector kinds are represented, which is what the brief asked
  // for and is easy to lose track of.
  EXPECT_EQ(ConnectorKind::kCsv, bundle.Source("wpi")->connector);
  EXPECT_EQ(ConnectorKind::kCsv, bundle.Source("unlocode")->connector);
  EXPECT_EQ(ConnectorKind::kHttp, bundle.Source("digitraffic")->connector);
  EXPECT_EQ(ConnectorKind::kPostgres, bundle.Source("marinecadastre")->connector);

  // UN/LOCODE is the code authority and must outrank the World Port Index on
  // the locode property, which is what the trust weights encode.
  EXPECT_GT(bundle.Source("unlocode")->trust, bundle.Source("wpi")->trust);
  // A registry record outranks a self-reported AIS broadcast, for the same
  // reason. If these ever invert, the fusion rules quietly start preferring
  // whatever a transmitter was configured with.
  EXPECT_GT(bundle.Source("digitraffic")->trust,
            bundle.Source("digitraffic_ais")->trust);

  // The two vessel feeds must not share a source id: SRCREC keys carry no
  // endpoint, so a shared id would make the second ingest overwrite the first
  // and the vessels would have no cross-source duplicates at all.
  EXPECT_NE(bundle.Source("digitraffic")->id, bundle.Source("digitraffic_ais")->id);
}
