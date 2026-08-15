// The GUI's half of the device-RPC mTLS session: the material it generated,
// the loopback host:port it chose, and the instance id the session was STARTED
// with — serialized so a GUI restart can re-present the SAME triple to
// urnetworkd and be ADOPTED (daemon TunnelHost::CanAdopt compares
// rpc_server_pem / rpc_client_cert_pem / rpc_listen_hostport byte for byte)
// instead of tearing down a working tunnel and rebuilding it on every launch.
//
// Pure: nlohmann + ControlProtocol.hpp only — no glib, no GTK, no SDK — so it
// is unit-testable exactly the way the protocol header is. The SHAPE gates are
// deliberately built out of urnw::ctl rather than reimplemented: the daemon
// validates arriving material with those same functions, so a blob this parser
// accepts is a request that validator accepts. The port gate below is STRICTLY
// TIGHTER than the daemon's (which must also admit other clients' choices);
// tighter preserves that property, looser would break it.
//
// THE INSTANCE-ID TRAP (a real Windows post-mortem — urnetwork-windows
// app/src/Common/RpcSessionBlob.h): the SDK rotates the local instance id
// whenever the by-client JWT STRING changes, and a JWT refresh re-signs the
// same client. So localState_->getInstanceId() at the next launch can differ
// from the id the daemon's DeviceLocal was born with, and DeviceLocalRpc.Sync
// refuses forever a nonzero instance id that is not its own — a DeviceRemote
// that connects and never populates, with no synchronous signal anywhere. The
// blob therefore stores the id the session was STARTED with, and the reattach
// path uses THAT, never the one on disk now.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ControlProtocol.hpp"

namespace urnw::rpcsession {

// Canonical 36-char dashed uuid ONLY, and the NIL uuid is REFUSED: a zero id
// is the value the SDK uses to SKIP the pairing check, so reattaching with one
// would pair with anything (or nothing) instead of failing loudly. A
// non-pairable id is not an error — it just means "do not remember this
// session", which costs a tunnel rebuild and never a wrong pairing.
inline constexpr bool IsPairableInstanceId(std::string_view s) noexcept {
  if (s.size() != 36) return false;
  bool anyNonZero = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
      continue;
    }
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
    if (c != '0') anyNonZero = true;
  }
  return anyNonZero;
}

// DELIBERATE DIVERGENCE FROM WINDOWS: the Windows blob stores only
// client_pem + server_cert_pem + host_port + instance_id, because Windows
// reattaches through a separate hello/state query and never re-sends the
// server half. Linux has no such verb — adoption is decided INSIDE
// start_tunnel by comparing the request against the live one — so the Linux
// blob must also carry the server half, or every GUI restart fails CanAdopt
// and the daemon rebuilds a tunnel that was working.
//
// Exposure delta: the file is written 0600 beside the SDK LocalState that
// already holds by_jwt, the account bearer token. Anything that can read the
// blob can already read something strictly more valuable. The only honest
// alternative is to accept a tunnel rebuild on every GUI restart — never to
// weaken CanAdopt to ignore the PEMs, which would let any control-socket peer
// adopt a session it cannot drive.
struct Blob {
  std::string server_pem;       // -> daemon: the key+cert its listener presents
  std::string client_cert_pem;  // -> daemon: the client cert it pins
  std::string client_pem;       // kept here: the key+cert this GUI presents
  std::string server_cert_pem;  // kept here: the server cert this GUI pins
  std::string host_port;        // "127.0.0.1:<port>", drawn by this GUI
  std::string instance_id;      // the id the session was STARTED with
};

// The port half of the shape gate, TIGHTER than ctl::IsLoopbackRpcHostPort —
// which accepts any 1024..65535 because it also guards values arriving off the
// control socket from other clients. A blob is a value THIS app wrote, and
// this app draws its listener port from [kRpcPortMin, kRpcPortMax]
// (SdkHost.cpp RandomLoopbackRpcHostPort), a range that deliberately excludes
// ctl::kDeviceRpcPort.
//
// THE ONE THAT MATTERS IS 12025. The GUI reserves 127.0.0.1:12025 for the
// whole process (SdkHost::HoldDeviceRpcDefaultPortLocked) so the SDK's
// hard-coded unpinned first dial lands on a dead address. A blob naming that
// same port would therefore ask the daemon to listen where this process is
// holding the address: DeviceLocal::setRpcServer binds LAZILY, so nothing
// throws, the daemon reports rpc_pinned, and the only symptom is a DeviceRemote
// that never connects until the bind watchdog kills the session 8 s later.
// Refusing the blob costs one tunnel rebuild instead.
inline bool IsUsableRpcHostPort(const std::string& host_port) {
  const int port = ctl::RpcPortFromHostPort(host_port);
  return ctl::kRpcPortMin <= port && port <= ctl::kRpcPortMax;
}

// True when every field is present and usable. The same predicate Parse
// applies, exposed so the FRESH path can refuse to persist a blob it would
// refuse to read back.
inline bool IsUsable(const Blob& b) {
  return ctl::LooksLikePem(b.server_pem) && ctl::LooksLikePem(b.client_cert_pem) &&
         ctl::LooksLikePem(b.client_pem) && ctl::LooksLikePem(b.server_cert_pem) &&
         IsUsableRpcHostPort(b.host_port) && IsPairableInstanceId(b.instance_id);
}

inline std::string Serialize(const Blob& b) {
  nlohmann::json j;
  j["server_pem"] = b.server_pem;
  j["client_cert_pem"] = b.client_cert_pem;
  j["client_pem"] = b.client_pem;
  j["server_cert_pem"] = b.server_cert_pem;
  j["host_port"] = b.host_port;
  j["instance_id"] = b.instance_id;
  return j.dump();
}

// Non-throwing. nullopt unless ALL six are present and IsUsable — a
// half-readable blob must be treated as ABSENT (which costs a tunnel rebuild)
// rather than half-used (which costs a DeviceRemote that connects and never
// populates: nothing throws on either side, and the only evidence is
// getRemoteConnected() never turning true).
inline std::optional<Blob> Parse(std::string_view text) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return std::nullopt;
  Blob b;
  const auto read = [&j](const char* key, std::string& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
  };
  if (!read("server_pem", b.server_pem)) return std::nullopt;
  if (!read("client_cert_pem", b.client_cert_pem)) return std::nullopt;
  if (!read("client_pem", b.client_pem)) return std::nullopt;
  if (!read("server_cert_pem", b.server_cert_pem)) return std::nullopt;
  if (!read("host_port", b.host_port)) return std::nullopt;
  if (!read("instance_id", b.instance_id)) return std::nullopt;
  if (!IsUsable(b)) return std::nullopt;
  return b;
}

}  // namespace urnw::rpcsession
