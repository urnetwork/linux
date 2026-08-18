// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "TunnelPolicy.hpp"

UR_TEST(tunnelPolicyAcceptsIpv4OnlyConfiguration) {
  urnw::TunnelConfig config;
  config.local_addr_v4 = "169.254.2.1";
  config.prefix_v4 = 24;
  config.dns_servers_v4 = {"65.49.70.65", "9.9.9.9"};

  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));
}

UR_TEST(tunnelPolicyRejectsIpv6AddressAndDnsTransport) {
  urnw::TunnelConfig config;
  config.local_addr_v4 = "fd00::1";
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));

  config.local_addr_v4 = "169.254.2.1";
  config.dns_servers_v4 = {"2001:4860:4860::8888"};
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
}

UR_TEST(tunnelPolicyHasNoIpv6ConfigurationSurface) {
  urnw::TunnelConfig config;

  // TunnelConfig intentionally exposes only explicitly named IPv4 fields.
  // This guards the defaults as the remote-provider contract evolves.
  UR_EXPECT_TRUE(config.local_addr_v4 == "169.254.2.1");
  UR_EXPECT_EQ(config.prefix_v4, 24);
  UR_EXPECT_TRUE(config.dns_servers_v4.empty());
}

UR_TEST(tunnelPolicyRejectsHostnamesAndInvalidIpv4Values) {
  urnw::TunnelConfig config;
  config.local_addr_v4 = "resolver.example";
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));

  config.local_addr_v4 = "192.0.2.999";
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));

  config.local_addr_v4 = "192.0.2.1";
  config.prefix_v4 = 33;
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
}
