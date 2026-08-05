// The urnetworkd control-socket server (ControlProtocol.hpp): AF_UNIX/
// SOCK_STREAM at /run/urnetwork/control.sock, newline-delimited JSON, driven
// by the daemon's GMainLoop (plain glib — the daemon has no GTK).
//
// Authorization (APPIMAGE.md §11c, enforced here exactly):
//   * SO_PEERCRED on every accept(), BEFORE parsing any frame; the ucred is
//     cached on the connection.
//   * Allow uid 0 and members of the `urnetwork` group (getgrnam +
//     getpwuid/getgrouplist). The decision itself is the pure
//     ctl::AuthorizeControlPeer, unit-tested without a socket.
//   * Never authorize on pid (CVE-2019-6133); never attempt peer-binary
//     attestation (root cannot read an AppImage's FUSE mount).
//
// Version skew (§11b): `hello` is mandatory and carries protocol_version in
// both directions. A client below kMinSupportedClientProtocol is rejected
// with kCodeClientProtocolTooOld; every other verb before a successful hello
// gets kCodeHelloRequired — which is what makes the negotiation actually
// enforced rather than declared (the Windows twin declares and never checks).
//
// Concurrency: multiple clients connect concurrently; requests are handled
// serially on the main loop. The tunnel has ONE owner — the first
// authenticated client whose start_tunnel succeeds; a start/stop/set_provide
// from a different live client gets kCodeTunnelOwnedByOtherClient (root may
// always stop). When the owner disconnects the tunnel keeps running (the GUI
// hides to the tray and may reconnect) and ownership is up for adoption by
// the next start_tunnel.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <glib.h>

#include "ControlProtocol.hpp"
#include "LocationOverride.hpp"
#include "daemon/TunnelHost.hpp"

namespace urnw {

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

  // Creates the socket dir (0750 root:urnetwork), binds, chmods the socket
  // 0660 root:urnetwork and starts accepting on the default main context.
  bool Start();
  void Stop();

  // Daemon-version string reported in the hello reply.
  void SetDaemonVersion(std::string version) { daemonVersion_ = std::move(version); }
  // This binary's urnet::version(), exact-match enforced against the client's
  // hello (ctl::SdkVersionsAgree): the gob device RPC has no version field of
  // its own, so the control channel is the only place a drifted SDK pair can
  // be caught loudly instead of failing silently.
  void SetSdkVersion(std::string version) { sdkVersion_ = std::move(version); }

 private:
  struct Connection {
    int fd = -1;
    guint watchId = 0;
    // cached peer credentials (resolved from SO_PEERCRED at accept)
    int64_t uid = -1;
    int64_t gid = -1;
    bool helloOk = false;  // a successful, version-checked hello happened
    std::string inBuf;
  };

  static gboolean OnAcceptReady(gint fd, GIOCondition condition, gpointer data);
  static gboolean OnConnectionReadable(gint fd, GIOCondition condition, gpointer data);

  void AcceptOne();
  // false => the connection was closed and freed
  bool ReadAndDispatch(Connection* conn);
  void CloseConnection(Connection* conn);
  bool SendFrame(Connection* conn, const nlohmann::json& frame);

  nlohmann::json Dispatch(Connection* conn, const nlohmann::json& request);
  nlohmann::json HandleHello(Connection* conn, int64_t id, const nlohmann::json& request);
  nlohmann::json HandleStartTunnel(Connection* conn, int64_t id, const nlohmann::json& request);

  // owner gate for tunnel-lifecycle verbs; fills *denied with the error reply
  bool CheckTunnelOwner(Connection* conn, int64_t id, nlohmann::json* denied);

  TunnelHost& tunnel_;
  GeoClueWriter& geoWriter_;
  std::string daemonVersion_;
  std::string sdkVersion_;

  int listenFd_ = -1;
  guint listenWatchId_ = 0;
  std::string boundPath_;
  std::map<int, std::unique_ptr<Connection>> connections_;  // by fd

  // the connection whose start_tunnel succeeded last; nullptr when it went
  // away (tunnel keeps running, ownership adoptable)
  Connection* tunnelOwner_ = nullptr;
  // the connection that last wrote the geo override; its disconnect clears
  // the override — never keep reporting a city nobody is tracking
  Connection* overrideWriter_ = nullptr;
};

}  // namespace urnw
