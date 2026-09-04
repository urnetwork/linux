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
// is attached; earlier epochs earned points only. There is no USDC, no payout
// wallet and no payout history anywhere on this surface (support@ur.io holds
// the old ledger).
//
// Three panes, folded by the window's ApplyBreakpoint:
//   >= 1500  earnings(360) | history(*) | network(380)
//   >=  900  earnings(360) | history(*)
//   <   900  history(*) only — the HISTORY survives to the smallest width.
//
// Pane A (earnings): the points headline with its Providing / Referral /
// Reliability breakdown (and the Seeker multiplier, points-only), the protocol
// note with the ur.xyz link, the Unclaimed SN25α tile
// (wallet only), the Bittensor wallet block (connect through the bridge, or
// enter an address manually — validated locally and then against
// /sn/wallet/validate before anything is sent — and still sign it), and the
// Top 200 head-spot tile / bound status.
//
// Pane B (history): a two-item tab switch over the per-epoch history and the
// leaderboard. The leaderboard is fetched the FIRST time its tab is looked at.
//
// Pane C (network): own ranking, the public-leaderboard switch (echo-guarded)
// and the reliability window.
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

#include "PaneKit.hpp"
#include "SdkHost.hpp"
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

struct SnHeadInfo {
  bool eligible = false;
  double score = 0;
  double floor = 0;
  int64_t rankEstimate = 0;
  int64_t cutoff = 200;
  bool bound = false;
  std::string hotkey;
  int64_t uid = 0;
  int64_t rank = 0;
  int64_t epoch = 0;
  std::string source;
};

struct SnWalletCheck {
  bool validSyntax = false;
  bool existsOnChain = true;
  bool banned = false;
  std::string message;
};

class ClaimAlphaSheet;
class EmojiTagSheet;

class EarningsPage : public Gtk::Box {
 public:
  explicit EarningsPage(SdkHost& host);
  ~EarningsPage() override;

  // nav-select + auth-change: the independent earnings-pane fetches. Bumps the
  // stale-async epoch first, so every completion armed for the previous
  // session is dropped before it touches a widget.
  void Load();

  // The provide-mode row (the connect page's indicator + label with the
  // current mode) and the providing gate: with providing off the reliability
  // chart hides and the group says so, the same gate and message as the stats
  // widget. Fed from the same live stats the connect page paints.
  void ApplyProvideState(const LiveStats& stats);
  // the provide mode is changed on the connect page; the row opens it there
  std::function<void()> on_open_provide_settings;

  // The points board's row and stat tile (public: a free helper in the .cpp builds rows).
  struct PointsRowUi {
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
      return networkId == o.networkId && displayName == o.displayName &&
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
  // attached-wallet layer (claims, gas key, alpha column);
  // URNETWORK_PREVIEW_TOP200=bound renders the bound head-spot status;
  // URNETWORK_PREVIEW_MANUAL=1 opens the manual entry on a "new wallet" verdict;
  // URNETWORK_PREVIEW_GAS=low puts the claim dialog in its needs-gas state.
  void ApplyPreviewSample();
  // URNETWORK_PREVIEW_CLAIM=1: open the claim dialog over the sample.
  void ShowPreviewClaimDialog();

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

  // ---- the points board (android/POINTSLEADERBOARD.md) ----------------------
  // The leaderboard is two boards behind one switch: Data (the last-4-payments
  // board) and Points (the all-time points board). The Points board is the
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
  // Mirrors the controller into the page: rows (value-compared, so a no-op
  // event does not re-render the table), sort, loading, end, error, `me`.
  void ReadPointsBoard();
  void RebuildPointsRows(size_t fromIndex = 0);
  std::string OwnPointsName();
  void RenderPointsHeader();
  void RenderPointsFooter();
  void OnPointsSortChanged(const std::string& sort);
  void OnPointsScrolled();
  void OnPointsRetry();
  void OnPointsPublicToggled();
  void SetPointsToggle(bool on);
  void ApplyPointsPublicResult(uint32_t generation, bool ok, bool requested,
                               const std::string& serverError);
  void OnEditEmoji();
  void SaveEmojiTag(std::string tag, std::function<void(std::string)> done);
  void SettlePointsBoardPreview();

  // ---- loads -----------------------------------------------------------------
  void LoadEarnings();
  void LoadWalletLayer();  // claims + gas key: only with an attached wallet
  void LoadLeaderboard();

  // ---- appliers (one writer per surface) -------------------------------------
  void ApplyPoints(std::optional<urnet::AccountPointsList> points, Fetch state);
  void ApplyEpochs(std::optional<std::vector<AccountEpochRow>> epochs, Fetch state);
  void ApplySnWallet(std::optional<SnWalletInfo> wallet, Fetch state);
  void ApplyClaims(std::optional<std::vector<SnClaimRow>> claims, int64_t totalClaimableRao,
                   Fetch state, const Glib::ustring& failure = {});
  void ApplyGas(std::optional<SnGasInfo> gas);
  void ApplyHead(std::optional<SnHeadInfo> head, Fetch state);
  void ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state);
  void ApplyRanking(std::optional<urnet::NetworkRanking> ranking, bool ok);
  void ApplyLeaderboard(std::optional<urnet::LeaderboardEarnersList> earners, Fetch state);

  // ---- rebuilders ------------------------------------------------------------
  void RebuildPointsCard();
  void RebuildUnclaimedTile();
  void RebuildWalletBlock();
  void RebuildTop200();
  void RebuildHistory();
  void RebuildLeaderboard();
  void RebuildReliabilityCard();
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
  // The bridge as a signer: `expectedAddress` is the typed address (the bridge
  // must sign with that wallet) or empty (whichever wallet the bridge picks).
  void StartWalletSignature(const std::string& expectedAddress);
  void OnWalletSigned(uint32_t generation, const SdkHost::WalletSignature& signature,
                      const std::string& expectedAddress);
  void SetSnWallet(const std::string& address, const std::string& signature,
                   const std::string& message);
  void ApplyWalletConnectResult(uint32_t generation, bool ok, const std::string& serverError,
                                const std::string& address);
  void FinishConnecting();

  // ---- claim -----------------------------------------------------------------
  void OnClaim();
  void OpenClaimSheet(bool allowActions);
  void StartClaim(std::vector<int64_t> epochs);

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
  Gtk::Box* unclaimedCard_ = nullptr;
  Gtk::Label* unclaimedValue_ = nullptr;
  Gtk::Label* unclaimedStatus_ = nullptr;
  Gtk::Button* claimButton_ = nullptr;
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
  Gtk::ToggleButton* dataBoardTab_ = nullptr;
  Gtk::ToggleButton* pointsBoardTab_ = nullptr;
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
  Gtk::Label* rankValue_ = nullptr;
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
  std::optional<SnGasInfo> gas_;
  std::optional<SnHeadInfo> head_;
  Fetch headState_ = Fetch::Loading;
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

  // in-flight gates
  bool connecting_ = false;      // bridge / set-wallet in flight
  bool changingWallet_ = false;  // "Change" opened the connect affordances
  bool manualEntryOpen_ = false;
  bool claiming_ = false;
  bool settingRankingPublic_ = false;
  bool applyingRankingToggle_ = false;  // ECHO GUARD on the public switch
  bool leaderboardRequested_ = false;

  // manual entry validation: the verdict for the address in the box
  std::string checkedAddress_;
  std::optional<SnWalletCheck> check_;
  bool checkInFlight_ = false;
  uint64_t checkGeneration_ = 0;  // bare counter: no timer, no watchdog
  sigc::connection checkDebounce_;

  Flow connectFlow_;   // 180s: the user legitimately spends minutes in a browser
  Flow setWalletFlow_; // 20s: POST /sn/wallet
  Flow pointsPublicFlow_;  // 20s: POST /network/points-ranking-visibility
  Flow claimFlow_;     // 180s: chain round trips
  Flow rankingFlow_;   // 20s

  // preview + balance relay
  bool previewMode_ = false;
  bool samplePinned_ = false;
  bool isPro_ = false;
  bool isGuest_ = false;

  std::shared_ptr<Gtk::Window> sheet_;
  std::weak_ptr<ClaimAlphaSheet> claimSheet_;
};

}  // namespace urnw
