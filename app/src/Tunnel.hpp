// Opens and configures /dev/net/tun in-process; the fd is handed to the SDK's
// IoLoop. Routes + DNS are applied like wg-quick (via `ip` and `resolvectl`).
// Since the daemon split (linux/MIGRATION.md) this runs inside urnetworkd,
// which is root under systemd — the GUI never touches it. Non-persistent tun:
// the interface (and its routes/addresses) disappears when the fd is closed.
// C++ port of the (retired) Go tunnel.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>

#include "TunnelPolicy.hpp"

namespace urnw {

// RAII tun device. Closing removes the (non-persistent) interface + its routes.
class Tunnel {
 public:
  static std::unique_ptr<Tunnel> Open(const TunnelConfig& cfg);
  ~Tunnel();

  Tunnel(const Tunnel&) = delete;
  Tunnel& operator=(const Tunnel&) = delete;

  int fd() const { return fd_; }
  const std::string& name() const { return name_; }

 private:
  Tunnel() = default;
  bool Configure(const TunnelConfig& cfg);

  int fd_ = -1;
  std::string name_;
};

}  // namespace urnw
