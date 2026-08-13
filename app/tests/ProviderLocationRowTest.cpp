// Label composition and override-target selection -- the pure logic behind the
// provider-locations list and the location override.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "ProviderLocationRow.hpp"

using urnw::ProviderLocationRow;

namespace {

ProviderLocationRow Row(const std::string& clientId, const std::string& city,
                        const std::string& region, const std::string& country,
                        bool hasCoordinates = true, double lat = 0, double lon = 0,
                        int64_t connectedSinceMillis = 0) {
  ProviderLocationRow row;
  row.clientId = clientId;
  row.city = city;
  row.region = region;
  row.country = country;
  row.hasLocation = !city.empty() || !region.empty() || !country.empty();
  row.hasCoordinates = hasCoordinates;
  row.lat = lat;
  row.lon = lon;
  row.connectedSinceMillis = connectedSinceMillis;
  return row;
}

}  // namespace

UR_TEST(placeLabelJoinsTheKnownPartsCityFirst) {
  UR_EXPECT_TRUE(urnw::PlaceLabel(Row("a", "Tokyo", "Tokyo", "Japan")) == "Tokyo, Tokyo, Japan");
  // whichever parts the server does not know are simply omitted
  UR_EXPECT_TRUE(urnw::PlaceLabel(Row("a", "", "California", "United States")) ==
                 "California, United States");
  UR_EXPECT_TRUE(urnw::PlaceLabel(Row("a", "Paris", "", "France")) == "Paris, France");
  UR_EXPECT_TRUE(urnw::PlaceLabel(Row("a", "", "", "Brazil")) == "Brazil");
}

UR_TEST(placeLabelIsEmptyWhenNothingIsKnown) {
  // the caller substitutes the localized "Location unknown"
  UR_EXPECT_TRUE(urnw::PlaceLabel(Row("a", "", "", "")).empty());
}

UR_TEST(coordinatesLabelUses4DecimalPlaces) {
  UR_EXPECT_TRUE(urnw::CoordinatesLabel(Row("a", "", "", "", true, 37.7749295, -122.4194155)) ==
                 "37.7749, -122.4194");
  // a legitimate zero coordinate still renders as a number, not an em dash
  UR_EXPECT_TRUE(urnw::CoordinatesLabel(Row("a", "", "", "", true, 0, 0)) == "0.0000, 0.0000");
}

UR_TEST(coordinatesLabelIsAnEmDashWithoutCoordinates) {
  UR_EXPECT_TRUE(urnw::CoordinatesLabel(Row("a", "Somewhere", "", "", false)) == "\xE2\x80\x94");
}

UR_TEST(oldestPlottableSkipsProvidersWithNoCoordinates) {
  const std::vector<ProviderLocationRow> rows{
      Row("no-coords", "", "", "", false, 0, 0, 1000),
      Row("also-none", "Nowhere", "", "", false, 0, 0, 2000),
      Row("target", "Tokyo", "", "Japan", true, 35.6762, 139.6503, 3000),
      Row("younger", "Paris", "", "France", true, 48.8566, 2.3522, 4000),
  };
  const int index = urnw::OldestPlottableIndex(rows);
  UR_EXPECT_EQ(2, index);
  UR_EXPECT_TRUE(rows[static_cast<size_t>(index)].clientId == "target");
}

// The rows arrive in DISPLAY order (west to east), which says nothing about how
// long anything has been connected, so the target is found by stamp rather than
// by position -- the oldest provider can be anywhere in the list.
UR_TEST(oldestPlottableIsFoundByStampNotByPosition) {
  const std::vector<ProviderLocationRow> rows{
      Row("west-but-newest", "Los Angeles", "", "United States", true, 34.05, -118.24, 9000),
      Row("target", "Tokyo", "", "Japan", true, 35.6762, 139.6503, 1000),
      Row("east-and-middling", "Sydney", "", "Australia", true, -33.87, 151.21, 5000),
  };
  const int index = urnw::OldestPlottableIndex(rows);
  UR_EXPECT_EQ(1, index);
  UR_EXPECT_TRUE(rows[static_cast<size_t>(index)].clientId == "target");
}

// An unknown stamp (0, an older device peer) sorts LAST in the sdk, so it only
// wins when nothing else can be plotted.
UR_TEST(oldestPlottablePrefersAKnownStampOverAnUnknownOne) {
  const std::vector<ProviderLocationRow> mixed{
      Row("unknown-stamp", "Tokyo", "", "Japan", true, 35.6762, 139.6503, 0),
      Row("target", "Paris", "", "France", true, 48.8566, 2.3522, 7000),
  };
  UR_EXPECT_EQ(1, urnw::OldestPlottableIndex(mixed));

  // with only unknown stamps the first plottable row is as good as any
  const std::vector<ProviderLocationRow> allUnknown{
      Row("no-coords", "", "", "", false, 0, 0, 0),
      Row("first", "Tokyo", "", "Japan", true, 35.6762, 139.6503, 0),
      Row("second", "Paris", "", "France", true, 48.8566, 2.3522, 0),
  };
  UR_EXPECT_EQ(1, urnw::OldestPlottableIndex(allUnknown));
}

UR_TEST(oldestPlottableReportsNoneWhenNothingIsLocated) {
  UR_EXPECT_EQ(-1, urnw::OldestPlottableIndex(std::vector<ProviderLocationRow>{}));
  const std::vector<ProviderLocationRow> unlocated{Row("a", "", "", "", false)};
  UR_EXPECT_EQ(-1, urnw::OldestPlottableIndex(unlocated));
}

// The list's display order and the globe's clamped stepping over it are the
// SDK's ProviderLocationsViewController (provider_locations_view_controller.go,
// tested there), so there is nothing to order here.
