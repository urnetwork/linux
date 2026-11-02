// The login's browser round trips (BrowserSignInGate.hpp): coming back to the
// app after one gives the sign-in affordances back once, and never for the
// moment's focus loss of closing the app's own chooser dialog; the attempt itself
// is never cancelled. The window's call sites need GTK and the SDK, so the wiring cases
// read MainWindow.cpp with the comments blanked.
// SPDX-License-Identifier: MPL-2.0
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "BrowserSignInGate.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::signin::AppFocusAway;
using urnw::signin::BrowserFlowGate;
using urnw::signin::kMinAwayMillis;

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadBrowserSignInSource(const std::string& relative) {
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

std::string BrowserSignInBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool BrowserSignInHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool BrowserSignInInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(BrowserSignInGate_AReturnGivesTheAffordancesBackOnce) {
  BrowserFlowGate gate;
  UR_EXPECT_FALSE(gate.InFlight());
  gate.Begin();
  UR_EXPECT_TRUE(gate.InFlight());
  UR_EXPECT_TRUE(gate.TakeOnReturn(kMinAwayMillis));
  UR_EXPECT_FALSE(gate.InFlight());
  // a second return has nothing to give back
  UR_EXPECT_FALSE(gate.TakeOnReturn(10 * kMinAwayMillis));
  // and a new attempt arms it again
  gate.Begin();
  UR_EXPECT_TRUE(gate.TakeOnReturn(10 * kMinAwayMillis));
}

UR_TEST(BrowserSignInGate_AnAnsweredAttemptIsNoReturn) {
  BrowserFlowGate gate;
  UR_EXPECT_FALSE(gate.TakeOnReturn(10 * kMinAwayMillis));
  gate.Begin();
  gate.Settle();
  UR_EXPECT_FALSE(gate.InFlight());
  UR_EXPECT_FALSE(gate.TakeOnReturn(10 * kMinAwayMillis));
}

// Closing a chooser dialog hands the focus back to the window: a short
// absence leaves the gate armed for the real return.
UR_TEST(BrowserSignInGate_AShortAbsenceIsNoReturn) {
  BrowserFlowGate gate;
  gate.Begin();
  UR_EXPECT_FALSE(gate.TakeOnReturn(0));
  UR_EXPECT_FALSE(gate.TakeOnReturn(kMinAwayMillis - 1));
  UR_EXPECT_TRUE(gate.InFlight());
  UR_EXPECT_TRUE(gate.TakeOnReturn(kMinAwayMillis));
}

UR_TEST(BrowserSignInGate_TheAbsenceIsMeasuredFromTheFirstUnfocusedReading) {
  AppFocusAway away;
  // the first reading is no return, focused or not
  UR_EXPECT_FALSE(away.Read(true, 100).has_value());
  UR_EXPECT_FALSE(away.Read(true, 200).has_value());
  UR_EXPECT_FALSE(away.Read(false, 1000).has_value());
  // a second unfocused reading does not move the start
  UR_EXPECT_FALSE(away.Read(false, 1500).has_value());
  const std::optional<int64_t> back = away.Read(true, 4000);
  UR_EXPECT_TRUE(back.has_value());
  UR_EXPECT_EQ(3000, back.value_or(-1));
  // focused again: no new return until the app leaves
  UR_EXPECT_FALSE(away.Read(true, 5000).has_value());
  UR_EXPECT_FALSE(away.Read(false, 6000).has_value());
  UR_EXPECT_EQ(40, away.Read(true, 6040).value_or(-1));
}

// The browser sign-ins arm the gate once they have disabled the affordances,
// the shared answer settles it, and only Google, Apple, Solana and Bittensor
// arm it: the email discovery and the auth code are api calls that always
// answer.
UR_TEST(BrowserSignInWiring_TheBrowserSignInsArmTheGateAndTheAnswerSettlesIt) {
  const std::string window = ReadBrowserSignInSource("MainWindow.cpp");
  for (const char* signature : {"void MainWindow::OnSolana(WalletConnect::Provider provider) {",
                                "void MainWindow::OnSso(const std::string& provider) {",
                                "void MainWindow::OnBittensorWallet(const std::string& walletId) {"}) {
    const std::string body = BrowserSignInBody(window, signature);
    UR_EXPECT_TRUE_MSG(signature, BrowserSignInInOrder(
                                      body, {"SetLoginBusy(true);", "browserSignIn_.Begin();",
                                             "host_.SignIn", "OnWalletAuth(r)"}));
  }
  const std::string answer =
      BrowserSignInBody(window, "void MainWindow::OnWalletAuth(const AuthResult& result) {");
  UR_EXPECT_TRUE(BrowserSignInInOrder(
      answer, {"PostToMain(", "browserSignIn_.Settle();", "SetLoginBusy(false);"}));
  size_t begins = 0;
  for (size_t at = window.find("browserSignIn_.Begin()"); at != std::string::npos;
       at = window.find("browserSignIn_.Begin()", at + 1)) {
    ++begins;
  }
  UR_EXPECT_EQ(3, begins);
}

// The app-wide focus reading feeds the return, which gives the affordances
// back behind the gate, clears only the progress notice, and cancels nothing: a
// late deep link must still land.
UR_TEST(BrowserSignInWiring_TheReturnEnablesTheAffordancesAndCancelsNothing) {
  const std::string window = ReadBrowserSignInSource("MainWindow.cpp");
  const std::string sync = BrowserSignInBody(window, "void MainWindow::ScheduleAppFocusSync() {");
  UR_EXPECT_TRUE(BrowserSignInInOrder(
      sync, {"gtk_window_is_active", "appFocusAway_.Read(focused, g_get_monotonic_time() / 1000)",
             "OnAppReturned(*away);"}));
  const std::string returned =
      BrowserSignInBody(window, "void MainWindow::OnAppReturned(int64_t awayMillis) {");
  UR_EXPECT_TRUE(BrowserSignInInOrder(
      returned, {"browserSignIn_.TakeOnReturn(awayMillis)", "return;", "SetLoginBusy(false);",
                 "has_css_class(\"dim-label\")"}));
  UR_EXPECT_FALSE(BrowserSignInHas(returned, "Cancel"));
  UR_EXPECT_FALSE(BrowserSignInHas(returned, "host_."));
}
