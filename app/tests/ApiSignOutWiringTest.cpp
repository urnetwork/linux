// The GUI's Api reports its own sign-out to the app. The sdk clears the
// account credential the server rejects, or the one the account signs out
// from its Sessions list (ClientSessionViewController's self-revoke), and
// says so on that Api (Api.addAuthLogoutListener). The GUI listened only on
// the daemon's device, so with no tunnel up, or for the account credential
// the device does not carry, the app went on looking signed in on a
// credential that was gone. REVOKE-UI-FINAL.md §4: after a successful
// self-sign-out the app follows its normal logout flow, whether or not the
// Sessions page is still on screen.
//
// And it says why (§5, AuthLogout.hpp): each listener reads the sdk's cause on
// its own thread and reports it with the sign-in it was heard in; the window
// signs out on the first report of a rejection only (the Api's and the bound
// DeviceRemote's listeners both report one), and on the sign-in page says
// "This session was signed out from another device." once, for the
// session-revoked cause alone. Before this the handler took no cause, and the
// two listeners of one rejection ran Logout() twice. SdkHost and MainWindow
// need the SDK and GTK, so this reads their sources.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "WiringSource.hpp"

using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;

// Every Api the GUI adopts (ClientInfoWiringTest pins that each `api_ =` is
// adopted) reports its sign-out, with the cause its listener reads through a
// handle of its own, and drops the previous Api's subscription first.
UR_TEST(ApiSignOut_EveryAdoptedApiReportsItsSignOut) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string adopt = FunctionBody(host, "void SdkHost::AdoptSpaceApiLocked()");
  UR_EXPECT_TRUE_MSG("SdkHost::AdoptSpaceApiLocked is defined", !adopt.empty());
  UR_EXPECT_TRUE(Before(adopt, "apiLogoutSub_.reset();",
                        "apiLogoutSub_.emplace(api_->addAuthLogoutListener("));
  UR_EXPECT_TRUE(Before(adopt, "auto api = std::make_shared<urnet::Api>(networkSpace_->getApi());",
                        "apiLogoutSub_.emplace(api_->addAuthLogoutListener([this, api] {"));
  const size_t listen = adopt.find("api_->addAuthLogoutListener(");
  UR_EXPECT_TRUE(listen != std::string::npos &&
                 adopt.find("ReportAuthLogout(api->getAuthLogoutCause());", listen) !=
                     std::string::npos);
  // the report carries the sign-in it was heard in, to the handler
  UR_EXPECT_TRUE(Contains(FunctionBody(host, "void SdkHost::ReportAuthLogout("),
                          "if (onAuthInvalid_) onAuthInvalid_(authLogouts_.Hear(std::move(cause)));"));
}

// The device's listener reports to the same place, with the device's own
// cause, read through the handle it was added on.
UR_TEST(ApiSignOut_TheDeviceReportsItsOwnCause) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string bind = FunctionBody(host, "TunnelStartResult SdkHost::BindRemoteDeviceLocked(");
  UR_EXPECT_TRUE(Before(bind, "const uint64_t deviceHandle = device_->handle();",
                        "device_->addAuthLogoutListener([this, deviceHandle] {"));
  UR_EXPECT_TRUE(Contains(bind, "ReportAuthLogout(DeviceAuthLogoutCause(deviceHandle));"));
  const std::string read = FunctionBody(host, "std::string DeviceAuthLogoutCause(uint64_t device)");
  UR_EXPECT_TRUE(Before(read, "urnet_device_get_auth_logout_cause(device);",
                        "urnet_free_string(cause);"));
  UR_EXPECT_TRUE(Contains(read, "if (cause == nullptr) return std::string();"));
  // nothing else hands the handler a report: every report is a listener's
  UR_EXPECT_EQ(1u, CountOf(host, "onAuthInvalid_("));
  UR_EXPECT_EQ(2u, CountOf(host, "ReportAuthLogout(") - CountOf(host, "SdkHost::ReportAuthLogout("));
  // the mirrored cause is the sdk's, or the GUI does not build
  UR_EXPECT_TRUE(Contains(host, "static_assert(std::string_view(auth_logout::kSessionRevoked) ==\n"
                                "                  std::string_view(urnet::AuthLogoutCauseSessionRevoked),"));
}

// The subscription closes before the handler it calls and the Api it listens
// to are destroyed: members die in reverse order of declaration. What every
// listener reads, the sign-in tracker, outlives them all.
UR_TEST(ApiSignOut_TheSubscriptionClosesFirst) {
  const std::string header = ReadCode("SdkHost.hpp");
  UR_EXPECT_TRUE(Before(header, "std::optional<urnet::Api> api_;",
                        "std::optional<urnet::Sub> apiLogoutSub_;"));
  UR_EXPECT_TRUE(Before(header, "AuthInvalidHandler onAuthInvalid_;",
                        "std::optional<urnet::Sub> apiLogoutSub_;"));
  UR_EXPECT_TRUE(Before(header, "auth_logout::Tracker authLogouts_;",
                        "std::vector<urnet::Sub> subs_;"));
  UR_EXPECT_TRUE(Before(header, "auth_logout::Tracker authLogouts_;",
                        "std::optional<urnet::Sub> apiLogoutSub_;"));
  UR_EXPECT_TRUE(Contains(header, "using AuthInvalidHandler = std::function<void(auth_logout::Report report)>;"));
  UR_EXPECT_TRUE(Contains(header, "bool SignsOut(const auth_logout::Report& report) const { return authLogouts_.SignsOut(report); }"));
}

// The tracker follows the GUI's sign-in wherever signedOut_ moves: every
// sign-out, every sign-in's stored credential, a server switch either way,
// and a launch with a stored sign-in. A sign-in the tracker missed would let
// no report sign it out; a sign-out it missed would let the second listener
// sign out again.
UR_TEST(ApiSignOut_TheTrackerFollowsEverySignInAndSignOut) {
  const std::string host = ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE(Before(FunctionBody(host, "void SdkHost::Logout() {"), "signedOut_.store(true);",
                        "authLogouts_.SignedOut();"));
  const std::string registered = FunctionBody(host, "void SdkHost::RegisterNetworkClient(");
  UR_EXPECT_TRUE(Before(registered, "signedOut_.store(false);", "authLogouts_.SignedIn();"));
  UR_EXPECT_TRUE(Before(registered, "authLogouts_.SignedIn();", "onAuth_(true);"));
  const std::string server = FunctionBody(host, "bool SdkHost::ApplyNetworkServer(");
  UR_EXPECT_TRUE(Before(server, "signedOut_.store(!loggedIn);", "if (loggedIn) {\n        authLogouts_.SignedIn();\n      } else {\n        authLogouts_.SignedOut();"));
  const std::string initialize = FunctionBody(host, "bool SdkHost::Initialize(");
  UR_EXPECT_TRUE(Before(initialize, "localState_ = asyncLocalState_->getLocalState();",
                        "if (!localState_->getByClientJwt().empty()) authLogouts_.SignedIn();"));
  // ...and nowhere else moves signedOut_ without it
  for (size_t at = host.find("signedOut_.store("); at != std::string::npos;
       at = host.find("signedOut_.store(", at + 1)) {
    const size_t next = host.find(';', at);
    const std::string after = host.substr(next, 200);
    UR_EXPECT_TRUE_MSG(host.substr(at, next - at), Contains(after, "authLogouts_.Signed"));
  }
  UR_EXPECT_EQ(3u, CountOf(host, "signedOut_.store("));
}

// The handler marshals the report to the main loop; there the first report of
// a rejection owes the notice and runs the app's sign-out, and any other report
// does nothing.
UR_TEST(ApiSignOut_TheHandlerRunsTheNormalLogoutOnce) {
  const std::string window = ReadCode("MainWindow.cpp");
  const size_t handler = window.find("host_.SetAuthInvalidHandler([this](auth_logout::Report report) {");
  UR_EXPECT_TRUE(handler != std::string::npos);
  const size_t post = window.find("PostToMain([this, report] { OnAuthLogout(report); });", handler);
  UR_EXPECT_TRUE(post != std::string::npos && post - handler < 120);
  const std::string onLogout = FunctionBody(window, "void MainWindow::OnAuthLogout(");
  UR_EXPECT_TRUE(Before(onLogout, "if (!host_.SignsOut(report)) {", "signInNotice_.Arm(report.cause);"));
  UR_EXPECT_TRUE(Before(onLogout, "signInNotice_.Arm(report.cause);", "host_.Logout();"));
  const size_t dropped = onLogout.find("if (!host_.SignsOut(report)) {");
  UR_EXPECT_TRUE(dropped != std::string::npos &&
                 onLogout.find("return;", dropped) < onLogout.find("signInNotice_.Arm("));
  // only that report owes it: the window's explicit sign-outs (Account's Sign
  // out, a deleted account) call Logout() and owe nothing
  UR_EXPECT_EQ(1u, CountOf(window, "signInNotice_.Arm("));
  UR_EXPECT_EQ(0u, CountOf(ReadCode("AccountPage.cpp"), "signInNotice_"));
}

// The sign-out lands on the sign-in page with a clean line and then, once, the
// notice owed; a sign-in drops a notice never shown.
UR_TEST(ApiSignOut_TheSignInPageSaysWhyOnce) {
  const std::string window = ReadCode("MainWindow.cpp");
  const std::string apply = FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn)");
  const size_t signedOut = apply.find("} else {");
  UR_EXPECT_TRUE(signedOut != std::string::npos);
  const std::string out = apply.substr(signedOut);
  UR_EXPECT_TRUE(Before(out, "loginError_.set_text(\"\");", "if (signInNotice_.Take()) {"));
  UR_EXPECT_TRUE(Before(out, "if (signInNotice_.Take()) {",
                        "SetLoginNotice(\n          T_(\"sessions_signed_out_remotely\", \"This session was "
                        "signed out from another device.\"));"));
  UR_EXPECT_TRUE(Contains(apply.substr(0, signedOut), "signInNotice_.Drop();"));
  UR_EXPECT_EQ(1u, CountOf(window, "signInNotice_.Take()"));
  // the notice voice of the sign-in page's line, not its error voice
  const std::string notice = FunctionBody(window, "void MainWindow::SetLoginNotice(");
  UR_EXPECT_TRUE(Contains(notice, "loginError_.add_css_class(\"dim-label\");"));
}
