// One connected provider as rendered by the globe and the provider-locations
// list, plus the pure label/selection helpers over it.
//
// Toolkit- and SDK-independent (plain C++17) so the ordering, the label
// composition and the override-target selection are unit testable standalone --
// see tests/ProviderLocationRowTest.cpp. The sheet maps
// urnet::ConnectedProviderLocation onto this; nothing here knows about the SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace urnw {

struct ProviderLocationRow {
  std::string clientId;  // the EGRESS provider client id (what is displayed/copied)
  std::string country;
  std::string countryCode;  // lowercase; feeds the SDK color palette
  std::string region;
  std::string city;
  bool hasLocation = false;
  // the coordinates to plot: the city centroid when known, else the region
  // centroid. hasCoordinates is false when the provider has neither.
  bool hasCoordinates = false;
  double lat = 0;
  double lon = 0;
  int64_t connectedSinceMillis = 0;

  // Providers with no coordinates are listed but never plotted.
  bool plottable() const { return hasCoordinates; }

  bool operator==(const ProviderLocationRow& other) const {
    return clientId == other.clientId && country == other.country &&
           countryCode == other.countryCode && region == other.region && city == other.city &&
           hasLocation == other.hasLocation && hasCoordinates == other.hasCoordinates &&
           lat == other.lat && lon == other.lon &&
           connectedSinceMillis == other.connectedSinceMillis;
  }
  bool operator!=(const ProviderLocationRow& other) const { return !(*this == other); }
};

// "City, Region, Country" -- omitting whichever parts the server does not know.
// Empty when nothing is known; the caller substitutes the localized
// "Location unknown".
std::string PlaceLabel(const ProviderLocationRow& row);

// "37.7749, -122.4194" at 4 decimal places, or an em dash when the provider has
// no coordinates.
std::string CoordinatesLabel(const ProviderLocationRow& row);

// The oldest connected provider that has coordinates -- the location-override
// target. The SDK returns the list already sorted oldest-connected first, so
// this is the first plottable entry; providers with neither city nor region
// coordinates are skipped rather than ending the search. Returns -1 when there
// is none.
int OldestPlottableIndex(const std::vector<ProviderLocationRow>& rows);

// The wheel order: by longitude, west to east, independent of the list's
// duration order. Returns indexes into `rows`, plottable entries only.
std::vector<int> WheelOrderByLongitude(const std::vector<ProviderLocationRow>& rows);

}  // namespace urnw
