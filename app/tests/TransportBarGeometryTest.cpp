// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <vector>

#include "TransportBarGeometry.hpp"

using urnw::transportbar::LerpBoundaries;
using urnw::transportbar::Segment;
using urnw::transportbar::SegmentsFor;

namespace {

// the SDK's boundaries for 62% h3, 30% h1, 8% p2p (h3, h1, dns, dnspump, p2p,
// unknown in the stable order): each value is the running sum
const std::vector<double> kSixtyThirtyEight = {0.62, 0.92, 0.92, 0.92, 1.0, 1.0};

double TiledWidth(const std::vector<Segment>& segments) {
  double covered = 0;
  for (const auto& segment : segments) covered += segment.end - segment.start;
  return covered;
}

}  // namespace

// Boundaries interpolate element-wise; a missing element counts as zero, so
// the first tween grows every segment out of an empty bar.
UR_TEST(TransportBarGeometry_LerpBoundariesElementWise) {
  const std::vector<double> from = {0.5, 1.0};
  const std::vector<double> to = {0.25, 0.75, 1.0};
  const auto half = LerpBoundaries(from, to, 0.5);
  UR_EXPECT_EQ(3u, half.size());
  UR_EXPECT_NEAR(0.375, half[0], 1e-9);
  UR_EXPECT_NEAR(0.875, half[1], 1e-9);
  UR_EXPECT_NEAR(0.5, half[2], 1e-9);  // grows from the implicit zero

  const auto start = LerpBoundaries({}, kSixtyThirtyEight, 0.0);
  for (double value : start) UR_EXPECT_NEAR(0.0, value, 1e-12);
  const auto end = LerpBoundaries({}, kSixtyThirtyEight, 1.0);
  for (size_t i = 0; i < end.size(); ++i) UR_EXPECT_NEAR(kSixtyThirtyEight[i], end[i], 1e-12);
}

// Segment i spans [boundary(i-1), boundary(i)] of the width; zero-width
// segments (dns, dnspump, unknown here) draw nothing; the visible ones tile
// exactly 100% with the SDK's stable indices for the colors.
UR_TEST(TransportBarGeometry_SegmentsTileTheWidth) {
  const auto segments = SegmentsFor(kSixtyThirtyEight, 200);
  UR_EXPECT_EQ(3u, segments.size());
  UR_EXPECT_EQ(0u, segments[0].index);
  UR_EXPECT_NEAR(0, segments[0].start, 1e-9);
  UR_EXPECT_NEAR(124, segments[0].end, 1e-9);
  UR_EXPECT_EQ(1u, segments[1].index);
  UR_EXPECT_NEAR(124, segments[1].start, 1e-9);
  UR_EXPECT_NEAR(184, segments[1].end, 1e-9);
  UR_EXPECT_EQ(4u, segments[2].index);  // p2p keeps its stable-order index
  UR_EXPECT_NEAR(184, segments[2].start, 1e-9);
  UR_EXPECT_NEAR(200, segments[2].end, 1e-9);
  UR_EXPECT_NEAR(200, TiledWidth(segments), 1e-9);
  // the first visible segment has no separator; the others a full hairline
  UR_EXPECT_NEAR(0, segments[0].separatorWidth, 1e-9);
  UR_EXPECT_NEAR(1, segments[1].separatorWidth, 1e-9);
  UR_EXPECT_NEAR(1, segments[2].separatorWidth, 1e-9);
}

// Mid-tween the segments still tile the width exactly, at every frame: every
// edge is read from the one interpolated vector.
UR_TEST(TransportBarGeometry_TilesAtEveryFrame) {
  // whodis enters (h3 62 / h1 30 / p2p 8 -> h3 40 / h1 20 / whodis 30 / p2p 10)
  const std::vector<double> to = {0.40, 0.60, 0.90, 0.90, 1.0, 1.0};
  for (int step = 0; step <= 20; ++step) {
    const double t = step / 20.0;
    const auto boundaries = LerpBoundaries(kSixtyThirtyEight, to, t);
    const auto segments = SegmentsFor(boundaries, 300);
    UR_EXPECT_NEAR_MSG("frame " + urnw::testing::Str(t), 300, TiledWidth(segments), 1e-9);
    // no gaps and no overlaps between consecutive visible segments
    for (size_t i = 1; i < segments.size(); ++i) {
      UR_EXPECT_NEAR_MSG("frame " + urnw::testing::Str(t), segments[i - 1].end,
                         segments[i].start, 1e-9);
    }
  }
  // the entering segment grows between its neighbours: at t=0 whodis (index 2)
  // is absent, at t=1 it is present, and its separator eases in with its width
  const auto early = SegmentsFor(LerpBoundaries(kSixtyThirtyEight, to, 0.005), 300);
  bool sawWhodis = false;
  for (const auto& segment : early) {
    if (segment.index == 2) {
      sawWhodis = true;
      UR_EXPECT_TRUE(segment.separatorWidth < 1.0);
      UR_EXPECT_NEAR((segment.end - segment.start) / 4, segment.separatorWidth, 1e-9);
    }
  }
  UR_EXPECT_TRUE(sawWhodis);
}

// The empty window: every boundary 0 (or an empty vector) draws nothing, and
// values outside 0..1 or running backwards can never produce a gap or overlap.
UR_TEST(TransportBarGeometry_EmptyAndClamped) {
  UR_EXPECT_TRUE(SegmentsFor({0, 0, 0, 0, 0, 0}, 200).empty());
  UR_EXPECT_TRUE(SegmentsFor({}, 200).empty());
  UR_EXPECT_TRUE(SegmentsFor(kSixtyThirtyEight, 0).empty());
  const auto clamped = SegmentsFor({0.5, 0.25, 1.5}, 100);
  UR_EXPECT_EQ(2u, clamped.size());
  UR_EXPECT_NEAR(50, clamped[0].end, 1e-9);
  UR_EXPECT_NEAR(50, clamped[1].start, 1e-9);  // the backwards value drew nothing
  UR_EXPECT_NEAR(100, clamped[1].end, 1e-9);   // and 1.5 clamps to the width
}
