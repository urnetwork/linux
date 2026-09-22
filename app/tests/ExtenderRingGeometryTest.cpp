// The extender rings around a provider dot -- the pure geometry behind the
// connect canvas's dots (EXTENDER.md K2/K3).
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "ExtenderRingGeometry.hpp"

using urnw::extender::kMaxRings;
using urnw::extender::kRingStroke;
using urnw::extender::PairColors;
using urnw::extender::RingCapacity;
using urnw::extender::Rings;
using urnw::extender::RingsFor;
using urnw::extender::SplitList;

namespace {
// the connect canvas's own cell at a 16-column grid (256 / 16)
constexpr double kCell = 16.0;
// a cell with room for all three rings (256 / 8)
constexpr double kWideCell = 32.0;

std::vector<std::string> Colors(std::initializer_list<const char*> hexes) {
  return std::vector<std::string>(hexes.begin(), hexes.end());
}
}  // namespace

// A direct route carries no extender fields at all, and that must read as an
// EMPTY list: one nameless entry would draw a ring around every direct dot.
UR_TEST(ExtenderRings_EmptyFieldsAreNoExtenders) {
  UR_EXPECT_EQ(0, static_cast<int>(SplitList("").size()));
  UR_EXPECT_EQ(0, static_cast<int>(SplitList(",").size()));
  UR_EXPECT_EQ(0, static_cast<int>(SplitList("  ").size()));
  UR_EXPECT_EQ(0, static_cast<int>(PairColors("", "").size()));
  UR_EXPECT_TRUE(RingsFor(kCell, {}).empty());
  UR_EXPECT_EQ(0, RingsFor(kCell, {}).collapsedCount);
  // ...and the dot keeps the whole cell
  UR_EXPECT_NEAR(kCell / 2.0, RingsFor(kCell, {}).dotRadius, 1e-9);
}

UR_TEST(ExtenderRings_ListSplitsAndTrims) {
  const auto ips = SplitList("192.0.2.1, 2001:db8::1 ,198.51.100.7");
  UR_EXPECT_EQ(3, static_cast<int>(ips.size()));
  UR_EXPECT_TRUE(ips[0] == "192.0.2.1");
  UR_EXPECT_TRUE(ips[1] == "2001:db8::1");
  UR_EXPECT_TRUE(ips[2] == "198.51.100.7");
}

// The two SDK fields are in the same order, and the app draws the color as
// given -- these are the pinned values of EXTENDER.md K3.
UR_TEST(ExtenderRings_ColorsPairWithIpsInOrder) {
  const auto colors = PairColors("192.0.2.1,2001:db8::1", "3cdd67,dd4f3c");
  UR_EXPECT_EQ(2, static_cast<int>(colors.size()));
  UR_EXPECT_TRUE(colors[0] == "3cdd67");
  UR_EXPECT_TRUE(colors[1] == "dd4f3c");

  const auto rings = RingsFor(kWideCell, colors);
  UR_EXPECT_EQ(2, static_cast<int>(rings.rings.size()));
  UR_EXPECT_TRUE(rings.rings[0].colorHex == "3cdd67");
  UR_EXPECT_TRUE(rings.rings[1].colorHex == "dd4f3c");
}

// A short color list must not drop a live extender: the ip count decides how
// many rings there are and the missing color comes back empty for the caller
// to substitute.
UR_TEST(ExtenderRings_MissingColorStillDrawsItsRing) {
  const auto colors = PairColors("192.0.2.1,198.51.100.7", "3cdd67");
  UR_EXPECT_EQ(2, static_cast<int>(colors.size()));
  UR_EXPECT_TRUE(colors[1].empty());
  UR_EXPECT_EQ(2, static_cast<int>(RingsFor(kWideCell, colors).rings.size()));
}

// The load-bearing rule: the outermost ring's OUTER edge is the cell edge, so
// the footprint of a dot with rings equals the footprint of one without.
UR_TEST(ExtenderRings_OutermostRingTouchesTheCellEdge) {
  for (int count = 1; count <= 3; ++count) {
    std::vector<std::string> colors(static_cast<size_t>(count), "3cdd67");
    const Rings rings = RingsFor(kWideCell, colors);
    UR_EXPECT_EQ(count, static_cast<int>(rings.rings.size()));
    const double outerEdge = rings.rings[0].radius + rings.rings[0].lineWidth / 2.0;
    UR_EXPECT_NEAR_MSG("outer edge", kWideCell / 2.0, outerEdge, 1e-9);
  }
}

// 2 px stroke, 2 px between successive rings and 2 px between the dot and the
// innermost ring -- so the dot loses exactly 4 px of radius per ring.
UR_TEST(ExtenderRings_StrokeGapAndShrinkArePinned) {
  const Rings one = RingsFor(kWideCell, Colors({"aaaaaa"}));
  UR_EXPECT_NEAR(2.0, one.rings[0].lineWidth, 1e-9);
  UR_EXPECT_NEAR(kWideCell / 2.0 - 4.0, one.dotRadius, 1e-9);

  const Rings three = RingsFor(kWideCell, Colors({"aaaaaa", "bbbbbb", "cccccc"}));
  UR_EXPECT_NEAR(kWideCell / 2.0 - 12.0, three.dotRadius, 1e-9);
  for (size_t i = 0; i < three.rings.size(); ++i) {
    const double outer = three.rings[i].radius + kRingStroke / 2.0;
    const double inner = three.rings[i].radius - kRingStroke / 2.0;
    UR_EXPECT_NEAR_MSG("outer edge", kWideCell / 2.0 - 4.0 * static_cast<double>(i), outer, 1e-9);
    // the gap below this ring: the next ring's outer edge, or the dot
    const double below = i + 1 < three.rings.size()
                             ? three.rings[i + 1].radius + kRingStroke / 2.0
                             : three.dotRadius;
    UR_EXPECT_NEAR_MSG("gap", 2.0, inner - below, 1e-9);
  }
}

// Four or more extenders collapse into a DASHED third ring; three or fewer
// are all solid.
UR_TEST(ExtenderRings_FourOrMoreCollapseIntoADashedThirdRing) {
  const Rings three = RingsFor(kWideCell, Colors({"a1a1a1", "b2b2b2", "c3c3c3"}));
  UR_EXPECT_EQ(3, static_cast<int>(three.rings.size()));
  UR_EXPECT_FALSE(three.rings[2].dashed);
  UR_EXPECT_EQ(0, three.collapsedCount);

  const Rings five = RingsFor(kWideCell, Colors({"a1a1a1", "b2b2b2", "c3c3c3", "d4d4d4", "e5e5e5"}));
  UR_EXPECT_EQ(kMaxRings, static_cast<int>(five.rings.size()));
  UR_EXPECT_FALSE(five.rings[0].dashed);
  UR_EXPECT_FALSE(five.rings[1].dashed);
  UR_EXPECT_TRUE(five.rings[2].dashed);
  UR_EXPECT_EQ(2, five.collapsedCount);
  // the collapsed ring wears the THIRD extender's color and the dot has still
  // shrunk by exactly three rings, never by five
  UR_EXPECT_TRUE(five.rings[2].colorHex == "c3c3c3");
  UR_EXPECT_NEAR(kWideCell / 2.0 - 12.0, five.dotRadius, 1e-9);
}

// The first ip keeps the outermost radius as extenders come and go: nothing
// about ring 0 moves when a second or third extender appears.
UR_TEST(ExtenderRings_FirstIpKeepsTheOutermostRing) {
  const Rings one = RingsFor(kWideCell, Colors({"a1a1a1"}));
  const Rings two = RingsFor(kWideCell, Colors({"a1a1a1", "b2b2b2"}));
  UR_EXPECT_NEAR(one.rings[0].radius, two.rings[0].radius, 1e-9);
  UR_EXPECT_TRUE(two.rings[0].colorHex == "a1a1a1");
  UR_EXPECT_TRUE(two.rings[1].colorHex == "b2b2b2");
}

// A small cell takes the rings it has room for rather than eating the dot.
UR_TEST(ExtenderRings_SmallCellsDropTheRingsThatDoNotFit) {
  UR_EXPECT_EQ(3, RingCapacity(32.0));
  UR_EXPECT_EQ(2, RingCapacity(24.0));
  UR_EXPECT_EQ(1, RingCapacity(16.0));
  UR_EXPECT_EQ(0, RingCapacity(8.0));
  UR_EXPECT_EQ(0, RingCapacity(0.0));
  UR_EXPECT_EQ(0, RingCapacity(-4.0));

  const Rings tight = RingsFor(kCell, Colors({"a1a1a1", "b2b2b2", "c3c3c3", "d4d4d4"}));
  UR_EXPECT_EQ(1, static_cast<int>(tight.rings.size()));
  // only a full three-ring set collapses: a lone ring is never dashed, since
  // there was no room to show a count in the first place
  UR_EXPECT_FALSE(tight.rings[0].dashed);
  UR_EXPECT_EQ(0, tight.collapsedCount);
  UR_EXPECT_NEAR(kCell / 2.0 - 4.0, tight.dotRadius, 1e-9);
}

// No cell at all (a grid that has not arrived) draws nothing rather than a
// negative-radius arc.
UR_TEST(ExtenderRings_DegenerateCellDrawsNothing) {
  const Rings none = RingsFor(0.0, Colors({"a1a1a1"}));
  UR_EXPECT_TRUE(none.empty());
  UR_EXPECT_NEAR(0.0, none.dotRadius, 1e-9);
  const Rings negative = RingsFor(-12.0, Colors({"a1a1a1"}));
  UR_EXPECT_TRUE(negative.empty());
}

// Whatever the input, the dot survives: a ring is dropped before the fill is.
UR_TEST(ExtenderRings_DotRadiusIsAlwaysPositive) {
  for (double cell = 1.0; cell <= 64.0; cell += 0.5) {
    const Rings rings = RingsFor(cell, Colors({"a", "b", "c", "d", "e", "f"}));
    UR_EXPECT_TRUE_MSG("dot radius", rings.dotRadius > 0.0);
    UR_EXPECT_TRUE_MSG("ring count", rings.rings.size() <= static_cast<size_t>(kMaxRings));
    for (const auto& ring : rings.rings) {
      UR_EXPECT_TRUE_MSG("ring radius", ring.radius > 0.0);
    }
  }
}
