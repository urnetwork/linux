// The client limit's call sites (ClientLimitNotice.hpp). Before them nothing in
// the GUI read the device's client limit hold (Device.GetClientLimitStatus):
// a device the platform held off sat on "Connecting" with no word of why or
// what to do, and a sign-in the limit refused showed the server's words, with
// x402 on the raw payment terms. The presentation is pure and runs in
// ClientLimitNoticeTest.cpp; the surfaces need GTK, gio and the SDK, so this
// reads their sources with the comments blanked (WiringSource.hpp), so prose
// cannot satisfy a contract:
//   - SdkHost keeps a device-lifetime feed of the hold off the bound device,
//     shown only over a connected remote, read once after its listeners
//     register, and cleared with the device and on the daemon's tunnel-gone
//     verdict; Retry runs on the command queue's worker;
//   - a refused sign-in carries the sdk's prompt, and every sign-in screen that
//     shows a refusal says what to do for it;
//   - the window shows the notice while the device is held and a connection is
//     requested, lets the sdk's tracker decide the notification (sent only
//     while the app is away, replaced quietly, withdrawn), resets the tracker
//     at a sign-out, and retries on an upgrade seen in this run, never at
//     launch; nothing here disconnects;
//   - the Connect page offers Get Pro on a free plan and Retry.
// SPDX-License-Identifier: MPL-2.0
#include <string>
#include <vector>

#include "TestHarness.hpp"
#include "WiringSource.hpp"

namespace {

namespace wiring = urnw::testing::wiring;

// Each needle occurs after the one before it.
bool ClientLimitInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(ClientLimitWiring_TheHostFeedsTheHoldOffTheBoundDevice) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string bind =
      wiring::FunctionBody(host, "TunnelStartResult SdkHost::BindRemoteDeviceLocked(");
  UR_EXPECT_TRUE(ClientLimitInOrder(bind, {"BindTransportDelayLocked();", "BindClientLimitLocked();"}));

  const std::string feed = wiring::FunctionBody(host, "void SdkHost::BindClientLimitLocked() {");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      feed, {"generation = clientLimitFeed_.Bind();",
             "statusRevision = clientLimitFeed_.StatusRevision();",
             "remoteRevision = clientLimitFeed_.RemoteRevision();",
             "subs_.push_back(device_->addRemoteChangeListener(",
             "clientLimitFeed_.NoteRemote(generation, connected)",
             "subs_.push_back(device_->addClientLimitStatusChangeListener(",
             "clientLimitFeed_.NoteStatus(generation, ToClientLimitStatus(status))",
             // the reads come after the registrations, and lose to their pushes
             "device_->getRemoteConnected();", "device_->getClientLimitStatus();",
             "clientLimitFeed_.SeedRemote(generation, remoteRevision, connected);",
             "clientLimitFeed_.SeedStatus(generation, statusRevision, status)"}));
  // the bind, both listeners and the reads take the feed's lock, never mutex_
  UR_EXPECT_EQ(4u, wiring::CountOf(feed, "std::scoped_lock lock(clientLimitMutex_);"));
  UR_EXPECT_FALSE(wiring::Contains(feed, "lock(mutex_)"));

  // the published reading is whole and numbered, and goes out only when it changed
  const std::string publish =
      wiring::FunctionBody(host, "void SdkHost::PublishClientLimitLocked() {");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      publish, {"clientLimitFeed_.Current();", "reading.SameAs(clientLimitPublished_)",
                "clientLimitPublished_.serial = ++clientLimitSerial_;",
                "onClientLimit_(clientLimitPublished_);"}));

  // gone with its device, and with the daemon's tunnel: a DeviceRemote whose
  // daemon is gone keeps its last readout
  UR_EXPECT_TRUE(ClientLimitInOrder(wiring::FunctionBody(host, "void SdkHost::TeardownDeviceLocked() {"),
                                    {"subs_.clear();", "ClearClientLimit();"}));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::NoteDaemonTunnelGone() {"),
                                  "ClearClientLimit();"));

  // the hold is the sdk's own reading of its constants
  const std::string status = wiring::FunctionBody(
      host, "client_limit::Status ToClientLimitStatus(const std::optional<urnet::ClientLimitStatus>& status) {");
  UR_EXPECT_TRUE(wiring::Contains(status, "status->Status == urnet::ClientLimitStatusExceeded"));
  const std::string prompt =
      wiring::FunctionBody(host, "client_limit::Prompt ToClientLimitPrompt(const std::string& prompt) {");
  UR_EXPECT_TRUE(wiring::Contains(prompt, "urnet::ClientLimitPromptUpgrade"));
  UR_EXPECT_TRUE(wiring::Contains(prompt, "urnet::ClientLimitPromptRemoveClient"));
}

// The device answers over its rpc, so Retry never runs on the main loop, and
// only the sdk decides whether a hold ends (at most one per 30 s).
UR_TEST(ClientLimitWiring_RetryRunsOnTheCommandQueue) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string retry =
      wiring::FunctionBody(host, "void SdkHost::RetryClientLimit(const char* reason) {");
  UR_EXPECT_TRUE(ClientLimitInOrder(retry, {"RunCommand(ConnectCommandQueue::Kind::Device,",
                                            "std::scoped_lock lock(mutex_);", "if (!device_)",
                                            "device_->retryClientLimit();"}));
  UR_EXPECT_EQ(1u, wiring::CountOf(host, "retryClientLimit()"));
}

// A refused registration carries the sdk's prompt beside the server's words,
// ahead of the message path that used to show them; discovery's own sign-in
// hands it on.
UR_TEST(ClientLimitWiring_ARefusedSignInCarriesThePrompt) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string registration = wiring::FunctionBody(
      host, "void SdkHost::RegisterNetworkClient(const std::string& byJwt,");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      registration, {"if (!result) {",
                     "refused.clientLimitPrompt = ToClientLimitPrompt(result->error->client_limit_prompt);",
                     "done(refused);",
                     "if (result->error && !result->error->message.empty()) {"}));
  const std::string login = wiring::FunctionBody(host, "void SdkHost::StartLogin(");
  UR_EXPECT_TRUE(wiring::Contains(login, "routed.clientLimitPrompt = r.clientLimitPrompt;"));
}

// Every sign-in screen that shows a refusal says what to do about the limit
// instead of the server's words. The verify page's own line is for a bad code,
// which a code the limit refused afterwards is not.
UR_TEST(ClientLimitWiring_EverySignInScreenSaysWhatToDo) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(window, "void MainWindow::OnSignIn() {"),
                                  "SignInFailureText(r, T_(\"sign_in_failed\""));
  UR_EXPECT_TRUE(wiring::Contains(window, "SignInFailureText(r, T_(\"seedphrase_login_failed\""));
  UR_EXPECT_TRUE(wiring::Contains(window, "r, T_(\"instant_account_failed\""));
  UR_EXPECT_TRUE(wiring::Contains(window, "SignInFailureText(r, T_(\"code_sign_in_failed\""));
  UR_EXPECT_TRUE(wiring::Contains(window, "routing.clientLimitPrompt, routing.error,"));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "void MainWindow::OnWalletAuth(const AuthResult& result) {"),
      "SetLoginError(SignInFailureText("));
  // the shared words: the prompt's body first, the server's words after
  const std::string text = wiring::FunctionBody(
      window, "Glib::ustring SignInFailureText(client_limit::Prompt clientLimitPrompt,");
  UR_EXPECT_TRUE(ClientLimitInOrder(text, {"client_limit::SignInRefusalBody(clientLimitPrompt)",
                                           "return T_(body->key, body->english);",
                                           "error.empty()"}));
  // no sign-in screen shows a refusal's raw words by itself any more
  UR_EXPECT_FALSE(wiring::Contains(window, ": r.error.c_str());"));
  UR_EXPECT_FALSE(wiring::Contains(window, "SetLoginError(result.error);"));

  const std::string views = wiring::ReadCode("AuthViews.cpp");
  UR_EXPECT_TRUE(ClientLimitInOrder(wiring::FunctionBody(views, "void CreateNetworkPage::OnContinue() {"),
                                    {"ClientLimitRefusalText(r)", "error_creating_network"}));
  UR_EXPECT_TRUE(ClientLimitInOrder(views, {"host_.VerifyCode(userAuth_, code,",
                                            "ClientLimitRefusalText(r)", "verify_input_invalid"}));
}

// The window shows the hold while a connection is requested and the sdk's
// tracker decides the notification, which goes only while the app is away.
// Nothing here disconnects: the traffic stays in the tunnel until the user acts.
UR_TEST(ClientLimitWiring_TheWindowShowsTheHoldWhileAConnectionIsRequested) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(ClientLimitInOrder(window, {"host_.SetClientLimitHandler(",
                                             "PostToMain([this, reading] { ApplyClientLimit(reading); });"}));
  const std::string apply = wiring::FunctionBody(
      window, "void MainWindow::ApplyClientLimit(const client_limit::Reading& reading) {");
  UR_EXPECT_TRUE(ClientLimitInOrder(apply, {"reading.serial <= clientLimitReading_.serial",
                                            "clientLimitReading_ = reading;", "ObserveClientLimit();"}));
  const std::string observe = wiring::FunctionBody(window, "void MainWindow::ObserveClientLimit() {");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      observe, {"signedIn_ && reading_.destinationSelected",
                "connectPage_->ApplyClientLimitNotice(",
                "client_limit::NoticeFor(clientLimitReading_, connectRequested)",
                "if (clientLimitReading_.available)",
                "ApplyClientLimitNotification(clientLimitNotice_.update(status, connectRequested));"}));
  // the connect request moves the notice with it
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window, "void MainWindow::ApplyConnectReading(const ConnectReading& reading) {"),
      "ObserveClientLimit();"));

  const std::string notify = wiring::FunctionBody(
      window, "void MainWindow::ApplyClientLimitNotification(const std::string& trackerAction) {");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      notify, {"!windowVisible_ || !appFocused_",
               "client_limit::NotificationStepFor(ToTrackerAction(trackerAction), background)",
               "Gio::Notification::create(T_(client_limit::kTitle.key, client_limit::kTitle.english))",
               "Gio::Notification::Priority::LOW",
               "app->send_notification(kClientLimitNoticeId, notification);",
               "app->withdraw_notification(kClientLimitNoticeId);"}));
  for (const std::string& body : {observe, notify, apply}) {
    UR_EXPECT_FALSE(wiring::Contains(body, "ToggleConnect("));
    UR_EXPECT_FALSE(wiring::Contains(body, "Disconnect"));
  }
  // the tracker's answers by the sdk's values
  const std::string actions =
      wiring::FunctionBody(window, "client_limit::TrackerAction ToTrackerAction(const std::string& action) {");
  UR_EXPECT_TRUE(ClientLimitInOrder(actions, {"urnet::ClientLimitNoticePost",
                                              "urnet::ClientLimitNoticeUpdate",
                                              "urnet::ClientLimitNoticeRemove"}));
  // in front means a toplevel of the app is active
  UR_EXPECT_TRUE(ClientLimitInOrder(wiring::FunctionBody(window, "void MainWindow::ScheduleAppFocusSync() {"),
                                    {"gtk_window_is_active(", "appFocused_ = focused;"}));
}

// A sign-out ends the departed account's hold and its notification, and the
// next account's first hold alerts again.
UR_TEST(ClientLimitWiring_ASignOutResetsTheTracker) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  const std::string auth = wiring::FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {");
  UR_EXPECT_TRUE(ClientLimitInOrder(auth, {"} else {", "clientLimitReading_.available = false;",
                                           "ApplyClientLimitNotification(clientLimitNotice_.reset());",
                                           "clientLimitPlanLoaded_ = false;"}));
}

// The plan turning Pro in this run under a hold ends the hold at once; a launch,
// where the plan starts unread, never does (a Pro account held at the Pro limit
// would otherwise redial on every start).
UR_TEST(ClientLimitWiring_AnUpgradeSeenInThisRunRetries) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      window, {"const bool upgraded = clientLimitPlanLoaded_ && !clientLimitPlanPro_ && planLoaded && planPro;",
               "clientLimitPlanLoaded_ = planLoaded;", "clientLimitPlanPro_ = planPro;",
               "if (upgraded && client_limit::Held(clientLimitReading_)) {",
               "host_.RetryClientLimit(\"the plan turned Pro\");"}));
  UR_EXPECT_EQ(2u, wiring::CountOf(window, "host_.RetryClientLimit("));
}

// The Connect page's notice: Get Pro is the window's upgrade path (a guest adds a
// sign-in first), and Retry dials again at once. Get Pro shows on a free plan.
UR_TEST(ClientLimitWiring_TheNoticeOffersTheUpgradePathAndRetry) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Contains(window, "connectPage_->on_client_limit_upgrade = [this] { OpenUpgrade(); };"));
  UR_EXPECT_TRUE(wiring::Contains(
      window, "connectPage_->on_client_limit_retry = [this] { host_.RetryClientLimit(\"the notice's Retry\"); };"));

  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  UR_EXPECT_TRUE(ClientLimitInOrder(
      page, {"clientLimitAlert_ = Gtk::make_managed<Gtk::Box>(",
             "T_(client_limit::kTitle.key, client_limit::kTitle.english)",
             "clientLimitUpgrade_ = Gtk::make_managed<Gtk::Button>(T_(\"get_pro\", \"Get Pro\"));",
             "if (on_client_limit_upgrade) on_client_limit_upgrade();",
             "Gtk::make_managed<Gtk::Button>(T_(\"retry\", \"Retry\"));",
             "if (on_client_limit_retry) on_client_limit_retry();",
             "paneAContent_->append(*clientLimitAlert_);"}));
  const std::string render = wiring::FunctionBody(
      page, "void ConnectPage::ApplyClientLimitNotice(const client_limit::Notice& notice) {");
  UR_EXPECT_TRUE(wiring::Contains(render, "clientLimitAlert_->set_visible(notice.shows);"));
  UR_EXPECT_TRUE(wiring::Contains(render, "T_(notice.body.key, notice.body.english)"));
  UR_EXPECT_TRUE(wiring::Contains(render, "clientLimitUpgrade_->set_visible(notice.offersUpgrade);"));
}
