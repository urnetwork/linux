// GTK4 (gtkmm4) main window. Auth pages (email-first sign-in with authLogin
// account discovery, password step, create network, verify, password reset)
// and home, toggled through a Gtk::Stack by auth state and the
// login-flow navigation; wired directly to the single-process SdkHost. SDK
// callbacks are marshaled onto the GTK main loop with PostToMain (Ui.hpp).
// libadwaita widgets (AdwHeaderBar, AdwViewStack, C API) are layered in as the
// UI grows to full parity; this subset uses gtkmm widgets so it compiles
// standalone. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>

#include <gtkmm.h>

#include "AuthLogout.hpp"
#include "AuthViews.hpp"
#include "BittensorManualSheet.hpp"
#include "BrowserSignInGate.hpp"
#include "ConnectPage.hpp"
#include "DataInfoSheet.hpp"
#include "FailsafeNotice.hpp"
#include "HomeShell.hpp"
#include "InsufficientBalanceNotice.hpp"
#include "AccountPage.hpp"
#include "DeveloperPage.hpp"
#include "EarningsPage.hpp"
#include "NetworkPage.hpp"
#include "SettingsPage.hpp"
#include "UpdateChecker.hpp"
#include "SupportPage.hpp"
#include "LocationOverride.hpp"
#include "LocationsSheet.hpp"
#include "LoginCarousel.hpp"
#include "NetworkServerSheet.hpp"
#include "Onboarding.hpp"
#include "ProviderLocationsSheet.hpp"
#include "RedeemCodeSheet.hpp"
#include "ReferralsPage.hpp"
#include "SdkHost.hpp"
#include "SeedphraseSheet.hpp"
#include "SessionsPage.hpp"
#include "ProCelebration.hpp"
#include "GuestConversionSheet.hpp"
#include "SubscriptionBalance.hpp"
#include "UpgradeSheet.hpp"
#include "UrMotion.hpp"

namespace urnw {

class MainWindow : public Gtk::ApplicationWindow {
 public:
  explicit MainWindow(SdkHost& host);
  ~MainWindow() override;

  // THE ACTION IS A PARAMETER. `disconnect` is the action that wrote the label
  // the user clicked, carried with the press by ConnectPage::on_connect_action.
  // It is not re-derived here, because this window's own answer (connected_ =
  // SdkHost::Connected(), which additionally requires the DeviceRemote to be
  // bound to the CURRENT control session) is a different, stricter question
  // than the one the page put on the button — and in every state where they
  // disagree, re-deriving turns a Disconnect press into a connection attempt.
  void ToggleConnect(bool disconnect);
  // The no-argument form is for callers with NO button in front of the user —
  // the tray menu — which must therefore ask the page what the press means.
  void ToggleConnect();
  // the Failed state's press: disconnect, then connect to the same selection
  void RetryConnect();
  bool connected() const { return connected_; }
  // The out-of-balance notification's Disconnect button ("app." +
  // kBalanceNoticeDisconnectAction, registered in main.cpp). Disconnect only:
  // it never starts a connection.
  static constexpr const char* kBalanceNoticeDisconnectAction = "balance-notice-disconnect";
  void DisconnectFromBalanceNotice();
  // Closing just hid the window to the tray: the first time ever, a
  // notification says the app is still running there (TrayPolicy.hpp).
  void NoteHiddenToTray();
  // The window's size and maximized state, for the next run to open at
  // (WindowGeometry.hpp): when the window closes, when the app quits, and
  // shortly after a resize.
  void SaveGeometry();
  // The screenshot hook (main.cpp URNETWORK_SHOOT) renders this window when a
  // URNW_ONBOARDING_PREVIEW review has it open, else null.
  Gtk::Window* PreviewSheet() const { return onboarding_ ? onboarding_.get() : nullptr; }
  // The tray: the session (its Connect/Disconnect item), whether a provider
  // is proven (its icon) and the state's words (its tooltip), on change.
  std::function<void(bool sessionUp, bool proven, const std::string& status)> on_tray_state;
  // The tray's recovery items (failsafe_notice::TrayRecoveryFor), pushed from
  // the health poll when they change.
  std::function<void(failsafe_notice::TrayRecovery)> on_tray_recovery_change;
  // The two items' actions, for a session this window does not hold: stop the
  // daemon's tunnel, and turn the kill switch off.
  void ForceTunnelOff();
  void LiftKillSwitch();

 private:
  void BuildChrome();         // 48px title bar: 20px app icon + PP NeueBit wordmark
  void BuildLogin();          // initial step: user auth discovery + the other entry points
  void BuildPasswordStep();   // password step of the email-first login
  void BuildSeedphraseStep(); // sign in with a 12/24-word phrase (windows parity)
  void BuildInstantStep();    // instant (seedphrase-only) account (windows parity)
  void BuildHome();
  // StartTunnel + render the daemon session state. "Daemon unreachable" and
  // "daemon too old" are DISTINCT actionable lines (MIGRATION.md) — the same
  // gray treatment as the app's other unavailable states, never a blank.
  // Once the tunnel is up it is pointed at `target`, or at the best available
  // provider when the target is none or flagged best available: a tunnel with
  // no destination installs routes, DNS and the filter and then carries
  // nothing, while looking identical to a working one.
  // `reason` names the gesture in the journal (a static string).
  TunnelStartResult StartTunnelUi(const char* reason,
                                  const std::optional<urnet::ConnectLocation>& target);
  // The same, to the location selected now (the Connect button, the tray,
  // connect on launch, a sign-in). Read before the start, because a start that
  // builds a new device answers SelectedLocation from that device.
  TunnelStartResult StartTunnelUi(const char* reason);
  void BuildAuthPages();  // create network / verify / password reset
  void OnGetStarted();  // authLogin discovery -> password / create / inline error
  void OnSignIn();
  void OnUseCode();     // auth-code login: a modal sheet, not an inline field
  void OnGoogle();         // Google through Google's web flow (SdkHost::SignInWithSso)
  void OnApple();          // Apple through Apple's web flow (SdkHost::SignInWithSso)
  void OnSso(const std::string& provider);
  void OnSolanaChooser();  // ONE Solana button -> a Phantom/Solflare chooser
  void OnSolana(WalletConnect::Provider provider);
  void OnBittensor();
  void OnBittensorWallet(const std::string& walletId);
  void ShowBittensorManualSheet(const SdkHost::BittensorManualRequest& request);
  void OnWalletAuth(const AuthResult& result);  // shared tail of both wallet sign-ins
  void OnSeedphraseChanged();
  void OnSeedphraseSubmit();
  void OnInstantSubmit();
  // android disables every sign-in affordance while any one is in flight
  void SetLoginBusy(bool busy);
  // narrow <-> wide login (the app-wide 1000dip breakpoint): the wide layout
  // reparents the carousel into the art pane beside a fixed 544dip form column
  void ApplyLoginBreakpoint(bool wide);
  // The signed-in twin of ApplyLoginBreakpoint: fan the window's content width
  // out to every destination so their pane folds actually fire (each page owns
  // its own thresholds; the window only measures).
  void ApplyPageBreakpoint(int widthDip);
  // Poll the daemon's own view of the tunnel. Nothing else does: the app learns
  // about the daemon only when the user presses something, so a session the
  // daemon tore down PROTECTIVELY (proven-unprotected traffic, or an
  // amplification storm) never reached the user while they sat idle — they kept
  // a green "Connected" while blocked or unprotected. This is the consumer for
  // that state.
  bool PollDaemonHealth();
  // The poll's reply, on the main loop: the follow-ups, the strip's daemon
  // facts and the protective-teardown verdict.
  void ApplyDaemonHealth(const std::optional<ctl::StatusReply>& status);

 protected:
  // GTK4 has no size-allocate signal; the window's own vfunc is the only place
  // the real (maximized, tiled, WM-resized) content width is observable.
  void size_allocate_vfunc(int width, int height, int baseline) override;

 private:
  // run the carousel only on the initial login step while the window shows
  void UpdateCarouselRunning();
  // App focus for the purchase-confirmation poll: true while ANY of the
  // process's toplevels is active (the upgrade and redeem sheets are their
  // own transient windows). Hooks every toplevel's notify::is-active as it
  // appears and coalesces the reading onto one idle.
  void TrackAppFocus();
  void ScheduleAppFocusSync();
  void UntrackAppFocus();
  // The app came back to the user after `awayMillis`: a browser sign-in they
  // abandoned gets its affordances back (BrowserSignInGate.hpp).
  void OnAppReturned(int64_t awayMillis);
  static void OnToplevelActiveChanged(GObject* window, GParamSpec* pspec, gpointer self);
  // one label, two voices: a coral inline error vs a muted progress notice
  void SetLoginError(const Glib::ustring& text);
  void SetLoginNotice(const Glib::ustring& text);
  // the signed-out Hero Bloom (motion-overhaul §2.1): hero spring + ripple
  void RunSignedOutReveal();
  void SettleReveal();  // CancelToFinal: every ring to the settled pose
  void NavigateCreate(CreateNetworkPage::Mode mode, const std::string& userAuth, bool fromHome);
  // Every create-account and purchase affordance for a legacy guest network
  // (the Connect page's held alert, Account's plan action, the upgrade sheet)
  // opens the in-place conversion (GuestConversionSheet):
  // a sign-in is added to THIS network and verified, so its plan and balance
  // stay. Signing out would abandon the network for good (it has no login).
  void OpenGuestConversion();
  // A guest's purchase entry: the in-place conversion first, then `checkout`
  // once it is done and the network no longer reads as a guest
  // (GuestUpgradeContinuation).
  void DivertGuestToConversion(std::function<void()> checkout);
  void OnOnboardingGuestSignInRequired();
  void NavigateVerify(const std::string& userAuth);
  // The sdk reported that the server rejected this app's sign-in
  // (SdkHost::SetAuthInvalidHandler), marshaled here: the normal Logout(), once
  // per rejection, and the sign-in page's notice its cause allows.
  void OnAuthLogout(const auth_logout::Report& report);
  void ApplyAuthState(bool loggedIn);
  // ONE READING IN, EVERY WINDOW SURFACE OUT. There is no SetConnected(bool)
  // any more: a bool is what let this window's copy of "connected" age
  // independently of the page's, and of the stats copy beside it.
  void ApplyConnectReading(const ConnectReading& reading);
  // The current reading with tunnelBound forced down: what the daemon status
  // poll has just proven when it finds urnetworkd no longer carrying.
  ConnectReading DaemonTunnelGoneReading();
  void ApplyStats(const LiveStats& stats);  // the pages' live stats and the status strip
  // The status strip's Advanced fields that come from the session rather than
  // the stats: Network, Session, Routes and RPC.
  void ApplyStatusStripDetails();
  // The daemon's last status reply (PollDaemonHealth), for the strip's Routes
  // field; dropped when a session starts or ends on purpose.
  std::optional<ctl::StatusReply> daemonStatus_;
  // Bumped with every drop of daemonStatus_, so a poll reply that was in
  // flight across a start, a Disconnect or a sign-in or -out is dropped.
  uint64_t daemonStatusEpoch_ = 0;
  // Drops the last reply and any still in flight, and renders the strip
  // without it.
  void ForgetDaemonStatus();
  void OpenProviderLocations();             // the "Connected to N providers" entry point
  // Keep the device-location override pointed at the oldest connected provider
  // that has coordinates. Runs off the SDK change feed rather than from the
  // sheet, so the override keeps following the window while the sheet is closed
  // and the window is hidden to the tray.
  void SyncLocationOverrideTarget();

  SdkHost& host_;
  // subscription balance / plan / referral store (the pages' plan views, the
  // upgrade + redeem confirmation polling)
  SubscriptionBalanceStore balance_;
  Gtk::Stack stack_;

  // The Pro celebration (ProCelebration.hpp): the overlay above the whole
  // window, the mosaic container around the page stack, and the one clock
  // both follow. Plays once at the free -> Pro upgrade, and on a tap of the
  // Account plan label while Pro.
  ProFlightClock proFlightClock_;
  ProCelebrationOverlay* proCelebration_ = nullptr;
  PixelateBin* proPixelateBin_ = nullptr;
  bool proCelebrated_ = false;
  void LaunchProCelebration();

  Gtk::Entry email_;
  Gtk::Button* getStartedBtn_ = nullptr;  // disabled while a discovery is in flight
  Gtk::PasswordEntry password_;           // lives on the password step
  Gtk::Button* signInBtn_ = nullptr;      // disabled while a sign-in is in flight
  bool signingIn_ = false;
  Gtk::Label passwordUserAuth_;           // the discovered auth the password belongs to
  Gtk::Label passwordError_;
  Gtk::Label loginError_;
  // "This session was signed out from another device.", owed to the sign-in
  // page by the sign-out OnAuthLogout made, until ApplyAuthState shows it
  auth_logout::SignInNotice signInNotice_;

  // ---- the branded initial step (windows LoginPanel parity) ----------------
  LoginCarousel* carousel_ = nullptr;
  motion::MotionBin* heroBin_ = nullptr;   // the carousel's motion wrapper (the HERO)
  Gtk::Box* artPane_ = nullptr;            // wide login: the carousel's pane
  Gtk::Box* loginPanel_ = nullptr;         // the initial step's column
  Gtk::Widget* loginFormColumn_ = nullptr; // fixed 544dip in the wide layout
  bool wideLogin_ = false;
  // reveal rings (the signed-out Hero Bloom table)
  motion::MotionBin* brandBin_ = nullptr;      // the wordmark's reveal ring (120ms beat)
  int64_t revealStartedUs_ = 0;  // monotonic start of the reveal in flight, 0 for none
  motion::MotionBin* emailGroupBin_ = nullptr;
  motion::MotionBin* getStartedBin_ = nullptr;
  motion::MotionBin* orBin_ = nullptr;
  motion::MotionBin* walletBin_ = nullptr;     // the three full-width pills
  motion::MotionBin* secondaryBin_ = nullptr;  // the icon tiles (four per row)
  std::vector<Gtk::Widget*> loginAffordances_;  // everything SetLoginBusy toggles
  // armed while a browser sign-in waits on its deep link
  signin::BrowserFlowGate browserSignIn_;

  // ---- seedphrase + instant steps (windows parity) -------------------------
  Gtk::TextView* seedphraseView_ = nullptr;
  Gtk::Label* seedphraseCount_ = nullptr;
  Gtk::Button* seedphraseSubmit_ = nullptr;
  Gtk::Label* seedphraseError_ = nullptr;
  bool seedphraseLoggingIn_ = false;
  Gtk::CheckButton* instantTerms_ = nullptr;
  Gtk::Switch* instantProductUpdates_ = nullptr;  // "Periodic product updates", default on
  Gtk::Button* instantCreate_ = nullptr;
  Gtk::Label* instantError_ = nullptr;
  bool creatingInstant_ = false;
  // the optional referral code on the instant path, always visible above
  // Create Account (android/apple parity)
  ReferralCodeBox* instantReferralCode_ = nullptr;
  std::unique_ptr<SeedphraseSheet> seedphraseSheet_;
  std::unique_ptr<BittensorManualSheet> bittensorManualSheet_;
  std::unique_ptr<NetworkServerSheet> networkServerSheet_;
  // The user auth the discovery routed to the password step (normalized echo);
  // the password sign-in, forgot-password, and reset flows all key off it.
  std::string loginUserAuth_;
  bool discoveringLogin_ = false;

  HomeShell* shell_ = nullptr;       // the signed-in nav shell (windows NavigationView home)
  // Windows-parity destinations (docs/parity/*.md); the rest are placeholders
  // until their pages land.
  ConnectPage* connectPage_ = nullptr;
  NetworkPage* networkPage_ = nullptr;
  SettingsPage* settingsPage_ = nullptr;
  DeveloperPage* developerPage_ = nullptr;
  SupportPage* supportPage_ = nullptr;
  EarningsPage* earningsPage_ = nullptr;
  AccountPage* accountPage_ = nullptr;
  ReferralsPage* referralsPage_ = nullptr;  // reached from Account's Referrals row
  SessionsPage* sessionsPage_ = nullptr;    // reached from Account's Sessions row
  // The in-app updater (UpdateChecker.hpp): built after the pages, bound to
  // Settings (the notice + the auto-check toggle) and Developer (the manual
  // check), then started. Its worker is joined when the window goes.
  std::unique_ptr<UpdateChecker> updates_;
  // Account's Redeem row opens it (lazily built).
  std::unique_ptr<RedeemCodeSheet> redeemSheet_;
  // Every upgrade entry point opens it through OpenUpgrade (lazily built).
  std::unique_ptr<UpgradeSheet> upgradeSheet_;
  // The next OpenUpgrade's sheet says when the free data refreshes: set only
  // around the start-connect block's opening (ConnectBlockedByBalance).
  bool nextUpgradeFreeRefresh_ = false;
  // The location/provider chooser behind the Connect page's location row
  // (lazily built); refreshed from the drawer event feed while it is open.
  std::unique_ptr<LocationsSheet> locationsSheet_;
  void OpenLocationChooser();
  std::unique_ptr<DataInfoSheet> dataInfoSheet_;  // lazily built
  std::unique_ptr<GuestConversionSheet> guestConversionSheet_;  // lazily built
  GuestUpgradeContinuation guestUpgrade_;
  std::unique_ptr<OnboardingWindow> onboarding_;
  void OpenOnboardingIfPending();
  // urnetwork://onboarding/<connect|widgets|offer|feedback> (the campaign
  // emails' buttons): Connect, Account (the closest page to Widgets), the
  // offer page (the upgrade sheet when no offer is active), the feedback form
  // pre-filled from the link's token.
  void HandleOnboardingLink(const std::string& url);
  // connect.first, once per network (remembered in the prefs)
  void NoteConnected();
  // Last width (in dip) fanned out to the destinations. Pages fold their own
  // panes; nothing else in the app measures the window for them.
  int pageWidthDip_ = -1;
  // One modal sheet at a time across the whole signed-in shell (the windows
  // rule): a page asks before presenting and reports its own open/close.
  bool sheetOpen_ = false;

  // login-flow pages (stack children; MainWindow wires the navigation)
  CreateNetworkPage* createPage_ = nullptr;
  VerifyPage* verifyPage_ = nullptr;
  ResetPasswordPage* resetPage_ = nullptr;
  bool createPageFromHome_ = false;  // guest upgrade backs out to home, not login

  // The last reading, and the one bit every window surface derives from it:
  // "there is a session to disconnect from" (health::SessionUp). The tray's
  // label and the tray's action both read it, so they cannot disagree.
  ConnectReading reading_;
  // Has a reading ever been applied / pushed to the tray? Without these the
  // FIRST reading — which for an idle app equals the default-constructed one —
  // would be skipped as "unchanged" and no surface would ever be seeded.
  bool readingApplied_ = false;
  bool trayStatePushed_ = false;
  bool trayProven_ = false;
  std::string trayStatus_;
  bool connected_ = false;
  // A session this window saw has ended and no status has been read since:
  // the explanation of a failsafe stop is owed to the disconnected poll, since
  // the connect feed can report the disconnect before a poll reads the stop.
  bool stopExplanationOwed_ = false;
  // What the tray last offered, and whether anything was pushed yet.
  failsafe_notice::TrayRecovery trayRecovery_;
  bool trayRecoveryPushed_ = false;
  void PushTrayRecovery(const failsafe_notice::TrayRecovery& recovery);
  // The out-of-balance desktop notification (InsufficientBalanceNotice.hpp),
  // fed from the connect reading and the balance store.
  balance_notice::Tracker balanceNotice_;
  void UpdateBalanceNotice();
  // The last known out-of-balance state, kept across the user's Disconnect
  // (the SDK clears the contract status with the destination).
  balance_notice::OutOfBalanceLatch outOfBalance_;
  // The start-connect gate every connect entry point asks before starting
  // anything (balance_notice::DecideStartConnect). True means nothing was
  // started: the press was turned into the upgrade path, or the balance is
  // stale and is read first, after which retry repeats the press. A session
  // that is already up is never blocked.
  bool ConnectBlockedByBalance(std::function<void()> retry);
  // Reads the balance (kBalanceCheckTimeoutMillis at most), then runs the
  // waiting connect.
  void CheckBalanceThen(std::function<void()> retry);
  // Drops a connect waiting on a balance read (sign-in and sign-out).
  void CancelBalanceCheck();
  // A connect the balance blocked, retried by itself once data is back
  // (balance_notice::BalanceRecovery): the press the gate refused (the retry
  // it was handed), or the connection held out of balance.
  balance_notice::BalanceRecovery<std::function<void()>> balanceRecovery_;
  // ObserveBalanceRecovery is running the refused press: the gate admits it
  bool retryingRefusedConnect_ = false;
  // Feeds the recovery after the latch (UpdateBalanceNotice), shows its lines
  // on the Connect page, and makes the retry it decides on.
  void ObserveBalanceRecovery();
  // Another connect, the user's Disconnect, a sign-out or Cancel: nothing
  // waits on the balance any more.
  void ClearBalanceRecovery();
  // The held alert's signals, as UpdateBalanceNotice builds them, and the
  // balance as the gate reads it, for the recovery and its lines.
  balance_notice::Signals BalanceSignals() const;
  balance_notice::AccountBalance CurrentAccountBalance() const;
  // Shows the recovery's lines on the Connect page (no retry).
  void ApplyBalanceRecoveryLines();
  std::function<void()> pendingConnect_;
  bool balanceCheckPending_ = false;
  uint64_t balanceCheck_ = 0;
  sigc::connection balanceCheckTimeout_;
  // when the last balance read for a connect failed or timed out, -1 for none
  int64_t balanceCheckFailedAtMillis_ = -1;
  // The upgrade path: guest conversion for a guest, else the upgrade sheet.
  void OpenUpgrade();
  // "About your data" (DataInfoSheet): Account's info button, the Connect
  // page alert's Why?
  void OpenDataInfo();
  // tray app: skip window-widget updates while hidden (resynced on show) so a
  // hidden window doesn't churn on high-frequency SDK updates
  bool windowVisible_ = false;
  bool homeRevealed_ = false;  // Home has shown once in this window (its entrance)
  sigc::connection appFocusSync_;      // the pending coalesced focus reading
  sigc::connection geometrySave_;      // the pending save after a resize
  signin::AppFocusAway appFocusAway_;  // how long the coalesced reading was away
  sigc::connection toplevelsChanged_;  // the toplevel list's items-changed hook
  LiveStats lastStats_;  // resynced into the widgets when the window is shown

  // Device-location override (GeoClue static source). Owned here rather than by
  // the sheet so that its MANDATORY startup cleanup runs on every launch, even
  // when the feature's UI is never opened -- nothing on the system reverts
  // /etc/geolocation for us, so an override left by a killed process would
  // otherwise persist indefinitely, across reboots.
  std::unique_ptr<GeoClueLocationOverride> locationOverride_;
  std::unique_ptr<ProviderLocationsSheet> providerLocationsSheet_;
};

}  // namespace urnw
