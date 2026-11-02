// The Earnings destination — points first.
//
// Points are URnetwork's own system and are always the headline: the points
// earned with their breakdown, and a per-epoch history. The UR protocol is an
// OPT-IN layer on top: once a Bittensor coldkey is attached (signed through the
// ur.io wallet bridge, purpose "connect", verified by POST /sn/wallet) the same
// history rows gain an SN25α column, an Unclaimed tile appears and a claim
// dialog lets the user settle the alpha. Claiming is DIRECT between the SDK on
// this device and the settlement vault contract: the SDK builds, signs (with
// its own gas key) and sends the transaction; no URnetwork API is in the path.
// Nothing is retroactive — alpha accrues from the first epoch after the wallet
// is attached; earlier epochs earned points only. Settlement v2 shows apart
// from those v1 claims: a tile with the coldkey's v2 ledger accounts and its
// own claim dialog, claimed the same direct way from the v2 vault
// (SnLedgerPresentation.hpp).
//
// USDC payouts continue until the migration to Bittensor completes, so the
// Solana payout wallet stays reachable from here: a three-dot overflow beside
// the Bittensor actions opens "Connect Solana wallet" (SolanaWalletSheet: the
// wallet bridge, connect only, or an address entered by hand), a card under the
// Bittensor block shows the payout wallet with the USDC waiting for it and
// removes it, and with no payout wallet one "N USDC waiting" line sits above
// the Bittensor actions. Its three reads (account wallets, payout wallet,
// account payments) are secondary: they settle the card together, never touch
// the Bittensor block, and a plain reload keeps the card while they are out.
// There is no payout history on this surface (support@ur.io holds the old
// ledger).
//
// Three panes, folded by the window's ApplyBreakpoint:
//   >= 1500  earnings(360) | history(*) | network(380)
//   >=  900  earnings(360) | history(*)
//   <   900  history(*) only — the HISTORY survives to the smallest width.
//
// Pane A (earnings): the points headline with its Providing / Referral /
// Reliability breakdown (and the Seeker multiplier, points-only), the protocol
// note with the ur.xyz link, the Unclaimed SN25α tile
// (wallet only) and the Settlement v2 tile under it, then the payout consent
// (signed in: the network's coldkey consent, signed with the Bittensor wallet,
// NetworkConsent.hpp), the Bittensor wallet
// block (connect through the bridge, or
// enter an address manually — validated locally and then against
// /sn/wallet/validate before anything is sent — and still sign it), the Solana
// payout wallet card (only with a payout wallet), and the Top 200 tile: the
// projected place from GET /sn/top/me (the Protocol board's own place) for a
// network projected inside the top 200 or claimed, with "Claiming opens soon"
// while binding is closed (ProtocolBoardPresentation.hpp TileFor).
//
// Pane B (history): a two-item tab switch over the per-epoch history and the
// leaderboard. The leaderboard is fetched the FIRST time its tab is looked at.
// It has three boards: Protocol (the first and the default, ProtocolBoard.hpp),
// Points and Data.
//
// Pane C (network): the board's own block (the Protocol board's banner,
// own-status card and explainer, which sit on top of its list when pane C is
// folded away; the Points board's block; or the Data board's own ranking and
// the public-leaderboard switch, echo-guarded),
// the reliability window, and under it the extender statistics and the
// provider statistics with the provide mode row and the read-only extender row
// (EXTENDER.md N7, O5, O8). Under the provide mode row, one line says why an
// enabled provider is idle; after the provider plots, under their gate, the
// Demand histogram and "Why?" show the network's provider status for this
// device (support part P008, ProviderStatusPresentation.hpp).
//
// Every panel settles on exactly one of Loading / Ready-empty / Failed, and
// every server write AND every server question is gated by CanCallApi(); the
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

#include "DemandChart.hpp"
#include "ExtenderProvidePresentation.hpp"
#include "LeaderboardIndicator.hpp"
#include "NetworkConsent.hpp"
#include "PaneKit.hpp"
#include "ProtocolBoard.hpp"
#include "ProviderStatusPresentation.hpp"
#include "SdkHost.hpp"
#include "SnLedgerPresentation.hpp"
#include "SnPayoutPresentation.hpp"
#include "SolanaWalletPresentation.hpp"
#include "TransferChart.hpp"
#include "TransportBar.hpp"
#include "TransportSheet.hpp"
#include "Ui.hpp"

namespace urnw {

// ---- the earnings model -----------------------------------------------------
// Plain mirrors of the SDK earnings surface (EARNINGS_SDK_SPEC.md), so the
// page renders the same rows whether they come from the SDK bindings or from
// the preview sample.

struct SnWalletInfo {
  std::string coldkeySs58;
  std::string clientId;  // the provider client the wallet is attached to ("" = network)
  int64_t setAtMillis = 0;
};

struct AccountEpochRow {
  int64_t epoch = 0;
  int64_t startMillis = 0;
  int64_t endMillis = 0;
  double points = 0;
  int64_t shareBps = 0;
};

struct SnClaimRow {
  int64_t epoch = 0;
  int64_t shareBps = 0;
  int64_t amountRao = 0;   // 1 α = 1e9 rao
  std::string status;      // open | claimable | claimed | expired | not-finalized
  int64_t claimOpenBlock = 0;
  int64_t expiryBlock = 0;
  std::string txHash;
  std::string message;  // why the epoch is not claimable, when the status alone does not say
};

struct SnGasInfo {
  std::string address;     // 0x… (pays gas only; the secret never leaves the SDK)
  std::string mirrorSs58;  // fund it by sending TAO here
  bool balanceKnown = false;
  double tao = 0;
};

struct SnWalletCheck {
  bool validSyntax = false;
  bool existsOnChain = true;
  bool banned = false;
  std::string message;
};

class ClaimAlphaSheet;
class EmojiTagSheet;
class LedgerClaimSheet;
class Top200SignSheet;

class EarningsPage : public Gtk::Box {
 public:
  explicit EarningsPage(SdkHost& host);
  ~EarningsPage() override;

  // nav-select + auth-change: the independent earnings-pane fetches. Bumps the
  // stale-async epoch first, so every completion armed for the previous
  // session is dropped before it touches a widget.
  void Load();

  // Sign-out: forgets the own-account facts of the network that left, then
  // Loads, whose no-session path empties the boards, the history and the
  // points board. Those facts survive a Load, and the points header and both
  // public switches read them until the next account's answers land.
  void ResetForSignOut();

  // The provide-mode row (the connect page's indicator + label with the
  // current mode) and the providing gate: with providing off the reliability
  // chart hides and the group says so, the same gate and message as the stats
  // widget. Fed from the same live stats the connect page paints.
  // The provider statistics share the gate (EXTENDER.md O8).
  void ApplyProvideState(const LiveStats& stats);
  // the provide mode is changed on the connect page; the row opens it there
  // (and so does the read-only extender row, whose switch lives there too)
  std::function<void()> on_open_provide_settings;

  // The drawer's change feed (SdkHost::DrawerEvent), relayed by MainWindow
  // under the same visibility gate as the connect page's: the throughput tick
  // feeds the provider and extender charts and the provider transport bar, and
  // the extender status repaints the read-only extender row and the running
  // state behind the extender statistics (EXTENDER.md N7, O5, O8).
  void OnHostEvent(DrawerEvent event);
  // The window presenting or hiding to the tray (MainWindow's
  // reconcilePresentation), by ConnectPage::SetPresentationActive's rule that a
  // sheet may not outlive the surface that feeds it: hiding closes the provider
  // transport sheet, and showing re-reads the statistics and the extender row,
  // since every drawer event was dropped while hidden.
  void SetPresentationActive(bool active);

  // The points board's row and stat tile (public: a free helper in the .cpp builds rows).
  struct PointsRowUi {
    int64_t position = 0;  // the row's 1-based place in the whole ranking (no ties)
    std::string networkId;
    std::string displayName;  // empty when anonymous: the row shows "Anonymous"
    std::string emojiTag;     // shows either way
    bool anonymous = false;
    std::string totalPointsText;
    std::string blocksText;
    std::string streakText;
    std::string longestStreakText;
    std::string rankPointsText;
    std::string rankBlocksText;
    std::string rankStreakText;
    bool operator==(const PointsRowUi& o) const {
      return position == o.position && networkId == o.networkId && displayName == o.displayName &&
             emojiTag == o.emojiTag && anonymous == o.anonymous &&
             totalPointsText == o.totalPointsText && blocksText == o.blocksText &&
             streakText == o.streakText && longestStreakText == o.longestStreakText &&
             rankPointsText == o.rankPointsText && rankBlocksText == o.rankBlocksText &&
             rankStreakText == o.rankStreakText;
    }
    bool operator!=(const PointsRowUi& o) const { return !(*this == o); }
  };
  struct PointsStatTile {
    Gtk::Label* value = nullptr;
    Gtk::Label* rank = nullptr;
  };

  // The spec's pane-fold table (window width in dip).
  void ApplyBreakpoint(int widthDip);

  // The window's balance relay (kept for the window's wiring; the page has no
  // upgrade affordance any more — the Account destination owns plans).
  void SetBalanceState(bool isPro, bool guest);

  // ---- preview harness (--preview-ui) ---------------------------------------
  // Preview mode makes CanCallApi() false everywhere and swaps the loads for
  // ShowPreviewState(), which settles EVERY panel on its real empty state.
  void SetPreviewMode(bool on);
  void ShowPreviewState();
  // --preview-ui=wallet: raise the earnings snackbar at Error severity.
  void ShowPreviewSnackbar();
  // URNETWORK_PREVIEW_SAMPLE=1 (a SECOND gate): obviously-synthetic rows pushed
  // through the SAME Apply* functions. URNETWORK_PREVIEW_WALLET=1 adds the
  // attached-wallet layer (claims, gas key, alpha column, the v2 tile);
  // URNETWORK_PREVIEW_PROTOCOL=<state> opens the Leaderboard on its Protocol
  // board with a sample board in that state (ProtocolBoard::ApplyPreviewSample;
  // "claimed" is the Top 200 tile's claimed state);
  // URNETWORK_PREVIEW_MANUAL=1 opens the manual entry on a "new wallet" verdict;
  // URNETWORK_PREVIEW_GAS=low puts the claim dialog in its needs-gas state;
  // URNETWORK_PREVIEW_SOLANA=1 adds a Solana payout wallet with 3.87 USDC
  // waiting (the card); URNETWORK_PREVIEW_USDC_WAITING=1 the same figure with no
  // payout wallet (the waiting line above the Bittensor actions).
  void ApplyPreviewSample();
  // URNETWORK_PREVIEW_CLAIM=1: open the claim dialog over the sample.
  void ShowPreviewClaimDialog();

  // Earnings › Top 200 › Sign fleet binding (Top200SignSheet). The Top 200
  // block's button opens it with an empty field; the app link
  // urnetwork://top200/sign opens it whatever the block shows, pre-filled with
  // `prefill` (nullopt with `fromLink`: the link's binding did not decode). A
  // link that lands while the sheet is open pre-fills that sheet. Never signs.
  void OpenTop200SignSheet(const std::optional<std::string>& prefill, bool fromLink);

  // ---- window-level routing --------------------------------------------------
  // The window's one-ContentDialog-at-a-time gate.
  std::function<bool()> sheet_open;
  std::function<void(bool open)> on_sheet_open_changed;
  // Fallback snackbar surface for the folds where both pane bars are hidden.
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;

 private:
  // Per-panel fetch state. NoSession is LEADERBOARD-ONLY ("we never asked" is
  // not the answer "the board is empty").
  enum class Fetch { Loading, Ready, Failed, NoSession };

  // One non-repeating watchdog + a generation. BeginFlow bumps the generation
  // and arms the timer; on timeout the handler bumps AGAIN (making the give-up
  // final) and runs the timeout action. SettleFlow returns false — the caller
  // must do NOTHING — when its generation was superseded or already timed out.
  struct Flow {
    uint32_t generation = 0;
    sigc::connection timer;
  };
  uint32_t BeginFlow(Flow& flow, int timeoutMs, std::function<void()> onTimeout);
  bool SettleFlow(Flow& flow, uint32_t generation, const char* what);

  // ---- construction ----------------------------------------------------------
  void BuildEarningsPane();
  void BuildLedgerPane();
  void BuildNetworkPane();

  // ---- the Protocol board (apple/LEADERTOP.md) ------------------------------
  // "Claim on Earnings": pane A's Top 200 tile, scrolled to and focused; with
  // pane A folded away, the tile's own claim link.
  void OnProtocolClaim();
  // The Top 200 tile's "See the Protocol leaderboard": the Leaderboard tab on
  // its Protocol board, at the top.
  void ShowProtocolBoard();
  // The shared "Show my network name" switch seeded from the Protocol board's
  // own place, until the user or the Points board's `me` says otherwise.
  void ApplyProtocolPublic();

  // ---- the points board (android/POINTSLEADERBOARD.md) ----------------------
  // The leaderboard is three boards behind one switch: Protocol (the SN25 top
  // board, first and the default), Points (the all-time points board) and Data
  // (the last-4-payments board, frozen). The Points board is the
  // SDK's PointsLeaderboardViewController rendered as it is: rows, ranks, sort
  // and pages all come from the controller; nothing here sorts, ranks or
  // pages. The controller is opened the first time the Points board shows and
  // closed with the page.
  void BuildPointsBoard();         // pane B: the switch, the sort chips, the rows
  void BuildPointsNetworkBlock();  // pane C: this network's block
  void OnBoardTabChanged();
  // Opens the controller on the current device (or shows why it cannot);
  // safe to call on every look: a controller on a device that is still the
  // device is kept.
  void EnsurePointsBoard();
  void ClosePointsBoard(bool deviceAlive);
  // ClosePointsBoard without the redraw: closes the controller and resets the
  // board's state, touching no widget (the destructor's half)
  void ReleasePointsBoard(bool deviceAlive);
  // Mirrors the controller into the page: rows (value-compared, so a no-op
  // event does not re-render the table), sort, loading, end, error, `me`.
  void ReadPointsBoard();
  void RebuildPointsRows(size_t fromIndex = 0);
  std::string OwnPointsName();
  void RenderPointsHeader();
  void RenderPointsFooter();
  void OnPointsSortChanged(const std::string& sort);
  void OnPointsScrolled();
  // ---- the position indicator and the tab reset (mmm/DESIGNSTYLE.md "Long
  // ranked lists"; the math in LeaderboardIndicator.hpp)
  void BuildPointsIndicator();  // the overlay over pane B: track, thumb, the drag label
  void DrawPointsIndicator(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
  // the thumb for the current window and scroll position; the track's
  // extent inside the indicator comes back through the out-params
  urnw::leaderboard::Thumb PointsThumb(double& trackTop, double& trackHeight) const;
  void RefreshPointsPosition();  // the first row in view, from the scroller
  void UpdatePointsIndicator();  // visibility, the slider value, a redraw
  // the pane's scrollbar, aside while either board's indicator shows
  void ApplyPaneBScrollPolicy();
  void OnPointsDragBegin(double x, double y, const Glib::RefPtr<Gtk::GestureDrag>& drag);
  void OnPointsDragUpdate(double dy);
  void OnPointsDragEnd(double dy);
  bool OnPointsIndicatorKey(guint keyval);
  void SeekPoints(int64_t rank);  // a rank in the window scrolls there; any other asks the controller
  void ShowPointsDragLabel(int64_t rank);
  void HidePointsDragLabel(bool fade);
  void ScrollPointsToFirstRow();
  void AnchorPointsScroll(double prependedHeight);
  double PointsRowsTop() const;  // where the first row starts in the scrolled content
  void PrependPointsRows(size_t count);
  Gtk::Widget* MakePointsRow(const PointsRowUi& row, const std::string& ownId);
  void ResetBoardList(bool pointsBoard);  // a board tab activated (the active one included)
  void ApplyPointsBoardSample();          // preview rows for the indicator (URNETWORK_PREVIEW_SAMPLE)
  void OnPointsRetry();
  void OnPointsPublicToggled();
  // The "Show my network name" switch flipped, on the Points block or on the
  // Protocol card: one setting (points_leaderboard_public, Q12), one request.
  void RequestPointsPublic(bool requested);
  void SetPointsToggle(bool on);
  void ApplyPointsPublicResult(uint32_t generation, bool ok, bool requested,
                               const std::string& serverError);
  void OnEditEmoji();
  void SaveEmojiTag(std::string tag, std::function<void(std::string)> done);
  void SettlePointsBoardPreview();

  // ---- loads -----------------------------------------------------------------
  void LoadEarnings();
  void LoadWalletLayer();  // claims, v2 accounts + gas key: only with an attached wallet
  void LoadGas();          // the gas key and its balance
  void LoadLeaderboard();

  // ---- appliers (one writer per surface) -------------------------------------
  void ApplyPoints(std::optional<urnet::AccountPointsList> points, Fetch state);
  void ApplyEpochs(std::optional<std::vector<AccountEpochRow>> epochs, Fetch state);
  void ApplySnWallet(std::optional<SnWalletInfo> wallet, Fetch state);
  // `schedule` is the current epoch's, read with the claims (absent when the
  // SDK could not read the coordinator's policy)
  void ApplyClaims(std::optional<std::vector<SnClaimRow>> claims, int64_t totalClaimableRao,
                   Fetch state, const Glib::ustring& failure = {},
                   std::optional<snpayout::EpochSchedule> schedule = std::nullopt);
  void ApplyGas(std::optional<SnGasInfo> gas);
  // the v2 accounts; `claimConfigured` is whether the chain settings name the
  // v2 vault, read with them
  void ApplyLedger(snledger::Load load, std::vector<snledger::Account> accounts,
                   bool claimConfigured);
  void ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state);
  void ApplyRanking(std::optional<urnet::NetworkRanking> ranking, bool ok);
  void ApplyLeaderboard(std::optional<urnet::LeaderboardEarnersList> earners, Fetch state);

  // ---- rebuilders ------------------------------------------------------------
  void RebuildPointsCard();
  // how and when SN payouts happen, under the points (snpayout::PayoutLineFor)
  void RebuildPayoutLine();
  void RebuildUnclaimedTile();
  void RebuildLedgerTile();  // the Settlement v2 tile (snledger::SectionFor)
  void RebuildWalletBlock();
  void RebuildTop200();
  void RebuildHistory();
  void RebuildLeaderboard();
  void RebuildReliabilityCard();

  // ---- the extender and provider statistics (pane C; EXTENDER.md O5, O8) ----
  // One pull per throughput tick, the shape of ConnectPage::PullThroughput: the
  // two point lists, the window, the provider distribution and the provider
  // stats flag. `forced` marks a re-read right after SdkHost opened a new
  // contract view controller, which asks the device for the provider stats
  // flag rather than the controller, which has not sampled yet.
  void PullProviderThroughput(bool forced);
  // Re-reads the extender status: the read-only row (N7) and the running state
  // of the role, re-applying the sections when it flips.
  void ApplyExtenderProvideState();
  void DrawExtenderRow(const extender::ProvideRow& row);
  // The O8 rule over its three inputs, applied to the two groups' rows.
  void ApplyStatsSections();
  void OpenProviderTransportSheet();

  // ---- the provider status (support part P008) -------------------------------
  // The line under the provide mode row: the local idle reason merged with the
  // server's reason (providerstatus::StatusLineFor), shown while providing is
  // not never. Every input re-applies it: the live stats, the throughput
  // tick's provider bytes and the controller.
  void ApplyStatusLine();
  // Re-reads the SDK's ProviderStatusViewController (SdkHost::ProviderStatusNow)
  // into the demand chart, "Why?" and the line.
  void ApplyProviderStatus();
  // The rows under "Why?", rebuilt only when they change.
  void RebuildWhyRows();
  // "Why?" shows with the plots and a status; its rows while it is expanded.
  void ApplyWhyVisibility();
  void OnWhyToggled();
  void ApplyLedgerMeta();
  void OnLedgerTabChanged();

  // ---- attach a Bittensor wallet ---------------------------------------------
  void OnConnectWithBridge();
  void OnToggleManualEntry();
  void OnChangeWallet();
  void OnWalletAddressChanged();
  void ValidateWalletAddress();
  void ApplyWalletCheck(uint64_t generation, const std::string& address,
                        std::optional<SnWalletCheck> check, const std::string& err);
  void OnConnectManual();
  // Which Bittensor wallet signs on the browser bridge (Talisman or TAO.com
  // through its extension, WalletConnect through the bridge's pairing): `chosen`
  // gets its id; a dismissed chooser chooses nothing. The coldkey connect hands it to
  // StartWalletSignature, the payout consent to its flow.
  void ChooseBittensorWallet(std::function<void(const std::string& walletId)> chosen);
  // The wallet as a signer: `expectedAddress` is the typed address (the wallet
  // must sign with that account) or empty (whichever account the wallet picks).
  void StartWalletSignature(const std::string& walletId, const std::string& expectedAddress);
  // The wallet's step is in the system browser now: the snackbar says where to
  // look.
  void NotifyContinueInBrowser(const std::string& walletId);
  void OnWalletSigned(uint32_t generation, const SdkHost::WalletSignature& signature,
                      const std::string& expectedAddress);
  void SetSnWallet(const std::string& address, const std::string& signature,
                   const std::string& message);
  void ApplyWalletConnectResult(uint32_t generation, bool ok, const std::string& serverError,
                                const std::string& address);
  void FinishConnecting();

  // ---- the network payout consent (NetworkConsent.hpp) -------------------------
  // The section beside the Settlement v2 tile, drawn from the flow's view only.
  void RebuildConsentSection();
  // Sign with wallet and Change wallet: the shared chooser, then the coldkey.
  void OnConsentAction();
  // The coldkey the consent names, typed and checked (ss58) before the flow
  // starts with it and `walletId`.
  void AskConsentColdkey(const std::string& walletId);
  // The flow's ports: the SDK's consent calls and the wallet. Every answer is
  // marshaled to the main loop, dropped once the page's epoch moved, and given
  // up on by consentStep_'s watchdog (one step is in flight at a time).
  consent::Flow::Ports ConsentPorts();

  // ---- the Solana payout wallet (USDC until the Bittensor migration) ---------
  // The three-dot overflow (icon-only, named "Wallet options") over `menu`.
  Gtk::MenuButton* BuildWalletOverflow(const Glib::RefPtr<const Gio::MenuModel>& menu);
  // A plain reload keeps the card while the reads are out; `reset` (after a
  // write) hides it until they land.
  void LoadLegacyWallets(bool reset = false);
  void ApplyLegacyWallets();  // commits a round once all three reads answered
  // the round after a removal committed: name the wallet the server promoted
  void NotifyPromotedPayoutWallet();
  void RebuildSolanaCard();
  void OnConnectSolanaWallet();  // the overflow's item: the connect sheet
  // The sheet linked `walletId`: make it the payout wallet unless a fresh read
  // shows it already is (one 20 s flow over the read and the switch).
  void OnSolanaConnected(const std::string& walletId);
  void SwitchPayoutWallet(const std::string& walletId, uint32_t generation);
  void OnRemoveSolanaWallet();  // the card's item: the confirmation
  void RemoveSolanaWallet(const std::string& walletId);

  // ---- claim -----------------------------------------------------------------
  void OnClaim();
  void OpenClaimSheet(bool allowActions);
  void StartClaim(std::vector<int64_t> epochs);
  // settlement v2: the tile's Claim opens the v2 claim sheet, whose Claim claims
  // every claimable v2 account
  void OnLedgerClaim();
  void StartLedgerClaim();

  // ---- the public-leaderboard switch ----------------------------------------
  void SetRankingToggle(bool on);  // the echo-guarded programmatic write
  void OnLeaderboardPublicToggled();
  void ApplyRankingPublicResult(uint32_t generation, bool ok, bool requested,
                                const std::string& serverError);

  // ---- sheets + links --------------------------------------------------------
  void PresentSheet(const std::shared_ptr<Gtk::Window>& sheet);
  void CloseSheet();
  void OpenLink(const std::string& url);

  // ---- gating + messaging ----------------------------------------------------
  bool CanCallApi();
  void RefuseNoSession();
  void Notify(const Glib::ustring& message, kit::Snackbar::Severity severity);
  void SetStatValue(Gtk::Label* value, const Glib::ustring& key,
                    const Glib::ustring& text, bool loaded);
  void SettleAllEmpty();
  // The provider client this device runs (the daemon's DeviceLocal); "" when no
  // device is bound, which attaches the wallet at the network level.
  std::string ProviderClientId();

  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);  // stale-async guard
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // ---- pane shell ------------------------------------------------------------
  kit::Pane paneA_;  // EARNINGS (360)
  kit::Pane paneB_;  // HISTORY (*)
  kit::Pane paneC_;  // NETWORK (380)
  Gtk::Widget* ruleB_ = nullptr;
  Gtk::Widget* ruleC_ = nullptr;
  int lanes_ = -1;

  // pane A widgets
  Gtk::Label* pointsStatus_ = nullptr;
  Gtk::Box* pointsCard_ = nullptr;
  Gtk::Box* pointsPanel_ = nullptr;
  Gtk::Box* payoutCard_ = nullptr;
  Gtk::Label* payoutText_ = nullptr;
  Gtk::Label* payoutTimes_ = nullptr;
  Gtk::Button* setColdkeyButton_ = nullptr;
  Gtk::Button* payoutClaimButton_ = nullptr;
  Gtk::Box* unclaimedCard_ = nullptr;
  Gtk::Label* unclaimedValue_ = nullptr;
  Gtk::Label* unclaimedStatus_ = nullptr;
  Gtk::Button* claimButton_ = nullptr;
  Gtk::Box* ledgerCard_ = nullptr;  // the Settlement v2 tile
  Gtk::Box* ledgerRows_ = nullptr;
  Gtk::Label* ledgerStatus_ = nullptr;
  Gtk::Button* ledgerClaimButton_ = nullptr;
  // the payout consent section (RebuildConsentSection)
  Gtk::Widget* consentHeader_ = nullptr;
  Gtk::Box* consentCard_ = nullptr;
  Gtk::Label* consentLine_ = nullptr;
  Gtk::Label* consentBody_ = nullptr;
  Gtk::Label* consentNotice_ = nullptr;
  Gtk::Button* consentSignButton_ = nullptr;
  Gtk::Button* consentChangeButton_ = nullptr;
  Gtk::Label* consentChangeNote_ = nullptr;
  Gtk::Label* walletStatus_ = nullptr;
  Gtk::Box* walletConnectedPanel_ = nullptr;
  Gtk::Label* walletAddressLabel_ = nullptr;
  Gtk::Button* changeWalletButton_ = nullptr;
  Gtk::Box* walletConnectPanel_ = nullptr;
  Gtk::Label* walletConnectNote_ = nullptr;
  // provide mode row + gate
  Gtk::Label provideModeDot_;
  Gtk::Label* provideModeValue_ = nullptr;
  Gtk::Button* provideModeRow_ = nullptr;
  bool providingEnabled_ = true;
  std::optional<urnet::ReliabilityWindow> lastReliability_;
  Fetch lastReliabilityState_ = Fetch::Loading;
  Gtk::Button* connectBridgeButton_ = nullptr;
  Gtk::Button* manualToggleButton_ = nullptr;
  Gtk::Box* manualPanel_ = nullptr;
  Gtk::Entry* walletAddressBox_ = nullptr;
  Gtk::Label* walletSupportingText_ = nullptr;
  Gtk::Button* connectManualButton_ = nullptr;
  Gtk::Label* connectingStatus_ = nullptr;
  // the Solana payout wallet: the overflow in both Bittensor states, the one
  // waiting line (a label in each Bittensor panel) and the card
  Gtk::MenuButton* walletMoreDisconnected_ = nullptr;
  Gtk::MenuButton* walletMoreConnected_ = nullptr;
  Gtk::Label* usdcWaitingLine_ = nullptr;           // in the connect panel
  Gtk::Label* usdcWaitingLineConnected_ = nullptr;  // in the connected panel
  Gtk::Box* solanaCard_ = nullptr;
  Gtk::Label* solanaTitle_ = nullptr;
  Gtk::Label* solanaAddressLabel_ = nullptr;
  Gtk::Label* solanaDefaultTag_ = nullptr;
  Gtk::Label* solanaPendingLabel_ = nullptr;
  Gtk::MenuButton* solanaMore_ = nullptr;
  // the two overflow menus over the page's "earnings" action group, built once
  Glib::RefPtr<Gio::SimpleActionGroup> walletActions_;
  Glib::RefPtr<Gio::SimpleAction> connectSolanaAction_;
  Glib::RefPtr<Gio::SimpleAction> removeSolanaAction_;
  Glib::RefPtr<Gio::Menu> connectSolanaMenu_;
  Glib::RefPtr<Gio::Menu> solanaCardMenu_;
  Gtk::Box* top200Card_ = nullptr;
  Gtk::Box* top200Panel_ = nullptr;
  kit::Snackbar walletInfo_;

  // pane B widgets
  Gtk::ToggleButton* historyTab_ = nullptr;
  Gtk::ToggleButton* leaderboardTab_ = nullptr;
  Gtk::Box* historyHost_ = nullptr;
  Gtk::Box* historyPanel_ = nullptr;
  Gtk::Label* historyStatus_ = nullptr;
  Gtk::Box* leaderboardHost_ = nullptr;
  Gtk::Box* leaderboardRows_ = nullptr;
  Gtk::Label* leaderboardStatus_ = nullptr;
  // the points board (pane B)
  Gtk::ToggleButton* protocolBoardTab_ = nullptr;
  Gtk::ToggleButton* dataBoardTab_ = nullptr;
  Gtk::ToggleButton* pointsBoardTab_ = nullptr;
  bool protocolBoardShowing_ = true;  // the Protocol board is the default
  Gtk::Box* leaderboardDataHost_ = nullptr;  // the data board's rows + status
  Gtk::Box* pointsHost_ = nullptr;
  Gtk::ToggleButton* pointsSortTabs_[3] = {nullptr, nullptr, nullptr};  // points, blocks, streak
  Gtk::Box* pointsRows_ = nullptr;
  Gtk::Box* pointsFooter_ = nullptr;
  Gtk::Spinner* pointsFooterSpinner_ = nullptr;
  Gtk::Label* pointsFooterLabel_ = nullptr;
  Gtk::Button* pointsRetryButton_ = nullptr;
  Gtk::Label* pointsBoardStatus_ = nullptr;
  sigc::connection pointsScrollConn_;

  // pane C widgets
  Gtk::Label* netProvidedValue_ = nullptr;
  Gtk::Label* rankValue_ = nullptr;  // the Current Ranking header's meta
  Gtk::Switch* publicToggle_ = nullptr;
  kit::Snackbar leaderboardInfo_;
  // the data board's own-ranking block, hidden while the Points board shows
  std::vector<Gtk::Widget*> dataRankingWidgets_;
  // the points board's block (pane C)
  Gtk::Box* pointsGroup_ = nullptr;
  Gtk::Label* pointsGroupMeta_ = nullptr;
  Gtk::Label* pointsEmojiLabel_ = nullptr;
  Gtk::Label* pointsNameLabel_ = nullptr;
  Gtk::Label* pointsRankedLabel_ = nullptr;
  Gtk::Button* editEmojiButton_ = nullptr;
  PointsStatTile pointsTiles_[3];
  Gtk::Label* pointsLongestLabel_ = nullptr;
  Gtk::Switch* pointsPublicToggle_ = nullptr;
  Gtk::Widget* pointsPrivateHintRow_ = nullptr;
  Gtk::Label* reliabilityStatus_ = nullptr;
  Gtk::Box* reliabilityCard_ = nullptr;
  Gtk::Box* reliabilityPanel_ = nullptr;
  // the extender statistics group (O4, O8): its header and chart row show only
  // with the provider statistics and a running role
  kit::PaneGroupHeader extenderStatsHeader_;
  Gtk::Box* extenderChartRow_ = nullptr;
  TransferChart* extenderChart_ = nullptr;
  // the provider statistics group (O5, O8): the header's meta says
  // providing_disabled while the chart rows are collapsed; the provide mode
  // row and the read-only extender row stay
  kit::PaneGroupHeader providerStatsHeader_;
  Gtk::Button* extenderRow_ = nullptr;
  Gtk::Label* extenderDot_ = nullptr;
  Gtk::Label* extenderState_ = nullptr;
  Gtk::Box* localChartRow_ = nullptr;
  TransferChart* localChart_ = nullptr;
  Gtk::Box* providerTransportRow_ = nullptr;
  TransportBar* providerTransportBar_ = nullptr;
  Gtk::Box* blockedChartRow_ = nullptr;
  TransferChart* blockedChart_ = nullptr;
  // the provider status (P008): the line under the provide mode row with its
  // Change, and after the plots, under their gate, the demand chart and "Why?"
  Gtk::Box* statusLineRow_ = nullptr;
  Gtk::Label* statusLineText_ = nullptr;
  Gtk::Box* demandChartRow_ = nullptr;
  DemandChart* demandChart_ = nullptr;
  Gtk::Button* whyRow_ = nullptr;
  Gtk::Image* whyChevron_ = nullptr;
  Gtk::Box* whyRows_ = nullptr;
  bool whyExpanded_ = false;  // collapsed by default
  std::unique_ptr<TransportSheet> providerTransportSheet_;
  // the reading the extender row last drew, so a push that changes nothing is
  // dropped
  extender::ProvideRow extenderRowDrawn_;
  bool extenderRowApplied_ = false;
  // the rule's inputs beside providingEnabled_, and the sections last applied
  bool extenderRunning_ = false;
  bool hasProviderStats_ = false;
  bool providerDistributionKnown_ = false;
  extender::StatsSections statsSections_;
  bool statsSectionsApplied_ = false;
  // the provider status's inputs: the control mode the row shows, the live
  // provide state, the provider bytes of the throughput window (nullopt while
  // no window has been read) and the controller's last reading, with the view
  // and the "Why?" rows last drawn
  std::string controlMode_;
  int64_t liveProvideMode_ = 0;
  bool providePaused_ = false;
  std::optional<int64_t> recentProviderBytes_;
  providerstatus::Poll providerStatus_;
  providerstatus::StatusView providerStatusView_;
  std::vector<providerstatus::NumberRow> whyRowsDrawn_;
  bool whyRowsApplied_ = false;

  // ---- state -----------------------------------------------------------------
  urnet::AccountPointsList points_;
  Fetch pointsState_ = Fetch::Loading;
  std::vector<AccountEpochRow> epochs_;
  Fetch epochsState_ = Fetch::Loading;
  std::optional<SnWalletInfo> wallet_;
  Fetch walletState_ = Fetch::Loading;
  std::vector<SnClaimRow> claims_;
  int64_t totalClaimableRao_ = 0;
  Fetch claimsState_ = Fetch::Loading;
  Glib::ustring claimsFailure_;
  std::optional<snpayout::EpochSchedule> schedule_;
  std::vector<snledger::Account> ledgerAccounts_;
  snledger::Load ledgerLoad_ = snledger::Load::Loading;
  bool ledgerConfigured_ = false;  // the chain settings name the v2 vault: v2 Claim shows
  std::optional<SnGasInfo> gas_;
  std::optional<urnet::ReliabilityWindow> reliability_;
  Fetch reliabilityState_ = Fetch::Loading;
  urnet::LeaderboardEarnersList leaderboard_;
  Fetch leaderboardState_ = Fetch::Loading;
  int leaderboardCount_ = 0;
  std::string ownNetworkId_;
  bool rankingPublic_ = false;
  // the points board's mirror of its controller
  std::optional<urnet::PointsLeaderboardViewController> pointsVc_;
  std::optional<urnet::Sub> pointsSub_;
  uint64_t pointsVcDevice_ = 0;  // the device handle the controller was opened on
  bool pointsBoardShowing_ = false;
  std::vector<PointsRowUi> pointsRowsUi_;
  std::string pointsSort_ = urnet::PointsLeaderboardSortPoints;
  std::string pointsRenderedSort_;
  std::string pointsRenderedOwnId_;  // the own id the rows were drawn with
  size_t pointsRenderedCount_ = 0;   // rows drawn; the next page appends after them
  bool pointsLoading_ = false;
  bool pointsEnd_ = false;
  bool pointsHasLoaded_ = false;  // the first page landed (rows, an empty end, or an error)
  std::string pointsError_;
  int64_t pointsTotalRanked_ = 0;
  int64_t pointsFirstPosition_ = 1;  // the loaded window's first position
  bool pointsHasMoreBefore_ = false;  // rows exist above the window (after a seek)
  int64_t pointsFirstVisible_ = 1;    // the first row in view's position
  // the draggable position indicator: a slider over ranks 1..N floating at
  // the pane's right edge, with the rank and tier beside the thumb while dragging
  Gtk::Overlay* paneBOverlay_ = nullptr;
  Gtk::DrawingArea* pointsIndicator_ = nullptr;
  Gtk::Box* pointsIndicatorLabel_ = nullptr;
  Gtk::Label* pointsIndicatorRank_ = nullptr;
  Gtk::Label* pointsIndicatorTier_ = nullptr;
  bool pointsDragging_ = false;
  double pointsDragStartTop_ = 0;  // the thumb's top when the drag began
  double pointsDragTop_ = 0;       // the thumb's top while dragging
  int64_t pointsDragRank_ = 0;
  uint64_t pointsLabelGen_ = 0;     // cancels a fade the next show overtakes
  bool pointsSeekPending_ = false;  // a seek's window is on its way: scroll to its first row when it lands
  sigc::connection pointsAnchorConn_;
  std::optional<PointsRowUi> pointsMe_;
  bool pointsPublic_ = false;  // this network's opt-in, from `me`, updated locally on toggle
  std::string emojiTag_;       // this network's tag, from `me`, updated locally on save
  bool settingPointsPublic_ = false;
  bool applyingPointsToggle_ = false;  // ECHO GUARD on the opt-in switch
  bool savingEmojiTag_ = false;
  // after a local toggle or save, `me` from an older in-flight page could
  // briefly disagree with what the user just did; the local values win until
  // a response newer than the edit lands
  uint64_t ownFlagsClock_ = 0;
  uint64_t ownFlagsEditedAt_ = 0;
  uint64_t ownFlagsAppliedAt_ = 0;
  // the Solana payout wallet: the reads the card is drawn from, and the round
  // of reads in flight (SolanaWalletPresentation.hpp LegacyLoad)
  solana::LegacyCommitted legacyCommitted_;
  solana::LegacyLoad legacyLoad_;

  // in-flight gates
  bool connecting_ = false;      // bridge / set-wallet in flight
  // the wallet the current coldkey connect signs with (its refusal names it)
  bool changingWallet_ = false;  // "Change" opened the connect affordances
  bool manualEntryOpen_ = false;
  // a v1 or a v2 claim in flight: the gas key sends one claim at a time
  bool claiming_ = false;
  bool settingRankingPublic_ = false;
  bool applyingRankingToggle_ = false;  // ECHO GUARD on the public switch
  bool leaderboardRequested_ = false;
  bool removingSolanaWallet_ = false;
  // a removal that landed, until the round of reads after it commits
  std::optional<solana::PayoutRemoval> payoutRemoval_;
  bool switchingPayoutWallet_ = false;  // the fresh read and the switch after a link

  // manual entry validation: the verdict for the address in the box
  std::string checkedAddress_;
  std::optional<SnWalletCheck> check_;
  bool checkInFlight_ = false;
  uint64_t checkGeneration_ = 0;  // bare counter: no timer, no watchdog
  sigc::connection checkDebounce_;

  Flow connectFlow_;   // 330s: the challenge's life, which signing by hand can take all of
  Flow setWalletFlow_; // 20s: POST /sn/wallet
  Flow pointsPublicFlow_;  // 20s: POST /network/points-ranking-visibility
  Flow claimFlow_;     // 180s: chain round trips
  Flow ledgerClaimFlow_;  // 3 min + 6 per account: the v2 claim, past the SDK's bounds
  Flow rankingFlow_;   // 20s
  Flow legacyFlow_;    // 20s: the payout read and POST /account/payout-wallet after a link
  Flow removeSolanaFlow_;  // 20s: POST /account/wallets/remove
  Flow consentStep_;  // the consent's step: the wallet's minutes, 20s for a call

  // the payout consent (NetworkConsent.hpp); its ports reach host_ and epoch_
  consent::Flow consent_;

  // preview + balance relay
  bool previewMode_ = false;
  bool samplePinned_ = false;
  bool isPro_ = false;
  bool isGuest_ = false;

  std::shared_ptr<Gtk::Window> sheet_;
  std::weak_ptr<ClaimAlphaSheet> claimSheet_;
  std::weak_ptr<LedgerClaimSheet> ledgerClaimSheet_;
  std::weak_ptr<Top200SignSheet> top200SignSheet_;

  // the Protocol board: its list in pane B, its own block in pane C (or on top
  // of its list), its own place behind the Top 200 tile; created before the
  // panes that hold its halves
  std::unique_ptr<ProtocolBoard> protocol_;
  Gtk::Button* top200ClaimButton_ = nullptr;  // the tile's claim button, while it shows
};

}  // namespace urnw
