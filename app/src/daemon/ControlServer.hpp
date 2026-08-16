// The urnetworkd control-socket server (ControlProtocol.hpp): AF_UNIX/
// SOCK_STREAM at /run/urnetwork/control.sock, newline-delimited JSON, driven
// by the daemon's GMainLoop (plain glib — the daemon has no GTK).
//
// AUTHORIZATION — polkit, with the unix group only as a fallback.
//
// The group check alone was the problem: supplementary groups are applied at
// LOGIN, so a user the installer had just added to `urnetwork` could not talk
// to this daemon at all until they logged out and back in. polkit authorizes a
// SUBJECT for an ACTION (allow_any / allow_inactive / allow_active), so the
// person at this machine's screen is authorized in their CURRENT session, with
// no group change and no re-login. That is what NetworkManager, firewalld and
// rpm-ostree do.
//
//   * SO_PEERCRED on every accept(), BEFORE parsing any frame — UNCHANGED, and
//     still the only identity a unix socket can authenticate. Its job moved
//     from POLICY to IDENTITY: it is the uid-0 fast path, the uid+gids of the
//     polkit subject, the tunnel-ownership uid, the per-uid connection cap and
//     every audit line. Never authorize on pid (CVE-2019-6133): the pid is a
//     LOCATOR handed to polkitd beside the uid, pinned by SO_PEERPIDFD where
//     the kernel offers it (no reuse window at all — the kernel pins the peer's
//     struct pid at connect(2), exactly as it does the ucred) and otherwise
//     paired with the /proc start-time polkitd itself cross-checks.
//   * Each privileged verb goes through RequireAuth() -> one of the four
//     ctl::kAction* ids. hello and status are deliberately NOT gated: a peer
//     that cannot authorize must still be TOLD WHY (version/SDK skew) rather
//     than dropped, and status is polled at ~4 Hz — it is REDACTED for a
//     foreign uid instead of refused.
//   * FALLBACK: when this machine has no ctl::kPolkitPolicyPath the daemon
//     latches `group` mode at start and behaves byte-for-byte as before —
//     socket 0660 root:urnetwork, ctl::AuthorizeControlPeer, accept-time drop.
//     The discriminator is an INSTALL FACT, never a runtime probe: keying on
//     "polkitd answered" would make `kill polkitd` a policy downgrade.
//   * Under polkit the socket is 0666: you cannot render a legible refusal from
//     a socket you were not allowed to open. DAC stops being the boundary, so
//     the DoS caps below (per-uid/global connection counts, a hello deadline,
//     a frame-rate bucket) are load-bearing, not optional hardening.
//
// Version skew (§11b): `hello` is mandatory and carries protocol_version in
// both directions. A client below kMinSupportedClientProtocol is rejected
// with kCodeClientProtocolTooOld; every other verb before a successful hello
// gets kCodeHelloRequired — which is what makes the negotiation actually
// enforced rather than declared (the Windows twin declares and never checks).
// The hello REPLY now also carries auth_mode, so the GUI only offers the
// log-out-and-back-in remediation on a daemon where it is true.
//
// Concurrency: multiple clients connect concurrently; requests are handled
// serially on the main loop, but a request may now be ANSWERED LATER — an
// interactive polkit check takes human time and must not block the loop. So
// Dispatch takes a ReplyFn and a connection ID (never a Connection*, which the
// peer can free mid-check by disconnecting while the password dialog is up),
// and a connection with a check in flight STOPS being read from, so a client
// cannot queue hundreds of checks and carpet the user's screen with dialogs.
//
// The tunnel has ONE owner — the first authorized client whose start_tunnel
// succeeds — and now also ONE OWNING UID. A start/stop/set_provide from a
// different live client still gets kCodeTunnelOwnedByOtherClient; from a
// different UID it additionally costs ctl::kActionTakeOverTunnel, which is
// auth_admin_keep even at the console. root may always act.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glib.h>

#include "ControlProtocol.hpp"
#include "LocationOverride.hpp"
#include "daemon/TunnelHost.hpp"

namespace urnw {

// Defined in ControlServer.cpp — a thin async GDBus wrapper over
// org.freedesktop.PolicyKit1.Authority. Held by unique_ptr so the GDBus/gio
// surface stays out of every TU that includes this header (the GUI's
// ControlClient includes ControlProtocol.hpp, not this).
class PolkitAuthorizer;

// Everything the kernel will tell us about the peer, captured at accept()
// before any byte is read. Never populated from anything a client sends: the
// daemon is uid 0, and polkit treats uid 0 as a trusted caller allowed to
// submit subjects for OTHER identities, so a request field reaching this struct
// would be a silent, total local privilege escalation.
struct PeerIdentity {
  int64_t uid = -1;           // SO_PEERCRED, translated into OUR user ns
  int64_t gid = -1;           // SO_PEERCRED primary gid
  int64_t pid = -1;           // SO_PEERCRED, in OUR pid ns (the host's)
  uint64_t start_time = 0;    // /proc/<pid>/stat field 22, read once at accept
  bool start_time_known = false;
  int pidfd = -1;             // SO_PEERPIDFD; OWNED, must be close()d
  std::vector<int64_t> gids;  // host-resolved, NEVER peer-reported
};

class ControlServer {
 public:
  // The daemon is the privileged GeoClue writer; the GUI's state machine
  // drives it over the three location_override_* verbs.
  ControlServer(TunnelHost& tunnel, GeoClueWriter& geoWriter);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // The socket path: $URNETWORK_CONTROL_SOCKET when set (dev), else the
  // normative /run/urnetwork/control.sock.
  static std::string SocketPath();
  // Our polkit action file: $URNETWORK_POLKIT_POLICY when set (dev), else
  // ctl::kPolkitPolicyPath. Its PRESENCE is the authority discriminator.
  static std::string PolicyPath();
  // access(PolicyPath(), F_OK) == 0, evaluated live. Latched into authMode_ at
  // Start(); main.cpp also calls it for --diagnose.
  static bool PolkitPolicyPresent();
  // Is polkit itself installed? An install fact, checked alongside our action
  // file so a package that ships the .policy onto a polkit-less host falls back
  // to the group instead of failing shut. Never a runtime "did polkitd answer".
  static bool PolkitRuntimePresent();

  // Latches the authority, creates the socket dir and binds:
  //   polkit  dir 0755 root:root,      sock 0666 root:root
  //   group   dir 0750 root:urnetwork, sock 0660 root:urnetwork
  // then starts accepting on the default main context.
  bool Start();
  void Stop();

  // ctl::kAuthModePolkit | ctl::kAuthModeGroup — valid after Start().
  const char* AuthModeName() const;

  // Daemon-version string reported in the hello reply.
  void SetDaemonVersion(std::string version) { daemonVersion_ = std::move(version); }
  // This binary's urnet::version(), exact-match enforced against the client's
  // hello (ctl::SdkVersionsAgree): the gob device RPC has no version field of
  // its own, so the control channel is the only place a drifted SDK pair can
  // be caught loudly instead of failing silently.
  void SetSdkVersion(std::string version) { sdkVersion_ = std::move(version); }

 private:
  enum class AuthMode { Polkit, Group };

  struct Connection {
    int fd = -1;
    // Stable and monotonic. THIS is what a deferred reply captures — never a
    // Connection*, which CloseConnection frees while a check may still be in
    // flight (the peer disconnecting mid-prompt is the NORMAL case: the user
    // quits the app while the password dialog is up).
    uint64_t id = 0;
    guint watchId = 0;
    PeerIdentity peer;
    bool helloOk = false;  // a successful, version-checked hello happened
    // Fallback mode only: the accept-time ctl::AuthorizeControlPeer result.
    bool groupAuthorized = false;
    // A polkit check is in flight for this connection. While it is set we STOP
    // consuming lines, so a client cannot queue prompts.
    bool authPending = false;
    // Positive polkit results, per action, for THIS connection only — never
    // per-uid, never global, and they die with the connection. Keeps a polled
    // verb off the bus and keeps a multi-step Connect from re-prompting.
    // kActionTakeOverTunnel is deliberately never cached.
    std::set<std::string> grants;
    // Checks owned by this connection, to be cancelled from CloseConnection.
    std::vector<uint64_t> checks;
    // Frame-rate bucket (the socket is world-connectable under polkit).
    int64_t rateWindowMs = 0;
    int rateFrames = 0;
    // Dropped when hello has not arrived in time.
    guint helloDeadlineId = 0;
    std::string inBuf;
  };

  using ReplyFn = std::function<void(nlohmann::json)>;

  static gboolean OnAcceptReady(gint fd, GIOCondition condition, gpointer data);
  static gboolean OnConnectionReadable(gint fd, GIOCondition condition, gpointer data);
  static gboolean OnHelloDeadline(gpointer data);

  void AcceptOne();
  // Parses and dispatches whatever is buffered, stopping at the first request
  // that defers. false => the connection was closed and freed.
  bool PumpConnection(uint64_t connId);
  bool ReadIntoBuffer(Connection* conn);
  void CloseConnection(Connection* conn);
  bool SendFrame(Connection* conn, const nlohmann::json& frame);
  // Delivers a reply produced after Dispatch returned, then resumes reading.
  void DeliverDeferredReply(uint64_t connId, nlohmann::json reply);
  Connection* FindConnection(uint64_t connId);

  // Answers via `reply` either inline or later. NEVER captures a Connection*.
  void Dispatch(uint64_t connId, const nlohmann::json& request, ReplyFn reply);
  // The body of a verb whose polkit check has already passed.
  // authorizedCrossUid: the crossUid value that SELECTED the polkit action for
  // this request. Carried rather than re-derived — see CheckTunnelOwner.
  void DispatchAuthorized(uint64_t connId, int64_t id, ctl::Verb verb, bool isLogTail,
                          const nlohmann::json& request, const ReplyFn& reply,
                          bool authorizedCrossUid);
  nlohmann::json HandleHello(Connection* conn, int64_t id, const nlohmann::json& request);
  nlohmann::json HandleStartTunnel(Connection* conn, int64_t id, const nlohmann::json& request,
                                   bool authorizedCrossUid);
  // Frame-rate bucket; false => the peer is flooding and is dropped.
  bool AllowFrame(Connection* conn);
  // Only under polkit, and only for a peer that is neither root nor the uid
  // that owns the running tunnel.
  bool StatusMustBeRedactedFor(const Connection* conn) const;
  // log_tail's gate. NOT TunnelOwnedByOtherUid: the log ring outlives the
  // tunnel, so ownership of the LOG must not lapse when the tunnel stops.
  bool LogBelongsToOtherUid(const Connection* conn) const;

  // THE single choke point every privileged verb goes through. Applies, in
  // order: uid 0 fast path -> per-connection grant cache -> group fallback (in
  // `group` mode) -> PolkitAuthorizer::CheckAsync -> fail closed. next(true,{})
  // to proceed, next(false, denied_reply) to refuse. Every refusal is logged
  // with the uid, the pid, the action id and the reason before it returns.
  void RequireAuth(uint64_t connId, const char* actionId, bool interactive, int64_t replyId,
                   std::function<void(bool ok, nlohmann::json denied)> next);

  // owner gate for tunnel-lifecycle verbs; fills *denied with the error reply.
  // *crossUid is written out so the caller can select kActionTakeOverTunnel.
  // authorizedTakeOver: was THIS request authorized as a cross-uid take-over,
  // i.e. was kActionTakeOverTunnel the action polkit actually checked? It is
  // the crossUid value captured when the action was selected, not a fresh
  // derivation — re-deriving it here is what let an unprompted control-tunnel
  // check stand in for an admin one when ownership changed mid-flight.
  bool CheckTunnelOwner(Connection* conn, int64_t id, nlohmann::json* denied, bool* crossUid,
                        bool authorizedTakeOver);
  // True when a live/starting tunnel is owned by a uid other than this peer's.
  bool TunnelOwnedByOtherUid(const Connection* conn) const;
  void ClaimTunnelOwnership(Connection* conn);

  TunnelHost& tunnel_;
  GeoClueWriter& geoWriter_;
  std::string daemonVersion_;
  std::string sdkVersion_;

  AuthMode authMode_ = AuthMode::Group;
  std::unique_ptr<PolkitAuthorizer> polkit_;

  int listenFd_ = -1;
  guint listenWatchId_ = 0;
  std::string boundPath_;
  uint64_t nextConnId_ = 1;
  std::map<uint64_t, std::unique_ptr<Connection>> connections_;  // by id
  std::map<int, uint64_t> connIdByFd_;                           // watch -> id
  std::map<int64_t, int> connectionsPerUid_;                     // DoS cap

  // the connection whose start_tunnel succeeded last; nullptr when it went
  // away (tunnel keeps running, ownership adoptable)
  Connection* tunnelOwner_ = nullptr;
  // The OWNING UID, which — unlike tunnelOwner_ — outlives the owner's
  // disconnect. Gating on the connection alone let any peer adopt or displace a
  // tunnel whose owner GUI simply was not attached at that moment.
  int64_t tunnelOwnerUid_ = -1;
  // Who the daemon log ring belongs to, and whether it has become a mixture.
  // Unlike tunnelOwnerUid_ these do not lapse when a tunnel stops.
  int64_t logOwnerUid_ = -1;
  bool logMixedUids_ = false;
  // the connection that last wrote the geo override; its disconnect clears
  // the override — never keep reporting a city nobody is tracking
  Connection* overrideWriter_ = nullptr;
};

}  // namespace urnw
