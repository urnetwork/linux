// Label composition, override-target selection and wheel ordering -- the pure
// logic behind the provider-locations list and the location override.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "ProviderLocationRow.hpp"

using urnw::ProviderLocationRow;

namespace {

ProviderLocationRow Row(const std::string& clientId, const std::string& city,
                        const std::string& region, const std::string& country,
                        bool hasCoordinates = true, double lat = 0, double lon = 0) {
  ProviderLocationRow row;
  row.clientId = clientId;
  row.city = city;
  row.region = region;
  row.country = country;
  row.hasLocation = !city.empty() || !region.empty() || !country.empty();
  row.hasCoordinates = hasCoordinates;
  row.lat = lat;
  row.lon = lon;
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
  // the sdk returns the list oldest-connected first, so the target is the first
  // entry that has coordinates -- entries with none are skipped, not fatal
  const std::vector<ProviderLocationRow> rows{
      Row("no-coords", "", "", "", false),
      Row("also-none", "Nowhere", "", "", false),
      Row("target", "Tokyo", "", "Japan", true, 35.6762, 139.6503),
      Row("younger", "Paris", "", "France", true, 48.8566, 2.3522),
  };
  const int index = urnw::OldestPlottableIndex(rows);
  UR_EXPECT_EQ(2, index);
  UR_EXPECT_TRUE(rows[static_cast<size_t>(index)].clientId == "target");
}

UR_TEST(oldestPlottableReportsNoneWhenNothingIsLocated) {
  UR_EXPECT_EQ(-1, urnw::OldestPlottableIndex(std::vector<ProviderLocationRow>{}));
  const std::vector<ProviderLocationRow> unlocated{Row("a", "", "", "", false)};
  UR_EXPECT_EQ(-1, urnw::OldestPlottableIndex(unlocated));
}

UR_TEST(wheelOrderIsByLongitudeWestToEastAndPlottableOnly) {
  const std::vector<ProviderLocationRow> rows{
      Row("tokyo", "Tokyo", "", "Japan", true, 35.6762, 139.6503),
      Row("unlocated", "", "", "", false),
      Row("sf", "San Francisco", "", "United States", true, 37.7749, -122.4194),
      Row("paris", "Paris", "", "France", true, 48.8566, 2.3522),
  };
  const std::vector<int> order = urnw::WheelOrderByLongitude(rows);
  UR_EXPECT_EQ(3u, order.size());
  // -122.4194 (sf) < 2.3522 (paris) < 139.6503 (tokyo); the unlocated row is absent
  UR_EXPECT_TRUE(rows[static_cast<size_t>(order[0])].clientId == "sf");
  UR_EXPECT_TRUE(rows[static_cast<size_t>(order[1])].clientId == "paris");
  UR_EXPECT_TRUE(rows[static_cast<size_t>(order[2])].clientId == "tokyo");
}
