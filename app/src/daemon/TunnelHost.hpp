// The daemon's tunnel core: DeviceLocal(enable_rpc=true) + /dev/net/tun +
// urnet::newIoLoop, lifted from the GUI SdkHost::StartTunnel at the daemon
// split (linux/MIGRATION.md). The Linux analogue of the Windows
// Service/TunnelController: the GUI's DeviceRemote reaches this device over
// the SDK's loopback mTLS RPC (127.0.0.1:12025) once Start() succeeds.
//
// Owns the persisted device identity (Ed25519 client key seed + provide TLS
// cert/key) under the daemon state dir (/var/lib/urnetwork): it is
// device-scoped, not session-scoped, so peers keep verifying this device
// across daemon restarts and GUI re-logins. Only an explicit identity wipe
// rotates it.
//
// Threading: every method is called on the daemon main loop (the control
// server serializes request handling), so there is no lock. SDK callbacks
// never re-enter this class.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <urnetwork_sdk.hpp>

#include "ControlProtocol.hpp"
#include "Tunnel.hpp"

namespace urnw {

class TunnelHost {
 public:
  // storageRoot: the daemon state dir (normally /var/lib/urnetwork). Key
  // material lives directly in it; the SDK network-space storage in
  // storageRoot/sdk.
  explicit TunnelHost(std::string storageRoot);
  ~TunnelHost();

  TunnelHost(const TunnelHost&) = delete;
  TunnelHost& operator=(const TunnelHost&) = delete;

  // Builds the DeviceLocal, installs the caller's per-session mTLS RPC
  // listener, opens the tun, and wires the IoLoop.
  // Idempotent restart: an existing session is torn down first (the control
  // server gates WHO may call this; see its ownership rules).
  ctl::StatusReply Start(const ctl::StartTunnelRequest& config);
  void Stop();

  // Applies the provide control mode to the live device, or stashes it for
  // the next Start when the tunnel is down.
  bool SetProvideMode(const std::string& mode);

  ctl::StatusReply Status() const;
  bool TunnelUp() const { return state_ == ctl::TunnelState::Up; }

 private:
  std::optional<urnet::DeviceLocalKeyMaterial> LoadKeyMaterial() const;
  void PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) const;
  void StopInternal();

  std::string storageRoot_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::DeviceLocal> device_;
  std::optional<urnet::IoLoop> ioLoop_;
  std::unique_ptr<Tunnel> tunnel_;

  ctl::TunnelState state_ = ctl::TunnelState::Stopped;
  std::string error_;              // last start error ("" when none)
  std::string instanceId_;         // exact live DeviceLocal pairing identity
  std::string rpcSessionId_;       // opaque generation for safe GUI adoption
  std::string pendingProvideMode_; // set_provide received while down
};

}  // namespace urnw
