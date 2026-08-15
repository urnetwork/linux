// The Earnings destination — wallets, the payouts/leaderboard ledger and the
// network-points column merged into ONE destination (port of the windows
// WalletPage + WalletSheets, spec docs/parity/earnings.md). The old separate
// Leaderboard destination is gone; its tables live behind a tab in the ledger.
//
// Three panes, folded by the window's ApplyBreakpoint:
//   >= 1500  wallets(360) | ledger(*) | points(380)
//   >=  900  wallets(360) | ledger(*)
//   <   900  ledger(*) only — the LEDGER survives to the smallest width: a
//            payouts table is what the user opens this destination to read.
//
// Pane A is the payout wallets: three header stats (pending payout, unpaid
// data, referrals), the payout-threshold note, the Stripe upgrade action, one
// 44px row per wallet, and the connect-a-wallet form whose address box is
// validated per chain (SOL > MATIC > TAO precedence) behind a 300ms debounce.
// A TAO address CAN be connected and can NEVER be a payout wallet — the
// affordance is replaced by the reason wherever it would appear.
//
// Pane B is the ledger: a two-item tab switch over the payouts table and the
// leaderboard table, exactly one visible. The leaderboard is fetched the FIRST
// time its tab is looked at (one-shot), never on destination selection.
//
// Pane C is the network-earnings column: own ranking, the public-leaderboard
// switch (echo-guarded — the handler cannot tell a user flip from the
// programmatic render of the server's answer), the account-points breakdown,
// the seeker multiplier block and the reliability window.
//
// Every panel settles on exactly one of Loading / Ready-empty / Failed, and
// the three renders are distinguishable: the header stats show a FAINT dash
// when unloaded or failed (a dash must read "no answer yet", never "the
// answer is nothing"), each list carries its own status line, and one panel's
// failure never blanks another — LoadWallet fires EIGHT independent requests.
//
// Every server write AND every server question is gated by CanCallApi(); the
// one affordance allowed to decline silently is address validation while
// typing (the user did not ask for anything).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

#include "PaneKit.hpp"
#include "SdkHost.hpp"
#include "Ui.hpp"

namespace urnw {

class EarningsPage : public Gtk::Box {
 public:
  explicit EarningsPage(SdkHost& host);
  ~EarningsPage() override;

  // nav-select + auth-change: the eight independent wallet-pane fetches.
  // Bumps the stale-async epoch first, so every completion armed for the
  // previous session is dropped before it touches a widget.
  void Load();

  // The spec's pane-fold table (window width in dip).
  void ApplyBreakpoint(int widthDip);

  // The window's balance relay: the Upgrade action is visible iff
  // !isPro && !guest — hidden for Pro AND for guests (an account comes first).
  void SetBalanceState(bool isPro, bool guest);

  // ---- preview harness (--preview-ui) ---------------------------------------
  // Preview mode makes CanCallApi() false everywhere (a preview build once
  // reached production authenticated) and swaps the loads for
  // ShowPreviewState(), which settles EVERY panel on its real empty state —
  // a permanent "Loading..." is indistinguishable from a hang.
  void SetPreviewMode(bool on);
  void ShowPreviewState();
  // --preview-ui=wallet: raise the wallet snackbar at Error severity, which is
  // the persistent treatment (demonstrates the severity gate).
  void ShowPreviewSnackbar();
  // URNETWORK_PREVIEW_SAMPLE=1 (a SECOND gate): obviously-synthetic rows
  // pushed through the SAME Apply* functions. Sample rows are INTERACTIVE,
  // which is exactly why CanCallApi() gates the actions and not the loads.
  void ApplyPreviewSample();

  // ---- window-level routing --------------------------------------------------
  // OnOpenUpgrade: guests go to the create-account (guest upgrade) flow,
  // everyone else to the existing UpgradeSheet. The window owns both, so the
  // page emits and the window routes.
  std::function<void()> on_open_upgrade;
  // The window's one-ContentDialog-at-a-time gate: every sheet open path asks
  // first and bails, and reports both edges. Unbound = the page's own flag is
  // the only gate.
  std::function<bool()> sheet_open;
  std::function<void(bool open)> on_sheet_open_changed;
  // Fallback snackbar surface. The spec puts one InfoBar in pane A and one in
  // pane C so a message lands beside content the user is looking at; below
  // 1500dip pane C is folded away and below 900dip pane A is too, so a message
  // with no visible bar is handed to the shell instead of being dropped
  // (spec FLAG: "consider routing to whichever bar is actually visible").
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;

 private:
  // Per-panel fetch state. Loading / Ready / Failed must render distinguishably
  // — a list that renders the same three ways is the bug this enum exists for.
  //
  // NoSession is the FOURTH state and it is LEADERBOARD-ONLY (ApplyLeaderboard
  // is the only applier that handles it; nothing else is ever passed it). It
  // exists because "we never asked" was being rendered as Ready+empty, i.e. as
  // the authoritative answer "there are no networks on the leaderboard" — the
  // exact bug in this file's history: LoadWallet's no-session settle stamped
  // that string into a pane nobody had asked about, and the one-shot fetch flag
  // then made it permanent for the life of the process.
  enum class Fetch { Loading, Ready, Failed, NoSession };

  // One non-repeating watchdog + a generation. BeginFlow bumps the generation
  // and arms the timer; on timeout the handler bumps AGAIN (making the give-up
  // final: a late success must not undo what the user was already told) and
  // runs the timeout action. SettleFlow returns false — caller must do NOTHING
  // — when its generation was superseded or already timed out.
  struct Flow {
    uint32_t generation = 0;
    sigc::connection timer;
  };
  uint32_t BeginFlow(Flow& flow, int timeoutMs, std::function<void()> onTimeout);
  bool SettleFlow(Flow& flow, uint32_t generation, const char* what);

  // ---- construction ----------------------------------------------------------
  void BuildWalletsPane();
  void BuildLedgerPane();
  void BuildPointsPane();

  // ---- loads -----------------------------------------------------------------
  void LoadWallet();
  void LoadLeaderboard();

  // ---- appliers (one writer per surface) -------------------------------------
  void ApplyWallets(std::optional<urnet::AccountWalletsList> wallets, Fetch state);
  void ApplyPayoutWalletId(const std::string& walletId);
  void ApplyTransferStats(bool ok, int64_t unpaidBytes);
  void ApplyWalletBalance(bool ok, int64_t balanceNanoCents);
  void ApplyReferrals(bool ok, int64_t totalReferrals);
  void ApplyPoints(std::optional<urnet::AccountPointsList> points, Fetch state);
  void ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state);
  void ApplyPayments(std::optional<urnet::AccountPaymentsList> payments, Fetch state);
  void ApplyRanking(std::optional<urnet::NetworkRanking> ranking, bool ok);
  void ApplyLeaderboard(std::optional<urnet::LeaderboardEarnersList> earners, Fetch state);

  // ---- rebuilders ------------------------------------------------------------
  void RebuildWalletCards();
  void RebuildPayouts();
  void RebuildLeaderboard();
  void RebuildPointsCard();
  void RebuildReliabilityCard();
  void ApplySeekerState();
  void ApplyLedgerMeta();
  void OnLedgerTabChanged();

  // ---- connect a wallet ------------------------------------------------------
  void OnWalletAddressChanged();
  void ValidateWalletAddress();
  void ApplyWalletValidation(const std::string& chain, uint64_t generation, bool valid);
  void OnConnectWallet();
  void ApplyWalletConnectResult(uint32_t generation, bool ok, const std::string& serverError);

  // ---- the seeker browser-bridge flow ---------------------------------------
  void OnVerifySeeker();
  void StartSeekerVerification(WalletConnect::Provider provider);
  void ApplySeekerResult(uint32_t generation, bool ok, const std::string& serverError);

  // ---- the public-leaderboard switch ----------------------------------------
  void SetRankingToggle(bool on);  // the echo-guarded programmatic write
  void OnLeaderboardPublicToggled();
  void ApplyRankingPublicResult(uint32_t generation, bool ok, bool requested,
                                const std::string& serverError);

  // ---- sheets ----------------------------------------------------------------
  void ShowWalletDetail(const urnet::AccountWallet& wallet);
  void ShowPayoutDetail(const urnet::AccountPayment& payment);
  void PresentSheet(const std::shared_ptr<Gtk::Window>& sheet);
  void CloseSheet();

  // ---- gating + messaging ----------------------------------------------------
  // !previewUi && apiReady() && IsLoggedIn(): guards every server write AND
  // every server question, not only the loads.
  bool CanCallApi();
  void RefuseNoSession();
  void Notify(const Glib::ustring& message, kit::Snackbar::Severity severity);
  // The stat placeholder rule: unloaded or failed renders a FAINT dash — a
  // dash must read "no answer yet", never "the answer is nothing". The key is
  // passed so the value re-announces as "Label, value" on every write.
  void SetStatValue(Gtk::Label* value, const Glib::ustring& key,
                    const Glib::ustring& text, bool loaded);
  // Settle every panel on its real empty state (no session / preview): a
  // permanent "Loading..." is indistinguishable from a hang.
  void SettleAllEmpty();
  // Lifetime COMPLETED usdc paid into a wallet (iOS totalPaymentsByWalletId).
  double TotalPaidToWallet(const std::string& walletId) const;

  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);  // stale-async guard
  // Liveness token for marshaled work that must run even across a Load()
  // (the sheet-dismiss cleanup): the epoch answers "is this answer stale",
  // this answers "does the page still exist".
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // ---- pane shell ------------------------------------------------------------
  kit::Pane paneA_;  // WALLETS (360)
  kit::Pane paneB_;  // LEDGER (*)
  kit::Pane paneC_;  // POINTS (380)
  Gtk::Widget* ruleB_ = nullptr;
  Gtk::Widget* ruleC_ = nullptr;
  int lanes_ = -1;  // last applied fold (3 / 2 / 1); -1 = never applied

  // pane A widgets
  Gtk::Label* pendingValue_ = nullptr;
  Gtk::Label* unpaidValue_ = nullptr;
  Gtk::Label* referralsValue_ = nullptr;
  Gtk::Button* upgradeButton_ = nullptr;
  Gtk::Label* walletsStatus_ = nullptr;
  Gtk::Box* walletCardsPanel_ = nullptr;
  Gtk::Widget* walletsEmptyPanel_ = nullptr;
  Gtk::Entry* walletAddressBox_ = nullptr;
  Gtk::Label* walletChainText_ = nullptr;
  Gtk::Button* connectWalletButton_ = nullptr;
  kit::Snackbar walletInfo_;

  // pane B widgets
  Gtk::ToggleButton* payoutsTab_ = nullptr;
  Gtk::ToggleButton* leaderboardTab_ = nullptr;
  Gtk::Box* payoutsHost_ = nullptr;
  Gtk::Box* payoutsPanel_ = nullptr;
  Gtk::Label* payoutsStatus_ = nullptr;
  Gtk::Box* leaderboardHost_ = nullptr;
  Gtk::Box* leaderboardRows_ = nullptr;
  Gtk::Label* leaderboardStatus_ = nullptr;

  // pane C widgets
  Gtk::Label* netProvidedValue_ = nullptr;
  Gtk::Label* rankValue_ = nullptr;
  Gtk::Switch* publicToggle_ = nullptr;
  kit::Snackbar leaderboardInfo_;
  Gtk::Label* accountPointsStatus_ = nullptr;
  Gtk::Box* accountPointsCard_ = nullptr;   // the row (collapsed until Ready)
  Gtk::Box* accountPointsPanel_ = nullptr;  // its content column
  Gtk::Label* seekerStatus_ = nullptr;
  Gtk::Button* verifySeekerButton_ = nullptr;
  Gtk::Label* reliabilityStatus_ = nullptr;
  Gtk::Box* reliabilityCard_ = nullptr;
  Gtk::Box* reliabilityPanel_ = nullptr;

  // ---- state -----------------------------------------------------------------
  urnet::AccountWalletsList wallets_;
  Fetch walletsState_ = Fetch::Loading;
  std::string payoutWalletId_;
  urnet::AccountPaymentsList payments_;
  Fetch paymentsState_ = Fetch::Loading;
  urnet::AccountPointsList points_;
  Fetch pointsState_ = Fetch::Loading;
  std::optional<urnet::ReliabilityWindow> reliability_;
  Fetch reliabilityState_ = Fetch::Loading;
  urnet::LeaderboardEarnersList leaderboard_;
  Fetch leaderboardState_ = Fetch::Loading;
  int leaderboardCount_ = 0;
  std::string ownNetworkId_;  // the JWT's network id: the own-row highlight
  bool rankingPublic_ = false;
  bool seekerHolder_ = false;  // any wallet with has_seeker_token

  // in-flight gates
  bool connectingWallet_ = false;
  bool verifyingSeeker_ = false;
  bool settingRankingPublic_ = false;
  bool applyingRankingToggle_ = false;  // ECHO GUARD on the public switch
  // One-shot PER LOOK, not per process: set when a look at the Leaderboard tab
  // issues the fetch, and re-armed by Load() and by every no-session settle, so
  // a board fetched (or refused) under one session never outlives it.
  bool leaderboardRequested_ = false;

  // detected chains for the address in the box (SOL > MATIC > TAO precedence)
  bool walletValidSol_ = false;
  bool walletValidMatic_ = false;
  bool walletValidTao_ = false;
  std::string walletChain_;
  uint64_t walletValidateGeneration_ = 0;  // bare counter: no timer, no watchdog
  sigc::connection walletDebounce_;

  Flow seekerFlow_;   // 180s: the user legitimately spends minutes in a browser
  Flow connectFlow_;  // 20s
  Flow rankingFlow_;  // 20s

  // preview + balance relay
  bool previewMode_ = false;
  bool samplePinned_ = false;
  bool isPro_ = false;
  bool isGuest_ = false;

  // The page holds the open sheet for its whole life, so a late callback after
  // dismissal finds nothing (the sheet captures itself weakly).
  std::shared_ptr<Gtk::Window> sheet_;
};

}  // namespace urnw
