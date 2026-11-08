// The device list's call sites (ManageDevicesSheet, SdkHost's device list and
// refused sign-in). Before them a Pro network held at its limit was told to
// sign out of a device it no longer uses with no way to see or remove one from
// this app, and a sign-in the limit refused could only be tried again after
// removing a device somewhere else. The words are pure and run in
// ManageDevicesPresentationTest.cpp; the surfaces need GTK and the SDK, so this
// reads their sources with the comments blanked (WiringSource.hpp):
//   - SdkHost opens the sdk's list on the bound device when there is one (a
//     removal ends its hold), else on the Api with the stored sign-in token, and
//     for a refused sign-in with that sign-in's token and a retry that runs its
//     registration again; snapshots of an older open are dropped, the listener
//     takes no lock of the host's, and a device teardown moves the list to the
//     Api first;
//   - a sign-in the limit refused is kept until one goes through or a sign-out,
//     and offered to the window from whichever screen signed in;
//   - the window opens the list from the notice's Manage devices (Pro), from
//     Settings > Device and after a refused sign-in, and takes it down when its
//     account goes;
//   - the sheet renders the snapshots, confirms a removal inside itself, and
//     closes the list when it hides.
// SPDX-License-Identifier: MPL-2.0
#include <string>
#include <vector>

#include "TestHarness.hpp"
#include "WiringSource.hpp"

namespace {

namespace wiring = urnw::testing::wiring;

// Each needle occurs after the one before it.
bool ManageDevicesInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(ManageDevicesWiring_TheHostOpensTheListOnTheRightThing) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  // asked for on the main loop, opened on the command queue's worker
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::OpenManageDevices(bool forSignIn, ManageDevicesHandler handler) {"),
      {"manageDevicesGeneration_.fetch_add(1) + 1;", "manageDevicesForSignIn_ = forSignIn;",
       "RunCommand(ConnectCommandQueue::Kind::Device,", "std::scoped_lock lock(mutex_);",
       "OpenManageDevicesLocked(generation);"}));
  const std::string open =
      wiring::FunctionBody(host, "void SdkHost::OpenManageDevicesLocked(uint64_t generation) {");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      open, {"if (generation != manageDevicesGeneration_.load()) return;",
             "CloseManageDevicesVcLocked();", "if (!manageDevicesForSignIn_ && device_) {",
             "device_->openManageDevicesViewController();",
             "manageDevicesOwner_ = ManageDevicesOwner::Device;",
             "OpenManageDevicesOnApiLocked(manageDevicesForSignIn_);",
             // nothing to open with: the sheet hears canManage false
             "ManageDevicesUpdate{urnet::ManageDevicesSnapshot{}, false}",
             "StartManageDevicesLocked(generation);"}));
  const std::string onApi =
      wiring::FunctionBody(host, "bool SdkHost::OpenManageDevicesOnApiLocked(bool forSignIn) {");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      onApi, {"if (forSignIn) {", "byJwt = refusedSignIn_->byJwt;",
              "retry = [this] { RetryRefusedSignIn(); };", "} else {",
              "if (signedOut_.load() || !localState_) return false;",
              "byJwt = localState_->getByJwt();",
              "urnet::newSignInManageDevicesViewController(*api_, byJwt, std::move(retry));"}));
  // the listener copies the handler, drops an older open's snapshot and takes
  // no lock of this host's (the list calls it from inside start too)
  const std::string start =
      wiring::FunctionBody(host, "void SdkHost::StartManageDevicesLocked(uint64_t generation) {");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      start, {"manageDevicesVc_.getCanManage()", "const auto handler = manageDevicesHandler_;",
              "manageDevicesVc_.addManageDevicesListener(",
              "if (generation != manageDevicesGeneration_.load()) return;",
              "(*handler)(ManageDevicesUpdate{std::move(*snapshot), canManage});",
              "manageDevicesVc_.start();"}));
  UR_EXPECT_FALSE(wiring::Contains(start, "lock(mutex_)"));
  UR_EXPECT_FALSE(wiring::Contains(start, "lock(manageDevicesMutex_)"));
  // closed through the device it was opened on while it is bound
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::CloseManageDevicesVcLocked() {"),
      {"manageDevicesSub_ = urnet::Sub{};",
       "manageDevicesOwner_ == ManageDevicesOwner::Device && device_",
       "device_->closeManageDevicesViewController(manageDevicesVc_);",
       "manageDevicesVc_.close();"}));
  // the sheet's close: its handler hears nothing more, and the list closes on
  // the worker
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::CloseManageDevices() {"),
      {"manageDevicesGeneration_.fetch_add(1);", "manageDevicesHandler_.reset();",
       "RunCommand(ConnectCommandQueue::Kind::Device,", "CloseManageDevicesVcLocked();"}));
  // a device going away under its list moves the list to the Api first
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::TeardownDeviceLocked() {"),
      {"ReopenManageDevicesOffDeviceLocked();", "device_->close();"}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::ReopenManageDevicesOffDeviceLocked() {"),
      // (the reads blank the /*forSignIn=*/ comment before the false)
      {"if (manageDevicesOwner_ != ManageDevicesOwner::Device) return;",
       "CloseManageDevicesVcLocked();", "OpenManageDevicesOnApiLocked(", "false))",
       "StartManageDevicesLocked(generation);"}));
}

// A sign-in the client limit refused is kept for its list, offered to the
// window from whichever screen signed in, ended by one that goes through and by
// a sign-out, and run again, with its own token and epoch, by the list's retry.
UR_TEST(ManageDevicesWiring_ARefusedSignInIsKeptAndRetried) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string registration = wiring::FunctionBody(
      host, "void SdkHost::RegisterNetworkClient(const std::string& byJwt,");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      registration, {"AuthNetworkClientWithLocale(args, [this, byJwt, done, authEpoch](",
                     "refused.clientLimitPrompt != client_limit::Prompt::None",
                     "refusedSignIn_ = RefusedSignIn{byJwt, authEpoch};",
                     "if (onClientLimitRefusal_) onClientLimitRefusal_(refused.clientLimitPrompt);",
                     "done(refused);"}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::FinishNetworkClientRegistration("),
      {"published = true;", "if (published) {", "refusedSignIn_.reset();"}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(wiring::FunctionBody(host, "void SdkHost::Logout() {"),
                                      {"refusedSignIn_.reset();", "signedOut_.store(true);"}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(host, "void SdkHost::RetryRefusedSignIn() {"),
      {"refused = refusedSignIn_;", "if (!refused) return;", "RegisterNetworkClient(",
       "refused->byJwt,", "if (onRefusedSignInRetry_) onRefusedSignInRetry_(r);",
       "refused->epoch);"}));
}

// The window opens the list from the notice's Manage devices, from Settings >
// Device and after a refused sign-in (with the refusal's words above it), says
// what a retry answered, and takes the list down when its account goes.
UR_TEST(ManageDevicesWiring_TheWindowOpensAndClosesTheList) {
  const std::string window = wiring::ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(ManageDevicesInOrder(window, {"host_.SetClientLimitRefusalHandler(",
                                               "OfferSignInManageDevices(prompt);",
                                               "host_.SetRefusedSignInRetryHandler(",
                                               "OnRefusedSignInRetry(result);"}));
  UR_EXPECT_TRUE(wiring::Contains(
      window,
      "connectPage_->on_client_limit_manage_devices = [this] { ShowManageDevicesSheet(false, {}); };"));
  UR_EXPECT_TRUE(wiring::Contains(
      window, "settingsPage_->on_open_manage_devices = [this] { ShowManageDevicesSheet(false, {}); };"));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(window,
                           "void MainWindow::ShowManageDevicesSheet(bool forSignIn, const Glib::ustring& message) {"),
      "manageDevicesSheet_->Open(forSignIn, message);"));
  const std::string offer =
      wiring::FunctionBody(window, "void MainWindow::OfferSignInManageDevices(client_limit::Prompt prompt) {");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      offer, {"client_limit::SignInRefusalBody(prompt);", "manageDevicesSheet_->ForSignIn()",
              "manageDevicesSheet_->SetMessage(T_(body->key, body->english));",
              "if (host_.HasRefusedSignIn()) ShowManageDevicesSheet(true, T_(body->key, body->english));"}));
  const std::string retried =
      wiring::FunctionBody(window, "void MainWindow::OnRefusedSignInRetry(const AuthResult& result) {");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      retried, {"if (result.ok) {", "StartTunnelUi(",
                "if (result.clientLimitPrompt != client_limit::Prompt::None) return;",
                "manageDevicesSheet_->SetMessage(SignInFailureText("}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {"),
      {"manageDevicesSheet_->ForSignIn() == loggedIn", "manageDevicesSheet_->Dismiss();"}));
}

// The notice offers the list on a Pro plan, where no upgrade helps; Settings >
// Device carries a row after Spec.
UR_TEST(ManageDevicesWiring_TheNoticeAndSettingsOfferTheList) {
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      page, {"clientLimitManage_ =", "manage_devices::kTitleText.key",
             "if (on_client_limit_manage_devices) on_client_limit_manage_devices();",
             "actions->append(*clientLimitManage_);"}));
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(page, "void ConnectPage::ApplyClientLimitNotice(const client_limit::Notice& notice) {"),
      "clientLimitManage_->set_visible(!notice.offersUpgrade);"));

  const std::string settings = wiring::ReadCode("SettingsPage.cpp");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(settings, "void SettingsPage::BuildDeviceSection(Gtk::Box& host) {"),
      {"T_(\"device_spec_label\", \"Spec\")", "manage_devices::kTitleText.key",
       "if (on_open_manage_devices) on_open_manage_devices();"}));
}

// The sheet renders the snapshots of its own open, confirms a removal inside
// itself, and closes the list when it hides, however it hides.
UR_TEST(ManageDevicesWiring_TheSheetRendersAndClosesItsList) {
  const std::string sheet = wiring::ReadCode("ManageDevicesSheet.cpp");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(sheet, "void ManageDevicesSheet::Open(bool forSignIn, const Glib::ustring& message) {"),
      {"const uint64_t issued = ++*epoch_;", "host_.OpenManageDevices(forSignIn,", "PostToMain(",
       "*current != issued", "Apply(update);", "present();"}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(sheet, {"property_visible().signal_changed().connect(",
                                              "if (!get_visible()) OnHidden();"}));
  UR_EXPECT_TRUE(ManageDevicesInOrder(wiring::FunctionBody(sheet, "void ManageDevicesSheet::OnHidden() {"),
                                      {"++*epoch_;", "host_.CloseManageDevices();"}));
  UR_EXPECT_TRUE(wiring::Contains(sheet, "host_.RefreshManageDevices();"));
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      wiring::FunctionBody(sheet, "void ManageDevicesSheet::AskRemove(const manage_devices::Device& device) {"),
      {"manage_devices::Removable(device)", "manage_devices::kRemoveBodyText.key",
       "confirmBox_->set_visible(true);"}));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(sheet, "void ManageDevicesSheet::ConfirmRemove() {"),
                                  "host_.RemoveManagedDevice(clientId);"));
  // the sdk's own spellings of its kinds and errors
  const std::string plain =
      wiring::FunctionBody(sheet, "manage_devices::Snapshot PlainOf(const urnet::ManageDevicesSnapshot& snapshot) {");
  UR_EXPECT_TRUE(ManageDevicesInOrder(
      plain, {"urnet::ManagedDeviceKindProxy", "SanitizeExternalDisplayText(device.Name)",
              "urnet::ManageDevicesErrorSignInRequired", "urnet::ManageDevicesErrorLoadFailed",
              "urnet::ManageDevicesErrorRemoveFailed"}));
  // built into the GUI
  UR_EXPECT_TRUE(wiring::Contains(wiring::ReadRaw("../meson.build"), "'src/ManageDevicesSheet.cpp',"));
}
