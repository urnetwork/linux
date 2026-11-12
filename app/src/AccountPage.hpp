// AccountPage — the ACCOUNT destination of the Windows-parity shell (windows
// AccountPage.{h,cpp} + the account-subject sections SettingsPage.cpp builds
// into Account's host panels + BalanceSheets/SettingsSheets; spec
// docs/parity/account.md). "Who you are, in three columns":
//
//   Pane A "Plan" (360)  — what the network HAS: the plan value (Pro wears
//     gold, and nothing else in the app may), the used/pending/available usage
//     bar and its legend, the upgrade/create-account action, the daily balance
//     and referral figures, Redeem Balance Code, Manage Subscription.
//   Pane B "Account" (*) — who the network IS: the profile group (network-name
//     view row + edit panel, the auth line, the status line, update password,
//     the Sessions row that opens the Sessions page and the API keys row that
//     opens the API keys page; every row of this pane leads with an icon),
//     the security group (login methods + add + auth code + client id), the
//     referral group (bonus code, referral network, summary, royalty badge)
//     and the danger group (sign out, delete account).
//   Pane C "Balance Codes" (380) — the redeemed-code record. It folds first:
//     nothing in it is actionable and Redeem lives in the plan pane.
//
// FIVE INVARIANTS THIS PAGE IS BUILT AROUND (spec §7):
//
//  1. ONE WRITER PER SURFACE. ApplyBalance paints pane A's plan/usage/referral
//     figures, RenderBalanceCodes paints pane C, ApplyAccountState gates every
//     profile control in pane B. Scattered writers used to disagree.
//  2. The session gate is IsLoggedIn(), never "the api exists" — an
//     unauthenticated request's 401 renders as an EMPTY account, which is the
//     lie this page exists to avoid. Every async field terminates in exactly
//     one of six states (AccountFieldState) and all six are textually
//     distinct; NoDevice exists because "signed in, service not up" must not
//     say "please login".
//  3. Results with NO error field (password reset, unlink, network delete,
//     redeemed codes) are successful ONLY when a result arrived AND the
//     transport error is empty. getReferralNetwork inverts the usual rule: a
//     structured error IS a valid "no referral network" answer, and only a
//     transport failure is Failed.
//  4. Destructive confirmations never commit on Enter: the default control is
//     Cancel/Close, the destructive word appears only on the committing
//     control, and red belongs to the confirmation context — not to the row.
//  5. Copy affordances copy the FULL value; the masked or abbreviated display
//     never reaches the clipboard.
//
// Fold table (spec §1.1): >= 1500 three panes; 900..1499 plan | account;
// < 900 the ACCOUNT pane alone (360 beside ~340 is two unreadable halves, and
// the usage figures also live on the status strip and the tray).
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

#include "ExtenderImportSheet.hpp"
#include "ExtenderSection.hpp"
#include "ExtenderShareSheet.hpp"
#include "PaneKit.hpp"
#include "ReferralTotalsState.hpp"
#include "SdkHost.hpp"
#include "Ui.hpp"

namespace urnw {

// The usage bar the plan pane draws (file-local to AccountPage.cpp; see the
// TODO(kit) there — the tree's UsageBar bundles the bar WITH the daily and
// referral rows, which this destination renders as its own pane rows).
class AccountUsageBar;
// Sheets this destination owns, built file-local in AccountPage.cpp because
// the spec defines them completely and the tree has none of them yet.
class AccountAddAuthSheet;
class AccountAuthCodeSheet;
class AccountDeleteSheet;

// §0.4 — the terminal states of EVERY async field on this destination and its
// sheets. NoDevice is not a nicety: the client id comes off the DEVICE, so
// "signed in but the daemon is not attached" must say so rather than tell a
// signed-in user to log in.
enum class AccountFieldState { NoSession, NoDevice, Loading, Loaded, Empty, Failed };

// One non-repeating watchdog + a generation — the give-up this destination's
// every request needs (CONTRACT rule 3: 20 s for a plain api call). This SDK
// is known to drop callbacks, and EarningsPage carries the same pattern for
// the same reason: without it a load parks its panel on "Loading..." forever
// and a mutation leaves its button disabled for the rest of the process, which
// is exactly the hang §0.4's six terminal states exist to prevent.
//
// Begin() disarms whatever was armed, bumps the generation and arms the timer;
// the timeout handler bumps AGAIN so the give-up is FINAL — a late answer must
// never undo what the user has already been told. Settle() returns false when
// the answer was superseded or already given up on, and the caller must then
// do NOTHING. Abandon() drops an in-flight request without a verdict (the
// session changed under it; the caller is repainting anyway).
//
// Declared at namespace scope because the four sheets this destination owns
// (file-local classes in AccountPage.cpp) need the same watchdog as the page.
struct AccountFlow {
  uint32_t generation = 0;
  sigc::connection timer;
  ~AccountFlow() { timer.disconnect(); }
  uint32_t Begin(int timeoutMs, std::function<void()> onTimeout);
  bool Settle(uint32_t armed, const char* what);
  void Abandon();
};

// §2.3 — the balance snapshot the window relays from SubscriptionBalanceStore.
// The page deliberately does NOT own the store: the store is a window-level
// singleton feeding the warning bar and the upgrade sheet too, and
// the Account page is one more subscriber to its publish.
struct AccountBalance {
  int64_t usedByteCount = 0;
  int64_t pendingByteCount = 0;
  int64_t availableByteCount = 0;
  int64_t startBalanceByteCount = 0;
  bool isPro = false;
  // ShownPlan and PlanReadFailed: the plan value (account_plan::PlanViewFor),
  // never Free before the server says so; "unknown" until a relay lands
  std::string shownPlan = "unknown";
  bool planReadFailed = false;
  bool guest = false;
  // SubscriptionStoreFamily: Manage Subscription shows only for "stripe", and
  // "crypto" (a one-time crypto year) reads "does not renew"
  std::string subscriptionStoreFamily;
  // A one-time crypto year's renewal notice is due (CryptoTermNoticeDue), and
  // the local date its grace ends, which the notice names.
  bool cryptoTermNotice = false;
  std::string cryptoTermRenewBy;
  // The store is retrying a renewal whose payment did not go through
  // (PaymentPastDue): the past-due notice with "Update payment method".
  bool paymentPastDue = false;
  bool loaded = false;      // HasFetched: a snapshot has landed at least once
  bool confirming = false;  // IsPolling: the post-checkout confirmation poll
  bool timedOut = false;    // PurchaseConfirmationTimedOut (relayed for parity)
};

class AccountPage : public Gtk::Box {
 public:
  explicit AccountPage(SdkHost& host);
  ~AccountPage() override;

  // nav-select + auth-change API loads (windows AccountPage::LoadAccount +
  // the SettingsPage-owned loads that render on THIS destination). Bumps the
  // stale-async epoch first, so every completion armed for the previous
  // session is dropped before it touches a widget. With no session every load
  // settles its own panel on NoSession WITHOUT a request — nothing is left on
  // a spinner, and no 401 can arrive to be mistaken for an empty account.
  void Load();

  // §5.1 — the redeem path's pane-C refresh, and the ONE writer of pane C's
  // load. RedeemCodeSheet is window-owned (it needs the balance store), so the
  // window fires this on a successful redeem: the code the user just redeemed
  // has to appear in the record without a full Load() re-issuing all four
  // round trips and repainting the profile status line as a side effect.
  void LoadBalanceCodes();

  // §3.2.5 — the client id comes off the DEVICE, and the device attaches on a
  // DIFFERENT event from the auth change that drives Load(). The window calls
  // this on DrawerEvent::DeviceLifecycle so "Attaching device controls…" is a
  // state the row LEAVES rather than one it is stuck in until the user
  // navigates away and back. The Extenders section rides the same event for
  // the same reason: its view controller lives only while a device does.
  void RefreshClientId();

  // The spec's pane-fold table (window width in dip): 1500 / 900.
  void ApplyBreakpoint(int widthDip);

  // The window's balance relay (§2.3). THE only painter of the plan value and
  // gold, the upgrade/create visibility, the confirmation ring, the usage bar
  // and the two referral lines.
  void ApplyBalance(const AccountBalance& snapshot);

  // §1 — sign-out wipes ALL per-account state. userAuth_ is the dangerous one:
  // password reset mails a link to it, so a leftover value would mail the
  // PREVIOUS owner.
  void ResetForSignOut();

  // ---- preview harness (--preview-ui) ---------------------------------------
  // Preview runs with NO jwt and NO device: SetPreviewMode makes every server
  // question decline, and ShowPreviewState settles every panel on the real
  // state it would show — a permanent "Loading..." is indistinguishable from a
  // hang.
  void SetPreviewMode(bool on);
  void ShowPreviewState();

  // ---- window-level routing --------------------------------------------------
  // The plan pane's primary action. Guests get the sign-out-and-create-account
  // offer, everyone else the UpgradeSheet the window/drawer owns. A crypto
  // year's notice offers Renew, which opens the same sheet.
  std::function<void()> on_open_upgrade;
  // "Payment in progress": the window opens the saved crypto invoice
  // (UpgradeSheet::OpenInvoice).
  std::function<void()> on_open_crypto_invoice;
  // The signed-in network's crypto invoice, relayed by the window
  // (CryptoInvoiceHost): an unpaid one shows the "Payment in progress" row,
  // with the network it is paid on under it.
  void ApplyCryptoInvoice(bool inProgress, const Glib::ustring& network);
  // The data-usage group's info button: the "About your data" sheet the
  // window owns (it needs the balance store).
  std::function<void()> on_open_data_info;
  // A Pro network's plan label ("Pro") replays the Pro celebration (android
  // AccountRootSubscription onPlanLabelTap); free and guest labels are inert.
  std::function<void()> on_plan_label_tap;
  // The plan value's Try again, after a balance read failed with no answer
  // since: the window reads the balance again (the store it owns).
  std::function<void()> on_retry_plan;
  // The Redeem row: the existing RedeemCodeSheet (linux-reuse §2.15), which
  // needs the balance store the window owns.
  std::function<void()> on_open_redeem;
  // the Referrals row: the window navigates to the Refer and earn page
  std::function<void()> on_open_referrals;
  // the Sessions row: the window navigates to the Sessions page
  std::function<void()> on_open_sessions;
  // the API keys row: the window navigates to the API keys page
  std::function<void()> on_open_api_keys;
  // Back up first, from the sign-out and account-deletion warnings: the window
  // opens Earnings on the one-tap key's Key screen
  std::function<void()> on_back_up_bittensor_key;
  // The window's one-modal-at-a-time gate (§0.8): every sheet path asks first
  // and reports both edges.
  std::function<bool()> sheet_open;
  std::function<void(bool open)> on_sheet_open_changed;
  // Window-level snackbar surface. error=true is the PERSISTENT treatment
  // (Warning/Error stay until dismissed — an error string is often the only
  // diagnostic the user gets).
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;

 private:
  // ---- construction ----------------------------------------------------------
  void BuildPlanPane();
  void BuildAccountPane();
  void BuildCodesPane();
  void BuildProfileGroup(Gtk::Box& host);
  void BuildSecurityGroup(Gtk::Box& host);
  void BuildReferralsRow(Gtk::Box& host);
  void BuildExtendersGroup(Gtk::Box& host);
  void BuildDangerGroup(Gtk::Box& host);

  // ---- loads -----------------------------------------------------------------
  void LoadAccount();          // getNetworkUser: name, auth line, login methods
  void LoadReferralInfo();     // getNetworkReferralCode: the Referrals row + pane A
  void RetryReferralInfo();    // pane A's Try again: back to Loading, read again
  void ApplyClientId();        // DEVICE read, no round trip

  // ---- appliers (one writer per surface) -------------------------------------
  void ApplyAccountState(AccountFieldState state);
  void ApplyNetworkName(const std::string& name);  // the acknowledged name
  void ApplyAuthLine();
  void ApplyReferralsRow(AccountFieldState state);
  void RenderAuthMethods();
  void RenderBalanceCodes();
  void SettleNoSession();  // every panel on its real no-session state

  // ---- name edit -------------------------------------------------------------
  void OpenNameEditor();
  void CloseNameEditor();
  void OnSaveNetworkName();
  void FinishNameSave(bool ok, const std::string& acceptedName,
                      const std::string& message);

  // ---- mutations -------------------------------------------------------------
  void SendPasswordReset();
  void ApplyResetRateLimit();
  void OpenCustomerPortal();
  // The past-due notice's "Update payment method": a one-time auth code, then
  // ur.io's Manage Subscription signed in with it (past_due::UpdatePaymentUrl).
  void OpenUpdatePayment();
  // Opens the page in the browser with `authCode`, or without a code when it
  // is empty (it could not be created), and re-enables the action.
  void FinishUpdatePayment(const std::string& authCode);
  void ConfirmRemoveAuth(const std::string& authType, const Glib::ustring& label);
  void RemoveAuth(const std::string& authType);

  // ---- sheets ----------------------------------------------------------------
  Gtk::Window* RootWindow();
  bool BeginSheet(const char* what);  // the one-modal-at-a-time gate
  void EndSheet();
  void WireSheet(Gtk::Window& sheet);  // report the close edge exactly once
  void ShowAddAuthSheet();
  void ShowAuthCodeSheet();
  void ShowDeleteAccountSheet();
  // Sign out, with the warning while the network's one-tap key isn't backed up
  void RequestSignOut();
  void ShowKeySignOutWarning();
  void ShowExtenderShareSheet();
  void ShowExtenderImportSheet();
  // The Extenders section's Reset extenders, confirmed in the page's modal
  // before the section runs it.
  void ConfirmResetExtenders();

  // ---- helpers ---------------------------------------------------------------
  // !previewUi && IsLoggedIn(): the gate on every server question AND every
  // server write. apiReady() is deliberately NOT part of it (it is true from
  // SDK init, long before a login).
  bool CanCallApi();
  void Snack(const Glib::ustring& message, bool error);
  void CopyFull(const std::string& value, const Glib::ustring& confirmation);
  // Release every in-flight flag and disarm every mutation watchdog. Called by
  // the paths that bump the epoch (Load, sign-out): a completion the epoch
  // guard drops is STILL a completion for flag purposes — the guard's job is
  // to stop a stale write to a widget, not to strand the page's own state
  // machine. Without this, one nav-select landing between a Send and its
  // answer disables that Send for the rest of the process, and neither a
  // sign-out nor a sign-in clears it.
  void ReleaseInFlight();
  // Sign-out dismisses every modal this page owns. An open sheet is a SURFACE:
  // §1 wipes all per-account state, and a sheet left up keeps the previous
  // account's referral network, freshly minted auth code or network name on
  // screen with its own gate still lit.
  void CloseSheets();

  SdkHost& host_;
  // stale-async guard: bumped by Load(), ResetForSignOut() and the destructor;
  // a completion carrying an older value is dropped before it touches the page
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  // liveness token for marshaled work that must survive a Load() (the sheet
  // close edge): the epoch answers "is this stale", this answers "am I alive"
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // ---- the pane shell --------------------------------------------------------
  kit::Pane paneA_;  // PLAN (360)
  kit::Pane paneB_;  // ACCOUNT (*)
  kit::Pane paneC_;  // BALANCE CODES (380)
  Gtk::Widget* ruleB_ = nullptr;
  Gtk::Widget* ruleC_ = nullptr;
  int lanes_ = -1;  // last applied fold (3 / 2 / 1); -1 = never applied

  // ---- pane A ----------------------------------------------------------------
  Gtk::Spinner* planRing_ = nullptr;  // active + visible ONLY while confirming
  Gtk::Label* planValue_ = nullptr;
  Gtk::Button* planRetry_ = nullptr;  // shown only after a failed balance read
  AccountUsageBar* usageBar_ = nullptr;
  Gtk::Button* upgradeButton_ = nullptr;
  // a renewal whose payment did not go through: the past-due notice and its
  // "Update payment method" (disabled while its auth code is in flight)
  Gtk::Widget* paymentPastDueRow_ = nullptr;
  Gtk::Button* updatePaymentButton_ = nullptr;
  // a one-time crypto year: the renewal notice with Renew, or "does not renew"
  Gtk::Widget* cryptoTermRow_ = nullptr;
  Gtk::Label* cryptoTermLine_ = nullptr;
  Gtk::Widget* doesNotRenewRow_ = nullptr;
  // the saved crypto invoice that is not paid yet
  Gtk::Button* paymentInProgressRow_ = nullptr;
  Gtk::Label* paymentInProgressNote_ = nullptr;
  Gtk::Label* dailyValue_ = nullptr;
  // the usage bar's three figures, which its legend only colours
  Gtk::Label* usedValue_ = nullptr;
  Gtk::Label* pendingValue_ = nullptr;
  Gtk::Label* availableValue_ = nullptr;
  Gtk::Label* referralTotals_ = nullptr;
  Gtk::Label* referralBonus_ = nullptr;
  Gtk::Button* referralRetry_ = nullptr;  // shown only on a failed read
  Gtk::Label* referralDetail_ = nullptr;  // the program's rule, from the terms
  Gtk::Widget* codesCountRow_ = nullptr;  // hidden while no fetch has answered
  Gtk::Label* codesCountValue_ = nullptr;
  Gtk::Button* portalRow_ = nullptr;  // Manage Subscription (disabled in flight)

  // ---- pane B: profile -------------------------------------------------------
  kit::PaneTwoLineRowButton nameRow_;
  Gtk::Widget* nameEditPanel_ = nullptr;
  Gtk::Entry* nameBox_ = nullptr;
  Gtk::Button* saveNameButton_ = nullptr;
  Gtk::Widget* authRow_ = nullptr;  // collapsed whenever its text is empty
  Gtk::Label* authText_ = nullptr;
  Gtk::Label* statusLine_ = nullptr;  // load state / save verdict / reset verdict
  Gtk::Button* sendResetButton_ = nullptr;

  // ---- pane B: security ------------------------------------------------------
  Gtk::Box* authMethodsPanel_ = nullptr;
  Gtk::Label* clientIdValue_ = nullptr;
  Gtk::Button* clientIdCopy_ = nullptr;

  // ---- pane B: referrals -----------------------------------------------------
  kit::PaneTwoLineRowButton referralsRow_;  // opens the Refer and earn page

  // ---- pane B: extenders (EXTENDER.md K6) -------------------------------------
  ExtenderSection* extenderSection_ = nullptr;

  // ---- pane C ----------------------------------------------------------------
  Gtk::Box* codesPanel_ = nullptr;
  Gtk::Label* codesEmpty_ = nullptr;

  // ---- state -----------------------------------------------------------------
  AccountBalance balance_;
  AccountFieldState accountState_ = AccountFieldState::NoSession;
  AccountFieldState methodsState_ = AccountFieldState::NoSession;
  AccountFieldState codesState_ = AccountFieldState::NoSession;
  std::string userAuth_;          // NEVER survives a sign-out (see ResetForSignOut)
  // where Update password mails its reset link: a password sign-in's address,
  // which an account that added an email later has though it has no
  // userAuth_, else empty (PasswordSignIn.hpp). Same lifetime.
  std::string passwordResetUserAuth_;
  bool userAuthVerified_ = false;
  bool needsNameClaim_ = false;   // claim (no cooldown) vs change (24h, server-side)
  std::string acknowledgedName_;  // the server-acknowledged name; the box is never truth
  std::vector<std::string> authMethods_;
  std::string referralCode_;
  ReferralTotalsFetch referralCount_;
  std::string clientId_;
  std::vector<urnet::RedeemedBalanceCode> codes_;

  // 20 s watchdogs (CONTRACT rule 3), one per load and one per page-level
  // mutation: every one of them ends in a verdict even when the SDK never
  // calls back. The sheets carry their own.
  AccountFlow accountFlow_;
  AccountFlow codesFlow_;
  AccountFlow referralFlow_;
  AccountFlow nameFlow_;
  AccountFlow resetFlow_;
  AccountFlow portalFlow_;
  AccountFlow removeFlow_;
  AccountFlow updatePaymentFlow_;  // the past-due notice's auth code

  // re-entry flags (§7): a button disabled in flight, re-enabled on EVERY path
  bool savingName_ = false;
  bool sendingReset_ = false;
  // a reset rate limit: Send stays off until the server will send again
  ResendCooldown resetRateLimit_;
  sigc::connection resetRateLimitTick_;
  bool openingPortal_ = false;
  bool openingUpdatePayment_ = false;
  bool removingAuth_ = false;
  bool editingName_ = false;
  bool previewMode_ = false;

  // ---- sheets ----------------------------------------------------------------
  bool sheetShowing_ = false;  // this page's half of the one-modal gate
  std::unique_ptr<AccountAddAuthSheet> addAuthSheet_;
  std::unique_ptr<AccountAuthCodeSheet> authCodeSheet_;
  std::unique_ptr<AccountDeleteSheet> deleteSheet_;
  std::unique_ptr<ExtenderShareSheet> extenderShareSheet_;
  std::unique_ptr<ExtenderImportSheet> extenderImportSheet_;
  std::unique_ptr<Gtk::Window> confirmDialog_;  // the remove-login-method confirm
};

}  // namespace urnw
