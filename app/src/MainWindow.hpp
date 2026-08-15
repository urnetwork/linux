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

#include "AuthViews.hpp"
#include "ConnectDrawer.hpp"
#include "ConnectPage.hpp"
#include "HomeShell.hpp"
#include "AccountPage.hpp"
#include "DeveloperPage.hpp"
#include "EarningsPage.hpp"
#include "NetworkPage.hpp"
#include "SettingsPage.hpp"
#include "SupportPage.hpp"
#include "LocationOverride.hpp"
#include "LoginCarousel.hpp"
#include "NetworkServerSheet.hpp"
#include "ProviderLocationsSheet.hpp"
#include "SdkHost.hpp"
#include "SeedphraseSheet.hpp"
#include "SubscriptionBalance.hpp"
#include "UrMotion.hpp"

namespace urnw {

class MainWindow : public Gtk::ApplicationWindow {
 public:
  explicit MainWindow(SdkHost& host);

  void ToggleConnect();  // driven by the tray too
  bool connected() const { return connected_; }
  std::function<void(bool connected)> on_connected_change;

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
  TunnelStartResult StartTunnelUi();
  void RefreshPeersStatus();  // home-screen peers status line (dot + "{n} peers")
  void ApplyProvideControlMode();  // picker -> host (guarded during sync)
  void SyncProvideControlMode();   // host -> picker
  void BuildAuthPages();  // create network / verify / password reset
  void OnGetStarted();  // authLogin discovery -> password / create / inline error
  void OnSignIn();
  void OnUseCode();     // auth-code login: a modal sheet, not an inline field
  void OnSolanaChooser();  // ONE Solana button -> a Phantom/Solflare chooser
  void OnSolana(WalletConnect::Provider provider);
  void OnBittensor();
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

 protected:
  // GTK4 has no size-allocate signal; the window's own vfunc is the only place
  // the real (maximized, tiled, WM-resized) content width is observable.
  void size_allocate_vfunc(int width, int height, int baseline) override;

 private:
  // run the carousel only on the initial login step while the window shows
  void UpdateCarouselRunning();
  // one label, two voices: a coral inline error vs a muted progress notice
  void SetLoginError(const Glib::ustring& text);
  void SetLoginNotice(const Glib::ustring& text);
  // the signed-out Hero Bloom (motion-overhaul §2.1): hero spring + ripple
  void RunSignedOutReveal();
  void SettleReveal();  // CancelToFinal: every ring to the settled pose
  void NavigateCreate(CreateNetworkPage::Mode mode, const std::string& userAuth, bool fromHome);
  void NavigateVerify(const std::string& userAuth);
  void ApplyAuthState(bool loggedIn);
  void SetConnected(bool connected);
  void ApplyStats(const LiveStats& stats);  // live provider count / throughput / provide
  void OpenProviderLocations();             // the "Connected to N providers" entry point
  // Keep the device-location override pointed at the oldest connected provider
  // that has coordinates. Runs off the SDK change feed rather than from the
  // sheet, so the override keeps following the window while the sheet is closed
  // and the window is hidden to the tray.
  void SyncLocationOverrideTarget();

  SdkHost& host_;
  // subscription balance / plan / referral store (the drawer's plan card, the
  // upgrade + redeem confirmation polling)
  SubscriptionBalanceStore balance_;
  Gtk::Stack stack_;

  Gtk::Entry email_;
  Gtk::Button* getStartedBtn_ = nullptr;  // disabled while a discovery is in flight
  Gtk::PasswordEntry password_;           // lives on the password step
  Gtk::Button* signInBtn_ = nullptr;      // disabled while a sign-in is in flight
  bool signingIn_ = false;
  Gtk::Label passwordUserAuth_;           // the discovered auth the password belongs to
  Gtk::Label passwordError_;
  Gtk::Label loginError_;

  // ---- the branded initial step (windows LoginPanel parity) ----------------
  LoginCarousel* carousel_ = nullptr;
  motion::MotionBin* heroBin_ = nullptr;   // the carousel's motion wrapper (the HERO)
  Gtk::Box* artPane_ = nullptr;            // wide login: the carousel's pane
  Gtk::Box* loginPanel_ = nullptr;         // the initial step's column
  Gtk::Widget* loginFormColumn_ = nullptr; // fixed 544dip in the wide layout
  bool wideLogin_ = false;
  // reveal rings (the signed-out Hero Bloom table)
  motion::MotionBin* brandBin_ = nullptr;      // the wordmark's reveal ring (120ms beat)
  motion::MotionBin* emailGroupBin_ = nullptr;
  motion::MotionBin* getStartedBin_ = nullptr;
  motion::MotionBin* orBin_ = nullptr;
  motion::MotionBin* walletBin_ = nullptr;
  motion::MotionBin* secondaryBin_ = nullptr;
  std::vector<Gtk::Widget*> loginAffordances_;  // everything SetLoginBusy toggles

  // ---- seedphrase + instant steps (windows parity) -------------------------
  Gtk::TextView* seedphraseView_ = nullptr;
  Gtk::Label* seedphraseCount_ = nullptr;
  Gtk::Button* seedphraseSubmit_ = nullptr;
  Gtk::Label* seedphraseError_ = nullptr;
  bool seedphraseLoggingIn_ = false;
  Gtk::CheckButton* instantTerms_ = nullptr;
  Gtk::Button* instantCreate_ = nullptr;
  Gtk::Label* instantError_ = nullptr;
  bool creatingInstant_ = false;
  std::unique_ptr<SeedphraseSheet> seedphraseSheet_;
  std::unique_ptr<NetworkServerSheet> networkServerSheet_;
  // The user auth the discovery routed to the password step (normalized echo);
  // the password sign-in, forgot-password, and reset flows all key off it.
  std::string loginUserAuth_;
  bool discoveringLogin_ = false;

  Gtk::Label status_{"Disconnected"};
  // daemon session problems (unreachable / out of date / app out of date /
  // start failure), rendered under the status line; hidden while healthy
  Gtk::Label daemonStatusLabel_;
  Gtk::Button connectBtn_{"Connect"};
  // provide indicator (apple parity): "●" solid dot = Network provide (green;
  // coral when not providing), "◉" dot + ring = Public provide (amber when
  // paused — pause stops public only)
  Gtk::Label provideModeDot_;
  // provide control mode picker: Auto | Always | Network | Never. "Network" is
  // the private provider (always on, provides to same-network peers only).
  Gtk::ToggleButton* provideAuto_ = nullptr;
  Gtk::ToggleButton* provideAlways_ = nullptr;
  Gtk::ToggleButton* provideNetwork_ = nullptr;
  Gtk::ToggleButton* provideNever_ = nullptr;
  bool syncingProvideMode_ = false;  // guards Apply during programmatic sync
  // live stats (macOS parity); also the entry point into the provider-locations
  // sheet, clickable only while genuinely connected
  Gtk::Label providerCountLabel_;
  bool providerCountClickable_ = false;
  Gtk::Label throughputLabel_;
  Gtk::Label provideStatsLabel_;
  Gtk::Label peersStatusDot_;   // green when peers > 0, red at 0
  Gtk::Label peersStatusText_;  // "{n} peers"; tapping opens the chooser
  Gtk::Label discoverableLabel_;  // "This device is discoverable" (apple parity)
  ConnectDrawer* drawer_ = nullptr;  // connect drawer (controls/stats/dns/blocker/plan cards)
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
  // Account's Redeem row opens the same sheet the drawer owns, but the
  // drawer exposes no opener, so the window keeps its own (lazily built).
  std::unique_ptr<RedeemCodeSheet> redeemSheet_;
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

  bool connected_ = false;
  // the free -> Pro provide reset ran for this session's upgrade detection
  // (the store's flag stays up all session; the reset must apply exactly once
  // so the user's later opt-back-in sticks)
  bool provideResetOnUpgrade_ = false;
  // tray app: skip window-widget updates while hidden (resynced on show) so a
  // hidden window doesn't churn on high-frequency SDK updates
  bool windowVisible_ = false;
  std::string lastStatus_ = "Disconnected";
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
