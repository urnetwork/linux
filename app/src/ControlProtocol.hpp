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
inline constexpr int kControlProtocolVersion = 2;
// The daemon refuses a client hello carrying a protocol_version below this.
inline constexpr int kMinSupportedClientProtocol = 2;
// The GUI refuses a daemon hello reply carrying a protocol_version below this.
inline constexpr int kMinSupportedDaemonProtocol = 2;

// Socket location (MIGRATION.md table): dir 0750 root:urnetwork, sock 0660.
// Never abstract — permissions are meaningless in the abstract namespace.
inline constexpr const char* kControlSocketDir = "/run/urnetwork";
inline constexpr const char* kControlSocketPath = "/run/urnetwork/control.sock";
// The unix group whose members (plus uid 0) may use the control socket.
inline constexpr const char* kControlGroupName = "urnetwork";

// The device RPC the GUI's DeviceRemote dials once start_tunnel succeeds.
// The address matches the SDK default, but both peers explicitly replace that
// default transport with fresh per-tunnel mTLS material. Deliberately not a
// unix socket yet; the control socket authorizes lifecycle requests while mTLS
// independently authenticates RPC clients. Surfaced in status/start replies.
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
  AttachTunnel,
  StopTunnel,
  SetProvide,
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
    case Verb::AttachTunnel: return "attach_tunnel";
    case Verb::StopTunnel: return "stop_tunnel";
    case Verb::SetProvide: return "set_provide";
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
  if (s == "attach_tunnel") return Verb::AttachTunnel;
  if (s == "stop_tunnel") return Verb::StopTunnel;
  if (s == "set_provide") return Verb::SetProvide;
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
  // Additive within the protocol version: both halves ship from one pipeline
  // and the hello sdk_version exact-match already refuses mismatched pairs.
  std::string network_space_json;
  // Per-tunnel RPC identity. The daemon receives only the server private key
  // and the public client certificate; the GUI retains the client private key.
  std::string rpc_session_id;
  std::string rpc_server_pem;
  std::string rpc_client_cert_pem;
};
inline void to_json(nlohmann::json& j, const StartTunnelRequest& v) {
  j["by_jwt"] = v.by_jwt;
  j["instance_id"] = v.instance_id;
  j["app_version"] = v.app_version;
  if (!v.network_space_json.empty()) j["network_space_json"] = v.network_space_json;
  j["rpc_session_id"] = v.rpc_session_id;
  j["rpc_server_pem"] = v.rpc_server_pem;
  j["rpc_client_cert_pem"] = v.rpc_client_cert_pem;
}
inline void from_json(const nlohmann::json& j, StartTunnelRequest& v) {
  detail::Get(j, "by_jwt", v.by_jwt);
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "app_version", v.app_version);
  detail::Get(j, "network_space_json", v.network_space_json);
  detail::Get(j, "rpc_session_id", v.rpc_session_id);
  detail::Get(j, "rpc_server_pem", v.rpc_server_pem);
  detail::Get(j, "rpc_client_cert_pem", v.rpc_client_cert_pem);
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
  if (req.rpc_session_id.empty()) return "rpc_session_id is required";
  if (req.rpc_server_pem.empty()) return "rpc_server_pem is required";
  if (req.rpc_client_cert_pem.empty()) return "rpc_client_cert_pem is required";
  return std::nullopt;
}

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
inline std::optional<std::string> ValidateAttachTunnelRequest(
    const AttachTunnelRequest& req) {
  if (req.instance_id.empty()) return "instance_id is required";
  if (req.rpc_session_id.empty()) return "rpc_session_id is required";
  return std::nullopt;
}

struct StartTunnelReply {
  int rpc_port = 0;
  std::string instance_id;
  std::string rpc_session_id;
};
inline void to_json(nlohmann::json& j, const StartTunnelReply& v) {
  j["rpc_port"] = v.rpc_port;
  j["instance_id"] = v.instance_id;
  j["rpc_session_id"] = v.rpc_session_id;
}
inline void from_json(const nlohmann::json& j, StartTunnelReply& v) {
  detail::Get(j, "rpc_port", v.rpc_port);
  detail::Get(j, "instance_id", v.instance_id);
  detail::Get(j, "rpc_session_id", v.rpc_session_id);
}

struct SetProvideRequest {
  std::string mode;  // "never"|"always"|"network"|"auto"|"manual"
};
inline void to_json(nlohmann::json& j, const SetProvideRequest& v) { j["mode"] = v.mode; }
inline void from_json(const nlohmann::json& j, SetProvideRequest& v) {
  detail::Get(j, "mode", v.mode);
}

struct StatusReply {
  TunnelState tunnel_state = TunnelState::Stopped;
  int rpc_port = 0;          // 0 while the tunnel is down
  std::string client_id;     // the DeviceLocal's client id ("" while down)
  std::string instance_id;   // exact live DeviceLocal identity ("" while down)
  std::string rpc_session_id; // opaque per-tunnel RPC credential generation
  std::string error;         // last start error ("" when none)
};
inline void to_json(nlohmann::json& j, const StatusReply& v) {
  j["tunnel_state"] = ToString(v.tunnel_state);
  j["rpc_port"] = v.rpc_port;
  j["client_id"] = v.client_id;
  j["instance_id"] = v.instance_id;
  j["rpc_session_id"] = v.rpc_session_id;
  j["error"] = v.error;
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
inline constexpr const char* kCodeTunnelAlreadyRunning = "tunnel_already_running";
inline constexpr const char* kCodeRpcSessionMismatch = "rpc_session_mismatch";

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
