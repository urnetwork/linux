// SPDX-License-Identifier: MPL-2.0
#include "ProviderLocationRow.hpp"

#include <cstdio>

namespace urnw {

std::string PlaceLabel(const ProviderLocationRow& row) {
  std::string label;
  for (const std::string* part : {&row.city, &row.region, &row.country}) {
    if (part->empty()) continue;
    if (!label.empty()) label += ", ";
    label += *part;
  }
  return label;
}

std::string CoordinatesLabel(const ProviderLocationRow& row) {
  if (!row.hasCoordinates) return "\xE2\x80\x94";  // em dash
  char buf[64];
  // always the C locale's '.' decimal point, matching the other apps
  std::snprintf(buf, sizeof(buf), "%.4f, %.4f", row.lat, row.lon);
  return buf;
}

int OldestPlottableIndex(const std::vector<ProviderLocationRow>& rows) {
  // Searched by STAMP, not by position: the rows arrive in the view
  // controller's display order (west to east), which says nothing about how
  // long anything has been connected. A zero stamp is "unknown" -- the sdk
  // sorts those last, so one only wins when nothing else is plottable.
  int best = -1;
  int64_t bestSince = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (!rows[i].plottable()) continue;
    const int64_t since = rows[i].connectedSinceMillis;
    const bool better = best < 0 ||                        // nothing yet
                        (bestSince == 0 && since != 0) ||  // any stamp beats none
                        (bestSince != 0 && since != 0 && since < bestSince);
    if (better) {
      best = static_cast<int>(i);
      bestSince = since;
    }
  }
  return best;
}

}  // namespace urnw
