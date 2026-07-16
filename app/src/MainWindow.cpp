// SPDX-License-Identifier: MPL-2.0
#include "MainWindow.hpp"

#include <adwaita.h>
#include <glib.h>

#include <cstdio>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// a user auth is an email or a phone number (light shape check gating the
// discovery call; the server is the real validator — Windows/mac parity)
bool LooksLikeUserAuth(const std::string& value) {
  if (value.find('@') != std::string::npos) return value.size() >= 3;
  size_t digits = 0;
  for (char c : value) {
    if ('0' <= c && c <= '9') ++digits;
  }
  return digits >= 7;
}

}  // namespace

MainWindow::MainWindow(SdkHost& host) : host_(host), balance_(host) {
  set_title("URnetwork");
  set_default_size(480, 780);  // tall enough for the connect drawer column

  BuildLogin();
  BuildPasswordStep();
  BuildHome();
  BuildAuthPages();
  set_child(stack_);

  // Track window visibility (tray app: closing hides to tray). Skip window-widget
  // updates while hidden and resync when shown, so a hidden window doesn't churn
  // on high-frequency SDK updates. Live stats and the balance poll follow this
  // same gate (the balance store resyncs itself on show).
  property_visible().signal_changed().connect([this] {
    windowVisible_ = get_visible();
    balance_.SetWindowVisible(windowVisible_);
    if (windowVisible_) {
      status_.set_text(lastStatus_);
      SetConnected(host_.Connected());
      ApplyStats(lastStats_);
      if (drawer_) drawer_->RefreshAll();  // drawer events are dropped while hidden
    }
  });

  // Balance/plan changes land on the GTK loop already (the store marshals);
  // fan out to the drawer's plan card, banner, and the upgrade sheet states.
  balance_.SetChangedHandler([this] {
    if (drawer_) drawer_->OnBalanceChanged();
    // The free -> Pro upgrade side effect (mac MainView reacts to
    // didDetectUpgradeToPro): reset provide mode to never at the upgrade,
    // exactly once — the user can opt back in afterward and that sticks.
    if (balance_.DidDetectUpgradeToPro() && !provideResetOnUpgrade_) {
      provideResetOnUpgrade_ = true;
      host_.ResetProvideToNever();
      provideSwitch_.set_active(false);  // reflect it in the home controls
    }
  });

  // Auth-state transitions are marshaled onto the GTK loop and flip the view.
  host_.SetAuthStateHandler([this](bool loggedIn) {
    PostToMain([this, loggedIn] { ApplyAuthState(loggedIn); });
  });
  // The server rejected the stored auth (e.g. the client was removed): log out
  // and return to the login panel. Logout() fires the auth-state handler.
  host_.SetAuthInvalidHandler([this] {
    PostToMain([this] { host_.Logout(); });
  });
  host_.SetConnectionStatusHandler([this](std::string status) {
    PostToMain([this, status] {
      lastStatus_ = status;
      // the tray must reflect state even while hidden; the window label only
      // when visible (resynced on show)
      SetConnected(host_.Connected());
      if (windowVisible_) status_.set_text(status);
    });
  });
  // Live stats (provider count / throughput / provide). Same visibility gate:
  // cache always, but only touch widgets while the window is shown.
  host_.SetStatsHandler([this](const LiveStats& stats) {
    PostToMain([this, stats] {
      lastStats_ = stats;
      if (windowVisible_) ApplyStats(stats);
    });
  });
  // Connect drawer feed (charts / block actions / dns / blocker / controls).
  // Same visibility gate: dropped while hidden, resynced on show.
  host_.SetDrawerEventHandler([this](DrawerEvent event) {
    PostToMain([this, event] {
      if (windowVisible_ && drawer_) drawer_->OnHostEvent(event);
      if (event == DrawerEvent::Peers || event == DrawerEvent::DeviceLifecycle) {
        RefreshPeersStatus();
      }
    });
  });

  if (host_.IsLoggedIn()) {
    host_.StartTunnel();
    ApplyAuthState(true);
  } else {
    ApplyAuthState(false);
  }
}

void MainWindow::BuildLogin() {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_valign(Gtk::Align::CENTER);

  // the product name is never translated (the store marks it so)
  auto* title = Gtk::make_managed<Gtk::Label>();
  title->set_markup("<span size='xx-large' weight='bold'>URnetwork</span>");
  box->append(*title);

  // Email-first account discovery (mac LoginInitialView / Windows initial
  // step): Get started routes through Api::authLogin to the password step,
  // sign-up, or an inline use-your-other-method error (OnGetStarted).
  email_.set_placeholder_text(T_("user_auth_label", "Email or phone"));
  email_.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::OnGetStarted));
  box->append(email_);

  getStartedBtn_ = Gtk::make_managed<Gtk::Button>(T_("get_started", "Get started"));
  getStartedBtn_->add_css_class("suggested-action");
  getStartedBtn_->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnGetStarted));
  box->append(*getStartedBtn_);

  loginError_.add_css_class("error");
  loginError_.set_wrap(true);
  box->append(loginError_);

  box->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  // sign-up (create network) — a new user onboards here
  auto* createAccount = Gtk::make_managed<Gtk::Button>(T_("create_account", "Create account"));
  createAccount->signal_clicked().connect([this] {
    loginError_.set_text("");
    NavigateCreate(CreateNetworkPage::Mode::Password, email_.get_text(), /*fromHome=*/false);
  });
  box->append(*createAccount);

  code_.set_placeholder_text(T_("auth_code", "Auth code"));
  box->append(code_);
  auto* useCode = Gtk::make_managed<Gtk::Button>(T_("sign_in_with_code", "Sign in with code"));
  useCode->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnUseCode));
  box->append(*useCode);

  // Guest mode (iOS/macOS parity, DESKTOP2 §2): one-tap throwaway network,
  // upgradeable to a full account later (the plan card's Create account).
  auto* guest = Gtk::make_managed<Gtk::Button>(T_("try_guest_mode", "Try Guest Mode"));
  guest->add_css_class("flat");
  guest->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnGuestMode));
  box->append(*guest);

  // Sign in with Bittensor (apple/BITTENSOR.md): the same ur.io/wallet-connect
  // bridge, in one hop — it drives an injected substrate wallet (Bittensor Wallet,
  // SubWallet, Talisman, polkadot-js), or pairs with a wallet app over
  // WalletConnect when Config.hpp carries a project id, and returns the ss58
  // address + sr25519 signature via urnetwork://bittensor-sign-message.
  auto* bittensor =
      Gtk::make_managed<Gtk::Button>(T_("bittensor_sign_in", "Sign in with Bittensor"));
  bittensor->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnBittensor));
  box->append(*bittensor);

  // Sign in with Solana (DESKTOP2 wallet-connect): opens the ur.io/wallet-connect
  // browser bridge; the wallet extension signs and returns via urnetwork://.
  box->append(*Gtk::make_managed<Gtk::Label>(T_("solana_sign_in", "Sign in with Solana")));
  auto* walletRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  walletRow->set_halign(Gtk::Align::CENTER);
  // wallet names are product names: never translated (the store marks them so)
  auto* phantom = Gtk::make_managed<Gtk::Button>("Phantom");
  phantom->signal_clicked().connect([this] { OnSolana(WalletConnect::Provider::Phantom); });
  auto* solflare = Gtk::make_managed<Gtk::Button>("Solflare");
  solflare->signal_clicked().connect([this] { OnSolana(WalletConnect::Provider::Solflare); });
  walletRow->append(*phantom);
  walletRow->append(*solflare);
  box->append(*walletRow);

  // NOTE full-parity: Google/Apple browser SSO slots in here; the SdkHost/Api
  // surface exists (authLogin{auth_jwt}).
  stack_.add(*box, "login");
}

// The password step of the email-first login (mac LoginPasswordView / Windows
// PasswordPanel): discovery said the user auth has a password account. The
// verify routing for an unverified account and the forgot-password reset both
// hang off this step, keyed on the discovered loginUserAuth_.
void MainWindow::BuildPasswordStep() {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_valign(Gtk::Align::CENTER);

  auto* back = Gtk::make_managed<Gtk::Button>();
  back->set_icon_name("go-previous-symbolic");
  back->add_css_class("flat");
  back->set_halign(Gtk::Align::START);
  back->signal_clicked().connect([this] {
    password_.set_text("");
    passwordError_.set_text("");
    stack_.set_visible_child("login");
  });
  box->append(*back);

  auto* title = Gtk::make_managed<Gtk::Label>(
      T_("its_nice_to_see_you_again", "It's nice to see you again"));
  title->add_css_class("title-1");
  title->set_wrap(true);
  box->append(*title);

  // the discovered user auth this password belongs to
  passwordUserAuth_.add_css_class("dim-label");
  passwordUserAuth_.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
  box->append(passwordUserAuth_);

  password_.set_show_peek_icon(true);
  password_.property_placeholder_text() = T_("password_label", "Password");
  // Enter submits. The GtkPasswordEntry::activate C signal is GTK 4.0; the
  // gtkmm wrapper (signal_activate) only landed in 4.18 and core24 ships 4.10,
  // so connect at the C level.
  g_signal_connect(password_.gobj(), "activate",
                   G_CALLBACK(+[](GtkPasswordEntry*, gpointer data) {
                     static_cast<MainWindow*>(data)->OnSignIn();
                   }),
                   this);
  box->append(password_);

  auto* signIn = Gtk::make_managed<Gtk::Button>(T_("sign_in", "Sign in"));
  signIn->add_css_class("suggested-action");
  signIn->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnSignIn));
  box->append(*signIn);

  // password reset flow (Api::authPasswordReset behind the reset page)
  auto* forgot = Gtk::make_managed<Gtk::Button>(T_("forgot_password", "Forgot your password?"));
  forgot->add_css_class("flat");
  forgot->signal_clicked().connect([this] {
    passwordError_.set_text("");
    resetPage_->Configure(loginUserAuth_);
    stack_.set_visible_child("reset");
  });
  box->append(*forgot);

  passwordError_.add_css_class("error");
  passwordError_.set_wrap(true);
  box->append(passwordError_);

  stack_.add(*box, "password");
}

void MainWindow::RefreshPeersStatus() {
  const auto peers = host_.ConnectedProvidePeers();
  const int peerCount = peers ? static_cast<int>(peers->size()) : 0;
  const Rgba dotColor = 0 < peerCount ? kUrGreen : kUrAmber;
  peersStatusDot_.set_markup("<span foreground='" + HexForMarkup(dotColor) + "'>●</span>");
  peersStatusText_.set_text(
      Format(TN_("network_peer_count", "You have {} other device online",
                 "You have {} other devices online", peerCount),
             peerCount));
}

void MainWindow::BuildHome() {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  box->set_margin(24);
  box->set_valign(Gtk::Align::START);

  status_.add_css_class("title-2");
  box->append(status_);

  connectBtn_.add_css_class("suggested-action");
  connectBtn_.add_css_class("pill");
  connectBtn_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::ToggleConnect));
  box->append(connectBtn_);

  // network peers status line, right under the connect button: a dot (green when
  // peers are online, red at zero) + "{n} peers", always shown. Tapping opens the
  // location chooser (owned by the drawer).
  auto* peersRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  peersRow->set_halign(Gtk::Align::CENTER);
  peersStatusDot_.set_valign(Gtk::Align::CENTER);
  peersRow->append(peersStatusDot_);
  peersStatusText_.add_css_class("dim-label");
  peersRow->append(peersStatusText_);
  peersRow->add_css_class("ur-card-tappable");
  SetPointerCursor(*peersRow);
  auto peersGesture = Gtk::GestureClick::create();
  peersGesture->signal_released().connect(
      [this](int, double, double) { if (drawer_) drawer_->OpenLocationChooser(); });
  peersRow->add_controller(peersGesture);
  box->append(*peersRow);
  RefreshPeersStatus();

  // live stats (macOS parity): provider window size, throughput, provide
  providerCountLabel_.add_css_class("dim-label");
  throughputLabel_.add_css_class("dim-label");
  provideStatsLabel_.add_css_class("dim-label");
  box->append(providerCountLabel_);
  box->append(throughputLabel_);
  box->append(provideStatsLabel_);

  auto* provideRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  provideRow->set_halign(Gtk::Align::CENTER);
  provideRow->append(*Gtk::make_managed<Gtk::Label>(T_("provide", "Provide")));
  provideSwitch_.property_active().signal_changed().connect([this] {
    host_.SetProvideEnabled(provideSwitch_.get_active());
  });
  provideRow->append(provideSwitch_);
  box->append(*provideRow);

  // connect drawer (macOS ConnectActions parity): connection controls, the
  // three stats cards, the block-ads-and-trackers toggle, and the plan +
  // usage card (with the upgrade + redeem flows behind it)
  drawer_ = Gtk::make_managed<ConnectDrawer>(host_, *this, balance_);
  drawer_->on_create_account = [this] {
    // guest -> full account: the create page in upgrade-guest mode
    NavigateCreate(CreateNetworkPage::Mode::UpgradeGuest, "", /*fromHome=*/true);
  };
  box->append(*drawer_);

  box->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  auto* signOut = Gtk::make_managed<Gtk::Button>(T_("sign_out", "Sign out"));
  signOut->add_css_class("destructive-action");
  signOut->signal_clicked().connect([this] { host_.Logout(); });
  box->append(*signOut);

  auto* protocolLink = Gtk::make_managed<Gtk::LinkButton>(
      "https://ur.xyz", T_("uses_ur_protocol", "Uses the UR Protocol"));
  protocolLink->add_css_class("dim-label");
  box->append(*protocolLink);

  // AdwClamp caps the column at the drawer's max width (600, macOS parity)
  // while still shrinking with a narrow window; the scroller makes the taller
  // home view usable at any height.
  GtkWidget* clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 600);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 600);
  adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(box->gobj()));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_child(*Glib::wrap(clamp));

  // NOTE full-parity: location/provider picker, connection detail, account/wallet/
  // leaderboard surfaces attach here (ConnectViewController already drives them).
  // The wallet surface, when it lands, carries the Bittensor rules
  // (apple/NEXTSTEPS2.md §4): "Connect wallet" for Bittensor is paste-an-ss58-
  // address — no signature — via Api::createAccountWallet{blockchain: urnet::TAO}
  // (or WalletViewController::addExternalWallet, which accepts SOL|MATIC|TAO), and
  // TAO wallets can NEVER be the payout wallet: hide "Make default" for them and
  // show "Bittensor wallets are stored for future use and can't receive payouts
  // yet." (the server rejects TAO in SetPayoutWallet regardless).
  stack_.add(*scroller, "home");
}

// The sign-up / verify / password-reset pages (AuthViews.cpp) as stack
// children; each page reports back through callbacks and MainWindow routes.
void MainWindow::BuildAuthPages() {
  auto wrapInScroller = [](Gtk::Widget& page) {
    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 480);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 480);
    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(page.gobj()));
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(*Glib::wrap(clamp));
    return scroller;
  };

  createPage_ = Gtk::make_managed<CreateNetworkPage>(host_);
  createPage_->on_success = [this] {
    host_.StartTunnel();  // auth handler flips the view
  };
  createPage_->on_verify = [this](std::string userAuth) { NavigateVerify(userAuth); };
  createPage_->on_back = [this] {
    stack_.set_visible_child(createPageFromHome_ ? "home" : "login");
  };
  stack_.add(*wrapInScroller(*createPage_), "create");

  verifyPage_ = Gtk::make_managed<VerifyPage>(host_);
  verifyPage_->on_success = [this] {
    host_.StartTunnel();  // auth handler flips the view
  };
  verifyPage_->on_back = [this] { stack_.set_visible_child("login"); };
  stack_.add(*wrapInScroller(*verifyPage_), "verify");

  resetPage_ = Gtk::make_managed<ResetPasswordPage>(host_);
  resetPage_->on_back = [this] {
    // back to the password step when a discovery routed there (Windows parity)
    stack_.set_visible_child(loginUserAuth_.empty() ? "login" : "password");
  };
  stack_.add(*wrapInScroller(*resetPage_), "reset");
}

void MainWindow::NavigateCreate(CreateNetworkPage::Mode mode, const std::string& userAuth,
                                bool fromHome) {
  createPageFromHome_ = fromHome;
  createPage_->Configure(mode, userAuth);
  stack_.set_visible_child("create");
}

void MainWindow::NavigateVerify(const std::string& userAuth) {
  verifyPage_->Configure(userAuth);
  stack_.set_visible_child("verify");
}

// The email-first entry: discover the account for the user auth and route
// (Windows ApplyLoginRouting / mac LoginInitialViewModel.getStarted).
void MainWindow::OnGetStarted() {
  const std::string userAuth = TrimWhitespace(email_.get_text());
  if (discoveringLogin_ || !LooksLikeUserAuth(userAuth)) return;
  discoveringLogin_ = true;
  getStartedBtn_->set_sensitive(false);
  loginError_.set_text("");
  host_.StartLogin(userAuth, [this](LoginRouting routing) {
    PostToMain([this, routing] {
      discoveringLogin_ = false;
      getStartedBtn_->set_sensitive(true);
      switch (routing.route) {
        case LoginRoute::Login:
          host_.StartTunnel();  // auth handler flips the view
          break;
        case LoginRoute::Password:
          loginUserAuth_ = routing.userAuth;
          passwordUserAuth_.set_text(loginUserAuth_);
          password_.set_text("");
          passwordError_.set_text("");
          stack_.set_visible_child("password");
          password_.grab_focus();
          break;
        case LoginRoute::Create:
          // a new user: continue into sign-up with the user auth prefilled
          NavigateCreate(CreateNetworkPage::Mode::Password, routing.userAuth,
                         /*fromHome=*/false);
          break;
        case LoginRoute::Verify:
          NavigateVerify(routing.userAuth);
          break;
        case LoginRoute::IncorrectAuth:
          // the account exists under another sign-in method
          loginError_.set_text(
              Format(T_("login_error_auth_allowed", "Please login with one of: {}."),
                     routing.authAllowed));
          break;
        case LoginRoute::Error:
          loginError_.set_text(routing.error.empty()
                                   ? T_("there_was_an_error_logging_in",
                                        "There was an error logging in")
                                   : routing.error);
          break;
      }
    });
  });
}

// The password step's sign-in, against the discovered loginUserAuth_.
void MainWindow::OnSignIn() {
  const std::string password(password_.get_text());
  if (loginUserAuth_.empty() || password.empty()) return;
  passwordError_.set_text("");
  host_.LoginWithPassword(loginUserAuth_, password, [this](AuthResult r) {
    PostToMain([this, r] {
      if (r.verification_required) {
        // the login sent a fresh numeric code; route into the verify page
        // instead of dead-ending on a label
        NavigateVerify(loginUserAuth_);
      } else if (!r.ok) {
        passwordError_.set_text(r.error.empty() ? T_("sign_in_failed", "Sign in failed")
                                                : r.error);
      } else {
        host_.StartTunnel();  // auth handler flips the view
      }
    });
  });
}

void MainWindow::OnUseCode() {
  loginError_.set_text("");
  host_.LoginWithCode(code_.get_text(), [this](AuthResult r) {
    PostToMain([this, r] {
      if (!r.ok) {
        loginError_.set_text(r.error.empty() ? T_("code_sign_in_failed", "Code sign in failed")
                                             : r.error);
      } else {
        host_.StartTunnel();
      }
    });
  });
}

void MainWindow::OnGuestMode() {
  loginError_.set_text("");
  host_.LoginAsGuest([this](AuthResult r) {
    PostToMain([this, r] {
      if (!r.ok) {
        loginError_.set_text(r.error.empty() ? T_("guest_mode_failed", "Guest mode failed")
                                             : r.error);
      } else {
        host_.StartTunnel();
      }
    });
  });
}

void MainWindow::OnSolana(WalletConnect::Provider provider) {
  loginError_.set_text(T_("opening_wallet_in_browser", "Opening your wallet in the browser…"));
  host_.SignInWithSolana(provider, [this](AuthResult r) { OnWalletAuth(r); });
}

void MainWindow::OnBittensor() {
  loginError_.set_text(T_("opening_bittensor_wallet_in_browser",
                          "Opening your Bittensor wallet in the browser…"));
  host_.SignInWithBittensor([this](AuthResult r) { OnWalletAuth(r); });
}

// Shared tail of both wallet sign-ins (the SDK callback thread lands here).
void MainWindow::OnWalletAuth(const AuthResult& result) {
  PostToMain([this, result] {
    if (result.wallet_needs_network) {
      // wallet authenticated but has no network: the host kept the signed
      // wallet_auth; finish sign-up on the create page (name + terms)
      loginError_.set_text("");
      NavigateCreate(CreateNetworkPage::Mode::Wallet, "", /*fromHome=*/false);
    } else if (!result.ok) {
      loginError_.set_text(result.error.empty()
                               ? T_("wallet_sign_in_failed", "Wallet sign-in failed")
                               : result.error);
    } else {
      loginError_.set_text("");
      host_.StartTunnel();  // auth handler flips the view
    }
  });
}

void MainWindow::ApplyAuthState(bool loggedIn) {
  stack_.set_visible_child(loggedIn ? "home" : "login");
  // a fresh session (either way) re-arms the once-only Pro-upgrade provide
  // reset; the balance store's detection flag resets in Start()/Stop() below
  provideResetOnUpgrade_ = false;
  if (loggedIn) {
    provideSwitch_.set_active(host_.ProvideEnabled());
    SetConnected(host_.Connected());
    // (re)seed the balance/plan store from the (possibly new) jwt: login,
    // guest upgrade, and app start all land here
    balance_.SetWindowVisible(windowVisible_);
    balance_.Start();
  } else {
    SetConnected(false);
    balance_.Stop();
    // sign-out lands back on the initial step with a clean login flow
    loginUserAuth_.clear();
    password_.set_text("");
    passwordError_.set_text("");
    loginError_.set_text("");
  }
}

void MainWindow::ToggleConnect() {
  if (connected_) {
    host_.Disconnect();
  } else {
    host_.ConnectBestAvailable();
  }
  // status handler + SetConnected reflect the real state as it changes
}

void MainWindow::SetConnected(bool connected) {
  connected_ = connected;
  connectBtn_.set_label(connected ? T_("disconnect", "Disconnect") : T_("connect", "Connect"));
  if (on_connected_change) on_connected_change(connected);
}

void MainWindow::ApplyStats(const LiveStats& stats) {
  auto rate = [](int64_t bps) -> std::string {
    double v = static_cast<double>(bps);
    const char* u = "bps";
    if (v >= 1e9) { v /= 1e9; u = "Gbps"; }
    else if (v >= 1e6) { v /= 1e6; u = "Mbps"; }
    else if (v >= 1e3) { v /= 1e3; u = "Kbps"; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, u);
    return buf;
  };
  // the drawer surfaces the insufficient-balance banner (upgrade flow CTA)
  if (drawer_) drawer_->SetInsufficientBalance(stats.insufficientBalance);
  if (stats.insufficientBalance) {
    providerCountLabel_.set_text(T_("insufficient_balance_add_balance_or_plan",
                                    "Insufficient balance — add balance or a plan"));
    throughputLabel_.set_text("");
  } else if (stats.connected) {
    // the catalog carries the plural forms; never inflect here
    providerCountLabel_.set_text(
        Format(TN_("connected_provider_count", "Connected to {} provider",
                   "Connected to {} providers", static_cast<unsigned long>(stats.providerCount)),
               stats.providerCount));
    // arrows + rates: no translatable text
    throughputLabel_.set_text("↓ " + rate(stats.downBitsPerSecond) + "   ↑ " +
                              rate(stats.upBitsPerSecond));
  } else {
    providerCountLabel_.set_text("");
    throughputLabel_.set_text("");
  }
  std::string provide;
  if (stats.provideEnabled) {
    provide = stats.providePaused
                  ? std::string(T_("providing_paused", "Providing (paused)"))
                  : Format(TN_("providing_client_count", "Providing to {} client",
                               "Providing to {} clients",
                               static_cast<unsigned long>(stats.provideClients)),
                           stats.provideClients);
  }
  provideStatsLabel_.set_text(provide);
}

}  // namespace urnw
