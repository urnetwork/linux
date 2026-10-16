// Location row clicks, coalesced (RowConnectCoalescer.hpp), as Windows has
// them: a burst of clicks connects once, to the last row, 1.2 s after the last
// click; a click on the target the session is already driving connects nothing
// and drops a newer click still settling; the immediate gestures cancel a
// settling click. The host's timer and its call sites need glib and the SDK,
// so the wiring cases read their sources with the comments blanked.
// SPDX-License-Identifier: MPL-2.0
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "Health.hpp"
#include "LocationSelection.hpp"
#include "RowConnectCoalescer.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// A row's target as SdkHost has it: a location (here its name), or none for
// the best-available row.
using RowTarget = std::optional<std::string>;
using Coalescer = urnw::RowConnectCoalescer<RowTarget>;
constexpr int64_t kSettle = Coalescer::kSettleMillis;

// The fields of urnet::ConnectLocationId and urnet::ConnectLocation that
// IsTargetSelected reads, by the generated urnetwork_sdk.hpp's names and types.
struct CoalescerId {
  std::optional<std::string> client_id;
  std::optional<std::string> location_id;
  std::optional<std::string> location_group_id;
  std::optional<bool> best_available;
};

struct CoalescerLocation {
  std::optional<CoalescerId> connect_location_id;
};

std::optional<CoalescerLocation> CoalescerAt(const std::string& locationId) {
  CoalescerLocation location;
  CoalescerId id;
  id.location_id = locationId;
  location.connect_location_id = id;
  return location;
}

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadCoalescerSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  bool inString = false;
  for (size_t at = 0; at < text.size(); ++at) {
    const char c = text[at];
    if (inString) {
      if (c == '\\') {
        ++at;
      } else if (c == '"' || c == '\n') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
      while (at < text.size() && text[at] != '\n') text[at++] = ' ';
    }
  }
  return text;
}

std::string CoalescerBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Each needle occurs after the one before it.
bool CoalescerInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(RowConnectCoalescer_ABurstConnectsOnceToTheLastRow) {
  Coalescer rows;
  int64_t now = 1000;
  const char* burst[] = {"jp", "de", "fr", "us", "ca"};
  for (const char* name : burst) {
    UR_EXPECT_TRUE(rows.Offer(RowTarget(name), /*driving=*/false, now));
    // every click moves the deadline
    UR_EXPECT_EQ(now + kSettle, rows.DueAtMillis());
    UR_EXPECT_FALSE(rows.TakeDue(now + kSettle - 1).has_value());
    now += 200;
  }
  int connects = 0;
  RowTarget last;
  for (int64_t t = 1000; t <= now + 2 * kSettle; t += 50) {
    if (auto due = rows.TakeDue(t)) {
      ++connects;
      last = *due;
    }
  }
  UR_EXPECT_EQ(1, connects);
  UR_EXPECT_TRUE(last == RowTarget("ca"));
  UR_EXPECT_FALSE(rows.Pending());
}

UR_TEST(RowConnectCoalescer_AClickIsTakenOnceItHasSettled) {
  Coalescer rows;
  UR_EXPECT_TRUE(rows.Offer(RowTarget(), /*driving=*/false, 0));
  UR_EXPECT_TRUE(rows.Pending());
  UR_EXPECT_FALSE(rows.TakeDue(kSettle - 1).has_value());
  const auto due = rows.TakeDue(kSettle);
  UR_EXPECT_TRUE(due.has_value());
  // the best-available row's target is none
  UR_EXPECT_TRUE(due.has_value() && !due->has_value());
  UR_EXPECT_FALSE(rows.TakeDue(kSettle + 1).has_value());
}

// "Click away, think better of it, click back": nothing connects.
UR_TEST(RowConnectCoalescer_AClickOnTheDrivenTargetDropsTheNewerOne) {
  Coalescer rows;
  UR_EXPECT_TRUE(rows.Offer(RowTarget("de"), /*driving=*/false, 0));
  UR_EXPECT_FALSE(rows.Offer(RowTarget("jp"), /*driving=*/true, 300));
  UR_EXPECT_FALSE(rows.Pending());
  UR_EXPECT_FALSE(rows.TakeDue(10 * kSettle).has_value());
  // and alone it is simply no connect
  UR_EXPECT_FALSE(rows.Offer(RowTarget("jp"), /*driving=*/true, 0));
  UR_EXPECT_FALSE(rows.TakeDue(10 * kSettle).has_value());
}

UR_TEST(RowConnectCoalescer_CancelDropsASettlingClick) {
  Coalescer rows;
  UR_EXPECT_FALSE(rows.Cancel());
  UR_EXPECT_TRUE(rows.Offer(RowTarget("jp"), /*driving=*/false, 0));
  UR_EXPECT_TRUE(rows.Cancel());
  UR_EXPECT_FALSE(rows.TakeDue(10 * kSettle).has_value());
  UR_EXPECT_FALSE(rows.Cancel());
}

// Driving is a session up and its selection the row's target: a selection
// that survived a Disconnect is no session, so its row connects.
UR_TEST(RowConnectCoalescer_TheDrivenTargetIsTheSelection) {
  const std::optional<CoalescerLocation> none;
  const std::optional<CoalescerLocation> japan = CoalescerAt("jp");
  UR_EXPECT_TRUE(urnw::IsTargetSelected(none, none));
  UR_EXPECT_TRUE(urnw::IsTargetSelected(japan, CoalescerAt("jp")));
  UR_EXPECT_FALSE(urnw::IsTargetSelected(japan, CoalescerAt("de")));
  UR_EXPECT_FALSE(urnw::IsTargetSelected(none, japan));
  UR_EXPECT_FALSE(urnw::IsTargetSelected(japan, none));
  CoalescerLocation best;
  CoalescerId bestId;
  bestId.best_available = true;
  best.connect_location_id = bestId;
  UR_EXPECT_TRUE(urnw::IsTargetSelected(std::optional<CoalescerLocation>(best), none));
  UR_EXPECT_TRUE(urnw::IsTargetSelected(none, std::optional<CoalescerLocation>(best)));
}

// A session drives its selection while it is up and its window has not
// settled on failure, as Windows' RowClickIsCurrent has it: a click on a
// failed session's row connects again.
UR_TEST(RowConnectCoalescer_AFailedSessionDrivesNothing) {
  using urnw::health::DrivesSelection;
  using urnw::health::SdkStatus;
  using urnw::health::Signals;
  const auto session = [](SdkStatus sdk) {
    Signals s;
    s.sdk = sdk;
    s.destinationSelected = true;
    s.tunnelBound = true;
    return s;
  };
  for (const SdkStatus sdk : {SdkStatus::Connected, SdkStatus::Connecting,
                              SdkStatus::DestinationSet, SdkStatus::Unknown}) {
    UR_EXPECT_TRUE(DrivesSelection(session(sdk)));
  }
  UR_EXPECT_FALSE(DrivesSelection(session(SdkStatus::Failed)));
  Signals down = session(SdkStatus::Connected);
  down.tunnelBound = false;
  UR_EXPECT_FALSE(DrivesSelection(down));
}

// The host offers every row click with the driven test, runs the settled one
// through the window's start path, and drops a settling click on the
// immediate gestures and with itself.
UR_TEST(RowConnectCoalescer_TheHostCoalescesEveryRowClick) {
  const std::string host = ReadCoalescerSource("SdkHost.cpp");
  const std::string fromRow = CoalescerBody(host, "void SdkHost::ConnectFromRow(");
  UR_EXPECT_TRUE(CoalescerInOrder(
      fromRow, {"health::DrivesSelection(CurrentConnectReading().ToSignals(false)) &&",
                "IsTargetSelected(SelectedLocation(), location);",
                "rowConnects_.Offer(location, driving,", "return;",
                "ArmRowConnectTimer(rowConnects_.kSettleMillis);"}));
  UR_EXPECT_TRUE(fromRow.find("rowConnect_(") == std::string::npos);
  const std::string due = CoalescerBody(host, "void SdkHost::OnRowConnectDue() {");
  UR_EXPECT_TRUE(CoalescerInOrder(due, {"rowConnects_.TakeDue(", "RunRowConnect(*due);"}));
  const std::string run = CoalescerBody(host, "void SdkHost::RunRowConnect(");
  UR_EXPECT_TRUE(run.find("rowConnect_(location);") != std::string::npos);
  UR_EXPECT_TRUE(CoalescerInOrder(CoalescerBody(host, "void SdkHost::Disconnect() {"),
                                  {"CancelRowConnect(\"disconnect\");", "DisconnectNow();"}));
  UR_EXPECT_TRUE(CoalescerBody(host, "void SdkHost::Logout() {").find("CancelRowConnect(") !=
                 std::string::npos);
  UR_EXPECT_TRUE(CoalescerInOrder(CoalescerBody(host, "SdkHost::~SdkHost() {"),
                                  {"if (rowConnectTimerId_ != 0) {",
                                   "g_source_remove(rowConnectTimerId_);"}));
  const std::string window = ReadCoalescerSource("MainWindow.cpp");
  const std::string toggle =
      CoalescerBody(window, "void MainWindow::ToggleConnect(bool disconnect) {");
  UR_EXPECT_TRUE(
      CoalescerInOrder(toggle, {"host_.CancelRowConnect(\"connect press\");",
                                "StartTunnelUi(\"connect press\");"}));
}
