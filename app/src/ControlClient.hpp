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

  // ---- verbs (each ensures a session first) --------------------------------
  // start_tunnel: on success fills `result` (the daemon's rpc_port plus the
  // instance/session identity the GUI verifies before dialing). On failure
  // `error` carries the daemon's message (e.g. another client owns the
  // tunnel). The request carries the GUI's active space
  // (network_space_json), so the daemon's DeviceLocal lives in the same
  // network ("" = the compiled-in default).
  bool StartTunnel(const ctl::StartTunnelRequest& request,
                   ctl::StartTunnelReply* result, std::string* error);
  // Claims an already-running tunnel without restarting it. The daemon only
  // transfers ownership when both live identifiers match exactly.
  bool AttachTunnel(const ctl::AttachTunnelRequest& request,
                    ctl::StartTunnelReply* result, std::string* error);
  bool StopTunnel(std::string* error = nullptr);
  bool SetProvide(const std::string& mode, std::string* error = nullptr);
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
  // EnsureSession + round-trip with one reconnect retry on a dead socket.
  std::optional<nlohmann::json> CallLocked(ctl::Verb verb, nlohmann::json payload,
                                           std::string* error);
  bool SendAllLocked(const std::string& data);
  // Reads one full line (frame) into `line`, false on EOF/error/timeout.
  bool ReadLineLocked(std::string& line);

  std::mutex mutex_;
  int fd_ = -1;
  bool helloOk_ = false;
  int64_t nextId_ = 1;
  std::string recvBuffer_;
  DaemonSessionState lastState_ = DaemonSessionState::Unreachable;
  std::string daemonVersion_;
  std::string localSdkVersion_;
};

}  // namespace urnw
