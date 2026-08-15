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

// Socket location (MIGRATION.md table): dir 0750 root:urnetwork, sock 0660.
// Never abstract — permissions are meaningless in the abstract namespace.
inline constexpr const char* kControlSocketDir = "/run/urnetwork";
inline constexpr const char* kControlSocketPath = "/run/urnetwork/control.sock";
// The unix group whose members (plus uid 0) may use the control socket.
inline constexpr const char* kControlGroupName = "urnetwork";

// The device RPC the GUI's DeviceRemote dials once start_tunnel succeeds.
// Loopback TCP + mTLS, the SDK's built-in default (sdk/device_rpc.go:109,
// deviceRpcDefaultAddress = "127.0.0.1:12025") — deliberately NOT a unix
// socket yet; the control socket is the authorization boundary (MIGRATION.md
// "Decision"). Surfaced in status/start_tunnel replies as rpc_port.
inline constexpr int kDeviceRpcPort = 12025;

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
// The socket-facing code resolves SO_PEERCRED on every accept(), BEFORE
// parsing any frame, then resolves the peer's group list via getpwuid +
// getgrouplist and the control group's gid via getgrnam — and hands the plain
// numbers here. Policy (MIGRATION.md): allow uid 0 and members of the
// `urnetwork` group. NEVER authorize on pid (CVE-2019-6133), never attempt
// peer-binary attestation (root cannot read an AppImage's FUSE mount —
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
};
inline void to_json(nlohmann::json& j, const HelloReply& v) {
  j["protocol_version"] = v.protocol_version;
  j["daemon_version"] = v.daemon_version;
  j["sdk_version"] = v.sdk_version;
}
inline void from_json(const nlohmann::json& j, HelloReply& v) {
  v.protocol_version = 0;
  detail::Get(j, "protocol_version", v.protocol_version);
  detail::Get(j, "daemon_version", v.daemon_version);
  detail::Get(j, "sdk_version", v.sdk_version);
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
  // The SDK's loopback device RPC pins with EXACT raw-certificate equality
  // (tls.RequireAnyClientCert), and the two halves keep SEPARATE SDK storage
  // roots (~/.local/share/urnetwork vs /var/lib/urnetwork/sdk), so no
  // "defaults" path can produce a matching pinned pair. The GUI generates one
  // urnet::DeviceRpcKeyMaterial and splits it: server_pem + client_cert_pem
  // to the daemon (DeviceLocal::setRpcServer), client_pem + server_cert_pem
  // kept for its own DeviceRemote::setRpcServer. All three empty = the SDK's
  // built-in default listener, which is what shipped before this field
  // existed; all three must be present together or none.
  std::string rpc_server_pem;
  std::string rpc_client_cert_pem;
  std::string rpc_listen_hostport;  // e.g. "127.0.0.1:12025"
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

// Validation the daemon applies before constructing anything. Pure so the
// contract is unit-testable: `instance_id` is the DEVICE PAIRING KEY — the
// daemon constructs its DeviceLocal with exactly this value and the GUI hands
// the same value to newDeviceRemoteWithDefaults; the device rpc rejects a
// sync whose InstanceId differs from the DeviceLocal's
// (sdk/device_rpc.go:6545), which surfaces as a remote that connects but
// never populates. An empty id must therefore fail loudly here, never fall
// back to a daemon-generated one.
inline std::optional<std::string> ValidateStartTunnelRequest(const StartTunnelRequest& req) {
  if (req.by_jwt.empty()) return "by_jwt is required";
  if (req.instance_id.empty()) return "instance_id is required";
  // The rpc pinning triple is all-or-nothing: a half-supplied pair would make
  // the daemon listen with a pinned server while the GUI dials with the SDK
  // default (or the reverse), which presents as a DeviceRemote that connects
  // and never populates — the exact silent failure the exact-match hello
  // exists to prevent.
  const int rpcParts = (req.rpc_server_pem.empty() ? 0 : 1) +
                       (req.rpc_client_cert_pem.empty() ? 0 : 1) +
                       (req.rpc_listen_hostport.empty() ? 0 : 1);
  if (rpcParts != 0 && rpcParts != 3) {
    return "rpc_server_pem, rpc_client_cert_pem and rpc_listen_hostport must be sent together";
  }
  return std::nullopt;
}

struct StartTunnelReply {
  int rpc_port = 0;
  // "up" for a completed synchronous start; "starting" when async was asked
  // for and the bring-up is still running. A client that ignores this field
  // sees exactly the old contract.
  TunnelState tunnel_state = TunnelState::Up;
};
inline void to_json(nlohmann::json& j, const StartTunnelReply& v) {
  j["rpc_port"] = v.rpc_port;
  j["tunnel_state"] = ToString(v.tunnel_state);
}
inline void from_json(const nlohmann::json& j, StartTunnelReply& v) {
  detail::Get(j, "rpc_port", v.rpc_port);
  std::string state;
  detail::Get(j, "tunnel_state", state);
  // absent = a daemon predating the field, which only ever replied ok on a
  // completed start
  v.tunnel_state = state.empty() ? TunnelState::Up : TunnelStateFromString(state);
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
