// SPDX-License-Identifier: MPL-2.0
#include "Tray.hpp"

#include <unistd.h>

#include <string>

#include "FailsafeNotice.hpp"
#include "I18n.hpp"
#include "RuntimePaths.hpp"
#include "TrayPolicy.hpp"

// Where the tray art is installed (meson passes the configured package data
// dir); the ladder in RuntimePaths.hpp prefixes $APPDIR inside an AppImage
// and falls back to the build tree.
#ifndef UR_PKGDATADIR
#define UR_PKGDATADIR "/usr/share/urnetwork"
#endif

namespace urnw {
namespace {

// Resolved once: the directory holding urnetwork-tray-*.png, handed to the
// SNI host as IconThemePath so it can resolve our bare IconName. Empty when
// the art is missing, which is the correct signal to the host ("use the
// theme") rather than a broken path.
const std::string& TrayIconThemePath() {
  static const std::string path = ResolveRuntimePath(UR_PKGDATADIR "/icons",
                                                     G_FILE_TEST_IS_DIR, "assets");
  return path;
}

// The desktop's tray, which takes the icon (RegisterStatusNotifierItem).
constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
// Whether a host is there to draw the icons the watcher took.
constexpr const char* kHostRegisteredProperty = "IsStatusNotifierHostRegistered";

// ---- interface definitions ------------------------------------------------

constexpr const char* kSniXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
</node>)XML";

constexpr const char* kMenuXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="Status" type="s" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <signal name="LayoutUpdated"><arg name="revision" type="u"/><arg name="parent" type="i"/></signal>
  </interface>
</node>)XML";

// Menu item ids (0 is the root).
enum : int {
  kIdConnect = 1,
  kIdSep = 2,
  kIdShow = 3,
  kIdQuit = 4,
  kIdRecoverySep = 5,
  kIdForceTunnelOff = 6,
  kIdLiftKillSwitch = 7,
};

std::string ConnectLabel(bool connected) {
  return connected ? T_("disconnect", "Disconnect") : T_("connect", "Connect");
}

// The connect item as the menu offers it now (tray_policy::ConnectItemFor).
tray_policy::ConnectItem ConnectItemOf(const Tray& tray) {
  return tray_policy::ConnectItemFor(tray.hasStatus(), tray.sessionUp());
}

GVariant* BuildItem(int id, const std::string& label, bool separator) {
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  if (separator) {
    g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
  } else {
    g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(label.c_str()));
    g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
  }
  GVariantBuilder children;  // leaf items have no children
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  return g_variant_new("(i@a{sv}@av)", id, g_variant_builder_end(&props),
                       g_variant_builder_end(&children));
}

}  // namespace

// ---- SNI vtable -----------------------------------------------------------

static void SniMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                      const gchar* method, GVariant*, GDBusMethodInvocation* inv, gpointer user) {
  auto* self = static_cast<Tray*>(user);
  if (g_strcmp0(method, "Activate") == 0 || g_strcmp0(method, "SecondaryActivate") == 0) {
    if (self->on_activate) self->on_activate();
  }
  g_dbus_method_invocation_return_value(inv, nullptr);
}

static GVariant* SniGetProp(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                            const gchar* prop, GError**, gpointer user) {
  auto* self = static_cast<Tray*>(user);
  if (g_strcmp0(prop, "Category") == 0) return g_variant_new_string("ApplicationStatus");
  if (g_strcmp0(prop, "Id") == 0) return g_variant_new_string("urnetwork");
  // Title is the product name (the store never translates it), not UI copy.
  if (g_strcmp0(prop, "Title") == 0) return g_variant_new_string("URnetwork");
  if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("Active");
  if (g_strcmp0(prop, "IconName") == 0)
    return g_variant_new_string(self->provenForIcon() ? "urnetwork-tray-connected"
                                                      : "urnetwork-tray-disconnected");
  // Where our tray PNGs actually live. IconName above is a bare name, so
  // without this the host can only resolve it from the icon THEME -- and our
  // art is installed to <pkgdatadir>/icons, not into hicolor, so the tray
  // silently falls back to a missing-image icon. The path must be resolved at
  // runtime for the same reason the catalogs are ($APPDIR relocation;
  // APPIMAGE.md §3b, the icon-staging item on §9's fix list).
  if (g_strcmp0(prop, "IconThemePath") == 0) {
    return g_variant_new_string(TrayIconThemePath().c_str());
  }
  if (g_strcmp0(prop, "Menu") == 0) return g_variant_new_object_path("/MenuBar");
  if (g_strcmp0(prop, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
  // (icon name, icon pixmaps, title, text): the product and the state's words
  if (g_strcmp0(prop, "ToolTip") == 0) {
    GVariant* noPixmaps = g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
    return g_variant_new("(s@a(iiay)ss)", "", noPixmaps, "URnetwork",
                         self->statusForToolTip().c_str());
  }
  return nullptr;
}

// ---- dbusmenu vtable ------------------------------------------------------

static void MenuMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                       const gchar* method, GVariant* params, GDBusMethodInvocation* inv,
                       gpointer user) {
  auto* self = static_cast<Tray*>(user);
  if (g_strcmp0(method, "GetLayout") == 0) {
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    // no status, no connection to start or end: no item (apple e46555b7)
    if (ConnectItemOf(*self) != tray_policy::ConnectItem::None) {
      g_variant_builder_add(&kids, "v",
                            BuildItem(kIdConnect, ConnectLabel(self->sessionUp()), false));
    }
    // The recovery items, apart from the everyday ones so they read as
    // recovery, and only while each is the answer to something.
    if (self->offersForceTunnelOff() || self->offersLiftKillSwitch()) {
      g_variant_builder_add(&kids, "v", BuildItem(kIdRecoverySep, "", true));
    }
    if (self->offersForceTunnelOff()) {
      const failsafe_notice::Copy copy = failsafe_notice::TrayRecovery::ForceTunnelOffCopy();
      g_variant_builder_add(&kids, "v",
                            BuildItem(kIdForceTunnelOff, T_(copy.key, copy.english), false));
    }
    if (self->offersLiftKillSwitch()) {
      const failsafe_notice::Copy copy = failsafe_notice::TrayRecovery::LiftKillSwitchCopy();
      g_variant_builder_add(&kids, "v",
                            BuildItem(kIdLiftKillSwitch, T_(copy.key, copy.english), false));
    }
    g_variant_builder_add(&kids, "v", BuildItem(kIdSep, "", true));
    g_variant_builder_add(&kids, "v",
                          BuildItem(kIdShow, T_("show_urnetwork", "Show URnetwork"), false));
    g_variant_builder_add(&kids, "v", BuildItem(kIdQuit, T_("quit", "Quit"), false));

    GVariantBuilder rootProps;
    g_variant_builder_init(&rootProps, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&rootProps, "{sv}", "children-display", g_variant_new_string("submenu"));
    GVariant* root = g_variant_new("(i@a{sv}@av)", 0, g_variant_builder_end(&rootProps),
                                   g_variant_builder_end(&kids));
    g_dbus_method_invocation_return_value(
        inv, g_variant_new("(u@(ia{sv}av))", self->menuRevision(), root));
    return;
  }
  if (g_strcmp0(method, "Event") == 0) {
    gint id = 0;
    const gchar* eventId = nullptr;
    GVariant* data = nullptr;
    guint32 ts = 0;
    g_variant_get(params, "(i&svu)", &id, &eventId, &data, &ts);
    if (g_strcmp0(eventId, "clicked") == 0) {
      // a click on a menu fetched before the item was withdrawn does nothing
      if (id == kIdConnect && ConnectItemOf(*self) != tray_policy::ConnectItem::None &&
          self->on_toggle_connect) {
        self->on_toggle_connect();
      } else if (id == kIdShow && self->on_show) self->on_show();
      else if (id == kIdQuit && self->on_quit) self->on_quit();
      // A click on a menu the host fetched before the item was withdrawn does
      // nothing: forcing the tunnel off must never reach a session the window
      // has since taken up.
      if (id == kIdForceTunnelOff && self->offersForceTunnelOff() && self->on_force_tunnel_off) {
        self->on_force_tunnel_off();
      }
      if (id == kIdLiftKillSwitch && self->offersLiftKillSwitch() && self->on_lift_kill_switch) {
        self->on_lift_kill_switch();
      }
    }
    if (data) g_variant_unref(data);
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (g_strcmp0(method, "AboutToShow") == 0) {
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    return;
  }
  if (g_strcmp0(method, "GetGroupProperties") == 0) {
    // Minimal: hosts that call this get an empty set and fall back to GetLayout.
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &b));
    return;
  }
  g_dbus_method_invocation_return_value(inv, nullptr);
}

static GVariant* MenuGetProp(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                             const gchar* prop, GError**, gpointer) {
  if (g_strcmp0(prop, "Version") == 0) return g_variant_new_uint32(3);
  if (g_strcmp0(prop, "Status") == 0) return g_variant_new_string("normal");
  return nullptr;
}

const GDBusInterfaceVTable Tray::kSniVtable = {SniMethod, SniGetProp, nullptr, {nullptr}};
const GDBusInterfaceVTable Tray::kMenuVtable = {MenuMethod, MenuGetProp, nullptr, {nullptr}};

// ---- lifecycle ------------------------------------------------------------

Tray::Tray() {
  cancellable_ = g_cancellable_new();
  service_name_ = "org.kde.StatusNotifierItem-" + std::to_string(::getpid()) + "-1";
  owner_id_ = g_bus_own_name(
      G_BUS_TYPE_SESSION, service_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
      +[](GDBusConnection* c, const gchar*, gpointer u) { static_cast<Tray*>(u)->OnBusAcquired(c); },
      +[](GDBusConnection*, const gchar*, gpointer u) { static_cast<Tray*>(u)->OnItemNamed(); },
      +[](GDBusConnection* c, const gchar*, gpointer u) { static_cast<Tray*>(u)->OnNameLost(c); },
      this, nullptr);
  // The watcher is the desktop's tray: it can come after the app (a session's
  // tray host starts late) and come back (a restarted plasmashell or
  // gnome-shell), and every new owner has to be told about the icon again.
  watcher_watch_id_ = g_bus_watch_name(
      G_BUS_TYPE_SESSION, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
      +[](GDBusConnection*, const gchar*, const gchar*, gpointer u) {
        static_cast<Tray*>(u)->OnWatcherAppeared();
      },
      +[](GDBusConnection*, const gchar*, gpointer u) {
        static_cast<Tray*>(u)->OnWatcherVanished();
      },
      this, nullptr);
}

void Tray::OnBusAcquired(GDBusConnection* conn) {
  conn_ = conn;
  GError* err = nullptr;
  if (GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kSniXml, &err)) {
    sni_reg_ = g_dbus_connection_register_object(conn, "/StatusNotifierItem", info->interfaces[0],
                                                 &kSniVtable, this, nullptr, nullptr);
    g_dbus_node_info_unref(info);
  }
  g_clear_error(&err);
  if (GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kMenuXml, &err)) {
    menu_reg_ = g_dbus_connection_register_object(conn, "/MenuBar", info->interfaces[0],
                                                  &kMenuVtable, this, nullptr, nullptr);
    g_dbus_node_info_unref(info);
  }
  g_clear_error(&err);
  // A host that comes or goes behind a watcher that stays: the spec's
  // StatusNotifierHostRegistered, KDE's StatusNotifierHostUnregistered, and
  // the property's change where the watcher announces it. Each is answered
  // by reading the property again.
  const auto onHostSignal = +[](GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                const gchar*, GVariant*, gpointer u) {
    static_cast<Tray*>(u)->ReadHostRegistered();
  };
  host_registered_sub_ = g_dbus_connection_signal_subscribe(
      conn, kWatcherName, kWatcherName, "StatusNotifierHostRegistered", kWatcherPath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, onHostSignal, this, nullptr);
  host_unregistered_sub_ = g_dbus_connection_signal_subscribe(
      conn, kWatcherName, kWatcherName, "StatusNotifierHostUnregistered", kWatcherPath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, onHostSignal, this, nullptr);
  host_property_sub_ = g_dbus_connection_signal_subscribe(
      conn, kWatcherName, "org.freedesktop.DBus.Properties", "PropertiesChanged", kWatcherPath,
      kWatcherName, G_DBUS_SIGNAL_FLAGS_NONE, onHostSignal, this, nullptr);
}

// The well-known name could not be owned. Inside a Flatpak that is not a
// failure, it is the rule: the sandbox's D-Bus policy can only grant a dotted
// subtree (`org.foo.*`), and the SNI item name is HYPHENATED
// (org.kde.StatusNotifierItem-<pid>-<id>), so no --own-name can ever match it.
// The StatusNotifierItem spec allows the registered service to be either a
// well-known name or the connection's unique name, and a unique name needs no
// ownership at all — so registering under it is what gets a sandboxed build a
// tray icon instead of none. A null connection means there is no session bus,
// which really is "run without a tray".
void Tray::OnNameLost(GDBusConnection* conn) {
  if (!conn) return;
  if (!conn_) OnBusAcquired(conn);
  const char* unique = g_dbus_connection_get_unique_name(conn);
  if (!unique) return;
  service_name_ = unique;
  OnItemNamed();
}

void Tray::OnItemNamed() {
  item_named_ = true;
  RegisterWithWatcher();
}

// Registered once the item has its name and a watcher is there, whichever
// comes last; the reply says whether the icon is on a tray.
void Tray::RegisterWithWatcher() {
  if (!conn_ || !item_named_ || !watcher_present_) return;
  g_dbus_connection_call(conn_, kWatcherName, kWatcherPath, kWatcherName,
                         "RegisterStatusNotifierItem",
                         g_variant_new("(s)", service_name_.c_str()), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, cancellable_, &Tray::OnRegistered, this);
}

void Tray::OnRegistered(GObject* source, GAsyncResult* result, gpointer self) {
  GError* error = nullptr;
  if (GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error)) {
    g_variant_unref(reply);
  }
  // cancelled: the tray is being destroyed, and `self` with it
  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_error_free(error);
    return;
  }
  auto* tray = static_cast<Tray*>(self);
  if (error) {
    g_warning("tray: the StatusNotifierWatcher refused the icon: %s", error->message);
    g_error_free(error);
    tray->SetRegistered(false);
    return;
  }
  // a reply from a watcher that has gone since says nothing about the tray
  if (!tray->watcher_present_) return;
  tray->SetRegistered(true);
  tray->ReadHostRegistered();
}

// A watcher can run with no host to draw what it took: KDE's lives in kded,
// so it stays while the panel's tray is removed or plasmashell is down.
void Tray::ReadHostRegistered() {
  if (!conn_ || !watcher_present_) return;
  g_dbus_connection_call(conn_, kWatcherName, kWatcherPath, "org.freedesktop.DBus.Properties",
                         "Get", g_variant_new("(ss)", kWatcherName, kHostRegisteredProperty),
                         G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, cancellable_,
                         &Tray::OnHostRegisteredRead, this);
}

void Tray::OnHostRegisteredRead(GObject* source, GAsyncResult* result, gpointer self) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  // cancelled: the tray is being destroyed, and `self` with it
  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_error_free(error);
    return;
  }
  auto* tray = static_cast<Tray*>(self);
  bool hostRegistered = false;
  if (reply) {
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
      hostRegistered = g_variant_get_boolean(value);
    }
    g_variant_unref(value);
    g_variant_unref(reply);
  } else {
    // no answer is no host: the window must not hide to an icon nobody draws
    g_warning("tray: could not read whether a tray host is registered: %s", error->message);
    g_error_free(error);
  }
  if (!tray->watcher_present_) return;
  tray->SetHostRegistered(hostRegistered);
}

void Tray::OnWatcherAppeared() {
  watcher_present_ = true;
  RegisterWithWatcher();
}

void Tray::OnWatcherVanished() {
  g_message("tray: no StatusNotifierWatcher on the session bus");
  watcher_present_ = false;
  SetRegistered(false);
  SetHostRegistered(false);
}

void Tray::SetRegistered(bool registered) {
  if (registered == registered_) return;
  const bool wasAvailable = Available();
  registered_ = registered;
  g_message("tray: the watcher %s the icon", registered ? "took" : "does not hold");
  NoteAvailability(wasAvailable);
}

void Tray::SetHostRegistered(bool hostRegistered) {
  if (hostRegistered == host_registered_) return;
  const bool wasAvailable = Available();
  host_registered_ = hostRegistered;
  g_message("tray: %s", hostRegistered ? "a tray host draws the icons"
                                       : "no tray host draws the icons");
  NoteAvailability(wasAvailable);
}

void Tray::NoteAvailability(bool wasAvailable) {
  const bool available = Available();
  if (available == wasAvailable) return;
  g_message("tray: the icon is %s the tray", available ? "on" : "off");
  if (on_availability_change) on_availability_change(available);
}

void Tray::SetState(bool sessionUp, bool proven, const std::string& status, bool hasStatus) {
  session_up_ = sessionUp;
  has_status_ = hasStatus;
  proven_ = proven;
  status_ = status;
  if (!conn_) return;
  // Tell the host the icon and the tooltip changed, and bump the menu so
  // "Connect"/"Disconnect" refreshes.
  g_dbus_connection_emit_signal(conn_, nullptr, "/StatusNotifierItem",
                                "org.kde.StatusNotifierItem", "NewIcon", nullptr, nullptr);
  g_dbus_connection_emit_signal(conn_, nullptr, "/StatusNotifierItem",
                                "org.kde.StatusNotifierItem", "NewToolTip", nullptr, nullptr);
  EmitMenuLayoutUpdated();
}

void Tray::SetRecovery(bool forceTunnelOff, bool liftKillSwitch) {
  if (forceTunnelOff == force_tunnel_off_ && liftKillSwitch == lift_kill_switch_) return;
  force_tunnel_off_ = forceTunnelOff;
  lift_kill_switch_ = liftKillSwitch;
  EmitMenuLayoutUpdated();
}

void Tray::EmitMenuLayoutUpdated() {
  menu_revision_++;
  if (!conn_) return;
  g_dbus_connection_emit_signal(conn_, nullptr, "/MenuBar", "com.canonical.dbusmenu",
                                "LayoutUpdated", g_variant_new("(ui)", menu_revision_, 0), nullptr);
}

Tray::~Tray() {
  g_cancellable_cancel(cancellable_);
  if (watcher_watch_id_) g_bus_unwatch_name(watcher_watch_id_);
  for (guint sub : {host_registered_sub_, host_unregistered_sub_, host_property_sub_}) {
    if (conn_ && sub) g_dbus_connection_signal_unsubscribe(conn_, sub);
  }
  if (conn_ && sni_reg_) g_dbus_connection_unregister_object(conn_, sni_reg_);
  if (conn_ && menu_reg_) g_dbus_connection_unregister_object(conn_, menu_reg_);
  if (owner_id_) g_bus_unown_name(owner_id_);
  g_object_unref(cancellable_);
}

}  // namespace urnw
