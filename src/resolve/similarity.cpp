#include "similarity.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace sextant::resolve {

double Jaro(const std::string& a, const std::string& b) {
  if (a.empty() && b.empty()) return 1.0;
  if (a.empty() || b.empty()) return 0.0;
  if (a == b) return 1.0;

  // Two characters match if they are equal and no further apart than half the
  // longer string. The minus one matters: without it the window is one
  // character too wide and short strings score higher than they should.
  const size_t window =
      std::max<size_t>(std::max(a.size(), b.size()) / 2, 1) - 1;

  std::vector<bool> a_matched(a.size(), false);
  std::vector<bool> b_matched(b.size(), false);

  size_t matches = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const size_t lo = i > window ? i - window : 0;
    const size_t hi = std::min(i + window + 1, b.size());
    for (size_t j = lo; j < hi; ++j) {
      if (b_matched[j] || a[i] != b[j]) continue;
      a_matched[i] = true;
      b_matched[j] = true;
      ++matches;
      break;
    }
  }
  if (matches == 0) return 0.0;

  // A transposition is a pair of matched characters that appear in a different
  // order in the two strings. Counted as half each, which is what makes
  // "MARTHA" and "MARHTA" cost less than a substitution would.
  size_t transpositions = 0;
  size_t k = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!a_matched[i]) continue;
    while (!b_matched[k]) ++k;
    if (a[i] != b[k]) ++transpositions;
    ++k;
  }

  const double m = static_cast<double>(matches);
  return (m / static_cast<double>(a.size()) +
          m / static_cast<double>(b.size()) +
          (m - static_cast<double>(transpositions) / 2.0) / m) /
         3.0;
}

double JaroWinkler(const std::string& a, const std::string& b,
                   double prefix_weight) {
  const double jaro = Jaro(a, b);
  // The bonus is only applied to pairs that are already fairly similar. Without
  // that floor, two unrelated names sharing a first letter get pulled upwards
  // for no good reason.
  if (jaro < 0.7) return jaro;

  size_t prefix = 0;
  const size_t limit = std::min<size_t>({a.size(), b.size(), 4});
  while (prefix < limit && a[prefix] == b[prefix]) ++prefix;

  return jaro + static_cast<double>(prefix) * prefix_weight * (1.0 - jaro);
}

double TokenJaccard(const std::vector<std::string>& a,
                    const std::vector<std::string>& b) {
  if (a.empty() && b.empty()) return 1.0;
  if (a.empty() || b.empty()) return 0.0;

  const std::unordered_set<std::string> set_a(a.begin(), a.end());
  const std::unordered_set<std::string> set_b(b.begin(), b.end());

  size_t intersection = 0;
  for (const auto& token : set_a) {
    if (set_b.count(token) != 0) ++intersection;
  }
  const size_t union_size = set_a.size() + set_b.size() - intersection;
  if (union_size == 0) return 0.0;
  return static_cast<double>(intersection) / static_cast<double>(union_size);
}

bool TokensContained(const std::vector<std::string>& a,
                     const std::vector<std::string>& b) {
  if (a.empty() || b.empty()) return false;
  const std::vector<std::string>& shorter = a.size() <= b.size() ? a : b;
  const std::vector<std::string>& longer = a.size() <= b.size() ? b : a;

  const std::unordered_set<std::string> set(longer.begin(), longer.end());
  for (const auto& token : shorter) {
    if (set.count(token) == 0) return false;
  }
  return true;
}

}  // namespace sextant::resolve
