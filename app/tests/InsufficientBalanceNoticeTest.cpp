// The out-of-balance notice: the Connect page's held alert and the
// once-per-episode desktop notification (urnetwork/android#483). Every case
// drives the tracker through an explicit sequence of readings, so the expected
// calls are exact.
//
// SPDX-License-Identifier: MPL-2.0
#include "Health.hpp"
#include "InsufficientBalanceNotice.hpp"
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::balance_notice::Signals;
using urnw::balance_notice::Tracker;

// Counts every call. Disconnect is here only to prove the tracker never asks
// for one: the notice informs, the user's own Disconnect is the way out.
struct CountingSink {
  int posts = 0;
  int withdraws = 0;
  int disconnects = 0;
  std::string title;
  std::string body;
  void Post(const urnw::balance_notice::Text& t, const urnw::balance_notice::Text& b) {
    ++posts;
    title = t.key;
    body = b.key;
  }
  void Withdraw() { ++withdraws; }
  void Disconnect() { ++disconnects; }
};

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

bool Has(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

Signals Held() {
  Signals s;
  s.insufficientBalance = true;
  s.connectRequested = true;
  return s;
}

// The harness's EXPECT_EQ is numeric; strings compare here with both values in
// the failure.
void ExpectText(const char* what, const std::string& expected, const char* actual) {
  if (expected != actual) {
    UR_FAIL(std::string(what) + ": \"" + actual + "\", expected \"" + expected + "\"");
  }
}

Signals Funded() {
  Signals s;
  s.connectRequested = true;
  return s;
}

}  // namespace

UR_TEST(balanceNoticePostsOncePerEpisode) {
  Tracker tracker;
  CountingSink sink;
  tracker.Observe(Funded(), sink);
  UR_EXPECT_EQ(0, sink.posts);
  tracker.Observe(Held(), sink);
  UR_EXPECT_EQ(1, sink.posts);
  UR_EXPECT_TRUE(tracker.Shown());
  // the feed re-emits the same state many times a second: still one post
  for (int i = 0; i < 10; ++i) tracker.Observe(Held(), sink);
  UR_EXPECT_EQ(1, sink.posts);
  UR_EXPECT_EQ(0, sink.withdraws);
  UR_EXPECT_EQ(0, sink.disconnects);
}

UR_TEST(balanceNoticeReArmsAfterTheEpisodeEnds) {
  Tracker tracker;
  CountingSink sink;
  tracker.Observe(Held(), sink);
  tracker.Observe(Funded(), sink);
  UR_EXPECT_EQ(1, sink.withdraws);
  UR_EXPECT_FALSE(tracker.Shown());
  tracker.Observe(Held(), sink);
  UR_EXPECT_EQ(2, sink.posts);
  UR_EXPECT_EQ(0, sink.disconnects);
}

UR_TEST(balanceNoticeNeverPostsForProOrWhilePolling) {
  Tracker tracker;
  CountingSink sink;
  Signals pro = Held();
  pro.pro = true;
  Signals polling = Held();
  polling.polling = true;
  for (int i = 0; i < 3; ++i) {
    tracker.Observe(pro, sink);
    tracker.Observe(polling, sink);
  }
  UR_EXPECT_EQ(0, sink.posts);
  // a poll at entry defers the post; it lands once the poll settles unfunded
  tracker.Observe(Held(), sink);
  UR_EXPECT_EQ(1, sink.posts);
  UR_EXPECT_EQ(0, sink.disconnects);
}

UR_TEST(balanceNoticeWithdrawsOnDisconnectAndDoesNotRePost) {
  Tracker tracker;
  CountingSink sink;
  tracker.Observe(Held(), sink);
  Signals disconnected = Held();
  disconnected.connectRequested = false;
  tracker.Observe(disconnected, sink);
  UR_EXPECT_EQ(1, sink.withdraws);
  UR_EXPECT_FALSE(tracker.Shown());
  // reconnecting within the same episode does not post again
  tracker.Observe(Held(), sink);
  UR_EXPECT_EQ(1, sink.posts);
  // and a disconnected start never posts at all
  Tracker idle;
  CountingSink idleSink;
  idle.Observe(disconnected, idleSink);
  UR_EXPECT_EQ(0, idleSink.posts);
  UR_EXPECT_EQ(0, idleSink.withdraws);
  UR_EXPECT_EQ(0, sink.disconnects + idleSink.disconnects);
}

UR_TEST(balanceNoticeGateKeepsDisconnectAsTheAction) {
  // the notice tells the user to disconnect, so the button must offer it
  urnw::health::Signals s;
  s.sdk = urnw::health::SdkStatus::Connected;
  s.destinationSelected = true;
  s.tunnelBound = true;
  s.insufficientBalance = true;
  UR_EXPECT_TRUE(urnw::health::Render(s).action == urnw::health::Action::Disconnect);
}

UR_TEST(balanceNoticeIsWiredIntoTheWindow) {
  // a tracker nothing feeds is the defect again, so the call sites are read as
  // text (MainWindow needs GTK and the SDK)
  const std::string window = ReadSource("MainWindow.cpp");
  const std::string app = ReadSource("main.cpp");
  UR_EXPECT_TRUE(Has(window, "balanceNotice_.Observe("));
  UR_EXPECT_TRUE(Has(window, "send_notification("));
  UR_EXPECT_TRUE(Has(window, "withdraw_notification("));
  UR_EXPECT_TRUE(Has(window, "ToggleConnect(/*disconnect=*/true)"));
  UR_EXPECT_TRUE(Has(app, "kBalanceNoticeDisconnectAction"));
}

// gtkmm's Gtk::Window removes itself from its application when it is hidden
// and nothing adds it back when it shows again, so a notice posted through the
// window's get_application() went nowhere once the window had been hidden to
// the tray, which is when it is needed. Both the post and the withdraw go
// through the default application.
UR_TEST(balanceNoticeGoesThroughTheDefaultApplication) {
  const std::string window = ReadSource("MainWindow.cpp");
  const size_t at = window.find("void MainWindow::UpdateBalanceNotice() {");
  UR_EXPECT_TRUE(at != std::string::npos);
  const std::string body = window.substr(at, window.find("\n}\n", at) - at);
  UR_EXPECT_TRUE(Has(window, "Glib::RefPtr<Gio::Application> NotifyingApp() { return Gio::Application::get_default(); }"));
  UR_EXPECT_TRUE(Has(body, "auto app = NotifyingApp();"));
  UR_EXPECT_TRUE(Has(body, "if (auto app = NotifyingApp()) app->withdraw_notification(kBalanceNoticeId);"));
  UR_EXPECT_FALSE(Has(body, "get_application()"));
}

UR_TEST(balanceNoticeHeldAlertTable) {
  // gate x connect requested x pro x polling: the alert (and its Upgrade and
  // Disconnect) shows only for an unfunded, non-Pro, settled account whose
  // tunnel is asked to carry traffic
  for (int bits = 0; bits < 32; ++bits) {
    Signals s;
    s.insufficientBalance = bits & 1;
    s.connectRequested = bits & 2;
    s.pro = bits & 4;
    s.polling = bits & 8;
    const bool expected = s.insufficientBalance && s.connectRequested && !s.pro && !s.polling;
    if (urnw::balance_notice::HeldAlert(s) != expected) {
      UR_FAIL("held alert wrong for case " + std::to_string(bits));
    }
  }
}

UR_TEST(balanceNoticeHeldAlertIsOnTheReachableConnectPage) {
  // the Connect page the user sees hosts the alert and both ways out
  const std::string page = ReadSource("ConnectPage.cpp");
  const std::string window = ReadSource("MainWindow.cpp");
  const size_t alertAt = page.find("heldAlert_ = Gtk::make_managed");
  UR_EXPECT_TRUE(alertAt != std::string::npos);
  if (alertAt != std::string::npos) {
    const size_t endAt = page.find("paneAContent_->append(*heldAlert_)", alertAt);
    const std::string block = page.substr(alertAt, endAt - alertAt);
    // what the traffic is doing is a live line, not one written when it was built
    UR_EXPECT_FALSE(Has(block, "insufficient_balance_held_notice"));
    UR_EXPECT_TRUE(Has(block, "heldAlertTraffic_ = Gtk::make_managed<Gtk::Label>()"));
    UR_EXPECT_TRUE(Has(block, "T_(\"upgrade\", \"Upgrade\")"));
    UR_EXPECT_TRUE(Has(block, "T_(\"disconnect\", \"Disconnect\")"));
    UR_EXPECT_TRUE(Has(block, "on_open_upgrade()"));
    UR_EXPECT_TRUE(Has(block, "on_balance_disconnect()"));
    // always enabled, and never the connect path
    UR_EXPECT_FALSE(Has(block, "set_sensitive"));
    UR_EXPECT_FALSE(Has(block, "RelayConnectPress"));
  }
  UR_EXPECT_TRUE(Has(page, "heldAlert_->set_visible(balance_notice::ShowsAlert(signals))"));
  UR_EXPECT_TRUE(Has(window, "connectPage_->ApplyBalanceNotice(signals)"));
  UR_EXPECT_TRUE(Has(window, "on_balance_disconnect = [this] { DisconnectFromBalanceNotice(); }"));
  UR_EXPECT_TRUE(Has(window, "connectPage_->on_open_upgrade"));
}

// ---- reserved or used up, and recovering by itself ----------------------------

namespace {

using urnw::balance_notice::AccountBalance;
using urnw::balance_notice::BalanceRecovery;
using urnw::balance_notice::kRecoveryBudgetResetMillis;
using urnw::balance_notice::kRecoveryMaxRetries;
using urnw::balance_notice::kRecoveryThresholdBytes;
using urnw::balance_notice::OutOfBalanceKind;
using urnw::balance_notice::OutOfBalanceKindFor;
using urnw::balance_notice::RecoveryLinesFor;
using urnw::balance_notice::RecoveryState;
using urnw::balance_notice::RecoveryStep;
using urnw::balance_notice::RecoveryStepKind;

constexpr int64_t kMinute = 60 * 1000;
constexpr int64_t kGib = 1024LL * 1024 * 1024;
constexpr int64_t kLow = kRecoveryThresholdBytes - 1;
constexpr int64_t kBack = kRecoveryThresholdBytes;

AccountBalance Reading(int64_t available, int64_t at, int64_t pending = 0, bool pro = false) {
  AccountBalance b;
  b.known = true;
  b.pro = pro;
  b.availableBytes = available;
  b.openTransferBytes = pending;
  b.fetchedAtMillis = at;
  return b;
}

using Recovery = BalanceRecovery<std::string>;

// The definition that starts at `signature`, up to its closing brace; empty
// when the source no longer has it.
std::string FunctionBody(const std::string& source, const char* signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", at);
  return source.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

RecoveryStepKind HeldAt(Recovery& r, int64_t available, int64_t at) {
  return r.Observe(/*gate=*/true, /*connectRequested=*/true, Reading(available, at), at).kind;
}

RecoveryStepKind DisconnectedAt(Recovery& r, int64_t available, int64_t at, bool gate = false) {
  return r.Observe(gate, /*connectRequested=*/false, Reading(available, at), at).kind;
}

}  // namespace

UR_TEST(outOfBalanceKindSaysReservedOrUsedUp) {
  // the reported case: barely used, but open (or abandoned) connections hold
  // the balance as Pending
  UR_EXPECT_TRUE(OutOfBalanceKindFor(Reading(0, 0, 30 * kGib)) == OutOfBalanceKind::Reserved);
  UR_EXPECT_TRUE(OutOfBalanceKindFor(Reading(kLow, 0, kBack)) == OutOfBalanceKind::Reserved);
  UR_EXPECT_TRUE(OutOfBalanceKindFor(Reading(0, 0, 0)) == OutOfBalanceKind::Exhausted);
  UR_EXPECT_TRUE(OutOfBalanceKindFor(Reading(0, 0, kLow)) == OutOfBalanceKind::Exhausted);
  // neither without a reading, for Pro, or when the reading says available
  UR_EXPECT_TRUE(OutOfBalanceKindFor(AccountBalance{}) == OutOfBalanceKind::Unknown);
  UR_EXPECT_TRUE(OutOfBalanceKindFor(Reading(0, 0, 0, true)) == OutOfBalanceKind::Unknown);
  UR_EXPECT_TRUE(OutOfBalanceKindFor(Reading(kBack, 0, 30 * kGib)) == OutOfBalanceKind::Unknown);
}

UR_TEST(balanceRecoveryNeverConnectsAUserWhoDidNotAsk) {
  Recovery r;
  int64_t t = 0;
  for (int round = 0; round < 5; ++round) {
    for (bool gate : {true, false}) {
      t += kMinute;
      UR_EXPECT_TRUE(DisconnectedAt(r, 0, t, gate) == RecoveryStepKind::None);
      t += kMinute;
      UR_EXPECT_TRUE(DisconnectedAt(r, kBack, t, gate) == RecoveryStepKind::None);
    }
    t += kMinute;
    UR_EXPECT_TRUE(r.Observe(false, true, Reading(kBack, t), t).kind == RecoveryStepKind::None);
  }
  UR_EXPECT_FALSE(r.State().startWaiting);
}

UR_TEST(balanceRecoveryRunsTheRefusedPressOnceWhenDataIsBack) {
  Recovery r;
  r.StartRefused("de", 0);
  UR_EXPECT_TRUE(r.State().startWaiting);
  UR_EXPECT_TRUE(DisconnectedAt(r, 0, kMinute, true) == RecoveryStepKind::None);
  const RecoveryStep<std::string> step = r.Observe(false, false, Reading(kBack, 2 * kMinute), 2 * kMinute);
  UR_EXPECT_TRUE(step.kind == RecoveryStepKind::Start);
  ExpectText("refused target", "de", step.target.c_str());
  UR_EXPECT_FALSE(r.State().startWaiting);
  UR_EXPECT_TRUE(DisconnectedAt(r, kBack, 3 * kMinute) == RecoveryStepKind::None);
}

UR_TEST(balanceRecoveryNeverRetriesOnAReadingFromBeforeTheBlock) {
  Recovery r;
  r.StartRefused("de", 10 * kMinute);
  UR_EXPECT_TRUE(DisconnectedAt(r, kBack, 9 * kMinute) == RecoveryStepKind::None);
  UR_EXPECT_TRUE(DisconnectedAt(r, kBack, 11 * kMinute) == RecoveryStepKind::Start);

  // the hold is first seen with the reading from before the balance ran out
  Recovery hold;
  UR_EXPECT_TRUE(hold.Observe(true, true, Reading(kBack, 0), 10 * kMinute).kind == RecoveryStepKind::None);
  UR_EXPECT_TRUE(HeldAt(hold, 0, 11 * kMinute) == RecoveryStepKind::None);
  UR_EXPECT_TRUE(HeldAt(hold, kBack, 12 * kMinute) == RecoveryStepKind::Rebuild);
}

UR_TEST(balanceRecoveryRetriesOncePerRecoveryAndAtMostThreeTimes) {
  Recovery r;
  HeldAt(r, 0, kMinute);
  UR_EXPECT_TRUE(HeldAt(r, kBack, 2 * kMinute) == RecoveryStepKind::Rebuild);
  UR_EXPECT_TRUE(HeldAt(r, kBack, 3 * kMinute) == RecoveryStepKind::None);
  UR_EXPECT_TRUE(HeldAt(r, kBack, 4 * kMinute) == RecoveryStepKind::None);
  HeldAt(r, 0, 5 * kMinute);
  UR_EXPECT_TRUE(HeldAt(r, kBack, 6 * kMinute) == RecoveryStepKind::Rebuild);

  Recovery bounded;
  int rebuilds = 0;
  int64_t t = 0;
  for (int round = 0; round < 10; ++round) {
    t += kMinute;
    HeldAt(bounded, 0, t);
    t += kMinute;
    if (HeldAt(bounded, kBack, t) == RecoveryStepKind::Rebuild) ++rebuilds;
  }
  UR_EXPECT_EQ(kRecoveryMaxRetries, rebuilds);
  UR_EXPECT_FALSE(bounded.State().retriesLeft);
  // a new ask from the user refills them
  t += kMinute;
  bounded.StartRefused("fr", t);
  UR_EXPECT_TRUE(bounded.State().retriesLeft);
  t += kMinute;
  UR_EXPECT_TRUE(HeldAt(bounded, kBack, t) == RecoveryStepKind::Start);

  // and so does a retried connection that stays out of the block
  Recovery up;
  t = 0;
  for (int round = 0; round < kRecoveryMaxRetries; ++round) {
    t += kMinute;
    HeldAt(up, 0, t);
    t += kMinute;
    HeldAt(up, kBack, t);
  }
  const int64_t lastRetry = t;
  up.Observe(false, true, Reading(kBack, t + 1), lastRetry + kMinute);
  UR_EXPECT_FALSE(up.State().retriesLeft);
  up.Observe(false, true, Reading(kBack, lastRetry + kRecoveryBudgetResetMillis),
             lastRetry + kRecoveryBudgetResetMillis);
  UR_EXPECT_TRUE(up.State().retriesLeft);
}

UR_TEST(balanceRecoveryEndsWithCancelOrDisconnectAndKeepsItsEdges) {
  Recovery cancelled;
  cancelled.StartRefused("de", 0);
  cancelled.Clear();
  UR_EXPECT_FALSE(cancelled.State().startWaiting);
  UR_EXPECT_TRUE(DisconnectedAt(cancelled, 0, kMinute) == RecoveryStepKind::None);
  UR_EXPECT_TRUE(DisconnectedAt(cancelled, kBack, 2 * kMinute) == RecoveryStepKind::None);

  // a held connection the user disconnects is not reconnected
  Recovery disconnected;
  HeldAt(disconnected, 0, 0);
  disconnected.Clear();
  UR_EXPECT_TRUE(DisconnectedAt(disconnected, 0, kMinute, true) == RecoveryStepKind::None);
  UR_EXPECT_TRUE(DisconnectedAt(disconnected, kBack, 2 * kMinute, true) == RecoveryStepKind::None);

  // a hold that ends by itself is not rebuilt
  Recovery recovered;
  HeldAt(recovered, 0, 0);
  recovered.Observe(false, true, Reading(kBack, kMinute), kMinute);
  UR_EXPECT_TRUE(recovered.Observe(false, true, Reading(kBack, 2 * kMinute), 2 * kMinute).kind ==
                 RecoveryStepKind::None);

  // data is back only at the threshold, and an unknown balance waits
  Recovery threshold;
  threshold.StartRefused("", 0);
  UR_EXPECT_TRUE(DisconnectedAt(threshold, kLow, kMinute) == RecoveryStepKind::None);
  UR_EXPECT_TRUE(threshold.Observe(true, false, AccountBalance{}, 2 * kMinute).kind == RecoveryStepKind::None);
  UR_EXPECT_TRUE(DisconnectedAt(threshold, kBack, 3 * kMinute) == RecoveryStepKind::Start);

  // the latest refused start wins over the held connection
  Recovery latest;
  HeldAt(latest, 0, 0);
  latest.StartRefused("jp", kMinute / 2);
  const RecoveryStep<std::string> step = latest.Observe(true, true, Reading(kBack, kMinute), kMinute);
  UR_EXPECT_TRUE(step.kind == RecoveryStepKind::Start);
  ExpectText("latest target", "jp", step.target.c_str());
}

UR_TEST(balanceRecoveryLinesOnTheConnectPage) {
  RecoveryState waiting;
  RecoveryState startWaiting;
  startWaiting.startWaiting = true;
  RecoveryState spent;
  spent.retriesLeft = false;
  Signals idle = Held();
  idle.connectRequested = false;
  for (OutOfBalanceKind kind :
       {OutOfBalanceKind::Unknown, OutOfBalanceKind::Reserved, OutOfBalanceKind::Exhausted}) {
    UR_EXPECT_TRUE(RecoveryLinesFor(Held(), kind, waiting).kind == kind);
    UR_EXPECT_TRUE(RecoveryLinesFor(idle, kind, waiting).kind == OutOfBalanceKind::Unknown);
  }
  // a held connection is rebuilt by itself; Disconnect is its way out
  UR_EXPECT_TRUE(RecoveryLinesFor(Held(), OutOfBalanceKind::Reserved, waiting).willReconnect);
  UR_EXPECT_FALSE(RecoveryLinesFor(Held(), OutOfBalanceKind::Reserved, waiting).cancel);
  // a refused start waits with Cancel, with nothing held
  UR_EXPECT_TRUE(RecoveryLinesFor(idle, OutOfBalanceKind::Unknown, startWaiting).willReconnect);
  UR_EXPECT_TRUE(RecoveryLinesFor(idle, OutOfBalanceKind::Unknown, startWaiting).cancel);
  // nothing the user asked for waits, or the retries are spent: no promise
  UR_EXPECT_FALSE(RecoveryLinesFor(idle, OutOfBalanceKind::Exhausted, waiting).willReconnect);
  UR_EXPECT_FALSE(RecoveryLinesFor(Held(), OutOfBalanceKind::Exhausted, spent).willReconnect);
}

UR_TEST(balanceRecoveryIsWiredIntoTheWindowAndTheConnectPage) {
  const std::string window = ReadSource("MainWindow.cpp");
  const std::string page = ReadSource("ConnectPage.cpp");
  const std::string sheet = ReadSource("UpgradeSheet.cpp");
  // the gate's refused press waits on the balance; an admitted press ends a wait
  const std::string gate =
      FunctionBody(window, "bool MainWindow::ConnectBlockedByBalance(std::function<void()> retry) {");
  UR_EXPECT_TRUE(Has(gate, "if (retryingRefusedConnect_) return false;"));
  UR_EXPECT_TRUE(Has(gate, "balanceRecovery_.StartRefused(retry, BalanceClockMillis());"));
  UR_EXPECT_TRUE(Has(gate, "ClearBalanceRecovery();"));
  // every reading and balance change feeds it after the latch, and its retry
  // runs past the gate and says so
  const std::string notice = FunctionBody(window, "void MainWindow::UpdateBalanceNotice() {");
  const size_t latchAt = notice.find("outOfBalance_.Observe(observation);");
  UR_EXPECT_TRUE(latchAt != std::string::npos &&
                 latchAt < notice.find("ObserveBalanceRecovery();"));
  const std::string observe = FunctionBody(window, "void MainWindow::ObserveBalanceRecovery() {");
  const size_t setAt = observe.find("retryingRefusedConnect_ = true;");
  UR_EXPECT_TRUE(setAt != std::string::npos &&
                 setAt < observe.find("retryingRefusedConnect_ = false;"));
  UR_EXPECT_TRUE(Has(observe, "T_(\"insufficient_balance_reconnecting\""));
  // the user's Disconnect and a sign-out end the wait, and so does Cancel
  const std::string toggle = FunctionBody(window, "void MainWindow::ToggleConnect(bool disconnect) {");
  const size_t clearAt = toggle.find("ClearBalanceRecovery();");
  UR_EXPECT_TRUE(clearAt != std::string::npos && clearAt < toggle.find("host_.Disconnect();"));
  UR_EXPECT_TRUE(Has(FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {"),
                     "ClearBalanceRecovery();"));
  UR_EXPECT_TRUE(Has(window, "connectPage_->on_cancel_balance_recovery = [this] { ClearBalanceRecovery(); };"));
  // the Connect page: the kind in the held alert, the promise and Cancel under it
  UR_EXPECT_TRUE(Has(page, "T_(\"insufficient_balance_will_reconnect\""));
  UR_EXPECT_TRUE(Has(page, "on_cancel_balance_recovery()"));
  UR_EXPECT_TRUE(Has(page, "OutOfBalanceKindText("));
  // the upgrade sheet a blocked connect opens says it too
  UR_EXPECT_TRUE(Has(sheet, "OutOfBalanceKindText("));
}

// ---- what the traffic is doing while refused ----------------------------------

namespace {

using urnw::balance_notice::AlertOffersUpgrade;
using urnw::balance_notice::AlertTitle;
using urnw::balance_notice::Captured;
using urnw::balance_notice::DaemonCapture;
using urnw::balance_notice::DataLimitAlert;
using urnw::balance_notice::DataLimitGate;
using urnw::balance_notice::ShowsAlert;
using urnw::balance_notice::TrafficLine;
using urnw::balance_notice::TrafficLineFor;
using urnw::balance_notice::TrafficText;

// A paying account refused at a limit, with the tunnel asked for.
Signals AtLimit() {
  Signals s = Held();
  s.pro = true;
  s.dataLimit = true;
  return s;
}

}  // namespace

UR_TEST(balanceNoticeTrafficLineSaysWhatTheCaptureIs) {
  Signals s = Held();
  s.captured = true;
  UR_EXPECT_TRUE(TrafficLineFor(s) == TrafficLine::Held);
  s.floorInForce = true;
  UR_EXPECT_TRUE(TrafficLineFor(s) == TrafficLine::Held);
  s.captured = false;
  UR_EXPECT_TRUE(TrafficLineFor(s) == TrafficLine::Blocked);
  s.floorInForce = false;
  UR_EXPECT_TRUE(TrafficLineFor(s) == TrafficLine::NotProtected);
}

UR_TEST(balanceNoticeCapturedFollowsTheBoundDeviceFirst) {
  // the defect's reading: the daemon stopped the tunnel, the last status in hand
  // still says routes, and the selected destination outlives both
  UR_EXPECT_FALSE(Captured(false, DaemonCapture{true, true}));
  UR_EXPECT_FALSE(Captured(false, std::nullopt));
  // a bound device with no status yet stands on its own
  UR_EXPECT_TRUE(Captured(true, std::nullopt));
  UR_EXPECT_TRUE(Captured(true, DaemonCapture{true, true}));
  // a running tunnel the daemon says holds no routes captures nothing
  UR_EXPECT_FALSE(Captured(true, DaemonCapture{true, false}));
  // a status from before the tunnel came up says nothing about it
  UR_EXPECT_TRUE(Captured(true, DaemonCapture{false, false}));
}

UR_TEST(balanceNoticeSaysHeldOnlyOverACapture) {
  Signals s = Held();
  s.captured = true;
  UR_EXPECT_TRUE(std::string(TrafficText(s).key) == "insufficient_balance_held_notice");
  s.captured = false;
  UR_EXPECT_TRUE(std::string(TrafficText(s).key) == "conn_not_protected");
  s.floorInForce = true;
  UR_EXPECT_TRUE(std::string(TrafficText(s).key) == "conn_traffic_blocked");
}

UR_TEST(balanceNoticeNotificationSaysWhatIsTrueAndFollowsIt) {
  Tracker tracker;
  CountingSink sink;
  Signals s = Held();
  s.captured = true;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(1, sink.posts);
  UR_EXPECT_TRUE(sink.title == "insufficient_balance");
  UR_EXPECT_TRUE(sink.body == "insufficient_balance_held_notice");
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(1, sink.posts);
  // the daemon stopped the tunnel under it: the notice up is replaced in place
  s.captured = false;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(2, sink.posts);
  UR_EXPECT_TRUE(sink.body == "conn_not_protected");
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(2, sink.posts);
  // the user disconnects: withdrawn, and not posted again in the episode
  s.connectRequested = false;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(1, sink.withdraws);
  s.connectRequested = true;
  s.captured = true;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(2, sink.posts);
  UR_EXPECT_EQ(0, sink.disconnects);
}

UR_TEST(balanceNoticeSessionThatNeverCapturedIsNotToldItIsHeld) {
  Tracker tracker;
  CountingSink sink;
  Signals s = Held();
  s.captured = false;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(1, sink.posts);
  UR_EXPECT_TRUE(sink.body == "conn_not_protected");
}

// ---- a paying account refused: the data limit ---------------------------------

UR_TEST(dataLimitIsAPayingAccountsRefusal) {
  {
    DataLimitGate gate;
    UR_EXPECT_TRUE(gate.Observe(true, true, true, false));
    UR_EXPECT_FALSE(gate.Observe(false, true, true, false));
  }
  {
    DataLimitGate gate;
    UR_EXPECT_FALSE(gate.Observe(true, true, false, false));
  }
  {
    DataLimitGate gate;
    UR_EXPECT_FALSE(gate.Observe(false, true, true, false));
  }
  {
    DataLimitGate gate;
    UR_EXPECT_FALSE(gate.Observe(true, true, true, true));
    UR_EXPECT_TRUE(gate.Observe(true, true, true, false));
  }
}

UR_TEST(dataLimitIgnoresARefusalFromBeforePro) {
  DataLimitGate gate;
  UR_EXPECT_FALSE(gate.Observe(true, true, false, false));
  UR_EXPECT_FALSE(gate.Observe(true, true, false, true));
  UR_EXPECT_FALSE(gate.Observe(true, true, true, true));
  // the purchase landed with the old refusal still standing
  UR_EXPECT_FALSE(gate.Observe(true, true, true, false));
  UR_EXPECT_FALSE(gate.Observe(true, true, true, false));
  UR_EXPECT_FALSE(gate.Observe(false, true, true, false));
  // one the Pro plan got for itself
  UR_EXPECT_TRUE(gate.Observe(true, true, true, false));
}

UR_TEST(dataLimitWaitsForTheFirstBalance) {
  {
    DataLimitGate gate;
    UR_EXPECT_FALSE(gate.Observe(true, false, false, false));
    // Pro on the first reading of a launch is no upgrade
    UR_EXPECT_TRUE(gate.Observe(true, true, true, false));
  }
  {
    DataLimitGate gate;
    UR_EXPECT_FALSE(gate.Observe(true, false, false, false));
    UR_EXPECT_FALSE(gate.Observe(true, true, false, false));
    UR_EXPECT_FALSE(gate.Observe(true, true, true, false));
  }
}

UR_TEST(dataLimitAlertIsTheSameKindWithNothingToBuy) {
  Signals s = AtLimit();
  UR_EXPECT_TRUE(DataLimitAlert(s));
  UR_EXPECT_TRUE(ShowsAlert(s));
  UR_EXPECT_FALSE(urnw::balance_notice::HeldAlert(s));
  UR_EXPECT_FALSE(AlertOffersUpgrade(s));
  UR_EXPECT_TRUE(AlertOffersUpgrade(Held()));
  UR_EXPECT_TRUE(std::string(AlertTitle(s).key) == "data_limit_reached");
  UR_EXPECT_TRUE(std::string(AlertTitle(Held()).key) == "insufficient_balance");
  s.captured = true;
  UR_EXPECT_TRUE(std::string(TrafficText(s).key) == "data_limit_held_notice");
  s.captured = false;
  UR_EXPECT_TRUE(std::string(TrafficText(s).key) == "conn_not_protected");
  s.floorInForce = true;
  UR_EXPECT_TRUE(std::string(TrafficText(s).key) == "conn_traffic_blocked");
  // a Disconnect ends it
  s.connectRequested = false;
  UR_EXPECT_FALSE(ShowsAlert(s));
}

UR_TEST(dataLimitNotificationOncePerEpisodeAndNeverDisconnects) {
  Tracker tracker;
  CountingSink sink;
  Signals s = AtLimit();
  s.captured = true;
  tracker.Observe(s, sink);
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(1, sink.posts);
  UR_EXPECT_TRUE(sink.title == "data_limit_reached");
  UR_EXPECT_TRUE(sink.body == "data_limit_held_notice");
  s.captured = false;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(2, sink.posts);
  UR_EXPECT_TRUE(sink.body == "conn_not_protected");
  s.insufficientBalance = false;
  s.dataLimit = false;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(1, sink.withdraws);
  s = AtLimit();
  s.captured = true;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(3, sink.posts);
  UR_EXPECT_EQ(0, sink.disconnects);
}

UR_TEST(dataLimitNotificationNotAnnouncedForAPurchase) {
  // the refusal from before the purchase, Pro now, the gate has said no limit
  Tracker tracker;
  CountingSink sink;
  Signals s = Held();
  s.pro = true;
  s.dataLimit = false;
  tracker.Observe(s, sink);
  UR_EXPECT_EQ(0, sink.posts);
}

UR_TEST(dataLimitAndCaptureAreWiredIntoTheWindowAndThePage) {
  const std::string window = ReadSource("MainWindow.cpp");
  const std::string page = ReadSource("ConnectPage.cpp");
  // no surface writes the held sentence itself any more
  UR_EXPECT_FALSE(Has(window, "insufficient_balance_held_notice"));
  UR_EXPECT_FALSE(Has(page, "insufficient_balance_held_notice"));
  UR_EXPECT_TRUE(Has(window, "Gio::Notification::create(T_(title.key, title.english))"));
  UR_EXPECT_TRUE(Has(window, "notification->set_body(T_(body.key, body.english))"));
  // the window's signals carry the capture and the limit, from the raw status
  const size_t signalsAt = window.find("balance_notice::Signals MainWindow::BalanceSignals() const {");
  UR_EXPECT_TRUE(signalsAt != std::string::npos);
  const std::string signals = window.substr(signalsAt, window.find("balance_notice::AccountBalance MainWindow::", signalsAt) - signalsAt);
  UR_EXPECT_TRUE(Has(signals, "signals.dataLimit = dataLimit_;"));
  UR_EXPECT_TRUE(Has(signals, "balance_notice::Captured(reading_.tunnelBound, daemon)"));
  UR_EXPECT_TRUE(Has(signals, "daemonStatus_->routes_installed"));
  UR_EXPECT_TRUE(Has(signals, "ctl::KillSwitchState::Armed"));
  UR_EXPECT_TRUE(Has(window, "dataLimitGate_.Observe(reading_.insufficientBalance, balance_.HasFetched(),"));
  // the status the tray and the page read has the plan's say over the refusal
  UR_EXPECT_TRUE(Has(window, "health::WithPlan(folded, balance_.IsPro(), dataLimit_)"));
  UR_EXPECT_TRUE(Has(page, "health::WithPlan(\n      connect_tap::Fold(reading_.ToSignals(disconnecting), tapPending_), balancePro_,"));
  // a poll's answer about the capture reaches the alert
  const size_t healthAt = window.find("void MainWindow::ApplyDaemonHealth(");
  UR_EXPECT_TRUE(healthAt != std::string::npos);
  const std::string health = window.substr(healthAt, window.find("if (!connected_) {", healthAt) - healthAt);
  UR_EXPECT_TRUE(Has(health, "UpdateBalanceNotice();"));
  // the alert shows the line the signals say, and offers Upgrade only for a balance
  UR_EXPECT_TRUE(Has(page, "balance_notice::TrafficText(signals)"));
  UR_EXPECT_TRUE(Has(page, "heldAlertTraffic_->set_text(T_(traffic.key, traffic.english))"));
  UR_EXPECT_TRUE(Has(page, "heldAlertUpgrade_->set_visible(offersUpgrade)"));
  UR_EXPECT_TRUE(Has(page, "heldAlertRefresh_->set_visible(offersUpgrade)"));
}
