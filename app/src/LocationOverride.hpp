// Device-location override: report the oldest connected provider's coordinates
// as this machine's location, for everything that asks GeoClue.
//
// MECHANISM (verified against the GeoClue 2.7.x/2.8.x sources, see
// PROVIDERLOCATIONS.md and the notes on GeoClueWriter below): GeoClue's static
// source reads a fixed position from `/etc/geolocation` -- four lines, lat /
// lon / altitude / accuracy -- and live-monitors the file with a GFileMonitor,
// so a write is picked up without restarting anything. `[static-source]
// enable=true` is the shipped default (and the default when the key is absent).
// There is NO D-Bus setter and no per-user config path: the file is the only
// injection point and it lives under /etc, so a privileged writer is required.
//
// THE SEAM. The GUI must never need root. All privileged work goes through the
// abstract GeoClueWriter, so the GUI holds an interface and never touches /etc
// itself:
//
//   LocationOverrideController   <- what the UI talks to (this file)
//     └── GeoClueLocationOverride    owns the state machine + system probing
//           └── GeoClueWriter        <- THE PRIVILEGE SEAM
//                 ├── DirectGeoClueWriter   writes /etc/geolocation itself:
//                 │                         what urnetworkd (root) uses, and
//                 │                         behind the daemon's three
//                 │                         location_override_* verbs
//                 └── DaemonGeoClueWriter   the same three calls over the
//                                           urnetworkd control socket: what
//                                           the unprivileged GUI uses
//
// Swapping the writer was the whole daemon migration: nothing above the seam
// changed. With no daemon reachable, DaemonGeoClueWriter reports itself
// unavailable and the feature degrades to NeedsPrivilege (the setup guide's
// "install the URnetwork system service" step) rather than silently doing
// nothing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "LocationOverrideState.hpp"

namespace urnw {

class ControlClient;

// The privilege seam. Three calls, all of which may need root.
class GeoClueWriter {
 public:
  virtual ~GeoClueWriter() = default;

  // Whether this writer can currently perform a write. False makes the whole
  // feature report NeedsPrivilege; it must never be optimistic.
  virtual bool Available() = 0;

  // Replaces the static location. Coordinates are decimal degrees; accuracy is
  // in metres and must be > 0 (GeoClue discards a location whose accuracy is
  // the -1 "unknown" sentinel). Returns false on any failure.
  virtual bool Write(double lat, double lon, double accuracyMeters) = 0;

  // Removes the static location, restoring whatever GeoClue would otherwise
  // report. Returns false if the override is (or may still be) in place --
  // which is what raises the Orphaned status.
  virtual bool Clear() = 0;
};

// Writes /etc/geolocation directly. Available only when this process can
// actually write there — i.e. urnetworkd running as root; an ordinary user
// session gets Available() == false and the GUI never prompts for or
// requires root.
class DirectGeoClueWriter : public GeoClueWriter {
 public:
  bool Available() override;
  bool Write(double lat, double lon, double accuracyMeters) override;
  bool Clear() override;

  // The file GeoClue's static source reads. Public so the setup guide can name
  // it and the tests can reason about it.
  static const char* Path();

  // True when /etc/geolocation exists AND carries the URnetwork marker header
  // Write() emits. The daemon's startup cleaner keys on this: a marker-bearing
  // file surviving a crash/reboot is removed before any client is served, but
  // an admin's hand-written static location is never touched.
  static bool SystemOverridePresent();
};

// The unprivileged GUI's writer: the same three calls forwarded to urnetworkd
// over the control socket (location_override_available / _write / _clear).
// Available() is honest end to end — false whenever the daemon is
// unreachable, too old, or itself unable to write — so the state machine
// reports NeedsPrivilege instead of pretending.
class DaemonGeoClueWriter : public GeoClueWriter {
 public:
  // The client is shared with the SdkHost's tunnel control; it must outlive
  // this writer.
  explicit DaemonGeoClueWriter(ControlClient& control);

  bool Available() override;
  bool Write(double lat, double lon, double accuracyMeters) override;
  bool Clear() override;

 private:
  ControlClient& control_;
};

// What the UI talks to.
class LocationOverrideController {
 public:
  virtual ~LocationOverrideController() = default;

  // Whether the feature can be offered at all on this machine (GeoClue present
  // with a static source, and a writer). The toggle stays visible when false --
  // it routes into the setup guide, which explains what is missing.
  virtual bool Available() = 0;

  virtual LocationOverrideState State() = 0;

  // The persisted user toggle. Turning it on does NOT itself complete setup;
  // the caller opens the setup guide when State().setupComplete() is false
  // (android parity).
  virtual void SetEnabled(bool enabled) = 0;

  // The current sync inputs: whether the tunnel is up and which provider to
  // follow (the oldest connected provider that has coordinates). Passing no
  // target tears the override down -- we never report a city we are not
  // exiting through.
  virtual void SetTarget(bool tunnelUp, const LocationOverrideTarget* target) = 0;

  // Re-probe the system signals (GeoClue installed / static source enabled /
  // writer available). Cheap; call it when the guide or the sheet appears.
  virtual void RefreshSignals() = 0;

  // Fired on the GTK main loop whenever State() would return something new.
  std::function<void()> on_state_changed;
};

// The GeoClue implementation. Probing is read-only and unprivileged; every
// mutation goes through the writer.
class GeoClueLocationOverride : public LocationOverrideController {
 public:
  explicit GeoClueLocationOverride(std::unique_ptr<GeoClueWriter> writer);
  ~GeoClueLocationOverride() override;

  bool Available() override;
  LocationOverrideState State() override;
  void SetEnabled(bool enabled) override;
  void SetTarget(bool tunnelUp, const LocationOverrideTarget* target) override;
  void RefreshSignals() override;

  // Removes an override left behind by a previous run. MANDATORY at startup
  // and the direct analogue of android's startup cleaner: nothing on the system
  // ever reverts /etc/geolocation for us, so a crash or kill while active would
  // otherwise leave the machine reporting a provider's city indefinitely,
  // across reboots. Runs whenever the persisted "we wrote an override" marker
  // is set but the toggle is off.
  void CleanUpStaleOverride();

 private:
  void Apply();               // reconcile the written state with the inputs
  void Publish();             // recompute status + fire on_state_changed
  void LoadSettings();
  void SaveSettings();
  static std::string SettingsPath();

  std::unique_ptr<GeoClueWriter> writer_;
  LocationOverrideState state_;
  bool tunnelUp_ = false;
  bool hasTarget_ = false;
  LocationOverrideTarget target_;
  // set while an override we wrote is in place; persisted so a killed process
  // still leaves an exact cleanup instruction behind (android's lesson: write
  // the marker BEFORE the override, never after)
  bool overrideWritten_ = false;
  bool orphaned_ = false;
};

}  // namespace urnw
