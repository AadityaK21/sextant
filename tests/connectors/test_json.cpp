// The JSON row source and the snapshot fetcher.

#include "json_source.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace sextant::connectors;

namespace {

std::unique_ptr<JsonRowSource> Open(const std::string& body,
                                    const std::string& records_at = {},
                                    const std::string& endpoint = "test") {
  std::unique_ptr<JsonRowSource> src;
  const Status s = JsonRowSource::Open(body, records_at, endpoint, &src);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return src;
}

std::string Cell(const JsonRowSource& src, const std::string& path) {
  std::string out;
  EXPECT_TRUE(src.current().Get(path, &out)) << "no value at " << path;
  return out;
}

}  // namespace

TEST(JsonSource, StreamsABareArray) {
  auto src = Open(R"([{"a": 1}, {"a": 2}])");
  ASSERT_NE(nullptr, src);
  EXPECT_EQ(2u, src->size());

  ASSERT_TRUE(src->Next());
  EXPECT_EQ("1", Cell(*src, "a"));
  EXPECT_EQ(1u, src->row_seq());
  ASSERT_TRUE(src->Next());
  EXPECT_EQ("2", Cell(*src, "a"));
  EXPECT_FALSE(src->Next());
  EXPECT_EQ("test", src->endpoint());
}

TEST(JsonSource, UnwrapsARecordsAtPath) {
  auto src = Open(R"({"portCalls": [{"id": 1}], "meta": {}})", "portCalls");
  ASSERT_NE(nullptr, src);
  EXPECT_EQ(1u, src->size());
  ASSERT_TRUE(src->Next());
  EXPECT_EQ("1", Cell(*src, "id"));
}

// An API that changed shape should stop the ingest, not silently load nothing.
// A connector that quietly writes zero records looks exactly like a source with
// no new data.
TEST(JsonSource, AShapeChangeIsAnErrorRatherThanAnEmptyStream) {
  std::unique_ptr<JsonRowSource> src;
  EXPECT_FALSE(JsonRowSource::Open(R"({"portCalls": []})", "portCall", "e", &src).ok());
  EXPECT_FALSE(JsonRowSource::Open(R"({"portCalls": {}})", "portCalls", "e", &src).ok());
  EXPECT_FALSE(JsonRowSource::Open(R"({"a": 1})", "", "e", &src).ok());
  EXPECT_FALSE(JsonRowSource::Open("not json at all", "", "e", &src).ok());
  EXPECT_FALSE(JsonRowSource::Open(R"([{"a": 1},)", "", "e", &src).ok())
      << "a truncated download must be an error, not a short read";

  // An empty array IS valid - the source genuinely had nothing.
  ASSERT_TRUE(JsonRowSource::Open("[]", "", "e", &src).ok());
  EXPECT_FALSE(src->Next());
}

TEST(JsonPaths, WalkNestedObjectsAndArrays) {
  const std::string doc = R"({
    "portCallId": 3120001,
    "portAreaDetails": [
      {"ata": "2026-04-03T07:15:00+03:00", "berth": {"code": "A1"}},
      {"ata": "2026-04-04T07:15:00+03:00"}
    ]
  })";
  std::string out;
  EXPECT_TRUE(JsonPathLookup(doc, "portCallId", &out));
  EXPECT_EQ("3120001", out);
  EXPECT_TRUE(JsonPathLookup(doc, "portAreaDetails[0].ata", &out));
  EXPECT_EQ("2026-04-03T07:15:00+03:00", out);
  EXPECT_TRUE(JsonPathLookup(doc, "portAreaDetails[1].ata", &out));
  EXPECT_EQ("2026-04-04T07:15:00+03:00", out);
  EXPECT_TRUE(JsonPathLookup(doc, "portAreaDetails[0].berth.code", &out));
  EXPECT_EQ("A1", out);

  // Out of range, missing key, wrong kind of container: all absent rather than
  // empty, so the mapper knows the source does not carry the value.
  EXPECT_FALSE(JsonPathLookup(doc, "portAreaDetails[9].ata", &out));
  EXPECT_FALSE(JsonPathLookup(doc, "portAreaDetails[1].berth.code", &out));
  EXPECT_FALSE(JsonPathLookup(doc, "nosuchfield", &out));
  EXPECT_FALSE(JsonPathLookup(doc, "portCallId.nested", &out));
  EXPECT_FALSE(JsonPathLookup(doc, "portAreaDetails[]", &out));
}

// Digitraffic reports an IMO as a JSON number. Rendering an integral value
// through a floating-point path gives "9074729.0", which validate_imo would
// then reject for having eight characters - and the vessel would silently lose
// its strongest identifier.
TEST(JsonSource, NumbersRenderAsIntegersWhenTheyAreIntegers) {
  auto src = Open(R"([{
    "imo": 9074729,
    "mmsi": 230123456,
    "grossTonnage": 24500.0,
    "draught": 6.8,
    "negative": -12,
    "flag": true,
    "missing": null,
    "nested": {"a": 1}
  }])");
  ASSERT_TRUE(src->Next());
  EXPECT_EQ("9074729", Cell(*src, "imo"));
  EXPECT_EQ("230123456", Cell(*src, "mmsi"));
  EXPECT_EQ("24500", Cell(*src, "grossTonnage"));
  EXPECT_EQ("6.8", Cell(*src, "draught"));
  EXPECT_EQ("-12", Cell(*src, "negative"));
  EXPECT_EQ("true", Cell(*src, "flag"));
  // An explicit JSON null is present-but-empty, which is different from absent
  // and is what null_if_blank exists to handle.
  EXPECT_EQ("", Cell(*src, "missing"));
  EXPECT_EQ(R"({"a":1})", Cell(*src, "nested"));
}

TEST(JsonSource, RawIsTheRecordNotTheWholeDocument) {
  auto src = Open(R"({"portCalls": [{"id": 1}, {"id": 2}]})", "portCalls");
  ASSERT_TRUE(src->Next());
  EXPECT_EQ(R"({"id":1})", src->current().Raw());
  ASSERT_TRUE(src->Next());
  EXPECT_EQ(R"({"id":2})", src->current().Raw());
}

// --- the snapshot fetcher ---------------------------------------------------

TEST(SnapshotFetcher, ReplaysTheShippedResponses) {
  SnapshotFetcher fetcher(std::string(SEXTANT_SOURCE_DIR) +
                          "/data/snapshots/digitraffic");
  std::string body;
  ASSERT_TRUE(fetcher.Fetch("port_calls", "/api/port-call/v1/port-calls", &body).ok());
  EXPECT_FALSE(body.empty());

  auto src = Open(body, "portCalls", "port_calls");
  ASSERT_NE(nullptr, src);
  EXPECT_GE(src->size(), 5u);

  // Shape, not literals: the corpus is generated by eval/make_corpus.py and the
  // specific ids move when it is regenerated.
  ASSERT_TRUE(src->Next());
  EXPECT_FALSE(Cell(*src, "portCallId").empty());
  EXPECT_EQ(5u, Cell(*src, "portToVisit").size()) << "a UN/LOCODE is 5 characters";
  const std::string ata = Cell(*src, "portAreaDetails[0].ata");
  EXPECT_NE(std::string::npos, ata.find("2026-")) << ata;
  EXPECT_NE(std::string::npos, ata.find('T')) << ata;
}

// A missing snapshot must say what to run, not just fail. This is the message a
// new contributor sees first.
TEST(SnapshotFetcher, MissingSnapshotsExplainThemselves) {
  SnapshotFetcher fetcher("/nonexistent/snapshot/dir");
  std::string body;
  const Status s = fetcher.Fetch("ports", "/api/port-call/v1/ports", &body);
  ASSERT_FALSE(s.ok());
  EXPECT_NE(std::string::npos, s.ToString().find("sextant fetch"));
  EXPECT_NE(std::string::npos, s.ToString().find("ports.json"));
}

TEST(SnapshotFetcher, EveryShippedEndpointParses) {
  SnapshotFetcher fetcher(std::string(SEXTANT_SOURCE_DIR) +
                          "/data/snapshots/digitraffic");
  for (const char* endpoint : {"ports", "vessel_details"}) {
    std::string body;
    ASSERT_TRUE(fetcher.Fetch(endpoint, "/", &body).ok()) << endpoint;
    std::unique_ptr<JsonRowSource> src;
    const Status s = JsonRowSource::Open(body, "", endpoint, &src);
    ASSERT_TRUE(s.ok()) << endpoint << ": " << s.ToString();
    EXPECT_GT(src->size(), 0u) << endpoint;
  }
}
