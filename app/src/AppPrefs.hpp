// Small persisted app preferences — the Linux twin of windows AppPrefs.h
// (%LOCALAPPDATA%\URnetwork\app\app_prefs.json): one JSON object at
// $XDG_CONFIG_HOME/urnetwork/app_prefs.json, read-modify-write of the WHOLE
// file on every set so keys never clobber each other. Known keys:
//   "advanced_mode"           bool   the D5 standing state
//   "onboarding_version_seen" int    replay gate for onboarding
// Header-only; nlohmann + glib only.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <fstream>
#include <string>

#include <glib.h>
#include <nlohmann/json.hpp>

namespace urnw::prefs {

inline std::string PrefsPath() {
  std::string dir = std::string(g_get_user_config_dir()) + "/urnetwork";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir + "/app_prefs.json";
}

inline nlohmann::json ReadAll() {
  std::ifstream in(PrefsPath());
  if (!in.good()) return nlohmann::json::object();
  try {
    nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_object()) return parsed;
  } catch (...) {
  }
  return nlohmann::json::object();
}

template <typename T>
inline T Get(const char* key, T fallback) {
  const nlohmann::json all = ReadAll();
  if (auto it = all.find(key); it != all.end()) {
    try {
      return it->get<T>();
    } catch (...) {
    }
  }
  return fallback;
}

template <typename T>
inline void Set(const char* key, const T& value) {
  nlohmann::json all = ReadAll();  // whole-file read-modify-write: keys never clobber
  all[key] = value;
  std::ofstream out(PrefsPath(), std::ios::trunc);
  out << all.dump(2) << "\n";
}

}  // namespace urnw::prefs
