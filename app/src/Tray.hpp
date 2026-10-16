// System-tray icon via the StatusNotifierItem (SNI) spec, spoken directly over
// D-Bus with GDBus. We deliberately avoid libappindicator/ayatana because those
// pull GTK3 in-process for their menu — this app is GTK4-only, so we serve the
// SNI object and its com.canonical.dbusmenu ourselves. Falls back gracefully
// (no tray) on desktops without a StatusNotifierWatcher, or whose watcher has
// no host to draw the icons, and says so (Available): the window must not hide
// to a tray that is not there (TrayPolicy.hpp). The watcher's name is watched,
// so a tray host that starts after the app, or restarts, gets the icon again.
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

  // The connection as the window reads it: the Connect/Disconnect item
  // follows the session (`sessionUp`, so the label and a press agree), the
  // connected icon means a provider is proven to carry it, and the tooltip
  // names the state in the status row's words.
  // `hasStatus` is false while there is no connection status at all (signed
  // out): the menu then offers neither Connect nor Disconnect
  // (tray_policy::ConnectItemFor).
  void SetState(bool sessionUp, bool proven, const std::string& status, bool hasStatus);
  // Which recovery items the menu offers (failsafe_notice::TrayRecovery).
  void SetRecovery(bool forceTunnelOff, bool liftKillSwitch);
  // A StatusNotifierWatcher has taken the icon and says a host is registered
  // to draw it, so a click can bring the window back.
  bool Available() const { return registered_ && host_registered_; }

  std::function<void()> on_activate;         // left-click the tray icon
  std::function<void()> on_toggle_connect;   // menu: Connect/Disconnect
  std::function<void()> on_show;             // menu: Show window
  std::function<void()> on_quit;             // menu: Quit
  std::function<void()> on_force_tunnel_off;  // menu: Force the tunnel off (recovery)
  std::function<void()> on_lift_kill_switch;  // menu: Turn off the kill switch
  std::function<void(bool available)> on_availability_change;  // Available() flipped

  // Read by the D-Bus vtable callbacks (free functions in the .cpp).
  bool sessionUp() const { return session_up_; }
  bool hasStatus() const { return has_status_; }
  bool provenForIcon() const { return proven_; }
  const std::string& statusForToolTip() const { return status_; }
  guint menuRevision() const { return menu_revision_; }
  bool offersForceTunnelOff() const { return force_tunnel_off_; }
  bool offersLiftKillSwitch() const { return lift_kill_switch_; }

 private:
  void OnBusAcquired(GDBusConnection* conn);
  // The item has its bus name: register it once a watcher is there too.
  void OnItemNamed();
  void RegisterWithWatcher();
  static void OnRegistered(GObject* source, GAsyncResult* result, gpointer self);
  // Whether the watcher has a host (IsStatusNotifierHostRegistered).
  void ReadHostRegistered();
  static void OnHostRegisteredRead(GObject* source, GAsyncResult* result, gpointer self);
  void OnWatcherAppeared();
  void OnWatcherVanished();
  void SetRegistered(bool registered);
  void SetHostRegistered(bool hostRegistered);
  // Tells on_availability_change when Available() differs from `wasAvailable`.
  void NoteAvailability(bool wasAvailable);
  // Fallback when the well-known name cannot be owned (a Flatpak sandbox):
  // register with the watcher under the connection's UNIQUE name.
  void OnNameLost(GDBusConnection* conn);
  // Bumps the revision and tells the host to fetch the menu again.
  void EmitMenuLayoutUpdated();

  GDBusConnection* conn_ = nullptr;
  guint owner_id_ = 0;
  guint watcher_watch_id_ = 0;
  // the watcher's host signals and its property changes
  guint host_registered_sub_ = 0;
  guint host_unregistered_sub_ = 0;
  guint host_property_sub_ = 0;
  // cancelled on destruction, so a registration reply never reaches a freed tray
  GCancellable* cancellable_ = nullptr;
  bool item_named_ = false;      // service_name_ is ours to register
  bool watcher_present_ = false;  // org.kde.StatusNotifierWatcher has an owner
  bool registered_ = false;       // and it took the icon
  bool host_registered_ = false;  // and a host is there to draw it
  guint sni_reg_ = 0;
  guint menu_reg_ = 0;
  guint menu_revision_ = 1;
  std::string service_name_;  // org.kde.StatusNotifierItem-<pid>-1
  bool session_up_ = false;
  bool has_status_ = false;
  bool proven_ = false;
  std::string status_;
  bool force_tunnel_off_ = false;
  bool lift_kill_switch_ = false;

  static const GDBusInterfaceVTable kSniVtable;
  static const GDBusInterfaceVTable kMenuVtable;
};

}  // namespace urnw
