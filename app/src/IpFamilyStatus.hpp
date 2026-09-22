// The ip family status row's pure logic (connect/IPV6.md D2), kept free of GTK
// and the SDK so it is unit-testable anywhere a C++17 compiler runs
// (tests/IpFamilyStatusTest.cpp). The widget in IpFamilyStatusRow.hpp only
// draws what these functions decide.
//
// The row shows the connect window's providers as three columns, Dualstack,
// IPv4 and IPv6, each the provider CATEGORY the platform proved for that exit
// (a dualstack provider counts once, under Dualstack, never as an IPv4 or an
// IPv6 provider as well). A column counts the providers connected (Added) and
// connecting (InEvaluation); a provider that failed evaluation, was not added
// or is on its way out counts as nothing.
//
// The families rank: dualstack carries both, IPv4 and IPv6 tie below it. The
// best-ranked column with a CONNECTED provider is bright (shared on a tie),
// any other column with something connected or connecting is dimmed, and a
// column with nothing is dimmed out. A column that is only connecting carries
// no traffic yet, so it is never best.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <string>
#include <vector>

namespace urnw::ipfamily {

// The SDK's provider grid point, reduced to the two fields the row reads.
struct Point {
  std::string state;     // urnet::ProviderGridPoint::State ("Added", ...)
  std::string ipFamily;  // urnet::ProviderGridPoint::IpFamily ("dualstack", ...)
};

// Reduces any grid point list whose elements carry `State` and `IpFamily`
// (the SDK's urnet::ProviderGridPoint, or a test double) to the fields above.
template <typename Points>
inline std::vector<Point> ReducePoints(const Points& points) {
  std::vector<Point> reduced;
  reduced.reserve(points.size());
  for (const auto& point : points) reduced.push_back(Point{point.State, point.IpFamily});
  return reduced;
}

// The three columns, in display order, which is also rank order.
enum class Column { Dualstack = 0, V4 = 1, V6 = 2 };
inline constexpr int kColumnCount = 3;

// The SDK's category values (sdk ip_family.go, connect ip_family.go).
inline constexpr const char* kFamilyDualstack = "dualstack";
inline constexpr const char* kFamilyV4Only = "v4-only";
inline constexpr const char* kFamilyV6Only = "v6-only";

// The grid states that count: a routing-eligible provider, and one still
// being evaluated, as the SDK spells them.
inline constexpr const char* kStateConnected = "Added";
inline constexpr const char* kStateConnecting = "InEvaluation";

// The column a category lands in. Legacy (empty) and anything this build does
// not know read as v4: that is what such a provider carries (the SDK
// normalizes legacy to v4-only before it reaches an app, and this is the same
// rule applied once more at the edge).
inline Column ColumnFor(const std::string& ipFamily) {
  if (ipFamily == kFamilyDualstack) return Column::Dualstack;
  if (ipFamily == kFamilyV6Only) return Column::V6;
  return Column::V4;
}

// The family's rank, lower is better: dualstack carries both families, and
// IPv4 and IPv6 tie below it.
inline int Rank(Column column) { return column == Column::Dualstack ? 0 : 1; }

// The emphasis of a column.
enum class Tier {
  Best,         // the best-ranked column with a connected provider, shared on a tie
  Active,       // something connected or connecting, but a better column is connected
  Unavailable,  // nothing connected or connecting
};

struct ColumnStatus {
  Column column = Column::Dualstack;
  int connected = 0;   // the Added providers in this category
  int connecting = 0;  // the InEvaluation providers in this category

  // nothing connected or connecting: the column reads "disconnected"
  bool unavailable() const { return connected == 0 && connecting == 0; }

  bool operator==(const ColumnStatus& other) const {
    return column == other.column && connected == other.connected &&
           connecting == other.connecting;
  }
  bool operator!=(const ColumnStatus& other) const { return !(*this == other); }
};

// The three columns, always present, in display order.
using Statuses = std::array<ColumnStatus, kColumnCount>;

// Counts only the connected and the connecting providers of each category.
inline Statuses StatusesFor(const std::vector<Point>& points) {
  Statuses statuses;
  for (int i = 0; i < kColumnCount; ++i) statuses[static_cast<size_t>(i)].column = static_cast<Column>(i);
  for (const auto& point : points) {
    auto& status = statuses[static_cast<size_t>(ColumnFor(point.ipFamily))];
    if (point.state == kStateConnected) {
      ++status.connected;
    } else if (point.state == kStateConnecting) {
      ++status.connecting;
    }
  }
  return statuses;
}

// The tier of every column, indexed like Statuses: the best rank among the
// columns with a connected provider is bright (every column at that rank, on a
// tie), any other column with a live provider is active, and the rest are
// unavailable.
inline std::array<Tier, kColumnCount> TiersFor(const Statuses& statuses) {
  int bestRank = -1;
  for (const auto& status : statuses) {
    if (status.connected <= 0) continue;
    const int rank = Rank(status.column);
    if (bestRank < 0 || rank < bestRank) bestRank = rank;
  }
  std::array<Tier, kColumnCount> tiers;
  for (size_t i = 0; i < statuses.size(); ++i) {
    const auto& status = statuses[i];
    if (status.unavailable()) {
      tiers[i] = Tier::Unavailable;
    } else if (0 < status.connected && Rank(status.column) == bestRank) {
      tiers[i] = Tier::Best;
    } else {
      tiers[i] = Tier::Active;
    }
  }
  return tiers;
}

// One status line of a column, in display order: connected, then connecting,
// or disconnected alone.
enum class LineKind { Connected, Connecting, Disconnected };

struct Line {
  LineKind kind = LineKind::Disconnected;
  int count = 0;  // meaningless for Disconnected

  bool operator==(const Line& other) const { return kind == other.kind && count == other.count; }
  bool operator!=(const Line& other) const { return !(*this == other); }
};

// The status lines of a column: the non-zero counts, connected first, or
// "disconnected" alone when both are zero. Never more than two.
inline std::vector<Line> LinesFor(const ColumnStatus& status) {
  std::vector<Line> lines;
  if (0 < status.connected) lines.push_back(Line{LineKind::Connected, status.connected});
  if (0 < status.connecting) lines.push_back(Line{LineKind::Connecting, status.connecting});
  if (lines.empty()) lines.push_back(Line{LineKind::Disconnected, 0});
  return lines;
}

// The most lines a column can show: the reserved height of the row.
inline constexpr int kMaxLines = 2;

}  // namespace urnw::ipfamily
