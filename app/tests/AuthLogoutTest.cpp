// The sign-out the sdk reports, as the app follows it (AuthLogout.hpp;
// REVOKE-UI-FINAL.md §5): only the server's session-revoked cause says "This
// session was signed out from another device.", once, on the sign-in page; ""
// and every other cause say nothing new; and one rejection, which the Api's
// and the bound DeviceRemote's listeners each report, signs the app out once,
// however their posts interleave with the sign-out. The window drives these
// the way App below does (ApiSignOutWiringTest pins MainWindow and SdkHost to
// it); nothing here waits, races or reads a clock. The account's sign-in
// needed while this device stays signed in follows the same rule for its
// notice's words.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "AuthLogout.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace a = urnw::auth_logout;

namespace {

// The app's half, in the window's order: a report that still signs out owes
// the notice and signs out (MainWindow::OnAuthLogout, SdkHost::Logout); the
// sign-out reaching the sign-in page shows what is owed (ApplyAuthState).
struct App {
  a::Tracker tracker;
  a::SignInNotice notice;
  int signOuts = 0;
  int noticesShown = 0;

  void SignIn() {
    tracker.SignedIn();
    notice.Drop();
  }
  // a sign-out of any kind; Arm comes before it only on the report path
  void SignOut() {
    tracker.SignedOut();
    ++signOuts;
    if (notice.Take()) ++noticesShown;
  }
  void Receive(const a::Report& report) {
    if (!tracker.SignsOut(report)) return;
    notice.Arm(report.cause);
    SignOut();
  }
};

}  // namespace

UR_TEST(AuthLogout_OnlyTheTrustedCauseSaysSignedOutRemotely) {
  UR_EXPECT_TRUE(std::string(a::kSessionRevoked) == "session_revoked");
  UR_EXPECT_TRUE(a::SignedOutRemotely("session_revoked"));
  for (const char* other : {"", "SESSION_REVOKED", "session_revoked ", " session_revoked",
                            "revoked", "session", "signed_out", "unauthorized"}) {
    UR_EXPECT_TRUE_MSG(std::string("\"") + other + "\"", !a::SignedOutRemotely(other));
  }
}

// The notice is owed by the report that signs out and shown once.
UR_TEST(AuthLogout_ARemoteSignOutIsSaidOnce) {
  a::SignInNotice notice;
  notice.Arm(a::kSessionRevoked);
  UR_EXPECT_TRUE(notice.Take());
  UR_EXPECT_FALSE(notice.Take());  // the same sign-out says nothing a second time
  // "" and any other cause owe nothing
  notice.Arm("");
  UR_EXPECT_FALSE(notice.Take());
  notice.Arm("not_a_cause");
  UR_EXPECT_FALSE(notice.Take());
  // a cause after a revoked one replaces it
  notice.Arm(a::kSessionRevoked);
  notice.Arm("");
  UR_EXPECT_FALSE(notice.Take());
  // a sign-in before the sign-in page showed it drops it
  notice.Arm(a::kSessionRevoked);
  notice.Drop();
  UR_EXPECT_FALSE(notice.Take());
}

// Signed out from another device with the tunnel up: the Api's listener and
// then the DeviceRemote's report the one rejection; one sign-out, one notice,
// whether the device's report was heard before the sign-out or after it.
UR_TEST(AuthLogout_OneRejectionSignsOutOnceWithTheNotice) {
  {
    App app;
    app.SignIn();
    const a::Report api = app.tracker.Hear(a::kSessionRevoked);
    const a::Report device = app.tracker.Hear(a::kSessionRevoked);
    app.Receive(api);
    app.Receive(device);
    UR_EXPECT_EQ(1, app.signOuts);
    UR_EXPECT_EQ(1, app.noticesShown);
  }
  {
    // the device's listener runs after the window has signed out
    App app;
    app.SignIn();
    const a::Report api = app.tracker.Hear(a::kSessionRevoked);
    app.Receive(api);
    const a::Report device = app.tracker.Hear(a::kSessionRevoked);
    app.Receive(device);
    UR_EXPECT_EQ(1, app.signOuts);
    UR_EXPECT_EQ(1, app.noticesShown);
  }
  {
    // the device's post lands first: still one
    App app;
    app.SignIn();
    const a::Report api = app.tracker.Hear(a::kSessionRevoked);
    const a::Report device = app.tracker.Hear(a::kSessionRevoked);
    app.Receive(device);
    app.Receive(api);
    UR_EXPECT_EQ(1, app.signOuts);
    UR_EXPECT_EQ(1, app.noticesShown);
  }
}

// Any other rejection signs out as before and says nothing new.
UR_TEST(AuthLogout_AGenericRejectionSignsOutWithoutTheNotice) {
  App app;
  app.SignIn();
  app.Receive(app.tracker.Hear(""));
  app.Receive(app.tracker.Hear(""));
  UR_EXPECT_EQ(1, app.signOuts);
  UR_EXPECT_EQ(0, app.noticesShown);
}

// Revoking this session from the Sessions list: the sdk reports it without a
// cause, so the app signs out and says nothing about another device.
UR_TEST(AuthLogout_SigningThisSessionOutHereSaysNothing) {
  App app;
  app.SignIn();
  const a::Report api = app.tracker.Hear("");
  const a::Report device = app.tracker.Hear("");
  app.Receive(api);
  app.Receive(device);
  UR_EXPECT_EQ(1, app.signOuts);
  UR_EXPECT_EQ(0, app.noticesShown);
}

// The app's own Sign out reports nothing, owes nothing, and spends a report
// heard just before it, whatever its cause.
UR_TEST(AuthLogout_AnExplicitSignOutSaysNothing) {
  App app;
  app.SignIn();
  app.SignOut();
  UR_EXPECT_EQ(1, app.signOuts);
  UR_EXPECT_EQ(0, app.noticesShown);

  app.SignIn();
  const a::Report inFlight = app.tracker.Hear(a::kSessionRevoked);
  app.SignOut();  // the person signed out first
  app.Receive(inFlight);
  UR_EXPECT_EQ(2, app.signOuts);
  UR_EXPECT_EQ(0, app.noticesShown);
}

// A report signs out only the sign-in it was heard in: nothing heard while
// signed out, and nothing from before a new sign-in, ends the next one.
UR_TEST(AuthLogout_AReportEndsOnlyItsOwnSignIn) {
  App app;
  const a::Report signedOut = app.tracker.Hear(a::kSessionRevoked);
  UR_EXPECT_EQ(0u, signedOut.signIn);
  app.Receive(signedOut);
  UR_EXPECT_EQ(0, app.signOuts);

  app.SignIn();
  const a::Report first = app.tracker.Hear(a::kSessionRevoked);
  app.Receive(first);
  const a::Report late = app.tracker.Hear(a::kSessionRevoked);  // while signed out
  app.SignIn();  // signed in again before the late posts land
  app.Receive(first);
  app.Receive(late);
  UR_EXPECT_EQ(1, app.signOuts);
  UR_EXPECT_EQ(1, app.noticesShown);
  // a server switch onto a stored sign-in is a new one too
  const a::Report beforeSwitch = app.tracker.Hear(a::kSessionRevoked);
  app.SignIn();
  app.Receive(beforeSwitch);
  UR_EXPECT_EQ(1, app.signOuts);

  // the new sign-in's own rejection signs it out, once
  const a::Report second = app.tracker.Hear(a::kSessionRevoked);
  app.Receive(second);
  app.Receive(second);
  UR_EXPECT_EQ(2, app.signOuts);
  UR_EXPECT_EQ(2, app.noticesShown);
}

// The account's sign-in needed while this device's client stays signed in
// (Api.addAccountSignInRequiredListener): after this app signed its own
// session out from Sessions in the same sign-in, its sign-out follows, as the
// confirmation said; otherwise the window only says the account needs a
// sign-in again, and nothing signs out or disconnects. A report of a sign-in
// that has ended, or an own sign-out asked in an earlier sign-in, never signs
// the next one out.
UR_TEST(AuthLogout_AnAccountSignInNeededSignsOutOnlyAfterOwnSignOut) {
  a::Tracker tracker;
  const a::Report none;  // no own sign-out asked
  // heard while signed out: nothing
  const a::Report signedOut = tracker.Hear("");
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(signedOut), signedOut, none) ==
                 a::AccountSignInStep::None);

  tracker.SignedIn();
  // signed out from elsewhere: the notice, never a sign-out
  const a::Report elsewhere = tracker.Hear("");
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(elsewhere), elsewhere, none) ==
                 a::AccountSignInStep::Notice);
  // this app signed its own session out in this sign-in: its sign-out
  const a::Report own = tracker.Hear("own session signed out from Sessions");
  const a::Report afterOwn = tracker.Hear("");
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(afterOwn), afterOwn, own) ==
                 a::AccountSignInStep::SignOut);

  // a new sign-in: the own sign-out of the last one is spent, and a report of
  // the last one does nothing
  tracker.SignedOut();
  tracker.SignedIn();
  const a::Report next = tracker.Hear("");
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(next), next, own) ==
                 a::AccountSignInStep::Notice);
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(afterOwn), afterOwn, own) ==
                 a::AccountSignInStep::None);
  // an own sign-out asked while signed out names no sign-in
  tracker.SignedOut();
  const a::Report ownSignedOut = tracker.Hear("");
  tracker.SignedIn();
  const a::Report later = tracker.Hear("");
  UR_EXPECT_EQ(0u, ownSignedOut.signIn);
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(later), later, ownSignedOut) ==
                 a::AccountSignInStep::Notice);
  // the step does not read the cause: a trusted one is a notice too
  const a::Report revoked = tracker.Hear(a::kSessionRevoked);
  UR_EXPECT_TRUE(a::AccountSignInStepFor(tracker.SignsOut(revoked), revoked, none) ==
                 a::AccountSignInStep::Notice);
}

// The notice's words (REVOKE-UI-FINAL.md §5), in the English sources: the
// generic sign-in line, after "This session was signed out from another
// device." only for the server's trusted cause, the same rule the sign-in
// page's notice follows. "" and every other cause name no other device.
UR_TEST(AuthLogout_AnAccountSignInNoticeNamesAnotherDeviceOnlyForTheTrustedCause) {
  const auto english = [](const a::Text& text) { return std::string(text.english); };
  a::Report report;
  report.signIn = 1;
  report.cause = a::kSessionRevoked;
  UR_EXPECT_TRUE(a::AccountSignInNotice(report, english) ==
                 "This session was signed out from another device. Sign in again to manage sessions.");
  for (const char* other : {"", "SESSION_REVOKED", "session_revoked ", "revoked", "signed_out",
                            "own session signed out from Sessions"}) {
    report.cause = other;
    UR_EXPECT_TRUE_MSG(std::string("\"") + other + "\"",
                       a::AccountSignInNotice(report, english) == "Sign in again to manage sessions.");
  }
  // Each part is looked up once. The lookup order is unspecified by C++.
  std::multiset<std::string> asked;
  const auto keys = [&asked](const a::Text& text) {
    asked.emplace(text.key);
    return std::string("[") + text.key + "]";
  };
  report.cause = a::kSessionRevoked;
  UR_EXPECT_TRUE(a::AccountSignInNotice(report, keys) ==
                 "[sessions_signed_out_remotely] [sessions_sign_in_required]");
  const std::multiset<std::string> expectedKeys = {
      "sessions_signed_out_remotely", "sessions_sign_in_required"};
  UR_EXPECT_TRUE(asked == expectedKeys);
  // both are the catalog's entries, byte for byte
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  UR_EXPECT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  for (const a::Text& text : {a::kSignedOutRemotelyText, a::kSignInRequiredText}) {
    const std::string entry =
        std::string("msgctxt \"") + text.key + "\"\nmsgid \"" + text.english + "\"\n";
    UR_EXPECT_TRUE_MSG(entry, buffer.str().find(entry) != std::string::npos);
  }
}
