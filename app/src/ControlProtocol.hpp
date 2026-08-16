// URnetwork Linux control protocol — the contract between the unprivileged GUI
// (urnetwork, urnet::DeviceRemote) and the root daemon (urnetworkd,
// urnet::DeviceLocal + tun + IoLoop). The normative definition lives in
// linux/MIGRATION.md — every verb, payload field and path here mirrors it.
//
// Transport: AF_UNIX/SOCK_STREAM at /run/urnetwork/control.sock,
// newline-delimited UTF-8 JSON: one request object per line, one reply object
// per line. Requests are {"verb":…,"id":N,…payload}; replies are
// {"id":N,"ok":bool,…payload}. Unknown/absent fields are tolerated on both
// sides (the SDK wrapper's forward-compatible convention), which is what lets
// the protocol version below evolve additively.
//
// Unlike the Windows Protocol.h twin, the protocol version is actually
// ENFORCED, in both directions (APPIMAGE.md §11b): `hello` carries
// protocol_version both ways; the daemon rejects a client below
// kMinSupportedClientProtocol and the GUI rejects a daemon below
// kMinSupportedDaemonProtocol. The two halves update on independent schedules
// (apt/install.sh vs. AppImage zsync), so unlike every other URnetwork
// platform the skew check has teeth here.
//
// This header is shared by both binaries and by the unit tests: it must stay
// free of GTK, glib and the SDK — plain C++17 + nlohmann_json only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace urnw::ctl {

// Bump ONLY on incompatible wire-format changes, decoupled from the app
// version (hello negotiates it; see the enforcement note above).
inline constexpr int kControlProtocolVersion = 1;
// The daemon refuses a client hello carrying a protocol_version below this.
inline constexpr int kMinSupportedClientProtocol = 1;
// The GUI refuses a daemon hello reply carrying a protocol_version below this.
inline constexpr int kMinSupportedDaemonProtocol = 1;

// Socket location (MIGRATION.md table). Never abstract — permissions are
// meaningless in the abstract namespace.
//
// The MODE is no longer fixed, because it now encodes WHICH AUTHORITY is in
// force (see the polkit block below):
//   polkit mode  dir 0755 root:root,      sock 0666 root:root
//   group mode   dir 0750 root:urnetwork, sock 0660 root:urnetwork  (as before)
// Under polkit the socket is deliberately world-connectable: you cannot render
// a legible refusal from a socket you were not allowed to open. EACCES at
// connect(2) is a bare errno with no reason and no remedy; a refusal that
// arrives as a protocol frame carries a code, a sentence and an action id.
// DAC stops being the boundary there — SO_PEERCRED plus polkit is.
inline constexpr const char* kControlSocketDir = "/run/urnetwork";
inline constexpr const char* kControlSocketPath = "/run/urnetwork/control.sock";
// The unix group whose members (plus uid 0) may use the control socket — ONLY
// on the fallback path now. See AuthorizeControlPeer below.
inline constexpr const char* kControlGroupName = "urnetwork";

// ---- authorization: polkit first, the unix group only as a fallback --------
//
// The re-login requirement is gone. Supplementary groups are applied at LOGIN,
// so a user the installer had just added to `urnetwork` could not talk to the
// daemon AT ALL until they logged out and back in — a first-run experience no
// shipping VPN asks for. polkit authorizes a SUBJECT for an ACTION, with
// allow_any / allow_inactive / allow_active, so the person at this machine's
// screen is authorized in their CURRENT session with no group change. This is
// what NetworkManager (org.freedesktop.NetworkManager.network-control),
// firewalld and rpm-ostree all do; it is not a novel arrangement.
//
// THE FALLBACK DISCRIMINATOR IS AN INSTALL FACT, NOT A RUNTIME PROBE: the
// presence of OUR OWN action file at kPolkitPolicyPath. Keying on "polkitd
// answered" would make `kill polkitd` a policy-downgrade attack — kill the
// service, fall back to the differently-shaped group check. An install fact
// cannot be induced at runtime by an unprivileged local process.
//
//   policy file present  -> polkit is the sole authority. A check that cannot
//                           be completed is a DENY (fail closed), never a
//                           silent downgrade to the group.
//   policy file absent   -> byte-for-byte today's behaviour: 0660
//                           root:urnetwork and AuthorizeControlPeer.
inline constexpr const char* kPolkitPolicyPath =
    "/usr/share/polkit-1/actions/network.ur.urnetwork.policy";

// HelloReply::auth_mode — which authority this daemon actually latched at
// start. The GUI needs it because the "add yourself to the urnetwork group,
// then log out and back in" remediation is correct under `group` and WRONG
// under `polkit`.
inline constexpr const char* kAuthModePolkit = "polkit";
inline constexpr const char* kAuthModeGroup = "group";

// The four polkit action ids, namespaced to the app id. Shipped in
// packaging/polkit/network.ur.urnetwork.policy (0644 root:root — polkit
// ignores group- or world-writable action files). They are checked by
// urnetworkd, never by the GUI: the subject is built from SO_PEERCRED on the
// connection being served, so a client can neither nominate its own subject
// nor skip the check by not asking.
inline constexpr const char* kActionControlTunnel = "network.ur.urnetwork.control-tunnel";
inline constexpr const char* kActionManageKillSwitch =
    "network.ur.urnetwork.manage-kill-switch";
inline constexpr const char* kActionTakeOverTunnel = "network.ur.urnetwork.take-over-tunnel";
inline constexpr const char* kActionReadLog = "network.ur.urnetwork.read-log";

// The SDK's built-in default device-RPC address (sdk/device_rpc.go:109,
// deviceRpcDefaultAddress = "127.0.0.1:12025"). Kept as a NAMED CONSTANT ONLY,
// deliberately no longer a fallback: since the mTLS pinning triple became
// mandatory (see ValidateStartTunnelRequest) the daemon never listens here,
// because an unpinned listener on this port is reachable by any local process
// that can open a TCP socket — including one the control socket's SO_PEERCRED
// + `urnetwork` group check would refuse. Its remaining job is to be the port
// the GUI's random draw EXCLUDES, so an echoed rpc_port can never match by
// accident against a peer that ignored the pin.
inline constexpr int kDeviceRpcPort = 12025;

// The GUI draws a fresh loopback port per session from this closed range. It
// starts one ABOVE kDeviceRpcPort on purpose (Windows draws 12000..12100 and
// so has a ~1% chance of a vacuously-passing port cross-check). A random port
// does not defeat squatting — it stops an unrelated service already holding
// 12025 from presenting as a pinning failure.
inline constexpr int kRpcPortMin = 12026;
inline constexpr int kRpcPortMax = 12125;

// Per-PEM cap. The 1 MiB frame bound in ControlServer/ControlClient limits the
// LINE, not the individual field, so without this a single start_tunnel could
// hand the root daemon two ~500 KiB "PEMs" to parse.
inline constexpr size_t kMaxRpcPemBytes = 64 * 1024;

// ---- version negotiation (pure, unit-tested) -------------------------------
// One direction each; together they cover both rejection directions. A peer
// NEWER than this build is accepted here — the newer side is the one that
// knows whether compatibility was dropped, and its own min-check rejects us.

constexpr bool DaemonAcceptsClientProtocol(int client_protocol) {
  return client_protocol >= kMinSupportedClientProtocol;
}

constexpr bool ClientAcceptsDaemonProtocol(int daemon_protocol) {
  return daemon_protocol >= kMinSupportedDaemonProtocol;
}

// ---- SDK version agreement (pure, unit-tested) -----------------------------
// A SECOND, independent, stricter check — deliberately not conflated with the
// control protocol version. The device RPC between the GUI's DeviceRemote and
// the daemon's DeviceLocal is the SDK's own gob-encoded channel with NO
// version field of its own (sdk/device_rpc.go: DeviceRemoteSyncRequest
// carries only the pairing InstanceId + listener ids), and gob's tolerance
// makes drift fail silently (a renamed field decodes as zero) rather than
// loudly. Every other platform ships both halves in one artifact so the two
// SDK copies are byte-identical; Linux is the first where they update on
// independent schedules (AppImage zsync vs. apt/install.sh). So: `hello`
// carries each side's urnet::version() and BOTH sides require an EXACT match
// before any DeviceRemote is constructed. Both halves come out of the same
// release pipeline stamped with the same version, so any difference is a
// genuinely mismatched pair. An empty version (a peer predating this field)
// fails closed.
inline bool SdkVersionsAgree(const std::string& local_sdk, const std::string& peer_sdk) {
  return !local_sdk.empty() && local_sdk == peer_sdk;
}

// ---- peer-credential authorization (pure, unit-tested) ---------------------
// UNCHANGED, still unit-tested, and now THE FALLBACK PREDICATE rather than the
// primary gate: it decides only on machines with no kPolkitPolicyPath (minimal
// images, a tarball install that skipped the actions dir). Where polkit is
// present this function is not consulted at all and group membership is inert.
//
// SO_PEERCRED IS NOT REPLACED — it changes job from POLICY to IDENTITY. It is
// still resolved on every accept(), BEFORE parsing any frame, and it remains
// the source of: the uid-0 fast path (root recovery must work with the system
// bus down), the uid and gids in the polkit subject (polkitd cross-checks the
// uid — that is the CVE-2019-6133 "slowfork" defence), the identity for this
// fallback predicate, the per-uid connection accounting, the tunnel-ownership
// uid, and every authorization audit line.
//
// The socket-facing code resolves SO_PEERCRED, then resolves the peer's group
// list via getpwuid + getgrouplist and the control group's gid via getgrnam —
// and hands the plain numbers here. Policy (MIGRATION.md): allow uid 0 and
// members of the `urnetwork` group. NEVER authorize on pid (CVE-2019-6133) —
// under polkit the pid is a LOCATOR handed to polkitd beside the uid, never an
// identity, and it is pinned by SO_PEERPIDFD where the kernel offers it. Never
// attempt peer-binary attestation (root cannot read an AppImage's FUSE mount —
// APPIMAGE.md §11c: uid is the only identity a unix socket can authenticate).
//
// peer_group_ids: every gid the peer uid belongs to (primary + supplementary).
// control_group_id: the `urnetwork` group's gid, or a negative value when the
// group does not exist on this system — in which case only root is allowed
// (fail closed; an absent group must never widen access).
inline bool AuthorizeControlPeer(int64_t peer_uid, int64_t peer_gid,
                                 const std::vector<int64_t>& peer_group_ids,
                                 int64_t control_group_id) {
  if (peer_uid == 0) return true;
  if (control_group_id < 0) return false;
  if (peer_gid == control_group_id) return true;
  for (const int64_t gid : peer_group_ids) {
    if (gid == control_group_id) return true;
  }
  return false;
}

// ---- device-RPC mTLS pinning (pure, unit-tested) ---------------------------
// The SDK's loopback device RPC pins with EXACT raw-certificate equality, and
// the two halves keep SEPARATE SDK storage roots (~/.local/share/urnetwork vs
// /var/lib/urnetwork/sdk), so no "defaults" path can ever produce a matching
// pinned pair. That is what makes ABSENT unambiguous, and therefore what lets
// this contract be fail-closed in both directions:
//
//   * the GUI generates ONE urnet::DeviceRpcKeyMaterial and splits it —
//     (server_pem, client_cert_pem) travel UP to the daemon on start_tunnel,
//     (client_pem, server_cert_pem) never leave the GUI;
//   * an old GUI that sends nothing is REFUSED here (kCodeRpcPinRequired)
//     rather than quietly getting root's DeviceLocal on an unpinned
//     127.0.0.1:12025, which any local process could drive;
//   * an old daemon that ignores the fields is refused by the GUI, which
//     branches on StartTunnelReply/StatusReply::rpc_pinned.
//
// THREAT MODEL, stated precisely so it is not oversold: this defends against
// local processes NOT authorized on /run/urnetwork/control.sock. It does not
// defend against a member of the `urnetwork` group — that peer can already
// stop and start the tunnel over the control socket.

// Rejection codes for the pinning contract. They live HERE, above
// ValidateStartTunnelRequest, because that validator returns them; the main
// kCode* table further down cross-references them.
//
// The request carried no pinning triple at all (a GUI predating mTLS), or only
// part of one. Never a reason to fall back to the SDK default listener.
inline constexpr const char* kCodeRpcPinRequired = "rpc_pin_required";
// The triple is present but does not pass the shape gate: not a PEM, oversize,
// contains a NUL, or a host:port that is not literal loopback.
inline constexpr const char* kCodeRpcPinInvalid = "rpc_pin_invalid";
// The shape gate passed and DeviceLocal::setRpcServer still refused (bad key
// material, or the port is taken). Distinct from kCodeTunOpenFailed on
// purpose: an mTLS bind failure must never render as "could not open the tun
// device".
inline constexpr const char* kCodeRpcListenFailed = "rpc_listen_failed";

// "127.0.0.1:<port>" -> port, or 0 when the string is not a usable loopback
// device-RPC address. THE ESCALATION GATE: the hostport arrives off the wire
// and is handed to root's DeviceLocal::setRpcServer, so an unvalidated value
// lets a control-socket peer make root bind the device RPC on "0.0.0.0:12025"
// (the whole LAN) or on a privileged port.
//
// Deliberately strict, and deliberately shared by both halves so they parse
// identically — the daemon's own std::atoi helper used to turn
// "127.0.0.1:notaport" into 0 and then silently report 12025 while the
// listener was elsewhere, a mismatch the GUI had no way to detect:
//   * literal "127.0.0.1" only — NOT "localhost" (resolver-dependent) and NOT
//     "::1" (a v6 listener sits outside the IPv6 fail-closed story);
//   * digits only, so a second ':', whitespace or a leading '+' all fail;
//   * 1024 <= port <= 65535. Port 0 is refused: the SDK would bind an
//     ephemeral port the GUI could never dial, and the echoed rpc_port would
//     then be a fiction.
inline int RpcPortFromHostPort(const std::string& host_port) {
  constexpr size_t kPrefixLen = 10;  // strlen("127.0.0.1:")
  if (host_port.size() <= kPrefixLen) return 0;
  if (host_port.compare(0, kPrefixLen, "127.0.0.1:") != 0) return 0;
  const size_t digits = host_port.size() - kPrefixLen;
  if (digits > 5) return 0;  // 65535 is five digits; anything longer overflows
  int value = 0;
  for (size_t i = kPrefixLen; i < host_port.size(); ++i) {
    const char c = host_port[i];
    if (c < '0' || c > '9') return 0;
    value = value * 10 + (c - '0');
  }
  if (value < 1024 || value > 65535) return 0;
  return value;
}

inline bool IsLoopbackRpcHostPort(const std::string& host_port) {
  return RpcPortFromHostPort(host_port) != 0;
}

// SHAPE gate only — non-empty, bounded, NUL-free, PEM-framed. NOT crypto
// validation: the SDK's own parse is, and it throws. This exists to catch the
// two cheap failures before either side calls into the SDK, and in particular
// to catch a handle-0 urnet::generateDeviceRpcKeyMaterial, whose four getters
// return four EMPTY strings with no exception (the binding maps a NULL char*
// to "" rather than throwing).
inline bool LooksLikePem(const std::string& pem) {
  if (pem.empty() || pem.size() > kMaxRpcPemBytes) return false;
  if (pem.find('\0') != std::string::npos) return false;
  if (pem.compare(0, 11, "-----BEGIN ") != 0) return false;
  return pem.find("-----END ") != std::string::npos;
}

// ---- verbs -----------------------------------------------------------------

enum class Verb {
  Hello,
  Status,
  StartTunnel,
  StopTunnel,
  SetProvide,
  SetKillSwitch,
  LocationOverrideAvailable,
  LocationOverrideWrite,
  LocationOverrideClear,
  Unknown,
};

inline const char* ToString(Verb v) {
  switch (v) {
    case Verb::Hello: return "hello";
    case Verb::Status: return "status";
    case Verb::StartTunnel: return "start_tunnel";
    case Verb::StopTunnel: return "stop_tunnel";
    case Verb::SetProvide: return "set_provide";
    case Verb::SetKillSwitch: return "set_kill_switch";
    case Verb::LocationOverrideAvailable: return "location_override_available";
    case Verb::LocationOverrideWrite: return "location_override_write";
    case Verb::LocationOverrideClear: return "location_override_clear";
    case Verb::Unknown: break;
  }
  return "unknown";
}

inline Verb VerbFromString(const std::string& s) {
  if (s == "hello") return Verb::Hello;
  if (s == "status") return Verb::Status;
  if (s == "start_tunnel") return Verb::StartTunnel;
  if (s == "stop_tunnel") return Verb::StopTunnel;
  if (s == "set_provide") return Verb::SetProvide;
  if (s == "set_kill_switch") return Verb::SetKillSwitch;
  if (s == "location_override_available") return Verb::LocationOverrideAvailable;
  if (s == "location_override_write") return Verb::LocationOverrideWrite;
  if (s == "location_override_clear") return Verb::LocationOverrideClear;
  return Verb::Unknown;
}

// ---- which polkit action gates which verb (pure, unit-testable) ------------
// nullptr means "no polkit check": hello (which MUST stay pre-authorization so
// version/SDK skew still renders — a peer that cannot authorize must still be
// TOLD WHY rather than dropped), status (polled at ~4 Hz during async bring-up;
// a round trip there would be intolerable, and it is redacted for a foreign uid
// instead — see RedactStatusForForeignUid) and location_override_available,
// which is a capability query that changes nothing.
//
// cross_uid: a tunnel is live or starting and is owned by a DIFFERENT uid than
// the caller. That is the case the `urnetwork` group could not express at all —
// every member was interchangeable, and one member could silently displace
// another's tunnel. It now costs kActionTakeOverTunnel, which is
// auth_admin_keep in all three slots.
inline const char* ActionIdForVerb(Verb verb, bool is_log_tail, bool cross_uid) {
  if (is_log_tail) return kActionReadLog;
  switch (verb) {
    case Verb::StartTunnel:
    case Verb::StopTunnel:
    case Verb::SetProvide:
    case Verb::LocationOverrideWrite:
    case Verb::LocationOverrideClear:
      return cross_uid ? kActionTakeOverTunnel : kActionControlTunnel;
    case Verb::SetKillSwitch:
      // Its own id so a site can require a password for the kill switch and
      // not for Connect. Same defaults as control-tunnel, so it costs no extra
      // prompt on the normal path.
      return cross_uid ? kActionTakeOverTunnel : kActionManageKillSwitch;
    case Verb::Hello:
    case Verb::Status:
    case Verb::LocationOverrideAvailable:
    case Verb::Unknown:
      break;
  }
  return nullptr;
}

// Interactive == polkit may raise the session's agent dialog. Only for verbs a
// human just pressed. log_tail is polled every few seconds and status at ~4 Hz,
// so an interactive check there would carpet the screen with password dialogs;
// a polled verb gets a non-interactive check and a challenge code instead.
inline bool VerbWantsInteraction(Verb verb, bool is_log_tail) {
  if (is_log_tail) return false;
  switch (verb) {
    case Verb::StartTunnel:
    case Verb::StopTunnel:
    case Verb::SetProvide:
    case Verb::SetKillSwitch:
    case Verb::LocationOverrideWrite:
    case Verb::LocationOverrideClear:
      return true;
    case Verb::Hello:
    case Verb::Status:
    case Verb::LocationOverrideAvailable:
    case Verb::Unknown:
      break;
  }
  return false;
}

// ---- tunnel lifecycle state (status.tunnel_state) --------------------------

enum class TunnelState {
  Stopped,
  Starting,
  Up,  // tun open, DeviceLocal running, device RPC listener ready
  Stopping,
  Error,
};

inline const char* ToString(TunnelState s) {
  switch (s) {
    case TunnelState::Stopped: return "stopped";
    case TunnelState::Starting: return "starting";
    case TunnelState::Up: return "up";
    case TunnelState::Stopping: return "stopping";
    case TunnelState::Error: return "error";
  }
  return "stopped";
}

inline TunnelState TunnelStateFromString(const std::string& s) {
  if (s == "starting") return TunnelState::Starting;
  if (s == "up") return TunnelState::Up;
  if (s == "stopping") return TunnelState::Stopping;
  if (s == "error") return TunnelState::Error;
  return TunnelState::Stopped;
}

// ---- kill-switch state (status.kill_switch) --------------------------------
// What is ACTUALLY installed, not what the toggle says. The Linux kill switch
// is an nftables table (`table inet urnetwork`), so unlike the Windows dynamic
// WFP session it can be present without this process, and it can fail to
// install while the toggle reads on. The UI must render the INSTALLED state:
// Device::setRouteLocal alone is not a kill switch (windows WfpPolicy.h:9-19
// says why: it is a branch inside sendPacket, so it never sees IPv6, the
// deliberately-excluded LAN, another adapter's resolver, or a dead daemon).
enum class KillSwitchState {
  Off,        // no floor installed
  Armed,      // floor installed, no tunnel: nothing but the daemon/LAN leaves
  Connected,  // floor installed alongside a live tunnel
  Failed,     // asked for, could NOT be installed — never render this as Off
};

inline const char* ToString(KillSwitchState s) {
  switch (s) {
    case KillSwitchState::Off: return "off";
    case KillSwitchState::Armed: return "armed";
    case KillSwitchState::Connected: return "connected";
    case KillSwitchState::Failed: return "failed";
  }
  return "off";
}

inline KillSwitchState KillSwitchStateFromString(const std::string& s) {
  if (s == "armed") return KillSwitchState::Armed;
  if (s == "connected") return KillSwitchState::Connected;
  if (s == "failed") return KillSwitchState::Failed;
  return KillSwitchState::Off;
}

// ---- payloads --------------------------------------------------------------
// Envelope fields (verb/id/ok/error) are handled by the helpers below; these
// structs carry only the verb payloads, so a round-trip test can compare them
// field for field. Deserialization uses the find/get_to pattern so unknown or
// absent fields are tolerated.

namespace detail {
// tolerant field read: absent or null leaves `out` at its default
template <typename T>
inline void Get(const nlohmann::json& j, const char* key, T& out) {
  if (auto it = j.find(key); it != j.end() && !it->is_null()) it->get_to(out);
}
}  // namespace detail

struct HelloRequest {
  int protocol_version = kControlProtocolVersion;
  // the client's urnet::version(); exact-match enforced (SdkVersionsAgree)
  std::string sdk_version;
};
inline void to_json(nlohmann::json& j, const HelloRequest& v) {
  j["protocol_version"] = v.protocol_version;
  j["sdk_version"] = v.sdk_version;
}
inline void from_json(const nlohmann::json& j, HelloRequest& v) {
  v.protocol_version = 0;  // absent = pre-versioning peer, always rejected
  detail::Get(j, "protocol_version", v.protocol_version);
  detail::Get(j, "sdk_version", v.sdk_version);  // absent = "" = fails closed
}

struct HelloReply {
  int protocol_version = kControlProtocolVersion;
  std::string daemon_version;
  // the daemon's urnet::version(); exact-match enforced (SdkVersionsAgree)
  std::string sdk_version;

  // kAuthModePolkit | kAuthModeGroup — which authority this daemon latched at
  // start. ADDITIVE within protocol v1, and ABSENT PARSES AS "group", which is
  // exactly what a daemon predating this field is. The GUI branches on it for
  // one reason: "add yourself to the `urnetwork` group, then log out and back
  // in" is the correct remediation under `group` and is actively wrong under
  // `polkit`, where no group and no re-login are involved.
  std::string auth_mode = kAuthModeGroup;
};
inline void to_json(nlohmann::json& j, const HelloReply& v) {
  j["protocol_version"] = v.protocol_version;
  j["daemon_version"] = v.daemon_version;
  j["sdk_version"] = v.sdk_version;
  j["auth_mode"] = v.auth_mode;
}
inline void from_json(const nlohmann::json& j, HelloReply& v) {
  v.protocol_version = 0;
  detail::Get(j, "protocol_version", v.protocol_version);
  detail::Get(j, "daemon_version", v.daemon_version);
  detail::Get(j, "sdk_version", v.sdk_version);
  v.auth_mode = kAuthModeGroup;  // absent = a daemon predating polkit
  detail::Get(j, "auth_mode", v.auth_mode);
}

struct StartTunnelRequest {
  std::string by_jwt;
  std::string instance_id;
  std::string app_version;
  // The GUI's active NetworkSpace, serialized (windows StartTunnel's
  // network_space_json). The daemon builds its DeviceLocal in THIS space so a
  // custom-server session never syncs against a device registered on
  // production. Empty = the compiled-in default space (BuildUrNetworkSpace) —
  // the safe direction: silence means production, never a surprise server.
  // Additive within protocol v1: both halves ship from one pipeline and the
  // hello sdk_version exact-match already refuses mismatched pairs.
  std::string network_space_json;

  // ---- additive within protocol v1 (same argument as network_space_json:
  //      both halves ship from one pipeline and the hello sdk_version
  //      exact-match already refuses a mismatched pair) --------------------

  // Return as soon as the request is accepted (tunnel_state=starting) and let
  // the client poll `status`, instead of holding both main loops for the whole
  // bring-up — DeviceLocal construction is a network round trip and the tun +
  // route + DNS setup is ~35 subprocesses. A client that leaves this false
  // gets the old synchronous behaviour unchanged.
  bool async = false;

  // The kill switch the client wants in force for this session. The daemon
  // reports back what it ACTUALLY installed in StatusReply::kill_switch —
  // never assume the request took.
  bool kill_switch = false;

  // ---- device-RPC mTLS pinning (windows TunnelController.cpp:800-806) -----
  // REQUIRED, all three, every start (see the mTLS section above and
  // ValidateStartTunnelRequest below). The GUI generates one
  // urnet::DeviceRpcKeyMaterial and splits it: server_pem + client_cert_pem
  // travel here for DeviceLocal::setRpcServer, and client_pem +
  // server_cert_pem stay in the GUI for its own DeviceRemote::setRpcServer.
  //
  // The "all three empty = the SDK's built-in default listener" clause that
  // used to live on this comment is GONE, deliberately: it was a silent
  // fall-back to an unpinned root listener, and the whole point of this field
  // set is that there is no such path.
  //
  // Nothing travels the other way. client_pem and server_cert_pem NEVER leave
  // the GUI — the verifier has to choose its own pin, or it is not pinning.
  std::string rpc_server_pem;       // server KEY+CERT the daemon presents
  std::string rpc_client_cert_pem;  // client CERT the daemon pins
  std::string rpc_listen_hostport;  // "127.0.0.1:<port>", GUI-chosen
};
inline void to_json(nlohmann::json& j, const StartTunnelRequest& v) {
  j["by_jwt"] = v.by_jwt;
  j["instance_id"] = v.instance_id;
  j["app_version"] = v.app_version;
  if (!v.network_space_json.empty()) j["network_space_json"] = v.network_space_json;
  j["async"] = v.async;
  j["kill_switch"] = v.kill_switch;
  if (!v.rpc_server_pem.empty()) j["rpc_server_pem"] = v.rpc_server_pem;
  if (!v.rpc_client_cert_pem.empty()) j["rpc_client_cert_pem"] = v.rpc_client_cert_pem;
  if (!v.rpc_listen_hostport.empty()) j["rpc_listen_hostport"] = v.rpc_listen_hostport;
}
inline void from_json(const nlohmann::json& j, StartTunnelRequest& v) {
  detail::Get(j, "by_jwt", v.by_jwt);
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "app_version", v.app_version);
  detail::Get(j, "network_space_json", v.network_space_json);
  detail::Get(j, "async", v.async);
  detail::Get(j, "kill_switch", v.kill_switch);
  detail::Get(j, "rpc_server_pem", v.rpc_server_pem);
  detail::Get(j, "rpc_client_cert_pem", v.rpc_client_cert_pem);
  detail::Get(j, "rpc_listen_hostport", v.rpc_listen_hostport);
}

// A rejected start_tunnel, with the machine-readable code attached. The code
// is a pointer into the kCode* table (static storage, never freed) and is
// nullptr for the two plain field errors, which have no actionable branch
// beyond their message.
struct StartTunnelRejection {
  std::string message;
  const char* code = nullptr;
};

// Validation the daemon applies before constructing anything, and which the
// GUI-side client applies before putting a frame on the wire. Pure, so the
// contract is unit-testable and both halves enforce the identical rule.
//
// `instance_id` is the DEVICE PAIRING KEY — the daemon constructs its
// DeviceLocal with exactly this value and the GUI hands the same value to
// newDeviceRemoteWithDefaults; the device rpc rejects a sync whose InstanceId
// differs from the DeviceLocal's (sdk/device_rpc.go:6545), which surfaces as a
// remote that connects but never populates. An empty id must therefore fail
// loudly here, never fall back to a daemon-generated one.
//
// The rpc pinning triple is REQUIRED. This is a deliberate BEHAVIOUR change
// inside protocol v1 (the wire is still purely additive): an old GUI's
// start_tunnel now fails with kCodeRpcPinRequired instead of quietly getting
// an unpinned root listener on 127.0.0.1:12025. Per the hello exact-version
// match, a mismatched shipped pair is already refused before start_tunnel is
// reachable, so this branch turns "should be unreachable" into "is".
inline std::optional<StartTunnelRejection> ValidateStartTunnelRequest(
    const StartTunnelRequest& req) {
  if (req.by_jwt.empty()) return StartTunnelRejection{"by_jwt is required", nullptr};
  if (req.instance_id.empty()) return StartTunnelRejection{"instance_id is required", nullptr};
  const int rpcParts = (req.rpc_server_pem.empty() ? 0 : 1) +
                       (req.rpc_client_cert_pem.empty() ? 0 : 1) +
                       (req.rpc_listen_hostport.empty() ? 0 : 1);
  if (rpcParts == 0) {
    return StartTunnelRejection{
        "this start_tunnel carries no device-rpc pinning material; the daemon will not run "
        "an unauthenticated local rpc listener",
        kCodeRpcPinRequired};
  }
  if (rpcParts != 3) {
    // A half-supplied pair would make the daemon listen with a pinned server
    // while the GUI dials with the SDK default (or the reverse), which
    // presents as a DeviceRemote that connects and never populates.
    return StartTunnelRejection{
        "rpc_server_pem, rpc_client_cert_pem and rpc_listen_hostport must be sent together",
        kCodeRpcPinRequired};
  }
  if (!LooksLikePem(req.rpc_server_pem)) {
    return StartTunnelRejection{"rpc_server_pem is not a usable PEM", kCodeRpcPinInvalid};
  }
  if (!LooksLikePem(req.rpc_client_cert_pem)) {
    return StartTunnelRejection{"rpc_client_cert_pem is not a usable PEM", kCodeRpcPinInvalid};
  }
  if (!IsLoopbackRpcHostPort(req.rpc_listen_hostport)) {
    // THE ESCALATION GATE. Unvalidated, this value makes ROOT bind wherever
    // the caller says.
    return StartTunnelRejection{
        "rpc_listen_hostport must be 127.0.0.1:<port> with 1024 <= port <= 65535",
        kCodeRpcPinInvalid};
  }
  return std::nullopt;
}

struct StartTunnelReply {
  int rpc_port = 0;
  // "up" for a completed synchronous start; "starting" when async was asked
  // for and the bring-up is still running. A client that ignores this field
  // sees exactly the old contract.
  TunnelState tunnel_state = TunnelState::Up;
  // The daemon actually called DeviceLocal::setRpcServer with the supplied
  // triple and it returned without throwing. ADDITIVE within v1, and absent
  // parses FALSE — which is the fail-closed default: a daemon predating this
  // field never pinned, so a client that sees false must refuse to dial rather
  // than fall back to plaintext.
  //
  // rpc_port beside it is an ECHO, not a discovery (the SDK exposes no getter
  // for the bound address), so the client's check is
  // rpc_pinned && rpc_port == RpcPortFromHostPort(the hostport it sent).
  bool rpc_pinned = false;
};
inline void to_json(nlohmann::json& j, const StartTunnelReply& v) {
  j["rpc_port"] = v.rpc_port;
  j["tunnel_state"] = ToString(v.tunnel_state);
  j["rpc_pinned"] = v.rpc_pinned;
}
inline void from_json(const nlohmann::json& j, StartTunnelReply& v) {
  detail::Get(j, "rpc_port", v.rpc_port);
  std::string state;
  detail::Get(j, "tunnel_state", state);
  // absent = a daemon predating the field, which only ever replied ok on a
  // completed start
  v.tunnel_state = state.empty() ? TunnelState::Up : TunnelStateFromString(state);
  detail::Get(j, "rpc_pinned", v.rpc_pinned);  // absent = false = fails closed
}

struct SetKillSwitchRequest {
  bool enabled = false;
};
inline void to_json(nlohmann::json& j, const SetKillSwitchRequest& v) {
  j["enabled"] = v.enabled;
}
inline void from_json(const nlohmann::json& j, SetKillSwitchRequest& v) {
  detail::Get(j, "enabled", v.enabled);
}

struct SetProvideRequest {
  std::string mode;  // "never"|"always"|"network"|"auto"|"manual"
};
inline void to_json(nlohmann::json& j, const SetProvideRequest& v) { j["mode"] = v.mode; }
inline void from_json(const nlohmann::json& j, SetProvideRequest& v) {
  detail::Get(j, "mode", v.mode);
}

// The daemon's whole feedback channel. Every field beyond the original four is
// additive within protocol v1 and answers a question the UI previously had no
// way to ask: "routes are in but DNS is not", "the kill switch is holding this
// machine blocked", "the tunnel is up over nothing", "why did it stop".
// Windows pushes the same inventory as TunnelStatus.
struct StatusReply {
  TunnelState tunnel_state = TunnelState::Stopped;
  int rpc_port = 0;          // 0 while the tunnel is down
  std::string client_id;     // the DeviceLocal's client id ("" while down)
  std::string error;         // last start error ("" when none)

  // Machine-readable twin of `error` (one of the kCode* below), so the UI
  // branches on a code and never on prose.
  std::string error_code;
  // What is actually in force, as opposed to what was attempted.
  bool routes_installed = false;
  // The daemon's own SDK sockets are demonstrably steered around the tunnel
  // (defect R4). false with tunnel_state=up means the control plane is
  // starving and the session cannot carry traffic.
  bool egress_protected = false;
  bool dns_applied = false;
  std::string dns_detail;    // why DNS is not in force ("" when it is)
  KillSwitchState kill_switch = KillSwitchState::Off;
  std::string kill_switch_detail;  // why it is Failed ("" otherwise)
  bool ipv6_blocked = false;       // v6 has no tunnel: blocked rather than leaked
  std::string tunnel_interface;    // "urnet0" while up
  // Why the last session ended: "user" (an explicit stop_tunnel), "io_loop"
  // (the SDK loop finished under us), "start_failed", "daemon_shutdown", or ""
  // when no session has ended.
  std::string stop_reason;
  int64_t up_since_millis = 0;     // unix millis of the up edge, 0 while down
  // A control client currently owns this tunnel. false with tunnel_state=up is
  // a captured machine with no UI attached — the state the tray "Stop the
  // tunnel" recovery item exists for.
  bool owner_connected = false;

  // The live session's device RPC is pinned to the material this client sent.
  // The SAME fact as StartTunnelReply::rpc_pinned, published here because it
  // is UNREADABLE on the async start path: an async start replies
  // tunnel_state=starting with rpc_port=0, so the real answer can only arrive
  // on `status`. A client that constructs its DeviceRemote off a `status`
  // transition to Up must gate on this field exactly as the synchronous path
  // gates on the start reply. Absent parses false — fail closed.
  bool rpc_pinned = false;

  // This status was cut down because the caller is neither root nor the uid
  // that owns the running tunnel. ADDITIVE within v1; absent parses false.
  //
  // Only tunnel_state, kill_switch, owner_connected and this flag are
  // meaningful when it is true. It exists because the socket is
  // world-connectable under polkit: the full status names the provider, the
  // location, the client id, the loopback rpc port and the byte counters, and
  // none of that is another local user's business. Shipping kill_switch to
  // everyone anyway is deliberate and pro-user — a person whose network is
  // dead deserves to learn why.
  bool redacted = false;
};
inline void to_json(nlohmann::json& j, const StatusReply& v) {
  j["tunnel_state"] = ToString(v.tunnel_state);
  j["rpc_port"] = v.rpc_port;
  j["client_id"] = v.client_id;
  j["error"] = v.error;
  j["error_code"] = v.error_code;
  j["routes_installed"] = v.routes_installed;
  j["egress_protected"] = v.egress_protected;
  j["dns_applied"] = v.dns_applied;
  j["dns_detail"] = v.dns_detail;
  j["kill_switch"] = ToString(v.kill_switch);
  j["kill_switch_detail"] = v.kill_switch_detail;
  j["ipv6_blocked"] = v.ipv6_blocked;
  j["tunnel_interface"] = v.tunnel_interface;
  j["stop_reason"] = v.stop_reason;
  j["up_since_millis"] = v.up_since_millis;
  j["owner_connected"] = v.owner_connected;
  j["rpc_pinned"] = v.rpc_pinned;
  j["redacted"] = v.redacted;
}
inline void from_json(const nlohmann::json& j, StatusReply& v) {
  std::string state;
  detail::Get(j, "tunnel_state", state);
  v.tunnel_state = TunnelStateFromString(state);
  detail::Get(j, "rpc_port", v.rpc_port);
  detail::Get(j, "client_id", v.client_id);
  detail::Get(j, "error", v.error);
  detail::Get(j, "error_code", v.error_code);
  detail::Get(j, "routes_installed", v.routes_installed);
  detail::Get(j, "egress_protected", v.egress_protected);
  detail::Get(j, "dns_applied", v.dns_applied);
  detail::Get(j, "dns_detail", v.dns_detail);
  std::string killSwitch;
  detail::Get(j, "kill_switch", killSwitch);
  v.kill_switch = KillSwitchStateFromString(killSwitch);
  detail::Get(j, "kill_switch_detail", v.kill_switch_detail);
  detail::Get(j, "ipv6_blocked", v.ipv6_blocked);
  detail::Get(j, "tunnel_interface", v.tunnel_interface);
  detail::Get(j, "stop_reason", v.stop_reason);
  detail::Get(j, "up_since_millis", v.up_since_millis);
  detail::Get(j, "owner_connected", v.owner_connected);
  detail::Get(j, "rpc_pinned", v.rpc_pinned);  // absent = false = fails closed
  detail::Get(j, "redacted", v.redacted);      // absent = false = a full status
}

// The non-owner, non-root view of a tunnel somebody else on this machine
// started. Pure, so the exact set of fields that survive is unit-testable and
// cannot drift silently as StatusReply grows: this is a whitelist, not a
// blacklist — a field added to StatusReply is redacted by DEFAULT because it is
// simply never copied here.
//
// KEPT:    tunnel_state, kill_switch, kill_switch_detail, owner_connected
//          (a person whose machine is captured or blocked must be able to see
//          THAT, and the tray's "stop the tunnel" recovery needs it)
// STRIPPED: client_id, rpc_port, rpc_pinned, tunnel_interface, error,
//          error_code, dns_detail, stop_reason, up_since_millis and every
//          "what is in force" flag — provider identity, location and the
//          session's own plumbing.
inline StatusReply RedactStatusForForeignUid(const StatusReply& full) {
  StatusReply out;
  out.tunnel_state = full.tunnel_state;
  out.kill_switch = full.kill_switch;
  out.kill_switch_detail = full.kill_switch_detail;
  out.owner_connected = full.owner_connected;
  out.redacted = true;
  return out;
}

struct LocationOverrideAvailableReply {
  bool available = false;
  std::string reason;  // "" when available; short reason otherwise
};
inline void to_json(nlohmann::json& j, const LocationOverrideAvailableReply& v) {
  j["available"] = v.available;
  j["reason"] = v.reason;
}
inline void from_json(const nlohmann::json& j, LocationOverrideAvailableReply& v) {
  detail::Get(j, "available", v.available);
  detail::Get(j, "reason", v.reason);
}

struct LocationOverrideWriteRequest {
  double lat = 0;
  double lon = 0;
  double accuracy_m = 0;  // metres, must be > 0 (GeoClue discards -1/0)
};
inline void to_json(nlohmann::json& j, const LocationOverrideWriteRequest& v) {
  j["lat"] = v.lat;
  j["lon"] = v.lon;
  j["accuracy_m"] = v.accuracy_m;
}
inline void from_json(const nlohmann::json& j, LocationOverrideWriteRequest& v) {
  detail::Get(j, "lat", v.lat);
  detail::Get(j, "lon", v.lon);
  detail::Get(j, "accuracy_m", v.accuracy_m);
}

// ---- envelope + framing ----------------------------------------------------

// Machine-readable rejection codes carried in the reply's optional "code"
// field, so the GUI can distinguish "the daemon is too old" from "this app is
// too old" without parsing prose.
inline constexpr const char* kCodeClientProtocolTooOld = "client_protocol_too_old";
inline constexpr const char* kCodeSdkVersionMismatch = "sdk_version_mismatch";
inline constexpr const char* kCodeHelloRequired = "hello_required";
inline constexpr const char* kCodeTunnelOwnedByOtherClient = "tunnel_owned_by_other_client";

// ---- authorization refusals ------------------------------------------------
// These arrive over a SUCCESSFUL connection, from a daemon that answered. They
// are verb errors, NOT transport failures, and that separation is the point:
// DaemonUnreachableReason (SocketMissing / PermissionDenied / StaleSandboxMount
// / Other) keeps every one of its meanings and gains no enumerator, so
// "refused" and "the service is not running" stay visibly distinct — which they
// could not be under the old design, where a refusal was an EACCES at
// connect(2) with no reason attached to it.
//
// Every one of them is logged daemon-side with the peer uid, the pid, the
// action id and the reason before it goes on the wire.

// polkit answered (is_authorized=false, is_challenge=false): a site rule or an
// explicit `no` default refuses this subject. Retrying will not help.
inline constexpr const char* kCodeAuthDenied = "auth_denied";
// polkit answered is_challenge=true. Either we asked non-interactively (a
// polled verb), or an interactive check found no authentication agent — which
// is what a remote/ssh or session-less caller looks like, since polkit's
// "local" means "has a seat" and sshd sessions are seatless.
inline constexpr const char* kCodeAuthRequired = "auth_required";
// The spelling the other half of the design review used for the same refusal.
// ACCEPTED, NEVER EMITTED: this daemon always sends kCodeAuthRequired. It is
// declared so a client written against the other half of the design still has
// the symbol and can branch on both, which is exactly what ControlClient.hpp
// does — one extra string compare, and no refusal ever falls through to a raw
// diagnostic because two halves picked different words for one verdict.
inline constexpr const char* kCodeAuthChallengeRequired = "auth_challenge_required";
// The agent dialog was raised and the user closed it. The client MUST render
// NOTHING for this — a declined elevation is the user changing their mind, not
// an error (docs/parity/settings.md:130).
inline constexpr const char* kCodeAuthDismissed = "auth_dismissed";
// A tunnel owned by a DIFFERENT uid, and kActionTakeOverTunnel was not granted.
// The refusal the `urnetwork` group could not express: under it, any member
// could silently tear down and replace another member's tunnel.
inline constexpr const char* kCodeAuthNotTunnelOwner = "auth_not_tunnel_owner";
// polkit was NOT expected on this machine (no kPolkitPolicyPath) and the
// fallback group check refused this peer. This is the ONE code under which
// "add your user to the `urnetwork` group, then log out and back in" is still
// the right advice.
inline constexpr const char* kCodeAuthUnavailable = "auth_unavailable";
// polkit WAS expected and the check could not be completed: no system bus,
// polkitd unreachable or gone, an unreadable peer start-time, a malformed
// reply. FAIL CLOSED — never a downgrade to the group check, or "stop polkitd"
// becomes a way to pick the weaker policy.
inline constexpr const char* kCodeAuthCheckFailed = "auth_check_failed";
// Nobody answered the agent dialog inside the daemon's own bound and
// CancelCheckAuthorization was issued. Distinct from Dismissed on purpose: an
// unanswered prompt is worth saying out loud, a declined one is not.
inline constexpr const char* kCodeAuthTimeout = "auth_timeout";

// ---- start_tunnel failure codes -------------------------------------------
// Each one is a DIFFERENT problem with a DIFFERENT fix, and the whole point of
// carrying them is that none of them may reach the user as the single string
// "could not open/configure the tun device" (which is what every one of them
// used to render as), and none may render as a blank.
//
// A start already running; NOT a reason to tear down and restart (which is how
// a 30 s client timeout used to turn into a restart loop on exactly the slow
// networks where it fires).
inline constexpr const char* kCodeStartInProgress = "start_in_progress";
// The `tun` kernel module is absent and modprobe could not load it.
inline constexpr const char* kCodeTunModuleMissing = "tun_module_missing";
// /dev/net/tun or TUNSETIFF was refused: the daemon lacks CAP_NET_ADMIN.
inline constexpr const char* kCodeTunPermissionDenied = "tun_permission_denied";
// The interface name is taken by another process.
inline constexpr const char* kCodeTunBusy = "tun_busy";
// open()/TUNSETIFF failed for some other reason (message carries strerror).
inline constexpr const char* kCodeTunOpenFailed = "tun_open_failed";
// The device handed back an address/mtu/prefix that is not usable.
inline constexpr const char* kCodeTunConfigInvalid = "tun_config_invalid";
// `ip`, `nft` or `resolvectl` is not installed.
inline constexpr const char* kCodeMissingTool = "missing_tool";
// A capture route or a policy rule could not be installed (message names the
// prefix and the command's own stderr).
inline constexpr const char* kCodeRouteInstallFailed = "route_install_failed";
// THE R4 REFUSAL: the daemon's own SDK sockets would be captured by its own
// tunnel, so the control plane would starve the instant we returned success.
// Never start in this state — a tunnel that comes up and carries nothing is
// worse than a start that says why it refused.
inline constexpr const char* kCodeEgressUnprotected = "egress_unprotected";
// The nftables floor the kill switch needs could not be installed. Only fatal
// when the client asked for the kill switch: a protection that is not in force
// must never be reported as on.
inline constexpr const char* kCodeKillSwitchFailed = "kill_switch_failed";
// DNS could not be pointed at the tunnel AND the kill switch was requested, so
// coming up would leave the machine unable to resolve at all.
inline constexpr const char* kCodeDnsApplyFailed = "dns_apply_failed";
// Also in this table, declared earlier because ValidateStartTunnelRequest
// returns them: kCodeRpcPinRequired, kCodeRpcPinInvalid, kCodeRpcListenFailed
// (see the device-RPC mTLS pinning section near the top of this header).

// {"verb":…,"id":N,…payload}
inline nlohmann::json MakeRequest(Verb verb, int64_t id,
                                  nlohmann::json payload = nlohmann::json::object()) {
  payload["verb"] = ToString(verb);
  payload["id"] = id;
  return payload;
}

// {"id":N,"ok":bool,…payload} (+ "error"/"code" when failing)
inline nlohmann::json MakeReply(int64_t id, bool ok,
                                nlohmann::json payload = nlohmann::json::object()) {
  payload["id"] = id;
  payload["ok"] = ok;
  return payload;
}

inline nlohmann::json MakeErrorReply(int64_t id, const std::string& error,
                                     const char* code = nullptr) {
  nlohmann::json j = MakeReply(id, false);
  j["error"] = error;
  if (code != nullptr) j["code"] = code;
  return j;
}

inline Verb RequestVerb(const nlohmann::json& j) {
  if (auto it = j.find("verb"); it != j.end() && it->is_string()) {
    return VerbFromString(it->get<std::string>());
  }
  return Verb::Unknown;
}

inline int64_t FrameId(const nlohmann::json& j) {
  if (auto it = j.find("id"); it != j.end() && it->is_number()) {
    return it->get<int64_t>();
  }
  return -1;
}

inline bool ReplyOk(const nlohmann::json& j) {
  if (auto it = j.find("ok"); it != j.end() && it->is_boolean()) return it->get<bool>();
  return false;
}

inline std::string ReplyError(const nlohmann::json& j) {
  std::string error;
  detail::Get(j, "error", error);
  return error;
}

inline std::string ReplyCode(const nlohmann::json& j) {
  std::string code;
  detail::Get(j, "code", code);
  return code;
}

// One frame per line. nlohmann's dump() never emits raw newlines (they are
// escaped inside strings), so '\n' is an unambiguous frame terminator.
inline std::string EncodeFrame(const nlohmann::json& j) { return j.dump() + "\n"; }

// Parses one line (with or without its trailing newline). Returns nullopt for
// anything that is not a single JSON object — the caller treats that as a
// protocol error and drops the connection.
inline std::optional<nlohmann::json> DecodeFrame(const std::string& line) {
  nlohmann::json j = nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;
  return j;
}

}  // namespace urnw::ctl
