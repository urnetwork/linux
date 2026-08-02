// Subscription balance store — an almost-literal port of the apple
// SubscriptionBalanceViewModel (apple/app/network/Shared/ViewModels/
// SubscriptionBalanceViewModel.swift):
//
//   * Api::subscriptionBalance -> used / pending / available / start bytes and
//     current_subscription -> isPro. There is no SDK view controller for the
//     subscription balance (AccountViewController only wraps
//     walletValidateAddress), so this store calls the Api directly, exactly
//     like the mac view model it ports.
//   * a 30s background poll while the network is not Pro (it drives the usage
//     bar), gated on window visibility so a hidden tray window doesn't churn;
//   * a 5s confirmation poll with a 2-MINUTE DEADLINE for right after a
//     checkout or a redeemed balance code: the purchase reaches the server
//     asynchronously (Stripe webhook), so we poll to bridge the gap — and give
//     up loudly instead of spinning forever when a webhook is lost. The timer
//     pauses while hidden but preserves its deadline, then resumes or times out
//     immediately when the window is shown.
//   * offline Pro: the jwt's Pro claim (LocalState::parseByJwt) seeds the
//     state before the first fetch; the server is the source of truth, and
//     the jwt is refreshed (Device::refreshToken) whenever the two disagree
//     in either direction — an upgrade AND a lapse both go stale in the jwt.
//   * the referral code + total referrals for the usage bar's referral row
//     ride along on the same poll (mac polls them in a separate 60s view
//     model; one cycle here keeps the plumbing in one place).
//
// All state lives on the GTK main loop: SDK callbacks marshal through
// PostToMain, the timers are Glib main-loop timeouts, and the UI reads the
// accessors directly. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <glibmm/main.h>

#include "SdkHost.hpp"

namespace urnw {

class SubscriptionBalanceStore {
 public:
  // Fired on the GTK main loop after any state change.
  using ChangedHandler = std::function<void()>;

  explicit SubscriptionBalanceStore(SdkHost& host);
  ~SubscriptionBalanceStore();

  // Login: seed Pro/Guest from the stored jwt (offline read), fetch once, and
  // start the 30s background poll when not Pro (mac parity: a Pro network with
  // balance does not poll).
  void Start();
  // Logout: stop the timers, invalidate in-flight fetches, clear the state.
  void Stop();

  // The tray-app visibility gate (MainWindow::windowVisible_): the background
  // timers are stopped while the window is hidden and resync on show.
  void SetWindowVisible(bool visible);

  void FetchNow();

  // Re-derive Pro from the (freshly refreshed) jwt. Wired to the sdk's jwt-refresh
  // listener so a mid-session Pro change — notably a Pro->free lapse, which a Pro
  // network's paused poll would otherwise miss until the window is re-shown — is
  // reflected right away. Must be called on the GTK main thread.
  void OnJwtRefreshed();

  // The upgrade/redeem confirmation poll: 5s interval, 2-minute deadline.
  void StartConfirmationPolling();
  void ClearPurchaseConfirmationTimeout();

  bool IsPro() const { return isPro_; }
  bool IsGuest() const { return isGuest_; }
  // Set once when a free -> paid upgrade is first detected (mac
  // didDetectUpgradeToPro), so the app can reset provide mode to never at the
  // upgrade (the user can opt back in after). Stays up for the session; the
  // observer applies the side effect once.
  bool DidDetectUpgradeToPro() const { return didDetectUpgradeToPro_; }
  bool IsPolling() const { return isPolling_; }
  bool PurchaseConfirmationTimedOut() const { return purchaseConfirmationTimedOut_; }
  bool HasFetched() const { return hasFetched_; }
  int64_t UsedByteCount() const { return usedByteCount_; }
  int64_t PendingByteCount() const { return pendingByteCount_; }
  int64_t AvailableByteCount() const { return availableByteCount_; }
  int64_t StartBalanceByteCount() const { return startBalanceByteCount_; }
  int64_t TotalReferrals() const { return totalReferrals_; }
  const std::string& ReferralCode() const { return referralCode_; }

  void SetChangedHandler(ChangedHandler h) { onChanged_ = std::move(h); }

 private:
  void FetchSubscriptionBalance();
  void FetchReferralCode();
  void UpdateIsPro(bool isPro);  // mac updateIsPro: flips the polling mode
  void StartBackgroundPolling();
  void ResumeConfirmationPolling();
  void StopPolling();  // both timers + deadline (mac stopPolling)
  bool IsSupporterWithBalance() const { return isPro_ && availableByteCount_ > 0; }
  void Emit();

  SdkHost& host_;
  ChangedHandler onChanged_;

  // Invalidates in-flight fetch callbacks across Stop()/Start() (logout must
  // not let a stale result repopulate the next session's state).
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  bool started_ = false;
  bool windowVisible_ = false;
  bool isLoading_ = false;
  bool errorFetching_ = false;
  bool hasFetched_ = false;

  bool isPro_ = false;
  bool isGuest_ = false;
  bool didDetectUpgradeToPro_ = false;

  int64_t usedByteCount_ = 0;
  int64_t pendingByteCount_ = 0;
  int64_t availableByteCount_ = 0;
  int64_t startBalanceByteCount_ = 0;

  bool isLoadingReferral_ = false;
  int64_t totalReferrals_ = 0;
  std::string referralCode_;

  // polling (mac: backgroundPollingTimer / pollingTimer / pollingDeadline)
  sigc::connection backgroundTimer_;
  sigc::connection pollingTimer_;
  bool isPolling_ = false;
  bool hasPollingDeadline_ = false;
  gint64 pollingDeadlineUs_ = 0;  // g_get_monotonic_time deadline
  bool purchaseConfirmationTimedOut_ = false;
};

}  // namespace urnw
