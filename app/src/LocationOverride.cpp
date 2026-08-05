// SPDX-License-Identifier: MPL-2.0
#include "LocationOverride.hpp"

#include <unistd.h>  // W_OK

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>

#include "ControlClient.hpp"

namespace urnw {
namespace {

// GeoClue's static-source file and its config, all compile-time constants in
// GeoClue itself (SYSCONFDIR based, and every distro configures SYSCONFDIR as
// /etc). There is no env var, config key or D-Bus call that relocates them.
constexpr const char* kGeolocationPath = "/etc/geolocation";
// The file's exact bytes (marker header + four lines) come from the pure
// RenderGeolocationFileContents in LocationOverrideState.cpp, so the marker
// the packaging's purge/uninstall greps for has ONE definition and is
// unit-testable without glib or /etc.
constexpr const char* kGeoClueConfigPath = "/etc/geoclue/geoclue.conf";
constexpr const char* kGeoClueConfigDir = "/etc/geoclue/conf.d";
constexpr const char* kStaticSourceGroup = "static-source";

// The accuracy we claim, in metres. GeoClue's arbitration prefers the smaller
// accuracy value unconditionally, and IP/Wi-Fi fixes report thousands of
// metres, so a small value wins -- but the number should stay honest: a
// provider's coordinates are a city or region centroid, not a GPS fix. It must
// be > 0: GeoClue discards a location whose accuracy is the -1 "unknown"
// sentinel, and 0 would additionally arm the priority-source lock semantics.
constexpr double kAccuracyMeters = 5000.0;
// GeoClue's static source reads exactly four lines; altitude is not something
// we know, and 0 is the conventional "sea level, unspecified" value.
constexpr double kAltitudeMeters = 0.0;

bool FileExists(const char* path) { return g_file_test(path, G_FILE_TEST_IS_REGULAR); }

// Reads `enable` for a group from a GeoClue config file. Returns true/false
// when the key is present, and leaves `out` untouched when it is not -- GeoClue
// treats a missing `enable` as enabled, and conf.d files override the main file
// only for the keys they actually carry.
void ReadEnableKey(const char* path, const char* group, bool* out, bool* groupSeen) {
  GKeyFile* keyFile = g_key_file_new();
  if (g_key_file_load_from_file(keyFile, path, G_KEY_FILE_NONE, nullptr)) {
    if (g_key_file_has_group(keyFile, group)) {
      if (groupSeen != nullptr) *groupSeen = true;
      GError* error = nullptr;
      const gboolean value = g_key_file_get_boolean(keyFile, group, "enable", &error);
      if (error == nullptr) {
        *out = value != FALSE;
      } else {
        g_error_free(error);  // group present, key absent -> GeoClue defaults to enabled
      }
    }
  }
  g_key_file_free(keyFile);
}

}  // namespace

// ---- DirectGeoClueWriter ----------------------------------------------------

const char* DirectGeoClueWriter::Path() { return kGeolocationPath; }

bool DirectGeoClueWriter::Available() {
  // Honest capability check: can this process actually replace the file?
  // Writing it means creating/renaming inside /etc, so what matters is write
  // access to the directory (plus the file itself when it already exists).
  // An ordinary desktop session fails both and the feature reports
  // NeedsPrivilege -- the GUI never asks for or needs root.
  if (g_access("/etc", W_OK) != 0) return false;
  if (FileExists(kGeolocationPath) && g_access(kGeolocationPath, W_OK) != 0) return false;
  return true;
}

bool DirectGeoClueWriter::Write(double lat, double lon, double accuracyMeters) {
  if (!Available()) return false;
  if (!(accuracyMeters > 0)) return false;  // -1/0 would be discarded by GeoClue

  // The exact bytes GeoClue's static source parses -- including the marker
  // line the packaging's purge/uninstall greps for. Rendered by the pure
  // helper (C-locale by construction), so the uninstall contract is pinned by
  // a unit test rather than by this call site.
  const std::string rendered =
      RenderGeolocationFileContents(lat, lon, kAltitudeMeters, accuracyMeters);

  // g_file_set_contents writes to a temporary in the same directory and
  // rename()s it into place. GeoClue's GFileMonitor turns that rename into a
  // synthetic CREATED + CHANGES_DONE_HINT, which is exactly the event it acts
  // on -- so the new position is picked up live, with no service restart.
  GError* error = nullptr;
  const gboolean ok = g_file_set_contents(kGeolocationPath, rendered.c_str(),
                                          static_cast<gssize>(rendered.size()), &error);
  if (!ok) {
    g_warning("location override: could not write %s: %s", kGeolocationPath,
              error != nullptr ? error->message : "unknown error");
    if (error != nullptr) g_error_free(error);
    return false;
  }
  // The GeoClue daemon runs as the unprivileged `geoclue` user on Debian,
  // Ubuntu, Fedora and Arch, so the file has to be world readable to be read
  // at all. It contains nothing secret -- it is a coarse city centroid.
  if (g_chmod(kGeolocationPath, 0644) != 0) {
    g_warning("location override: could not chmod %s", kGeolocationPath);
  }
  return true;
}

bool DirectGeoClueWriter::Clear() {
  if (!FileExists(kGeolocationPath)) return true;  // already gone
  if (!Available()) return false;
  // GeoClue watches for DELETED and drops the static location immediately.
  if (g_remove(kGeolocationPath) != 0) {
    g_warning("location override: could not remove %s", kGeolocationPath);
    return false;
  }
  return true;
}

bool DirectGeoClueWriter::SystemOverridePresent() {
  gchar* contents = nullptr;
  gsize length = 0;
  if (!g_file_get_contents(kGeolocationPath, &contents, &length, nullptr)) return false;
  const bool ours = GeolocationContentsAreOurs(std::string(contents, length));
  g_free(contents);
  return ours;
}

// ---- DaemonGeoClueWriter ----------------------------------------------------

DaemonGeoClueWriter::DaemonGeoClueWriter(ControlClient& control) : control_(control) {}

bool DaemonGeoClueWriter::Available() {
  // Honest end to end: the daemon must be reachable, version-compatible AND
  // itself able to write /etc. Every failure mode collapses to false, which
  // the state machine reports as NeedsPrivilege ("install the URnetwork
  // system service").
  bool available = false;
  std::string reason;
  if (!control_.LocationOverrideAvailable(&available, &reason)) return false;
  return available;
}

bool DaemonGeoClueWriter::Write(double lat, double lon, double accuracyMeters) {
  if (!(accuracyMeters > 0)) return false;  // -1/0 would be discarded by GeoClue
  return control_.LocationOverrideWrite(lat, lon, accuracyMeters);
}

bool DaemonGeoClueWriter::Clear() { return control_.LocationOverrideClear(); }

// ---- GeoClueLocationOverride ------------------------------------------------

GeoClueLocationOverride::GeoClueLocationOverride(std::unique_ptr<GeoClueWriter> writer)
    : writer_(std::move(writer)) {
  LoadSettings();
  RefreshSignals();
  // an override left by a previous run is removed before anything else
  CleanUpStaleOverride();
}

GeoClueLocationOverride::~GeoClueLocationOverride() {
  // Best effort: a clean shutdown must not leave the machine reporting a
  // provider's location. A crash still can, which is what the startup cleaner
  // and the Orphaned status exist for.
  if (overrideWritten_ && writer_) {
    if (writer_->Clear()) {
      overrideWritten_ = false;
      SaveSettings();
    }
  }
}

std::string GeoClueLocationOverride::SettingsPath() {
  char* dir = g_build_filename(g_get_user_config_dir(), "urnetwork", nullptr);
  g_mkdir_with_parents(dir, 0700);
  char* path = g_build_filename(dir, "location-override.conf", nullptr);
  std::string out = path;
  g_free(path);
  g_free(dir);
  return out;
}

void GeoClueLocationOverride::LoadSettings() {
  GKeyFile* keyFile = g_key_file_new();
  if (g_key_file_load_from_file(keyFile, SettingsPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
    state_.enabled = g_key_file_get_boolean(keyFile, "override", "enabled", nullptr) != FALSE;
    overrideWritten_ =
        g_key_file_get_boolean(keyFile, "override", "written", nullptr) != FALSE;
    orphaned_ = g_key_file_get_boolean(keyFile, "override", "orphaned", nullptr) != FALSE;
  }
  g_key_file_free(keyFile);
}

void GeoClueLocationOverride::SaveSettings() {
  GKeyFile* keyFile = g_key_file_new();
  g_key_file_set_boolean(keyFile, "override", "enabled", state_.enabled ? TRUE : FALSE);
  g_key_file_set_boolean(keyFile, "override", "written", overrideWritten_ ? TRUE : FALSE);
  g_key_file_set_boolean(keyFile, "override", "orphaned", orphaned_ ? TRUE : FALSE);
  g_key_file_save_to_file(keyFile, SettingsPath().c_str(), nullptr);
  g_key_file_free(keyFile);
}

void GeoClueLocationOverride::RefreshSignals() {
  // GeoClue installed at all: its config file is what the package ships.
  state_.geoclueInstalled = FileExists(kGeoClueConfigPath) ||
                            g_file_test(kGeoClueConfigDir, G_FILE_TEST_IS_DIR);

  // The static source exists only from GeoClue 2.7.0, and the shipped
  // geoclue.conf gained its [static-source] group in the same release -- so the
  // group's presence in the MAIN file is the practical version signal. Its
  // absence is why Ubuntu 22.04 (2.5.7) and Debian 12 (2.6.0) can never work.
  bool groupSeen = false;
  bool enabled = true;  // GeoClue defaults a source to enabled when `enable` is absent
  ReadEnableKey(kGeoClueConfigPath, kStaticSourceGroup, &enabled, &groupSeen);
  state_.staticSourceSupported = groupSeen;

  // conf.d/*.conf are loaded after the main file, sorted by name, later
  // overriding earlier -- mirror that so an admin's override is respected.
  if (GDir* dir = g_dir_open(kGeoClueConfigDir, 0, nullptr)) {
    std::vector<std::string> names;
    while (const char* name = g_dir_read_name(dir)) {
      if (g_str_has_suffix(name, ".conf")) names.push_back(name);
    }
    g_dir_close(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      char* path = g_build_filename(kGeoClueConfigDir, name.c_str(), nullptr);
      bool seenHere = false;
      ReadEnableKey(path, kStaticSourceGroup, &enabled, &seenHere);
      if (seenHere) state_.staticSourceSupported = true;
      g_free(path);
    }
  }
  state_.staticSourceEnabled = state_.staticSourceSupported && enabled;

  state_.writerAvailable = writer_ && writer_->Available();
  Publish();
}

bool GeoClueLocationOverride::Available() {
  return state_.geoclueInstalled && state_.staticSourceEnabled && state_.writerAvailable;
}

LocationOverrideState GeoClueLocationOverride::State() { return state_; }

void GeoClueLocationOverride::SetEnabled(bool enabled) {
  if (state_.enabled == enabled) return;
  state_.enabled = enabled;
  SaveSettings();
  Apply();
}

void GeoClueLocationOverride::SetTarget(bool tunnelUp, const LocationOverrideTarget* target) {
  const bool hasTarget = target != nullptr;
  if (tunnelUp == tunnelUp_ && hasTarget == hasTarget_ &&
      (!hasTarget || (target->clientId == target_.clientId && target->lat == target_.lat &&
                      target->lon == target_.lon))) {
    return;  // nothing that affects the override changed
  }
  tunnelUp_ = tunnelUp;
  hasTarget_ = hasTarget;
  if (hasTarget) target_ = *target;
  Apply();
}

void GeoClueLocationOverride::Apply() {
  // Active only while: toggle on AND setup complete AND tunnel up AND a located
  // provider exists. Anything else tears the override down -- we never report a
  // city we are not exiting through.
  const bool shouldBeActive = writer_ && state_.enabled && state_.setupComplete() && tunnelUp_ &&
                              hasTarget_ && !orphaned_;

  if (shouldBeActive) {
    // Persist the marker BEFORE writing, so a process killed mid-write still
    // leaves an exact cleanup instruction behind (the android lesson).
    if (!overrideWritten_) {
      overrideWritten_ = true;
      SaveSettings();
    }
    // A provider change is a teleport, which is expected of a VPN: just write
    // the new position. GeoClue picks it up from the file monitor.
    if (!writer_->Write(target_.lat, target_.lon, kAccuracyMeters)) {
      state_.status = LocationOverrideStatus::ErrorTransient;
      if (on_state_changed) on_state_changed();
      return;
    }
  } else if (overrideWritten_) {
    if (writer_ && writer_->Clear()) {
      overrideWritten_ = false;
      orphaned_ = false;
    } else {
      // We wrote an override and can no longer remove it: the machine is stuck
      // reporting a provider's location and the user needs the recovery
      // instructions. Cleared only by a later successful removal.
      orphaned_ = true;
    }
    SaveSettings();
  }
  Publish();
}

void GeoClueLocationOverride::CleanUpStaleOverride() {
  if (!overrideWritten_) {
    // Nothing of ours is in place. If the toggle is off there is nothing to do;
    // if it is on, Apply() will write when a target arrives.
    Publish();
    return;
  }
  if (state_.enabled) {
    // The toggle survived the restart; Apply() rewrites the position as soon as
    // the tunnel and a target are known. Leaving the stale file in place until
    // then would report a provider we are no longer using, so clear it now.
    if (writer_ && writer_->Clear()) {
      overrideWritten_ = false;
      orphaned_ = false;
      SaveSettings();
    }
    Publish();
    return;
  }
  if (writer_ && writer_->Clear()) {
    overrideWritten_ = false;
    orphaned_ = false;
  } else {
    orphaned_ = true;
  }
  SaveSettings();
  Publish();
}

void GeoClueLocationOverride::Publish() {
  state_.hasTarget = hasTarget_;
  // never report a stale target alongside hasTarget == false
  state_.target = hasTarget_ ? target_ : LocationOverrideTarget{};
  state_.status = ResolveLocationOverrideStatus(state_.enabled, state_.geoclueInstalled,
                                                state_.staticSourceEnabled,
                                                state_.writerAvailable, tunnelUp_, hasTarget_,
                                                orphaned_);
  if (on_state_changed) on_state_changed();
}

}  // namespace urnw
