// The daemon's tunnel core: DeviceLocal(enable_rpc=false, then setRpcServer
// with the client's pinned mTLS material — see RunStart) + /dev/net/tun +
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
#include <vector>
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

  // Take ownership of a kill-switch floor installed by NetFilter's static
  // startup sweep after a crash. Without this the machine is blocked by a floor
  // no object owns: status reports "not blocked", the reaper does not re-install
  // it if something flushes it, and nothing tears it down.
  void AdoptArmedFloor();

  TunnelHost(const TunnelHost&) = delete;
  TunnelHost& operator=(const TunnelHost&) = delete;

  // Builds the DeviceLocal with NO rpc listener of its own and then PINS its
  // loopback device RPC to the client-supplied mTLS material (the SDK's
  // enable_rpc=true constructor would bind an unpinned PLAINTEXT ws on
  // 127.0.0.1:12025 first, which any local process could drive), opens the tun,
  // installs the capture routes
  // into their own table behind the fwmark rule, verifies the daemon's own
  // traffic still escapes, applies DNS and swaps the nftables floor to
  // Connected.
  //
  // The pinning triple is REQUIRED and re-validated here with
  // ctl::ValidateStartTunnelRequest even though ControlServer already checked
  // it — this is root, and rpc_listen_hostport decides where root binds. There
  // is no unpinned fallback: a start without usable material fails with
  // kCodeRpcPinRequired / kCodeRpcPinInvalid, and a setRpcServer that throws
  // fails with kCodeRpcListenFailed. Success is published as
  // StatusReply::rpc_pinned, which is the only signal the async path has.
  //
  // The listener is dropped by destroying the DeviceLocal and by nothing else:
  // the SDK binding exposes no clearRpcServer/stopRpcServer, and setRpcServer
  // has no documented re-entrancy contract, so it is called exactly ONCE per
  // DeviceLocal, before any listener or getter. Every RunStart begins with
  // StopInternalLocked, so each session gets a fresh device and a fresh call.
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
  //
  // The rpc comparison is a CORRECTNESS gate, not a security boundary: the
  // control socket's SO_PEERCRED check is the boundary, and every peer past it
  // could stop and restart the tunnel anyway. What it buys is that adoption
  // can only ever hand back a listener the client can actually dial — and it
  // is why the client must re-present the SAME material across its own
  // restarts (a per-launch regenerate would fail this and tear down a working
  // tunnel on every GUI launch, which is precisely what CanAdopt was added to
  // stop).
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
  void ReapRetiredLoopsLocked();
  // RUNAWAY GUARD. Samples the tun's own byte counters and tears the tunnel
  // down if it is transmitting hard while receiving nothing — the signature of
  // a capture loop. Returns true when it stopped the tunnel.
  bool CheckTunnelStormLocked();
  uint64_t stormLastTx_ = 0;
  uint64_t stormLastRx_ = 0;
  int stormStrikes_ = 0;

  // THE CONTINUOUS EGRESS WITNESS. Re-runs Tunnel::VerifyEgressWitness on the
  // reaper tick and tears the session down on two CONSECUTIVE failures.
  //
  // The bring-up witness proves the exclusion held at t=0. It cannot prove it
  // still holds at t=40min, and the incident this exists for ran for forty
  // minutes: a `systemctl restart nftables` (whose shipped Fedora/Bazzite
  // config begins with `flush ruleset`), a default-route change, an interface
  // flap or a competing `ip rule` can each end the exclusion under a tunnel
  // that has already been declared up.
  //
  // WHY IT DOES NOT REPLACE THE STORM GUARD, AND WHY BOTH SHIP. The storm
  // guard fires on a SYMPTOM — >=64 MiB out with <1 MiB back, three ticks
  // running — so it needs a loop already moving ~192 MiB before it acts, and
  // it is blind to a low-rate failure where the SDK simply backs off and the
  // UI sits on Connected forever. This fires on DIRECT ATTRIBUTION, at four
  // datagrams, at any rate. The witness proves the precondition; the storm
  // guard bounds the damage if the precondition fails between samples.
  //
  // TWO consecutive failures, not one: a genuine transient during an interface
  // change is real, and tearing a healthy tunnel down on a single sample would
  // make the guard the outage. Two samples caps exposure at ~60s against the
  // 40 minutes that actually happened.
  bool CheckEgressWitnessLocked();
  int egressWitnessTicks_ = 0;
  int egressWitnessFailures_ = 0;

  // R4's primary mechanism: marks every socket this process creates with
  // kEgressMark at socket() time, from a cgroup-bpf sock_create program, so
  // the SDK's sockets never resolve a tunnel route (or a tunnel source
  // address) in the first place. Owned for the length of a session and
  // detached in the teardown; see EgressSocketMarker in Tunnel.hpp for why the
  // nftables mark chain alone cannot do this job.
  EgressSocketMarker egressMarker_;

  // ---- the nftables floor: ONE decision site --------------------------------
  //
  // Every state change goes through ApplyFilterLocked, which asks
  // FloorForTransition (Tunnel.hpp) whether the block floor rides along. It
  // used to take the floor as a parameter, and every caller answered it
  // separately: the bring-up hardcoded `false`, so a reconnect after an
  // UNEXPECTED DROP lifted the kill switch for the whole attempt — the exact
  // window the kill switch exists to close. The parameter is gone; the toggle
  // (killSwitchRequested_) is the only INPUT and the floor is the OUTPUT.
  //
  // All three require opMutex_.
  bool ApplyFilterLocked(FilterState next, std::string* error);
  // Re-install what is ALREADY in force, byte-for-byte the same decision. NOT
  // a transition — the floor is preserved, never re-derived — because this is
  // the tamper path: something outside URnetwork (a root `nft flush ruleset`,
  // which is the first line of Fedora/Bazzite's shipped nftables.conf) removed
  // our table and re-deriving would ask "should a Connecting->Connecting
  // transition carry the floor", which is not the question.
  bool ReinstallFilterLocked(std::string* error);
  // The shared core. `floor` is an OUTPUT of one of the two above and of
  // nothing else.
  bool InstallFilterLocked(FilterState state, bool floor, std::string* error);
  // The FilterConfig for `state`, built from the LIVE session (tun name, the
  // resolvers actually handed to resolved, the DNS-helper cgroups).
  FilterConfig FilterConfigForLocked(FilterState state, bool floor) const;
  // Reaper duty: verify the table is still ours and re-install it when it is
  // not, and retry a teardown that failed. Requires opMutex_.
  void MaintainFilterLocked();

  void JoinWorker();
  void Reap();

  // status_ mutators (take statusMutex_ themselves)
  void PublishError(const std::string& message, const std::string& code);

  std::string storageRoot_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::DeviceLocal> device_;
  std::optional<urnet::IoLoop> ioLoop_;
  // Set by the LIVE loop's done callback; a retired loop carries its own copy
  // (see retiredLoops_) so the two can never be confused.
  std::shared_ptr<std::atomic<bool>> ioLoopFinished_;

  // RETIRED IoLoops, kept alive until their done callback has actually fired.
  //
  // urnet::newIoLoop stores the done callback in a shared_ptr, hands GO A RAW
  // POINTER to it, and keeps it alive by retaining that shared_ptr INSIDE the
  // IoLoop object. IoLoop exposes only close(), which is asynchronous: it asks
  // the Go loop to stop and returns immediately. Destroying the IoLoop right
  // after close() therefore frees the callback while the loop is still winding
  // down, and the deferred done callback then fires through a dangling pointer.
  //
  // Measured, on the first working tunnel: pressing Disconnect three seconds
  // after connecting killed the daemon with
  //   SIGSEGV ... addr=0x0, signal arrived during cgo execution
  //   _Cfunc_urnet_invoke_io_loop_done <- (*IoLoop).run.deferwrap2
  // and systemd restarted it, taking the tunnel with it.
  //
  // So a stopped loop is RETIRED, not destroyed, and released only once its own
  // callback has run. Leaking a handful of handles on a pathological loop that
  // never finishes is strictly better than a root daemon core-dumping.
  struct RetiredIoLoop {
    urnet::IoLoop loop;
    std::shared_ptr<std::atomic<bool>> finished;
  };
  std::vector<RetiredIoLoop> retiredLoops_;
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

  // A teardown that FAILED, remembered so the reaper can retry it.
  // NetFilter::Remove() sets state_=Off *before* it checks whether nft
  // succeeded, so the filter itself keeps no memory of the failure and
  // ~NetFilter (guarded on state_ != Off) will not retry either: without this
  // flag one failed `nft -f` leaves the table — and, if it was armed, the block
  // — in the kernel until the machine reboots. Written on the worker and on the
  // main loop, so atomic.
  std::atomic<bool> filterRemovalPending_{false};
  // Reaper-tick bookkeeping for the tamper poll (main loop only).
  int filterVerifyTicks_ = 0;
  int filterVerifyFailures_ = 0;

  guint reaperId_ = 0;
  guint resolvedWatchId_ = 0;
  bool resolvedSeen_ = false;
};

}  // namespace urnw
