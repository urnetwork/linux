// What the status strip's Advanced Mode fields say (StatusStripPresentation.hpp),
// as Windows' strip has them: Session tunnel or none, Routes from the daemon's
// last report (on, on with DNS not applied, off, or off with the kill switch's
// floor armed; none while a session has no report yet), and the RPC endpoint
// only for a session. The strip and its writer need gtkmm, so the wiring cases
// read HomeShell.cpp and MainWindow.cpp with the comments blanked: every
// caption is a store lookup, the fields are fed from the session, the
// daemon's status poll and the identity, and the Advanced fields close with a
// standing "Advanced" tag and fade in and out with the mode.
// SPDX-License-Identifier: MPL-2.0
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "StatusStripPresentation.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::status_strip::RouteFacts;
using urnw::status_strip::RoutesWord;
using urnw::status_strip::RoutesWordFor;

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadStripFieldsSource(const std::string& relative) {
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

std::string StripFieldsBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool StripFieldsHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool StripFieldsInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(StatusStripPresentation_SessionIsTunnelOrNone) {
  using urnw::status_strip::SessionWord;
  using urnw::status_strip::SessionWordFor;
  UR_EXPECT_TRUE(SessionWordFor(true) == SessionWord::Tunnel);
  UR_EXPECT_TRUE(SessionWordFor(false) == SessionWord::None);
}

// All eight combinations of the daemon's three facts, with and without a
// session: the words come from the facts alone.
UR_TEST(StatusStripPresentation_RoutesFromEveryCombinationOfFacts) {
  for (const bool haveSession : {false, true}) {
    for (int bits = 0; bits < 8; ++bits) {
      RouteFacts facts;
      facts.routesInstalled = (bits & 1) != 0;
      facts.dnsApplied = (bits & 2) != 0;
      facts.killSwitchArmed = (bits & 4) != 0;
      RoutesWord expected = RoutesWord::Off;
      if (!facts.routesInstalled) {
        expected = facts.killSwitchArmed ? RoutesWord::KillSwitchArmed : RoutesWord::Off;
      } else {
        expected = facts.dnsApplied ? RoutesWord::On : RoutesWord::DnsNotApplied;
      }
      if (RoutesWordFor(haveSession, facts) != expected) {
        UR_FAIL("routes " + std::to_string(bits) + (haveSession ? " with" : " without") +
                " a session");
      }
    }
  }
}

// No report claims nothing: none while a session waits for its first poll,
// off with no session at all.
UR_TEST(StatusStripPresentation_NoReportClaimsNothing) {
  UR_EXPECT_TRUE(RoutesWordFor(true, std::nullopt) == RoutesWord::Unknown);
  UR_EXPECT_TRUE(RoutesWordFor(false, std::nullopt) == RoutesWord::Off);
}

// A host:port under "Session none" would read as a live listener.
UR_TEST(StatusStripPresentation_TheEndpointIsTheSessions) {
  using urnw::status_strip::RpcText;
  UR_EXPECT_TRUE(RpcText(true, "127.0.0.1:12025") == "127.0.0.1:12025");
  UR_EXPECT_TRUE(RpcText(false, "127.0.0.1:12025").empty());
  UR_EXPECT_TRUE(RpcText(true, "").empty());
}

// Every caption and the state field's name are store lookups, not literals.
UR_TEST(StatusStripPresentation_EveryCaptionIsLookedUp) {
  const std::string shell = ReadStripFieldsSource("HomeShell.cpp");
  for (const char* bare : {"MakeStatusField(\"Session\"", "MakeStatusField(\"Routes\"",
                           "MakeStatusField(\"RPC\"", "MakeStatusField(\"Raw\""}) {
    UR_EXPECT_TRUE_MSG(bare, !StripFieldsHas(shell, bare));
  }
  for (const char* lookup : {"T_(\"urnetwork_status\", \"URnetwork Status\")",
                             "MakeStatusField(T_(\"data\", \"Data\")",
                             "MakeStatusField(T_(\"adv_session_mode\", \"Session\")",
                             "MakeStatusField(T_(\"adv_routes\", \"Routes\")",
                             "MakeStatusField(T_(\"adv_rpc\", \"RPC\")",
                             "MakeStatusField(T_(\"adv_raw_status\", \"Raw status\")"}) {
    UR_EXPECT_TRUE_MSG(lookup, StripFieldsHas(shell, lookup));
  }
}

// The window writes the four from the session, the daemon's last status and
// the identity, keeps the status for one session only, and writes Raw as none
// when the controller says nothing. The session is the one there is to
// disconnect from: a Disconnect keeps the DeviceRemote, so a bound one is not
// a session, and the strip renders again when the session or the reply goes.
UR_TEST(StatusStripPresentation_TheWindowFeedsTheFields) {
  const std::string window = ReadStripFieldsSource("MainWindow.cpp");
  const std::string details =
      StripFieldsBody(window, "void MainWindow::ApplyStatusStripDetails() {");
  UR_EXPECT_TRUE(StripFieldsInOrder(
      details, {"shell_->SetStatusNetwork(balance_.IsGuest() || networkName.empty()",
                "const bool haveSession = health::SessionUp(reading_.ToSignals(",
                "status_strip::SessionWordFor(haveSession)",
                "status_strip::RoutesWordFor(haveSession, facts)",
                "status_strip::RpcText(haveSession, host_.RpcHostPort())"}));
  UR_EXPECT_FALSE(StripFieldsHas(details, "reading_.tunnelBound"));
  const std::string reading = StripFieldsBody(
      window, "void MainWindow::ApplyConnectReading(const ConnectReading& reading) {");
  UR_EXPECT_TRUE(StripFieldsInOrder(
      reading, {"return health::SessionUp(r.ToSignals(",
                "sessionUp(reading) != sessionUp(reading_);",
                "if (sessionChanged) ApplyStatusStripDetails();"}));
  UR_EXPECT_TRUE(StripFieldsInOrder(
      StripFieldsBody(window, "void MainWindow::ForgetDaemonStatus() {"),
      {"daemonStatus_.reset();", "ApplyStatusStripDetails();"}));
  UR_EXPECT_TRUE(
      StripFieldsHas(details, "daemonStatus_->kill_switch == ctl::KillSwitchState::Armed"));
  const std::string poll = StripFieldsBody(
      window,
      "void MainWindow::ApplyDaemonHealth(const std::optional<ctl::StatusReply>& status) {");
  // kept with a session or without one: the kill switch's floor is armed with none
  UR_EXPECT_TRUE(StripFieldsInOrder(poll, {"if (status) {", "daemonStatus_ = status;",
                                           "ApplyStatusStripDetails();", "if (!connected_) {"}));
  UR_EXPECT_TRUE(StripFieldsInOrder(
      StripFieldsBody(window, "void MainWindow::ToggleConnect(bool disconnect) {"),
      {"host_.Disconnect();", "ForgetDaemonStatus();"}));
  UR_EXPECT_TRUE(StripFieldsInOrder(
      StripFieldsBody(window, "void MainWindow::StartTunnelUi(const char* reason, const SdkHost::ConnectTarget& target) {"),
      {"ForgetDaemonStatus();", "host_.StartAndConnect(reason,"}));
  UR_EXPECT_TRUE(StripFieldsHas(
      StripFieldsBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {"),
      "ForgetDaemonStatus();"));
  const std::string stats = StripFieldsBody(window, "void MainWindow::ApplyStats(");
  UR_EXPECT_TRUE(StripFieldsHas(
      stats, "stats.connectionStatus.empty() ? Glib::ustring(T_(\"adv_none\", \"none\"))"));
  UR_EXPECT_FALSE(StripFieldsHas(stats, "SetStatusSession"));
}

// The tag closes the Advanced fields (so it drops with them), named by the
// mode and in the action blue; a flip fades them, a hard cut when animations
// are off or nothing is on screen, and a later flip cancels a pending hide.
UR_TEST(StatusStripPresentation_TheModeTagAndTheFade) {
  const std::string shell = ReadStripFieldsSource("HomeShell.cpp");
  UR_EXPECT_TRUE(StripFieldsInOrder(
      shell, {"appendAdvanced(rawField_, rawSeparator_);",
              "advancedFields_.append(*kit::MakeStatusSeparator());",
              "modeField_ = kit::MakeStatusField({}, false, T_(\"adv_advanced_mode\", "
              "\"Advanced mode\"));",
              "kit::SetStatusFieldValue(modeField_, T_(\"advanced\", \"Advanced\"));",
              "modeField_.value->add_css_class(\"ur-status-mode\");",
              "advancedFields_.append(*modeField_.root);"}));
  UR_EXPECT_TRUE(StripFieldsHas(ReadStripFieldsSource("UrTheme.cpp"),
                                ".ur-status-value.ur-status-mode { color: #638BFC; }"));
  const std::string flip = StripFieldsBody(shell, "void HomeShell::SetAdvancedMode(bool on) {");
  UR_EXPECT_TRUE(StripFieldsHas(flip, "FadeAdvancedFields("));
  UR_EXPECT_FALSE(StripFieldsHas(flip, "advancedFields_.set_visible("));
  const std::string fade =
      StripFieldsBody(shell, "void HomeShell::FadeAdvancedFields(bool show) {");
  UR_EXPECT_TRUE(StripFieldsInOrder(
      fade, {"const uint64_t generation = ++advancedFade_;",
             "if (!motion::ShouldAnimate() || !statusStrip_.get_mapped()) {",
             "motion::AnimateValue(advancedFields_, 0, kAdvancedFadeInMs,",
             "motion::AnimateValue(", "kAdvancedFadeOutMs",
             "if (generation != advancedFade_) return;", "advancedFields_.set_visible(false);"}));
}
