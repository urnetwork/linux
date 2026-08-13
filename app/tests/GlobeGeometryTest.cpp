// Port of the android GlobeGeometryTest.kt (25 cases), case for case, with the
// same tolerances. SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cmath>
#include <vector>

#include "GlobeGeometry.hpp"

using urnw::GlobePoint;
using urnw::GlobeRotation;
namespace globe = urnw::globe;

namespace {

void ExpectPoint(const char* file, int line, float expectedX, float expectedY,
                 const urnw::GlobeProjection& actual) {
  if (!actual.visible) {
    ::urnw::testing::Fail(file, line, "expected a visible projection, got back-hemisphere");
    return;
  }
  if (std::fabs(expectedX - actual.point.x) > 1e-3f) {
    ::urnw::testing::Fail(file, line, "x: expected " + ::urnw::testing::Str(expectedX) + ", got " +
                                          ::urnw::testing::Str(actual.point.x));
  }
  if (std::fabs(expectedY - actual.point.y) > 1e-3f) {
    ::urnw::testing::Fail(file, line, "y: expected " + ::urnw::testing::Str(expectedY) + ", got " +
                                          ::urnw::testing::Str(actual.point.y));
  }
}

#define EXPECT_POINT(x, y, actual) ExpectPoint(__FILE__, __LINE__, (x), (y), (actual))

double DistanceToCenter(const GlobePoint& point) {
  return std::hypot(static_cast<double>(point.x) - globe::kCenter,
                    static_cast<double>(point.y) - globe::kCenter);
}

bool AllEqual(const std::vector<float>& line, size_t offset) {
  for (size_t i = offset + 2; i < line.size(); i += 2) {
    if (std::fabs(line[i] - line[offset]) > 1e-4f) return false;
  }
  return true;
}

}  // namespace

UR_TEST(projectsCardinalPointsAtIdentityRotation) {
  // orthographic at rotation (0, 0), scale 300: the view center (0, 0) lands at
  // (300, 300); (90, 0) is the right limb, (0, 90) the top
  EXPECT_POINT(300.f, 300.f, globe::Project(0.f, 0.f, 0.f, 0.f, 300.f));
  EXPECT_POINT(600.f, 300.f, globe::Project(90.f, 0.f, 0.f, 0.f, 300.f));
  EXPECT_POINT(0.f, 300.f, globe::Project(-90.f, 0.f, 0.f, 0.f, 300.f));
  EXPECT_POINT(300.f, 0.f, globe::Project(0.f, 90.f, 0.f, 0.f, 300.f));
  EXPECT_POINT(300.f, 600.f, globe::Project(0.f, -90.f, 0.f, 0.f, 300.f));
}

UR_TEST(backHemisphereProjectsToNotVisible) {
  // the antipode of the view center: cos(angle to center) = -1
  UR_EXPECT_FALSE(globe::Project(180.f, 0.f, 0.f, 0.f, 300.f).visible);
  UR_EXPECT_FALSE(globe::Project(0.f, 0.f, 180.f, 0.f, 300.f).visible);
  UR_EXPECT_NEAR(-1.f, globe::CosAngleToCenter(180.f, 0.f, 0.f, 0.f), 1e-6f);
  UR_EXPECT_NEAR(1.f, globe::CosAngleToCenter(0.f, 0.f, 0.f, 0.f), 1e-6f);
}

UR_TEST(rotationCenteringLandsThePointAtScreenCenter) {
  const GlobeRotation rotation = globe::RotationCentering(-122.4f, 37.8f);
  UR_EXPECT_NEAR(122.4f, rotation.lambda, 0.f);
  UR_EXPECT_NEAR(-37.8f, rotation.phi, 0.f);

  EXPECT_POINT(300.f, 300.f,
               globe::Project(-122.4f, 37.8f, rotation.lambda, rotation.phi, 300.f));
  UR_EXPECT_NEAR(1.f, globe::CosAngleToCenter(-122.4f, 37.8f, rotation.lambda, rotation.phi),
                 1e-6f);
}

UR_TEST(projectClampedMatchesProjectOnTheVisibleHemisphere) {
  const urnw::GlobeProjection projected = globe::Project(30.f, 40.f, 10.f, -20.f, 300.f);
  const GlobePoint clamped = globe::ProjectClamped(30.f, 40.f, 10.f, -20.f, 300.f);
  UR_EXPECT_TRUE(projected.visible);
  UR_EXPECT_NEAR(projected.point.x, clamped.x, 1e-4f);
  UR_EXPECT_NEAR(projected.point.y, clamped.y, 1e-4f);
}

UR_TEST(projectClampedPutsBackPointsOnTheSilhouetteCircle) {
  // the exact antipode has no azimuthal direction; clamps to (600, 300)
  const GlobePoint antipode = globe::ProjectClamped(180.f, 0.f, 0.f, 0.f, 300.f);
  UR_EXPECT_NEAR(300.0, DistanceToCenter(antipode), 1e-2);
  UR_EXPECT_NEAR(600.f, antipode.x, 1e-2f);
  UR_EXPECT_NEAR(300.f, antipode.y, 1e-2f);

  // (135, 45) at rotation (0, 0): rotated vector is
  // x = cos(135) cos(45) = -0.5 (behind), y = sin(135) cos(45) = 0.5,
  // z = sin(45) = 0.7071068, so the azimuthal direction (y, z) normalized by
  // sqrt(0.5^2 + 0.7071068^2) = 0.8660254 gives
  // px = 300 + 300 * 0.5 / 0.8660254 = 473.205
  // py = 300 - 300 * 0.7071068 / 0.8660254 = 55.051
  const GlobePoint back = globe::ProjectClamped(135.f, 45.f, 0.f, 0.f, 300.f);
  UR_EXPECT_NEAR(300.0, DistanceToCenter(back), 1e-2);
  UR_EXPECT_NEAR(473.205f, back.x, 1e-2f);
  UR_EXPECT_NEAR(55.051f, back.y, 1e-2f);
}

UR_TEST(lerpRotationTakesTheShortWayAroundTheDateLine) {
  // 170 -> -170 is 20 degrees through the date line, not 340 back; the midpoint
  // is the date line itself (180 and -180 are the same)
  const GlobeRotation mid = globe::LerpRotation(GlobeRotation{170.f, 0.f},
                                                GlobeRotation{-170.f, 0.f}, 0.5f);
  UR_EXPECT_NEAR(180.f, std::fabs(mid.lambda), 1e-4f);
  UR_EXPECT_NEAR(0.f, mid.phi, 0.f);
}

UR_TEST(lerpRotationTreatsLongitudesModulo360) {
  // 350 is -10: from 10 the short way is backward 20 degrees
  const GlobeRotation mid = globe::LerpRotation(GlobeRotation{10.f, 0.f},
                                                GlobeRotation{350.f, 0.f}, 0.5f);
  UR_EXPECT_NEAR(0.f, mid.lambda, 1e-4f);

  const GlobeRotation end = globe::LerpRotation(GlobeRotation{10.f, 0.f},
                                                GlobeRotation{350.f, 0.f}, 1.f);
  UR_EXPECT_NEAR(-10.f, end.lambda, 1e-4f);

  const GlobeRotation start = globe::LerpRotation(GlobeRotation{10.f, 20.f},
                                                  GlobeRotation{350.f, -40.f}, 0.f);
  UR_EXPECT_NEAR(10.f, start.lambda, 0.f);
  UR_EXPECT_NEAR(20.f, start.phi, 0.f);
}

UR_TEST(lerpRotationInterpolatesAndClampsPhi) {
  const GlobeRotation mid = globe::LerpRotation(GlobeRotation{0.f, -30.f},
                                                GlobeRotation{0.f, 50.f}, 0.5f);
  UR_EXPECT_NEAR(10.f, mid.phi, 1e-4f);

  // phi never leaves [-90, 90] even for out-of-range endpoints
  const GlobeRotation clamped = globe::LerpRotation(GlobeRotation{0.f, 80.f},
                                                    GlobeRotation{0.f, 120.f}, 1.f);
  UR_EXPECT_NEAR(90.f, clamped.phi, 0.f);
}

UR_TEST(dragSensitivityMatchesTheWebFormula) {
  // Globe.jsx: k = width / projection.scale() / (3 * Math.PI), applied to
  // projection.rotate() which takes degrees. At scale 300:
  // 600 / 300 / (3 * pi) = 2 / (3 * pi) = 0.2122066 degrees per px.
  UR_EXPECT_NEAR(0.2122066f, globe::DragDegreesPerVirtualPx(300.f), 1e-4f);
  // doubling the zoom halves the sensitivity
  UR_EXPECT_NEAR(globe::DragDegreesPerVirtualPx(300.f) / 2.f,
                 globe::DragDegreesPerVirtualPx(600.f), 1e-6f);
}

UR_TEST(graticuleHasTheD3DefaultLineStructure) {
  const auto& lines = globe::Graticule();
  UR_EXPECT_EQ(53u, lines.size());

  int meridians = 0;
  int parallels = 0;
  for (const auto& line : lines) {
    UR_EXPECT_TRUE(line.size() >= 4);
    UR_EXPECT_EQ(0u, line.size() % 2);
    const bool constantLon = AllEqual(line, 0);
    const bool constantLat = AllEqual(line, 1);
    UR_EXPECT_TRUE_MSG("line is neither a meridian nor a parallel", constantLon || constantLat);
    if (constantLon) {
      ++meridians;
    } else {
      ++parallels;
    }
  }
  // 4 major meridians (-180, -90, 0, 90) + 32 minor (every 10 degrees skipping
  // multiples of 90); the equator + 16 minor parallels
  UR_EXPECT_EQ(36, meridians);
  UR_EXPECT_EQ(17, parallels);
}

UR_TEST(graticuleStaysInWorldBoundsWithD3Extents) {
  int fullMeridians = 0;
  int minorMeridians = 0;
  for (const auto& line : globe::Graticule()) {
    float minLat = 90.f;
    float maxLat = -90.f;
    for (size_t i = 0; i < line.size(); i += 2) {
      UR_EXPECT_TRUE(line[i] >= -180.0001f && line[i] <= 180.0001f);
      UR_EXPECT_TRUE(line[i + 1] >= -90.0001f && line[i + 1] <= 90.0001f);
      minLat = std::min(minLat, line[i + 1]);
      maxLat = std::max(maxLat, line[i + 1]);
    }
    if (AllEqual(line, 0)) {
      // major meridians run pole to pole, minor ones stop at 80
      if (maxLat > 85.f) {
        ++fullMeridians;
      } else {
        ++minorMeridians;
        UR_EXPECT_NEAR(80.f, maxLat, 1e-3f);
        UR_EXPECT_NEAR(-80.f, minLat, 1e-3f);
      }
    } else {
      // parallels span the full longitude range
      UR_EXPECT_NEAR(-180.f, line[0], 1e-3f);
      UR_EXPECT_NEAR(180.f, line[line.size() - 2], 1e-3f);
    }
  }
  UR_EXPECT_EQ(4, fullMeridians);
  UR_EXPECT_EQ(32, minorMeridians);
}

UR_TEST(graticuleIsSampledEvery2Point5Degrees) {
  for (const auto& line : globe::Graticule()) {
    const size_t varyingOffset = AllEqual(line, 0) ? 1 : 0;
    float maxStep = 0.f;
    for (size_t i = varyingOffset + 2; i < line.size(); i += 2) {
      const float step = line[i] - line[i - 2];
      // monotone, never a gap wider than the 2.5 degree precision (the final
      // segment may be shorter where the span is not an exact multiple)
      UR_EXPECT_TRUE(step >= -1e-4f);
      UR_EXPECT_TRUE(step <= 2.5f + 1e-3f);
      maxStep = std::max(maxStep, step);
    }
    UR_EXPECT_NEAR(2.5f, maxStep, 1e-3f);
  }
}

UR_TEST(nearestWithinPicksTheClosestPointInRange) {
  const std::vector<GlobePoint> points{
      GlobePoint{100.f, 100.f},
      GlobePoint{200.f, 200.f},
      GlobePoint{105.f, 100.f},
  };
  UR_EXPECT_EQ(0, globe::NearestWithin(101.f, 100.f, points, 10.f));
  UR_EXPECT_EQ(2, globe::NearestWithin(104.f, 100.f, points, 10.f));
  UR_EXPECT_EQ(1, globe::NearestWithin(201.f, 199.f, points, 10.f));
}

UR_TEST(nearestWithinRespectsTheRadius) {
  const std::vector<GlobePoint> points{GlobePoint{0.f, 0.f}};
  UR_EXPECT_EQ(-1, globe::NearestWithin(300.f, 300.f, points, 5.f));
  // the radius is inclusive: distance from (3, 4) to (0, 0) is 5
  UR_EXPECT_EQ(0, globe::NearestWithin(3.f, 4.f, points, 5.f));
  UR_EXPECT_EQ(-1, globe::NearestWithin(3.f, 4.01f, points, 5.f));
  UR_EXPECT_EQ(-1, globe::NearestWithin(0.f, 0.f, std::vector<GlobePoint>{}, 100.f));
}

// The globe is a scroll wheel when providers are present: a horizontal drag
// steps the selection once it passes the hysteresis threshold.

UR_TEST(aDragShorterThanTheThresholdDoesNotStep) {
  const urnw::WheelStep step = globe::ResolveWheelStep(-49.f, 50.f);
  UR_EXPECT_EQ(0, step.steps);
  // the travel is carried, so continuing the same drag still steps
  UR_EXPECT_NEAR(-49.f, step.remainingTravel, 1e-4f);
}

UR_TEST(swipingLeftAdvancesAndSwipingRightGoesBack) {
  UR_EXPECT_EQ(1, globe::ResolveWheelStep(-50.f, 50.f).steps);
  UR_EXPECT_EQ(-1, globe::ResolveWheelStep(50.f, 50.f).steps);
}

UR_TEST(aFastDragCrossesSeveralStepsAtOnce) {
  const urnw::WheelStep step = globe::ResolveWheelStep(-170.f, 50.f);
  UR_EXPECT_EQ(3, step.steps);
  // 20px of the drag is left over toward the next step
  UR_EXPECT_NEAR(-20.f, step.remainingTravel, 1e-4f);
}

// the hysteresis: after stepping, another full threshold is required, so a
// pointer resting at the boundary cannot flicker between two providers
UR_TEST(steppingConsumesExactlyOneThresholdOfTravel) {
  float travel = -50.f;
  const urnw::WheelStep first = globe::ResolveWheelStep(travel, 50.f);
  UR_EXPECT_EQ(1, first.steps);
  UR_EXPECT_NEAR(0.f, first.remainingTravel, 1e-4f);

  // jitter back and forth around the boundary must not step again
  travel = first.remainingTravel;
  for (const float jitter : {-20.f, 15.f, -18.f, 12.f}) {
    travel += jitter;
    const urnw::WheelStep step = globe::ResolveWheelStep(travel, 50.f);
    UR_EXPECT_EQ(0, step.steps);
    travel = step.remainingTravel;
  }
}

UR_TEST(aNonPositiveThresholdNeverSteps) {
  UR_EXPECT_EQ(0, globe::ResolveWheelStep(-1000.f, 0.f).steps);
}

// The wheel order and the step clamping are the SDK's
// ProviderLocationsViewController (provider_locations_view_controller.go,
// tested there); this module only converts drag travel to step counts.

// fit center: the globe scales to the smaller canvas dimension and centers in
// both, so a wide (non-square) box neither crops nor offsets it
UR_TEST(unitFitsTheSmallerDimension) {
  UR_EXPECT_NEAR(1.f, globe::UnitFor(600.f, 600.f), 1e-4f);
  // 800x600 -> fits the 600 height
  UR_EXPECT_NEAR(1.f, globe::UnitFor(800.f, 600.f), 1e-4f);
  // 600x450 (the 0.75 height ratio) -> fits the 450 height
  UR_EXPECT_NEAR(0.75f, globe::UnitFor(600.f, 450.f), 1e-4f);
}

UR_TEST(virtualCenterMapsToTheCanvasCenterOfAWideBox) {
  const GlobePoint center =
      globe::ToCanvas(GlobePoint{globe::kCenter, globe::kCenter}, 800.f, 600.f);
  UR_EXPECT_NEAR(400.f, center.x, 1e-3f);
  UR_EXPECT_NEAR(300.f, center.y, 1e-3f);
}

UR_TEST(theGlobeEdgesStayInsideAWideBox) {
  const float width = 800.f;
  const float height = 600.f;
  // the extreme points of the virtual space (the sphere's bounding box)
  const GlobePoint left = globe::ToCanvas(GlobePoint{0.f, globe::kCenter}, width, height);
  const GlobePoint right =
      globe::ToCanvas(GlobePoint{globe::kVirtualSize, globe::kCenter}, width, height);
  const GlobePoint top = globe::ToCanvas(GlobePoint{globe::kCenter, 0.f}, width, height);
  const GlobePoint bottom =
      globe::ToCanvas(GlobePoint{globe::kCenter, globe::kVirtualSize}, width, height);
  // fits the height exactly, and is inset horizontally (centered)
  UR_EXPECT_NEAR(0.f, top.y, 1e-3f);
  UR_EXPECT_NEAR(height, bottom.y, 1e-3f);
  UR_EXPECT_NEAR(100.f, left.x, 1e-3f);
  UR_EXPECT_NEAR(700.f, right.x, 1e-3f);
  UR_EXPECT_NEAR(width / 2.f - left.x, right.x - width / 2.f, 1e-3f);
}

UR_TEST(toVirtualInvertsToCanvas) {
  const std::vector<std::pair<float, float>> boxes{{600.f, 600.f}, {800.f, 600.f}, {400.f, 700.f}};
  const std::vector<GlobePoint> points{
      GlobePoint{globe::kCenter, globe::kCenter},
      GlobePoint{120.f, 480.f},
      GlobePoint{590.f, 10.f},
  };
  for (const auto& box : boxes) {
    for (const auto& point : points) {
      const GlobePoint canvas = globe::ToCanvas(point, box.first, box.second);
      const GlobePoint back = globe::ToVirtual(canvas.x, canvas.y, box.first, box.second);
      UR_EXPECT_NEAR(point.x, back.x, 1e-2f);
      UR_EXPECT_NEAR(point.y, back.y, 1e-2f);
    }
  }
}
