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

#include <gtkmm.h>

#include "AuthViews.hpp"
#include "ConnectDrawer.hpp"
#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"

namespace urnw {

class MainWindow : public Gtk::ApplicationWindow {
 public:
  explicit MainWindow(SdkHost& host);

  void ToggleConnect();  // driven by the tray too
  bool connected() const { return connected_; }
  std::function<void(bool connected)> on_connected_change;

 private:
  void BuildLogin();          // initial step: user auth discovery + the other entry points
  void BuildPasswordStep();   // password step of the email-first login
  void BuildHome();
  void RefreshPeersStatus();  // home-screen peers status line (dot + "{n} peers")
  void BuildAuthPages();  // create network / verify / password reset
  void OnGetStarted();  // authLogin discovery -> password / create / inline error
  void OnSignIn();
  void OnUseCode();
  void OnGuestMode();
  void OnSolana(WalletConnect::Provider provider);
  void OnBittensor();
  void OnWalletAuth(const AuthResult& result);  // shared tail of both wallet sign-ins
  void NavigateCreate(CreateNetworkPage::Mode mode, const std::string& userAuth, bool fromHome);
  void NavigateVerify(const std::string& userAuth);
  void ApplyAuthState(bool loggedIn);
  void SetConnected(bool connected);
  void ApplyStats(const LiveStats& stats);  // live provider count / throughput / provide

  SdkHost& host_;
  // subscription balance / plan / referral store (the drawer's plan card, the
  // upgrade + redeem confirmation polling)
  SubscriptionBalanceStore balance_;
  Gtk::Stack stack_;

  Gtk::Entry email_;
  Gtk::Button* getStartedBtn_ = nullptr;  // disabled while a discovery is in flight
  Gtk::PasswordEntry password_;           // lives on the password step
  Gtk::Label passwordUserAuth_;           // the discovered auth the password belongs to
  Gtk::Label passwordError_;
  Gtk::Entry code_;
  Gtk::Label loginError_;
  // The user auth the discovery routed to the password step (normalized echo);
  // the password sign-in, forgot-password, and reset flows all key off it.
  std::string loginUserAuth_;
  bool discoveringLogin_ = false;

  Gtk::Label status_{"Disconnected"};
  Gtk::Button connectBtn_{"Connect"};
  Gtk::Switch provideSwitch_;
  Gtk::Label providerCountLabel_;  // live stats (macOS parity)
  Gtk::Label throughputLabel_;
  Gtk::Label provideStatsLabel_;
  Gtk::Label peersStatusDot_;   // green when peers > 0, red at 0
  Gtk::Label peersStatusText_;  // "{n} peers"; tapping opens the chooser
  ConnectDrawer* drawer_ = nullptr;  // connect drawer (controls/stats/dns/blocker/plan cards)

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
};

}  // namespace urnw
