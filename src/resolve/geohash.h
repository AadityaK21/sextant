// Geohash: turning a coordinate into a blocking key.
//
// WHY A GEOHASH RATHER THAN A DISTANCE QUERY
//
// The scorer wants to know whether two records sit within a few kilometres of
// each other. Answering that directly means comparing every pair, which is the
// O(n^2) problem blocking exists to avoid. A geohash converts proximity into a
// *string prefix*, so "records near this one" becomes a lookup rather than a
// scan - the same trick the TIDX keyspace plays with time.
//
// The encoding interleaves bits of longitude and latitude, longitude first, and
// renders them in base32. Each additional character adds five bits and narrows
// the cell:
//
//     4 chars   +/- 20 km      cell about 39 x 19.5 km
//     5 chars   +/- 2.4 km     cell about 4.9 x 4.9 km
//     6 chars   +/- 0.61 km
//
// THE BOUNDARY PROBLEM, WHICH IS THE WHOLE REASON FOR NEIGHBOURS
//
// Two records for the same port can sit 200 metres apart and still land in
// different cells, because a cell boundary does not care what is near it. A
// naive geohash block therefore misses exactly the pairs it was built to
// catch, and misses them silently. Expanding to the eight surrounding cells
// costs 9x the block lookups and removes the failure mode.
//
// Precision 4 is used for ports, which is generous - a 39 km cell plus
// neighbours reaches over 100 km. That is deliberate. UN/LOCODE coordinates are
// degree-and-whole-minute, so they are already only accurate to about 1.8 km,
// and a large port's records can legitimately be 20 km apart: the World Port
// Index entry for Rotterdam and the one for Rotterdam Botlek describe the same
// port complex from different points. Blocking wants recall; the haversine
// feature in the scorer is where distance gets judged.

#pragma once

#include <string>
#include <vector>

namespace sextant::resolve {

// Base32 geohash. `precision` is the number of characters.
std::string GeohashEncode(double lat, double lon, int precision);

// Decode back to the centre of the cell, with the half-height and half-width of
// the cell as an error bound. Used by the tests to prove the encoding is not
// merely self-consistent.
bool GeohashDecode(const std::string& hash, double* lat, double* lon,
                   double* lat_err, double* lon_err);

// The eight cells surrounding `hash`, at the same precision. Cells at the
// poles or across the antimeridian have fewer than eight distinct neighbours,
// so the result may be shorter.
std::vector<std::string> GeohashNeighbours(const std::string& hash);

// The cell plus its eight neighbours, which is what a blocking lookup actually
// wants.
std::vector<std::string> GeohashCellAndNeighbours(const std::string& hash);

// Great-circle distance in kilometres. Lives here because it is the metric the
// geohash approximates, and the tests check one against the other.
double HaversineKm(double lat1, double lon1, double lat2, double lon2);

}  // namespace sextant::resolve
