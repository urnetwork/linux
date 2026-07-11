// SPDX-License-Identifier: MPL-2.0
#include "MainWindow.hpp"

#include <glib.h>

#include <cstdio>

namespace urnw {

void PostToMain(std::function<void()> fn) {
  // g_idle_add is documented thread-safe; the closure runs once on the GTK main loop.
  auto* heap = new std::function<void()>(std::move(fn));
  g_idle_add(
      +[](gpointer data) -> gboolean {
        auto* f = static_cast<std::function<void()>*>(data);
        (*f)();
        delete f;
        return G_SOURCE_REMOVE;
      },
      heap);
}

MainWindow::MainWindow(SdkHost& host) : host_(host) {
  set_title("URnetwork");
  set_default_size(420, 560);

  BuildLogin();
  BuildHome();
  set_child(stack_);

  // Track window visibility (tray app: closing hides to tray). Skip window-widget
  // updates while hidden and resync when shown, so a hidden window doesn't churn
  // on high-frequency SDK updates. Live stats should follow this same gate.
  property_visible().signal_changed().connect([this] {
    windowVisible_ = get_visible();
    if (windowVisible_) {
      status_.set_text(lastStatus_);
      SetConnected(host_.Connected());
      ApplyStats(lastStats_);
    }
  });

  // Auth-state transitions are marshaled onto the GTK loop and flip the view.
  host_.SetAuthStateHandler([this](bool loggedIn) {
    PostToMain([this, loggedIn] { ApplyAuthState(loggedIn); });
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

  auto* title = Gtk::make_managed<Gtk::Label>();
  title->set_markup("<span size='xx-large' weight='bold'>URnetwork</span>");
  box->append(*title);

  email_.set_placeholder_text("Email or phone");
  box->append(email_);

  password_.set_show_peek_icon(true);
  password_.property_placeholder_text() = "Password";
  box->append(password_);

  auto* signIn = Gtk::make_managed<Gtk::Button>("Sign in");
  signIn->add_css_class("suggested-action");
  signIn->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnSignIn));
  box->append(*signIn);

  loginError_.add_css_class("error");
  loginError_.set_wrap(true);
  box->append(loginError_);

  box->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  code_.set_placeholder_text("Auth code");
  box->append(code_);
  auto* useCode = Gtk::make_managed<Gtk::Button>("Sign in with code");
  useCode->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnUseCode));
  box->append(*useCode);

  // Guest mode (iOS/macOS parity, DESKTOP2 §2): one-tap throwaway network,
  // upgradeable to a full account later.
  auto* guest = Gtk::make_managed<Gtk::Button>("Try Guest Mode");
  guest->add_css_class("flat");
  guest->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnGuestMode));
  box->append(*guest);

  // Sign in with Solana (DESKTOP2 wallet-connect): opens the ur.io/wallet-connect
  // browser bridge; the wallet extension signs and returns via urnetwork://.
  box->append(*Gtk::make_managed<Gtk::Label>("Sign in with Solana"));
  auto* walletRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  walletRow->set_halign(Gtk::Align::CENTER);
  auto* phantom = Gtk::make_managed<Gtk::Button>("Phantom");
  phantom->signal_clicked().connect([this] { OnSolana(WalletConnect::Provider::Phantom); });
  auto* solflare = Gtk::make_managed<Gtk::Button>("Solflare");
  solflare->signal_clicked().connect([this] { OnSolana(WalletConnect::Provider::Solflare); });
  walletRow->append(*phantom);
  walletRow->append(*solflare);
  box->append(*walletRow);

  // NOTE full-parity: sign-up, Google/Apple browser SSO, verification flow, and
  // guest→full-account upgrade slot in here; the SdkHost/Api surface exists.
  stack_.add(*box, "login");
}

void MainWindow::BuildHome() {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  box->set_margin(24);
  box->set_valign(Gtk::Align::CENTER);

  status_.add_css_class("title-2");
  box->append(status_);

  connectBtn_.add_css_class("suggested-action");
  connectBtn_.add_css_class("pill");
  connectBtn_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::ToggleConnect));
  box->append(connectBtn_);

  // live stats (macOS parity): provider window size, throughput, provide
  providerCountLabel_.add_css_class("dim-label");
  throughputLabel_.add_css_class("dim-label");
  provideStatsLabel_.add_css_class("dim-label");
  box->append(providerCountLabel_);
  box->append(throughputLabel_);
  box->append(provideStatsLabel_);

  auto* provideRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  provideRow->set_halign(Gtk::Align::CENTER);
  provideRow->append(*Gtk::make_managed<Gtk::Label>("Provide"));
  provideSwitch_.property_active().signal_changed().connect([this] {
    host_.SetProvideEnabled(provideSwitch_.get_active());
  });
  provideRow->append(provideSwitch_);
  box->append(*provideRow);

  box->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  auto* signOut = Gtk::make_managed<Gtk::Button>("Sign out");
  signOut->add_css_class("destructive-action");
  signOut->signal_clicked().connect([this] { host_.Logout(); });
  box->append(*signOut);

  // NOTE full-parity: location/provider picker, connection detail, account/wallet/
  // leaderboard surfaces attach here (ConnectViewController already drives them).
  stack_.add(*box, "home");
}

void MainWindow::OnSignIn() {
  loginError_.set_text("");
  host_.LoginWithPassword(email_.get_text(), std::string(password_.get_text()), [this](AuthResult r) {
    PostToMain([this, r] {
      if (r.verification_required) {
        loginError_.set_text("This account needs verification. Check your email.");
      } else if (!r.ok) {
        loginError_.set_text(r.error.empty() ? "Sign in failed" : r.error);
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
        loginError_.set_text(r.error.empty() ? "Code sign in failed" : r.error);
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
        loginError_.set_text(r.error.empty() ? "Guest mode failed" : r.error);
      } else {
        host_.StartTunnel();
      }
    });
  });
}

void MainWindow::OnSolana(WalletConnect::Provider provider) {
  loginError_.set_text("Opening your wallet in the browser…");
  host_.SignInWithSolana(provider, [this](AuthResult r) {
    PostToMain([this, r] {
      if (!r.ok) {
        loginError_.set_text(r.error.empty() ? "Wallet sign-in failed" : r.error);
      } else {
        loginError_.set_text("");
        host_.StartTunnel();  // auth handler flips the view
      }
    });
  });
}

void MainWindow::ApplyAuthState(bool loggedIn) {
  stack_.set_visible_child(loggedIn ? "home" : "login");
  if (loggedIn) {
    provideSwitch_.set_active(host_.ProvideEnabled());
    SetConnected(host_.Connected());
  } else {
    SetConnected(false);
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
  connectBtn_.set_label(connected ? "Disconnect" : "Connect");
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
  if (stats.insufficientBalance) {
    providerCountLabel_.set_text("Insufficient balance — add balance or a plan");
    throughputLabel_.set_text("");
  } else if (stats.connected) {
    providerCountLabel_.set_text("Connected to " + std::to_string(stats.providerCount) +
                                 " provider" + (stats.providerCount == 1 ? "" : "s"));
    throughputLabel_.set_text("↓ " + rate(stats.downBitsPerSecond) + "   ↑ " +
                              rate(stats.upBitsPerSecond));
  } else {
    providerCountLabel_.set_text("");
    throughputLabel_.set_text("");
  }
  std::string provide;
  if (stats.provideEnabled) {
    provide = stats.providePaused
                  ? "Providing (paused)"
                  : "Providing to " + std::to_string(stats.provideClients) + " client" +
                        (stats.provideClients == 1 ? "" : "s");
  }
  provideStatsLabel_.set_text(provide);
}

}  // namespace urnw
