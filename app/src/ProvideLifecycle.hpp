// Whether this device provides, connected or not, for each provide control
// mode — the one lifecycle decision the GUI and the daemon both apply to the
// provider-only device (support inbox 1521, P008: providers get no traffic).
//
// The defect this answers. Since the daemon split the provider is the daemon's
// DeviceLocal, and that device was built only inside a tunnel session
// (TunnelHost::RunStart). Disconnect sends stop_tunnel (it has to: the capture
// routes would otherwise blackhole the machine, linux 1dbec0d fix 3), and
// stop_tunnel destroys the DeviceLocal, so on Linux the provider stopped in
// every mode the moment the user disconnected — and never started at all after
// a launch without auto-connect. A user who picked "Always" earned only while
// connected and was told nothing. The single-process app before the split did
// not do this: its Disconnect left the device running.
//
// What the other platforms do. Android and Apple keep their packet tunnel, and
// with it the DeviceLocal, running whenever providing is enabled
// (VpnServiceStartPolicy.kt vpnServiceRequired, VPNReconciliationSupport.swift
// VPNDesiredState.shouldRun: provideEnabled || connectEnabled || !routeLocal),
// and the sdk decides the tier. Linux keeps the DeviceLocal without the tunnel:
// while disconnected the daemon runs a provider-only device with no tun, no
// capture routes, no DNS change, no nftables change and no device RPC listener,
// so providing never changes how this machine's own traffic is routed.
//
// The SDK semantics, mirrored and never reinterpreted
// (DeviceLocal.applyProvideControlModeWithLock, sdk/device_local.go):
//   never   -> no providing
//   always  -> public
//   network -> network (the user's own devices), connected or not
//   auto    -> public while connected, network while not
//   anything else -> no providing (the sdk's conservative default)
// "manual" defers to an explicitly set provide mode that lives in the device,
// which does not exist while disconnected; this app never offers it (the
// Connect page and onboarding offer auto/always/network/never), so it is
// treated like any other unknown mode and gets no provider-only device.
//
// Pure and header-only (C++17, no GTK, glib or SDK): tests/ProvideLifecycleTest.cpp
// runs it on any host, ControlProtocol.hpp validates start_provider with it and
// TunnelHost retires the provider-only device with it. The tail of the file
// paces how the GUI reads that device's statistics (provider_stats) and how
// long the daemon keeps polling its provider status, and decides where the
// Extender switch reads and writes its setting while disconnected.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace urnw::provide {

enum class ControlMode { Never, Always, Network, Auto, Unknown };

constexpr ControlMode ControlModeFrom(std::string_view mode) {
  if (mode == "never") return ControlMode::Never;
  if (mode == "always") return ControlMode::Always;
  if (mode == "network") return ControlMode::Network;
  if (mode == "auto") return ControlMode::Auto;
  return ControlMode::Unknown;
}

// For logs: the mode's own name, never the raw string a client sent.
constexpr const char* ToString(ControlMode mode) {
  switch (mode) {
    case ControlMode::Never: return "never";
    case ControlMode::Always: return "always";
    case ControlMode::Network: return "network";
    case ControlMode::Auto: return "auto";
    case ControlMode::Unknown: break;
  }
  return "unknown";
}

// The live provide tier, with the sdk's values (urnet::ProvideModeNone,
// ProvideModeNetwork, ProvideModePublic), so a tier and a status field compare
// directly.
enum class Tier : int64_t { None = 0, Network = 1, Public = 3 };

// The tier the sdk applies for `mode`. `connected` is the sdk's own test: a
// destination is set on a tunnel session.
constexpr Tier SdkTierFor(ControlMode mode, bool connected) {
  switch (mode) {
    case ControlMode::Always:
      return Tier::Public;
    case ControlMode::Network:
      return Tier::Network;
    case ControlMode::Auto:
      return connected ? Tier::Public : Tier::Network;
    case ControlMode::Never:
    case ControlMode::Unknown:
      break;
  }
  return Tier::None;
}

// The lifecycle decision: does this device provide? Connected, the tunnel
// session's DeviceLocal is the provider. Disconnected, a provider-only device
// runs exactly when the answer is true.
constexpr bool ProviderRuns(ControlMode mode, bool connected) {
  return SdkTierFor(mode, connected) != Tier::None;
}

// What the daemon's `status` says, as the GUI's disconnected step reads it.
// providerControlMode points into the status it was read from.
struct DaemonProviderFacts {
  // A status answered at all. Without one nothing changes.
  bool answered = false;
  // The status was cut down for another user: their session, not ours.
  bool redacted = false;
  // A tunnel session is starting, up or stopping: its DeviceLocal provides.
  bool tunnelSession = false;
  // The kill-switch floor holds this machine blocked after an unexpected drop.
  bool killSwitchArmed = false;
  bool providerRunning = false;
  // A control client owns the session (status owner_connected).
  bool ownerConnected = false;
  std::string_view providerControlMode;
};

enum class DisconnectedStep {
  None,
  // start_provider. Also how the GUI adopts a provider whose owner went away
  // (a GUI restart): the daemon keeps a device built from the same request.
  Start,
  // set_provide: the running provider follows the newly chosen mode.
  Apply,
  // set_provide with a mode that does not provide: the daemon retires the
  // provider-only device.
  Stop,
};

// The step the GUI takes while it has no tunnel session. `settingsChanged`:
// the provider transport policy or the network space was just edited, so the
// request goes out again: the daemon applies a policy to the running device
// in place, and rebuilds the device for another space.
constexpr DisconnectedStep DisconnectedProviderStep(std::string_view controlMode,
                                                    const DaemonProviderFacts& facts,
                                                    bool settingsChanged = false) {
  if (!facts.answered || facts.redacted || facts.tunnelSession) return DisconnectedStep::None;
  if (!ProviderRuns(ControlModeFrom(controlMode), /*connected=*/false)) {
    return facts.providerRunning ? DisconnectedStep::Stop : DisconnectedStep::None;
  }
  // The armed floor stays the only thing in force until the user reconnects or
  // lifts it; providing resumes then. The daemon refuses a start here as well.
  if (facts.killSwitchArmed) return DisconnectedStep::None;
  if (!facts.providerRunning || !facts.ownerConnected || settingsChanged) {
    return DisconnectedStep::Start;
  }
  if (facts.providerControlMode != controlMode) return DisconnectedStep::Apply;
  return DisconnectedStep::None;
}

// Pacing for a provider step the daemon refused or could not run, so the
// 5 s health poll cannot turn a failure into a construction loop: 5 s, then
// doubling to 5 minutes. A user action starts over (NoteSuccess).
class ProviderStepBackoff {
 public:
  static constexpr int64_t kFirstDelayMillis = 5 * 1000;
  static constexpr int64_t kMaxDelayMillis = 5 * 60 * 1000;

  bool Allows(int64_t nowMillis) const { return nowMillis >= notBeforeMillis_; }

  void NoteFailure(int64_t nowMillis) {
    delayMillis_ =
        delayMillis_ == 0 ? kFirstDelayMillis : std::min(delayMillis_ * 2, kMaxDelayMillis);
    notBeforeMillis_ = nowMillis + delayMillis_;
  }

  void NoteSuccess() {
    delayMillis_ = 0;
    notBeforeMillis_ = 0;
  }

  int64_t DelayMillis() const { return delayMillis_; }

 private:
  int64_t delayMillis_ = 0;
  int64_t notBeforeMillis_ = 0;
};

// ---- the daemon's half: one device across connect and disconnect ----------
//
// The provider-only device and a tunnel session's device carry one identity:
// the persisted key material and the request's credential, space, instance
// and version. A Connect used to retire the provider-only device and build the
// session a new DeviceLocal, and a Disconnect closed that one for the GUI's
// start_provider to build a new provider-only device. Every switch closed at
// once everything the old device held: its platform transports, its network
// peers and every contract, each close posted to the api by itself. On a busy
// provider that is thousands of posts in a few seconds, drawing 429s whose
// retries dial again. So the device now moves between the two roles whenever
// the identity allows, and only what belongs to a session (the tun, the
// routes, the rpc listener, the destination) comes and goes with it.

// The request fields a DeviceLocal is built from, as start_provider and
// start_tunnel both carry them. The credential counts by the client it names
// (ctl::JwtSubject), never by the token: the GUI refreshes its token all the
// time, so two requests for one device rarely carry the same one. The views
// point into the request and are valid while it lives.
struct DeviceIdentity {
  std::string jwtSubject;
  std::string_view networkSpaceJson;
  std::string_view instanceId;
  std::string_view appVersion;
};

inline bool SameIdentity(const DeviceIdentity& a, const DeviceIdentity& b) {
  return a.jwtSubject == b.jwtSubject && a.networkSpaceJson == b.networkSpaceJson &&
         a.instanceId == b.instanceId && a.appVersion == b.appVersion;
}

// The fields that tell two identities apart, by request field name only: the
// values are credentials. Empty exactly when SameIdentity holds. The daemon
// logs it when a device under this identity is rebuilt rather than kept.
inline std::string DifferingIdentityFields(const DeviceIdentity& a, const DeviceIdentity& b) {
  std::string names;
  const auto note = [&names](bool differs, std::string_view name) {
    if (!differs) return;
    if (!names.empty()) names += ", ";
    names += name;
  };
  note(a.jwtSubject != b.jwtSubject, "by_jwt");
  note(a.networkSpaceJson != b.networkSpaceJson, "network_space_json");
  note(a.instanceId != b.instanceId, "instance_id");
  note(a.appVersion != b.appVersion, "app_version");
  return names;
}

// Connect: the running provider-only device becomes the session's device when
// it was built for the identity the start carries and every socket it ever
// made carries the egress mark (`markedFromFirst`: the daemon's socket marker
// was attached before the device was built and has stayed attached). A socket
// made unmarked would follow the session's capture routes into the tun.
// Otherwise it is retired and the session builds its own, as before.
inline bool PromotesProviderDevice(bool providerDevice, bool markedFromFirst,
                                   const DeviceIdentity& provider, const DeviceIdentity& start) {
  return providerDevice && markedFromFirst && SameIdentity(provider, start);
}

// Disconnect: what the daemon knows when a session ends.
struct SessionStopFacts {
  // An up session holds a DeviceLocal.
  bool sessionDevice = false;
  // The owning user's Disconnect (stop_tunnel with disconnect). Not a quit, a
  // sign-out, a failed or replaced start, the failsafe, another user's stop or
  // the daemon's own: each of those closes the device, as before.
  bool disconnect = false;
  // The device's own provide control mode.
  ControlMode mode = ControlMode::Unknown;
  // After the machine is given back: the nftables policy still in force (the
  // kill switch's floor, or a removal that failed). start_provider refuses
  // under the armed floor; a kept device follows the same rule.
  bool firewallInForce = false;
};

// The session's device stays, as the provider-only device, exactly when a
// provider-only device could run in its place now: a mode that provides while
// disconnected, with nothing of the firewall left in force.
constexpr bool KeepsSessionDevice(const SessionStopFacts& facts) {
  return facts.sessionDevice && facts.disconnect && !facts.firewallInForce &&
         ProviderRuns(facts.mode, /*connected=*/false);
}

// ---- the provider-only device's statistics (provider_stats) ----------------
// The provider-only device has no DeviceRemote, so the GUI's provider
// statistics (the Earnings plots, the "no traffic yet" line, the provider
// status) come from the daemon, which runs the SDK's view controllers on it.

// What the GUI's poll does on a tick, while the Earnings or the connect
// destination is on screen. A tunnel session's DeviceRemote is the source while
// one is bound, and with no provider-only device there is nothing to read, so
// the daemon's snapshot is dropped then. A daemon that answered that it
// predates the verb is not asked again on the same connection.
enum class ProviderStatsStep {
  Drop,   // forget the daemon's snapshot
  Fetch,  // provider_stats
};

constexpr ProviderStatsStep DaemonProviderStatsStep(bool sessionDevice, bool providerRunning,
                                                    bool verbUnsupported) {
  if (sessionDevice || !providerRunning || verbUnsupported) return ProviderStatsStep::Drop;
  return ProviderStatsStep::Fetch;
}

// The daemon's provider status controller polls the API only while a GUI that
// shows the provider status keeps asking (provider_stats with poll_status,
// about once a second): each such request renews the lease, and the reaper
// stops the controller once it runs out, so a GUI that quit or crashed leaves
// nothing polling. A new controller (a rebuilt device) starts over (Release).
class ProviderStatusLease {
 public:
  static constexpr int64_t kLeaseMillis = 15 * 1000;

  // A request asked at `nowMillis`. True when the controller has to start.
  bool Renew(int64_t nowMillis) {
    const bool start = !held_;
    held_ = true;
    untilMillis_ = nowMillis + kLeaseMillis;
    return start;
  }

  // True once, when the lease has run out at `nowMillis`: the controller has
  // to stop.
  bool Expire(int64_t nowMillis) {
    if (!held_ || nowMillis < untilMillis_) return false;
    held_ = false;
    return true;
  }

  void Release() {
    held_ = false;
    untilMillis_ = 0;
  }

  bool Held() const { return held_; }

 private:
  bool held_ = false;
  int64_t untilMillis_ = 0;
};

// ---- the extender switch while disconnected --------------------------------
// The provider extender setting belongs to the network space
// (`.provide_extender` in the daemon's storage), and every device reads it from
// its space when it starts.

// Where set_provide_extender writes it (TunnelHost::SetProvideExtender):
// through the device that runs, which persists it and applies it at once, and
// with none into the space the last device ran in, which the next start of
// either imports. The two devices never run side by side.
enum class ExtenderSettingTarget {
  ProviderDevice,  // the provider-only device: the switch's own case
  SessionDevice,   // a tunnel session's device that came up after the GUI asked
  NetworkSpace,    // no device: the space the last device ran in
  None,            // no device has run in this daemon: refused
};

constexpr ExtenderSettingTarget ExtenderSettingTargetFor(bool providerDevice, bool sessionDevice,
                                                         bool lastSpace) {
  if (providerDevice) return ExtenderSettingTarget::ProviderDevice;
  if (sessionDevice) return ExtenderSettingTarget::SessionDevice;
  if (lastSpace) return ExtenderSettingTarget::NetworkSpace;
  return ExtenderSettingTarget::None;
}

// For logs.
constexpr const char* ToString(ExtenderSettingTarget target) {
  switch (target) {
    case ExtenderSettingTarget::ProviderDevice: return "the provider-only device";
    case ExtenderSettingTarget::SessionDevice: return "the session's device";
    case ExtenderSettingTarget::NetworkSpace: return "the last device's network space";
    case ExtenderSettingTarget::None: break;
  }
  return "nothing";
}

// What the connect page's Extender switch reads and writes
// (SdkHost::GetExtenderProvideStatus, GetProvideExtender, SetProvideExtender):
// a bound DeviceRemote over the device rpc; with none, the provider-only
// device as provider_stats read it, written with set_provide_extender, when
// that reply said the daemon takes the write; otherwise nothing, and the
// switch hides (N1: never a dead switch).
enum class ExtenderSwitchSource {
  Device,
  Daemon,
  None,
};

constexpr ExtenderSwitchSource ExtenderSwitchSourceFor(bool sessionDevice, bool providerOnlyRead,
                                                       bool daemonWrites) {
  if (sessionDevice) return ExtenderSwitchSource::Device;
  if (providerOnlyRead && daemonWrites) return ExtenderSwitchSource::Daemon;
  return ExtenderSwitchSource::None;
}

// What the lag.dev speed tests switch under it reads and writes
// (SdkHost::GetMeasurementsEnabled, SetMeasurementsEnabled; lag.dev/PLAN.md
// C4): the measurements setting, a setting of the same space that rides on the
// role. The Extender switch's source, and with no session the daemon only when
// provider_stats said it takes this write too (set_measurements_enabled);
// otherwise nothing, and that switch hides (N1).
constexpr ExtenderSwitchSource MeasurementsSwitchSourceFor(bool sessionDevice,
                                                           bool providerOnlyRead,
                                                           bool daemonWritesExtender,
                                                           bool daemonWritesMeasurements) {
  if (sessionDevice) return ExtenderSwitchSource::Device;
  if (providerOnlyRead && daemonWritesExtender && daemonWritesMeasurements) {
    return ExtenderSwitchSource::Daemon;
  }
  return ExtenderSwitchSource::None;
}

}  // namespace urnw::provide
