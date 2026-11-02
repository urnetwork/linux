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
//   * StartProvider runs on the main loop too and only try-locks opMutex_. The
//     provider-only device it builds never coexists with a tunnel session's:
//     every teardown, and so the head of every bring-up, retires it first.
//     ProviderStats, which reads that device's view controllers for the GUI,
//     only try-locks it as well.
//   * UploadLogs runs on the main loop and only try-locks opMutex_; a request
//     that finds a bring-up is queued and the reaper starts it. The sdk's
//     upload (the zip and the post) runs on a thread of its own
//     (logupload::Flight), which touches nothing of this object but the
//     device's handle; the device stays alive until that call returns
//     (ReleaseDeviceLocked). The upload's callback ends the upload in the
//     flight; the reaper retires the standalone device.
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
#include "LogUpload.hpp"
#include "NetworkQuality.hpp"
#include "PassedLogFiles.hpp"
#include "Tunnel.hpp"
#include "TunnelWatchdog.hpp"
#include "daemon/ExitSampler.hpp"
#include "daemon/HostSettings.hpp"

namespace urnw {

class TunnelHost {
 public:
  // storageRoot: the daemon state dir (normally /var/lib/urnetwork). Key
  // material lives directly in it; the SDK network-space storage in
  // storageRoot/sdk. settings: every timing of the network-switch path
  // (HostSettings.hpp), defaults unless a caller says otherwise.
  explicit TunnelHost(std::string storageRoot, HostSettings settings = HostSettings());
  ~TunnelHost();

  // Take ownership of a kill-switch floor installed by NetFilter's static
  // startup sweep after a crash. Without this the machine is blocked by a floor
  // no object owns: status reports "not blocked", the reaper does not re-install
  // it if something flushes it, and nothing tears it down.
  void AdoptArmedFloor();

  // Publishes the stop a previous daemon recorded before it ended itself over
  // an SDK that stopped answering (SelfRestart.hpp), so the app is told why its
  // tunnel stopped by the daemon that replaced it, and deletes the record. A
  // no-op without a fresh one. Call before the control socket serves anyone.
  void RestoreStopRecord();

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
  // Main loop only.
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
  // is inside). Main loop only.
  void Stop(const std::string& reason = "user");

  // Applies the provide control mode to the live device, or stashes it for
  // the next Start when the tunnel is down. With no tunnel session it also
  // decides the provider-only device: a mode that does not provide while
  // disconnected retires it, any other mode is applied to it.
  bool SetProvideMode(const std::string& mode);

  // The provider-only device (start_provider; ProvideLifecycle.hpp). Keeps
  // providing while the user is disconnected: a DeviceLocal built from the
  // persisted identity and the request's credentials and network space, with
  // the request's provide mode — and nothing else. No tun, no capture routes,
  // no DNS change, no nftables change, no egress marker and no device RPC
  // listener, so this machine's own traffic is routed exactly as it would be
  // without URnetwork, and no client can drive the device.
  //
  // Refused while a tunnel session exists or is being built (its device is
  // the provider) and while the kill-switch floor is armed. The same request
  // again keeps the running device and only applies the mode. Main loop only.
  struct ProviderStartResult {
    bool ok = false;
    std::string error;
    const char* code = nullptr;  // a ctl::kCode* when !ok
  };
  ProviderStartResult StartProvider(const ctl::StartProviderRequest& request);
  // A provider-only device is running. Never blocks behind a bring-up.
  bool ProviderRunning() const;
  // What the provider-only device's own view controllers say (provider_stats):
  // its provider series, transport share and packet-stats bit, its provider
  // status, its extender role's status and series, and the provider extender
  // setting with this daemon's word that it takes its write. `pollStatus`
  // renews the status controller's polling lease (provide::ProviderStatusLease).
  // Empty without that device, and while a bring-up owns the session (which
  // retires it first). Main loop only; never blocks, and never throws across
  // the wire.
  ctl::ProviderStatsReply ProviderStats(bool pollStatus);
  // The connect page's Extender switch while no tunnel session's device takes
  // it (set_provide_extender): writes the provider extender setting where
  // provide::ExtenderSettingTargetFor says — through the provider-only device
  // (persisted in its space and applied at once), a session's device that came
  // up meanwhile, or with neither networkSpace_, the space the last device ran
  // in, which the next start imports again. Refused while a bring-up owns the
  // session, never waited behind. False with `error` when nothing took the
  // write. Main loop only; the write itself is local to this process (the
  // space's setting file, and the role's start or stop, as set_provide's mode
  // change does).
  bool SetProvideExtender(bool on, std::string* error);
  // The lag.dev speed tests switch while no tunnel session's device takes it
  // (set_measurements_enabled, lag.dev/PLAN.md C4): writes the measurements
  // setting where SetProvideExtender writes the provider extender setting, by
  // the same rule, refused the same way. Main loop only.
  bool SetMeasurementsEnabled(bool on, std::string* error);

  // Account > Extenders' Reset extenders (reset_extenders; connect EXTENDER.md
  // E7): applies a reset the GUI made in its own space to the space this daemon
  // holds under the request's key (applyExtenderReset), which clears what the
  // space learned about extenders and restarts its extender client and node.
  // The tunnel session's device and the provider-only device run in the
  // manager's space for the key, so one call covers both, and their live
  // extender paths keep running. `reset` says whether a space was held and the
  // reset was new to it: with no space for the key (no device has run, or the
  // key was never imported) nothing is held, and the next import of the space
  // applies the id it carries. Refused, never waited behind, while a bring-up
  // owns the session (ctl::kCodeStartInProgress). The space is looked up under
  // opMutex_ and the reset applied outside it, since the sdk joins the space's
  // old extender client. Main loop only.
  struct ExtenderResetResult {
    bool ok = false;
    bool reset = false;
    std::string error;
    const char* code = nullptr;  // a ctl::kCode* when !ok
  };
  ExtenderResetResult ResetExtenders(const ctl::ResetExtendersRequest& request);

  // upload_logs (ControlProtocol.hpp; LogUpload.hpp carries the lifecycle):
  // "send feedback with logs" uploads this process's glog files, connected or
  // not. The sdk's UploadLogs runs on the tunnel session's device, else on the
  // provider-only device, else on a standalone device built from the request's
  // credentials exactly as the provider-only device is (provide mode never,
  // and no tun, route, DNS, nftables or listener), which is retired once its
  // upload finishes, after logupload::kStandaloneDeviceMaxMillis, or before any
  // other device under this identity is built. While a bring-up owns the
  // session the request is queued and the reaper starts it once the bring-up
  // is over. One upload at a time (logupload::Flight): a request while one is
  // in flight is refused with ctl::kCodeLogUploadBusy. Returns once the upload
  // is admitted: the zip and the post run on their own thread, and status
  // reports the outcome under `uploadId`. `carrier` is logupload::ToString of
  // the device. Main loop only.
  //
  // `guiLogFiles` are the GUI's own log files, passed by descriptor
  // (PassedLogFiles.hpp): the zip carries them under gui/, and they are closed
  // once the sdk's call has read them, or when the request ends without one.
  struct LogUploadResult {
    bool ok = false;
    const char* carrier = "";
    int64_t uploadId = 0;
    std::string error;
    const char* code = nullptr;  // a ctl::kCode* when !ok
  };
  LogUploadResult UploadLogs(const ctl::UploadLogsRequest& request,
                             logupload::PassedLogFiles guiLogFiles);

  // The account signed out (logout; SignOut.hpp, owner decision 2026-10-05:
  // each network starts fresh). Ends what runs as an explicit stop does (the
  // session, the provider-only device, a log upload's standalone device and a
  // queued upload), forgets the provide mode and the kill switch the account
  // asked for, deletes the device identity this daemon keeps and logs out what
  // the sdk stored in `networkSpaceJson`'s space (the client credential and
  // instance a device persists when it starts, among the rest; an empty json
  // is the compiled-in default space, as for a start). The next start then
  // makes a new identity in a clean space. The space's extender state stays,
  // as every sign-out leaves it (the sdk's LocalState.logout keeps it).
  //
  // Never waits behind a bring-up: false, with nothing cleared, while one owns
  // the session (`*code` ctl::kCodeStartInProgress). False with `*error` when a
  // file or the space could not be cleared; what could be cleared is. The GUI
  // keeps a refused sign-out owed and sends it again. Whose session this may
  // clear is the control server's to decide. Main loop only.
  bool Logout(const std::string& networkSpaceJson, std::string* error, const char** code);

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

  // The uid that owns the tunnel (ControlServer::ClaimTunnelOwnership), -1 when
  // nobody does. Only that uid's urnetwork-exclude slice may leave outside the
  // tunnel ("per-app split tunnel", TunnelPolicy.hpp); the reaper picks a
  // change up on its next tick.
  void SetOwnerUid(int64_t uid);

  // The country of the mobile network this machine is on, "" for none
  // (NetworkCountry.hpp, P052; the daemon's NetworkCountryWatcher reads it).
  // Process-wide in the sdk, so it is in force for every device built after
  // this and applies in place to a running one from its next extender dial.
  // Published in status for the GUI, which applies it to its own dials too.
  // Main loop only.
  void SetNetworkCountryCode(const std::string& countryCode);

  // Seconds a tunnel may keep running with no owning client before the daemon
  // stops it by itself. 0 (the default) keeps the current behaviour: the
  // tunnel survives a GUI crash/restart and is adoptable. Set from
  // $URNETWORK_ORPHAN_TIMEOUT_SECONDS by the daemon entry point.
  void SetOrphanTimeoutSeconds(int seconds) { orphanTimeoutSeconds_ = seconds; }

 private:
  // THE FIRST MEMBER, AND IT HAS TO BE. C++ builds members in the order they are
  // declared here, whatever a constructor's list says, and members below copy
  // their timings out of this one when they are built. A member declared before
  // it would copy raw stack memory, which is what the dead-tunnel failsafe's
  // windows were from 2026-10-20 (read from the released arm64 daemon: 16, 0, 169
  // and pointers, not 20, 30 and 90 s).
  const HostSettings settings_;

  static gboolean OnReaperTick(gpointer data);
  static void OnResolvedAppeared(GDBusConnection* connection, const gchar* name,
                                 const gchar* nameOwner, gpointer data);

  std::optional<urnet::DeviceLocalKeyMaterial> LoadKeyMaterial() const;
  bool PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) const;
  bool HasStoredKeyMaterial() const;

  // The whole bring-up. Runs either inline (async=false) or on worker_.
  void RunStart(ctl::StartTunnelRequest config);
  // RunStart's network space and DeviceLocal steps, shared with StartProvider
  // so both devices come from one copy of the identity rules (never rotate the
  // stored identity after a failed restore). Both require opMutex_;
  // NewDeviceLocked throws when no device could be built at all.
  void LoadNetworkSpaceLocked(const std::string& networkSpaceJson);
  urnet::DeviceLocal NewDeviceLocked(const std::string& byJwt, const std::string& instanceId,
                                     const std::string& appVersion);
  // Closes the provider-only device, waits a bounded time for its sockets to
  // close and clears its published status. A no-op without one. Requires
  // opMutex_.
  void RetireProviderDeviceLocked();
  // The provider-only device's view controllers: opened right after the device
  // is built, each on its own (one that cannot open costs only its own
  // statistics), and closed with the typed closes before the device. Both
  // require opMutex_.
  void OpenProviderViewControllersLocked();
  void CloseProviderViewControllersLocked();
  // Publishes the provider-only device's live tier, network key and client
  // count into status_. Requires opMutex_.
  void RefreshProviderStatusLocked();
  // UploadLogs' body once no bring-up owns the session: admits the upload
  // (or starts the queued one, `queuedUploadId`), then hands it to its thread,
  // with the GUI's log files, on the device that runs, or on a standalone one
  // built for it. Requires opMutex_.
  LogUploadResult StartLogUploadLocked(const ctl::UploadLogsRequest& request,
                                       int64_t queuedUploadId,
                                       logupload::PassedLogFiles guiLogFiles);
  // Closes the standalone upload device, waiting a bounded time as the
  // provider-only device's retire does. A no-op without one. Requires opMutex_.
  void RetireUploadDeviceLocked();
  // Releases a device the caller has closed, unless the log upload's call is on
  // it: then the flight keeps it until that call returns. Requires opMutex_.
  void ReleaseDeviceLocked(std::optional<urnet::DeviceLocal>& device);
  // Reaper duty: retire the standalone device whose upload finished or ran out
  // of time, then start a queued request. Requires opMutex_.
  void MaintainLogUploadLocked();
  // Requires opMutex_. A reason means "somebody asked for this stop": it lifts
  // the nftables policy on the way out (ApplyFilterLocked(Off)) and REPLACES
  // status_.error/error_code with the reason. An EMPTY reason means "the caller
  // owns what happens next": the firewall is left exactly as it is and the
  // published error is left exactly as it is, for the caller to set.
  void StopInternalLocked(const std::string& reason);
  // The machine half of every teardown, and its first half: the session
  // published as stopping, the failsafe's watch ended, the DNS undo while the
  // tun still exists, the IoLoop cancelled and retired, and the tun closed with
  // its policy rules and capture routes.
  // No call here waits on the device (each one that does takes its state
  // lock), so an SDK that stopped answering cannot keep the machine pointed at
  // its tunnel. StopInternalLocked starts with it and goes on to the device; a
  // caller that lands the floor itself calls it first, lands, and then calls
  // StopInternalLocked(""), so the landing does not wait on the device either.
  // Idempotent. Requires opMutex_.
  void RevertSessionMachineLocked();

  // THE PROTECTIVE TEARDOWN. The session is being destroyed because it has been
  // PROVEN unsafe — the daemon's own traffic is no longer outside its own
  // tunnel, or the tun is amplifying instead of carrying — and not because
  // anybody asked it to stop. It exists because the landing state is the
  // OPPOSITE of a user stop's:
  //   * StopInternalLocked(<non-empty reason>) is a user disconnect and LIFTS
  //     the kill-switch floor (FloorForTransition answers false for every
  //     transition into Off — Tunnel.cpp).
  //   * this FAILS CLOSED: Armed, floor holding, whenever the user asked for
  //     the kill switch — an involuntary teardown is an unexpected drop, which
  //     is the one case that arms rather than lifts (§6.3) — and it publishes,
  //     LAST of all, whether this machine is now blocked and how to unblock it.
  // A guard must never reach for StopInternalLocked(<reason>): doing so lifted
  // the floor at the exact instant we had four datagrams of proof that traffic
  // was leaving unprotected, and erased the explanation on the same line.
  // Requires opMutex_.
  //
  // The dead-tunnel failsafe lands the same way, for a session proven to carry
  // nothing rather than proven unsafe; `why` names which in the log ("as
  // UNSAFE", "because it carried nothing").
  //
  // `sdkWedged`: the SDK has stopped answering (the failsafe's
  // SdkUnresponsive), so the device half would never return. The machine is
  // landed as above and the process then ends itself instead
  // (RestartForWedgedSdkLocked), and this does not return.
  void StopUnsafeSessionLocked(const std::string& reason, const std::string& message,
                               const std::string& code, const char* why = "as UNSAFE",
                               bool sdkWedged = false);
  // Leaves the stop record for the next daemon, detaches the egress marker and
  // _exits with selfrestart::kExitStatus, for systemd to start a clean daemon
  // (SelfRestart.hpp). Makes no call into the SDK. Only after the machine is
  // given back. Requires opMutex_.
  [[noreturn]] void RestartForWedgedSdkLocked(const std::string& reason, const std::string& error,
                                              const std::string& code);

  // Re-checks the DNS override mid-session and repairs it, escalating to
  // StopUnsafeSessionLocked when it cannot be restored. This is the only caller
  // of Tunnel::VerifyDnsStillApplied. Requires opMutex_.
  void MaintainDnsLocked();

  void ReapRetiredLoopsLocked();
  // RUNAWAY GUARD. Samples the tun's own byte counters and tears the tunnel
  // down if it is transmitting hard while receiving nothing — the signature of
  // a capture loop. Tears down through StopUnsafeSessionLocked, so a machine
  // whose owner asked for the kill switch stays blocked. Returns true when it
  // stopped the tunnel.
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
  //
  // The teardown goes through StopUnsafeSessionLocked and lands FAIL-CLOSED
  // (Armed, floor holding) when the kill switch was asked for: this guard fires
  // on PROVEN-UNPROTECTED traffic, so it is the last moment at which the floor
  // may come down.
  bool CheckEgressWitnessLocked();
  int egressWitnessTicks_ = 0;
  int egressWitnessFailures_ = 0;

  // The dead-tunnel failsafe (TunnelWatchdog.hpp; docs/linux_agent_help.md
  // 6.4). From the up edge to the teardown an ExitSampler asks the session's
  // device for its proven exits and its contract status and tells it of the
  // reaper's network changes, so the reaper makes no call on that device; the
  // reaper reads the tun's packet counters and those readings once a second and
  // hands them to deadTunnelWatch_. On a dead verdict the session ends through
  // StopUnsafeSessionLocked: the armed floor when the kill switch was asked
  // for, no table otherwise, a stop_reason of ctl::kStopReasonFailsafe* and the
  // sentence saying which. Nothing reconnects. An account out of balance gets no
  // such verdict for carrying nothing (watchdog::OutOfBalanceHold). While a
  // countdown is close, status says failsafe_armed. All three require opMutex_.
  void StartDeadTunnelWatchLocked();
  void StopDeadTunnelWatchLocked();
  // True when it ended the session.
  bool CheckDeadTunnelLocked();
  ExitSampler exitSampler_;
  watchdog::DeadTunnelWatch deadTunnelWatch_{settings_.dead_tunnel};
  bool deadTunnelWatching_ = false;

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
  // resolvers actually handed to resolved, the DNS-helper cgroups, the owner's
  // exclude slice). *excludeId gets that slice's cgroup id, 0 without one.
  FilterConfig FilterConfigForLocked(FilterState state, bool floor,
                                     uint64_t* excludeId = nullptr) const;
  // The tunnel owner's urnetwork-exclude slice when this host can honour it,
  // with its cgroup id (the directory's inode) in *id; an invalid ref and 0
  // otherwise. Requires opMutex_.
  CgroupRef ExcludeSliceLocked(uint64_t* id) const;
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
  // The provider-only device (StartProvider), and the request it was built
  // from. A separate slot from device_, which keeps its one meaning — the
  // tunnel session's device — for every check below that reads it (the filter
  // teardown retry, adoption, the published identity). Guarded by opMutex_.
  std::optional<urnet::DeviceLocal> providerDevice_;
  ctl::StartProviderRequest providerConfig_;
  // Its view controllers, which the GUI reads through provider_stats because
  // it has no DeviceRemote for this device: the provider series behind the
  // Earnings plots and the "no traffic yet" line, the extender series behind
  // the extender plot, and the provider status behind the reason line, the
  // demand histogram and "Why?". The status controller polls only while the
  // lease is held. Guarded by opMutex_.
  std::optional<urnet::ContractViewController> providerContractVc_;
  std::optional<urnet::ProviderStatusViewController> providerStatusVc_;
  provide::ProviderStatusLease providerStatusLease_;
  // The standalone device a log upload runs on while neither device above
  // exists (UploadLogs), and when it was built; the reaper retires it on the
  // main loop. A third slot, never engaged beside device_ or providerDevice_:
  // both are built only after RetireUploadDeviceLocked. Guarded by opMutex_.
  std::optional<urnet::DeviceLocal> uploadDevice_;
  int64_t uploadDeviceBuiltMillis_ = 0;
  // The log upload in flight, shared with its thread and its callback, which
  // hold nothing else of this object (logupload::Flight: its own lock).
  std::shared_ptr<logupload::Flight> logUploadFlight_;
  // A request that arrived while a bring-up owned the session, when, the id
  // the flight admitted it under, and the GUI's log files that came with it.
  // Main loop only (UploadLogs and the reaper), so it needs no lock.
  std::optional<ctl::UploadLogsRequest> queuedUpload_;
  logupload::PassedLogFiles queuedGuiLogFiles_;
  int64_t queuedUploadMillis_ = 0;
  int64_t queuedUploadId_ = 0;
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
  // Re-proven for every session before any SDK socket is created. The first is
  // the BPF measurement; the second is a non-committing nftables kernel probe.
  // Together they select the cgroup+mark or floorless mark-only ruleset.
  bool socketMarkerProven_ = false;
  bool cgroupSocketMatchSupported_ = true;
  // ---- per-app split tunnel (urnetwork-exclude) ----------------------------
  // See "per-app split tunnel" in TunnelPolicy.hpp. ownerUid_ is written on the
  // main loop (SetOwnerUid) and read by the worker's bring-up, so it is atomic.
  std::atomic<int64_t> ownerUid_{-1};
  // /proc/self/cgroup describes the unified hierarchy alone (IsCgroupV2Only).
  // Read once: a host does not change hierarchy under a running daemon.
  bool cgroupV2Only_ = false;
  // The cgroup id of the slice the installed ruleset excludes, 0 for none. nft
  // binds a `socket cgroupv2` path to that id at load time, so a slice that is
  // created, removed or recreated, or a tunnel that changes owner, needs the
  // ruleset installed again: MaintainFilterLocked compares on every tick.
  uint64_t excludeAppliedId_ = 0;
  // A slice whose rules the kernel refused (NetFilter::Apply then installs the
  // ruleset without them). Not offered again until its id changes, so a kernel
  // without nftables NAT does not cost an nft run every second.
  uint64_t excludeRefusedId_ = 0;
  // Backoff for a re-install that failed (main loop only).
  int excludeRetryTicks_ = 0;
  int excludeRetryFailures_ = 0;

  // $URNETWORK_ALLOW_UNPROTECTED_EGRESS — development escape hatch that lets
  // the tunnel come up with the daemon's own sockets INSIDE it. Logged loudly
  // and reported as egress_protected=false; never a default.
  bool allowUnprotectedEgress_ = false;

  mutable std::mutex statusMutex_;
  ctl::StatusReply status_;

  // ---- THE LIVE SESSION'S IDENTITY (upstream TunnelHost.hpp:70-71) ---------
  // The two values attach_tunnel names a session by. Held as their own members
  // rather than written straight into status_ so the lifetime is stated once
  // and in one shape: ONE site sets them (the up edge in RunStart), two clear
  // them (Start's transition to Starting and StopInternalLocked's tail), and
  // Status() assembles them into every reply the way it already assembles
  // owner_connected.
  //
  // NEITHER IS MINTED HERE, AND THAT IS THE DESIGN DECISION. Both arrive on the
  // accepted start_tunnel and are LATCHED: instance_id is the device pairing
  // key this DeviceLocal was constructed with, and rpc_session_id is the name
  // the GUI gave the credential generation it minted — in the same act, from
  // its own CSPRNG, and stored with it (metadata in a 0600 file, the client key
  // and pinned server cert in the Secret Service).
  //
  // Minting the session id daemon-side was considered and rejected. The client
  // has to persist the name BESIDE the half of the mTLS material that never
  // leaves it; a name invented after the tunnel is already up arrives too late
  // for that record, so the GUI would have to write it twice and would hold a
  // credential it cannot name if it died in between. What the daemon owes
  // instead is that the name always describes THIS session, and that it changes
  // whenever the material does:
  //   * set in the same locked block that publishes Up, so no client can read
  //     Up with a half-empty identity;
  //   * cleared on every teardown and at the head of every bring-up, so a
  //     stopped or starting tunnel names nothing and an attach cannot match it;
  //   * never re-pointed at different material while it is live — CanAdopt
  //     compares the name along with the material, and a start_tunnel that
  //     re-uses the live name with a different generation is refused
  //     (ControlServer::HandleStartTunnel, kCodeTunnelAlreadyRunning).
  // Guarded by statusMutex_ (short-held), never by opMutex_: `status` must
  // answer in microseconds at every phase of a bring-up.
  std::string instanceId_;    // exact live DeviceLocal pairing identity
  std::string rpcSessionId_;  // the generation name attach_tunnel matches

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
  // Latched at bring-up: did this session come up WITH DNS protection? See the
  // comment at the assignment — the reaper cannot gate on the live dns_applied,
  // because the failure it watches for is what clears it.
  bool dnsProtectionExpected_ = false;
  int dnsVerifyTicks_ = 0;
  // Consecutive failed repairs. The session is stopped only after several, so a
  // single transient (resolved mid-restart, a momentary unreadable
  // /etc/resolv.conf) does not tear down a working tunnel.
  int dnsVerifyFailures_ = 0;
  // Ticks since the override was last repaired (saturating at the contested
  // window, HostSettings::dns_contested_window_ticks), and whether the last
  // repair came within that window of the one before it: a file a writer keeps
  // rewriting over us, checked at the slower interval rather than every tick
  // (MaintainDnsLocked).
  int dnsTicksSinceRepair_ = 0;
  bool dnsContested_ = false;
  int filterVerifyFailures_ = 0;

  // The live device's network path (NetworkQuality.hpp): rebased right after
  // every NewDeviceLocked, observed by the reaper, under opMutex_.
  LinuxNetworkQualityTracker networkQualityTracker_{settings_.network_path};
  // A suspend, measured at the top of every reaper tick before anything can
  // return early (main loop only), and the kick it owes until a tick holding
  // opMutex_ hands it to the tracker.
  LinuxSuspendDetector suspendDetector_{settings_.suspend};
  bool resumePending_ = false;

  guint reaperId_ = 0;
  guint resolvedWatchId_ = 0;
  bool resolvedSeen_ = false;
};

}  // namespace urnw
