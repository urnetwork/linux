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

struct TunnelConfig {
  std::string name = "urnet0";
  std::string local_addr_v4 = "169.254.2.1";
  int prefix_v4 = 24;
  int mtu = 1440;
  std::vector<std::string> dns_servers_v4;
};

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
