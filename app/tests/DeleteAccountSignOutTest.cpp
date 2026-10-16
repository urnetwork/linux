// The account deletion signs out only the sign-in it deleted (android
// signOutApplies). The request reads the sign-in as it goes out
// (SdkHost::SignInNow) and the answer signs out only while that one is still
// signed in (SdkHost::LogoutIfStill). Before, the answer ran Logout() whatever
// had happened meanwhile: a server sign-out (the deletion itself revokes the
// session) and a sign-in to another network could both land before it, and
// the late answer then signed that other network out.
//
// The race is replayed on the sign-in tracker the comparison uses
// (auth_logout::Tracker); the sheet's call sites are read from the sources.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>

#include "AuthLogout.hpp"

namespace {

namespace wiring = urnw::testing::wiring;
using urnw::auth_logout::Report;
using urnw::auth_logout::Tracker;

}  // namespace

UR_TEST(DeleteAccountSignOut_TheAnswerSignsOutTheDeletedSignIn) {
  Tracker tracker;
  tracker.SignedIn();
  // the request goes out
  const Report deleted = tracker.Hear("account deleted");
  // and its answer lands with that sign-in still signed in
  UR_EXPECT_TRUE(tracker.SignsOut(deleted));
}

UR_TEST(DeleteAccountSignOut_ALateAnswerNeverSignsOutTheNextSignIn) {
  Tracker tracker;
  tracker.SignedIn();
  const Report deleted = tracker.Hear("account deleted");
  // the server signs the deleted session out, and another network signs in
  tracker.SignedOut();
  tracker.SignedIn();
  // the late answer is about a sign-in that has ended
  UR_EXPECT_FALSE(tracker.SignsOut(deleted));
  // and one read while signed out signs nothing out either
  tracker.SignedOut();
  UR_EXPECT_FALSE(tracker.SignsOut(tracker.Hear("account deleted")));
}

UR_TEST(DeleteAccountSignOut_TheSheetComparesTheSignIn) {
  const std::string page = wiring::ReadCode("AccountPage.cpp");
  // read as the request goes out, carried to the answer
  UR_EXPECT_TRUE(wiring::Before(page, "const auth_logout::Report signIn = host_.SignInNow(\"account deleted\");",
                                "host_.api().networkDelete("));
  UR_EXPECT_TRUE(wiring::Contains(page, "if (on_deleted) on_deleted(signIn);"));
  // and the page signs out only that sign-in, never whatever is signed in now
  const size_t at = page.find("deleteSheet_->on_deleted = [this](const auth_logout::Report& signIn) {");
  UR_EXPECT_TRUE(at != std::string::npos);
  if (at == std::string::npos) return;
  const std::string handler = page.substr(at, page.find("};", at) - at);
  UR_EXPECT_TRUE(wiring::Contains(handler, "host_.LogoutIfStill(signIn);"));
  UR_EXPECT_FALSE(wiring::Contains(handler, "host_.Logout();"));
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE(wiring::Before(
      wiring::FunctionBody(host, "bool SdkHost::LogoutIfStill(const auth_logout::Report& signIn) {"),
      "if (!authLogouts_.SignsOut(signIn)) {", "Logout();"));
}
