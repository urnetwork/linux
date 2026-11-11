// The Pro features notice's call sites (ProFeaturesNotice.hpp; owner decision
// 2026-11-11: SOCKS and WireGuard hosted proxies are Pro features). The words
// are pure and run in ProFeaturesNoticeTest.cpp; the surfaces need GTK and the
// SDK, so this reads their sources with the comments blanked
// (WiringSource.hpp), so prose cannot satisfy a contract:
//   - the window opens the sdk's ProFeaturesViewController on the Api this
//     process holds at each sign-in and starts it, keeps it for a sign-in again
//     on the same Api and network, and closes it at a sign-out and with the
//     window, so the next sign-in's notice alerts again; its listener only
//     marshals, and the main loop reads the latest snapshot;
//   - it reads the proxies again when the window comes to the front, when the
//     plan's Pro changes and when the device list closes;
//   - the Connect page shows the snapshot's MainPrompt under the client limit
//     notice, with Get Pro (the client limit notice's upgrade path), Manage
//     devices (the same list) and a close (the sdk's dismissal); nothing here
//     disconnects or sends a desktop notification;
//   - the device list says its snapshot's ProFeatures prompt above the rows,
//     with Get Pro except in a refused sign-in's list, and no close.
// SPDX-License-Identifier: MPL-2.0
#include <string>
#include <vector>

#include "TestHarness.hpp"
#include "WiringSource.hpp"

namespace {

namespace wiring = urnw::testing::wiring;

// Each needle occurs after the one before it.
bool ProFeaturesInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// One controller per sign-in, on the Api of this process (tunnel up or not):
// opened and started at a sign-in, kept for a sign-in again on the same Api
// and network, closed at a sign-out and with the window.
UR_TEST(ProFeaturesWiring_OneControllerPerSignIn) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  const std::string open = wiring::FunctionBody(window, "void MainWindow::OpenProFeatures() {");
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      open, {"if (!host_.IsLoggedIn()) return;", "const uint64_t api = host_.api().handle();",
             "proFeaturesVc_ && api == proFeaturesApi_", "RefreshProFeatures(", "return;",
             "CloseProFeatures();", "host_.api().openProFeaturesViewController();",
             "proFeaturesSub_ = proFeaturesVc_->addProFeaturesListener(",
             // the listener marshals, and a post from a closed one lands nowhere
             "PostToMain([this, epoch, issued] {", "if (*epoch != issued) return;",
             "ApplyProFeatures();", "proFeaturesVc_->start();"}));

  // unsubscribed, then closed; and no widget, since the window's end runs it
  const std::string close = wiring::FunctionBody(window, "void MainWindow::CloseProFeatures() {");
  UR_EXPECT_TRUE(ProFeaturesInOrder(close, {"++*proFeaturesEpoch_;", "proFeaturesSub_.reset();",
                                            "proFeaturesVc_->close();", "proFeaturesVc_.reset();"}));
  UR_EXPECT_FALSE(wiring::Contains(close, "connectPage_"));
  UR_EXPECT_TRUE(
      wiring::Contains(wiring::FunctionBody(window, "MainWindow::~MainWindow() {"), "CloseProFeatures();"));

  // opened with the sign-in, closed with the sign-out, and its notice with it
  const std::string auth = wiring::FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {");
  UR_EXPECT_TRUE(ProFeaturesInOrder(auth, {"host_.CryptoInvoice().SignIn();", "OpenProFeatures();",
                                           "} else {", "CloseProFeatures();", "ApplyProFeatures();"}));
  UR_EXPECT_EQ(1u, wiring::CountOf(window, "host_.api().openProFeaturesViewController()"));
}

// The proxies are read again when the window comes to the front, when the
// plan's Pro changes in this run, and when the device list closes; never while
// signed out.
UR_TEST(ProFeaturesWiring_TheProxiesAreReadAgain) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      wiring::FunctionBody(window, "void MainWindow::RefreshProFeatures(const char* reason) {"),
      {"if (!signedIn_ || !proFeaturesVc_) return;", "proFeaturesVc_->refresh();"}));

  // in front: shown, and a toplevel of the app active
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      wiring::FunctionBody(window, "void MainWindow::ObserveProFeaturesFront() {"),
      {"const bool inFront = windowVisible_ && appFocused_;", "proFeaturesInFront_ = inFront;",
       "if (inFront) RefreshProFeatures("}));
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      window, {"auto reconcilePresentation = [this] {", "windowVisible_ = presentationActive;",
               "ObserveProFeaturesFront();",
               "property_visible().signal_changed().connect(reconcilePresentation);"}));
  UR_EXPECT_TRUE(ProFeaturesInOrder(wiring::FunctionBody(window, "void MainWindow::ScheduleAppFocusSync() {"),
                                    {"appFocused_ = focused;", "ObserveProFeaturesFront();"}));

  // the plan's Pro changed in this run, either way: never at the first read
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      window, {"const bool planChanged = clientLimitPlanLoaded_ && planLoaded && planPro != clientLimitPlanPro_;",
               "clientLimitPlanPro_ = planPro;", "if (planChanged) {", "RefreshProFeatures("}));

  // the device list closed, however it closed
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window,
                           "void MainWindow::ShowManageDevicesSheet(bool forSignIn, const Glib::ustring& message) {"),
      "manageDevicesSheet_->on_closed = [this] { RefreshProFeatures(\"the device list closed\"); };"));
  const std::string sheet = wiring::ReadCode("ManageDevicesSheet.cpp");
  UR_EXPECT_TRUE(ProFeaturesInOrder(wiring::FunctionBody(sheet, "void ManageDevicesSheet::OnHidden() {"),
                                    {"if (!*alive_) return;", "host_.CloseManageDevices();",
                                     "if (on_closed) on_closed();"}));
}

// The Connect page shows the controller's MainPrompt under the client limit
// notice: Get Pro is the client limit notice's upgrade path, Manage devices
// the same list, and the close the sdk's dismissal. Nothing disconnects,
// holds a connect back or notifies the desktop.
UR_TEST(ProFeaturesWiring_TheConnectPageShowsTheMainPrompt) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(ProFeaturesInOrder(wiring::FunctionBody(window, "void MainWindow::ApplyProFeatures() {"),
                                    {"if (signedIn_ && proFeaturesVc_) {", "proFeaturesVc_->getSnapshot()",
                                     "prompt = ToProFeaturesPrompt(snapshot->MainPrompt);",
                                     "connectPage_->ApplyProFeaturesNotice(pro_features::NoticeFor(prompt));"}));
  const std::string prompt =
      wiring::FunctionBody(window, "pro_features::Prompt ToProFeaturesPrompt(const std::string& prompt) {");
  UR_EXPECT_TRUE(wiring::Contains(prompt, "urnet::ProFeaturesPromptUpgrade"));
  UR_EXPECT_TRUE(wiring::Contains(prompt, "urnet::ProFeaturesPromptStopped"));

  UR_EXPECT_TRUE(wiring::Contains(window, "connectPage_->on_client_limit_upgrade = [this] { OpenUpgrade(); };"));
  UR_EXPECT_TRUE(wiring::Contains(window, "connectPage_->on_pro_features_upgrade = [this] { OpenUpgrade(); };"));
  UR_EXPECT_TRUE(wiring::Contains(
      window, "connectPage_->on_pro_features_manage_devices = [this] { ShowManageDevicesSheet(false, {}); };"));
  UR_EXPECT_TRUE(
      wiring::Contains(window, "connectPage_->on_pro_features_dismiss = [this] { DismissProFeaturesNotice(); };"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(window, "void MainWindow::DismissProFeaturesNotice() {"),
                                  "proFeaturesVc_->dismissMainNotice();"));
  UR_EXPECT_EQ(1u, wiring::CountOf(window, "dismissMainNotice("));
  for (const char* signature :
       {"void MainWindow::OpenProFeatures() {", "void MainWindow::CloseProFeatures() {",
        "void MainWindow::RefreshProFeatures(const char* reason) {", "void MainWindow::ApplyProFeatures() {",
        "void MainWindow::DismissProFeaturesNotice() {", "void MainWindow::ObserveProFeaturesFront() {"}) {
    const std::string body = wiring::FunctionBody(window, signature);
    UR_EXPECT_TRUE_MSG(signature, !body.empty());
    UR_EXPECT_FALSE(wiring::Contains(body, "ToggleConnect("));
    UR_EXPECT_FALSE(wiring::Contains(body, "Disconnect"));
    UR_EXPECT_FALSE(wiring::Contains(body, "send_notification"));
  }

  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      page, {"paneAContent_->append(*clientLimitAlert_);",
             "proFeaturesAlert_ = Gtk::make_managed<Gtk::Box>(",
             "T_(pro_features::kInUseTitle.key, pro_features::kInUseTitle.english)",
             "close->set_tooltip_text(T_(\"close\", \"Close\"));",
             "kit::SetAccessibleLabel(*close, T_(\"close\", \"Close\"));",
             "if (on_pro_features_dismiss) on_pro_features_dismiss();",
             "Gtk::make_managed<Gtk::Button>(T_(\"get_pro\", \"Get Pro\"));",
             "if (on_pro_features_upgrade) on_pro_features_upgrade();",
             "T_(manage_devices::kTitleText.key, manage_devices::kTitleText.english)",
             "if (on_pro_features_manage_devices) on_pro_features_manage_devices();",
             "proFeaturesAlert_->set_visible(false);", "paneAContent_->append(*proFeaturesAlert_);"}));
  const std::string render =
      wiring::FunctionBody(page, "void ConnectPage::ApplyProFeaturesNotice(const pro_features::Notice& notice) {");
  UR_EXPECT_TRUE(wiring::Contains(render, "proFeaturesAlert_->set_visible(notice.shows);"));
  UR_EXPECT_TRUE(wiring::Contains(render, "T_(notice.title.key, notice.title.english)"));
  UR_EXPECT_TRUE(wiring::Contains(render, "T_(notice.body.key, notice.body.english)"));
}

// The device list says its snapshot's prompt above the rows, by the sdk's
// prompt values, with Get Pro (the window's upgrade path, after the sheet
// steps aside) except in a refused sign-in's list; it has no close.
UR_TEST(ProFeaturesWiring_TheDeviceListSaysThePrompt) {
  const std::string sheet = wiring::ReadCode("ManageDevicesSheet.cpp");
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      wiring::FunctionBody(sheet, "manage_devices::Snapshot PlainOf(const urnet::ManageDevicesSnapshot& snapshot) {"),
      {"if (snapshot.ProFeatures) {", "urnet::ProFeaturesPromptUpgrade",
       "plain.proFeatures = pro_features::Prompt::Upgrade;", "urnet::ProFeaturesPromptStopped",
       "plain.proFeatures = pro_features::Prompt::Stopped;"}));
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      wiring::FunctionBody(sheet, "void ManageDevicesSheet::BuildUi() {"),
      {"root->append(*message_);", "proFeaturesBox_ = Gtk::make_managed<Gtk::Box>(",
       "proFeaturesGetPro_ = Gtk::make_managed<Gtk::Button>(T_(\"get_pro\", \"Get Pro\"));",
       "set_visible(false);", "if (on_get_pro) on_get_pro();", "root->append(*proFeaturesBox_);",
       "root->append(*listBox_);"}));
  UR_EXPECT_TRUE(ProFeaturesInOrder(
      wiring::FunctionBody(sheet, "void ManageDevicesSheet::Render() {"),
      {"pro_features::NoticeFor(snapshot_.proFeatures)", "proFeaturesBox_->set_visible(proFeatures.shows);",
       "T_(proFeatures.title.key, proFeatures.title.english)",
       "T_(proFeatures.body.key, proFeatures.body.english)",
       "proFeaturesGetPro_->set_visible(pro_features::ListOffersUpgrade(forSignIn_));"}));
  // the user's close is the Connect page notice's alone
  UR_EXPECT_FALSE(wiring::Contains(sheet, "dismissMainNotice"));

  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window,
                           "void MainWindow::ShowManageDevicesSheet(bool forSignIn, const Glib::ustring& message) {"),
      "manageDevicesSheet_->on_get_pro = [this] { OpenUpgrade(); };"));
  // both tests are built into the suite (meson's comments are not C++'s)
  const std::string build = wiring::ReadRaw("../meson.build");
  UR_EXPECT_TRUE(wiring::Contains(build, "'tests/ProFeaturesNoticeTest.cpp',"));
  UR_EXPECT_TRUE(wiring::Contains(build, "'tests/ProFeaturesWiringTest.cpp',"));
}
