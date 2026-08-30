// The IPv4-only tunnel policy, and THE FACT THAT SOMETHING CALLS IT.
//
// The predicate below was restored from upstream with no caller: the file was
// present and the fix was not. That is the same shape as NetFilter::Verify,
// Tunnel::VerifyDnsStillApplied, Health.hpp and the egress resolver whose gate
// froze at handout -- four shipped defects where the logic existed and nothing
// ran it. So the last test here does not test the predicate at all; it reads
// Tunnel.cpp and fails if Tunnel::Open stops asking.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "TunnelPolicy.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

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
  UR_EXPECT_EQ(config.mtu, urnw::kTunnelMtu);
  UR_EXPECT_EQ(config.mtu, 1100);
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

// ---- what the call site must refuse ----------------------------------------

UR_TEST(tunnelPolicyRefusesAPrefixThatWouldSwallowTheDefaultRoute) {
  urnw::TunnelConfig config;
  // /0 on the tun means a CONNECTED route covering the whole IPv4 space, which
  // captures traffic without any of the policy routing that is supposed to
  // decide it. Tunnel::Open's own prefix check used to accept 0.
  config.prefix_v4 = 0;
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));

  config.prefix_v4 = 1;
  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));
  config.prefix_v4 = 32;
  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));
  config.prefix_v4 = -1;
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
}

UR_TEST(tunnelPolicyAcceptsTheConfigurationTheDaemonActuallyBuilds) {
  // TunnelHost's fallbacks verbatim: the link-local address it substitutes when
  // the device reports an unusable one, and a resolver list of one. A default
  // that the guard refuses would refuse every connect.
  urnw::TunnelConfig config;
  config.local_addr_v4 = "169.254.2.1";
  config.prefix_v4 = 24;
  config.dns_servers_v4 = {"65.49.70.65"};
  config.require_egress_protection = true;
  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));

  // No resolver at all is a tunnel without DNS of its own, not a refusal: the
  // nft DNS floor and status.dns_detail describe that state, and TunnelHost
  // reaches it when neither the device nor the SDK default is usable.
  config.dns_servers_v4.clear();
  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));

  // The fork's own field is not part of the address-family decision.
  config.require_egress_protection = false;
  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));
}

UR_TEST(nftCgroupPolicyKeepsTheCgroupBeltWhenTheKernelSupportsIt) {
  UR_EXPECT_TRUE(urnw::SelectNftCgroupMode(/*socketMarkerProven=*/false,
                                           /*cgroupSocketMatchSupported=*/true,
                                           /*blockFloor=*/true,
                                           /*helperDnsRequired=*/true) ==
                 urnw::NftCgroupMode::CgroupAndMark);
}

UR_TEST(nftCgroupPolicyUsesAProvenMarkOnFloorlessUnsupportedKernels) {
  UR_EXPECT_TRUE(urnw::SelectNftCgroupMode(/*socketMarkerProven=*/true,
                                           /*cgroupSocketMatchSupported=*/false,
                                           /*blockFloor=*/false,
                                           /*helperDnsRequired=*/false) ==
                 urnw::NftCgroupMode::MarkOnly);
}

UR_TEST(nftCgroupPolicyRefusesAnUnprovenMarkOnUnsupportedKernels) {
  UR_EXPECT_TRUE(urnw::SelectNftCgroupMode(/*socketMarkerProven=*/false,
                                           /*cgroupSocketMatchSupported=*/false,
                                           /*blockFloor=*/false,
                                           /*helperDnsRequired=*/false) ==
                 urnw::NftCgroupMode::Refuse);
}

UR_TEST(nftCgroupPolicyDoesNotWeakenFloorOrHelperDns) {
  UR_EXPECT_TRUE(urnw::SelectNftCgroupMode(/*socketMarkerProven=*/true,
                                           /*cgroupSocketMatchSupported=*/false,
                                           /*blockFloor=*/true,
                                           /*helperDnsRequired=*/false) ==
                 urnw::NftCgroupMode::Refuse);
  UR_EXPECT_TRUE(urnw::SelectNftCgroupMode(/*socketMarkerProven=*/true,
                                           /*cgroupSocketMatchSupported=*/false,
                                           /*blockFloor=*/false,
                                           /*helperDnsRequired=*/true) ==
                 urnw::NftCgroupMode::Refuse);
}

UR_TEST(tunnelPolicyRefusesAnEmptyOrTruncatedAddress) {
  urnw::TunnelConfig config;
  config.local_addr_v4 = "";
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
  config.local_addr_v4 = "192.0.2";
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
  config.local_addr_v4 = "192.0.2.";
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
  config.local_addr_v4 = "192.0.2.1";
  UR_EXPECT_TRUE(urnw::IsIpv4OnlyTunnelConfig(config));
  // One bad resolver poisons the whole list -- Open refuses rather than
  // quietly bringing a tunnel up with the survivors.
  config.dns_servers_v4 = {"9.9.9.9", "2001:4860:4860::8888"};
  UR_EXPECT_FALSE(urnw::IsIpv4OnlyTunnelConfig(config));
}

// ---- the call site ---------------------------------------------------------

namespace {

std::string ReadTunnelSource() {
  const std::string candidates[] = {
      std::string(UR_SRC_DIR) + "/Tunnel.cpp",
      "app/src/Tunnel.cpp",
      "../app/src/Tunnel.cpp",
      "linux/app/src/Tunnel.cpp",
  };
  for (const auto& path : candidates) {
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::ostringstream out;
    out << in.rdbuf();
    if (!out.str().empty()) return out.str();
  }
  return std::string();
}

}  // namespace

UR_TEST(tunnelOpenRefusesANonIpv4ConfigurationBeforeItTouchesTheDevice) {
  const std::string source = ReadTunnelSource();
  // NOT a skip. A predicate nobody calls is the defect this test exists for, so
  // "I could not check" has to read as failure, not as silence.
  // UR_FAIL records and continues, so every fatal step returns: one clear line
  // beats a cascade of consequences of the same missing thing.
  if (source.empty()) {
    UR_FAIL("could not read Tunnel.cpp to check the guard is wired");
    return;
  }

  const size_t open = source.find("std::unique_ptr<Tunnel> Tunnel::Open(");
  if (open == std::string::npos) {
    UR_FAIL("Tunnel::Open was not found in Tunnel.cpp");
    return;
  }

  const size_t guard = source.find("IsIpv4OnlyTunnelConfig(cfg)", open);
  if (guard == std::string::npos) {
    UR_FAIL("Tunnel::Open does not call IsIpv4OnlyTunnelConfig -- the IPv4-only "
            "policy exists and nothing enforces it");
    return;
  }

  // Upstream's diagnostic, so a reader of the journal sees the same line on
  // both codebases.
  if (source.find("[tun] refusing non-IPv4 tunnel configuration", open) == std::string::npos) {
    UR_FAIL("the [tun] refusal diagnostic is missing from Tunnel::Open");
  }

  // BEFORE the device: the whole point is that no tun is created, no address is
  // assigned and no route is installed for a configuration we will not honour.
  const size_t device = source.find("\"/dev/net/tun\"", open);
  if (device == std::string::npos) {
    UR_FAIL("Tunnel::Open no longer opens /dev/net/tun");
    return;
  }
  if (guard > device) UR_FAIL("the IPv4-only guard runs AFTER the tun device is opened");

  // And ahead of every other field check, matching upstream's placement.
  const size_t firstOtherCheck = source.find("ValidInterfaceName(cfg.name)", open);
  if (firstOtherCheck != std::string::npos && guard > firstOtherCheck) {
    UR_FAIL("the IPv4-only guard is no longer the first check in Tunnel::Open");
  }
}
