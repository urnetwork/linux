// The daemon's tunnel core: DeviceLocal(enable_rpc=true) + /dev/net/tun +
// urnet::newIoLoop + the nftables leak floor, lifted from the GUI
// SdkHost::StartTunnel at the daemon split (linux/MIGRATION.md). The Linux
// analogue of the Windows Service/TunnelController: the GUI's DeviceRemote
// reaches this device over the SDK's loopback mTLS RPC once Start() succeeds.
//
// Owns the persisted device identity (Ed25519 client key seed + provide TLS
// cert/key) under the daemon state dir (/var/lib/urnetwork): it is
// device-scoped, not session-scoped, so peers keep verifying this device
// across daemon restarts and GUI re-logins. Only an explicit identity wipe
// rotates it — and, since the audit, a FAILED restore never rotates it either.
//
// THREADING — this changed, and the change is the point:
//   * Start/Stop/SetProvideMode/SetKillSwitch and the two glib callbacks
//     (the reaper tick, the systemd-resolved name watch) all run on the daemon
//     MAIN LOOP. The control server serializes request handling, so they never
//     overlap each other.
//   * The bring-up itself (network space import, DeviceLocal construction —
//     a network round trip — tun open, ~35 subprocesses) may run on a WORKER
//     thread when the client sends async=true, so the control loop stays
//     answerable while it runs. Without that, a start slower than the client's
//     30 s receive timeout made the client reconnect and re-send start_tunnel,
//     which used to tear the half-built session down and start again: a
//     restart loop on exactly the slow networks where the timeout fires.
//   * opMutex_ guards the session objects (device_/ioLoop_/tunnel_/filter_).
//     The main-loop callbacks only ever TRY-lock it, so a slow bring-up can
//     never block the loop.
//   * statusMutex_ guards ONLY the published StatusReply, so `status` answers
//     in microseconds at every phase of a bring-up.
//   * The SDK IoLoop done callback runs on an SDK thread and does nothing but
//     publish; the real teardown (and arming the kill switch on an unexpected
//     drop) happens on the reaper tick, on the main loop, where it is safe.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// gio, not bare glib: the systemd-resolved restart watch is a bus-name watch,
// and gio is already a daemon dependency (no GTK is pulled in by it).
#include <gio/gio.h>

#include <urnetwork_sdk.hpp>

#include "ControlProtocol.hpp"
#include "Tunnel.hpp"

namespace urnw {

class TunnelHost {
 public:
  // storageRoot: the daemon state dir (normally /var/lib/urnetwork). Key
  // material lives directly in it; the SDK network-space storage in
  // storageRoot/sdk.
  explicit TunnelHost(std::string storageRoot);
  ~TunnelHost();

  TunnelHost(const TunnelHost&) = delete;
  TunnelHost& operator=(const TunnelHost&) = delete;

  // Builds the DeviceLocal (rpc enabled), opens the tun, installs the capture
  // routes into their own table behind the fwmark rule, verifies the daemon's
  // own traffic still escapes, applies DNS and swaps the nftables floor to
  // Connected.
  //
  // config.async=false: runs inline and returns the FINAL status (the old
  // contract). config.async=true: returns immediately with
  // tunnel_state=starting; the client polls `status`.
  //
  // A start while a start is already running does NOT tear down and restart —
  // it returns the live status with error_code=start_in_progress.
  // MAIN LOOP ONLY.
  ctl::StatusReply Start(const ctl::StartTunnelRequest& config);

  // True when the LIVE session already satisfies this request, so the daemon
  // adopts it instead of tearing a working tunnel down and rebuilding it —
  // which is what a GUI restart used to cost, every time, because the client
  // issues start_tunnel unconditionally at launch. Deliberately strict: the
  // instance id (the device pairing key), the jwt, the network space and the
  // rpc pinning material must all match, because anything else would hand the
  // client a DeviceRemote that attaches to a device it did not describe.
  bool CanAdopt(const ctl::StartTunnelRequest& config) const;

  // Tears the session down and records why. "user" for an explicit
  // stop_tunnel, "daemon_shutdown" at exit; the reaper supplies "io_loop".
  // Blocks until a running bring-up has finished (bounded by the SDK call it
  // is inside). MAIN LOOP ONLY.
  void Stop(const std::string& reason = "user");

  // Applies the provide control mode to the live device, or stashes it for
  // the next Start when the tunnel is down.
  bool SetProvideMode(const std::string& mode);

  // The kill switch the CLIENT asked for. Semantics follow the Windows source
  // of truth (docs/linux_agent_help.md §6.3): a user disconnect always lifts
  // the policy, and turning the switch on while nothing is connected does NOT
  // block the machine — only an UNEXPECTED drop arms it. Returns false with
  // *error when the floor could not be installed, and the published state
  // becomes KillSwitchState::Failed, never Off.
  bool SetKillSwitch(bool enabled, std::string* error);

  // Never blocks behind a bring-up.
  ctl::StatusReply Status() const;
  bool TunnelUp() const;

  // The control server publishes whether any client currently owns the
  // tunnel, so `status` can report a captured machine with no UI attached.
  void SetOwnerConnected(bool connected);

  // Seconds a tunnel may keep running with no owning client before the daemon
  // stops it by itself. 0 (the default) keeps the current behaviour: the
  // tunnel survives a GUI crash/restart and is adoptable. Set from
  // $URNETWORK_ORPHAN_TIMEOUT_SECONDS by the daemon entry point.
  void SetOrphanTimeoutSeconds(int seconds) { orphanTimeoutSeconds_ = seconds; }

 private:
  static gboolean OnReaperTick(gpointer data);
  static void OnResolvedAppeared(GDBusConnection* connection, const gchar* name,
                                 const gchar* nameOwner, gpointer data);

  std::optional<urnet::DeviceLocalKeyMaterial> LoadKeyMaterial() const;
  bool PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) const;
  bool HasStoredKeyMaterial() const;

  // The whole bring-up. Runs either inline (async=false) or on worker_.
  void RunStart(ctl::StartTunnelRequest config);
  // Requires opMutex_.
  void StopInternalLocked(const std::string& reason);
  // Requires opMutex_. Swaps the nftables table to match the current session.
  // `killSwitch` is passed explicitly rather than read from
  // killSwitchRequested_ because the block floor is deliberately NOT installed
  // during a bring-up — see RunStart.
  bool ApplyFilterLocked(FilterState state, bool killSwitch, std::string* error);
  void JoinWorker();
  void Reap();

  // status_ mutators (take statusMutex_ themselves)
  void PublishError(const std::string& message, const std::string& code);

  std::string storageRoot_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::DeviceLocal> device_;
  std::optional<urnet::IoLoop> ioLoop_;
  std::unique_ptr<Tunnel> tunnel_;
  NetFilter filter_;
  // Resolved once at construction: whose sockets the nftables mark chain
  // exempts from our own tunnel. Derived from /proc/self/cgroup, so a
  // --foreground dev run marks itself correctly too.
  CgroupRef cgroup_;
  // $URNETWORK_ALLOW_UNPROTECTED_EGRESS — development escape hatch that lets
  // the tunnel come up with the daemon's own sockets INSIDE it. Logged loudly
  // and reported as egress_protected=false; never a default.
  bool allowUnprotectedEgress_ = false;

  mutable std::mutex statusMutex_;
  ctl::StatusReply status_;

  std::mutex opMutex_;
  std::thread worker_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> stopRequested_{false};
  // Bumped on every teardown; the IoLoop done callback carries the generation
  // it was created with, so a callback arriving after an intentional stop
  // cannot be mistaken for a dead tunnel.
  std::atomic<uint64_t> sessionGeneration_{0};
  // Set by the IoLoop done callback (SDK thread). The reaper, on the main
  // loop, performs the actual teardown.
  std::atomic<bool> ioLoopDied_{false};

  // What the live session was built from, for CanAdopt. Written on the worker
  // under opMutex_, read on the main loop under statusMutex_.
  ctl::StartTunnelRequest activeConfig_;

  // set_provide received while down. Written on the main loop, read by the
  // worker mid-bring-up, so it is guarded by statusMutex_ (the short-held one)
  // rather than opMutex_ (which the worker owns for the whole bring-up).
  std::string pendingProvideMode_;
  // Written on the main loop (Start, SetKillSwitch), read by the worker when
  // it installs the Connected floor and by the reaper when it arms after an
  // unexpected drop.
  std::atomic<bool> killSwitchRequested_{false};
  std::atomic<bool> ownerConnected_{false};
  std::atomic<int> orphanTimeoutSeconds_{0};
  int64_t ownerLostMonotonicSeconds_ = 0;

  guint reaperId_ = 0;
  guint resolvedWatchId_ = 0;
  bool resolvedSeen_ = false;
};

}  // namespace urnw
