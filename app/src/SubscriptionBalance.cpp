// SPDX-License-Identifier: MPL-2.0
#include "SubscriptionBalance.hpp"

#include <algorithm>
#include <cstdio>

#include <glib.h>

#include "Ui.hpp"  // PostToMain

namespace urnw {
namespace {
constexpr unsigned kBackgroundPollingSeconds = 30;   // mac backgroundPollingInterval
constexpr unsigned kConfirmationPollingSeconds = 5;  // mac pollingInterval
// mac maxPollingDuration — but spent as a budget of ACTIVE polling time, not
// wall clock: it pauses with the timers (see SetWindowVisible)
constexpr gint64 kMaxPollingDurationUs = 120ll * G_USEC_PER_SEC;
}  // namespace

SubscriptionBalanceStore::SubscriptionBalanceStore(SdkHost& host) : host_(host) {}

SubscriptionBalanceStore::~SubscriptionBalanceStore() {
  backgroundTimer_.disconnect();
  pollingTimer_.disconnect();
}

void SubscriptionBalanceStore::Emit() {
  if (onChanged_) onChanged_();
}

void SubscriptionBalanceStore::Start() {
  ++*epoch_;
  started_ = true;
  hasFetched_ = false;
  errorFetching_ = false;
  purchaseConfirmationTimedOut_ = false;
  didDetectUpgradeToPro_ = false;
  usedByteCount_ = pendingByteCount_ = availableByteCount_ = startBalanceByteCount_ = 0;
  totalReferrals_ = 0;
  referralCode_.clear();

  // Offline Pro: the jwt's Pro (and GuestMode) claims are readable without a
  // network call — the plan label is right even before the first fetch.
  if (auto byJwt = host_.ParseByJwt()) {
    isPro_ = byJwt->Pro;
    isGuest_ = byJwt->GuestMode;
  } else {
    isPro_ = false;
    isGuest_ = false;
  }

  if (windowVisible_) {
    if (!isPro_) {
      StartBackgroundPolling();  // fetches immediately, then every 30s
    } else {
      FetchNow();  // Pro networks don't poll (mac parity), but the bar needs data once
    }
  }
  Emit();
}

void SubscriptionBalanceStore::Stop() {
  ++*epoch_;  // drop in-flight results
  started_ = false;
  StopPolling();
  isLoading_ = false;
  isLoadingReferral_ = false;
  hasFetched_ = false;
  isPro_ = false;
  isGuest_ = false;
  didDetectUpgradeToPro_ = false;
  usedByteCount_ = pendingByteCount_ = availableByteCount_ = startBalanceByteCount_ = 0;
  totalReferrals_ = 0;
  referralCode_.clear();
  purchaseConfirmationTimedOut_ = false;
  Emit();
}

void SubscriptionBalanceStore::SetWindowVisible(bool visible) {
  const bool wasVisible = windowVisible_;
  windowVisible_ = visible;
  if (!visible) {
    // Do not keep periodic main-loop wakeups merely to discover that the
    // window is still hidden. The confirmation deadline is a budget of ACTIVE
    // polling time: bank whatever is left so time spent in the browser's
    // checkout — losing focus is exactly what paying looks like — never
    // counts against the 2 minutes. It resumes ticking with the timers.
    if (isPolling_ && hasPollingDeadline_) {
      pollingBudgetUs_ = std::max<gint64>(0, pollingDeadlineUs_ - g_get_monotonic_time());
      hasPollingDeadline_ = false;
    }
    backgroundTimer_.disconnect();
    pollingTimer_.disconnect();
    return;
  }
  if (!started_ || wasVisible) return;
  if (isPolling_) {
    ResumeConfirmationPolling();  // immediate poll; the banked budget re-arms
    return;
  }
  if (!isPro_) {
    StartBackgroundPolling();  // fetches immediately, then every 30s
  } else {
    // A Pro network — including supporter-with-balance, whose periodic polls
    // the stop rule below silences — still refreshes ONCE per window
    // show/focus, so an upgrade or a lapse is picked up when the user comes
    // back to a tray-resident app.
    FetchNow();
  }
}

void SubscriptionBalanceStore::FetchNow() {
  if (!started_) return;
  FetchSubscriptionBalance();
  FetchReferralCode();
}

void SubscriptionBalanceStore::OnJwtRefreshed() {
  if (!started_) return;
  auto byJwt = host_.ParseByJwt();
  if (!byJwt) return;
  const bool before = isPro_;
  UpdateIsPro(byJwt->Pro);  // flips the polling mode if Pro changed
  if (isPro_ != before) Emit();
}

// mac updateIsPro: Pro stops all polling; a lapse back to free restarts the
// background poll.
void SubscriptionBalanceStore::UpdateIsPro(bool isPro) {
  if (isPro == isPro_) return;
  isPro_ = isPro;
  if (isPro) {
    StopPolling();
  } else {
    StopPolling();
    StartBackgroundPolling();
  }
}

void SubscriptionBalanceStore::FetchSubscriptionBalance() {
  if (isLoading_) return;
  isLoading_ = true;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().subscriptionBalance(
      [this, epoch, issued](std::optional<urnet::SubscriptionBalanceResult> result,
                            std::optional<std::string> err) {
        PostToMain([this, epoch, issued, result = std::move(result), err = std::move(err)] {
          if (*epoch != issued) return;  // logged out (or re-logged-in) since
          isLoading_ = false;
          if (err || !result) {
            if (err) std::fprintf(stderr, "[balance] fetch failed: %s\n", err->c_str());
            errorFetching_ = true;
          } else {
            errorFetching_ = false;
            hasFetched_ = true;
            availableByteCount_ = result->balance_byte_count;
            pendingByteCount_ = result->open_transfer_byte_count;
            usedByteCount_ =
                result->start_balance_byte_count - availableByteCount_ - pendingByteCount_;
            startBalanceByteCount_ = result->start_balance_byte_count;

            // The server is the source of truth for Pro: `current_subscription`
            // is non-nil exactly when the network is Pro. The jwt's Pro claim
            // is baked in at issue time, so it goes stale on BOTH an upgrade
            // and a lapse — refresh the jwt whenever the two disagree, in
            // either direction (the mac view model learned this the hard way).
            const bool serverIsPro = result->current_subscription.has_value();
            if (serverIsPro) {
              // A Pro confirmation resolves an earlier confirmation give-up,
              // even when it lands late (background poll, next window focus):
              // the upgrade sheet recovers TimedOut -> Success off this flip.
              purchaseConfirmationTimedOut_ = false;
            }
            if (serverIsPro && !isPro_) {
              // free -> paid: signal the upgrade so provide mode resets to
              // never once (mac didDetectUpgradeToPro; MainWindow applies it)
              didDetectUpgradeToPro_ = true;
            }
            if (auto byJwt = host_.ParseByJwt(); byJwt && byJwt->Pro != serverIsPro) {
              host_.RefreshJwt();
            }
            UpdateIsPro(serverIsPro);
          }

          // confirmation poll bookkeeping. mac runs these checks after EVERY
          // awaited fetch — errors included, or an unreachable server would
          // keep the confirmation poll spinning past its deadline forever.
          if (isPolling_) {
            if (IsSupporterWithBalance()) {
              StopPolling();
            } else if (hasPollingDeadline_ && g_get_monotonic_time() >= pollingDeadlineUs_) {
              // the server never confirmed within the (active-time) window —
              // stop hammering the api and tell the user, rather than
              // spinning for the session
              GiveUpConfirmationPolling();
            }
          } else if (IsSupporterWithBalance()) {
            StopPolling();  // background poll stops once supporter-with-balance
          }
          Emit();
        });
      });
}

// The referral row of the usage bar (mac ReferralLinkViewModel, folded into
// this store's poll). Api only: ReferralCodeViewController streams just the
// code string and needs an open device — total_referrals comes from this call.
void SubscriptionBalanceStore::FetchReferralCode() {
  if (isLoadingReferral_) return;
  isLoadingReferral_ = true;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().getNetworkReferralCode(
      [this, epoch, issued](std::optional<urnet::GetNetworkReferralCodeResult> result,
                            std::optional<std::string> err) {
        PostToMain([this, epoch, issued, result = std::move(result), err = std::move(err)] {
          if (*epoch != issued) return;
          isLoadingReferral_ = false;
          if (err || !result || result->error) return;  // the row just keeps its last value
          totalReferrals_ = result->total_referrals;
          referralCode_ = result->referral_code.value_or(std::string());
          Emit();
        });
      });
}

void SubscriptionBalanceStore::StartBackgroundPolling() {
  backgroundTimer_.disconnect();
  if (!windowVisible_) return;
  FetchNow();
  backgroundTimer_ = Glib::signal_timeout().connect_seconds(
      [this]() -> bool {
        FetchNow();
        return true;
      },
      kBackgroundPollingSeconds);
}

void SubscriptionBalanceStore::StartConfirmationPolling() {
  if (isPolling_) return;
  backgroundTimer_.disconnect();

  // A fresh confirmation attempt: clear any previous give-up and grant the
  // full budget. The budget is accumulated ACTIVE polling time — the deadline
  // is armed from it in ResumeConfirmationPolling and banked back on pause —
  // so hidden/unfocused stretches (the user paying in the browser) never
  // count toward the 2 minutes.
  purchaseConfirmationTimedOut_ = false;
  hasPollingDeadline_ = false;
  pollingBudgetUs_ = kMaxPollingDurationUs;
  isPolling_ = true;

  Emit();
  ResumeConfirmationPolling();
}

void SubscriptionBalanceStore::ResumeConfirmationPolling() {
  if (!started_ || !windowVisible_ || !isPolling_) return;
  if (!hasPollingDeadline_) {
    if (pollingBudgetUs_ <= 0) {
      // resumed with nothing left (the deadline hit exactly at pause time)
      GiveUpConfirmationPolling();
      Emit();
      return;
    }
    // the budget resumes ticking only now that the timer actually runs
    pollingDeadlineUs_ = g_get_monotonic_time() + pollingBudgetUs_;
    hasPollingDeadline_ = true;
  }

  // immediate poll on resume: a webhook that landed while hidden confirms now
  FetchNow();
  pollingTimer_.disconnect();
  pollingTimer_ = Glib::signal_timeout().connect_seconds(
      [this]() -> bool {
        // Check independently of the API callback. A callback that is delayed
        // or never arrives must not keep the confirmation timer alive forever.
        if (hasPollingDeadline_ && g_get_monotonic_time() >= pollingDeadlineUs_) {
          GiveUpConfirmationPolling();
          Emit();
          return false;
        }
        FetchNow();
        return true;
      },
      kConfirmationPollingSeconds);
}

// The confirmation window is spent: stop hammering the api, raise the
// timed-out flag for the sheet, and fall back to the background cadence so a
// late webhook is still picked up (and can clear the flag again).
void SubscriptionBalanceStore::GiveUpConfirmationPolling() {
  StopPolling();
  purchaseConfirmationTimedOut_ = true;
  if (!isPro_ && !IsSupporterWithBalance()) StartBackgroundPolling();
}

void SubscriptionBalanceStore::StopPolling() {
  backgroundTimer_.disconnect();
  pollingTimer_.disconnect();
  hasPollingDeadline_ = false;
  pollingBudgetUs_ = 0;
  if (isPolling_) {
    isPolling_ = false;
    Emit();
  }
}

void SubscriptionBalanceStore::ClearPurchaseConfirmationTimeout() {
  purchaseConfirmationTimedOut_ = false;
  Emit();
}

}  // namespace urnw
