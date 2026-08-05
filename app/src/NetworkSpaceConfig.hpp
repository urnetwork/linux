// The URnetwork network-space configuration and device identity strings,
// shared by the GUI (urnetwork, SdkHost) and the daemon (urnetworkd,
// TunnelHost). Since the daemon-split (linux/MIGRATION.md) the two binaries
// build their NetworkSpace independently — the GUI for the api/auth surface,
// the daemon for its DeviceLocal — and they MUST agree on these values or the
// DeviceRemote would sync against a device registered in a different space.
// Header-only; needs the SDK (both consumers link it) but no GTK/glib.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <unistd.h>

#include <string>

#include <urnetwork_sdk.hpp>

namespace urnw {

inline constexpr const char* kUrHostName = "ur.network";
inline constexpr const char* kUrEnvName = "main";
// matches the -Dapp_version meson option's default; the release pipeline
// passes the real version
inline constexpr const char* kUrAppVersionFallback = "0.0.0";

// The initial device name defaults to the machine's hostname (the "device
// name"), e.g. "brien-thinkpad", falling back to a generic label. Users can
// still rename their device (a separate, server-side device_name); this is
// only the default a brand-new device registers with.
inline std::string UrDeviceDescription() {
  char hostname[256];
  if (::gethostname(hostname, sizeof(hostname)) == 0 && hostname[0] != '\0') {
    hostname[sizeof(hostname) - 1] = '\0';
    return std::string(hostname);
  }
  return "linux-desktop";
}

inline std::string UrDeviceSpec() {
#if defined(__aarch64__)
  return "linux arm64";
#else
  return "linux amd64";
#endif
}

// Builds (or refreshes) the app's network space in the given manager's
// storage. Idempotent: updateNetworkSpaceValues persists and returns the
// space for the fixed host/env key.
inline urnet::NetworkSpace BuildUrNetworkSpace(urnet::NetworkSpaceManager& manager) {
  urnet::NetworkSpaceKey key;
  key.host_name = std::string(kUrHostName);
  key.env_name = std::string(kUrEnvName);
  urnet::NetworkSpaceValues values;
  values.bundled = true;
  values.net_expose_server_ips = true;
  values.net_expose_server_host_names = true;
  values.link_host_name = "ur.io";
  values.migration_host_name = "bringyour.com";
  values.wallet = "circle";
  values.sso_google = false;
  return manager.updateNetworkSpaceValues(key, values);
}

}  // namespace urnw
