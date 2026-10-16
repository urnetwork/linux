// Where the degrade hold (Health.hpp DegradeHold) runs, as Windows has its
// one Tracker: SdkHost folds it over every reading it hands out, so the
// Connect page, the status strip and the tray read one verdict; it reads
// again when a running hold ends, because nothing else may; and every
// deliberate connect or disconnect starts it over. The Connect page shows the
// held line under the status and the SDK's reason for a stall, and offers
// Retry when the attempt failed; the tray's connected icon means proven while
// its item follows the session and its tooltip names the state.
// SdkHost, ConnectPage, MainWindow and the tray need glib, gtkmm and the SDK,
// so this reads their sources with the comments blanked.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadHealthWiringSource(const std::string& relative) {
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

std::string HealthWiringBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool HealthWiringHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool HealthWiringInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// Every reading is the facts with the hold folded over them, and a running
// hold arms one timeout that hands a fresh reading on when it ends.
UR_TEST(ConnectionHealthWiring_EveryReadingCarriesTheHold) {
  const std::string host = ReadHealthWiringSource("SdkHost.cpp");
  const std::string reading =
      HealthWiringBody(host, "ConnectReading SdkHost::ReadConnectReading() {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      reading, {"ConnectReading r = ReadConnectFacts();", "std::scoped_lock lock(degradeMutex_);",
                "degradeHold_.Update(sessionUp, r.sdk == health::SdkStatus::Connected, nowMillis);",
                "degradeHold_.ReevalAtMillis();", "g_timeout_add(",
                "self->onReading_(self->CurrentConnectReading());", "return r;"}));
  // the verdict is part of the reading: compared, and handed to the table
  const std::string header = ReadHealthWiringSource("SdkHost.hpp");
  UR_EXPECT_TRUE(HealthWiringHas(header, "proofLoss == o.proofLoss"));
  UR_EXPECT_TRUE(HealthWiringHas(header, "s.proofLoss = proofLoss;"));
  // the timeout holds `this`, so it goes with the host
  UR_EXPECT_TRUE(HealthWiringInOrder(HealthWiringBody(host, "SdkHost::~SdkHost() {"),
                                     {"std::scoped_lock degradeLock(degradeMutex_);",
                                      "g_source_remove(degradeReevalId_);"}));
}

// A deliberate connect or disconnect starts the hold over, after the balance
// gate let it through.
UR_TEST(ConnectionHealthWiring_EveryGestureStartsANewAttempt) {
  const std::string host = ReadHealthWiringSource("SdkHost.cpp");
  // the gate on the caller's thread, then the queued command, whose body
  // starts the new attempt before it takes the lock
  for (const char* signature : {"void SdkHost::ConnectBestAvailable() {",
                                "void SdkHost::Connect(const std::optional"}) {
    UR_EXPECT_TRUE_MSG(signature, HealthWiringInOrder(HealthWiringBody(host, signature),
                                                      {"connectGate_(", "RunCommand("}));
  }
  for (const char* signature : {"void SdkHost::ConnectBestAvailableNow() {",
                                "void SdkHost::ConnectNow(const std::optional"}) {
    UR_EXPECT_TRUE_MSG(signature, HealthWiringInOrder(HealthWiringBody(host, signature),
                                                      {"NoteNewConnectAttempt();",
                                                       "std::scoped_lock lock(mutex_);"}));
  }
  UR_EXPECT_TRUE(HealthWiringInOrder(HealthWiringBody(host, "void SdkHost::DisconnectNow() {"),
                                     {"NoteNewConnectAttempt();", "control_.StopTunnel();"}));
  UR_EXPECT_TRUE(HealthWiringHas(HealthWiringBody(host, "void SdkHost::NoteNewConnectAttempt() {"),
                                 "degradeHold_.NoteNewAttempt();"));
}

// The page writes the held line from the same reading as the status row, and
// claims the floor only on the daemon's word.
UR_TEST(ConnectionHealthWiring_ThePageShowsTheHeldLine) {
  const std::string page = ReadHealthWiringSource("ConnectPage.cpp");
  const std::string status = HealthWiringBody(page, "void ConnectPage::ApplyConnectStatus() {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      status, {"const health::Reading view = health::Render(signals);",
               "if (health::TrafficHeld(view, signals)) {", "host_.CurrentKillSwitchStatus();",
               "killSwitch.installed_known &&",
               "killSwitch.installed == ctl::KillSwitchState::Connected",
               "held = T_(line.key, line.english);",
               "kit::SetTextOrCollapse(*trafficHeldText_, held);"}));
}

// The window pushes the tray the session, the proof and the state's words
// from the one reading, a session with no status observed keeping its own
// claim, and the tray shows each where it belongs.
UR_TEST(ConnectionHealthWiring_TheTrayIconMeansProven) {
  const std::string window = ReadHealthWiringSource("MainWindow.cpp");
  const std::string reading = HealthWiringBody(
      window, "void MainWindow::ApplyConnectReading(const ConnectReading& reading) {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      reading,
      {"const health::Reading view = health::Render(signals);",
       "connected_ = view.action != health::Action::Connect;",
       "health::TrayReading(",
       "view, signals, connect_tap::StatusObserved(reading.statusObserved, pendingTap_));",
       "const bool proven = health::Proven(tray);",
       "const std::string status = T_(tray.textKey, tray.textEnglish);",
       "on_tray_state(connected_, proven, status, signedIn_);"}));
  // observed means the presentation is open or the session latched a status
  const std::string host = ReadHealthWiringSource("SdkHost.cpp");
  const std::string facts = HealthWiringBody(host, "ConnectReading SdkHost::ReadConnectFacts() {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      facts, {"r.statusObserved = connectVc_.has_value();", "return r;",
              "r.statusObserved = connectVc_.has_value() || "
              "r.sdk != health::SdkStatus::Unknown;"}));
  UR_EXPECT_TRUE(HealthWiringHas(ReadHealthWiringSource("SdkHost.hpp"),
                                 "statusObserved == o.statusObserved"));
  UR_EXPECT_TRUE(HealthWiringHas(ReadHealthWiringSource("main.cpp"),
                                 "if (tray) tray->SetState(sessionUp, proven, status, hasStatus);"));
  const std::string tray = ReadHealthWiringSource("Tray.cpp");
  UR_EXPECT_TRUE(HealthWiringHas(tray, "self->provenForIcon() ? \"urnetwork-tray-connected\""));
  UR_EXPECT_TRUE(HealthWiringHas(tray, "ConnectLabel(self->sessionUp())"));
  UR_EXPECT_TRUE(HealthWiringHas(tray, "<property name=\"ToolTip\" type=\"(sa(iiay)ss)\""));
  UR_EXPECT_TRUE(HealthWiringHas(tray, "self->statusForToolTip().c_str()"));
  UR_EXPECT_TRUE(HealthWiringInOrder(
      HealthWiringBody(tray, "void Tray::SetState(bool sessionUp, bool proven, "
                             "const std::string& status, bool hasStatus) {"),
      {"\"NewIcon\"", "\"NewToolTip\"", "EmitMenuLayoutUpdated();"}));
  UR_EXPECT_TRUE(HealthWiringHas(HealthWiringBody(tray, "void Tray::EmitMenuLayoutUpdated() {"),
                                 "\"LayoutUpdated\""));
}

// The window's diagnosis is read only while it can matter, the page renders it
// under the status from the same reading, and a Retry press stops the failed
// session and connects to the selection it had, read before the stop.
UR_TEST(ConnectionHealthWiring_TheStallReasonAndRetry) {
  const std::string host = ReadHealthWiringSource("SdkHost.cpp");
  const std::string facts = HealthWiringBody(host, "ConnectReading SdkHost::ReadConnectFacts() {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      facts, {"if (!sessionUp) {", "return r;",
              "if (connectVc_ && r.sdk != health::SdkStatus::Connected) {",
              "device_->getWindowStatus()", "r.stallReason = windowStatus->StallReason;"}));
  UR_EXPECT_TRUE(HealthWiringHas(ReadHealthWiringSource("SdkHost.hpp"),
                                 "stallReason == o.stallReason;"));
  const std::string page = ReadHealthWiringSource("ConnectPage.cpp");
  const std::string status = HealthWiringBody(page, "void ConnectPage::ApplyConnectStatus() {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      status, {"health::ReasonLineFor(view.state, reading_.stallReason);",
               "kit::SetTextOrCollapse(*statusReasonText_,",
               "actionIsRetry_ = view.action == health::Action::Retry;",
               "connectBtn_->set_label(actionIsRetry_ ? T_(\"retry\", \"Retry\")"}));
  const std::string relay = HealthWiringBody(page, "void ConnectPage::RelayConnectPress() {");
  UR_EXPECT_TRUE(HealthWiringInOrder(
      relay, {"if (actionIsRetry_) {", "disconnectRequestedAtUs_ = 0;", "on_retry_connect();",
              "return;", "const bool disconnect = actionIsDisconnect_;"}));
  const std::string window = ReadHealthWiringSource("MainWindow.cpp");
  UR_EXPECT_TRUE(
      HealthWiringHas(window, "connectPage_->on_retry_connect = [this] { RetryConnect(); };"));
  UR_EXPECT_TRUE(HealthWiringInOrder(
      HealthWiringBody(window, "void MainWindow::RetryConnect() {"),
      {"host_.Disconnect();", "StartTunnelUi(\"retry\", SdkHost::ConnectTarget{});"}));
}
