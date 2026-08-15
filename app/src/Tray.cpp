// SPDX-License-Identifier: MPL-2.0
#include "Tray.hpp"

#include <unistd.h>

#include <string>

#include "I18n.hpp"
#include "RuntimePaths.hpp"

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
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewIcon"/>
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
enum : int { kIdConnect = 1, kIdSep = 2, kIdShow = 3, kIdQuit = 4 };

std::string ConnectLabel(bool connected) {
  return connected ? T_("disconnect", "Disconnect") : T_("connect", "Connect");
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
    return g_variant_new_string(self->connectedForIcon() ? "urnetwork-tray-connected"
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
    g_variant_builder_add(&kids, "v", BuildItem(kIdConnect, ConnectLabel(self->connectedForIcon()), false));
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
      if (id == kIdConnect && self->on_toggle_connect) self->on_toggle_connect();
      else if (id == kIdShow && self->on_show) self->on_show();
      else if (id == kIdQuit && self->on_quit) self->on_quit();
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
  service_name_ = "org.kde.StatusNotifierItem-" + std::to_string(::getpid()) + "-1";
  owner_id_ = g_bus_own_name(
      G_BUS_TYPE_SESSION, service_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
      +[](GDBusConnection* c, const gchar*, gpointer u) { static_cast<Tray*>(u)->OnBusAcquired(c); },
      +[](GDBusConnection*, const gchar*, gpointer u) { static_cast<Tray*>(u)->RegisterWithWatcher(); },
      +[](GDBusConnection* c, const gchar*, gpointer u) { static_cast<Tray*>(u)->OnNameLost(c); },
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
  RegisterWithWatcher();
}

void Tray::RegisterWithWatcher() {
  if (!conn_) return;
  g_dbus_connection_call(conn_, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                         "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                         g_variant_new("(s)", service_name_.c_str()), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

void Tray::SetConnected(bool connected) {
  connected_ = connected;
  if (!conn_) return;
  // Tell the host the icon changed and bump the menu so "Connect"/"Disconnect" refreshes.
  g_dbus_connection_emit_signal(conn_, nullptr, "/StatusNotifierItem",
                                "org.kde.StatusNotifierItem", "NewIcon", nullptr, nullptr);
  menu_revision_++;
  g_dbus_connection_emit_signal(conn_, nullptr, "/MenuBar", "com.canonical.dbusmenu",
                                "LayoutUpdated", g_variant_new("(ui)", menu_revision_, 0), nullptr);
}

Tray::~Tray() {
  if (conn_ && sni_reg_) g_dbus_connection_unregister_object(conn_, sni_reg_);
  if (conn_ && menu_reg_) g_dbus_connection_unregister_object(conn_, menu_reg_);
  if (owner_id_) g_bus_unown_name(owner_id_);
}

}  // namespace urnw
