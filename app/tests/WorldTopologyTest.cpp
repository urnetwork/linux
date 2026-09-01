// Port of the android WorldTopologyTest.kt (7 cases) against the same
// world-110m.json asset. SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "WorldTopology.hpp"

// meson passes the source-tree assets dir; the relative candidates cover
// running the binary from the build dir or the project root by hand.
#ifndef UR_ASSETS_DIR
#define UR_ASSETS_DIR ""
#endif

namespace {

const char* kAssetName = "world-110m.json";

std::string ReadFileIfPresent(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Decoded once; empty when the asset cannot be found, which skips the cases
// rather than failing them (matching the android assumeTrue).
const urnw::WorldTopology* Topology() {
  static urnw::WorldTopology topology;
  static bool loaded = false;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const std::vector<std::string> candidates{
        std::string(UR_ASSETS_DIR) + "/" + kAssetName,
        std::string("assets/") + kAssetName,
        std::string("../assets/") + kAssetName,
        std::string("linux/app/assets/") + kAssetName,
    };
    for (const auto& candidate : candidates) {
      const std::string text = ReadFileIfPresent(candidate);
      if (!text.empty() && urnw::WorldTopology::Decode(text, topology)) {
        loaded = true;
        break;
      }
    }
  }
  return loaded ? &topology : nullptr;
}

#define REQUIRE_TOPOLOGY(name)                                              \
  const urnw::WorldTopology* name = Topology();                             \
  if (name == nullptr) {                                                    \
    std::printf("skip  %s (world-110m.json asset not found)\n", __func__);  \
    return;                                                                 \
  }

}  // namespace

UR_TEST(decodesAll177Countries) {
  REQUIRE_TOPOLOGY(world);
  UR_EXPECT_EQ(177u, world->countries.size());
  for (const auto& country : world->countries) {
    UR_EXPECT_FALSE(country.isoNumeric.empty());
  }
}

UR_TEST(knownCountriesArePresent) {
  REQUIRE_TOPOLOGY(world);
  std::set<std::string> ids;
  for (const auto& country : world->countries) ids.insert(country.isoNumeric);
  UR_EXPECT_TRUE_MSG("USA missing", ids.count("840") == 1);
  UR_EXPECT_TRUE_MSG("Australia missing", ids.count("036") == 1);
}

UR_TEST(ringsAreWellFormedPolylines) {
  REQUIRE_TOPOLOGY(world);
  int ringCount = 0;
  for (const auto& country : world->countries) {
    UR_EXPECT_FALSE(country.rings.empty());
    for (const auto& ring : country.rings) {
      ++ringCount;
      UR_EXPECT_TRUE_MSG("odd float count", ring.size() % 2 == 0);
      UR_EXPECT_TRUE_MSG("ring under 4 points", ring.size() >= 8);
    }
  }
  UR_EXPECT_TRUE(ringCount >= 100);
}

UR_TEST(coordinatesAreWithinWorldBounds) {
  REQUIRE_TOPOLOGY(world);
  for (const auto& country : world->countries) {
    for (const auto& ring : country.rings) {
      for (size_t i = 0; i < ring.size(); i += 2) {
        UR_EXPECT_TRUE(ring[i] >= -180.0001f && ring[i] <= 180.0001f);
        UR_EXPECT_TRUE(ring[i + 1] >= -90.0001f && ring[i + 1] <= 90.0001f);
      }
    }
  }
}

UR_TEST(everyRingCloses) {
  // TopoJSON polygon rings close: the first point of the first arc equals the
  // last point of the last arc; a stitching bug (dropped or duplicated shared
  // endpoints) breaks this
  REQUIRE_TOPOLOGY(world);
  for (const auto& country : world->countries) {
    for (const auto& ring : country.rings) {
      UR_EXPECT_NEAR_MSG(country.isoNumeric, ring[0], ring[ring.size() - 2], 1e-3f);
      UR_EXPECT_NEAR_MSG(country.isoNumeric, ring[1], ring[ring.size() - 1], 1e-3f);
    }
  }
}

UR_TEST(totalPointCountIsInTheExpectedBand) {
  // world-110m decodes to ~10,500 ring points; far fewer means dropped arcs,
  // far more means shared endpoints were not deduplicated
  REQUIRE_TOPOLOGY(world);
  size_t totalPoints = 0;
  for (const auto& country : world->countries) {
    for (const auto& ring : country.rings) totalPoints += ring.size() / 2;
  }
  UR_EXPECT_TRUE_MSG("total points " + std::to_string(totalPoints),
                     5000 <= totalPoints && totalPoints <= 60000);
}

UR_TEST(dequantizesKnownUsaCoordinates) {
  // independently decoded: the first ring of the USA MultiPolygon
  // (Hawaii) has 17 points and starts at (-155.541355, 19.084175)
  REQUIRE_TOPOLOGY(world);
  const urnw::CountryShape* usa = nullptr;
  for (const auto& country : world->countries) {
    if (country.isoNumeric == "840") {
      usa = &country;
      break;
    }
  }
  UR_EXPECT_TRUE(usa != nullptr);
  if (usa == nullptr) return;
  const std::vector<float>& firstRing = usa->rings.front();
  UR_EXPECT_EQ(size_t(17 * 2), firstRing.size());
  UR_EXPECT_NEAR(-155.541355f, firstRing[0], 5e-4f);
  UR_EXPECT_NEAR(19.084175f, firstRing[1], 5e-4f);
  UR_EXPECT_TRUE(std::fabs(firstRing[0] - firstRing[firstRing.size() - 2]) < 1e-3f);
}
