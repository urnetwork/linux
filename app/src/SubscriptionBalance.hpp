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
//     up loudly instead of spinning forever when a webhook is lost. The
//     deadline is a budget of ACTIVE polling time, not wall clock
//     (ConfirmationPollGate): the confirmation poll and its budget pause
//     while the window is hidden OR the app has lost focus (a hosted
//     checkout keeps the window visible behind the browser for well over
//     2 minutes) and resume — with an immediate poll — when both are back.
//     A Pro confirmation that lands after a give-up still clears the
//     timed-out state.
//   * Pro is the sdk's decision (urnet::SubscriptionProTracker): the stored
//     jwt's Pro claim before the first fetch, the server's answer from then
//     on. When the two disagree (an upgrade AND a lapse both go stale in the
//     jwt) the jwt is refreshed (Device::refreshToken) once per stored jwt.
//     That refresh renews the client token, not the stored network jwt, so
//     asking on every read and re-deriving Pro from the unchanged claim on
//     the refresh ping-ponged Pro and re-read the balance as fast as the api
//     answered (support inbox 8211).
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
#include <optional>
#include <string>
#include <vector>

#include <glibmm/main.h>

#include "ConfirmationPollGate.hpp"
#include "CryptoPay.hpp"
#include "PricePresentation.hpp"
#include "ReferralCodeState.hpp"
#include "ReferralTotalsState.hpp"
#include "SdkHost.hpp"

namespace urnw {

// A batch of newly observed referrals for the local network. `isFirst` marks
// the crowning: the count went from zero to earned, which gets the full-screen
// celebration; later batches get the gold snackbar.
struct ReferralCelebration {
  int64_t joined = 0;
  bool isFirst = false;
};

class SubscriptionBalanceStore {
 public:
  // Fired on the GTK main loop after any state change.
  using ChangedHandler = std::function<void()>;
  using ReferralCelebrationHandler = std::function<void(const ReferralCelebration&)>;

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
  // Whether any of the app's windows is active (MainWindow tracks the
  // toplevels). Only the confirmation poll follows focus; the background and
  // referral polls keep the visibility gate.
  void SetAppFocused(bool focused);

  void FetchNow();
  // Fetch the balance now and call done(true) when a fresh result landed, or
  // done(false) when the read failed. A read already in flight is shared. Not
  // called after Stop(), and never without a session (done(false) at once).
  void FetchBalanceThen(std::function<void(bool ok)> done);

  // Hand the stored jwt to the Pro decision after a jwt refresh (the sdk's
  // jwt-refresh listener). Its claim counts only before the first fetch: once
  // the server answered, Pro stays the server's until the next fetch, and the
  // refresh's unchanged claim never flips it back. Must be called on the GTK
  // main thread.
  void OnJwtRefreshed();

  // The upgrade/redeem confirmation poll: 5s interval, 2 minutes of active
  // (visible and focused) polling before it gives up.
  void StartConfirmationPolling();
  void ClearPurchaseConfirmationTimeout();

  bool IsPro() const { return isPro_; }
  // The sdk's decision of what the plan value shows
  // (urnet::SubscriptionProTracker getShownPlan): "pro" or "free" as the
  // server answered (Pro also by a Pro claim before it does), else "unknown",
  // while the first read is on its way and after it failed, never Free
  // (account_plan::PlanViewFor; support inbox 4074).
  std::string ShownPlan() const { return proTracker_.getShownPlan(); }
  // the last read failed, and none has loaded since (getLastReadFailed)
  bool PlanReadFailed() const { return proTracker_.getLastReadFailed(); }
  bool IsGuest() const { return isGuest_; }
  // the current subscription's store family (urnet::classifySubscriptionStore:
  // "stripe", "apple", "google", "crypto" for a one-time USDC, Zcash or
  // Monero year, which never renews, or "other"), "" without a subscription
  const std::string& SubscriptionStoreFamily() const { return subscriptionStoreFamily_; }
  // Set once when a free -> paid upgrade is first detected (mac
  // didDetectUpgradeToPro), so the app can reset provide mode to never at the
  // upgrade (the user can opt back in after). Stays up for the session; the
  // observer applies the side effect once.
  bool DidDetectUpgradeToPro() const { return didDetectUpgradeToPro_; }
  bool IsPolling() const { return gate_.Confirming(); }
  bool PurchaseConfirmationTimedOut() const { return purchaseConfirmationTimedOut_; }
  bool HasFetched() const { return hasFetched_; }
  int64_t UsedByteCount() const { return usedByteCount_; }
  int64_t PendingByteCount() const { return pendingByteCount_; }
  int64_t AvailableByteCount() const { return availableByteCount_; }
  int64_t StartBalanceByteCount() const { return startBalanceByteCount_; }
  // When the balance above was fetched, on g_get_monotonic_time in ms (0
  // before the first fetch).
  int64_t FetchedAtMillis() const { return fetchedAtMillis_; }
  int64_t TotalReferrals() const { return totals_.Total(); }
  // what a "Total referrals" figure shows: the count, or an error until a read lands
  ReferralTotalsView TotalsView() const { return totals_.View(); }
  // the referral program's numbers, from the server (defaults until fetched)
  int64_t MaxReferrals() const { return maxReferrals_; }
  int64_t BonusGibPerDay() const { return bonusGibPerDay_; }
  int64_t ReferredBonusGibPerDay() const { return referredBonusGibPerDay_; }
  const std::string& ReferralCode() const { return referral_.Code(); }
  // what the referral panel shows where the code goes
  ReferralCodeView ReferralView() const { return referral_.View(); }
  // Read the code again now (the panel's Try again). Emits.
  void RetryReferral();

  // The plan response's price tier (standard/regional, from the storefront
  // country the server resolved), the network's welcome offer and the
  // experiment assignments (mmm/onboarding/PLAN.md). Defaults until fetched.
  const PriceTierView& Tier() const { return tier_; }
  const OfferView& Offer() const { return offer_; }
  bool OfferActive() const { return offer_.active; }
  // the variant for an experiment surface ("" when unassigned)
  std::string ExperimentVariant(const std::string& surface) const;
  std::string ExperimentId(const std::string& surface) const;
  // A freshly issued offer (POST /onboarding/offer/issue) lands here so every
  // plan surface prints it before the next poll.
  void SetOffer(const urnet::OnboardingOffer& offer);

  // The coin selector a plan surface offers beside Card (mmm/ZCASH-CONTRACT.md
  // §6.1, mmm/BTCPAY.md §17.6): the SDK's urnet::cryptoPaymentTabs over the
  // balance result's two lists, crypto_assets (the coins it sells now) and
  // crypto_unavailable_assets (the coins that are on but cannot be quoted
  // now, shown greyed instead of left out), for the one plan a coin would buy
  // there (crypto_pay::CoinPlan); each coin with the SDK's network line. The
  // SDK decides whether there is a selector: empty, which leaves Card alone,
  // before the first fetch, from a server that lists no coin, and on an SDK
  // error.
  std::vector<crypto_pay::PaymentTabView> PaymentTabs(const std::string& plan) const;
  // The renewal notice of a one-time crypto year is due now
  // (urnet::cryptoTermNoticeDue over the balance result's crypto_term:
  // term_end_time <= now < grace_end_time), and the grace's end it names
  // (RFC 3339, "" without a term).
  bool CryptoTermNoticeDue() const;
  std::string CryptoTermGraceEnd() const;
  // A renewal of the subscription did not go through and its store is still
  // retrying it: the latest balance carried payment_past_due
  // (past_due::NoticeShows). False before the first fetch.
  bool PaymentPastDue() const { return paymentPastDue_; }

  void SetChangedHandler(ChangedHandler h) { onChanged_ = std::move(h); }
  // Fired on the GTK main loop when the referral poll observes new referrals
  // over the persisted per-network baseline (the first observation only
  // records the baseline, so pre-existing referrals never celebrate).
  void SetReferralCelebrationHandler(ReferralCelebrationHandler h) {
    onReferralCelebration_ = std::move(h);
  }

 private:
  void MaybeCelebrateReferrals(int64_t count);
  void EnsureReferralPolling();
  void FetchSubscriptionBalance();
  void FetchReferralCode();
  void UpdateIsPro(bool isPro);  // mac updateIsPro: flips the polling mode
  // the 30s background poll; `fetchNow` reads at once too (not when Pro just
  // changed on a read: that read is the fresh one)
  void StartBackgroundPolling(bool fetchNow = true);
  // hands the stored jwt to proTracker_ (an empty read changes nothing)
  void TrackStoredJwt();
  void ResumeConfirmationPolling();
  // deadline spent: stop, flag timed-out, fall back to the background poll
  void GiveUpConfirmationPolling();
  void StopPolling();  // both timers + the confirmation (mac stopPolling)
  bool IsSupporterWithBalance() const { return isPro_ && availableByteCount_ > 0; }
  void Emit();

  SdkHost& host_;
  ChangedHandler onChanged_;
  ReferralCelebrationHandler onReferralCelebration_;
  // referral celebrations poll on their own timer: unlike the balance poll it
  // never stops for Pro (referrals keep landing either way)
  sigc::connection referralTimer_;

  // Invalidates in-flight fetch callbacks across Stop()/Start() (logout must
  // not let a stale result repopulate the next session's state).
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  bool started_ = false;
  bool windowVisible_ = false;
  bool isLoading_ = false;
  bool errorFetching_ = false;
  bool hasFetched_ = false;

  // the sdk's Pro and jwt refresh decision; isPro_ is its GetIsPro, and
  // ShownPlan its GetShownPlan, which a failed read (BalanceFailed) moves
  // from loading to failed before the server's first answer
  urnet::SubscriptionProTracker proTracker_ = urnet::newSubscriptionProTracker();
  bool isPro_ = false;
  // IsGuestNetwork(jwtGuest_, serverGuest_): the jwt's GuestMode claim (a
  // refresh clears it) or the server's `guest` (no login method, read live)
  bool isGuest_ = false;
  bool jwtGuest_ = false;
  bool serverGuest_ = false;
  std::string subscriptionStoreFamily_;
  bool didDetectUpgradeToPro_ = false;

  int64_t usedByteCount_ = 0;
  int64_t pendingByteCount_ = 0;
  int64_t availableByteCount_ = 0;
  int64_t startBalanceByteCount_ = 0;
  int64_t fetchedAtMillis_ = 0;
  // FetchBalanceThen callers waiting on the read in flight
  std::vector<std::function<void(bool)>> fetchWaiters_;

  bool isLoadingReferral_ = false;
  ReferralTotalsFetch totals_;
  int64_t maxReferrals_ = 20;
  int64_t bonusGibPerDay_ = 3;
  int64_t referredBonusGibPerDay_ = 3;
  ReferralCodeFetch referral_;

  PriceTierView tier_;
  OfferView offer_;
  std::vector<urnet::ExperimentAssignment> experiments_;
  // the balance result's crypto payment fields, as the server sent them (the
  // term's asset among them, which nothing here reads: no surface names the
  // coin a year was paid with)
  std::optional<urnet::CryptoPaymentAssetList> cryptoAssets_;
  std::optional<urnet::CryptoPaymentAssetList> cryptoUnavailableAssets_;
  std::optional<urnet::CryptoTerm> cryptoTerm_;
  // the latest balance result carried payment_past_due
  bool paymentPastDue_ = false;

  // polling (mac: backgroundPollingTimer / pollingTimer / pollingDeadline).
  // The gate holds whether a confirmation runs and its budget, which only
  // elapses while the poll timer runs (visible and focused).
  sigc::connection backgroundTimer_;
  sigc::connection pollingTimer_;
  ConfirmationPollGate gate_{kConfirmationBudgetMillis};
  bool purchaseConfirmationTimedOut_ = false;
};

}  // namespace urnw
