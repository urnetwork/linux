// SPDX-License-Identifier: MPL-2.0
#include "ProviderLocationRow.hpp"

#include <algorithm>
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
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].plottable()) return static_cast<int>(i);
  }
  return -1;
}

std::vector<int> WheelOrderByLongitude(const std::vector<ProviderLocationRow>& rows) {
  std::vector<int> order;
  order.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].plottable()) order.push_back(static_cast<int>(i));
  }
  // stable so providers sharing a longitude keep their duration order
  std::stable_sort(order.begin(), order.end(), [&rows](int a, int b) {
    return rows[static_cast<size_t>(a)].lon < rows[static_cast<size_t>(b)].lon;
  });
  return order;
}

}  // namespace urnw
