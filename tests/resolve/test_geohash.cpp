// Geohash encoding, neighbour expansion and the haversine metric it stands in
// for.
//
// The test that earns its place is the last one: it checks the geohash against
// the distance it is supposed to approximate, rather than only against itself.

#include "geohash.h"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace sextant::resolve;

// Published geohash values. Encoding that only agrees with its own decoder is
// a self-consistent encoding of nothing in particular.
TEST(Geohash, MatchesKnownValues) {
  // The canonical example from the original geohash description.
  EXPECT_EQ("ezs42", GeohashEncode(42.6, -5.6, 5));
  // Null Island.
  EXPECT_EQ("s0000", GeohashEncode(0.0, 0.0, 5));
  // Longitude comes first in the interleave; if that were reversed these two
  // would swap.
  EXPECT_EQ("u", GeohashEncode(51.92, 4.48, 1));
}

TEST(Geohash, PrecisionNarrowsTheCell) {
  const double lat = 51.9225, lon = 4.47917;
  std::string previous;
  for (int precision = 1; precision <= 8; ++precision) {
    const std::string hash = GeohashEncode(lat, lon, precision);
    EXPECT_EQ(static_cast<size_t>(precision), hash.size());
    // Each precision extends the previous one - that prefix property is the
    // whole reason a geohash works as a blocking key.
    EXPECT_EQ(previous, hash.substr(0, previous.size()));
    previous = hash;
  }
}

TEST(Geohash, DecodesBackInsideItsOwnErrorBounds) {
  const double points[][2] = {{51.9225, 4.47917}, {-33.92, 18.42},
                              {60.17, 24.94},     {-36.85, 174.76},
                              {0.0, 0.0},         {89.9, 179.9},
                              {-89.9, -179.9}};
  for (const auto& point : points) {
    for (int precision = 3; precision <= 8; ++precision) {
      const std::string hash = GeohashEncode(point[0], point[1], precision);
      double lat, lon, lat_err, lon_err;
      ASSERT_TRUE(GeohashDecode(hash, &lat, &lon, &lat_err, &lon_err)) << hash;
      EXPECT_LE(std::fabs(lat - point[0]), lat_err) << hash;
      EXPECT_LE(std::fabs(lon - point[1]), lon_err) << hash;
    }
  }
}

TEST(Geohash, ClampsRatherThanRejectsOutOfRangeInput) {
  // A source reporting 90.0001 has a rounding problem, not a record that should
  // disappear from every block.
  EXPECT_FALSE(GeohashEncode(90.5, 0.0, 5).empty());
  EXPECT_FALSE(GeohashEncode(-90.5, 0.0, 5).empty());
  EXPECT_FALSE(GeohashEncode(0.0, 180.5, 5).empty());
  EXPECT_TRUE(GeohashEncode(0.0, 0.0, 0).empty());
}

TEST(Geohash, RejectsSymbolsOutsideItsAlphabet) {
  double lat, lon, lat_err, lon_err;
  // a, i, l and o are excluded from the geohash alphabet.
  EXPECT_FALSE(GeohashDecode("ezsa2", &lat, &lon, &lat_err, &lon_err));
  EXPECT_FALSE(GeohashDecode("ezsi2", &lat, &lon, &lat_err, &lon_err));
  EXPECT_FALSE(GeohashDecode("", &lat, &lon, &lat_err, &lon_err));
}

TEST(Geohash, HasEightNeighboursInTheOpenOcean) {
  const std::vector<std::string> neighbours = GeohashNeighbours("u15j");
  EXPECT_EQ(8u, neighbours.size());
  const std::set<std::string> unique(neighbours.begin(), neighbours.end());
  EXPECT_EQ(neighbours.size(), unique.size());
  EXPECT_EQ(0u, unique.count("u15j")) << "a cell is not its own neighbour";

  const std::vector<std::string> all = GeohashCellAndNeighbours("u15j");
  EXPECT_EQ(9u, all.size());
  EXPECT_EQ("u15j", all.front());
}

// THE FAILURE NEIGHBOURS EXIST TO PREVENT. Two records for the same port, a few
// hundred metres apart, in different cells. Without expansion the block misses
// exactly the pair it was built to catch, and misses it silently.
TEST(Geohash, NeighboursRecoverPairsSplitByACellBoundary) {
  // Walk along a line until a boundary is crossed, then check the two cells are
  // neighbours of each other.
  const double lat = 51.9225;
  const std::string start = GeohashEncode(lat, 4.40, 5);
  bool found_boundary = false;

  for (double lon = 4.40; lon < 4.80; lon += 0.001) {
    const std::string here = GeohashEncode(lat, lon, 5);
    if (here == start) continue;
    found_boundary = true;

    const std::vector<std::string> neighbours = GeohashCellAndNeighbours(start);
    const std::set<std::string> unique(neighbours.begin(), neighbours.end());
    EXPECT_EQ(1u, unique.count(here))
        << "cells " << start << " and " << here
        << " are adjacent but the expansion does not connect them";

    // And the distance really is small - so this is a pair that must not be
    // lost, not two genuinely distant places.
    EXPECT_LT(HaversineKm(lat, 4.40, lat, lon), 30.0);
    break;
  }
  EXPECT_TRUE(found_boundary);
}

TEST(Geohash, NeighboursWrapAcrossTheAntimeridian) {
  const std::string east = GeohashEncode(0.0, 179.99, 4);
  const std::vector<std::string> neighbours = GeohashNeighbours(east);
  bool wrapped = false;
  for (const auto& n : neighbours) {
    double lat, lon, lat_err, lon_err;
    ASSERT_TRUE(GeohashDecode(n, &lat, &lon, &lat_err, &lon_err));
    if (lon < 0) wrapped = true;
  }
  EXPECT_TRUE(wrapped) << "a port either side of the date line is still a"
                          " neighbour";
}

TEST(Geohash, NearThePoleThereAreFewerNeighbours) {
  const std::vector<std::string> neighbours = GeohashNeighbours(
      GeohashEncode(89.99, 0.0, 4));
  EXPECT_LT(neighbours.size(), 8u);
  EXPECT_GT(neighbours.size(), 0u);
}

// --- haversine --------------------------------------------------------------

TEST(Haversine, MatchesKnownDistances) {
  // Rotterdam to Hamburg. About 412 km great-circle - which is a good deal more
  // than the 370 km road distance, and the discrepancy is worth keeping in mind
  // when reading any "distance between two ports" figure.
  EXPECT_NEAR(412.0, HaversineKm(51.92, 4.48, 53.55, 9.97), 10.0);
  // Rotterdam to Singapore, about 10,500 km great-circle.
  EXPECT_NEAR(10500.0, HaversineKm(51.92, 4.48, 1.29, 103.85), 150.0);
  // A degree of latitude is about 111 km anywhere.
  EXPECT_NEAR(111.2, HaversineKm(0.0, 0.0, 1.0, 0.0), 1.0);
  EXPECT_NEAR(111.2, HaversineKm(60.0, 0.0, 61.0, 0.0), 1.0);
  // A degree of longitude shrinks with latitude, which is exactly why a
  // geohash cell is not square.
  EXPECT_NEAR(111.3, HaversineKm(0.0, 0.0, 0.0, 1.0), 1.0);
  EXPECT_NEAR(55.8, HaversineKm(60.0, 0.0, 60.0, 1.0), 1.0);
}

TEST(Haversine, IsZeroAndSymmetric) {
  EXPECT_DOUBLE_EQ(0.0, HaversineKm(51.92, 4.48, 51.92, 4.48));
  EXPECT_NEAR(HaversineKm(51.92, 4.48, 53.55, 9.97),
              HaversineKm(53.55, 9.97, 51.92, 4.48), 1e-9);
}

// The reason for the haversine form rather than the spherical law of cosines:
// at short range the law of cosines loses most of its significant digits, and
// short range is the case this project cares about.
TEST(Haversine, StaysAccurateOverAFewHundredMetres) {
  // A tenth of a minute of latitude is about 185 metres.
  const double d = HaversineKm(51.9225, 4.4792, 51.9242, 4.4792);
  EXPECT_NEAR(0.189, d, 0.01);
  EXPECT_GT(d, 0.0);
}
