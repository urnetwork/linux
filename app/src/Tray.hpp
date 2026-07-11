// System-tray icon via the StatusNotifierItem (SNI) spec, spoken directly over
// D-Bus with GDBus. We deliberately avoid libappindicator/ayatana because those
// pull GTK3 in-process for their menu — this app is GTK4-only, so we serve the
// SNI object and its com.canonical.dbusmenu ourselves. Falls back gracefully
// (no tray) on desktops without a StatusNotifierWatcher.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <gio/gio.h>

namespace urnw {

class Tray {
 public:
  Tray();
  ~Tray();

  Tray(const Tray&) = delete;
  Tray& operator=(const Tray&) = delete;

  // Reflects VPN state in the icon + the Connect/Disconnect menu label.
  void SetConnected(bool connected);

  std::function<void()> on_activate;         // left-click the tray icon
  std::function<void()> on_toggle_connect;   // menu: Connect/Disconnect
  std::function<void()> on_show;             // menu: Show window
  std::function<void()> on_quit;             // menu: Quit

  // Read by the D-Bus vtable callbacks (free functions in the .cpp).
  bool connectedForIcon() const { return connected_; }
  guint menuRevision() const { return menu_revision_; }

 private:
  void OnBusAcquired(GDBusConnection* conn);
  void RegisterWithWatcher();
  void EmitMenuLayoutUpdated();

  GDBusConnection* conn_ = nullptr;
  guint owner_id_ = 0;
  guint sni_reg_ = 0;
  guint menu_reg_ = 0;
  guint menu_revision_ = 1;
  std::string service_name_;  // org.kde.StatusNotifierItem-<pid>-1
  bool connected_ = false;

  static const GDBusInterfaceVTable kSniVtable;
  static const GDBusInterfaceVTable kMenuVtable;
};

}  // namespace urnw
