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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ProvideLifecycle.hpp"

namespace urnw::ctl {

// Bump ONLY on incompatible wire-format changes, decoupled from the app
// version (hello negotiates it; see the enforcement note above).
//
// attach_tunnel DOES NOT BUMP IT, and the reasoning is recorded here because
// "we added a verb, surely that is a new protocol" is the tempting wrong
// answer:
//   * A new verb is additive in BOTH directions. An old daemon meets
//     attach_tunnel as Verb::Unknown and answers `unknown verb` — a normal
//     error frame the client can fall back from, not a dropped connection. A
//     new daemon meets a client that never sends it and behaves exactly as
//     before. Same for the two StartTunnelReply fields: every payload here is
//     parsed with the tolerant detail::Get, so absent is a default and unknown
//     is ignored.
//   * The version numbers are a MINIMUM gate, not a feature flag. Raising
//     kControlProtocolVersion to 2 without raising the minimums buys nothing;
//     raising the MINIMUMS would refuse every currently-installed pair — the
//     daemon shipped by apt/install.sh and the AppImage/Flatpak GUI update on
//     independent schedules, which is the whole reason this check has teeth —
//     to gain a verb that a v1 peer already declines gracefully.
//   * The hello gate is unchanged and needs no exception: attach_tunnel is a
//     post-hello verb like every other, so it is refused with
//     kCodeHelloRequired before authorization exactly as start_tunnel is, and
//     the SDK exact-version match still runs first.
// NOTE FOR THE MERGE: upstream's copy of this header reads 2/2/2 where ours
// reads 1/1/1, for reasons unrelated to this verb. The number that ships is
// upstream's to choose — this fork deliberately does not renumber a shared
// wire contract from a branch.
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
    "/usr/share/polkit-1/actions/com.bringyour.network.policy";
// The immutable-host twin. ostree/bootc machines mount /usr read-only, so the
// tarball installer maps its whole payload under /usr/local — and polkit has
// read both directories since 124. ControlServer::PolicyPath() probes this one
// FIRST; without it the daemon looks in the one place the file cannot be on
// Bazzite/Silverblue/Kinoite/SteamOS and silently falls back to the group.
inline constexpr const char* kPolkitPolicyPathLocal =
    "/usr/local/share/polkit-1/actions/com.bringyour.network.policy";

// HelloReply::auth_mode — which authority this daemon actually latched at
// start. The GUI needs it because the "add yourself to the urnetwork group,
// then log out and back in" remediation is correct under `group` and WRONG
// under `polkit`.
inline constexpr const char* kAuthModePolkit = "polkit";
inline constexpr const char* kAuthModeGroup = "group";

// The four polkit action ids, namespaced to the app id. Shipped in
// packaging/polkit/com.bringyour.network.policy (0644 root:root — polkit
// ignores group- or world-writable action files). They are checked by
// urnetworkd, never by the GUI: the subject is built from SO_PEERCRED on the
// connection being served, so a client can neither nominate its own subject
// nor skip the check by not asking.
inline constexpr const char* kActionControlTunnel = "com.bringyour.network.control-tunnel";
inline constexpr const char* kActionManageKillSwitch =
    "com.bringyour.network.manage-kill-switch";
inline constexpr const char* kActionTakeOverTunnel = "com.bringyour.network.take-over-tunnel";
inline constexpr const char* kActionReadLog = "com.bringyour.network.read-log";

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

// Cap on rpc_session_id, for the same reason: the 1 MiB frame bound limits the
// LINE. An identifier is a name, not a payload — a uuid is 36 bytes and 32
// random bytes are 64 in hex — so 128 is generous and still bounds a value the
// daemon copies into every `status` reply (polled at ~4 Hz) and holds for the
// life of a session.
inline constexpr size_t kMaxRpcSessionIdBytes = 128;

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
//     "::1" (the device rpc is a v4 loopback contract on every platform);
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

// SHAPE gate for rpc_session_id: non-empty, bounded, and printable ASCII with
// no space and no control byte. NOT an entropy check — nothing on the wire can
// prove a value was drawn from a CSPRNG, and the mint is the client's job (see
// StartTunnelRequest::rpc_session_id). What it does buy:
//   * a NUL or a newline can never reach the daemon's stderr, where the journal
//     is line-oriented and a forged line is a real (if minor) way to lie to
//     whoever reads a bug report;
//   * an unbounded identifier can never be latched for the life of a session
//     and re-copied into every status reply.
// Deliberately permissive about the ALPHABET: a uuid, hex, base64 and base64url
// all pass, so this cannot dictate the client's format.
inline bool LooksLikeRpcSessionId(const std::string& id) {
  if (id.empty() || id.size() > kMaxRpcSessionIdBytes) return false;
  for (const char c : id) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7f) return false;
  }
  return true;
}

// ---- verbs -----------------------------------------------------------------

enum class Verb {
  Hello,
  Status,
  StartTunnel,
  AttachTunnel,
  StopTunnel,
  SetProvide,
  StartProvider,
  ProviderStats,
  SetProvideExtender,
  SetMeasurementsEnabled,
  SetProvideExtenderPortMapping,
  ResetExtenders,
  SetKillSwitch,
  LocationOverrideAvailable,
  LocationOverrideWrite,
  LocationOverrideClear,
  UploadLogs,
  Logout,
  Unknown,
};

inline const char* ToString(Verb v) {
  switch (v) {
    case Verb::Hello: return "hello";
    case Verb::Status: return "status";
    case Verb::StartTunnel: return "start_tunnel";
    case Verb::AttachTunnel: return "attach_tunnel";
    case Verb::StopTunnel: return "stop_tunnel";
    case Verb::SetProvide: return "set_provide";
    case Verb::StartProvider: return "start_provider";
    case Verb::ProviderStats: return "provider_stats";
    case Verb::SetProvideExtender: return "set_provide_extender";
    case Verb::SetMeasurementsEnabled: return "set_measurements_enabled";
    case Verb::SetProvideExtenderPortMapping: return "set_provide_extender_port_mapping";
    case Verb::ResetExtenders: return "reset_extenders";
    case Verb::SetKillSwitch: return "set_kill_switch";
    case Verb::LocationOverrideAvailable: return "location_override_available";
    case Verb::LocationOverrideWrite: return "location_override_write";
    case Verb::LocationOverrideClear: return "location_override_clear";
    case Verb::UploadLogs: return "upload_logs";
    case Verb::Logout: return "logout";
    case Verb::Unknown: break;
  }
  return "unknown";
}

inline Verb VerbFromString(const std::string& s) {
  if (s == "hello") return Verb::Hello;
  if (s == "status") return Verb::Status;
  if (s == "start_tunnel") return Verb::StartTunnel;
  if (s == "attach_tunnel") return Verb::AttachTunnel;
  if (s == "stop_tunnel") return Verb::StopTunnel;
  if (s == "set_provide") return Verb::SetProvide;
  if (s == "start_provider") return Verb::StartProvider;
  if (s == "provider_stats") return Verb::ProviderStats;
  if (s == "set_provide_extender") return Verb::SetProvideExtender;
  if (s == "set_measurements_enabled") return Verb::SetMeasurementsEnabled;
  if (s == "set_provide_extender_port_mapping") return Verb::SetProvideExtenderPortMapping;
  if (s == "reset_extenders") return Verb::ResetExtenders;
  if (s == "set_kill_switch") return Verb::SetKillSwitch;
  if (s == "location_override_available") return Verb::LocationOverrideAvailable;
  if (s == "location_override_write") return Verb::LocationOverrideWrite;
  if (s == "location_override_clear") return Verb::LocationOverrideClear;
  if (s == "upload_logs") return Verb::UploadLogs;
  if (s == "logout") return Verb::Logout;
  return Verb::Unknown;
}

// ---- which polkit action gates which verb (pure, unit-testable) ------------
// nullptr means "no polkit check": hello (which MUST stay pre-authorization so
// version/SDK skew still renders — a peer that cannot authorize must still be
// TOLD WHY rather than dropped), status (polled at ~4 Hz during async bring-up;
// a round trip there would be intolerable, and it is redacted for a foreign uid
// instead — see RedactStatusForForeignUid), provider_stats (polled about once a
// second while the Earnings destination shows, and empty for a foreign uid on
// the same terms) and location_override_available, which is a capability
// query that changes nothing.
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
    // attach_tunnel SITS IN THE SAME ROW AS start_tunnel, DELIBERATELY, and
    // this is the one place the choice is worth defending. It is tempting to
    // read "attach takes over a running tunnel" as "attach is always a
    // take-over" and hard-wire kActionTakeOverTunnel. That is wrong twice:
    //   * The everyday attach is a GUI relaunching against ITS OWN uid's
    //     tunnel. That is the identical act start_tunnel's adoption path
    //     already performs (ControlServer::HandleStartTunnel -> CanAdopt)
    //     under kActionControlTunnel. Charging auth_admin_keep for the verb
    //     that says out loud what start_tunnel does silently would mean the
    //     honest verb costs a root password and the roundabout one does not —
    //     which does not protect anything, it just teaches every client to
    //     avoid attach_tunnel.
    //   * The case that IS a take-over is exactly the case cross_uid already
    //     names: a tunnel live under a DIFFERENT uid. There it gets
    //     kActionTakeOverTunnel (auth_admin_keep in all three slots) on the
    //     same terms as stop_tunnel and set_provide.
    // So the security boundary here is whose tunnel it is, never which verb
    // asked — and knowing the live instance_id + rpc_session_id is NOT part of
    // that boundary. Those identifiers are a pairing key, not a capability:
    // polkit decides, and a foreign uid that somehow learned them still faces
    // the admin prompt.
    case Verb::AttachTunnel:
    case Verb::StopTunnel:
    case Verb::SetProvide:
    // start_provider sits with set_provide: it applies the same provide mode,
    // to a device built without a tunnel. A provider-only session belongs to
    // the uid that started it, so another uid's start costs the take-over
    // action exactly as replacing their tunnel would.
    case Verb::StartProvider:
    // set_provide_extender too: it writes that session's provider extender
    // setting, the switch beside the provide mode.
    case Verb::SetProvideExtender:
    // set_measurements_enabled as well: the lag.dev speed tests switch under
    // the Extender switch, a setting of the same space that rides on the role.
    case Verb::SetMeasurementsEnabled:
    // set_provide_extender_port_mapping as well: the router port mapping switch
    // under the speed tests switch, another setting of that space that rides on
    // the role, and one that opens ports on the router.
    case Verb::SetProvideExtenderPortMapping:
    // reset_extenders as well: the daemon's spaces are shared by every user of
    // this machine, so a reset changes what another uid's live session dials
    // through, and that costs the take-over action.
    case Verb::ResetExtenders:
    case Verb::LocationOverrideWrite:
    case Verb::LocationOverrideClear:
      return cross_uid ? kActionTakeOverTunnel : kActionControlTunnel;
    case Verb::SetKillSwitch:
      // Its own id so a site can require a password for the kill switch and
      // not for Connect. Same defaults as control-tunnel, so it costs no extra
      // prompt on the normal path.
      return cross_uid ? kActionTakeOverTunnel : kActionManageKillSwitch;
    // upload_logs sends the daemon's log to URnetwork, so it asks for what
    // log_tail asks for, whoever runs the tunnel. Another uid's log is refused
    // outright, as log_tail refuses it (ControlServer::LogBelongsToOtherUid),
    // never escalated to the take-over action: that action decides who may
    // control a session, and a log is not something to take over.
    case Verb::UploadLogs:
      return kActionReadLog;
    // logout deletes what the daemon keeps for the account that signed out, so
    // it costs what a stop costs. Never the take-over action, whoever runs the
    // session: another user's live session is theirs, and the daemon refuses a
    // logout beside it whatever was authorized (ControlServer), so asking an
    // administrator could only buy a refusal.
    case Verb::Logout:
      return kActionControlTunnel;
    case Verb::Hello:
    case Verb::Status:
    case Verb::ProviderStats:
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
    // A human just launched the app or pressed Connect; an attach is that
    // press arriving at a tunnel that is already up.
    case Verb::AttachTunnel:
    case Verb::StopTunnel:
    case Verb::SetProvide:
    // a press of the connect page's Extender switch
    case Verb::SetProvideExtender:
    // a press of the lag.dev speed tests switch under it
    case Verb::SetMeasurementsEnabled:
    // and of the router port mapping switch under that
    case Verb::SetProvideExtenderPortMapping:
    // a confirmed press of Account > Extenders' Reset extenders
    case Verb::ResetExtenders:
    case Verb::SetKillSwitch:
    case Verb::LocationOverrideWrite:
    case Verb::LocationOverrideClear:
      return true;
    // Not a press: the GUI sends start_provider from its health poll as well
    // as after Disconnect, so a check that needs a password answers with a
    // challenge code (and the GUI backs off) instead of raising a dialog every
    // few seconds. At the console control-tunnel is allow_active=yes anyway.
    case Verb::StartProvider:
    // A press, but one the user has already been thanked for: the log upload
    // rides on feedback the server accepted, and its failure is silent by
    // design. A password dialog arriving after "Thanks for the feedback!" would
    // be a surprise, so a check that needs one answers with a challenge code
    // and the GUI falls back to its own path. read-log is allow_active=yes.
    case Verb::UploadLogs:
    // A press the first time, but a sign-out the daemon could not be told is
    // sent again from the health poll, so a check that needs a password
    // answers with a challenge code and the sign-out stays owed.
    case Verb::Logout:
    case Verb::Hello:
    case Verb::Status:
    case Verb::ProviderStats:
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

  // THE NAME OF THIS GENERATION — upstream's field, restored, and the value
  // attach_tunnel names later. It is minted in the SAME act as the three
  // fields above and stored with them: RpcSessionStore writes instance_id +
  // rpc_session_id + host_port to disk and puts the client private key and the
  // pinned server cert in the Secret Service, one record, one generation.
  //
  // WHO MINTS IT, AND WHY IT IS THE CLIENT. Half of the material this names
  // never reaches the daemon at all — client_pem and server_cert_pem stay in
  // the GUI — so only the GUI is in a position to name a generation and store
  // the name beside the secrets it belongs to. The daemon LATCHES what it is
  // given (TunnelHost::rpcSessionId_) and echoes it back on every reply; it
  // never invents one, because an id the client did not choose could not be
  // saved with the half it keeps, and an id minted per bring-up would be
  // unknown to the client until after the tunnel was already up.
  //
  // TWO PROPERTIES ARE REQUIRED OF THE MINT. Both are the client's to uphold
  // and only one of them can be checked from here:
  //   * UNGUESSABLE. Draw it from the CSPRNG — never a counter, a pid, a
  //     timestamp or the instance id. polkit is the security boundary (see the
  //     pairing-key note in ActionIdForVerb) and a foreign uid still faces the
  //     admin prompt, but a predictable name invites a SAME-uid process to
  //     attach to a session it did not create, and same-uid is the one place
  //     policy has nothing left to say.
  //   * IT CHANGES WHENEVER THE MATERIAL CHANGES. One generation, one name.
  //     The daemon enforces this for the LIVE session and nowhere else: a
  //     start_tunnel that re-uses the running generation's name while
  //     presenting different material is refused with
  //     kCodeTunnelAlreadyRunning rather than quietly rebuilt, because two key
  //     pairs answering to one name is exactly how a client ends up attached
  //     to a listener it cannot dial. Once a session is over the daemon
  //     forgets it, so re-use across sessions is unpoliced by anything but
  //     this contract.
  std::string rpc_session_id;
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
  if (!v.rpc_session_id.empty()) j["rpc_session_id"] = v.rpc_session_id;
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
  detail::Get(j, "rpc_session_id", v.rpc_session_id);
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
// The rpc GENERATION is REQUIRED, all four parts of it. This is a deliberate
// BEHAVIOUR change inside protocol v1 (the wire is still purely additive): an
// old GUI's start_tunnel now fails with kCodeRpcPinRequired instead of quietly
// getting an unpinned root listener on 127.0.0.1:12025. Per the hello
// exact-version match, a mismatched shipped pair is already refused before
// start_tunnel is reachable, so this branch turns "should be unreachable" into
// "is".
//
// rpc_session_id joined the other three here rather than sitting beside
// by_jwt, and that placement IS the contract: the name and the material are
// one atomic generation. A start that carries key material with no name
// produces a session no client can ever attach to (the identity in `status`
// would be half-empty); a start that carries a name with no material asks the
// daemon to publish an identity for a listener it never pinned. Validating
// them together is what makes "it changes whenever the material changes"
// checkable at all — the two are minted, sent, latched and forgotten as a
// unit.
inline std::optional<StartTunnelRejection> ValidateStartTunnelRequest(
    const StartTunnelRequest& req) {
  if (req.by_jwt.empty()) return StartTunnelRejection{"by_jwt is required", nullptr};
  if (req.instance_id.empty()) return StartTunnelRejection{"instance_id is required", nullptr};
  const int rpcParts = (req.rpc_server_pem.empty() ? 0 : 1) +
                       (req.rpc_client_cert_pem.empty() ? 0 : 1) +
                       (req.rpc_listen_hostport.empty() ? 0 : 1) +
                       (req.rpc_session_id.empty() ? 0 : 1);
  if (rpcParts == 0) {
    return StartTunnelRejection{
        "this start_tunnel carries no device-rpc pinning material; the daemon will not run "
        "an unauthenticated local rpc listener",
        kCodeRpcPinRequired};
  }
  if (rpcParts != 4) {
    // A half-supplied pair would make the daemon listen with a pinned server
    // while the GUI dials with the SDK default (or the reverse), which
    // presents as a DeviceRemote that connects and never populates. A
    // half-supplied IDENTITY is the same failure one step later: a session
    // that cannot be named cannot be re-attached to, so the next GUI launch
    // silently rebuilds a working tunnel instead.
    return StartTunnelRejection{
        "rpc_session_id, rpc_server_pem, rpc_client_cert_pem and rpc_listen_hostport must be "
        "sent together",
        kCodeRpcPinRequired};
  }
  if (!LooksLikeRpcSessionId(req.rpc_session_id)) {
    return StartTunnelRejection{
        "rpc_session_id must be 1..128 printable ASCII bytes with no space or control "
        "character",
        kCodeRpcPinInvalid};
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

// ---- attach_tunnel ---------------------------------------------------------
// Re-adopt a tunnel that is ALREADY UP, by naming the live session rather than
// re-describing it. The client sends the two identifiers of the session it
// believes is running; the daemon replies with the same StartTunnelReply a
// successful start would have produced (so one client-side code path handles
// both), or refuses. Nothing is torn down and nothing is rebuilt: this verb
// either matches the live session or it fails.
//
// It exists because the alternative — "start it again and hope the daemon
// notices it is the same session" — makes the client re-send by_jwt and the
// whole mTLS triple just to be told nothing changed.
//
// IT SUCCEEDS WHEN THE NAMED SESSION IS THE LIVE ONE. The daemon latches the
// accepted start_tunnel's (instance_id, rpc_session_id) at the up edge and
// publishes them in `status`, so ControlServer::HandleAttachTunnel has a real
// identity to compare against; the GUI keeps the same pair in its
// RpcSessionStore record, beside the client key and pinned server cert the
// Secret Service holds, and re-presents it after a relaunch.
//
// TWO DOORS, ONE OUTCOME, and they are not redundant. attach_tunnel names the
// session and sends NO key material, which is what a GUI that still has its
// Secret Service entry should use. start_tunnel's adoption path
// (TunnelHost::CanAdopt) re-describes the session in full and is what a GUI
// with no usable entry falls back to — it re-sends by_jwt and the whole
// generation and is adopted only if every byte matches. Both answer with the
// same StartTunnelReply and both leave the running tunnel untouched.
struct AttachTunnelRequest {
  std::string instance_id;
  std::string rpc_session_id;
};
inline void to_json(nlohmann::json& j, const AttachTunnelRequest& v) {
  j["instance_id"] = v.instance_id;
  j["rpc_session_id"] = v.rpc_session_id;
}
inline void from_json(const nlohmann::json& j, AttachTunnelRequest& v) {
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "rpc_session_id", v.rpc_session_id);
}
// BOTH identifiers are required, and neither may be defaulted. instance_id
// alone would let a client attach to whatever session happens to be up under
// the same pairing key after a credential rotation; rpc_session_id alone would
// not pin the DEVICE. Kept as upstream's optional<std::string> rather than
// converted to the StartTunnelRejection shape our start-tunnel validator grew:
// these two are plain field errors with no actionable client branch, which is
// exactly the case that shape reserves a nullptr code for.
inline std::optional<std::string> ValidateAttachTunnelRequest(
    const AttachTunnelRequest& req) {
  if (req.instance_id.empty()) return "instance_id is required";
  if (req.rpc_session_id.empty()) return "rpc_session_id is required";
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

  // THE LIVE SESSION'S IDENTITY, echoed back. Upstream fields, restored: this
  // is the reply attach_tunnel shares with start_tunnel, so the client learns
  // the two identifiers it must send to re-attach LATER from the reply that
  // created (or adopted) the session — never from its own local guess. That
  // matters most on the adoption path, where the id the daemon is actually
  // paired with is the id the FIRST start used, which is not necessarily the
  // one localState_ holds now (see RpcSession.hpp's instance-id trap).
  //
  // Absent parses "" on both, which is what a daemon predating the fields is —
  // and also what a daemon that has just ACCEPTED an ASYNC start sends, since
  // the identity is latched at the up edge and that reply is written while the
  // bring-up is still running (tunnel_state=starting, rpc_port=0). Same rule
  // as rpc_pinned beside it: on the async path the answer exists only in
  // `status` at the transition to Up. A client must never persist an empty
  // rpc_session_id as something to attach to later — that is a session it
  // cannot name, not a session named "".
  std::string instance_id;
  std::string rpc_session_id;
};
inline void to_json(nlohmann::json& j, const StartTunnelReply& v) {
  j["rpc_port"] = v.rpc_port;
  j["tunnel_state"] = ToString(v.tunnel_state);
  j["rpc_pinned"] = v.rpc_pinned;
  j["instance_id"] = v.instance_id;
  j["rpc_session_id"] = v.rpc_session_id;
}
inline void from_json(const nlohmann::json& j, StartTunnelReply& v) {
  detail::Get(j, "rpc_port", v.rpc_port);
  std::string state;
  detail::Get(j, "tunnel_state", state);
  // absent = a daemon predating the field, which only ever replied ok on a
  // completed start
  v.tunnel_state = state.empty() ? TunnelState::Up : TunnelStateFromString(state);
  detail::Get(j, "rpc_pinned", v.rpc_pinned);  // absent = false = fails closed
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "rpc_session_id", v.rpc_session_id);
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

// set_provide applies the mode to the live device and stashes it for the next
// start. With no tunnel session it also decides the provider-only device: a
// mode that does not provide while disconnected (provide::ProviderRuns)
// retires it, any other mode is applied to it.
struct SetProvideRequest {
  std::string mode;  // "never"|"always"|"network"|"auto"|"manual"
};
inline void to_json(nlohmann::json& j, const SetProvideRequest& v) { j["mode"] = v.mode; }
inline void from_json(const nlohmann::json& j, SetProvideRequest& v) {
  detail::Get(j, "mode", v.mode);
}

// logout: the account signed out of this app (SignOut.hpp; owner decision
// 2026-10-05: "logout should not cross contaminate other networks. Each
// network should start fresh"). The daemon ends the session, the provider-only
// device and a log upload's standalone device as an explicit stop does,
// forgets the provide mode and the kill switch the account asked for, deletes
// the device identity it keeps (one per daemon), and logs out what its sdk
// stored in the request's network space for the account: the client
// credential and instance a device persists when it starts, among the rest. So
// the next account signed in starts on a new identity and a clean space. The
// space's extender state (its directory, gossip role and identity, and the
// provider extender setting) stays, as every sign-out leaves it (the sdk's
// LocalState.logout keeps it; owner, 2026-10-05).
//
// Refused, with nothing cleared, while another user's session is live
// (kCodeAuthNotTunnelOwner: their session is theirs, whatever was authorized),
// while another client of this uid owns the live tunnel
// (kCodeTunnelOwnedByOtherClient), and while a bring-up owns the session
// (kCodeStartInProgress: never waited behind, as no main-loop verb is). The GUI
// keeps a refused sign-out owed and sends it again.
//
// A new verb, additive within protocol v1, mirroring the Windows service's
// logout. A daemon that predates it answers kErrorUnknownVerb, which the GUI
// counts as done: such a daemon has nothing to clear the identity with, and a
// sign-out kept owed to it would hold every start forever. An absent space is
// the compiled-in default, as for start_tunnel.
struct LogoutRequest {
  std::string network_space_json;
};
inline void to_json(nlohmann::json& j, const LogoutRequest& v) {
  j["network_space_json"] = v.network_space_json;
}
inline void from_json(const nlohmann::json& j, LogoutRequest& v) {
  detail::Get(j, "network_space_json", v.network_space_json);
}

// stop_tunnel. `disconnect` says the owning user pressed Disconnect: the daemon
// may keep the session's DeviceLocal running as the provider-only device when
// one could start in its place (provide::KeepsSessionDevice), so its
// transports, peers and contracts carry over. Every other stop (a quit, a
// sign-out, a server change, a failed start, the tray's recovery item) leaves
// it false and closes the device, retiring a provider-only device too, as
// stop_tunnel always has. Additive within v1: absent parses false, which is
// what a GUI predating it sends, and a daemon predating it ignores it.
struct StopTunnelRequest {
  bool disconnect = false;
};
inline void to_json(nlohmann::json& j, const StopTunnelRequest& v) {
  j = nlohmann::json::object();
  if (v.disconnect) j["disconnect"] = true;
}
inline void from_json(const nlohmann::json& j, StopTunnelRequest& v) {
  detail::Get(j, "disconnect", v.disconnect);
}

// set_provide_extender: the connect page's Extender switch while no tunnel
// session's device takes it (support inbox 1521, P008). The setting belongs to
// the network space (`.provide_extender` in the daemon's storage), and every
// device reads it from its space when it starts, so the daemon writes it
// through the device that runs, or with none into the space the last device
// ran in (provide::ExtenderSettingTargetFor). A Connect after the change reads
// it, and so does a provider-only device after a change made while connected.
// Gated like set_provide. A new verb, additive within protocol v1: the GUI
// sends it only to a daemon whose provider_stats said it takes it, so a daemon
// that predates it is never asked (it would answer kErrorUnknownVerb).
struct SetProvideExtenderRequest {
  bool provide_extender = true;
};
inline void to_json(nlohmann::json& j, const SetProvideExtenderRequest& v) {
  j["provide_extender"] = v.provide_extender;
}
// Strict, unlike the tolerant reads: a write whose value did not arrive must
// not write a default the user never chose. The daemon answers the throw as an
// error reply.
inline void from_json(const nlohmann::json& j, SetProvideExtenderRequest& v) {
  auto it = j.find("provide_extender");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error("set_provide_extender requires provide_extender (a boolean)");
  }
  v.provide_extender = it->get<bool>();
}

// ---- set_measurements_enabled ----------------------------------------------
// The lag.dev speed tests switch while no tunnel session's device takes it
// (lag.dev/PLAN.md C4, decision 18): whether the extender role serves
// lag.dev's measurement API. Like the provider extender setting it belongs to
// the network space (`.measurements_enabled`) and every device reads it from
// its space when it starts, so the daemon writes it by the same rule
// (provide::ExtenderSettingTargetFor). Gated like set_provide_extender. A new
// verb, additive within protocol v1: the GUI sends it only to a daemon whose
// provider_stats said it takes it (measurements_enabled_writable).
struct SetMeasurementsEnabledRequest {
  bool measurements_enabled = true;
};
inline void to_json(nlohmann::json& j, const SetMeasurementsEnabledRequest& v) {
  j["measurements_enabled"] = v.measurements_enabled;
}
// Strict, as set_provide_extender's: a write whose value did not arrive must
// not write a default the user never chose.
inline void from_json(const nlohmann::json& j, SetMeasurementsEnabledRequest& v) {
  auto it = j.find("measurements_enabled");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error("set_measurements_enabled requires measurements_enabled (a boolean)");
  }
  v.measurements_enabled = it->get<bool>();
}

// ---- set_provide_extender_port_mapping -------------------------------------
// The router port mapping switch while no tunnel session's device takes it
// (connect EXTENDER.md S8): whether the extender role asks the router, by UPnP,
// NAT-PMP or PCP, to forward its ports (tcp 443, udp 443 and udp 4053) to this
// device. Off by default on every app (owner, 2026-11-11). Like the provider
// extender setting it belongs to the network space
// (`.provide_extender_port_mapping`) and every device reads it from its space
// when it starts, so the daemon writes it by the same rule
// (provide::ExtenderSettingTargetFor): through the device that runs, or with
// none into the space the last device ran in. Gated like set_provide_extender.
// A new verb, additive within protocol v1: the GUI sends it only to a daemon
// whose provider_stats said it takes it (provide_extender_port_mapping_writable).
struct SetProvideExtenderPortMappingRequest {
  bool provide_extender_port_mapping = false;
};
inline void to_json(nlohmann::json& j, const SetProvideExtenderPortMappingRequest& v) {
  j["provide_extender_port_mapping"] = v.provide_extender_port_mapping;
}
// Strict, as set_provide_extender's: a write whose value did not arrive must
// not write a default the user never chose, least of all one that opens ports
// on the router.
inline void from_json(const nlohmann::json& j, SetProvideExtenderPortMappingRequest& v) {
  auto it = j.find("provide_extender_port_mapping");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error(
        "set_provide_extender_port_mapping requires provide_extender_port_mapping (a boolean)");
  }
  v.provide_extender_port_mapping = it->get<bool>();
}

// ---- reset_extenders -------------------------------------------------------
// Account > Extenders' Reset extenders (connect EXTENDER.md E7). The GUI resets
// its own network space (urnet::NetworkSpace::resetExtenders), which persists
// the cleared extender values with the id of the reset, and sends that id here
// with the space's key. The daemon applies the same reset to the space it holds
// under that key (applyExtenderReset): everything learned about extenders is
// cleared, and the space's extender client and node relearn as on a first run.
// The tunnel session's device and the provider-only device both run in that
// space, so one call covers both. Their live extender paths keep running; new
// dials draw from the fresh directory.
//
// The verb makes the reset immediate, and it is not the only path. The id
// travels in the space's values, and the sdk applies a reset whose id is newer
// than the last one the daemon's storage applied when the space is imported, so
// a daemon that was unreachable, busy or never asked applies it at its next
// start_tunnel or start_provider. An id the space applied already, or an older
// one, changes nothing, which also makes the verb safe to send again: a busy
// refusal (kCodeStartInProgress) comes while a bring-up imports the space as it
// was before the reset, so the GUI sends the same request again once that
// bring-up settled (ExtenderReset.hpp), asking for no dialog (`interactive`
// below).
//
// One daemon serves every user of the machine and keeps its spaces in
// /var/lib/urnetwork/sdk for all of them, so one user's reset of a key resets
// the daemon's state for that key for everyone. That is intended (extender
// knowledge is per installation, and local accounts trust each other that
// far), and it is gated like set_provide_extender: control-tunnel while the
// caller owns the live session or none is live, take-over-tunnel while another
// uid's tunnel or provider session is live, since the reset changes what that
// session dials through. Another user's own GUI space is untouched, and the id
// their next import carries is older, so it applies nothing.
//
// A new verb, additive within protocol v1: a daemon that predates it answers
// kErrorUnknownVerb.
struct ResetExtendersRequest {
  // The key of the space the GUI reset (urnet::NetworkSpaceKey).
  std::string host_name;
  std::string env_name;
  // The id the GUI's resetExtenders returned.
  std::string extender_reset_id;
  // False for a reset the GUI sends again without a press (ExtenderReset.hpp).
  // The daemon then never raises an authentication dialog for it: beside
  // another uid's live session, which only the take-over action reaches, it
  // is refused before any check (kCodeAuthNotTunnelOwner), and otherwise it is
  // checked without interaction, so a check that would need a dialog is
  // refused (kCodeAuthRequired) instead. Sent only when false; absent is a
  // press.
  bool interactive = true;
};
inline void to_json(nlohmann::json& j, const ResetExtendersRequest& v) {
  j["host_name"] = v.host_name;
  j["env_name"] = v.env_name;
  j["extender_reset_id"] = v.extender_reset_id;
  if (!v.interactive) j["interactive"] = false;
}

// The request's interactive field, read off the raw frame where the daemon
// chooses how to authorize it, before the frame is parsed, so it never throws.
// Absent or null is a press; anything but a boolean reads as no dialog, the
// side that cannot prompt.
inline bool ResetExtendersAllowsInteraction(const nlohmann::json& request) {
  const auto it = request.find("interactive");
  if (it == request.end() || it->is_null()) return true;
  return it->is_boolean() && it->get<bool>();
}

// Each field is a name, not a payload, and the daemon writes the key it reset
// to its line-oriented log: bounded, and free of control bytes.
inline constexpr size_t kMaxResetExtendersFieldBytes = 256;

// Both halves apply it, the GUI before a frame is sent and the daemon when it
// parses one, so the rule is one function. Every field is required.
inline std::optional<std::string> ValidateResetExtendersRequest(
    const ResetExtendersRequest& req) {
  auto invalid = [](const char* name, const std::string& value) -> std::optional<std::string> {
    if (value.empty()) return std::string(name) + " is required";
    if (value.size() > kMaxResetExtendersFieldBytes) return std::string(name) + " is too long";
    for (const char c : value) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (u < 0x20 || u == 0x7f) return std::string(name) + " has a control character";
    }
    return std::nullopt;
  };
  if (auto error = invalid("host_name", req.host_name)) return error;
  if (auto error = invalid("env_name", req.env_name)) return error;
  return invalid("extender_reset_id", req.extender_reset_id);
}

// Tolerant reads like every payload here, then strict: a reset that names no
// space or no reset must not be applied to a default one. The daemon answers
// the throw as an error reply.
inline void from_json(const nlohmann::json& j, ResetExtendersRequest& v) {
  detail::Get(j, "host_name", v.host_name);
  detail::Get(j, "env_name", v.env_name);
  detail::Get(j, "extender_reset_id", v.extender_reset_id);
  detail::Get(j, "interactive", v.interactive);
  if (const auto invalid = ValidateResetExtendersRequest(v)) {
    throw std::runtime_error("reset_extenders: " + *invalid);
  }
}

// Whether the daemon held a space under the key and the reset was new to it.
// False is no failure: the daemon holds no space before its first device, and
// a reset the space applied already has been applied. Absent parses false.
struct ResetExtendersReply {
  bool reset = false;
};
inline void to_json(nlohmann::json& j, const ResetExtendersReply& v) { j["reset"] = v.reset; }
inline void from_json(const nlohmann::json& j, ResetExtendersReply& v) {
  detail::Get(j, "reset", v.reset);
}

// ---- start_provider --------------------------------------------------------
// Keep providing while disconnected (support inbox 1521, P008). The daemon
// builds a provider-only DeviceLocal: the persisted device identity, this
// session's credentials and network space, the requested provide mode — and
// nothing else. No tun, no capture routes, no DNS change, no nftables change,
// no egress marker, and no device RPC listener: the GUI binds no DeviceRemote
// to it, so nothing in the GUI can mistake it for a tunnel session, and the
// user's own traffic is routed exactly as if URnetwork were not running.
// ProvideLifecycle.hpp carries the whole argument.
//
// A new verb, not a start_tunnel field. A daemon that predates it answers
// `unknown verb`; a start_tunnel field it silently dropped would have brought
// up a full tunnel — capture routes and all — for a request that asked for
// none (the Windows protocol notes record exactly that hazard for its mode
// field). Additive within protocol v1 for the reasons the version comment
// above gives.
//
// The same request again is idempotent: the daemon keeps a device built from
// identical credentials and only applies the mode, which is also how a
// relaunched GUI adopts the provider its previous run left running.
//
// Refusal codes, declared here because ValidateStartProviderRequest returns the
// first; the daemon emits the others (and kCodeStartInProgress while a tunnel
// bring-up runs).
//
// The mode does not provide while disconnected (never, or a mode the sdk does
// not enforce): the daemon will not run a device for it.
inline constexpr const char* kCodeProvideModeOff = "provide_mode_off";
// A tunnel session exists: its DeviceLocal is already the provider, and a
// second device under the same identity would compete with it.
inline constexpr const char* kCodeTunnelSessionActive = "tunnel_session_active";
// The kill-switch floor holds this machine blocked after an unexpected drop.
// It stays the only thing in force until the user reconnects or lifts it.
inline constexpr const char* kCodeKillSwitchArmed = "kill_switch_armed";
// The device could not be built; the message carries the SDK's error.
inline constexpr const char* kCodeProviderStartFailed = "provider_start_failed";

struct StartProviderRequest {
  // The same four as StartTunnelRequest, with the same meaning: the client jwt
  // the device registers with, the device instance id, the app version and the
  // GUI's active network space (empty = the compiled-in default).
  std::string by_jwt;
  std::string instance_id;
  std::string app_version;
  std::string network_space_json;
  // The provide control mode ("always", "network", "auto"); one that does not
  // provide while disconnected is refused with kCodeProvideModeOff.
  std::string provide_mode;
  // The provider transport policy the user edited (urnet::TransportSettings as
  // JSON), empty when never edited: the tunnel session seeds its device from
  // the same mirror (SdkHost::BindRemoteDeviceLocked), so providing behaves
  // the same connected or not.
  std::string provider_transport_settings_json;
};
inline void to_json(nlohmann::json& j, const StartProviderRequest& v) {
  j["by_jwt"] = v.by_jwt;
  j["instance_id"] = v.instance_id;
  j["app_version"] = v.app_version;
  if (!v.network_space_json.empty()) j["network_space_json"] = v.network_space_json;
  j["provide_mode"] = v.provide_mode;
  if (!v.provider_transport_settings_json.empty()) {
    j["provider_transport_settings_json"] = v.provider_transport_settings_json;
  }
}
inline void from_json(const nlohmann::json& j, StartProviderRequest& v) {
  detail::Get(j, "by_jwt", v.by_jwt);
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "app_version", v.app_version);
  detail::Get(j, "network_space_json", v.network_space_json);
  detail::Get(j, "provide_mode", v.provide_mode);
  detail::Get(j, "provider_transport_settings_json", v.provider_transport_settings_json);
}

// Both halves apply it, the GUI before a frame is sent and the daemon before
// anything is built, so the rule is one function. Reuses StartTunnelRejection:
// a message, and a code only where the client has a branch to take.
inline std::optional<StartTunnelRejection> ValidateStartProviderRequest(
    const StartProviderRequest& req) {
  if (req.by_jwt.empty()) return StartTunnelRejection{"by_jwt is required", nullptr};
  if (req.instance_id.empty()) return StartTunnelRejection{"instance_id is required", nullptr};
  if (!provide::ProviderRuns(provide::ControlModeFrom(req.provide_mode), /*connected=*/false)) {
    return StartTunnelRejection{
        "provide mode '" + req.provide_mode + "' does not provide while disconnected",
        kCodeProvideModeOff};
  }
  if (!req.provider_transport_settings_json.empty()) {
    const nlohmann::json settings =
        nlohmann::json::parse(req.provider_transport_settings_json, nullptr,
                              /*allow_exceptions=*/false);
    if (!settings.is_object()) {
      return StartTunnelRejection{"provider_transport_settings_json is not a JSON object",
                                  nullptr};
    }
  }
  return std::nullopt;
}

namespace detail {
// RFC 4648 §5 base64url, padding optional, as a jwt segment carries it. False
// for any other character.
inline bool DecodeBase64Url(std::string_view in, std::string& out) {
  out.clear();
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : in) {
    int value;
    if (c >= 'A' && c <= 'Z') value = c - 'A';
    else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
    else if (c >= '0' && c <= '9') value = c - '0' + 52;
    else if (c == '-') value = 62;
    else if (c == '_') value = 63;
    else if (c == '=') break;
    else return false;
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFFu));
    }
  }
  return true;
}
}  // namespace detail

// The client a credential names: its network_id and client_id claims, read
// without verifying the signature, as the sdk reads them (parseByJwtClientId)
// and as this daemon already takes the token from the GUI over its socket. A
// refreshed token names the same client. Anything that is not a jwt naming a
// client is its own subject, compared whole, as every credential was before.
inline std::string JwtSubject(std::string_view jwt) {
  const std::size_t first = jwt.find('.');
  const std::size_t second =
      first == std::string_view::npos ? first : jwt.find('.', first + 1);
  std::string payload;
  if (second == std::string_view::npos ||
      !detail::DecodeBase64Url(jwt.substr(first + 1, second - first - 1), payload)) {
    return std::string(jwt);
  }
  const nlohmann::json claims = nlohmann::json::parse(payload, nullptr, false);
  if (!claims.is_object()) return std::string(jwt);
  const auto client = claims.find("client_id");
  if (client == claims.end() || !client->is_string() ||
      client->get_ref<const std::string&>().empty()) {
    return std::string(jwt);
  }
  const auto network = claims.find("network_id");
  const std::string networkId = network != claims.end() && network->is_string()
                                    ? network->get<std::string>()
                                    : std::string();
  return "client:" + networkId + "/" + client->get<std::string>();
}

// The identity a DeviceLocal is built from (provide::DeviceIdentity), as each
// request carries it: the client its credential names, and views into the
// request for the rest, valid while the request lives.
inline provide::DeviceIdentity IdentityOf(const StartProviderRequest& r) {
  return {JwtSubject(r.by_jwt), r.network_space_json, r.instance_id, r.app_version};
}
inline provide::DeviceIdentity IdentityOf(const StartTunnelRequest& r) {
  return {JwtSubject(r.by_jwt), r.network_space_json, r.instance_id, r.app_version};
}

// "A device built from `a` can keep running for `b`." The identity it was
// built from must match: the client its credential names, its instance and
// version and its network space. A refreshed credential for the same client is
// the same device. The provide mode and the provider transport policy may
// differ: the running device takes both in place, as the build applies them
// right after construction. That is how a saved network space value still
// reaches a running provider: the bootstrap DoH servers (P216), the VLESS
// server, the private extender and the server itself all live in
// network_space_json, so a saved one is another device, and the GUI sends
// start_provider again when one is saved (SdkHost::ReconcileProvider with
// settingsChanged).
inline bool SameProviderDevice(const StartProviderRequest& a, const StartProviderRequest& b) {
  return provide::SameIdentity(IdentityOf(a), IdentityOf(b));
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
  // THE LIVE SESSION'S IDENTITY — and THE FIELDS attach_tunnel COMPARES
  // AGAINST. Both are latched from the accepted start_tunnel at the up edge
  // (TunnelHost::instanceId_ / rpcSessionId_, published in the same locked
  // block that flips the state to Up) and cleared on teardown. Two invariants
  // follow, and both are load-bearing: this daemon cannot publish Up with a
  // half-empty identity, and an attach can never match a session that is
  // merely `starting`.
  //
  // They are also what upstream's RpcSessionStore matches its stored record
  // against (RpcSessionMatchesStatus), which is how a relaunching GUI decides
  // between attaching and building a new session.
  //
  // NOT SHIPPED TO A FOREIGN UID: RedactStatusForForeignUid keeps four fields
  // and drops everything else, these two included. The socket is
  // world-connectable under polkit, and while polkit — not knowledge of the
  // name — is what gates the attach, handing every local process the attach
  // name of somebody else's session is a leak with no purpose.
  std::string instance_id;    // exact live DeviceLocal identity ("" while down)
  std::string rpc_session_id; // opaque per-tunnel RPC credential generation
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
  std::string dns_detail;    // always set: which tier holds DNS, or why none could
  KillSwitchState kill_switch = KillSwitchState::Off;
  std::string kill_switch_detail;  // why it is Failed ("" otherwise)
  // OFF-TUNNEL IPv6 is blocked (leak prevention, never gated on the kill
  // switch): v6 leaves this machine through the tunnel or not at all.
  bool ipv6_blocked = false;
  // The v6 half of the tunnel is in force: the tun carries a v6 address and
  // the v6 capture routes. false with tunnel_state=up means the host has IPv6
  // disabled (kernel ipv6.disable / net.ipv6.conf.all.disable_ipv6), where v6
  // can neither be carried nor leak. Absent from an older daemon = false.
  bool ipv6_captured = false;
  std::string tunnel_interface;    // "urnet0" while up
  // Why the last session ended: "user" (an explicit stop_tunnel), "io_loop"
  // (the SDK loop finished under us), "start_failed", "daemon_shutdown",
  // "orphaned", a protective teardown's own reason ("tunnel_storm",
  // "egress_unprotected" and the like), one of the dead-tunnel failsafe's
  // kStopReasonFailsafe* (IsFailsafeStop), or "" when no session has ended.
  std::string stop_reason;
  // The dead-tunnel failsafe (TunnelWatchdog.hpp) is counting down on the live
  // session and will end it soon unless something gets through, so the app can
  // warn before the teardown rather than explain it after. Additive within v1;
  // absent parses false, which is what a daemon without the failsafe is.
  bool failsafe_armed = false;
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

  // ---- the provider-only device (start_provider) --------------------------
  // Additive within v1; absent parses as "not running", which is what a daemon
  // predating start_provider is. A DeviceLocal provides without a tunnel
  // because the user is disconnected and their provide mode keeps a provider
  // running (ProvideLifecycle.hpp). Never true beside a tunnel session: that
  // session's device is the provider, and the GUI reads it over the device RPC.
  bool provider_running = false;
  // The control mode it runs under ("" while none), so the GUI can tell whether
  // a mode it just picked still has to be applied.
  std::string provider_control_mode;
  // Its live provide tier (provide::Tier: 0 none, 1 network, 3 public) and
  // whether it holds a network-mode provide key: the Connect page's provide
  // dot and discoverable line, which have no DeviceRemote to read while
  // disconnected.
  int64_t provider_mode = 0;
  bool provider_network_key = false;
  // How many clients it provides to: its connected network peers, the count
  // the GUI reads off a tunnel session's device (getNetworkPeers), for the
  // provide line under the connect controls. -1 = not read: no provider runs,
  // the read failed, or the daemon predates the field (absent parses -1). The
  // GUI then shows no count, never a 0 it did not read.
  int64_t provider_client_count = -1;
  // Those peers themselves, as the sdk's NetworkPeerList json with each peer cut
  // to what the GUI's peer rows show and connect with (client id, provide
  // enabled, device name and spec): the network peers on the Connect page, in
  // the location chooser and on the Network page, which a tunnel session's
  // PeerViewController feeds while connected. Read with the count, in one
  // reading. "" = not read, which is what a daemon predating the field sends:
  // the GUI then leaves its peer lists as they are and the count stands alone.
  // RedactStatusForForeignUid drops it like every field it does not list.
  std::string provider_network_peers_json;

  // ---- the network country (P052) -----------------------------------------
  // The country of the mobile network this machine is on, as the daemon reads
  // it from ModemManager (NetworkCountry.hpp): lower case ISO 3166-1 alpha-2
  // while the default route leaves through a registered modem, "" on every
  // other network. The daemon applies it to the devices it builds; the GUI
  // applies it to its own process, whose sign-in and api dials draw their
  // extender names from the same spoof list. Additive within v1; absent parses
  // "", which is what a daemon predating it is and what "no country" means
  // anyway. RedactStatusForForeignUid drops it like every field it does not
  // list, so another uid's GUI reports no country rather than this one.
  std::string network_country_code;

  // ---- the log upload (upload_logs) -----------------------------------------
  // The last upload "send feedback with logs" asked the daemon for, as
  // logupload::Flight reads it: its id (the upload_logs reply's upload_id; 0
  // before the first), where it is (logupload::ToString(FlightState): "queued",
  // "running", "uploaded", "refused", "failed") and the device that carries it.
  // The GUI reads the outcome of its upload here. Additive within v1; absent
  // parses 0 and "". RedactStatusForForeignUid drops them like every field it
  // does not list: they describe another user's feedback.
  int64_t log_upload_id = 0;
  std::string log_upload_state;
  std::string log_upload_carrier;

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
  j["instance_id"] = v.instance_id;
  j["rpc_session_id"] = v.rpc_session_id;
  j["error"] = v.error;
  j["error_code"] = v.error_code;
  j["routes_installed"] = v.routes_installed;
  j["egress_protected"] = v.egress_protected;
  j["dns_applied"] = v.dns_applied;
  j["dns_detail"] = v.dns_detail;
  j["kill_switch"] = ToString(v.kill_switch);
  j["kill_switch_detail"] = v.kill_switch_detail;
  j["ipv6_blocked"] = v.ipv6_blocked;
  j["ipv6_captured"] = v.ipv6_captured;
  j["tunnel_interface"] = v.tunnel_interface;
  j["stop_reason"] = v.stop_reason;
  j["failsafe_armed"] = v.failsafe_armed;
  j["up_since_millis"] = v.up_since_millis;
  j["owner_connected"] = v.owner_connected;
  j["rpc_pinned"] = v.rpc_pinned;
  j["provider_running"] = v.provider_running;
  j["provider_control_mode"] = v.provider_control_mode;
  j["provider_mode"] = v.provider_mode;
  j["provider_network_key"] = v.provider_network_key;
  j["provider_client_count"] = v.provider_client_count;
  j["provider_network_peers_json"] = v.provider_network_peers_json;
  j["network_country_code"] = v.network_country_code;
  j["log_upload_id"] = v.log_upload_id;
  j["log_upload_state"] = v.log_upload_state;
  j["log_upload_carrier"] = v.log_upload_carrier;
  j["redacted"] = v.redacted;
}
inline void from_json(const nlohmann::json& j, StatusReply& v) {
  std::string state;
  detail::Get(j, "tunnel_state", state);
  v.tunnel_state = TunnelStateFromString(state);
  detail::Get(j, "rpc_port", v.rpc_port);
  detail::Get(j, "client_id", v.client_id);
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "rpc_session_id", v.rpc_session_id);
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
  detail::Get(j, "ipv6_captured", v.ipv6_captured);  // absent = false = understates
  detail::Get(j, "tunnel_interface", v.tunnel_interface);
  detail::Get(j, "stop_reason", v.stop_reason);
  detail::Get(j, "failsafe_armed", v.failsafe_armed);  // absent = no countdown
  detail::Get(j, "up_since_millis", v.up_since_millis);
  detail::Get(j, "owner_connected", v.owner_connected);
  detail::Get(j, "rpc_pinned", v.rpc_pinned);  // absent = false = fails closed
  detail::Get(j, "provider_running", v.provider_running);  // absent = not running
  detail::Get(j, "provider_control_mode", v.provider_control_mode);
  detail::Get(j, "provider_mode", v.provider_mode);
  detail::Get(j, "provider_network_key", v.provider_network_key);
  detail::Get(j, "provider_client_count", v.provider_client_count);  // absent = not read
  detail::Get(j, "provider_network_peers_json", v.provider_network_peers_json);  // absent = not read
  detail::Get(j, "network_country_code", v.network_country_code);  // absent = no country
  detail::Get(j, "log_upload_id", v.log_upload_id);        // absent = no upload
  detail::Get(j, "log_upload_state", v.log_upload_state);
  detail::Get(j, "log_upload_carrier", v.log_upload_carrier);
  detail::Get(j, "redacted", v.redacted);      // absent = false = a full status
}

// The facts provide::DisconnectedProviderStep decides on, read off one status.
// providerControlMode points into `status`, which must outlive the result.
inline provide::DaemonProviderFacts ProviderFactsFrom(const StatusReply& status) {
  provide::DaemonProviderFacts facts;
  facts.answered = true;
  facts.redacted = status.redacted;
  facts.tunnelSession = status.tunnel_state == TunnelState::Starting ||
                        status.tunnel_state == TunnelState::Up ||
                        status.tunnel_state == TunnelState::Stopping;
  facts.killSwitchArmed = status.kill_switch == KillSwitchState::Armed;
  facts.providerRunning = status.provider_running;
  facts.ownerConnected = status.owner_connected;
  facts.providerControlMode = status.provider_control_mode;
  return facts;
}

// The stop reasons of the dead-tunnel failsafe (TunnelWatchdog.hpp), one per
// rule: no proven exit for 90 s, traffic in and nothing back for 20 s, the SDK
// not answering for 30 s. The spellings are Windows' TunnelStatus::stop_reason.
inline constexpr const char* kStopReasonFailsafeNoExit = "failsafe_no_exit";
inline constexpr const char* kStopReasonFailsafeNoInbound = "failsafe_no_inbound";
inline constexpr const char* kStopReasonFailsafeSdkUnresponsive = "failsafe_sdk_unresponsive";

// The daemon ended the session by itself because it carried nothing. One
// predicate for every surface, and it knows a failsafe reason added later.
inline bool IsFailsafeStop(const std::string& stopReason) {
  return stopReason.rfind("failsafe_", 0) == 0;
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

// ---- provider_stats --------------------------------------------------------
// What the GUI's provider statistics read while no tunnel session runs: the
// provider-only device's (start_provider) provider series, transport share and
// provider status, and its extender role, as the SDK's own view controllers in
// the daemon publish them (support inbox 1521, P008). With a tunnel session the
// GUI reads the same facts off its DeviceRemote, whose controllers run in the
// GUI; the provider-only device has no DeviceRemote, so the daemon reads them
// for it. They are the Earnings page's provider plots and their gate, its "no
// traffic yet" line, the provider status behind the reason line, the demand
// histogram and "Why?", its read-only extender row and extender plot, and the
// connect page's Extender switch.
//
// Polled, about once a second while the Earnings or the connect destination is
// on screen, so it is answered like `status`: no polkit check, and an empty
// reply for a caller whose status would be redacted (another uid's session;
// the series and the status describe that user's provider). A new verb,
// additive within protocol v1: a daemon that predates it answers
// kErrorUnknownVerb, and the GUI then shows no provider statistics while
// disconnected, as before.
struct ProviderStatsRequest {
  // The Earnings destination is on screen: keep the provider status controller
  // polling GET /network/provider-status (about once a minute). The daemon
  // stops it once requests that ask stop arriving (provide::ProviderStatusLease),
  // so a GUI that goes away leaves nothing polling the API.
  bool poll_status = false;
};
inline void to_json(nlohmann::json& j, const ProviderStatsRequest& v) {
  j["poll_status"] = v.poll_status;
}
inline void from_json(const nlohmann::json& j, ProviderStatsRequest& v) {
  detail::Get(j, "poll_status", v.poll_status);
}

// The SDK payloads travel as the SDK's own JSON (its generated to_json and
// from_json), the way provider_transport_settings_json does, so this header
// stays free of SDK types.
struct ProviderStatsReply {
  // A provider-only device runs. Every other field is empty without one.
  bool running = false;
  // The device reports provider packet stats (its getProviderPacketStats is
  // not nil): with the provide control mode, the provider plots' gate.
  bool has_provider_stats = false;
  // The contract view controller's provider series (urnet::ThroughputPointList)
  // and its transport distribution (urnet::TransportDistribution, whose
  // ByteCount is the window's provider bytes); "" while it has none.
  std::string provider_throughput_points_json;
  std::string provider_transport_distribution_json;
  // The provider status controller: one is open, a poll has succeeded, the
  // last failed poll's error ("" once one succeeds), and this device's status
  // (urnet::ProviderStatus; "" before a poll, or while the network lists no
  // status for this device).
  bool status_open = false;
  bool status_loaded = false;
  std::string status_last_fetch_error;
  std::string provider_status_json;
  // The device's own extender role (urnet::ExtenderProvideStatus, its
  // getExtenderProvideStatus: the read-only extender row, and in Enabled the
  // running state behind the extender statistics) and the contract view
  // controller's extender series (urnet::ThroughputPointList, the traffic the
  // role relayed, in the Remote route). "" while the device or the controller
  // has none. A daemon that predates them sends neither, which reads the same:
  // the extender row and plot stay hidden while disconnected, as before.
  std::string extender_provide_status_json;
  std::string extender_throughput_points_json;
  // The provider extender setting read beside the role (getProvideExtender),
  // the connect page's Extender switch, and that this daemon takes the
  // switch's write (set_provide_extender). A daemon that predates them sends
  // neither, which reads as no writer: the switch stays hidden while
  // disconnected, as before.
  bool provide_extender = false;
  bool provide_extender_writable = false;
  // The lag.dev measurements setting read beside them (getMeasurementsEnabled),
  // the speed tests switch under the Extender switch, and that this daemon
  // takes its write (set_measurements_enabled). A daemon that predates them
  // sends neither, which reads as the setting on and no writer: that switch
  // stays hidden while disconnected, and the setting still applies.
  bool measurements_enabled = true;
  bool measurements_enabled_writable = false;
  // The router port mapping setting read beside them
  // (getProvideExtenderPortMapping), the switch under the speed tests switch,
  // and that this daemon takes its write (set_provide_extender_port_mapping). A
  // daemon that predates them sends neither, which reads as the setting off and
  // no writer: that switch stays hidden while disconnected.
  bool provide_extender_port_mapping = false;
  bool provide_extender_port_mapping_writable = false;
};
inline void to_json(nlohmann::json& j, const ProviderStatsReply& v) {
  j["running"] = v.running;
  j["has_provider_stats"] = v.has_provider_stats;
  j["provider_throughput_points_json"] = v.provider_throughput_points_json;
  j["provider_transport_distribution_json"] = v.provider_transport_distribution_json;
  j["status_open"] = v.status_open;
  j["status_loaded"] = v.status_loaded;
  j["status_last_fetch_error"] = v.status_last_fetch_error;
  j["provider_status_json"] = v.provider_status_json;
  j["extender_provide_status_json"] = v.extender_provide_status_json;
  j["extender_throughput_points_json"] = v.extender_throughput_points_json;
  j["provide_extender"] = v.provide_extender;
  j["provide_extender_writable"] = v.provide_extender_writable;
  j["measurements_enabled"] = v.measurements_enabled;
  j["measurements_enabled_writable"] = v.measurements_enabled_writable;
  j["provide_extender_port_mapping"] = v.provide_extender_port_mapping;
  j["provide_extender_port_mapping_writable"] = v.provide_extender_port_mapping_writable;
}
inline void from_json(const nlohmann::json& j, ProviderStatsReply& v) {
  detail::Get(j, "running", v.running);  // absent = nothing runs
  detail::Get(j, "has_provider_stats", v.has_provider_stats);
  detail::Get(j, "provider_throughput_points_json", v.provider_throughput_points_json);
  detail::Get(j, "provider_transport_distribution_json", v.provider_transport_distribution_json);
  detail::Get(j, "status_open", v.status_open);
  detail::Get(j, "status_loaded", v.status_loaded);
  detail::Get(j, "status_last_fetch_error", v.status_last_fetch_error);
  detail::Get(j, "provider_status_json", v.provider_status_json);
  detail::Get(j, "extender_provide_status_json", v.extender_provide_status_json);
  detail::Get(j, "extender_throughput_points_json", v.extender_throughput_points_json);
  detail::Get(j, "provide_extender", v.provide_extender);
  detail::Get(j, "provide_extender_writable", v.provide_extender_writable);  // absent = no writer
  detail::Get(j, "measurements_enabled", v.measurements_enabled);  // absent = on
  detail::Get(j, "measurements_enabled_writable",
              v.measurements_enabled_writable);  // absent = no writer
  detail::Get(j, "provide_extender_port_mapping",
              v.provide_extender_port_mapping);  // absent = off
  detail::Get(j, "provide_extender_port_mapping_writable",
              v.provide_extender_port_mapping_writable);  // absent = no writer
}

// ---- upload_logs -----------------------------------------------------------
// "Send feedback with logs" with the tunnel down (support inbox 2090). The logs
// support reads are this daemon's: the sdk's UploadLogs zips the glog files of
// the process it runs in, and the GUI's DeviceRemote reaches the daemon's
// DeviceLocal only while a tunnel session runs. So a report sent while
// disconnected, held by the kill switch or failing to connect — when support
// needs the logs most — carried none. This verb asks the daemon to upload its
// own logs for a feedback the server has accepted, connected or not.
//
// The daemon calls the sdk's UploadLogsWithFiles on the device that runs, or on
// a standalone device built from this request's credentials when none does
// (LogUpload.hpp carries the lifecycle). Everything about the upload itself is
// the sdk's: the zip of this process's glog files (flushed first) and of the
// GUI's (gui_log_files, below), POST /log/{feedback_id}/upload on the space's
// API with the device's client credentials, the server's 100 MB cap and its
// rate limit of one upload per network per 5 minutes. Nothing goes anywhere it
// did not go before.
//
// The reply comes once the upload is admitted, running or queued behind a
// tunnel start in progress: the zip and the post run on a thread of their own
// (logupload::Flight), never on the main loop, which a zip of up to the
// upload's cap would hold for as long as it reads the disk. One upload at a
// time: a request while one is in flight is answered kCodeLogUploadBusy. The
// outcome reaches the GUI through status: log_upload_id names the upload the
// reply's upload_id named, and log_upload_state says how it ended
// (logupload::CompletionFor).
//
// A new verb, additive within protocol v1: a daemon that predates it answers
// kErrorUnknownVerb, and the GUI falls back to what it did before — the
// DeviceRemote's UploadLogs while a tunnel session is bound, nothing otherwise.
//
// One set of logs: the server keeps one file per feedback and admits one upload
// per network per 5 minutes, so the GUI's own glog files ride in this daemon's
// zip, under gui/ (gui_log_files). The GUI opens them and passes the
// descriptors with this frame (SCM_RIGHTS, FdPassing.hpp): this daemon never
// opens a path a client names, and may not read a user's home at all
// (PassedLogFiles.hpp).
struct UploadLogsRequest {
  // The server-issued feedback id the logs attach to.
  std::string feedback_id;
  // The same four as StartProviderRequest, with the same meaning. The daemon
  // builds a device from them only when none runs.
  std::string by_jwt;
  std::string instance_id;
  std::string app_version;
  std::string network_space_json;
  // The glog names of the GUI's own log files, in the order their descriptors
  // ride with this frame. Additive: a daemon that predates it ignores the
  // names, and the kernel closes descriptors that are never received.
  std::vector<std::string> gui_log_files;
};
inline void to_json(nlohmann::json& j, const UploadLogsRequest& v) {
  j["feedback_id"] = v.feedback_id;
  j["by_jwt"] = v.by_jwt;
  j["instance_id"] = v.instance_id;
  j["app_version"] = v.app_version;
  if (!v.network_space_json.empty()) j["network_space_json"] = v.network_space_json;
  if (!v.gui_log_files.empty()) j["gui_log_files"] = v.gui_log_files;
}
inline void from_json(const nlohmann::json& j, UploadLogsRequest& v) {
  detail::Get(j, "feedback_id", v.feedback_id);
  detail::Get(j, "by_jwt", v.by_jwt);
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "app_version", v.app_version);
  detail::Get(j, "network_space_json", v.network_space_json);
  detail::Get(j, "gui_log_files", v.gui_log_files);
}

// The device that carries it (logupload::ToString: "tunnel", "provider",
// "standalone", or "queued" behind a tunnel start in progress), and the id
// status names the upload by (log_upload_id). Additive: absent parses 0.
struct UploadLogsReply {
  std::string carrier;
  int64_t upload_id = 0;
};
inline void to_json(nlohmann::json& j, const UploadLogsReply& v) {
  j["carrier"] = v.carrier;
  j["upload_id"] = v.upload_id;
}
inline void from_json(const nlohmann::json& j, UploadLogsReply& v) {
  detail::Get(j, "carrier", v.carrier);
  detail::Get(j, "upload_id", v.upload_id);
}

// The daemon could not start the upload: the standalone device could not be
// built. The message carries the SDK's error. (A zip or a post that fails is
// an outcome, in status, not a refusal.)
inline constexpr const char* kCodeLogUploadFailed = "log_upload_failed";

// An upload is in flight already. The GUI does not fall back to its own path
// for it: the server would refuse a second upload anyway.
inline constexpr const char* kCodeLogUploadBusy = "log_upload_busy";

// The server's feedback ids are uuids, and this one becomes a path segment of
// the API url root's device posts to (/log/<feedback_id>/upload). Anything
// else — a '/', a "..", a query — is refused before it reaches the SDK.
inline bool LooksLikeFeedbackId(const std::string& id) {
  if (id.size() != 36) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

// Both halves apply it, the GUI before a frame is sent and the daemon before
// anything is built. The credentials are required although a running device
// does not need them: the daemon cannot know in advance which case it is in.
inline std::optional<StartTunnelRejection> ValidateUploadLogsRequest(
    const UploadLogsRequest& req) {
  if (!LooksLikeFeedbackId(req.feedback_id)) {
    return StartTunnelRejection{"feedback_id must be the server's feedback id", nullptr};
  }
  if (req.by_jwt.empty()) return StartTunnelRejection{"by_jwt is required", nullptr};
  if (req.instance_id.empty()) return StartTunnelRejection{"instance_id is required", nullptr};
  return std::nullopt;
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

// ---- start_tunnel names the LIVE generation, with the wrong material -------
// UPSTREAM'S CODE, restored, with a NARROWER emission than upstream's — and
// the narrowing is the interesting part. Upstream refuses every start_tunnel
// while a tunnel runs ("attach with its live identity"); this fork adopts
// instead (ControlServer::HandleStartTunnel -> TunnelHost::CanAdopt), so a
// start that matches the live session byte for byte succeeds and one that
// names a different session rebuilds. That leaves exactly one case where
// refusing beats both:
//
//   a tunnel is UP, the request carries the SAME instance_id and the SAME
//   rpc_session_id, and the material differs — the caller claims to BE the
//   running generation while presenting credentials that generation was not
//   built with.
//
// Rebuilding there would republish ONE name over TWO different key pairs, and
// any holder of the older pair — a second GUI, a stale keyring record — would
// afterwards attach BY NAME and be handed a port it cannot dial: the silent
// "connects and never populates" failure this header exists to make
// impossible. So the daemon keeps the working tunnel and says why.
//
// THE CLIENT'S RESPONSE IS TO MINT A NEW GENERATION — a new rpc_session_id AND
// new material, together — and start again; that terminates, because the new
// name cannot collide with the live one. Retrying the identical request cannot
// help, and this is NOT an ownership refusal: kCodeTunnelOwnedByOtherClient and
// kCodeAuthNotTunnelOwner are the codes for that, and they are answered before
// this one is reached.
inline constexpr const char* kCodeTunnelAlreadyRunning = "tunnel_already_running";

// ---- attach_tunnel refusals ------------------------------------------------
// UPSTREAM'S CODE, restored verbatim: the attach names a session that is not
// the one running — a different instance_id, a different RPC session
// generation, or no live tunnel at all. The honest client response is to fall
// back to start_tunnel and build a session; the stored record that produced
// the refusal names nothing that exists and may be discarded.
//
// It is this verb's ONLY refusal beyond authorization and the two empty-field
// errors, and that is now the whole story. There used to be a second code,
// rpc_session_not_persisted, for a daemon that never published a session
// generation and therefore had nothing to compare an attach against; it is
// DELETED rather than kept for compatibility, because the state it described
// cannot occur any more (start_tunnel requires rpc_session_id, and the daemon
// latches it before it publishes Up) and a code no reachable path can emit is
// a documented lie about what the daemon does.
inline constexpr const char* kCodeRpcSessionMismatch = "rpc_session_mismatch";

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
// The tunnel was torn down because it was AMPLIFYING: the tun's transmit
// counter ran away while nothing came back, which is what an egress-exclusion
// failure looks like from the outside (the daemon's own packets get captured
// into the tunnel, re-sent, re-captured). Measured once on a real machine at
// 1.34 Gbps out / 0 in, 3.38 Tb sent before a human noticed and killed it.
inline constexpr const char* kCodeTunnelStorm = "tunnel_storm";
// The dead-tunnel failsafe ended a session that was up but carried nothing
// (stop_reason says which rule, IsFailsafeStop).
inline constexpr const char* kCodeTunnelDead = "tunnel_dead";
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
// A Connect found no route off the tunnel in either family: this machine is
// not on a network, so nothing could carry the session. NOT the R4 refusal --
// the egress witness proved the exclusion and simply found nowhere to go.
// Bring-up only: a session that loses every route later (a network switch in
// progress) stays up and waits for the next network.
inline constexpr const char* kCodeNoNetwork = "no_network";
// The nftables floor the kill switch needs could not be installed. Only fatal
// when the client asked for the kill switch: a protection that is not in force
// must never be reported as on.
inline constexpr const char* kCodeKillSwitchFailed = "kill_switch_failed";
// DNS could not be pointed at the tunnel AND the kill switch was requested, so
// coming up would leave the machine unable to resolve at all.
inline constexpr const char* kCodeDnsApplyFailed = "dns_apply_failed";
// Also in this table, declared earlier because ValidateStartTunnelRequest
// returns them: kCodeRpcPinRequired, kCodeRpcPinInvalid, kCodeRpcListenFailed
// (see the device-RPC mTLS pinning section near the top of this header). And
// the start_provider refusals, declared with that verb: kCodeProvideModeOff,
// kCodeTunnelSessionActive, kCodeKillSwitchArmed, kCodeProviderStartFailed;
// and upload_logs' kCodeLogUploadFailed and kCodeLogUploadBusy.

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

// What every daemon answers for a verb it does not know, so a client can tell a
// daemon that predates a verb from a refusal (a new verb is additive within
// v1; see the version comment at the top).
inline constexpr const char* kErrorUnknownVerb = "unknown verb";

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

// The one way a frame becomes bytes on the socket, for both binaries.
//
// A plain dump() throws type_error.316 on a string that is not valid UTF-8, and
// the frames carry strings neither binary wrote: SDK errors, resolvectl and nft
// output, SDK log lines. The daemon sends from glib callbacks, so that throw
// was std::terminate in a root process holding the capture routes. With
// error_handler_t::replace a bad byte becomes U+FFFD instead. The strings at
// risk are human-readable diagnostics; the peer acts on codes, flags and
// states, which the daemon writes itself. DecodeFrame stays strict.
//
// dump() can still throw on allocation, so senders keep their try.
inline std::string DumpForWire(const nlohmann::json& j) {
  return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                nlohmann::json::error_handler_t::replace);
}

// One frame per line. nlohmann's dump() never emits raw newlines (they are
// escaped inside strings), so '\n' is an unambiguous frame terminator.
inline std::string EncodeFrame(const nlohmann::json& j) { return DumpForWire(j) + "\n"; }

// Parses one line (with or without its trailing newline). Returns nullopt for
// anything that is not a single JSON object — the caller treats that as a
// protocol error and drops the connection.
inline std::optional<nlohmann::json> DecodeFrame(const std::string& line) {
  nlohmann::json j = nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;
  return j;
}

}  // namespace urnw::ctl
