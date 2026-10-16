// A connect or disconnect tap shows its state at once, held until the command
// queue settles (ConnectTap.hpp), and the commands themselves run on SdkHost's
// queue rather than on the GTK thread (apple 8a8d0658, 852baf62).
//
// The fold is pure and read directly. Where the commands run, and that the
// GTK thread's own device reads stand back while one is outstanding, are call
// sites, read from the sources the way the other wiring tests read them.
// Before this change Connect, ConnectBestAvailable and Disconnect took
// mutex_ and wrote to the device on the caller's thread, StartTunnelUi ran the
// start on it, the page's feed poll and the drawer events read the device
// whatever was in flight, and nothing showed a connect tap until it returned.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>
#include <utility>

#include "ConnectTap.hpp"
#include "Health.hpp"

namespace {

using urnw::connect_tap::Fold;
using urnw::connect_tap::Pending;
namespace health = urnw::health;
namespace wiring = urnw::testing::wiring;

health::Signals Disconnected() { return health::Signals{}; }

health::Signals Connected() {
  health::Signals s;
  s.sdk = health::SdkStatus::Connected;
  s.destinationSelected = true;
  s.tunnelBound = true;
  s.providerCount = 4;
  return s;
}

}  // namespace

UR_TEST(connectTapShowsAConnectAtOnce) {
  // the reading is still the one from before the tap
  const health::Reading view = health::Render(Fold(Disconnected(), Pending::Connect));
  UR_EXPECT_TRUE(view.state == health::State::Connecting);
  // and the one control ends it, as a Disconnect
  UR_EXPECT_TRUE(view.action == health::Action::Disconnect);
}

UR_TEST(connectTapShowsADisconnectAtOnce) {
  const health::Reading view = health::Render(Fold(Connected(), Pending::Disconnect));
  UR_EXPECT_TRUE(view.state == health::State::Disconnecting);
}

UR_TEST(connectTapHoldsOverAReportFromBeforeIt) {
  // a connected report that predates a connect tap does not read connected,
  // and a failed one does not read failed, until the queue settles
  UR_EXPECT_TRUE(health::Render(Fold(Connected(), Pending::Connect)).state ==
                 health::State::Connecting);
  health::Signals failed = Connected();
  failed.sdk = health::SdkStatus::Failed;
  UR_EXPECT_TRUE(health::Render(Fold(failed, Pending::Connect)).state ==
                 health::State::Connecting);
}

UR_TEST(connectTapKeepsTheBalanceBlock) {
  health::Signals blocked = Disconnected();
  blocked.insufficientBalance = true;
  UR_EXPECT_TRUE(health::Render(Fold(blocked, Pending::Connect)).state ==
                 health::State::Blocked);
}

UR_TEST(connectTapWithNothingPendingIsTheReading) {
  for (const health::Signals& s : {Disconnected(), Connected()}) {
    const health::Signals folded = Fold(s, Pending::None);
    UR_EXPECT_TRUE(health::Render(folded).state == health::Render(s).state);
  }
}

UR_TEST(connectTapReadsTheSameInTheTray) {
  // the tray's reading of a pending connect is connecting too, never the
  // session's own claim of connected, with the window shown or hidden (no
  // controller, so the reading itself observed nothing)
  const health::Signals folded = Fold(Disconnected(), Pending::Connect);
  const health::Reading view = health::Render(folded);
  for (const bool readingObserved : {true, false}) {
    const health::Reading tray = health::TrayReading(
        view, folded, urnw::connect_tap::StatusObserved(readingObserved, Pending::Connect));
    UR_EXPECT_FALSE(health::Proven(tray));
    UR_EXPECT_TRUE(tray.state == health::State::Connecting);
  }
  // with nothing pending, an unobserved session keeps its own claim, as before
  UR_EXPECT_FALSE(urnw::connect_tap::StatusObserved(false, Pending::None));
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Contains(window, "connect_tap::StatusObserved(reading.statusObserved, pendingTap_)"));
}

// The three commands queue their device work, and take no lock on the
// caller's thread.
UR_TEST(connectCommandsRunOnTheQueue) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  for (const char* signature : {"void SdkHost::ConnectBestAvailable() {",
                                "void SdkHost::Connect(const std::optional<urnet::ConnectLocation>& location) {",
                                "void SdkHost::Disconnect() {"}) {
    const std::string body = wiring::FunctionBody(host, signature);
    UR_EXPECT_TRUE_MSG(signature, wiring::Contains(body, "RunCommand(ConnectCommandQueue::Kind::"));
    UR_EXPECT_FALSE(wiring::Contains(body, "scoped_lock"));
    UR_EXPECT_FALSE(wiring::Contains(body, "connectVc_->"));
    UR_EXPECT_FALSE(wiring::Contains(body, "control_."));
  }
  // the start in front of a connect is one queued connect with it
  const std::string start = wiring::FunctionBody(host, "bool SdkHost::StartAndConnect(");
  UR_EXPECT_TRUE(wiring::Before(start, "ConnectCommandQueue::Kind::Connect", "StartTunnel(reason)"));
  // and the presentation's controllers open and close behind the commands
  const std::string presentation =
      wiring::FunctionBody(host, "void SdkHost::SetPresentationActive(bool active) {");
  UR_EXPECT_TRUE(wiring::Before(presentation, "ConnectCommandQueue::Kind::Device",
                                "ClosePresentationLocked();"));
  UR_EXPECT_TRUE(wiring::Before(presentation, "ConnectCommandQueue::Kind::Device", "SubscribeStats();"));
}

// The window starts through the queue and shows the tap first.
UR_TEST(connectTapIsShownBeforeTheCommandIsQueued) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  const std::string start = wiring::FunctionBody(
      window, "void MainWindow::StartTunnelUi(const char* reason, const SdkHost::ConnectTarget& target) {");
  UR_EXPECT_TRUE(wiring::Before(start, "NoteTap(connect_tap::Pending::Connect);",
                                "host_.StartAndConnect(reason, target,"));
  // no start, and no read of the selection, on the GTK thread
  for (const char* signature :
       {"void MainWindow::StartTunnelUi(const char* reason) {",
        "void MainWindow::StartTunnelUi(const char* reason,\n",
        "void MainWindow::StartTunnelUi(const char* reason, const SdkHost::ConnectTarget& target) {",
        "void MainWindow::RetryConnect() {"}) {
    const std::string body = wiring::FunctionBody(window, signature);
    UR_EXPECT_TRUE_MSG(signature, !body.empty());
    UR_EXPECT_FALSE(wiring::Contains(body, "host_.StartTunnel("));
    UR_EXPECT_FALSE(wiring::Contains(body, "host_.SelectedLocation("));
  }
  const std::string toggle = wiring::FunctionBody(window, "void MainWindow::ToggleConnect(bool disconnect) {");
  UR_EXPECT_TRUE(wiring::Before(toggle, "NoteTap(connect_tap::Pending::Disconnect);", "host_.Disconnect();"));
  UR_EXPECT_FALSE(wiring::Contains(toggle, "CurrentConnectReading("));
  // the tray and the page both read the tap through the fold
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "void MainWindow::ApplyConnectReading(const ConnectReading& reading) {"),
      "connect_tap::Fold(reading.ToSignals("));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "void MainWindow::ApplyConnectReading(const ConnectReading& reading) {"),
      "false), pendingTap_);"));
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(page, "void ConnectPage::ApplyConnectStatus() {"),
                                  "connect_tap::Fold(reading_.ToSignals(disconnecting), tapPending_)"));
  // until the queue settles, with the reading the worker took after the last command
  UR_EXPECT_TRUE(wiring::Contains(window, "host_.SetCommandsSettledHandler("));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "void MainWindow::OnCommandsSettled(const ConnectReading& reading) {"),
      "NoteTap(connect_tap::Pending::None);"));
}

// A start waiting on its balance shows connecting, and goes back before the
// upgrade path opens.
UR_TEST(connectTapCoversTheBalanceWait) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  const std::string gate =
      wiring::FunctionBody(window, "bool MainWindow::ConnectBlockedByBalance(std::function<void()> retry) {");
  UR_EXPECT_TRUE(wiring::Before(gate, "CheckBalanceThen(std::move(retry));",
                                "NoteTap(connect_tap::Pending::Connect);"));
  UR_EXPECT_TRUE(wiring::Before(gate, "NoteTap(connect_tap::Pending::None);", "OpenUpgrade();"));
  // a disconnect or a sign-out cancels the wait, and its state with it
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(window, "void MainWindow::CancelBalanceCheck() {"),
                                  "NoteTap(connect_tap::Pending::None);"));
}

// While a command is outstanding the GTK thread makes no device read: the
// page's feed poll and throughput pull, the drawer events, the re-show, the
// health poll's reconcile and a row click's check stand back until it settles.
UR_TEST(connectCommandsKeepTheGtkThreadOffTheDevice) {
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  const std::pair<const char*, const char*> reads[] = {
      {"void ConnectPage::RefreshFeeds(bool force) {", "host_.CurrentConnectReading()"},
      {"void ConnectPage::PullThroughput() {", "host_.ThroughputPoints()"}};
  for (const auto& [signature, firstRead] : reads) {
    const std::string body = wiring::FunctionBody(page, signature);
    UR_EXPECT_TRUE_MSG(signature, wiring::Before(body, "if (host_.CommandsBusy()) return;", firstRead));
  }
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Before(window, "if (host_.CommandsBusy()) {\n        drawerEventsOwed_ = true;",
                                "if (windowVisible_ && connectPage_) connectPage_->OnHostEvent(event);"));
  const std::string reshow = window.substr(window.find("auto reconcilePresentation = [this] {"),
                                           window.find("property_visible().signal_changed()") -
                                               window.find("auto reconcilePresentation = [this] {"));
  UR_EXPECT_FALSE(wiring::Contains(reshow, "CurrentConnectReading("));
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE(wiring::Before(
      wiring::FunctionBody(host, "void SdkHost::ConnectFromRow(const std::optional<urnet::ConnectLocation>& location) {"),
      "!CommandsBusy() &&", "CurrentConnectReading()"));
}

// A sign-out fences the queue before anything is torn down, and the fence
// ends with the device.
UR_TEST(connectCommandsAreFencedBySignOut) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string logout = wiring::FunctionBody(host, "void SdkHost::Logout() {");
  UR_EXPECT_TRUE(wiring::Before(logout, "commands_.BeginSignOut();", "LogoutNow();"));
  // and a reading from before it is never handed on as the next account's
  UR_EXPECT_TRUE(wiring::Before(logout, "signInEpoch_.fetch_add(1);", "LogoutNow();"));
  // the device goes on the worker, behind the commands, and the fence ends
  // once it has (the command's main-loop apply)
  UR_EXPECT_TRUE(wiring::Before(logout, "ConnectCommandQueue::Kind::Disconnect", "LogoutNow();"));
  UR_EXPECT_TRUE(wiring::Before(logout, "LogoutNow();", "commands_.EndSignOut();"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::LogoutNow() {"),
                                  "TeardownDeviceLocked();"));
  // a second sign-out before the next sign-in tears nothing down again
  UR_EXPECT_TRUE(wiring::Before(logout, "if (signedOut_.exchange(true)) {", "commands_.BeginSignOut();"));
}

// A re-show queues the controllers' open behind the commands (SdkHost::
// SetPresentationActive), so the pages' re-show reads, and the degrade hold's
// re-evaluation, wait for the queue to settle instead of for the lock the
// open holds.
UR_TEST(connectCommandsHoldTheReshowReadsUntilTheySettle) {
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  const std::string resync = wiring::FunctionBody(page, "void ConnectPage::Resync() {");
  UR_EXPECT_TRUE(wiring::Before(resync, "if (host_.CommandsBusy()) {", "SyncProvideControlMode();"));
  UR_EXPECT_TRUE(wiring::Before(resync, "resyncOwed_ = true;", "RefreshAllPanes();"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(page, "void ConnectPage::OnCommandsSettled() {"),
                                  "if (resyncOwed_) Resync();"));
  const std::string earnings = wiring::FunctionBody(wiring::ReadCode("EarningsPage.cpp"),
                                                    "void EarningsPage::SetPresentationActive(bool active) {");
  UR_EXPECT_TRUE(wiring::Before(earnings, "if (host_.CommandsBusy()) return;",
                                "OnHostEvent(DrawerEvent::DeviceLifecycle);"));
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Before(window, "host_.SetPresentationActive(windowVisible_);",
                                "if (windowVisible_) drawerEventsOwed_ = true;"));
  const std::string settled =
      wiring::FunctionBody(window, "void MainWindow::OnCommandsSettled(const ConnectReading& reading) {");
  UR_EXPECT_TRUE(wiring::Contains(settled, "connectPage_->OnCommandsSettled();"));
  UR_EXPECT_TRUE(wiring::Contains(settled, "providerLocationsSheet_->Refresh();"));
  const std::string reading =
      wiring::FunctionBody(wiring::ReadCode("SdkHost.cpp"), "ConnectReading SdkHost::ReadConnectReading() {");
  UR_EXPECT_TRUE(wiring::Before(reading, "if (self->CommandsBusy()) {", "self->CurrentConnectReading()"));
  UR_EXPECT_TRUE(wiring::Before(reading, "self->degradeReevalAtMillis_ = 0;", "self->CurrentConnectReading()"));
}

// While commands are outstanding the window shows the tap's state, so the
// health poll concludes nothing from a Stopped the daemon reports (the queued
// disconnect's own stop, or a start not done yet) and reads nothing through
// the lock the command holds; and a disconnect still queued is one the user
// asked for, whoever made it (the tray, the balance notice).
UR_TEST(connectCommandsKeepTheHealthPollFromConcluding) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  const std::string poll = wiring::FunctionBody(
      window, "void MainWindow::ApplyDaemonHealth(const std::optional<ctl::StatusReply>& status) {");
  UR_EXPECT_TRUE(wiring::Before(poll, "if (host_.CommandsBusy()) return;",
                                "if (connectPage_ && connectPage_->DisconnectPending()) {"));
  UR_EXPECT_TRUE(wiring::Before(poll, "if (host_.CommandsBusy()) return;",
                                "T_(\"tunnel_stopped_unexpectedly\""));
  const std::string page = wiring::ReadCode("ConnectPage.hpp");
  UR_EXPECT_TRUE(wiring::Contains(
      page, "return disconnectRequestedAtUs_ != 0 || tapPending_ == connect_tap::Pending::Disconnect;"));
}

// The rpc bind watchdog's main-loop work goes behind the commands too: the
// remote-connected edge (mutex_ and the Secret Service save) and the deadline
// (the device's getters, the teardown, stop_tunnel, the keyring forget) both
// met the lock the queued connect after a fresh start holds. A fired source
// removed itself, so a teardown in between never removes it again.
UR_TEST(connectCommandsQueueTheRpcBindWatchdog) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string bind = wiring::FunctionBody(host, "TunnelStartResult SdkHost::BindRemoteDeviceLocked(");
  UR_EXPECT_TRUE(wiring::Before(bind, "addRemoteChangeListener(", "RunCommand(ConnectCommandQueue::Kind::Device"));
  UR_EXPECT_TRUE(wiring::Before(bind, "RunCommand(ConnectCommandQueue::Kind::Device", "RememberSyncedSessionLocked();"));
  const std::string arm = wiring::FunctionBody(host, "void SdkHost::ArmRpcBindWatchdogLocked() {");
  UR_EXPECT_TRUE(wiring::Before(arm, "rpcBindFiredId_.store(", "self->OnRpcBindDeadline();"));
  UR_EXPECT_TRUE(wiring::Before(arm, "RunCommand(ConnectCommandQueue::Kind::Device", "self->OnRpcBindDeadline();"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::RemoveRpcBindWatchLocked() {"),
                                  "rpcBindWatchId_ != rpcBindFiredId_.load()"));
}

// The device can be closed and released on the queue's worker now (a
// sign-out's teardown, a start's stale drop, the bind deadline), so the GTK
// thread uses the raw device only while no command is outstanding, and
// hasDevice reads an atomic mirror instead of the optional the worker sets.
UR_TEST(connectCommandsKeepTheRawDeviceOffTheGtkThreadWhileBusy) {
  const std::string header = wiring::ReadCode("SdkHost.hpp");
  UR_EXPECT_TRUE(wiring::Contains(header, "return deviceBound_.load() && deviceControlGeneration_ =="));
  UR_EXPECT_TRUE(wiring::Contains(header, "bool DeviceSteady() { return !CommandsBusy() && hasDevice(); }"));
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE(wiring::Before(host, "device_ = urnet::newDeviceRemoteWithDefaults(", "deviceBound_.store(true);"));
  UR_EXPECT_TRUE(wiring::Before(wiring::FunctionBody(host, "void SdkHost::TeardownDeviceLocked() {"),
                                "deviceBound_.store(false);", "device_.reset();"));
  // every raw device use on the Earnings page is behind DeviceSteady (or the
  // board's own busy return)
  const std::string earnings = wiring::ReadCode("EarningsPage.cpp");
  for (size_t at = earnings.find("device()."); at != std::string::npos;
       at = earnings.find("device().", at + 1)) {
    const size_t from = at > 900 ? at - 900 : 0;
    const std::string before = earnings.substr(from, at - from);
    const bool guarded = wiring::Contains(before, "DeviceSteady()") ||
                         wiring::Contains(before, "if (host_.CommandsBusy()) return;");
    UR_EXPECT_TRUE_MSG(earnings.substr(at, 60), guarded);
  }
}
