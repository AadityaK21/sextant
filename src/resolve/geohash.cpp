#include "geohash.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sextant::resolve {
namespace {

// The geohash alphabet. Note it is not the same base32 as ULIDs use: this one
// excludes a, i, l, o and is fixed by the geohash specification, so it cannot
// be swapped for Crockford's even though both are "base32".
constexpr char kBase32[] = "0123456789bcdefghjkmnpqrstuvwxyz";

int SymbolValue(char c) {
  for (int i = 0; i < 32; ++i) {
    if (kBase32[i] == c) return i;
  }
  return -1;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0088;  // IUGG mean radius

double ToRadians(double degrees) { return degrees * kPi / 180.0; }

}  // namespace

std::string GeohashEncode(double lat, double lon, int precision) {
  if (precision <= 0) return {};
  // Clamp rather than reject. A source that reports latitude 90.0001 has a
  // rounding problem, not a record that should vanish from every block.
  if (lat < -90.0) lat = -90.0;
  if (lat > 90.0) lat = 90.0;
  if (lon < -180.0) lon = -180.0;
  if (lon > 180.0) lon = 180.0;

  double lat_min = -90.0, lat_max = 90.0;
  double lon_min = -180.0, lon_max = 180.0;

  std::string out;
  out.reserve(static_cast<size_t>(precision));

  int bit = 0;
  int value = 0;
  bool even = true;  // longitude first

  while (static_cast<int>(out.size()) < precision) {
    // The comparison is >=, not >, so a coordinate landing exactly on a
    // division goes to the UPPER cell. Either convention is self-consistent -
    // the decoder below mirrors whichever is chosen - but this one matches the
    // published geohash values, which is what makes the reference vectors in
    // the tests an external check rather than a restatement of this code.
    // It only ever matters at exact zeros, and (0, 0) is exactly the case a
    // test would use.
    if (even) {
      const double mid = (lon_min + lon_max) / 2.0;
      if (lon >= mid) {
        value = (value << 1) | 1;
        lon_min = mid;
      } else {
        value <<= 1;
        lon_max = mid;
      }
    } else {
      const double mid = (lat_min + lat_max) / 2.0;
      if (lat >= mid) {
        value = (value << 1) | 1;
        lat_min = mid;
      } else {
        value <<= 1;
        lat_max = mid;
      }
    }
    even = !even;

    if (++bit == 5) {
      out.push_back(kBase32[value]);
      bit = 0;
      value = 0;
    }
  }
  return out;
}

bool GeohashDecode(const std::string& hash, double* lat, double* lon,
                   double* lat_err, double* lon_err) {
  if (hash.empty()) return false;

  double lat_min = -90.0, lat_max = 90.0;
  double lon_min = -180.0, lon_max = 180.0;
  bool even = true;

  for (const char c : hash) {
    const int value = SymbolValue(c);
    if (value < 0) return false;
    for (int mask = 16; mask > 0; mask >>= 1) {
      const bool high = (value & mask) != 0;
      if (even) {
        const double mid = (lon_min + lon_max) / 2.0;
        if (high) lon_min = mid; else lon_max = mid;
      } else {
        const double mid = (lat_min + lat_max) / 2.0;
        if (high) lat_min = mid; else lat_max = mid;
      }
      even = !even;
    }
  }

  *lat = (lat_min + lat_max) / 2.0;
  *lon = (lon_min + lon_max) / 2.0;
  *lat_err = (lat_max - lat_min) / 2.0;
  *lon_err = (lon_max - lon_min) / 2.0;
  return true;
}

std::vector<std::string> GeohashNeighbours(const std::string& hash) {
  std::vector<std::string> out;
  double lat, lon, lat_err, lon_err;
  if (!GeohashDecode(hash, &lat, &lon, &lat_err, &lon_err)) return out;

  const int precision = static_cast<int>(hash.size());

  // Re-encode a point one full cell away in each direction. Walking the base32
  // string directly is the textbook implementation and needs four border tables
  // and a recursive carry; going back through coordinates is a few lines,
  // exercises the encoder that everything else depends on, and runs once per
  // record at ingest rather than in any hot path.
  for (int dlat = -1; dlat <= 1; ++dlat) {
    for (int dlon = -1; dlon <= 1; ++dlon) {
      if (dlat == 0 && dlon == 0) continue;

      double nlat = lat + dlat * lat_err * 2.0;
      double nlon = lon + dlon * lon_err * 2.0;

      // Past a pole there is no neighbour in that direction.
      if (nlat > 90.0 || nlat < -90.0) continue;
      // The antimeridian does wrap, and a port either side of it is still a
      // neighbour.
      if (nlon > 180.0) nlon -= 360.0;
      if (nlon < -180.0) nlon += 360.0;

      std::string neighbour = GeohashEncode(nlat, nlon, precision);
      if (neighbour == hash) continue;  // degenerate near a pole
      bool seen = false;
      for (const auto& existing : out) {
        if (existing == neighbour) { seen = true; break; }
      }
      if (!seen) out.push_back(std::move(neighbour));
    }
  }
  return out;
}

std::vector<std::string> GeohashCellAndNeighbours(const std::string& hash) {
  std::vector<std::string> out;
  if (hash.empty()) return out;
  out.push_back(hash);
  for (auto& n : GeohashNeighbours(hash)) out.push_back(std::move(n));
  return out;
}

double HaversineKm(double lat1, double lon1, double lat2, double lon2) {
  const double phi1 = ToRadians(lat1);
  const double phi2 = ToRadians(lat2);
  const double dphi = ToRadians(lat2 - lat1);
  const double dlambda = ToRadians(lon2 - lon1);

  // The haversine form rather than the spherical law of cosines: for two points
  // a few hundred metres apart, `cos(d)` is within rounding distance of 1 and
  // the law of cosines loses most of its significant digits. Short distances
  // are exactly the case this project cares about.
  const double a = std::sin(dphi / 2.0) * std::sin(dphi / 2.0) +
                   std::cos(phi1) * std::cos(phi2) * std::sin(dlambda / 2.0) *
                       std::sin(dlambda / 2.0);
  const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return kEarthRadiusKm * c;
}

}  // namespace sextant::resolve
