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

// Display defaults for the referral program's numbers (pro.yml referral:
// bonus_per_referral / referred_bonus over 24h, max_referrals). The live
// values come from the server with the referral code, through
// GetNetworkReferralCodeResult; read CurrentReferralTerms(), not these.
inline constexpr int64_t kReferralGiBPerDay = 3;
inline constexpr int64_t kReferralMaxReferrals = 20;

struct ReferralTerms {
  int64_t maxReferrals = kReferralMaxReferrals;  // 0 = no cap
  int64_t bonusGibPerDay = kReferralGiBPerDay;
  int64_t referredBonusGibPerDay = kReferralGiBPerDay;
  int64_t PaidReferrals(int64_t total) const {
    if (total <= 0) return 0;
    return (0 < maxReferrals && maxReferrals < total) ? maxReferrals : total;
  }
};

// The terms the balance store last received from the server (defaults until then).
const ReferralTerms& CurrentReferralTerms();
void SetCurrentReferralTerms(const ReferralTerms& terms);

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
