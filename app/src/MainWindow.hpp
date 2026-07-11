// GTK4 (gtkmm4) main window. Two views (sign-in / home) toggled by auth state;
// wired directly to the single-process SdkHost. SDK callbacks are marshaled onto
// the GTK main loop with g_idle_add (thread-safe). libadwaita widgets (AdwHeaderBar,
// AdwViewStack, C API) are layered in as the UI grows to full parity; this subset
// uses gtkmm widgets so it compiles standalone. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

// Post a callback onto the GTK main loop from any thread.
void PostToMain(std::function<void()> fn);

class MainWindow : public Gtk::ApplicationWindow {
 public:
  explicit MainWindow(SdkHost& host);

  void ToggleConnect();  // driven by the tray too
  bool connected() const { return connected_; }
  std::function<void(bool connected)> on_connected_change;

 private:
  void BuildLogin();
  void BuildHome();
  void OnSignIn();
  void OnUseCode();
  void OnGuestMode();
  void OnSolana(WalletConnect::Provider provider);
  void ApplyAuthState(bool loggedIn);
  void SetConnected(bool connected);
  void ApplyStats(const LiveStats& stats);  // live provider count / throughput / provide

  SdkHost& host_;
  Gtk::Stack stack_;

  Gtk::Entry email_;
  Gtk::PasswordEntry password_;
  Gtk::Entry code_;
  Gtk::Label loginError_;

  Gtk::Label status_{"Disconnected"};
  Gtk::Button connectBtn_{"Connect"};
  Gtk::Switch provideSwitch_;
  Gtk::Label providerCountLabel_;  // live stats (macOS parity)
  Gtk::Label throughputLabel_;
  Gtk::Label provideStatsLabel_;

  bool connected_ = false;
  // tray app: skip window-widget updates while hidden (resynced on show) so a
  // hidden window doesn't churn on high-frequency SDK updates
  bool windowVisible_ = false;
  std::string lastStatus_ = "Disconnected";
  LiveStats lastStats_;  // resynced into the widgets when the window is shown
};

}  // namespace urnw
