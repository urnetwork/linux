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
// target. Found by the smallest non-zero connectedSinceMillis, NOT by position:
// these rows come in the view controller's display order (west to east), which
// says nothing about connected duration. Providers with neither city nor region
// coordinates are skipped rather than ending the search, and a provider with an
// unknown stamp (0) only wins when nothing else is plottable. Returns -1 when
// there is none.
int OldestPlottableIndex(const std::vector<ProviderLocationRow>& rows);

// The list's order (the plottable providers west to east about their centroid,
// then the ones with no coordinates) and the globe's clamped stepping over it
// are NOT here: they live in the SDK's shared ProviderLocationsViewController,
// which every URnetwork app binds -- see SdkHost::ConnectedProviderLocations
// and SdkHost::StepProviderSelection.

}  // namespace urnw
