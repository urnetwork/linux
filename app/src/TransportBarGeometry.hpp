// The transport bar's geometry math, kept free of GTK and the SDK so it is
// unit-testable anywhere a C++17 compiler runs (tests/TransportBarGeometryTest.cpp).
//
// The bar draws the SDK's TransportDistribution: one segment per transport
// with traffic in the stats window, in the SDK's stable order, sized from the
// SDK's cumulative `Boundary` values (the right edge of each segment as a
// fraction of the bar width). The bar animates ONE vector of boundaries -- every
// segment edge is read from the same interpolated vector -- so the segments
// tile exactly 100% of the width at every frame of the tween, and a transport
// entering or leaving grows or shrinks between its neighbours without any
// neighbour jumping (the apple TransportSegments/AnimatableVector rendering).
// Nothing here recomputes shares, percents or order: those are the SDK's.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace urnw::transportbar {

// Element-wise interpolation of two boundary vectors at eased progress t
// (0..1). Vectors of different lengths combine element-wise with a missing
// element counting as zero (the apple AnimatableVector semantics), so the
// very first tween grows every segment out of an empty bar.
inline std::vector<double> LerpBoundaries(const std::vector<double>& from,
                                          const std::vector<double>& to, double t) {
  const size_t count = std::max(from.size(), to.size());
  std::vector<double> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const double a = i < from.size() ? from[i] : 0.0;
    const double b = i < to.size() ? to[i] : 0.0;
    out.push_back(a + (b - a) * t);
  }
  return out;
}

// One visible segment: the boundary index it belongs to (= the share index in
// the SDK's stable order, which selects the color), its pixel edges, and the
// width of the hairline separator to draw between it and the previous visible
// segment (0 for the first visible segment).
struct Segment {
  size_t index = 0;
  double start = 0;
  double end = 0;
  double separatorWidth = 0;
};

// The visible segments over `width` px for one vector of cumulative
// boundaries: segment i spans [boundary(i-1), boundary(i)] of the width, and
// zero-width segments (a transport with no traffic, or one still tweening in
// from zero) draw nothing. Boundaries are clamped to 0..1 and never run
// backwards, so the segments always tile the width without gaps or overlaps.
// The separator hairline (1 px) eases in with the narrower of the two
// neighbouring segments -- min(1, width / 4) -- so a segment sliding in from
// zero width does not pop a full separator.
inline std::vector<Segment> SegmentsFor(const std::vector<double>& boundaries, double width) {
  std::vector<Segment> segments;
  if (width <= 0) return segments;
  double start = 0;
  bool havePrevious = false;
  for (size_t i = 0; i < boundaries.size(); ++i) {
    const double end = width * std::clamp(boundaries[i], 0.0, 1.0);
    const double segmentWidth = end - start;
    if (0 < segmentWidth) {
      Segment segment;
      segment.index = i;
      segment.start = start;
      segment.end = end;
      segment.separatorWidth = havePrevious ? std::min(1.0, segmentWidth / 4) : 0.0;
      segments.push_back(segment);
      havePrevious = true;
    }
    start = std::max(start, end);
  }
  return segments;
}

}  // namespace urnw::transportbar
