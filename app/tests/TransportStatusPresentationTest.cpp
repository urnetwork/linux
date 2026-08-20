// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "TransportStatusPresentation.hpp"

using urnw::kTransportConstraintMemory;
using urnw::TransportStatusDecorations;
using urnw::TransportStatusPresentation;

namespace {

const std::vector<std::string> kDefaultAuto = {"h3", "h1", "dns", "dnspump"};

// the matrix defaults: Auto, a matching applied policy, a degraded memory
// status with only h1 eligible
TransportStatusPresentation Compute(bool isAuto = true, bool draftMatches = true,
                                    const std::vector<std::string>& autoModes = kDefaultAuto,
                                    bool statusKnown = true, bool degraded = true,
                                    const std::vector<std::string>& eligible = {"h1"},
                                    const std::string& constraint = kTransportConstraintMemory) {
  return TransportStatusDecorations(isAuto, draftMatches, autoModes, statusKnown, degraded,
                                    eligible, constraint);
}

}  // namespace

// A healthy status decorates nothing.
UR_TEST(TransportStatusPresentation_HealthyShowsNoDecorations) {
  const auto presentation = Compute(true, true, kDefaultAuto, true, /*degraded=*/false,
                                    kDefaultAuto, "");
  UR_EXPECT_TRUE(presentation == TransportStatusPresentation{});
}

// Degraded: banner + a warning on every enabled-but-ineligible carrier.
UR_TEST(TransportStatusPresentation_DegradedMarksEnabledIneligible) {
  const auto presentation = Compute();
  UR_EXPECT_TRUE(presentation.showBanner);
  UR_EXPECT_TRUE(presentation.memoryConstraint);
  const std::set<std::string> expected = {"h3", "dns", "dnspump"};
  UR_EXPECT_TRUE(presentation.constrainedModes == expected);
}

// An Auto-disabled mode absent from eligibility is never marked.
UR_TEST(TransportStatusPresentation_AutoDisabledNeverConstrained) {
  const auto presentation = Compute(true, true, {"h1", "dns", "dnspump"});
  UR_EXPECT_TRUE(presentation.showBanner);
  const std::set<std::string> expected = {"dns", "dnspump"};
  UR_EXPECT_TRUE(presentation.constrainedModes == expected);
}

// Status applies to Auto only: an explicit mode hides everything.
UR_TEST(TransportStatusPresentation_ExplicitModeHidesStatus) {
  UR_EXPECT_TRUE(Compute(/*isAuto=*/false) == TransportStatusPresentation{});
}

// Unknown status is neither healthy nor constrained: no decorations.
UR_TEST(TransportStatusPresentation_UnknownStatusShowsNothing) {
  UR_EXPECT_TRUE(Compute(true, true, kDefaultAuto, /*statusKnown=*/false) ==
                 TransportStatusPresentation{});
}

// A dirty draft hides the decorations until it matches the applied policy the
// status was computed for.
UR_TEST(TransportStatusPresentation_DirtyDraftHidesStatus) {
  UR_EXPECT_TRUE(Compute(true, /*draftMatches=*/false) == TransportStatusPresentation{});
  UR_EXPECT_TRUE(Compute(true, /*draftMatches=*/true).showBanner);
}

// An unknown constraint keeps the banner but uses the generic copy.
UR_TEST(TransportStatusPresentation_UnknownConstraintUsesGenericCopy) {
  const auto presentation = Compute(true, true, kDefaultAuto, true, true, {"h1"}, "quantum");
  UR_EXPECT_TRUE(presentation.showBanner);
  UR_EXPECT_TRUE(!presentation.memoryConstraint);
}

// The authoritative degraded flag renders the banner even when the eligible
// list carries only vocabulary this app does not know.
UR_TEST(TransportStatusPresentation_UnknownVocabularyKeepsBanner) {
  const auto presentation = Compute(true, true, kDefaultAuto, true, true, {"warp9"});
  UR_EXPECT_TRUE(presentation.showBanner);
  const std::set<std::string> expected = {"h3", "h1", "dns", "dnspump"};
  UR_EXPECT_TRUE(presentation.constrainedModes == expected);
}
