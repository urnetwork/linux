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

#include <atomic>
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
// PermissionDenied used to be the state a FRESH INSTALL landed in: the
// installers created the `urnetwork` system group empty, so connect(2)
// returned EACCES at a socket that was right there, served by a healthy
// daemon, and the only fix was `usermod -aG urnetwork $USER` plus a re-login.
// A polkit-gated daemon makes the socket world-connectable and decides per
// action instead, so EACCES can no longer happen against one — which NARROWS
// this reason to "the daemon on the other end is still group-gated" and keeps
// the log-out-and-back-in copy correct for exactly that case and no other.
//
// A daemon that reaches us and then REFUSES is not this. It is
// DaemonAuthOutcome below, and the two must never render the same sentence.
enum class DaemonUnreachableReason {
  None,              // not in an Unreachable state
  SocketMissing,     // ENOENT/ECONNREFUSED: not installed, or not running
  PermissionDenied,  // EACCES/EPERM: the socket is there and we may not use it.
                     // Under a polkit-gated daemon the socket is world-
                     // connectable and this CANNOT happen, so it now means
                     // exactly one thing: the daemon on the other end is still
                     // group-gated (an old build, or a machine with no polkit,
                     // where it re-tightens the socket to 0660 root:urnetwork).
                     // That is the ONE case where the log-out-and-back-in
                     // remediation is still the correct advice.
  StaleSandboxMount, // EACCES inside a Flatpak whose bind mount of the socket
                     // directory has gone stale: the daemon restarted, systemd
                     // recreated /run/urnetwork, and this sandbox still holds
                     // the DELETED inode. Nothing is wrong with the daemon or
                     // the user's groups — only relaunching the app fixes it.
  Other,             // something else; LastUnreachableError() carries strerror
};

// ---- authorization (polkit) -------------------------------------------------
// WHY THIS IS NOT A DaemonUnreachableReason, and not a DaemonSessionState.
// A polkit refusal is the OPPOSITE of unreachable: the socket opened, `hello`
// succeeded, and the daemon answered a verb with a reason. Folding it into
// either of those enums is how "your administrator has not allowed this" would
// end up rendering through a SocketMissing/default arm as "The URnetwork system
// service is not running" — false, unactionable, and the exact failure this
// whole change exists to remove. So it is an ORTHOGONAL third axis: session
// state says whether we have a daemon, unreachable reason says why we do not,
// and this says what the daemon decided about *us*.
//
// It is also why the existing exhaustive switches over the other two enums (in
// KillSwitchCopy.hpp and files this change does not own) keep compiling
// untouched.
enum class DaemonAuthOutcome {
  None,               // the last reply carried no authorization verdict
  Authorized,         // a privileged verb was accepted (polkit said yes, or uid 0)
  Denied,             // a hard no: site policy refuses this subject outright
  ChallengeRequired,  // authentication is needed and none was obtained (no agent:
                      // typically an ssh/session-less caller), or the check was
                      // made non-interactively
  Dismissed,          // the user closed the polkit dialog. RENDER NOTHING for
                      // this: a user who changed their mind did not hit an error
                      // (docs/parity/settings.md:130).
  Unavailable,        // no polkit on this machine; the daemon fell back to the
                      // `urnetwork` group and that group refused us
  CheckFailed,        // the check itself could not be made (bus error, polkitd
                      // gone, peer /proc unreadable). Fail closed, say so.
  TimedOut,           // the prompt went unanswered inside the daemon's bound
  NotTunnelOwner,     // a DIFFERENT uid owns the running tunnel and take-over
                      // was not granted
};

// Which authority the daemon told us it is gating with, from `hello`. Absent on
// the wire (any daemon predating polkit) parses as Group, which is what such a
// daemon is — the safe direction, because Group is the only mode whose
// remediation mentions groups and re-login.
enum class DaemonAuthMode {
  Group,   // ctl::AuthorizeControlPeer: uid 0 or the `urnetwork` group
  Polkit,  // org.freedesktop.PolicyKit1 per-action checks
};

// The reply `code` strings the daemon uses for the verdicts above.
//
// TODO(polkit): these belong in ControlProtocol.hpp next to the other kCode*
// constants, so the two halves cannot drift. They are declared here only
// because this change owns ControlClient.{hpp,cpp} and MainWindow.cpp and
// nothing else; the daemon half must define the SAME strings, and whoever moves
// them should delete this block rather than leave two sources of truth.
namespace authz {
inline constexpr const char* kCodeAuthDenied = "auth_denied";
// Two accepted spellings for the same verdict: the daemon design calls it
// `auth_required`, the authorization-model design calls it
// `auth_challenge_required`. Accepting both costs one string compare and means
// a client and a daemon written from different halves of the same design still
// render the right sentence instead of falling through to a raw diagnostic.
inline constexpr const char* kCodeAuthRequired = "auth_required";
inline constexpr const char* kCodeAuthChallengeRequired = "auth_challenge_required";
inline constexpr const char* kCodeAuthDismissed = "auth_dismissed";
inline constexpr const char* kCodeAuthUnavailable = "auth_unavailable";
inline constexpr const char* kCodeAuthCheckFailed = "auth_check_failed";
inline constexpr const char* kCodeAuthTimeout = "auth_timeout";
inline constexpr const char* kCodeAuthNotTunnelOwner = "auth_not_tunnel_owner";
}  // namespace authz

// Pure, so it can be unit-tested without a daemon. Anything unrecognised —
// including every pre-existing kCode* — is None, i.e. "not an authorization
// verdict", which is what keeps the tun/route/dns failure codes rendering their
// own copy.
DaemonAuthOutcome AuthOutcomeFromCode(const std::string& code);

// True for the outcomes that mean "the daemon answered and refused". Dismissed
// is deliberately INCLUDED: it is a refusal the UI must own (by saying nothing),
// not something to fall through to a generic "could not start" line.
inline bool IsAuthRefusal(DaemonAuthOutcome outcome) {
  switch (outcome) {
    case DaemonAuthOutcome::Denied:
    case DaemonAuthOutcome::ChallengeRequired:
    case DaemonAuthOutcome::Dismissed:
    case DaemonAuthOutcome::Unavailable:
    case DaemonAuthOutcome::CheckFailed:
    case DaemonAuthOutcome::TimedOut:
    case DaemonAuthOutcome::NotTunnelOwner:
      return true;
    case DaemonAuthOutcome::None:
    case DaemonAuthOutcome::Authorized:
      return false;
  }
  return false;
}

// Which verbs the daemon gates. Used only to decide whether a SUCCESSFUL reply
// means "we were authorized" (Authorized) or "nothing was asked" (None) —
// `status` and `hello` stay unauthenticated so the UI can render service state
// before any grant exists.
inline bool VerbNeedsAuthorization(ctl::Verb verb) {
  switch (verb) {
    case ctl::Verb::StartTunnel:
    case ctl::Verb::StopTunnel:
    case ctl::Verb::SetProvide:
    case ctl::Verb::SetKillSwitch:
    case ctl::Verb::LocationOverrideWrite:
    case ctl::Verb::LocationOverrideClear:
      return true;
    case ctl::Verb::Hello:
    case ctl::Verb::Status:
    case ctl::Verb::LocationOverrideAvailable:
    case ctl::Verb::Unknown:
      return false;
  }
  return false;
}

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

  // ---- authorization verdict of the LAST daemon reply -----------------------
  // Meaningful only in combination with ReplySerial(): this describes the most
  // recent reply this client processed, and a caller's own failure path may not
  // have produced one (SdkHost::StartTunnel refuses "not signed in" and a bad
  // rpc key material without sending anything). Snapshot ReplySerial() before
  // the call, compare after, and believe LastAuthOutcome() only when it moved —
  // otherwise it is a verdict about an EARLIER request and rendering it would
  // put a stale "an administrator can change that" under an unrelated failure.
  DaemonAuthOutcome LastAuthOutcome();
  // polkit's own result string ("auth_admin_keep", "auth_self", …) when the
  // daemon passed it back, "" otherwise. For labelling the affordance BEFORE
  // the user presses the button; never rendered raw.
  std::string LastPolkitResult();
  // What `hello` said the daemon gates with. Group until a hello reports
  // otherwise — an old daemon says nothing and IS group-gated.
  DaemonAuthMode LastAuthMode();
  // Monotonic count of daemon replies processed. See LastAuthOutcome(). Atomic
  // and lock-free, like SessionGeneration(), so the GTK main loop can read it
  // either side of a blocking call without contending on mutex_.
  uint64_t ReplySerial() const { return replySerial_.load(); }

  // WHICH daemon connection we are on. Incremented on every completed hello,
  // i.e. every time a NEW connection is established — so a change means the
  // previous connection died and was rebuilt, which for this daemon means the
  // service restarted and every session object it held (the DeviceLocal, its
  // pinned rpc listener, the tunnel ownership) is gone.
  //
  // This exists because a live `urnet::DeviceRemote` handle is NOT proof of a
  // live tunnel: the handle is owned by this process and nothing invalidates
  // it when the daemon-side half disappears. Callers that cache a device
  // record the generation they bound it on and compare it here — an O(1),
  // lock-free read they can do on the GTK main loop — instead of believing a
  // handle forever. See SdkHost::hasDevice().
  uint64_t SessionGeneration() const { return sessionGeneration_.load(); }

  // ---- verbs (each ensures a session first) --------------------------------
  // The full start_tunnel surface — the ONLY start path. (The unpinned
  // convenience overload that used to sit here is gone: it carried no
  // device-rpc material, so ValidateStartTunnelRequest refused every call
  // locally, and leaving it declared only made it available to be called by
  // mistake.)
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
  // Is the CACHED connection still there? A cached fd is not evidence of a
  // live daemon — a service restart (or a reinstall) closes every connection
  // and nothing tells this process until the next write fails. Cheap: one
  // non-blocking poll(), plus a MSG_PEEK only when something is readable.
  // Errs toward "alive" when it cannot tell, so it can never invent a
  // disconnect. Requires mutex_.
  bool SessionAliveLocked();
  // One request/reply round-trip; nullopt on transport failure (socket
  // closed). `frameDelivered` (optional) reports whether the COMPLETE request
  // frame reached the daemon — a frame is one newline-terminated line, so a
  // short write cannot have been decoded, let alone acted on. That is the
  // difference between "the daemon may have acted on this" and "this request
  // never happened", which is what decides whether a non-idempotent verb may
  // be sent again.
  std::optional<nlohmann::json> RoundTripLocked(const nlohmann::json& request, int64_t id,
                                                bool* frameDelivered = nullptr);
  // EnsureSession + round-trip.
  //
  // allowRetry: re-SEND once on a dead socket. FALSE for start_tunnel. The
  // old unconditional retry is how a slow bring-up turned into a restart loop:
  // past the receive timeout the client closed the socket and re-sent
  // start_tunnel, the daemon dropped the old owner, accepted the retry, and
  // TunnelHost::Start tore the half-built session down and began again — on
  // exactly the slow networks where the timeout fires.
  //
  // It does NOT mean "one attempt". A request whose bytes never left this
  // process cannot have been acted on by anybody, so it is reconnected and
  // sent once regardless of allowRetry; only a request that was actually
  // written and then went unanswered is abandoned. Both decisions are logged.
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
  // Records the authorization verdict carried by one completed reply and bumps
  // replySerial_. Called for EVERY verb reply, including the ones that carry no
  // verdict, because "this reply said nothing about authorization" has to
  // overwrite the previous reply's verdict — a stale Denied is worse than none.
  void NoteReplyLocked(ctl::Verb verb, const nlohmann::json& reply);
  // Clears the verdict WITHOUT bumping the serial, for the paths that refuse
  // locally and never reach the daemon at all.
  void ResetAuthLocked();
  void SetReceiveTimeoutLocked(long seconds);
  // `sent` (optional) receives the number of bytes that reached the peer,
  // valid on the failure path too, where it is the difference between "the
  // socket was already dead" and "it died mid-frame".
  bool SendAllLocked(const std::string& data, size_t* sent = nullptr);
  // Reads one full line (frame) into `line`, false on EOF/error/timeout.
  bool ReadLineLocked(std::string& line);

  std::mutex mutex_;
  int fd_ = -1;
  bool helloOk_ = false;
  int64_t nextId_ = 1;
  std::string recvBuffer_;
  DaemonSessionState lastState_ = DaemonSessionState::Unreachable;
  DaemonUnreachableReason lastUnreachable_ = DaemonUnreachableReason::None;
  DaemonAuthOutcome lastAuth_ = DaemonAuthOutcome::None;
  std::string lastPolkitResult_;
  // Group, not Polkit, when nothing has told us: a daemon that says nothing
  // about auth_mode is a daemon that predates polkit gating.
  DaemonAuthMode lastAuthMode_ = DaemonAuthMode::Group;
  std::string lastSocketPath_;
  std::string daemonVersion_;
  std::string localSdkVersion_;
  long receiveTimeoutSeconds_ = 0;  // what is currently set on fd_
  // Bumped by HelloLocked on success. Atomic because SessionGeneration() is
  // read without mutex_ (from the GTK main loop, on every hasDevice()).
  std::atomic<uint64_t> sessionGeneration_{0};
  // Bumped by NoteReplyLocked. Atomic for the same reason as
  // sessionGeneration_: read from the GTK main loop without mutex_.
  std::atomic<uint64_t> replySerial_{0};
};

}  // namespace urnw
