// Pure tunnel-interface policy shared by the Linux daemon and its unit tests.
// Remote providers currently forward IPv4 only, so the platform tunnel must
// never advertise an IPv6 address, route, or DNS transport. Host-interface
// IPv6 remains outside the tunnel; this policy does not install a blackhole.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// Mirrors sdk.GetDefaultTunnelMtu/connect.DefaultMtu. One full encrypted
// tunnel packet plus the UR envelope fits one initial H3 QUIC DATAGRAM.
inline constexpr int kTunnelMtu = 1100;

struct TunnelConfig {
  std::string name = "urnet0";
  std::string local_addr_v4 = "169.254.2.1";
  int prefix_v4 = 24;
  int mtu = kTunnelMtu;
  std::vector<std::string> dns_servers_v4;
  // FORK ADDITION (not upstream). Refuse to install the capture routes unless
  // the daemon's own sockets are demonstrably steered around them. Only a dev
  // run (URNETWORK_ALLOW_UNPROTECTED_EGRESS=1) may clear it, and clearing it is
  // logged loudly, because the alternative is a tunnel that comes up and
  // carries nothing while the UI says Connected. It lives here rather than in
  // Tunnel.hpp because THIS header owns the one TunnelConfig: a second
  // definition of urnw::TunnelConfig in Tunnel.hpp is what kept
  // IsIpv4OnlyTunnelConfig from being callable at all (the two could never
  // appear in one translation unit), which is how the guard came to exist
  // without a caller. IsIpv4OnlyTunnelConfig deliberately ignores this field.
  bool require_egress_protection = true;
};

// Whether nftables may use its socket-cgroup expression for the daemon's
// egress exemption. Some otherwise-capable kernels (notably Docker Desktop's
// LinuxKit kernel) ship cgroup BPF but omit CONFIG_NFT_SOCKET. In that case a
// proven socket-creation mark is sufficient for a floorless tunnel, but it is
// not sufficient for a crash-safe kill-switch floor or for a DNS helper that
// lives in another cgroup.
enum class NftCgroupMode {
  CgroupAndMark,
  MarkOnly,
  Refuse,
};

// Pure policy boundary for the runtime kernel probe. Refusal is intentional:
// omitting an unavailable expression must never silently weaken the states
// whose safety depends on matching another cgroup or surviving daemon death.
constexpr NftCgroupMode SelectNftCgroupMode(bool socketMarkerProven,
                                            bool cgroupSocketMatchSupported,
                                            bool blockFloor,
                                            bool helperDnsRequired) {
  if (cgroupSocketMatchSupported) return NftCgroupMode::CgroupAndMark;
  if (!socketMarkerProven || blockFloor || helperDnsRequired) {
    return NftCgroupMode::Refuse;
  }
  return NftCgroupMode::MarkOnly;
}

constexpr bool IsIpv4Literal(std::string_view address) {
  if (address.empty()) return false;

  int octets = 0;
  int digits = 0;
  int value = 0;
  for (const char c : address) {
    if (c == '.') {
      if (digits == 0 || value > 255 || octets == 3) return false;
      ++octets;
      digits = 0;
      value = 0;
      continue;
    }
    if (c < '0' || c > '9' || digits == 3) return false;
    value = value * 10 + (c - '0');
    ++digits;
  }
  return octets == 3 && digits != 0 && value <= 255;
}

inline bool IsIpv4OnlyTunnelConfig(const TunnelConfig& config) {
  if (!IsIpv4Literal(config.local_addr_v4) || config.prefix_v4 < 1 ||
      config.prefix_v4 > 32) {
    return false;
  }
  for (const auto& server : config.dns_servers_v4) {
    if (!IsIpv4Literal(server)) return false;
  }
  return true;
}

}  // namespace urnw
