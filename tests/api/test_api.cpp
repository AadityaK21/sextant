// The HTTP API, tested over a real socket.
//
// WHY A REAL SOCKET RATHER THAN CALLING THE HANDLERS DIRECTLY
//
// Most of what can go wrong in an HTTP layer is in the parts a direct call
// skips: route regexes that do not match the ids they were written for, query
// parameters that arrive percent-encoded, a status code that says 500 when it
// means 400, a preflight that is never answered. A test that invokes the
// handler function proves none of that.
//
// Binding to port 0 lets the OS pick a free port, so these run safely under
// `ctest -j`. A fixed test port is a flaky test waiting for a second job on the
// same machine, which is a lesson this repository has already paid for once.

#include "server.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "corpus.h"

namespace {

namespace api = sextant::api;
namespace onto = sextant::ontology;

using nlohmann::json;
using sextant::testsupport::Corpus;

class ApiTest : public ::testing::Test {
 protected:
  static Corpus corpus_;
  static std::unique_ptr<api::Server> server_;
  static int port_;

  static void SetUpTestSuite() {
    ASSERT_TRUE(sextant::testsupport::BuildCorpus("apitest", &corpus_));

    api::ServerOptions options;
    options.host = "127.0.0.1";
    options.data_root = sextant::testsupport::SourceDir();
    options.log_requests = false;
    server_ = std::make_unique<api::Server>(corpus_.store.get(), &corpus_.bundle,
                                            options);
    ASSERT_TRUE(server_->StartBackground(&port_).ok());
  }

  static void TearDownTestSuite() {
    server_.reset();
    corpus_.store.reset();
    corpus_.Destroy();
  }

  static httplib::Client Client() {
    httplib::Client client("127.0.0.1", port_);
    client.set_read_timeout(30, 0);
    return client;
  }

  // Fetch and parse, failing the test rather than throwing on bad JSON.
  static bool GetJson(const std::string& path, json* out, int* status = nullptr) {
    auto client = Client();
    auto res = client.Get(path.c_str());
    if (!res) return false;
    if (status != nullptr) *status = res->status;
    try {
      *out = json::parse(res->body);
    } catch (const json::parse_error&) {
      return false;
    }
    return true;
  }

  static std::string RotterdamId() {
    json body;
    if (!GetJson("/api/entities?type=Port&locode=NLRTM", &body)) return {};
    if (!body.contains("entities") || body["entities"].empty()) return {};
    return body["entities"][0]["id"].get<std::string>();
  }
};

Corpus ApiTest::corpus_;
std::unique_ptr<api::Server> ApiTest::server_;
int ApiTest::port_ = 0;

TEST_F(ApiTest, TheOntologyRouteDescribesTheSchemaWellEnoughToRenderIt) {
  json body;
  ASSERT_TRUE(GetJson("/api/ontology", &body));

  ASSERT_TRUE(body.contains("types"));
  EXPECT_GT(body["types"].size(), 2u);
  EXPECT_GT(body["links"].size(), 0u);

  // The frontend decides whether to offer a date filter from this flag, so it
  // has to be present and it has to be right.
  bool found_timed_link = false;
  for (const auto& link : body["links"]) {
    if (link["time_indexed"].get<bool>()) {
      found_timed_link = true;
      EXPECT_TRUE(link.contains("time_index"));
      EXPECT_FALSE(link["time_index"].get<std::string>().empty());
    }
  }
  EXPECT_TRUE(found_timed_link) << "no link carries a time index";
}

TEST_F(ApiTest, EveryResponseCarriesItsCost) {
  json body;
  ASSERT_TRUE(GetJson("/api/entities?type=Port&limit=5", &body));
  ASSERT_TRUE(body.contains("_stats"));
  EXPECT_TRUE(body["_stats"].contains("keys_scanned"));
  EXPECT_TRUE(body["_stats"].contains("index_used"));
  EXPECT_TRUE(body["_stats"].contains("elapsed_us"));
  ASSERT_TRUE(body.contains("_plan"));
  EXPECT_GT(body["_plan"]["steps"].size(), 0u);
  // The reason string is the point of the plan being on the wire at all.
  EXPECT_FALSE(body["_plan"]["steps"][0]["reason"].get<std::string>().empty());
}

TEST_F(ApiTest, SearchingByPrefixFindsRotterdamAndReportsTheIndex) {
  json body;
  ASSERT_TRUE(GetJson("/api/entities?type=Port&q=Rott", &body));
  ASSERT_GT(body["entities"].size(), 0u);
  EXPECT_EQ(body["_stats"]["index_used"].get<std::string>(), "IDX_PREFIX");
}

TEST_F(ApiTest, AnUnknownQueryParameterIsRejectedRatherThanIgnored) {
  // Ignoring it would silently widen the filter, and the caller would see a
  // result set that looks like a data problem rather than a typo.
  int status = 0;
  json body;
  ASSERT_TRUE(GetJson("/api/entities?type=Port&locoode=NLRTM", &body, &status));
  EXPECT_EQ(status, 400);
  EXPECT_EQ(body["error"]["code"].get<std::string>(), "invalid_argument");
}

TEST_F(ApiTest, AnUnknownTypeIsAClientErrorAndNotAServerFault) {
  int status = 0;
  json body;
  ASSERT_TRUE(GetJson("/api/entities?type=Sandwich", &body, &status));
  EXPECT_EQ(status, 400);
}

TEST_F(ApiTest, AWellFormedIdThatDoesNotExistIsFourOhFour) {
  int status = 0;
  json body;
  // A valid ULID by shape, generated rather than stored.
  ASSERT_TRUE(GetJson("/api/entities/7ZZZZZZZZZZZZZZZZZZZZZZZZZ", &body, &status));
  EXPECT_EQ(status, 404);
  EXPECT_EQ(body["error"]["code"].get<std::string>(), "not_found");
}

TEST_F(ApiTest, OneEntityComesBackWithAProvenanceSummary) {
  const std::string id = RotterdamId();
  ASSERT_FALSE(id.empty());

  json body;
  ASSERT_TRUE(GetJson("/api/entities/" + id, &body));
  EXPECT_EQ(body["type"].get<std::string>(), "Port");
  EXPECT_TRUE(body["properties"].contains("locode"));

  ASSERT_TRUE(body.contains("_provenance"));
  ASSERT_TRUE(body["_provenance"].contains("locode"));
  const auto& provenance = body["_provenance"]["locode"];
  EXPECT_FALSE(provenance["source"].get<std::string>().empty());
  EXPECT_FALSE(provenance["rule"].get<std::string>().empty());
  // Recomputed on read, not stored. If it were stored it would only be a claim.
  EXPECT_TRUE(provenance["verified"].get<bool>());
}

TEST_F(ApiTest, TheQuarterQueryOverHttpUsesTheTimeIndex) {
  const std::string id = RotterdamId();
  ASSERT_FALSE(id.empty());

  const json request = {
      {"start", {{"type", "Port"}, {"ids", json::array({id})}}},
      {"hops", json::array({{{"link", "arrivals"},
                             {"reverse", true},
                             {"where",
                              {{"arrived_at",
                                {{"gte", "2026-04-01T00:00:00Z"},
                                 {"lt", "2026-07-01T00:00:00Z"}}}}}}})},
      {"limit", 500}};

  auto client = Client();
  auto res = client.Post("/api/traverse", request.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;

  const json body = json::parse(res->body);
  std::printf("  %llu arrivals, %llu keys, %llu us, index %s\n",
              static_cast<unsigned long long>(body["count"].get<uint64_t>()),
              static_cast<unsigned long long>(
                  body["_stats"]["keys_scanned"].get<uint64_t>()),
              static_cast<unsigned long long>(
                  body["_stats"]["elapsed_us"].get<uint64_t>()),
              body["_stats"]["index_used"].get<std::string>().c_str());

  EXPECT_GT(body["count"].get<uint64_t>(), 0u);
  EXPECT_EQ(body["_stats"]["index_used"].get<std::string>(), "TIDX");
  EXPECT_LT(body["_stats"]["elapsed_us"].get<uint64_t>(), 50000u);

  // Timestamps go out as ISO strings, not epoch numbers. See json.h: an epoch
  // in milliseconds is past the exact-integer range of a JavaScript number,
  // and the client here is a browser.
  ASSERT_GT(body["entities"].size(), 0u);
  const auto& first = body["entities"][0]["properties"];
  ASSERT_TRUE(first.contains("arrived_at"));
  EXPECT_TRUE(first["arrived_at"].is_string());
  EXPECT_NE(first["arrived_at"].get<std::string>().find('T'), std::string::npos);
}

TEST_F(ApiTest, MalformedJsonIsAFourHundredWithAUsefulMessage) {
  auto client = Client();
  auto res = client.Post("/api/traverse", "{not json", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  const json body = json::parse(res->body);
  EXPECT_EQ(body["error"]["code"].get<std::string>(), "invalid_argument");
}

TEST_F(ApiTest, ExplainReturnsAPlanWithoutRunningTheQuery) {
  // `harbor_size` carries no index, so this is the case worth explaining: the
  // plan says SCAN and warns, before anyone waits for the query.
  const json request = {
      {"start", {{"type", "Port"}, {"filter", {{"harbor_size", "L"}}}}},
      {"limit", 10}};

  auto client = Client();
  auto res = client.Post("/api/explain", request.dump(), "application/json");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200) << res->body;

  const json body = json::parse(res->body);
  ASSERT_TRUE(body.contains("_plan"));
  EXPECT_EQ(body["_plan"]["steps"][0]["access_path"].get<std::string>(), "SCAN");
  // The warning is the whole value of this route: it says the query will be
  // slow before anyone waits for it.
  EXPECT_GT(body["_plan"]["warnings"].size(), 0u);
  // Nothing was executed, so there is no cost block to report.
  EXPECT_FALSE(body.contains("_stats"));
}

TEST_F(ApiTest, TheLinksRouteWorksOutTheDirectionFromTheSchema) {
  const std::string id = RotterdamId();
  ASSERT_FALSE(id.empty());

  json body;
  int status = 0;
  ASSERT_TRUE(GetJson("/api/entities/" + id + "/links?type=arrivals&limit=10",
                      &body, &status));
  ASSERT_EQ(status, 200) << body.dump();
  EXPECT_GT(body["count"].get<uint64_t>(), 0u);
  for (const auto& entity : body["entities"]) {
    EXPECT_EQ(entity["type"].get<std::string>(), "Voyage");
  }
}

TEST_F(ApiTest, LineageGoesFromAValueBackToTheVerbatimSourceRow) {
  const std::string id = RotterdamId();
  ASSERT_FALSE(id.empty());

  json body;
  int status = 0;
  ASSERT_TRUE(GetJson("/api/lineage/" + id + "/name", &body, &status));
  ASSERT_EQ(status, 200) << body.dump();

  EXPECT_TRUE(body["found"].get<bool>());
  EXPECT_FALSE(body["stored_value"].get<std::string>().empty());
  EXPECT_FALSE(body["origin"]["source"].get<std::string>().empty());
  EXPECT_TRUE(body["raw_row_found"].get<bool>());
  EXPECT_FALSE(body["raw_row"].get<std::string>().empty());
  EXPECT_TRUE(body["replay"]["matches"].get<bool>())
      << "replayed " << body["replay"]["value"] << " stored "
      << body["stored_value"];

  // And the raw row is fetchable on its own, which is what the lineage panel's
  // "show me the original" link does.
  const std::string source = body["origin"]["source"].get<std::string>();
  const auto batch = body["origin"]["batch"].get<uint64_t>();
  const auto row = body["origin"]["row"].get<uint64_t>();

  json raw;
  ASSERT_TRUE(GetJson("/api/raw/" + source + "/" + std::to_string(batch) + "/" +
                          std::to_string(row),
                      &raw, &status));
  ASSERT_EQ(status, 200) << raw.dump();
  EXPECT_EQ(raw["raw"].get<std::string>(), body["raw_row"].get<std::string>());
}

TEST_F(ApiTest, AskingForLineageOfAPropertyThatDoesNotExistIsAFourHundred) {
  const std::string id = RotterdamId();
  ASSERT_FALSE(id.empty());
  int status = 0;
  json body;
  ASSERT_TRUE(GetJson("/api/lineage/" + id + "/not_a_property", &body, &status));
  EXPECT_EQ(status, 400);
}

TEST_F(ApiTest, StatsReportsEntityCountsAndTheDedupRatio) {
  json body;
  ASSERT_TRUE(GetJson("/api/stats", &body));

  EXPECT_GT(body["total_entities"].get<uint64_t>(), 300u);
  EXPECT_GT(body["source_records"].get<uint64_t>(), 600u);

  // The fraction of records removed by merging - the same definition
  // `sextant resolve` prints. Asserted rather than assumed, because the two
  // were briefly complements of each other and nothing caught it.
  const double ratio = body["dedup_ratio"].get<double>();
  const double expected =
      1.0 - static_cast<double>(body["total_entities"].get<uint64_t>()) /
                static_cast<double>(body["source_records"].get<uint64_t>());
  EXPECT_NEAR(ratio, expected, 1e-9);
  EXPECT_GT(ratio, 0.0) << "resolution merged nothing";
  EXPECT_LT(ratio, 1.0);
  std::printf("  %llu entities from %llu source records, ratio %.4f\n",
              static_cast<unsigned long long>(body["total_entities"].get<uint64_t>()),
              static_cast<unsigned long long>(body["source_records"].get<uint64_t>()),
              ratio);

  ASSERT_TRUE(body.contains("engine"));
  EXPECT_GT(body["engine"]["writes"].get<uint64_t>(), 0u);
}

TEST_F(ApiTest, TheReviewQueueComesBackMostUncertainFirst) {
  json body;
  ASSERT_TRUE(GetJson("/api/review?limit=10", &body));
  ASSERT_TRUE(body.contains("pairs"));

  // CAND is keyed by inverted score, so a forward scan is already sorted. This
  // asserts that property rather than assuming it.
  double previous = 1e308;
  for (const auto& pair : body["pairs"]) {
    const double score = pair["score"].get<double>();
    EXPECT_LE(score, previous + 1e-9);
    previous = score;
  }
}

TEST_F(ApiTest, ThePreflightIsAnsweredSoTheBrowserWillSendThePost) {
  // Without this the traverse route works in curl and fails in a browser, which
  // is a confusing hour if you have not met CORS preflight before.
  auto client = Client();
  auto res = client.Options("/api/traverse");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 204);
  EXPECT_TRUE(res->has_header("Access-Control-Allow-Origin"));
}

TEST_F(ApiTest, ALimitOutsideTheAllowedRangeIsRejected) {
  int status = 0;
  json body;
  ASSERT_TRUE(GetJson("/api/entities?type=Port&limit=0", &body, &status));
  EXPECT_EQ(status, 400);
  ASSERT_TRUE(GetJson("/api/entities?type=Port&limit=999999", &body, &status));
  EXPECT_EQ(status, 400);
}

}  // namespace
