// Pure state model for the device-location override (no GTK, no SDK, no glib)
// so the state resolution is unit testable standalone -- see
// tests/geometry_test.cpp. A port of the android MockLocationState.kt, with the
// android gates (developer options / mock app selection / location master
// switch) replaced by the linux ones (GeoClue present / static source enabled /
// a privileged writer reachable).
//
// The load-bearing lesson carried over from android: the state struct keeps the
// RAW setup signals alongside `status`. `status` collapses to Disabled whenever
// the toggle is off, so it cannot answer "is this machine set up?" -- which is
// exactly what the toggle and the setup guide must know while the feature is
// still off. Collapsing them broke both surfaces on android.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw {

enum class LocationOverrideStatus {
  // feature toggle off; invariant: no override file written by us is in place
  Disabled,

  // GeoClue is not installed on this system. It is not present by default
  // everywhere (notably not on stock Ubuntu), so this is a real, common state.
  NeedsGeoClue,

  // GeoClue is installed but its static source is disabled in geoclue.conf
  // ([static-source] enable=false), so the override file would be ignored.
  NeedsStaticSource,

  // No privileged writer: the override file lives under /etc, which the GUI
  // must never be able to write on its own. Either the urnetwork daemon is not
  // installed/running, or the process is unprivileged and no daemon answered.
  NeedsPrivilege,

  // all preconditions met; waiting for tunnel up + a located provider
  Eligible,

  // the override is written and tracks the oldest connected provider
  Active,

  // an override we wrote is (or may be) still in place and removing it failed
  // -- e.g. the daemon stopped, or privilege was lost, while active. This is
  // the direct analogue of android's ORPHANED and just as mandatory: nothing on
  // the system ever reverts /etc/geolocation for us, so a crash while active
  // would otherwise leave the machine reporting a provider's city forever,
  // across reboots. The flag clears only when a removal fully succeeds.
  Orphaned,

  // unexpected failure (e.g. a write error); torn down, retry allowed. Never
  // returned by ResolveLocationOverrideStatus -- the controller overlays it
  // while a retry is pending.
  ErrorTransient,
};

// The oldest connected provider with coordinates, i.e. where the device
// location is synced to while Active.
struct LocationOverrideTarget {
  std::string clientId;
  std::string label;
  double lat = 0;
  double lon = 0;
};

struct LocationOverrideState {
  LocationOverrideStatus status = LocationOverrideStatus::Disabled;
  bool enabled = false;
  bool hasTarget = false;
  LocationOverrideTarget target;

  // The raw setup signals, reported independently of `enabled`/`status`.
  bool geoclueInstalled = false;
  bool staticSourceEnabled = false;
  bool writerAvailable = false;

  // Diagnostic only -- deliberately NOT a gate and NOT part of setupComplete.
  // False means GeoClue is installed but predates 2.7.0, which is where the
  // static source was introduced (so Ubuntu 22.04 and Debian 12 can never
  // work). It only selects which explanation the setup guide shows for a
  // NeedsStaticSource status: "too old to support this" vs "turned off".
  bool staticSourceSupported = false;

  bool setupComplete() const {
    return geoclueInstalled && staticSourceEnabled && writerAvailable;
  }
};

// Resolves the user-visible status from the engine inputs.
//
// Orphaned wins over everything, including a disabled toggle: the flag means an
// override may still be in place with removal impossible, and the controller
// clears it only after a successful removal -- at which point a disabled toggle
// resolves to Disabled. The remaining gates apply in setup order: GeoClue
// present -> static source enabled -> privileged writer; then Active only while
// the tunnel is up and a located provider target exists, Eligible otherwise.
LocationOverrideStatus ResolveLocationOverrideStatus(bool enabled, bool geoclueInstalled,
                                                     bool staticSourceEnabled,
                                                     bool writerAvailable, bool tunnelUp,
                                                     bool hasTarget, bool orphaned);

// ---- /etc/geolocation file contents (pure; no glib) --------------------------

// THE UNINSTALL CONTRACT. The packaging's `postrm purge` and `uninstall.sh`
// delete /etc/geolocation ONLY when it carries this marker, so that a file an
// admin (or another tool) wrote is never destroyed. If a write stops emitting
// it, purge silently stops cleaning up and an uninstalling user keeps a
// spoofed system location PERMANENTLY -- nothing else on the system reverts
// that file. Never change this string without changing the packaging's check
// in lockstep; LocationOverrideStateTest pins it.
extern const char* const kGeolocationMarker;

// Renders the exact file GeoClue's static source parses: the marker + comment
// header, then four lines -- latitude, longitude, altitude (m), accuracy (m).
//
// Formatting is C-locale by construction (std::locale::classic), not by the
// caller's locale: under fr_FR/de_DE a comma decimal separator would make
// GeoClue reject every line. Pure, so the exact bytes are unit-testable
// without glib, /etc, or root.
std::string RenderGeolocationFileContents(double lat, double lon, double altitudeMeters,
                                          double accuracyMeters);

// True when `contents` is a geolocation file URnetwork authored (the marker is
// its first line). The daemon's startup cleaner keys on this so it removes
// only overrides we left behind.
bool GeolocationContentsAreOurs(const std::string& contents);

}  // namespace urnw
