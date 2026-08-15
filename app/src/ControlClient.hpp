// GUI-side client for the urnetworkd control socket (ControlProtocol.hpp; the
// Linux analogue of the Windows App/ServiceClient over PipeClient). Blocking,
// synchronous request/reply — the GUI calls these from the same paths that
// used to construct the DeviceLocal in-process, so a bounded blocking call is
// no worse than before — with one automatic reconnect on a dead socket.
//
// Version skew (APPIMAGE.md §11b, and unlike the Windows twin this is real):
// EnsureSession() performs `hello` with our protocol version and enforces both
// directions — DaemonTooOld when the daemon's advertised protocol is below
// kMinSupportedDaemonProtocol, ClientTooOld when the daemon rejects OUR hello
// with kCodeClientProtocolTooOld. The GUI renders the two as distinct,
// actionable states, never a blank.
//
// No GTK, no glib, no SDK: plain sockets + the shared protocol header, so the
// class compiles anywhere the tests do. Thread-safe (one internal mutex): the
// GTK main loop and SDK callback threads may both issue calls.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ControlProtocol.hpp"

namespace urnw {

// How the daemon session stands. Everything except Ok is a degraded state the
// UI must render distinctly (MIGRATION.md: "daemon unreachable" and "daemon
// too old" are never a blank or a zero).
enum class DaemonSessionState {
  Ok,            // connected, hello verified in both directions
  Unreachable,   // no daemon on the socket (not installed / not running / not authorized)
  DaemonTooOld,  // daemon protocol < kMinSupportedDaemonProtocol: update urnetworkd
  ClientTooOld,  // daemon rejected our protocol: update this app
  SdkMismatch,   // GUI and daemon carry different SDK builds: update to the same version
  Error,         // daemon answered but the request failed (see error string)
};

// WHY Unreachable, which is three different problems with three different
// fixes. Deliberately a SEPARATE accessor rather than three more
// DaemonSessionState enumerators: the existing switches over that enum are
// exhaustive and live in files this change does not own.
//
// The one that matters in practice is PermissionDenied. The installers create
// the `urnetwork` system group EMPTY and never add the desktop user to it, so
// a fresh install lands on connect(2) = EACCES — which the GUI currently
// renders as "The URnetwork system service is not running. Install or start
// it, then try again.": false, and unactionable. The fix is
// `sudo usermod -aG urnetwork $USER` plus a re-login, and the UI has to be
// able to say so.
enum class DaemonUnreachableReason {
  None,              // not in an Unreachable state
  SocketMissing,     // ENOENT/ECONNREFUSED: not installed, or not running
  PermissionDenied,  // EACCES/EPERM: the socket is there and we may not use it
  Other,             // something else; LastUnreachableError() carries strerror
};

class ControlClient {
 public:
  ControlClient() = default;
  ~ControlClient();

  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;

  // The socket path: $URNETWORK_CONTROL_SOCKET when set (dev/tests), else the
  // normative /run/urnetwork/control.sock.
  static std::string SocketPath();

  // Our urnet::version(), carried in hello and exact-match enforced against
  // the daemon's (ctl::SdkVersionsAgree) — the device RPC is gob with no
  // version field of its own, so the control channel guards it. Set once at
  // startup by the SDK-linking caller (this class itself stays SDK-free);
  // unset fails closed as a mismatch.
  void SetLocalSdkVersion(std::string sdkVersion);

  // Connect + hello (idempotent; cheap when already verified). The returned
  // state is also retained for LastSessionState().
  DaemonSessionState EnsureSession(std::string* error = nullptr);
  DaemonSessionState LastSessionState();
  // hello's daemon_version, "" until a session succeeded ("daemon too old" UI
  // detail; refreshed on every hello)
  std::string DaemonVersion();
  // Why the last connect attempt failed. Meaningful only while the session
  // state is Unreachable.
  DaemonUnreachableReason LastUnreachableReason();
  // The socket path the last attempt used, for the remediation copy.
  std::string LastSocketPath();

  // ---- verbs (each ensures a session first) --------------------------------
  // DEAD PATH, kept only so its one remaining caller (SdkHost.cpp:853) still
  // compiles while it migrates to StartTunnelEx. It carries NO device-RPC
  // pinning material, and pinning is now mandatory in both directions, so this
  // overload can only ever fail: it is refused locally, before a frame reaches
  // the socket, with ctl::kCodeRpcPinRequired in `error`. Falling back to an
  // unpinned root rpc listener is exactly what this change removes, so there is
  // no "compatible" behaviour left to give it. DELETE IT once SdkHost calls
  // StartTunnelEx.
  [[deprecated(
      "device-rpc mTLS pinning is mandatory: call StartTunnelEx with "
      "rpc_server_pem/rpc_client_cert_pem/rpc_listen_hostport")]]
  bool StartTunnel(const std::string& byJwt, const std::string& instanceId,
                   const std::string& appVersion, const std::string& networkSpaceJson,
                   int* rpcPort, std::string* error);

  // The full start_tunnel surface — the ONLY working start path.
  struct StartTunnelOptions {
    std::string by_jwt;
    std::string instance_id;
    std::string app_version;
    std::string network_space_json;
    // Return as soon as the daemon accepts the request (tunnel_state=starting)
    // and poll Status() from there, instead of blocking the GTK main loop for
    // the whole bring-up.
    bool async = false;
    // What the user's kill-switch toggle says. The daemon reports what it
    // ACTUALLY installed in StatusReply::kill_switch.
    bool kill_switch = false;
    // The device-RPC mTLS material, split from ONE
    // urnet::generateDeviceRpcKeyMaterial(): the daemon pins
    // (server_pem, client_cert_pem) and the caller keeps
    // (client_pem, server_cert_pem) for its own DeviceRemote::setRpcServer.
    //
    // ALL THREE ARE REQUIRED. StartTunnelEx runs
    // ctl::ValidateStartTunnelRequest on them BEFORE it sends anything, so a
    // missing or malformed triple is refused here with kCodeRpcPinRequired /
    // kCodeRpcPinInvalid rather than reaching the daemon. rpc_listen_hostport
    // must be literal "127.0.0.1:<port>"; draw the port from
    // [ctl::kRpcPortMin, ctl::kRpcPortMax].
    std::string rpc_server_pem;
    std::string rpc_client_cert_pem;
    std::string rpc_listen_hostport;
  };
  struct StartTunnelOutcome {
    bool ok = false;
    // "up" for a completed start; "starting" when async was requested.
    ctl::TunnelState tunnel_state = ctl::TunnelState::Stopped;
    int rpc_port = 0;
    std::string error;
    // ctl::kCode* — the actionable branch. kCodeStartInProgress in
    // particular means "do NOT retry", not "it failed".
    std::string code;
    DaemonSessionState session = DaemonSessionState::Unreachable;
    // The daemon confirmed it pinned the device RPC to the material we sent.
    // Absent on the wire parses false, so an old daemon that ignored the
    // fields lands here as false and ok=false (see StartTunnelEx).
    //
    // Meaningful only when tunnel_state == Up. On the ASYNC path the start
    // reply is `starting` and this is necessarily false; the caller must gate
    // on StatusReply::rpc_pinned at the transition to Up instead.
    bool rpc_pinned = false;
  };
  // Fail-closed by construction. Beyond the transport it enforces two things
  // the caller can no longer forget:
  //   1) the pinning triple is validated BEFORE the frame is sent;
  //   2) on a SYNCHRONOUS start that comes back Up, the reply must report
  //      rpc_pinned and must echo the port we asked for — otherwise the
  //      outcome is turned into a failure AND the daemon is told to stop,
  //      because a running tunnel whose root rpc listener is unauthenticated
  //      is worse than no tunnel.
  // The async path cannot be gated here; see StartTunnelOutcome::rpc_pinned.
  StartTunnelOutcome StartTunnelEx(const StartTunnelOptions& options);

  bool StopTunnel(std::string* error = nullptr);
  bool SetProvide(const std::string& mode, std::string* error = nullptr);
  // Asks the daemon to install (or lift) the nftables kill switch. `out`
  // receives the status whose kill_switch field is what is REALLY in force —
  // Failed is a distinct state from Off and must be rendered as such.
  bool SetKillSwitch(bool enabled, ctl::StatusReply* out = nullptr,
                     std::string* error = nullptr);
  std::optional<ctl::StatusReply> Status(std::string* error = nullptr);
  bool LocationOverrideAvailable(bool* available, std::string* reason);
  bool LocationOverrideWrite(double lat, double lon, double accuracyM);
  bool LocationOverrideClear();

  void Close();

 private:
  // All *Locked members require mutex_.
  bool ConnectLocked(std::string* error);
  DaemonSessionState HelloLocked(std::string* error);
  DaemonSessionState EnsureSessionLocked(std::string* error);
  void CloseLocked();
  // One request/reply round-trip; nullopt on transport failure (socket closed).
  std::optional<nlohmann::json> RoundTripLocked(const nlohmann::json& request, int64_t id);
  // EnsureSession + round-trip.
  //
  // allowRetry: reconnect once on a dead socket. FALSE for start_tunnel. The
  // old unconditional retry is how a slow bring-up turned into a restart loop:
  // past the receive timeout the client closed the socket and re-sent
  // start_tunnel, the daemon dropped the old owner, accepted the retry, and
  // TunnelHost::Start tore the half-built session down and began again — on
  // exactly the slow networks where the timeout fires.
  //
  // receiveTimeoutSeconds: per-verb. A synchronous start_tunnel legitimately
  // outlives the bound that fits every other verb.
  std::optional<nlohmann::json> CallLocked(ctl::Verb verb, nlohmann::json payload,
                                           std::string* error, bool allowRetry = true,
                                           long receiveTimeoutSeconds = 0);
  // The body of StopTunnel(), so the pinning-refusal path inside
  // StartTunnelEx can tear the daemon's session down without re-entering
  // mutex_ (std::mutex is not recursive: calling StopTunnel() there would
  // deadlock the GUI on its own control client).
  bool StopTunnelLocked(std::string* error);
  void SetReceiveTimeoutLocked(long seconds);
  bool SendAllLocked(const std::string& data);
  // Reads one full line (frame) into `line`, false on EOF/error/timeout.
  bool ReadLineLocked(std::string& line);

  std::mutex mutex_;
  int fd_ = -1;
  bool helloOk_ = false;
  int64_t nextId_ = 1;
  std::string recvBuffer_;
  DaemonSessionState lastState_ = DaemonSessionState::Unreachable;
  DaemonUnreachableReason lastUnreachable_ = DaemonUnreachableReason::None;
  std::string lastSocketPath_;
  std::string daemonVersion_;
  std::string localSdkVersion_;
  long receiveTimeoutSeconds_ = 0;  // what is currently set on fd_
};

}  // namespace urnw
