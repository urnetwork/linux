// Runtime resolution of installed paths — the ONE idiom for this, shared by
// every consumer (gettext catalogs, the globe asset, the tray icon dir).
//
// WHY THIS EXISTS. meson bakes the configured install prefix into the binary
// (UR_LOCALEDIR, UR_PKGDATADIR). Those are correct for a normal install and
// WRONG inside a relocated AppImage, where the same tree lives under a
// per-run mount point exported as $APPDIR. A compile-time absolute path used
// directly is the silent-regression class in APPIMAGE.md §3: nothing errors,
// the file is simply never found — the AppImage ships the .mo catalogs and
// every user gets English regardless of locale.
//
// The ladder, in order:
//   1. $APPDIR + <installed path>   the AppImage runtime sets APPDIR
//   2. <installed path>             a normal system install
//   3. <build-tree fallback>        running straight out of a build dir
//
// A miss returns "" so each caller keeps its own miss behaviour (gettext
// binds the compile-time dir anyway; the globe drops its land layer; the tray
// falls back to the icon theme).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

#include <glib.h>

namespace urnw {

// Resolves `installedPath` (an absolute, compile-time path) against the
// ladder above. `test` selects what must exist: G_FILE_TEST_IS_REGULAR for a
// file, G_FILE_TEST_IS_DIR for a directory. `buildTreeFallback` may be empty.
// Returns the first existing candidate, or "" when none exists.
inline std::string ResolveRuntimePath(const std::string& installedPath, GFileTest test,
                                      const std::string& buildTreeFallback = std::string()) {
  std::vector<std::string> candidates;
  if (const char* appDir = g_getenv("APPDIR")) {
    candidates.push_back(std::string(appDir) + installedPath);
  }
  candidates.push_back(installedPath);
  if (!buildTreeFallback.empty()) candidates.push_back(buildTreeFallback);
  for (const auto& candidate : candidates) {
    if (g_file_test(candidate.c_str(), test)) return candidate;
  }
  return std::string();
}

}  // namespace urnw
