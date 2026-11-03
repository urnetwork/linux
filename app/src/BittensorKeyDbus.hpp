// The D-Bus error name a GLib error carries, for the one-tap Bittensor key's
// Secret Service and polkit answers (BittensorKeyPlatform.hpp reads the name).
// GDBus maps the well-known D-Bus errors to G_DBUS_ERROR codes, and libsecret
// strips the remote name from some of its errors, so those are named from the
// code; any other remote error keeps its own name. "" for an error that never
// reached the bus (a timeout, a cancelled call, no bus).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gio/gio.h>
#include <glib.h>

#include <string>

namespace urnw::bittensor_key {

inline std::string DbusErrorName(const GError* error) {
  if (error == nullptr) return {};
  if (error->domain == G_DBUS_ERROR) {
    switch (error->code) {
      case G_DBUS_ERROR_SERVICE_UNKNOWN:
        return "org.freedesktop.DBus.Error.ServiceUnknown";
      case G_DBUS_ERROR_NAME_HAS_NO_OWNER:
        return "org.freedesktop.DBus.Error.NameHasNoOwner";
      case G_DBUS_ERROR_ACCESS_DENIED:
        return "org.freedesktop.DBus.Error.AccessDenied";
      case G_DBUS_ERROR_UNKNOWN_METHOD:
        return "org.freedesktop.DBus.Error.UnknownMethod";
      case G_DBUS_ERROR_UNKNOWN_INTERFACE:
        return "org.freedesktop.DBus.Error.UnknownInterface";
      case G_DBUS_ERROR_UNKNOWN_OBJECT:
        return "org.freedesktop.DBus.Error.UnknownObject";
      default:
        break;
    }
  }
  std::string name;
  if (gchar* remote = g_dbus_error_get_remote_error(error)) {
    name = remote;
    g_free(remote);
  }
  return name;
}

}  // namespace urnw::bittensor_key
