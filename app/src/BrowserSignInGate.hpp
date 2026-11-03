// The login's browser round trips and the user coming back from one. Google,
// Apple, Solana and the Bittensor wallets sign in through the browser (or a
// wallet app) and answer only through the urnetwork:// deep link. While one is
// in flight every sign-in affordance is disabled (MainWindow::SetLoginBusy),
// and a browser the user closed sends nothing, so without this the buttons
// stayed disabled until the app quit.
//
// Coming back to the app is the flow being over from the user's side: the
// window enables the affordances again, but leaves the SDK attempt armed. A
// late completion still lands (SdkHost matches its state and nonce), and a new
// click supersedes it through SdkHost's wallet flow counter. There is no
// reliable "browser closed" signal, so cancelling here could only kill a flow
// still open in another tab.
//
// One thing is not a return: closing one of the app's own dialogs (the Solana
// and Bittensor choosers start these flows) can drop the app's focus for a
// moment before the main window takes it back, so only an absence of
// kMinAwayMillis counts.
//
// Header-only and free of GTK (tests/BrowserSignInGateTest.cpp). Not thread
// safe: the window uses it on the GTK main loop only.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace urnw::signin {

// Longer than any focus hand-over between the app's own windows, shorter than
// any trip to a browser and back.
inline constexpr int64_t kMinAwayMillis = 1000;

// The app's focus, read app-wide (any of its windows active): how long it was
// away when a reading brings it back.
class AppFocusAway {
 public:
  // The absence a focused reading ends, in milliseconds; nothing for any
  // other reading, and for the first focused one.
  std::optional<int64_t> Read(bool focused, int64_t nowMillis) {
    if (!focused) {
      if (!leftAtMillis_) leftAtMillis_ = nowMillis;
      return std::nullopt;
    }
    std::optional<int64_t> away;
    if (leftAtMillis_) away = nowMillis - *leftAtMillis_;
    leftAtMillis_.reset();
    return away;
  }

 private:
  std::optional<int64_t> leftAtMillis_;
};

class BrowserFlowGate {
 public:
  // A browser sign-in started: the affordances are disabled.
  void Begin() { inFlight_ = true; }

  // The attempt answered (its callback enables the affordances).
  void Settle() { inFlight_ = false; }

  bool InFlight() const { return inFlight_; }

  // The app came back after `awayMillis`. True, at most once per Begin, when
  // the caller should enable the affordances again; a short absence leaves the
  // gate armed for a later return.
  bool TakeOnReturn(int64_t awayMillis) {
    if (!inFlight_ || awayMillis < kMinAwayMillis) return false;
    inFlight_ = false;
    return true;
  }

 private:
  bool inFlight_ = false;
};

}  // namespace urnw::signin
