// The referral king-frog gold moments (the ur.io referral panel, ported like
// android/apple): the one-time full-screen crowning when the network's first
// referral lands, the gold refer-friends sheet, and the shared royal-welcome
// panel the referral sheets show when a code is accepted.
//
// Gold here is the REFERRAL gold ramp (Ui.hpp kReferralGold*), deliberately a
// warmer gold than kProGold so the Pro plan value keeps its meaning.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <gtkmm/box.h>
#include <gtkmm/window.h>

namespace urnw {

class SdkHost;

// The daily GiB each side of a verified referral earns, for life (server
// config pro.yml referral: bonus_per_referral / referred_bonus over 24h).
inline constexpr int64_t kReferralGiBPerDay = 3;
// The max referrals a network is paid for (pro.yml referral.max_referrals).
inline constexpr int64_t kReferralMaxReferrals = 20;

// The royal-welcome panel: the crowned frog plus confirmation copy. Shown by
// the referral sheets the moment a code is accepted. Caller owns placement.
Gtk::Box* MakeRoyalWelcomePanel();

// The royal welcome as a one-shot sheet: shown for a beat (auto-closes) the
// moment a referral code is accepted, then gets out of the way.
void ShowRoyalWelcomeSheet(Gtk::Window& parent);

// One-time crowning celebration for the referrer: a modal gold sheet shown
// the first time a friend joins with their code (later referrals get the gold
// snackbar instead). `joined` is the batch size, `referralCode` the code to
// show/copy; copying the invite uses referral_share_message.
void ShowReferralCelebrationSheet(Gtk::Window& parent, int64_t joined,
                                  const std::string& referralCode);

// The refer-friends panel in the gold king-frog theme (the ur.io referral
// panel): heading (crowned once totalReferrals > 0), code, copy actions.
void ShowReferSheet(Gtk::Window& parent, int64_t totalReferrals,
                    const std::string& referralCode);

}  // namespace urnw
