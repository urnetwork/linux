// The ip family status row's counting, ranking and line selection -- the
// pure logic behind the drawer's Dualstack / IPv4 / IPv6 columns. The same
// cases as the apple IpFamilyStatusRowTests.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "IpFamilyStatus.hpp"

using urnw::ipfamily::Column;
using urnw::ipfamily::ColumnStatus;
using urnw::ipfamily::kColumnCount;
using urnw::ipfamily::Line;
using urnw::ipfamily::LineKind;
using urnw::ipfamily::LinesFor;
using urnw::ipfamily::Point;
using urnw::ipfamily::ReducePoints;
using urnw::ipfamily::Statuses;
using urnw::ipfamily::StatusesFor;
using urnw::ipfamily::Tier;
using urnw::ipfamily::TiersFor;

namespace {

Point P(const char* family, const char* state = "Added") { return Point{state, family}; }

ColumnStatus S(Column column, int connected = 0, int connecting = 0) {
  return ColumnStatus{column, connected, connecting};
}

bool AllUnavailable(const Statuses& statuses) {
  for (const auto& status : statuses) {
    if (!status.unavailable()) return false;
  }
  return true;
}

}  // namespace

// ---- counting ---------------------------------------------------------------

UR_TEST(IpFamilyStatus_ColumnsAreAlwaysPresentInDisplayOrder) {
  const Statuses statuses = StatusesFor({});
  UR_EXPECT_EQ(kColumnCount, static_cast<int>(statuses.size()));
  UR_EXPECT_TRUE(statuses[0].column == Column::Dualstack);
  UR_EXPECT_TRUE(statuses[1].column == Column::V4);
  UR_EXPECT_TRUE(statuses[2].column == Column::V6);
  UR_EXPECT_TRUE(AllUnavailable(statuses));
}

// The columns are categories, not capabilities: a dualstack provider counts
// once, under Dualstack, never under IPv4 or IPv6 as well.
UR_TEST(IpFamilyStatus_CountsConnectedAndConnectingByCategory) {
  const Statuses statuses = StatusesFor({
      P("dualstack"),
      P("dualstack"),
      P("dualstack", "InEvaluation"),
      P("v4-only"),
      P("v6-only", "InEvaluation"),
      P("v6-only", "InEvaluation"),
  });
  UR_EXPECT_TRUE(statuses[0] == S(Column::Dualstack, 2, 1));
  UR_EXPECT_TRUE(statuses[1] == S(Column::V4, 1));
  UR_EXPECT_TRUE(statuses[2] == S(Column::V6, 0, 2));
}

// A provider that failed evaluation, was not added, or is on its way out (it
// lingers on the grid for the removal tween) counts as nothing.
UR_TEST(IpFamilyStatus_IgnoresProvidersThatAreNotLive) {
  const Statuses statuses = StatusesFor({
      P("dualstack", "EvaluationFailed"),
      P("v4-only", "NotAdded"),
      P("v6-only", "Removed"),
      P("v6-only", "something-newer"),
  });
  UR_EXPECT_TRUE(AllUnavailable(statuses));
}

// A legacy or unknown category carries v4, so it is an IPv4 provider rather
// than one that vanishes from the row.
UR_TEST(IpFamilyStatus_LegacyAndUnknownCategoriesReadAsV4) {
  const Statuses statuses = StatusesFor({
      P(""),
      P("something-newer", "InEvaluation"),
  });
  UR_EXPECT_TRUE(statuses[1] == S(Column::V4, 1, 1));
}

// The widget reduces the SDK's grid points to their state and category; a
// stand-in with the SDK's field names proves the reduction.
UR_TEST(IpFamilyStatus_StatusesFromSdkGridPoints) {
  struct GridPoint {
    std::string State;
    std::string IpFamily;
  };
  const std::vector<GridPoint> points = {
      {"Added", "v6-only"},
      {"InEvaluation", "dualstack"},
  };
  const Statuses statuses = StatusesFor(ReducePoints(points));
  UR_EXPECT_TRUE(statuses[0] == S(Column::Dualstack, 0, 1));
  UR_EXPECT_TRUE(statuses[2] == S(Column::V6, 1));
}

// ---- ranking ----------------------------------------------------------------

UR_TEST(IpFamilyStatus_DualstackConnectedIsBestAndTheOthersAreDimmed) {
  const auto tiers = TiersFor({S(Column::Dualstack, 1), S(Column::V4, 3), S(Column::V6, 0, 1)});
  UR_EXPECT_TRUE(tiers[0] == Tier::Best);
  UR_EXPECT_TRUE(tiers[1] == Tier::Active);
  UR_EXPECT_TRUE(tiers[2] == Tier::Active);
}

// IPv4 and IPv6 tie, so with nothing dualstack connected they share the top.
UR_TEST(IpFamilyStatus_V4AndV6ShareBestWhenNothingDualstackIsConnected) {
  const auto tiers = TiersFor({S(Column::Dualstack), S(Column::V4, 2), S(Column::V6, 1)});
  UR_EXPECT_TRUE(tiers[0] == Tier::Unavailable);
  UR_EXPECT_TRUE(tiers[1] == Tier::Best);
  UR_EXPECT_TRUE(tiers[2] == Tier::Best);
}

UR_TEST(IpFamilyStatus_ALoneConnectedColumnIsBest) {
  const auto tiers = TiersFor({S(Column::Dualstack), S(Column::V4), S(Column::V6, 1)});
  UR_EXPECT_TRUE(tiers[0] == Tier::Unavailable);
  UR_EXPECT_TRUE(tiers[1] == Tier::Unavailable);
  UR_EXPECT_TRUE(tiers[2] == Tier::Best);
}

// A column that is only connecting carries no traffic yet: it is active, never
// best, even when it outranks the connected column.
UR_TEST(IpFamilyStatus_AConnectingOnlyColumnIsActiveNotBest) {
  const auto tiers = TiersFor({S(Column::Dualstack, 0, 2), S(Column::V4, 1), S(Column::V6)});
  UR_EXPECT_TRUE(tiers[0] == Tier::Active);
  UR_EXPECT_TRUE(tiers[1] == Tier::Best);
  UR_EXPECT_TRUE(tiers[2] == Tier::Unavailable);
}

UR_TEST(IpFamilyStatus_OnlyConnectingColumnsMakeNothingBest) {
  const auto tiers = TiersFor({S(Column::Dualstack, 0, 1), S(Column::V4, 0, 1), S(Column::V6)});
  UR_EXPECT_TRUE(tiers[0] == Tier::Active);
  UR_EXPECT_TRUE(tiers[1] == Tier::Active);
  UR_EXPECT_TRUE(tiers[2] == Tier::Unavailable);
}

UR_TEST(IpFamilyStatus_NothingLiveMakesEveryColumnUnavailable) {
  const auto tiers = TiersFor(StatusesFor({}));
  UR_EXPECT_EQ(kColumnCount, static_cast<int>(tiers.size()));
  for (const auto tier : tiers) UR_EXPECT_TRUE(tier == Tier::Unavailable);
}

// ---- lines ------------------------------------------------------------------

UR_TEST(IpFamilyStatus_LinesShowTheNonZeroCountsConnectedFirst) {
  const std::vector<Line> both = {Line{LineKind::Connected, 3}, Line{LineKind::Connecting, 1}};
  UR_EXPECT_TRUE(LinesFor(S(Column::V4, 3, 1)) == both);
  const std::vector<Line> connected = {Line{LineKind::Connected, 3}};
  UR_EXPECT_TRUE(LinesFor(S(Column::V4, 3)) == connected);
  const std::vector<Line> connecting = {Line{LineKind::Connecting, 1}};
  UR_EXPECT_TRUE(LinesFor(S(Column::V4, 0, 1)) == connecting);
}

UR_TEST(IpFamilyStatus_AColumnWithNothingReadsDisconnected) {
  const std::vector<Line> disconnected = {Line{LineKind::Disconnected, 0}};
  UR_EXPECT_TRUE(LinesFor(S(Column::V6)) == disconnected);
}

// A line is its kind: a count change updates the same line in place (its
// text changes) while a kind change is a different line.
UR_TEST(IpFamilyStatus_LineIdentityIsTheKind) {
  const Line one{LineKind::Connected, 1};
  const Line two{LineKind::Connected, 2};
  const Line connecting{LineKind::Connecting, 1};
  UR_EXPECT_TRUE(one.kind == two.kind);
  UR_EXPECT_TRUE(one.kind != connecting.kind);
  UR_EXPECT_TRUE(one != two);
}
