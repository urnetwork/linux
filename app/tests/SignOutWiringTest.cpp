// The call sites of signing out (SignOut.hpp, owner decisions 2026-10-05): the
// sign-out stops this user's tunnel and provider as Quit does, then has the
// daemon forget the account (its device identity and what its sdk stored), so
// each network starts fresh; it stays owed to a daemon that could not be told,
// and nothing of the signed-out account starts again. The delivery itself is
// pure and runs in SignOutTest.cpp; SdkHost and the daemon need glib and the
// SDK, so this reads their sources with the line comments blanked, so prose
// cannot satisfy a contract.
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

// The source with every // comment blanked; string literals are kept.
std::string ReadSignOutSource(const std::string& relative) {
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

std::string SignOutBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool SignOutHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool InOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// Logout: signed out at once, the local credentials logged out before anything
// can wait, the DeviceRemote down as Quit takes it, then the delivery through
// the obligation, not a best-effort stop.
UR_TEST(SignOutWiring_LogoutStopsAsQuitThroughTheObligation) {
  const std::string host = ReadSignOutSource("SdkHost.cpp");
  // signed out at once on the main loop, then the teardown on the queue
  UR_EXPECT_TRUE(InOrder(SignOutBody(host, "void SdkHost::Logout() {"),
                         {"if (signedOut_.exchange(true)) {", "RunCommand(", "LogoutNow();"}));
  const std::string logout = SignOutBody(host, "void SdkHost::LogoutNow() {");
  UR_EXPECT_TRUE(!logout.empty());
  UR_EXPECT_TRUE(InOrder(logout, {"std::scoped_lock lock(mutex_);",
                                  "asyncLocalState_->logout(", "TeardownDeviceLocked();",
                                  "signOut_.Begin(SignOutDaemonLocked());", "ForgetRpcSession();",
                                  "onAuth_(false);"}));
  UR_EXPECT_TRUE(SignOutHas(logout, "signOutBackoff_.NoteFailure("));
  for (const char* forbidden : {"control_.StopTunnel(", "control_.StartProvider(", "StartTunnel("}) {
    UR_EXPECT_TRUE_MSG(forbidden, !SignOutHas(logout, forbidden));
  }
  // Quit is still Quit: it stops and does not sign out
  const std::string shutdown = SignOutBody(host, "void SdkHost::Shutdown() {");
  UR_EXPECT_TRUE(SignOutHas(shutdown, "control_.StopTunnel();"));
  UR_EXPECT_TRUE(!SignOutHas(shutdown, "signOut_.") && !SignOutHas(shutdown, "logout("));
}

// The delivery: the session ensured, the status read for this uid, and
// another user's session left alone; stop_tunnel otherwise, then logout with
// the account's network space, where an older daemon's `unknown verb` and a
// refusal beside another user's live session count as done and every other
// failure keeps the sign-out owed.
UR_TEST(SignOutWiring_TheDeliveryStopsOnlyThisUsersWork) {
  const std::string daemon =
      SignOutBody(ReadSignOutSource("SdkHost.cpp"), "signout::Daemon SdkHost::SignOutDaemonLocked() {");
  UR_EXPECT_TRUE(InOrder(daemon, {"control_.EnsureSession(&error) == DaemonSessionState::Ok",
                                  "control_.Status(&error);", "return std::nullopt;",
                                  "return status->redacted;",
                                  "case signout::Request::StopTunnel: {",
                                  "if (control_.StopTunnel(&error)) return true;", "return false;",
                                  "case signout::Request::Logout: {",
                                  "spaceJson = networkSpace_->toJson();", "return false;",
                                  "switch (control_.Logout(spaceJson, &error, &code)) {",
                                  "case ControlClient::LogoutOutcome::Done:", "return true;",
                                  "case ControlClient::LogoutOutcome::Unsupported:", "return true;",
                                  "case ControlClient::LogoutOutcome::Failed:",
                                  "if (code == ctl::kCodeAuthNotTunnelOwner) {", "return true;",
                                  "break;", "return false;"}));
  // the client tells an older daemon's refusal from every other failure
  const std::string client = ReadSignOutSource("ControlClient.cpp");
  const std::string logout = SignOutBody(client, "ControlClient::LogoutOutcome ControlClient::Logout(");
  UR_EXPECT_TRUE(InOrder(logout, {"req.network_space_json = networkSpaceJson;",
                                  "CallLocked(ctl::Verb::Logout, nlohmann::json(req), error,",
                                  "if (!reply) return LogoutOutcome::Failed;",
                                  "replyError == ctl::kErrorUnknownVerb ? LogoutOutcome::Unsupported",
                                  ": LogoutOutcome::Failed;", "return LogoutOutcome::Done;"}));
}

// Logout also ends what the leaving account had in flight in the app: a sign-in
// method being added is cancelled before anything else (its late return would
// add it to the next account), and the api's credential is cleared with the
// local logout.
UR_TEST(SignOutWiring_LogoutCancelsTheAccountsAddAndItsApiCredential) {
  const std::string host = ReadSignOutSource("SdkHost.cpp");
  const std::string logout = SignOutBody(host, "void SdkHost::LogoutNow() {");
  UR_EXPECT_TRUE(InOrder(logout, {"CancelPendingAddSignIn(\"superseded by signing out\");",
                                  "std::scoped_lock lock(mutex_);", "pendingWalletAuth_.reset();",
                                  "pendingSsoAuth_ = false;", "pendingSsoJwt_.clear();",
                                  "pendingInstantJwt_.reset();", "asyncLocalState_->logout(",
                                  "if (api_) api_->setByJwt(\"\");",
                                  "signOut_.Begin(SignOutDaemonLocked());"}));
  // and the post-sign-up onboarding of a network created here goes with a
  // sign-out, never with a sign-in
  const std::string apply =
      SignOutBody(ReadSignOutSource("MainWindow.cpp"), "void MainWindow::ApplyAuthState(bool loggedIn) {");
  const size_t signedOutBranch = apply.find("} else {");
  if (signedOutBranch == std::string::npos) {
    UR_EXPECT_TRUE_MSG("ApplyAuthState has a signed-out branch", false);
    return;
  }
  UR_EXPECT_TRUE(SignOutHas(apply.substr(signedOutBranch), "prefs::Set(kOnboardingPendingKey, false);"));
  UR_EXPECT_TRUE(!SignOutHas(apply.substr(0, signedOutBranch), "kOnboardingPendingKey, false"));
  // so does the leaving account's view of its activity on Home: its Undo and
  // row menu, its filters and its selection, in one pass
  UR_EXPECT_TRUE(InOrder(apply.substr(signedOutBranch),
                         {"accountPage_->ResetForSignOut();", "connectPage_->ResetForSignOut();"}));
  UR_EXPECT_TRUE(!SignOutHas(apply.substr(0, signedOutBranch), "ResetForSignOut();"));
  const std::string reset = SignOutBody(ReadSignOutSource("ConnectPage.cpp"),
                                        "void ConnectPage::ResetForSignOut() {");
  UR_EXPECT_TRUE(InOrder(reset, {"quickToast_.Dismiss();", "if (rowMenu_) rowMenu_->popdown();",
                                 "selectedConnectionId_.clear();",
                                 "OnConnectionsClearFilters();"}));
}

// The daemon's logout: refused beside another user's live session before
// anything is read or cleared, whatever was authorized (never the take-over
// action); otherwise owner-checked as stop_tunnel, done by TunnelHost::Logout,
// and nobody owns the session after it.
UR_TEST(SignOutWiring_TheDaemonsLogoutLeavesAnotherUsersSession) {
  const std::string server = ReadSignOutSource("daemon/ControlServer.cpp");
  const std::string dispatch = SignOutBody(server, "void ControlServer::DispatchAuthorized(");
  const size_t at = dispatch.find("case ctl::Verb::Logout: {");
  if (at == std::string::npos) {
    UR_EXPECT_TRUE_MSG("DispatchAuthorized has a logout case", false);
    return;
  }
  const std::string logout = dispatch.substr(at, dispatch.find("case ctl::Verb::SetProvide: {", at) - at);
  UR_EXPECT_TRUE(InOrder(logout, {"if (TunnelOwnedByOtherUid(conn)) {", "ctl::kCodeAuthNotTunnelOwner",
                                  "return;",
                                  "CheckTunnelOwner(conn, id, &denied, &crossUid, /*authorizedTakeOver=*/false)",
                                  "request.get<ctl::LogoutRequest>()",
                                  "tunnel_.Logout(req.network_space_json, &error, &code)", "return;",
                                  "tunnelOwnerUid_ = -1;", "tunnel_.SetOwnerUid(-1);",
                                  "ctl::MakeReply(id, true, nlohmann::json(tunnel_.Status()))"}));
  UR_EXPECT_TRUE(!SignOutHas(logout, "authorizedCrossUid"));
}

// TunnelHost::Logout: never waits behind a bring-up; stops what runs as an
// explicit stop does and drops a queued upload (its credentials would make an
// identity); forgets what the account asked for; deletes the three identity
// files; and logs out the account's space.
UR_TEST(SignOutWiring_TheDaemonForgetsTheAccount) {
  const std::string host = ReadSignOutSource("daemon/TunnelHost.cpp");
  const std::string logout = SignOutBody(host, "bool TunnelHost::Logout(");
  UR_EXPECT_TRUE(InOrder(logout, {"std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);",
                                  "if (!lock.owns_lock()) {", "ctl::kCodeStartInProgress",
                                  "return false;", "StopInternalLocked(\"user\");",
                                  "queuedUpload_.reset();", "pendingProvideMode_.clear();",
                                  "killSwitchRequested_.store(false);",
                                  "{kClientKeySeedFile, kProvideCertFile, kProvideKeyFile}",
                                  "::unlink(path.c_str())",
                                  "LoadNetworkSpaceLocked(networkSpaceJson);",
                                  "networkSpace_->getAsyncLocalState().getLocalState().logout();",
                                  "return true;"}));
}

// Every reconcile delivers an owed sign-out first; a signed-out reconcile acts
// as mode never, so a provider the daemon still runs for this user is
// stopped; and no provider starts while a sign-out is owed.
UR_TEST(SignOutWiring_TheReconcileDeliversFirstAndStartsNothingWhileOwed) {
  const std::string reconcile =
      SignOutBody(ReadSignOutSource("SdkHost.cpp"), "void SdkHost::ReconcileProviderLocked(");
  UR_EXPECT_TRUE(InOrder(
      reconcile,
      {"if (!localState_ || providerReconcileClosed_) return;",
       "SettleSignOutLocked(reason, userInitiated);",
       "const bool signedIn = !signedOut_.load() && !clientJwt.empty() && !instanceId.empty();",
       "signedIn ? localState_->getProvideControlMode() : std::string(\"never\");",
       "control_.Status(&statusError);", "if (step == provide::DisconnectedStep::Start) {",
       "if (signOut_.Owed()) {", "return;", "control_.StartProvider(request, &after, &error, &code)"}));
  const std::string settle = SignOutBody(ReadSignOutSource("SdkHost.cpp"),
                                         "void SdkHost::SettleSignOutLocked(");
  UR_EXPECT_TRUE(InOrder(settle, {"if (!signOut_.Owed()) return;",
                                  "if (userInitiated) signOutBackoff_.NoteSuccess();",
                                  "if (!signOutBackoff_.Allows(nowMillis)) return;",
                                  "signOut_.Settle(SignOutDaemonLocked());",
                                  "signOutBackoff_.NoteFailure(nowMillis);"}));
  // the health poll runs the reconcile while disconnected, signed in or not
  const std::string poll = SignOutBody(
      ReadSignOutSource("MainWindow.cpp"),
      "void MainWindow::ApplyDaemonHealth(const std::optional<ctl::StatusReply>& status) {");
  UR_EXPECT_TRUE(
      InOrder(poll, {"if (!connected_) {", "host_.ReconcileProvider(\"health poll\", *status);"}));
  UR_EXPECT_TRUE(!SignOutHas(poll.substr(0, poll.find("host_.ReconcileProvider(")), "IsLoggedIn("));
}

// A connect delivers it first, is refused for a signed-out app whatever the
// stored jwt says, and attaches or starts nothing while a sign-out is owed.
UR_TEST(SignOutWiring_AConnectDeliversFirstAndStartsNothingWhileOwed) {
  const std::string start =
      SignOutBody(ReadSignOutSource("SdkHost.cpp"),
                  "TunnelStartResult SdkHost::StartTunnelLocked(const char* reason) {");
  UR_EXPECT_TRUE(InOrder(
      start, {"SettleSignOutLocked(\"connect\", /*userInitiated=*/true);",
              "signedOut_.load() ? std::string() : localState_->getByClientJwt();",
              "switch (control_.EnsureSession(&error)) {", "if (signOut_.Owed()) {",
              "return TunnelStartResult::Failed;", "control_.Status(&statusError);",
              "TryAttachRememberedSessionLocked("}));
}

// The in-memory sign-out ends only with a sign-in's stored credential or a
// space chosen with one, and the launch reads the marker.
UR_TEST(SignOutWiring_TheSignOutEndsWithASignInAndOutlivesTheApp) {
  const std::string host = ReadSignOutSource("SdkHost.cpp");
  const std::string registered = SignOutBody(host, "void SdkHost::RegisterNetworkClient(");
  UR_EXPECT_TRUE(InOrder(registered, {"asyncLocalState_->setByClientJwt(",
                                      "signedOut_.store(false);", "onAuth_(true);"}));
  const std::string server = SignOutBody(host, "bool SdkHost::ApplyNetworkServer(");
  UR_EXPECT_TRUE(InOrder(server, {"loggedIn = !localState_->getByClientJwt().empty();",
                                  "signedOut_.store(!loggedIn);"}));
  UR_EXPECT_TRUE(SignOutHas(SignOutBody(host, "bool SdkHost::IsLoggedIn() {"),
                            "return !signedOut_.load() && localState_ &&"));
  const std::string initialize = SignOutBody(host, "bool SdkHost::Initialize(");
  UR_EXPECT_TRUE(InOrder(initialize, {"std::scoped_lock lock(mutex_);", "signOut_.Load();"}));
  const std::string marker = SignOutBody(host, "signout::Marker SdkHost::SignOutMarker() {");
  UR_EXPECT_TRUE(InOrder(marker, {"g_file_test(SignOutOwedPath().c_str(), G_FILE_TEST_EXISTS)",
                                  "g_remove(path.c_str())", "std::ofstream file(path, std::ios::trunc);"}));
  UR_EXPECT_TRUE(SignOutHas(SignOutBody(host, "std::string SignOutOwedPath() {"),
                            "std::string(g_get_user_config_dir()) + \"/urnetwork\""));
  const std::string header = ReadSignOutSource("SdkHost.hpp");
  UR_EXPECT_TRUE(SignOutHas(header, "signout::Obligation signOut_{SignOutMarker()};"));
  UR_EXPECT_TRUE(SignOutHas(header, "std::atomic<bool> signedOut_{false};"));
}

// Earnings forgets the own-account facts a reload leaves, the points header's
// and both public switches' (through their echo guards), and abandons a
// switch's set still in flight, before the reload that empties its panels, and
// the window's sign-out asks for exactly that.
UR_TEST(SignOutWiring_EarningsForgetsTheDepartedNetwork) {
  const std::string page = ReadSignOutSource("EarningsPage.cpp");
  const std::string reset = SignOutBody(page, "void EarningsPage::ResetForSignOut() {");
  // a switch's set in flight is abandoned: no watchdog reports it to the next
  // account, and the switch is free again
  UR_EXPECT_TRUE(InOrder(reset, {"++pointsPublicFlow_.generation;",
                                 "pointsPublicFlow_.timer.disconnect();",
                                 "settingPointsPublic_ = false;",
                                 "if (pointsPublicToggle_ != nullptr) "
                                 "pointsPublicToggle_->set_sensitive(true);",
                                 "++rankingFlow_.generation;", "rankingFlow_.timer.disconnect();",
                                 "settingRankingPublic_ = false;",
                                 "publicToggle_->set_sensitive(true);", "Load();"}));
  UR_EXPECT_TRUE(InOrder(reset, {"ownNetworkId_.clear();", "pointsPublic_ = false;",
                                 "emojiTag_.clear();", "ownFlagsClock_ = 0;",
                                 "ownFlagsEditedAt_ = 0;", "ownFlagsAppliedAt_ = 0;",
                                 "SetPointsToggle(false);", "rankingPublic_ = false;",
                                 "SetRankingToggle(false);", "Load();"}));
  UR_EXPECT_TRUE(!SignOutHas(reset, "set_active("));
  const std::string window = ReadSignOutSource("MainWindow.cpp");
  const std::string auth = SignOutBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {");
  const size_t signedOut = auth.find("} else {");
  UR_EXPECT_TRUE(signedOut != std::string::npos);
  const std::string branch = signedOut == std::string::npos ? std::string() : auth.substr(signedOut);
  UR_EXPECT_TRUE(SignOutHas(branch, "if (earningsPage_) earningsPage_->ResetForSignOut();"));
  UR_EXPECT_TRUE(!SignOutHas(branch, "earningsPage_->Load()"));
}
