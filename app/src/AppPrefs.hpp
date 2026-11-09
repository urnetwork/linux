// Small persisted app preferences — the Linux twin of windows AppPrefs.h
// (%LOCALAPPDATA%\URnetwork\app\app_prefs.json): one JSON object at
// $XDG_CONFIG_HOME/urnetwork/app_prefs.json, read-modify-write of the WHOLE
// file on every set so keys never clobber each other, serialized across
// threads and replaced atomically (PrefsFile.hpp). Known keys:
//   "advanced_mode"           bool   the D5 standing state
//   "onboarding_version_seen" int    replay gate for onboarding
//   "connect_on_launch"       bool   opt-in auto-connect, DEFAULT FALSE
//   "onboarding_pending"      bool   a network was just created here: show the
//                                    post-sign-up onboarding once (MainWindow)
//   "onb_tray_balloon_seen"   bool   the first hide to the tray was announced
//                                    (TrayPolicy.hpp)
//   "window_width", "window_height", "window_maximized"
//                             int, int, bool   the window's size across runs
//                                    (WindowGeometry.hpp)
//   "slow_internet_notify"    bool   the slow-internet notification, DEFAULT
//                                    TRUE (SlowInternet.hpp kNotifyPrefKey)
//   "dns_settings_mirrored"   bool   the GUI's dns settings mirror holds every
//                                    write since (kDnsSettingsMirroredKey)
// Header-only; nlohmann + glib only.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include <glib.h>
#include <nlohmann/json.hpp>

#include "PrefsFile.hpp"

namespace urnw::prefs {

// Launch behaviour, owned by the Settings "Connect automatically when the app
// starts" toggle and read once by MainWindow's constructor.
//
// DEFAULT FALSE, deliberately: a VPN that dials itself the moment it is opened
// is a decision the user never made. It lives HERE rather than behind the
// account preferences API because launch behaviour is per-device — a laptop and
// a desktop signed into one account want different answers — and because the
// constructor must decide without waiting on a network round trip.
//
// It governs the NEXT launch only. Nothing reads it after startup, so turning
// it on connects nothing now.
inline constexpr const char* kConnectOnLaunchKey = "connect_on_launch";

// Whether the traffic dns settings the GUI keeps in its LocalState mirror every
// write this app made since (SdkHost::SetDnsResolverSettingsLocked), so
// StartTunnel applies them to the daemon's device: a pick made with the tunnel
// down reaches it. False by default: a copy from before the mirror (the
// in-process device before the daemon split kept one) can be older than the
// daemon's own, and is never applied over it. A write the mirror missed
// clears it.
inline constexpr const char* kDnsSettingsMirroredKey = "dns_settings_mirrored";

inline std::string PrefsPath() {
  std::string dir = std::string(g_get_user_config_dir()) + "/urnetwork";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir + "/app_prefs.json";
}

inline nlohmann::json ReadAll() { return ReadAllAt(PrefsPath()); }

template <typename T>
inline T Get(const char* key, T fallback) {
  return ValueOr(ReadAll(), key, fallback);
}

template <typename T>
inline void Set(const char* key, const T& value) {
  const std::string path = PrefsPath();
  if (const int failure = SetAt(path, key, value)) {
    g_warning("prefs: could not save %s to %s: %s", key, path.c_str(), g_strerror(failure));
  }
}

// Several keys in one write (MergeAt), and none when nothing changed.
inline void SetAll(const nlohmann::json& values) {
  const std::string path = PrefsPath();
  if (const int failure = MergeAt(path, values)) {
    g_warning("prefs: could not save %s to %s: %s", values.dump().c_str(), path.c_str(),
              g_strerror(failure));
  }
}

}  // namespace urnw::prefs
