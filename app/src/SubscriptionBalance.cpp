// SPDX-License-Identifier: MPL-2.0
#include "SubscriptionBalance.hpp"

#include <cmath>

#include "ReferralRoyalty.hpp"

#include "GuestConversion.hpp"

#include "AccountPlanRows.hpp"
#include "AppPrefs.hpp"
#include "CryptoPay.hpp"
#include "PaymentPastDue.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

#include <glib.h>

#include "Ui.hpp"  // PostToMain

namespace urnw {

// AccountPlanRows.hpp reads the sdk's plan values by their text
static_assert(std::string_view(urnet::ShownPlanPro) == account_plan::kShownPlanPro);
static_assert(std::string_view(urnet::ShownPlanFree) == account_plan::kShownPlanFree);
static_assert(std::string_view(urnet::ShownPlanUnknown) == account_plan::kShownPlanUnknown);

namespace {
constexpr unsigned kBackgroundPollingSeconds = 30;   // mac backgroundPollingInterval
// mac pollingInterval. The give-up budget (kConfirmationBudgetMillis, mac
// maxPollingDuration) is ACTIVE polling time: it pauses with this poll while
// the window is hidden or the app is unfocused (ConfirmationPollGate).
constexpr unsigned kConfirmationPollingSeconds = 5;

// monotonic milliseconds for the gate (never jumps with the wall clock)
int64_t NowMillis() { return g_get_monotonic_time() / 1000; }
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
  subscriptionStoreFamily_.clear();
  usedByteCount_ = pendingByteCount_ = availableByteCount_ = startBalanceByteCount_ = 0;
  fetchedAtMillis_ = 0;
  // the epoch bump orphans a read in flight; a FetchBalanceThen waiter is
  // served by this session's first read instead
  isLoading_ = false;
  totals_.Reset();
  referral_.Reset();
  // the coins, the crypto term and a failing renewal belong to the network the
  // fetch answers for
  cryptoAssets_.reset();
  cryptoUnavailableAssets_.reset();
  cryptoTerm_.reset();
  paymentPastDue_ = false;

  // Offline Pro: the jwt's Pro (and GuestMode) claims are readable without a
  // network call. The sdk's decision reads a Pro claim until the first fetch
  // lands; a claim of Free is not shown as Free until then (ShownPlan).
  serverGuest_ = false;
  proTracker_.reset();
  TrackStoredJwt();
  isPro_ = proTracker_.getIsPro();
  if (auto byJwt = host_.ParseByJwt()) {
    jwtGuest_ = byJwt->GuestMode;
  } else {
    jwtGuest_ = false;
  }
  isGuest_ = IsGuestNetwork(jwtGuest_, serverGuest_);

  if (windowVisible_) {
    if (!isPro_) {
      StartBackgroundPolling();  // fetches immediately, then every 30s
    } else {
      FetchNow();  // Pro networks don't poll (mac parity), but the bar needs data once
    }
    EnsureReferralPolling();  // referrals poll for Pro networks too
  }
  // a connect waiting on a read (FetchBalanceThen) gets one with the window hidden too
  if (!fetchWaiters_.empty()) FetchSubscriptionBalance();
  Emit();
}

void SubscriptionBalanceStore::Stop() {
  ++*epoch_;  // drop in-flight results
  started_ = false;
  StopPolling();
  referralTimer_.disconnect();
  isLoading_ = false;
  isLoadingReferral_ = false;
  hasFetched_ = false;
  proTracker_.reset();
  isPro_ = false;
  isGuest_ = false;
  jwtGuest_ = false;
  serverGuest_ = false;
  subscriptionStoreFamily_.clear();
  didDetectUpgradeToPro_ = false;
  usedByteCount_ = pendingByteCount_ = availableByteCount_ = startBalanceByteCount_ = 0;
  fetchedAtMillis_ = 0;
  fetchWaiters_.clear();
  totals_.Reset();
  referral_.Reset();
  cryptoAssets_.reset();
  cryptoUnavailableAssets_.reset();
  cryptoTerm_.reset();
  paymentPastDue_ = false;
  purchaseConfirmationTimedOut_ = false;
  Emit();
}

void SubscriptionBalanceStore::SetWindowVisible(bool visible) {
  const bool wasVisible = windowVisible_;
  windowVisible_ = visible;
  // the gate banks the confirmation budget on hide and re-arms it on show
  const bool resume = gate_.SetVisible(visible, NowMillis());
  if (!visible) {
    // Do not keep periodic main-loop wakeups merely to discover that the
    // window is still hidden.
    backgroundTimer_.disconnect();
    pollingTimer_.disconnect();
    referralTimer_.disconnect();
    return;
  }
  if (!started_ || wasVisible) return;
  EnsureReferralPolling();
  if (gate_.Confirming()) {
    // immediate poll with the banked budget, unless the app is still unfocused
    if (resume) ResumeConfirmationPolling();
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

void SubscriptionBalanceStore::SetAppFocused(bool focused) {
  const bool resume = gate_.SetFocused(focused, NowMillis());
  if (!gate_.Running()) {
    // focus loss (paying in the browser looks exactly like this): the
    // confirmation poll and its budget pause; the background poll is not
    // confirming and keeps its visibility gate
    pollingTimer_.disconnect();
    return;
  }
  if (resume && started_) ResumeConfirmationPolling();
}

void SubscriptionBalanceStore::FetchNow() {
  if (!started_) return;
  FetchSubscriptionBalance();
  FetchReferralCode();
}

void SubscriptionBalanceStore::FetchBalanceThen(std::function<void(bool ok)> done) {
  // a connect right after sign-in (or on launch) can come before Start()
  if (!started_ && !host_.IsLoggedIn()) {
    done(false);
    return;
  }
  fetchWaiters_.push_back(std::move(done));
  FetchSubscriptionBalance();
}

void SubscriptionBalanceStore::OnJwtRefreshed() {
  if (!started_) return;
  auto byJwt = host_.ParseByJwt();
  if (!byJwt) return;
  const bool before = isPro_;
  const bool guestBefore = isGuest_;
  const std::string shownPlanBefore = proTracker_.getShownPlan();
  // The refresh renewed the client token; the stored jwt is usually the one
  // read before, and the sdk's decision keeps Pro the server's once a fetch
  // answered. Re-deriving Pro from its claim here flipped Pro back, and the
  // poll that switched on read the balance again at once (support inbox 8211).
  TrackStoredJwt();
  UpdateIsPro(proTracker_.getIsPro());  // only before the first fetch
  // a refresh signs the jwt without GuestMode; the server's guest still holds
  jwtGuest_ = byJwt->GuestMode;
  isGuest_ = IsGuestNetwork(jwtGuest_, serverGuest_);
  // the plan can change without Pro (another network's jwt starts over)
  if (isPro_ != before || isGuest_ != guestBefore ||
      proTracker_.getShownPlan() != shownPlanBefore) {
    Emit();
  }
}

void SubscriptionBalanceStore::TrackStoredJwt() {
  if (const std::string byJwt = host_.StoredByJwt(); !byJwt.empty()) {
    proTracker_.setJwt(byJwt);
  }
}

// mac updateIsPro: Pro stops all polling; a lapse back to free restarts the
// background poll. Not with a read at once: Pro changes on a read (or, before
// the first one, with the stored jwt), so the balance is fresh.
void SubscriptionBalanceStore::UpdateIsPro(bool isPro) {
  if (isPro == isPro_) return;
  isPro_ = isPro;
  StopPolling();
  if (!isPro) StartBackgroundPolling(/*fetchNow=*/false);
}

void SubscriptionBalanceStore::FetchSubscriptionBalance() {
  if (isLoading_) return;
  isLoading_ = true;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  // the storefront variant of the balance call: the same balance plus the
  // price tier, the welcome offer and the experiment assignments. There is no
  // storefront on the desktop, so the server resolves the tier from billing
  // or the request's country (an estimate until the card's country is known).
  host_.api().subscriptionBalanceForStorefront(
      "", [this, epoch, issued](std::optional<urnet::SubscriptionBalanceResult> result,
                                std::optional<std::string> err) {
        PostToMain([this, epoch, issued, result = std::move(result), err = std::move(err)] {
          if (*epoch != issued) return;  // logged out (or re-logged-in) since
          isLoading_ = false;
          if (err || !result) {
            if (err) std::fprintf(stderr, "[balance] fetch failed: %s\n", err->c_str());
            errorFetching_ = true;
            // before the server's first answer the plan turns failed, with
            // Try again, never Free; the Emit below repaints it
            proTracker_.balanceFailed();
          } else {
            errorFetching_ = false;
            hasFetched_ = true;
            fetchedAtMillis_ = NowMillis();
            availableByteCount_ = result->balance_byte_count;
            pendingByteCount_ = result->open_transfer_byte_count;
            usedByteCount_ =
                result->start_balance_byte_count - availableByteCount_ - pendingByteCount_;
            startBalanceByteCount_ = result->start_balance_byte_count;
            if (result->price_tier) {
              tier_.name = result->price_tier->name.empty() ? kPriceTierStandard
                                                            : result->price_tier->name;
              if (0 < result->price_tier->yearly_usd) tier_.yearly = result->price_tier->yearly_usd;
              if (0 < result->price_tier->monthly_usd) tier_.monthly = result->price_tier->monthly_usd;
              if (!result->price_tier->currency.empty()) tier_.currency = result->price_tier->currency;
            }
            if (result->onboarding_offer) {
              SetOffer(*result->onboarding_offer);
            } else {
              offer_.active = false;
            }
            experiments_.clear();
            if (result->experiments) {
              for (const auto& a : *result->experiments) experiments_.push_back(a);
            }
            // the enabled coins (omitted when none, and by an older server, so
            // no crypto method is offered then), the ones that are on but
            // cannot be quoted now (greyed in the selector), and a one-time
            // crypto year whose grace has not ended
            cryptoAssets_ = result->crypto_assets;
            cryptoUnavailableAssets_ = result->crypto_unavailable_assets;
            cryptoTerm_ = result->crypto_term;
            // a renewal the store is still retrying after its payment did not
            // go through (left out by an older server): the past-due notice
            paymentPastDue_ = past_due::NoticeShows(*result);

            // The server is the source of truth for Pro: `current_subscription`
            // is non-nil exactly when the network is Pro. The jwt's Pro claim
            // is baked in at issue time, so it goes stale on BOTH an upgrade
            // and a lapse; the sdk decides when to refresh it (below).
            const bool serverIsPro = result->current_subscription.has_value();
            // no login method on the network (a legacy guest), read live by
            // the server: right even after a refresh cleared the jwt claim
            serverGuest_ = result->guest.value_or(false);
            isGuest_ = IsGuestNetwork(jwtGuest_, serverGuest_);
            subscriptionStoreFamily_ =
                serverIsPro ? urnet::classifySubscriptionStore(result->current_subscription->store)
                            : std::string();
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
            // The sdk's decision (urnet::SubscriptionProTracker): Pro is the
            // server's from here on, and a claim that disagrees asks for one
            // refresh per stored jwt. The refresh renews the client token and
            // leaves the stored jwt as it was, so asking on every read was
            // the ping-pong of support inbox 8211. A stored jwt renewed since
            // (it renews on its own schedule) is read again here.
            TrackStoredJwt();
            const bool refreshJwt = proTracker_.balanceLoaded(serverIsPro);
            UpdateIsPro(proTracker_.getIsPro());
            if (refreshJwt) host_.RefreshJwt();
          }

          // confirmation poll bookkeeping. mac runs these checks after EVERY
          // awaited fetch — errors included, or an unreachable server would
          // keep the confirmation poll spinning past its deadline forever.
          if (gate_.Confirming()) {
            if (IsSupporterWithBalance()) {
              StopPolling();
            } else if (gate_.ExpiredAt(NowMillis())) {
              // the server never confirmed within the (active-time) window —
              // stop hammering the api and tell the user, rather than
              // spinning for the session
              GiveUpConfirmationPolling();
            }
          } else if (IsSupporterWithBalance()) {
            StopPolling();  // background poll stops once supporter-with-balance
          }
          Emit();
          std::vector<std::function<void(bool)>> waiters;
          waiters.swap(fetchWaiters_);
          const bool ok = !err && result;
          for (auto& waiter : waiters) waiter(ok);
        });
      });
}

void SubscriptionBalanceStore::SetOffer(const urnet::OnboardingOffer& offer) {
  offer_.active = offer.state == "active";
  if (0 < offer.percent_off) offer_.percentOff = offer.percent_off;
  if (0 < offer.months_free) offer_.monthsFree = offer.months_free;
  if (0 < offer.regular_year_usd) offer_.regularYear = offer.regular_year_usd;
  offer_.firstYear = 0 < offer.first_year_usd ? offer.first_year_usd
                                              : OfferFirstYear(offer_.regularYear, offer_.percentOff);
  if (!offer.currency.empty()) offer_.currency = offer.currency;
  offer_.expiresAt = offer.expires_at;
}

std::vector<crypto_pay::PaymentTabView> SubscriptionBalanceStore::PaymentTabs(
    const std::string& plan) const {
  std::vector<crypto_pay::PaymentTabView> tabs;
  try {
    // USDC on Solana, Zcash, Monero, then More coins, each coin saying whether
    // it sells the plan now; a coin the balance lists as unavailable stays in
    // its place, greyed, and whether there is a selector at all (none while
    // neither list names a coin) is the SDK's to say
    const auto answer = urnet::cryptoPaymentTabs(cryptoAssets_, cryptoUnavailableAssets_,
                                                 urnet::StringList{plan});
    if (!answer) return tabs;
    for (const urnet::CryptoPaymentTab& sdkTab : *answer) {
      crypto_pay::PaymentTabView tab;
      tab.id = sdkTab.id;
      tab.label = sdkTab.label;
      tab.labelKey = sdkTab.label_key;
      if (sdkTab.icon_keys) tab.iconKeys = *sdkTab.icon_keys;
      if (sdkTab.coins) {
        for (const urnet::CryptoPaymentCoin& sdkCoin : *sdkTab.coins) {
          crypto_pay::PaymentCoinView coin;
          coin.asset = sdkCoin.asset;
          coin.label = sdkCoin.label;
          coin.labelKey = sdkCoin.label_key;
          coin.iconKey = sdkCoin.icon_key;
          coin.available = sdkCoin.available;
          // the line under it in the More coins list: the network it is sent on
          if (const auto lines = urnet::cryptoAssetLinesFor(sdkCoin.asset)) {
            coin.networkLine = {lines->network_line_key, lines->network_line};
          }
          tab.coins.push_back(std::move(coin));
        }
      }
      tab.available = sdkTab.available;
      tabs.push_back(std::move(tab));
    }
  } catch (const std::exception& e) {
    g_warning("balance: the payment methods could not be read: %s", e.what());
    tabs.clear();
  }
  return tabs;
}

bool SubscriptionBalanceStore::CryptoTermNoticeDue() const {
  if (!cryptoTerm_) return false;
  try {
    return urnet::cryptoTermNoticeDue(cryptoTerm_);
  } catch (const std::exception& e) {
    g_warning("balance: the crypto term could not be read: %s", e.what());
    return false;
  }
}

std::string SubscriptionBalanceStore::CryptoTermGraceEnd() const {
  return cryptoTerm_ ? cryptoTerm_->grace_end_time : std::string();
}

std::string SubscriptionBalanceStore::ExperimentVariant(const std::string& surface) const {
  for (const auto& a : experiments_) {
    if (a.surface == surface) return a.variant;
  }
  return "";
}

std::string SubscriptionBalanceStore::ExperimentId(const std::string& surface) const {
  for (const auto& a : experiments_) {
    if (a.surface == surface) return a.experiment_id;
  }
  return "";
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
          if (err || !result || result->error) {
            g_warning("balance: getNetworkReferralCode failed: %s",
                      err ? err->c_str()
                          : (result && result->error ? result->error->message.c_str()
                                                     : "(no result)"));
            // the rows keep their last value (the poll retries), but a panel
            // with no code says the read failed
            totals_.Fail();
            if (referral_.Fail()) Emit();
            return;
          }
          totals_.Succeed(result->total_referrals);
          referral_.Succeed(result->referral_code.value_or(std::string()));
          // the program terms ride along (server pro.yml); zero means the
          // server reported none, so the display defaults stay
          auto gibPerDay = [](int64_t bytes, int64_t periodSeconds) -> int64_t {
            if (bytes <= 0 || periodSeconds <= 0) return 0;
            const double perDay = static_cast<double>(bytes) * 86400.0 / periodSeconds;
            return static_cast<int64_t>(std::llround(perDay / (1024.0 * 1024.0 * 1024.0)));
          };
          if (0 < result->max_referrals) maxReferrals_ = result->max_referrals;
          if (const int64_t gib = gibPerDay(result->bonus_per_referral_bytes, result->bonus_period_seconds); 0 < gib) {
            bonusGibPerDay_ = gib;
          }
          if (const int64_t gib = gibPerDay(result->referred_bonus_bytes, result->bonus_period_seconds); 0 < gib) {
            referredBonusGibPerDay_ = gib;
          }
          SetCurrentReferralTerms(ReferralTerms{maxReferrals_, bonusGibPerDay_, referredBonusGibPerDay_});
          MaybeCelebrateReferrals(result->total_referrals);
          Emit();
        });
      });
}

// The celebration baseline is the count the last celebration (or the first
// observation) left behind, persisted per network in the app prefs so an
// increment observed on this machine celebrates exactly once.
void SubscriptionBalanceStore::MaybeCelebrateReferrals(int64_t count) {
  auto byJwt = host_.ParseByJwt();
  if (!byJwt || !byJwt->NetworkId) return;
  const std::string key = "referral_celebrated_count_" + *byJwt->NetworkId;

  const nlohmann::json all = prefs::ReadAll();
  if (all.find(key) == all.end()) {
    // first observation for this network on this machine: baseline only --
    // pre-existing referrals (reinstall, second machine) are old news
    prefs::Set<int64_t>(key.c_str(), count);
    return;
  }

  const int64_t previous = prefs::Get<int64_t>(key.c_str(), 0);
  if (count > previous) {
    ReferralCelebration celebration{count - previous, previous == 0};
    prefs::Set<int64_t>(key.c_str(), count);
    if (onReferralCelebration_) onReferralCelebration_(celebration);
  } else if (count < previous) {
    // referrals can be unlinked; re-baseline quietly
    prefs::Set<int64_t>(key.c_str(), count);
  }
}

void SubscriptionBalanceStore::RetryReferral() {
  if (!started_) return;
  referral_.Retry();
  totals_.Retry();
  Emit();
  FetchReferralCode();  // an in-flight read answers this retry instead
}

// Unlike the balance poll, referral polling never stops for a Pro network:
// referrals keep landing either way, and the crowning should fire while the
// user is looking at the app rather than a session later.
void SubscriptionBalanceStore::EnsureReferralPolling() {
  referralTimer_.disconnect();
  if (!started_ || !windowVisible_) return;
  referralTimer_ = Glib::signal_timeout().connect_seconds(
      [this]() -> bool {
        FetchReferralCode();
        return true;
      },
      kBackgroundPollingSeconds);
}

void SubscriptionBalanceStore::StartBackgroundPolling(bool fetchNow) {
  backgroundTimer_.disconnect();
  if (!windowVisible_) return;
  if (fetchNow) FetchNow();
  backgroundTimer_ = Glib::signal_timeout().connect_seconds(
      [this]() -> bool {
        FetchNow();
        return true;
      },
      kBackgroundPollingSeconds);
}

void SubscriptionBalanceStore::StartConfirmationPolling() {
  if (gate_.Confirming()) return;
  backgroundTimer_.disconnect();

  // A fresh confirmation attempt: clear any previous give-up and grant the
  // full budget. The gate spends it only while the poll runs, so hidden or
  // unfocused stretches (the user paying in the browser) never count toward
  // the 2 minutes.
  purchaseConfirmationTimedOut_ = false;
  gate_.Start(NowMillis());

  Emit();
  ResumeConfirmationPolling();
}

void SubscriptionBalanceStore::ResumeConfirmationPolling() {
  // hidden or unfocused: the gate holds the budget until it opens again
  if (!started_ || !gate_.Running()) return;
  if (gate_.ExpiredAt(NowMillis())) {
    // resumed with nothing left (the budget ran out exactly at pause time)
    GiveUpConfirmationPolling();
    Emit();
    return;
  }

  // immediate poll on resume: a webhook that landed while hidden confirms now
  FetchNow();
  pollingTimer_.disconnect();
  pollingTimer_ = Glib::signal_timeout().connect_seconds(
      [this]() -> bool {
        // Check independently of the API callback. A callback that is delayed
        // or never arrives must not keep the confirmation timer alive forever.
        if (gate_.ExpiredAt(NowMillis())) {
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
  if (gate_.Confirming()) {
    gate_.Stop();
    Emit();
  }
}

void SubscriptionBalanceStore::ClearPurchaseConfirmationTimeout() {
  purchaseConfirmationTimedOut_ = false;
  Emit();
}

}  // namespace urnw
