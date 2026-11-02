// The dual-stack tunnel policy, and THE FACT THAT SOMETHING CALLS IT.
//
// The predicate below was once restored from upstream with no caller: the file
// was present and the fix was not. That is the same shape as NetFilter::Verify,
// Tunnel::VerifyDnsStillApplied, Health.hpp and the egress resolver whose gate
// froze at handout -- four shipped defects where the logic existed and nothing
// ran it. So the last test here does not test the predicate at all; it reads
// Tunnel.cpp and fails if Tunnel::Open stops asking.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "Tunnel.hpp"
#include "TunnelPolicy.hpp"
#include "daemon/HostSettings.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

urnw::TunnelConfig DualStack() {
  urnw::TunnelConfig config;
  config.local_addr_v4 = "169.254.2.1";
  config.prefix_v4 = 24;
  config.dns_servers_v4 = {"65.49.70.65", "9.9.9.9"};
  config.local_addr_v6 = "fd00:7572:6e65:1a2b:3c4d:5e6f:7081:92a3";
  config.prefix_v6 = 64;
  config.dns_servers_v6 = {"2001:db8::65:49:70:65"};
  return config;
}

// Keep the genuinely compile-time half of TunnelPolicy on the C++17 boundary.
// The TunnelConfig validators below consume owning strings and vectors at
// runtime; compiling this file with the Ubuntu 22.04 GCC 11 toolchain is the
// regression for accidentally marking that runtime boundary constexpr.
static_assert(urnw::IsIpv4Literal("192.0.2.1"));
static_assert(!urnw::IsIpv4Literal("192.0.2.999"));
static_assert(urnw::IsIpv6Literal("2001:db8::1"));
static_assert(!urnw::IsIpv6Literal("2001:db8::1::2"));
static_assert(urnw::Ipv6PrefixContains("fc00::/7", "fd00::1"));
static_assert(urnw::IsIpv6UniqueLocal("fd00::1"));
static_assert(urnw::CaptureV6Claims("2001:db8::1"));
static_assert(!urnw::CaptureV6Claims("fe80::1"));
static_assert(urnw::WitnessTreatmentVerdict(/*bound=*/false, "", ENETUNREACH, true, 0x55524e57u,
                                            0x55524e57u, "169.254.2.1") ==
              urnw::WitnessTreatment::Unroutable);

}  // namespace

// ---- device memory target --------------------------------------------------

UR_TEST(deviceMemoryTargetAndProcessBudgetAreTheDesktopPairs) {
  UR_EXPECT_EQ(std::int64_t{128} * 1024 * 1024, urnw::kDeviceMemoryTargetByteCount);
  UR_EXPECT_EQ(std::int64_t{384} * 1024 * 1024, urnw::kProcessMemoryBudgetByteCount);
  UR_EXPECT_EQ(std::int64_t{256} * 1024 * 1024, urnw::kLargeHostDeviceMemoryTargetByteCount);
  UR_EXPECT_EQ(std::int64_t{768} * 1024 * 1024, urnw::kLargeHostProcessMemoryBudgetByteCount);
  UR_EXPECT_EQ(std::int64_t{7} * 1024 * 1024 * 1024, urnw::kLargeHostMemoryByteCount);
}

// The gate over the measurement, including the failure case. With the bar this
// low nearly every real machine is on one side of it, so an off-by-one in the
// comparison would be invisible in practice: the three rows around 7 GiB are
// the only thing that would catch it. The two rows after them are the machines
// the decision is about, and fail if the bar is rounded up to 8.
UR_TEST(theMemoryTierIsChosenFromMeasuredHostMemory) {
  constexpr std::int64_t gib = std::int64_t{1024} * 1024 * 1024;
  const struct {
    std::int64_t host;
    std::int64_t target;
  } rows[] = {
      {0, urnw::kDeviceMemoryTargetByteCount},                     // unmeasurable
      {-1, urnw::kDeviceMemoryTargetByteCount},                    // a failed read
      {2 * gib, urnw::kDeviceMemoryTargetByteCount},               // a small vps
      {4 * gib, urnw::kDeviceMemoryTargetByteCount},               // a small container
      {7 * gib - 1, urnw::kDeviceMemoryTargetByteCount},           // one byte under the bar
      {7 * gib, urnw::kDeviceMemoryTargetByteCount},               // exactly at it: strict
      {7 * gib + 1, urnw::kLargeHostDeviceMemoryTargetByteCount},  // one byte over
      {urnw::kNominal8GiBHostMemoryByteCount,                      // an exact 8 GiB reading
       urnw::kLargeHostDeviceMemoryTargetByteCount},
      {urnw::kUsable8GiBHostMemoryByteCount,  // an 8 GiB machine's usable ~7.68 GiB
       urnw::kLargeHostDeviceMemoryTargetByteCount},
      {64 * gib, urnw::kLargeHostDeviceMemoryTargetByteCount},
  };
  for (const auto& row : rows) {
    UR_EXPECT_EQ(row.target, urnw::MemoryTierForHost(row.host).device_target_byte_count);
  }
  // The budget always moves with the target it backs, including on the rare
  // unknown-host path and for the machines the bar exists to include.
  UR_EXPECT_EQ(urnw::kProcessMemoryBudgetByteCount,
               urnw::MemoryTierForHost(0).process_budget_byte_count);
  UR_EXPECT_EQ(urnw::kProcessMemoryBudgetByteCount,
               urnw::MemoryTierForHost(7 * gib).process_budget_byte_count);
  UR_EXPECT_EQ(urnw::kLargeHostProcessMemoryBudgetByteCount,
               urnw::MemoryTierForHost(7 * gib + 1).process_budget_byte_count);
  UR_EXPECT_EQ(urnw::kLargeHostProcessMemoryBudgetByteCount,
               urnw::MemoryTierForHost(urnw::kUsable8GiBHostMemoryByteCount)
                   .process_budget_byte_count);
}

// End to end through the probe's own parser: the /proc/meminfo an 8 GiB machine
// actually produces lands in the large tier, and a 6 GiB machine's does not.
UR_TEST(aMachineSoldAs8GiBTakesTheLargeTierThroughTheMeminfoParser) {
  const std::string eightGiB = "MemTotal:        8048972 kB\nMemFree:         1024 kB\n";
  UR_EXPECT_EQ(urnw::kLargeHostDeviceMemoryTargetByteCount,
               urnw::MemoryTierForHost(urnw::EffectiveHostMemoryByteCount(
                                           urnw::ParseMeminfoTotalByteCount(eightGiB), 0))
                   .device_target_byte_count);
  const std::string sixGiB = "MemTotal:        6021340 kB\nMemFree:         1024 kB\n";
  UR_EXPECT_EQ(urnw::kDeviceMemoryTargetByteCount,
               urnw::MemoryTierForHost(urnw::EffectiveHostMemoryByteCount(
                                           urnw::ParseMeminfoTotalByteCount(sixGiB), 0))
                   .device_target_byte_count);
}

// The two constraints, on BOTH tiers and at the bar itself. The header
// static_asserts them, which fails the build rather than a test; this states
// them where a reader looking for the rule will find it, and catches a header
// that drops them.
UR_TEST(everyMemoryTierIsBackedAndCollectorSafe) {
  constexpr std::int64_t gib = std::int64_t{1024} * 1024 * 1024;
  for (const std::int64_t host :
       {std::int64_t{0}, 7 * gib - 1, 7 * gib, 7 * gib + 1, 8 * gib, 128 * gib}) {
    const urnw::MemoryTier tier = urnw::MemoryTierForHost(host);
    // backing: the target is at most 20/34 of the budget, the pools taking 14
    UR_EXPECT_TRUE_MSG("a device memory target is not backed by its process budget",
                       urnw::MemoryTierIsBacked(tier));
    // collector: the budget is at least three times the target
    UR_EXPECT_TRUE_MSG("a process budget is too close to its device memory target",
                       urnw::MemoryTierIsCollectorSafe(tier));
  }
}

// The pure halves of the measurement (daemon/HostMemory.cpp does the reads).
UR_TEST(hostMemoryIsParsedFromMeminfoAndTheCgroupLimit) {
  const std::string meminfo =
      "MemTotal:       16305456 kB\nMemFree:         1234 kB\nSwapTotal:  0 kB\n";
  UR_EXPECT_EQ(std::int64_t{16305456} * 1024, urnw::ParseMeminfoTotalByteCount(meminfo));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::ParseMeminfoTotalByteCount(""));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::ParseMeminfoTotalByteCount("MemFree: 100 kB\n"));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::ParseMeminfoTotalByteCount("MemTotal:       kB\n"));
  // MemTotal need not be the first line.
  UR_EXPECT_EQ(std::int64_t{8} * 1024 * 1024,
               urnw::ParseMeminfoTotalByteCount("MemFree: 1 kB\nMemTotal: 8192 kB"));

  // cgroup v2 "max" and the v1 page-rounded sentinel are no limit at all.
  UR_EXPECT_EQ(std::int64_t{2} * 1024 * 1024 * 1024,
               urnw::ParseCgroupMemoryLimitByteCount("2147483648\n"));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::ParseCgroupMemoryLimitByteCount("max\n"));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::ParseCgroupMemoryLimitByteCount(""));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::ParseCgroupMemoryLimitByteCount("9223372036854771712"));

  // A container limit below physical memory is what the daemon may use.
  constexpr std::int64_t gib = std::int64_t{1024} * 1024 * 1024;
  UR_EXPECT_EQ(2 * gib, urnw::EffectiveHostMemoryByteCount(64 * gib, 2 * gib));
  UR_EXPECT_EQ(64 * gib, urnw::EffectiveHostMemoryByteCount(64 * gib, 0));
  UR_EXPECT_EQ(2 * gib, urnw::EffectiveHostMemoryByteCount(0, 2 * gib));
  UR_EXPECT_EQ(std::int64_t{0}, urnw::EffectiveHostMemoryByteCount(0, 0));
}

// A 64 GiB machine in a 2 GiB container is a SMALL host: the tier follows what
// this process may use, not what the machine has.
UR_TEST(aContainerLimitDecidesTheTierRatherThanTheMachine) {
  constexpr std::int64_t gib = std::int64_t{1024} * 1024 * 1024;
  const std::int64_t usable = urnw::EffectiveHostMemoryByteCount(64 * gib, 2 * gib);
  UR_EXPECT_EQ(urnw::kDeviceMemoryTargetByteCount,
               urnw::MemoryTierForHost(usable).device_target_byte_count);
}

// The budget is only real if the daemon passes it to the SDK, and it is a
// separate surface from the device target: setMemoryLimit sizes the pools and
// the go soft limit, never the device.
UR_TEST(theDaemonSetsTheProcessBudgetFromThePolicy) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/daemon/main.cpp");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string source = buffer.str();
  if (source.empty()) {
    UR_FAIL("could not read daemon/main.cpp to check the process budget");
    return;
  }
  UR_EXPECT_TRUE_MSG(
      "daemon/main.cpp does not take its process budget from the measured memory tier",
      source.find("urnw::MemoryTierForHost(urnw::HostMemoryByteCountCached())"
                  ".process_budget_byte_count") != std::string::npos);
  UR_EXPECT_TRUE_MSG(
      "daemon/main.cpp does not call setMemoryLimit with the tier's budget",
      source.find("setMemoryLimit(ProcessMemoryBudgetByteCount())") != std::string::npos);
}

// Same shape as the dual-stack guard test below: the constant is worthless if
// TunnelHost goes back to a constructor that takes no target, so read the
// source and fail when it does.
UR_TEST(tunnelHostConstructsEveryDeviceAtTheMemoryTarget) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/daemon/TunnelHost.cpp");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string source = buffer.str();
  if (source.empty()) {
    UR_FAIL("could not read daemon/TunnelHost.cpp to check the device memory target");
    return;
  }

  // The constructors without a target fall back to the SDK's 20 MiB default.
  UR_EXPECT_TRUE_MSG("TunnelHost.cpp still calls newDeviceLocalWithDefaults",
                     source.find("urnet::newDeviceLocalWithDefaults(") == std::string::npos);
  UR_EXPECT_TRUE_MSG("TunnelHost.cpp still calls newDeviceLocalWithKeyMaterial",
                     source.find("urnet::newDeviceLocalWithKeyMaterial(") == std::string::npos);

  // Both constructions (restored identity, new identity) pass the constant.
  const std::string call = "urnet::newDeviceLocalWithMemoryTarget(";
  size_t constructions = 0;
  for (size_t at = source.find(call); at != std::string::npos; at = source.find(call, at + 1)) {
    ++constructions;
    const size_t end = source.find(");", at);
    const std::string args = end == std::string::npos ? std::string() : source.substr(at, end - at);
    UR_EXPECT_TRUE_MSG(
        "a newDeviceLocalWithMemoryTarget call does not pass the tier's device target",
        args.find("memoryTier.device_target_byte_count") != std::string::npos);
  }
  UR_EXPECT_EQ(size_t{2}, constructions);

  // ...and the tier comes from the same cached measurement the budget used, so
  // a large target can never be paired with a small budget.
  UR_EXPECT_TRUE_MSG(
      "TunnelHost.cpp does not take its device target from the cached host measurement",
      source.find("urnw::MemoryTierForHost(urnw::HostMemoryByteCountCached())") !=
          std::string::npos);
}

// ---- v6 literals -----------------------------------------------------------

UR_TEST(ipv6LiteralAcceptsTheStandardForms) {
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("::1"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("::"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("fd00:7572:6e65:ffff::2"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("2001:db8::65:49:70:65"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("2001:0db8:0000:0000:0000:0000:0000:0001"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("2001:DB8::1"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("fe80::1"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("1::"));
  // an embedded dotted quad, as a mapped address is written
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("::ffff:192.0.2.1"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("64:ff9b::192.0.2.33"));
}

UR_TEST(ipv6LiteralRejectsWhatIsNotAnAddress) {
  UR_EXPECT_FALSE(urnw::IsIpv6Literal(""));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("169.254.2.1"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("resolver.example"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal(":1"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("1:"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal(":::"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("1::2::3"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("12345::1"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("g::1"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("1:2:3:4:5:6:7"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("1:2:3:4:5:6:7:8:9"));
  UR_EXPECT_TRUE(urnw::IsIpv6Literal("1:2:3:4:5:6:7::"));   // "::" is the one zero group left
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("1:2:3:4:5:6:7:8::"));  // nothing left for "::" to stand for
  // an embedded quad anywhere but last, or malformed
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("::192.0.2.1:1"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("::192.0.2"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("::192.0.2.256"));
  // no brackets, zone ids or prefix lengths: those are not addresses
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("[::1]"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("fe80::1%eth0"));
  UR_EXPECT_FALSE(urnw::IsIpv6Literal("fd00::/64"));
}

UR_TEST(ipv6LiteralParsesToTheRightBytes) {
  urnw::Ipv6Bytes bytes;
  UR_EXPECT_TRUE(urnw::ParseIpv6Literal("2001:db8::1", &bytes));
  UR_EXPECT_EQ(0x20, bytes.b[0]);
  UR_EXPECT_EQ(0x01, bytes.b[1]);
  UR_EXPECT_EQ(0x0d, bytes.b[2]);
  UR_EXPECT_EQ(0xb8, bytes.b[3]);
  for (int i = 4; i < 15; ++i) UR_EXPECT_EQ(0, bytes.b[i]);
  UR_EXPECT_EQ(1, bytes.b[15]);

  UR_EXPECT_TRUE(urnw::ParseIpv6Literal("::ffff:192.0.2.1", &bytes));
  UR_EXPECT_EQ(0xff, bytes.b[10]);
  UR_EXPECT_EQ(0xff, bytes.b[11]);
  UR_EXPECT_EQ(192, bytes.b[12]);
  UR_EXPECT_EQ(0, bytes.b[13]);
  UR_EXPECT_EQ(2, bytes.b[14]);
  UR_EXPECT_EQ(1, bytes.b[15]);

  UR_EXPECT_TRUE(urnw::ParseIpv6Literal("1::", &bytes));
  UR_EXPECT_EQ(0, bytes.b[0]);
  UR_EXPECT_EQ(1, bytes.b[1]);
  UR_EXPECT_EQ(0, bytes.b[15]);
}

UR_TEST(ipv6PrefixContainsHonoursThePrefixLength) {
  UR_EXPECT_TRUE(urnw::Ipv6PrefixContains("fc00::/7", "fd00:7572:6e65::1"));
  UR_EXPECT_TRUE(urnw::Ipv6PrefixContains("fc00::/7", "fc12::1"));
  UR_EXPECT_FALSE(urnw::Ipv6PrefixContains("fc00::/7", "fe80::1"));
  UR_EXPECT_TRUE(urnw::Ipv6PrefixContains("fe80::/10", "febf::1"));
  UR_EXPECT_FALSE(urnw::Ipv6PrefixContains("fe80::/10", "fec0::1"));
  UR_EXPECT_TRUE(urnw::Ipv6PrefixContains("::/0", "2001:db8::1"));
  UR_EXPECT_TRUE(urnw::Ipv6PrefixContains("::1/128", "::1"));
  UR_EXPECT_FALSE(urnw::Ipv6PrefixContains("::1/128", "::2"));
  // malformed is false, never a match
  UR_EXPECT_FALSE(urnw::Ipv6PrefixContains("fc00::/129", "fd00::1"));
  UR_EXPECT_FALSE(urnw::Ipv6PrefixContains("fc00::", "fd00::1"));
  UR_EXPECT_FALSE(urnw::Ipv6PrefixContains("fc00::/7", "not-an-address"));
}

// ---- the dual-stack floor --------------------------------------------------

UR_TEST(tunnelPolicyRuntimeValidatorsAcceptOwningStrings) {
  urnw::TunnelConfig config = DualStack();
  UR_EXPECT_TRUE(urnw::IsIpv4HalfValid(config));
  UR_EXPECT_TRUE(urnw::IsIpv6HalfValid(config));

  config.dns_servers_v4.push_back("not-an-address");
  UR_EXPECT_FALSE(urnw::IsIpv4HalfValid(config));
  UR_EXPECT_TRUE(urnw::IsIpv6HalfValid(config));

  config = DualStack();
  config.dns_servers_v6.push_back("not-an-address");
  UR_EXPECT_TRUE(urnw::IsIpv4HalfValid(config));
  UR_EXPECT_FALSE(urnw::IsIpv6HalfValid(config));
}

UR_TEST(tunnelPolicyAcceptsADualStackConfiguration) {
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(DualStack()));
}

UR_TEST(tunnelPolicyAcceptsTheConfigurationTheDaemonActuallyBuilds) {
  // TunnelHost's fallbacks verbatim: the addresses it substitutes when the
  // device reports unusable ones, and a resolver list of one per family. A
  // default that the guard refuses would refuse every connect.
  urnw::TunnelConfig config;
  UR_EXPECT_TRUE(config.local_addr_v4 == "169.254.2.1");
  UR_EXPECT_TRUE(config.local_addr_v6 == "fd00:7572:6e65:ffff::2");
  UR_EXPECT_EQ(config.prefix_v4, 24);
  UR_EXPECT_EQ(config.prefix_v6, 64);
  UR_EXPECT_EQ(config.mtu, urnw::kTunnelMtu);
  UR_EXPECT_EQ(config.mtu, 1280);
  UR_EXPECT_TRUE(config.dns_servers_v4.empty());
  UR_EXPECT_TRUE(config.dns_servers_v6.empty());
  config.dns_servers_v4 = {"65.49.70.65"};
  config.dns_servers_v6 = {"2001:db8::65:49:70:65"};
  config.require_egress_protection = true;
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));

  // No resolver of a family is a tunnel whose DNS the other family carries,
  // not a refusal: the nft DNS floor and status.dns_detail describe that state.
  config.dns_servers_v6.clear();
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
  config.dns_servers_v4.clear();
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));

  // The fork's own field is not part of the address-family decision.
  config.require_egress_protection = false;
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
}

UR_TEST(tunnelPolicyRefusesAMissingOrWrongFamilyHalf) {
  // the v4 half must be v4
  urnw::TunnelConfig config = DualStack();
  config.local_addr_v4 = "fd00::1";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config = DualStack();
  config.dns_servers_v4 = {"2001:4860:4860::8888"};
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));

  // the v6 half must be v6
  config = DualStack();
  config.local_addr_v6 = "169.254.2.1";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config = DualStack();
  config.local_addr_v6 = "";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config = DualStack();
  config.dns_servers_v6 = {"9.9.9.9"};
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  // One bad resolver poisons the whole list -- Open refuses rather than
  // quietly bringing a tunnel up with the survivors.
  config = DualStack();
  config.dns_servers_v6 = {"2001:db8::1", "resolver.example"};
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
}

UR_TEST(tunnelPolicyRequiresAUniqueLocalTunnelAddress) {
  // the SDK's fixed /48 and the daemon's fallback are both ULA
  UR_EXPECT_TRUE(urnw::IsIpv6UniqueLocal("fd00:7572:6e65:1a2b::1"));
  UR_EXPECT_TRUE(urnw::IsIpv6UniqueLocal("fd00:7572:6e65:ffff::2"));
  // a global address would be an address the tun claims on the internet, the
  // unspecified address is no address, and multicast cannot be an interface
  // address at all
  urnw::TunnelConfig config = DualStack();
  config.local_addr_v6 = "2001:db8::1";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.local_addr_v6 = "::";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.local_addr_v6 = "ff02::1";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.local_addr_v6 = "fe80::1";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
}

UR_TEST(tunnelPolicyRejectsHostnamesAndInvalidIpv4Values) {
  urnw::TunnelConfig config = DualStack();
  config.local_addr_v4 = "resolver.example";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));

  config.local_addr_v4 = "192.0.2.999";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));

  config.local_addr_v4 = "192.0.2.1";
  config.prefix_v4 = 33;
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
}

// ---- what the call site must refuse ----------------------------------------

UR_TEST(tunnelPolicyRefusesAPrefixThatWouldSwallowTheDefaultRoute) {
  urnw::TunnelConfig config = DualStack();
  // /0 on the tun means a CONNECTED route covering the whole space, which
  // captures traffic without any of the policy routing that is supposed to
  // decide it. Tunnel::Open's own prefix check used to accept 0.
  config.prefix_v4 = 0;
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.prefix_v4 = 1;
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
  config.prefix_v4 = 32;
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
  config.prefix_v4 = -1;
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));

  config = DualStack();
  config.prefix_v6 = 0;
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.prefix_v6 = 1;
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
  config.prefix_v6 = 128;
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
  config.prefix_v6 = 129;
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
}

UR_TEST(tunnelPolicyRefusesAnEmptyOrTruncatedAddress) {
  urnw::TunnelConfig config = DualStack();
  config.local_addr_v4 = "";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.local_addr_v4 = "192.0.2";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.local_addr_v4 = "192.0.2.";
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
  config.local_addr_v4 = "192.0.2.1";
  UR_EXPECT_TRUE(urnw::IsDualStackTunnelConfig(config));
  // One bad resolver poisons the whole list -- Open refuses rather than
  // quietly bringing a tunnel up with the survivors.
  config.dns_servers_v4 = {"9.9.9.9", "2001:4860:4860::8888"};
  UR_EXPECT_FALSE(urnw::IsDualStackTunnelConfig(config));
}

UR_TEST(tunnelMtuClearsTheIpv6LinkMinimum) {
  // The daemon hands the same MTU to both halves. Linux disables IPv6 on an
  // interface whose MTU is below 1280, so the constant must never fall below
  // it -- the v6 half of the tunnel would silently cease to exist.
  UR_EXPECT_TRUE(urnw::kIpv6MinimumMtu <= urnw::kTunnelMtu);
  UR_EXPECT_EQ(1280, urnw::kIpv6MinimumMtu);
}

// ---- the v6 capture set ----------------------------------------------------

UR_TEST(captureV6SetIsTheComplementOfTheThreeExclusions) {
  // captured: global unicast, the documentation prefix the in-tunnel resolver
  // and the egress witness use, 6to4, NAT64, the whole lower half
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("2001:db8::65:49:70:65"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("2001:db8::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("2606:4700:4700::1111"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("2002:c000:0204::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("3fff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("64:ff9b::192.0.2.33"));
  // the edges of every capture prefix
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("7fff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("8000::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("bfff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("c000::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("dfff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("e000::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("efff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("f000::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("f7ff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("f800::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("fbff:ffff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("fe00::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("fe7f:ffff::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("fec0::"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("feff:ffff::1"));

  // excluded: ULA (the LAN analogue, and the tun's own family), link-local,
  // multicast -- including the solicited-node block DAD depends on
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("fc00::1"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("fd00:7572:6e65:ffff::2"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("fdff:ffff::1"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("fe80::1"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("febf:ffff::1"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("ff02::1"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("ff02::1:ff00:1"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("ff05::1:3"));
  UR_EXPECT_FALSE(urnw::CaptureV6Claims("ffff::1"));
}

UR_TEST(captureV6PrefixesAreEightAndDisjointFromTheExclusions) {
  const auto& capture = urnw::CaptureV6Prefixes();
  UR_EXPECT_EQ(8u, capture.size());
  // every capture prefix is a well-formed prefix that excludes all three
  for (const auto& prefix : capture) {
    const size_t slash = prefix.find('/');
    UR_EXPECT_TRUE(slash != std::string::npos);
    UR_EXPECT_TRUE(urnw::IsIpv6Literal(prefix.substr(0, slash)));
    for (const auto& excluded : urnw::ExcludedV6Prefixes()) {
      const size_t excludedSlash = excluded.find('/');
      const std::string excludedNetwork = excluded.substr(0, excludedSlash);
      UR_EXPECT_TRUE_MSG(prefix + " must not contain " + excluded,
                         !urnw::Ipv6PrefixContains(prefix, excludedNetwork));
    }
  }
  // and the three exclusions are exactly the ones the design names
  UR_EXPECT_EQ(3u, urnw::ExcludedV6Prefixes().size());
  UR_EXPECT_TRUE(urnw::ExcludedV6Prefixes()[0] == "fe80::/10");
  UR_EXPECT_TRUE(urnw::ExcludedV6Prefixes()[1] == "fc00::/7");
  UR_EXPECT_TRUE(urnw::ExcludedV6Prefixes()[2] == "ff00::/8");
}

// Loopback is claimed by the kernel's `local` table (rule pref 0) before the
// capture rule is ever consulted, exactly as 127.0.0.0/8 inside 64.0.0.0/2 is
// for v4 -- so ::1 sits inside ::/1 and needs no route of its own.
UR_TEST(captureV6SetLeavesLoopbackToTheLocalTable) {
  UR_EXPECT_TRUE(urnw::Ipv6PrefixContains("::/1", "::1"));
  UR_EXPECT_TRUE(urnw::CaptureV6Claims("::1"));
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

// ---- the egress witness's treatment leg ------------------------------------

namespace {

struct TreatmentRow {
  const char* what;
  bool bound;
  const char* source;
  int connectErrno;
  bool markKnown;
  std::uint32_t mark;
  urnw::WitnessTreatment expected;
};

}  // namespace

// Every outcome of the one socket the treatment leg reads, in both families:
// the tunnel's own addresses are the yardsticks.
UR_TEST(witnessTreatmentVerdictReadsEveryOutcomeOfTheTreatmentSocket) {
  using urnw::WitnessTreatment;
  constexpr std::uint32_t kMark = urnw::kEgressMark;
  const char* const kTun4 = "169.254.2.1";
  const char* const kTun6 = "fd00:7572:6e65:ffff::2";
  const TreatmentRow v4[] = {
      {"bound a physical source with the mark", true, "192.168.1.20", 0, true, kMark,
       WitnessTreatment::Escaped},
      {"no route at all, with the mark (between networks, or IPv6-only)", false, "",
       ENETUNREACH, true, kMark, WitnessTreatment::Unroutable},
      {"an unreachable route, with the mark", false, "", EHOSTUNREACH, true, kMark,
       WitnessTreatment::Unroutable},
      {"no source address left, with the mark", false, "", EADDRNOTAVAIL, true, kMark,
       WitnessTreatment::Unroutable},
      {"bound the tun's own address", true, kTun4, 0, true, kMark,
       WitnessTreatment::Captured},
      {"bound the tun's own address unmarked", true, kTun4, 0, true, 0,
       WitnessTreatment::Captured},
      {"escaped without the mark", true, "192.168.1.20", 0, true, 0,
       WitnessTreatment::Unmarked},
      {"escaped with another tool's mark", true, "192.168.1.20", 0, true, urnw::kBypassMark,
       WitnessTreatment::Unmarked},
      {"escaped with an unreadable mark", true, "192.168.1.20", 0, false, kMark,
       WitnessTreatment::Unmarked},
      {"no route WITHOUT the mark", false, "", ENETUNREACH, true, 0,
       WitnessTreatment::Unmarked},
      {"no route with an unreadable mark", false, "", ENETUNREACH, false, kMark,
       WitnessTreatment::Unmarked},
      {"refused by a prohibit route", false, "", EACCES, true, kMark,
       WitnessTreatment::Failed},
      {"socket() or getsockname() failed before connect()", false, "", 0, true, kMark,
       WitnessTreatment::Failed},
  };
  for (const auto& row : v4) {
    if (urnw::WitnessTreatmentVerdict(row.bound, row.source, row.connectErrno, row.markKnown,
                                      row.mark, kMark, kTun4) != row.expected) {
      UR_FAIL(std::string("v4, ") + row.what);
    }
  }
  // The v6 half reads the same table against its own yardstick, the no-route
  // rows included: the mark is required there too now.
  UR_EXPECT_TRUE(urnw::WitnessTreatmentVerdict(true, "2001:db8:1::20", 0, true, kMark, kMark,
                                               kTun6) == WitnessTreatment::Escaped);
  UR_EXPECT_TRUE(urnw::WitnessTreatmentVerdict(false, "", ENETUNREACH, true, kMark, kMark,
                                               kTun6) == WitnessTreatment::Unroutable);
  UR_EXPECT_TRUE(urnw::WitnessTreatmentVerdict(false, "", ENETUNREACH, true, 0, kMark, kTun6) ==
                 WitnessTreatment::Unmarked);
  UR_EXPECT_TRUE(urnw::WitnessTreatmentVerdict(true, kTun6, 0, true, kMark, kMark, kTun6) ==
                 WitnessTreatment::Captured);
  // A v4 yardstick never matches a v6 source: the halves stay apart.
  UR_EXPECT_TRUE(urnw::WitnessTreatmentVerdict(true, kTun6, 0, true, kMark, kMark, kTun4) ==
                 WitnessTreatment::Escaped);
}

UR_TEST(onlyTheNoRouteErrnosMeanNoRoute) {
  UR_EXPECT_TRUE(urnw::IsNoRouteErrno(ENETUNREACH));
  UR_EXPECT_TRUE(urnw::IsNoRouteErrno(EHOSTUNREACH));
  UR_EXPECT_TRUE(urnw::IsNoRouteErrno(EADDRNOTAVAIL));
  for (const int other : {0, EACCES, EPERM, EINVAL, ECONNREFUSED, EAFNOSUPPORT}) {
    if (urnw::IsNoRouteErrno(other)) UR_FAIL("errno " + std::to_string(other) + " is not no-route");
  }
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

UR_TEST(tunnelOpenRefusesANonDualStackConfigurationBeforeItTouchesTheDevice) {
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

  const size_t guard = source.find("IsDualStackTunnelConfig(cfg)", open);
  if (guard == std::string::npos) {
    UR_FAIL("Tunnel::Open does not call IsDualStackTunnelConfig -- the dual-stack "
            "policy exists and nothing enforces it");
    return;
  }

  if (source.find("[tun] refusing a tunnel configuration that is not dual-stack", open) ==
      std::string::npos) {
    UR_FAIL("the [tun] refusal diagnostic is missing from Tunnel::Open");
  }

  // BEFORE the device: the whole point is that no tun is created, no address is
  // assigned and no route is installed for a configuration we will not honour.
  const size_t device = source.find("\"/dev/net/tun\"", open);
  if (device == std::string::npos) {
    UR_FAIL("Tunnel::Open no longer opens /dev/net/tun");
    return;
  }
  if (guard > device) UR_FAIL("the dual-stack guard runs AFTER the tun device is opened");

  // And ahead of every other field check.
  const size_t firstOtherCheck = source.find("ValidInterfaceName(cfg.name)", open);
  if (firstOtherCheck != std::string::npos && guard > firstOtherCheck) {
    UR_FAIL("the dual-stack guard is no longer the first check in Tunnel::Open");
  }

  // The v6 half is installed from the ONE capture table, so the routes and the
  // firewall's v6 permits cannot disagree about what is captured.
  if (source.find("CaptureV6Prefixes()", open) == std::string::npos) {
    UR_FAIL("Tunnel.cpp installs no v6 capture routes: the v6 half of the tunnel is missing");
  }
}

// ---- per-app split tunnel (urnetwork-exclude) -------------------------------

namespace {

void ExpectText(const char* what, const std::string& expected, const std::string& actual) {
  if (expected != actual) {
    UR_FAIL(std::string(what) + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

// A file below app/src (or beside it, with a leading "../").
std::string ReadAppSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The body of the function whose definition starts with `signature`, or ""
// (braces balanced from the first '{' after the signature).
std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = source.find('{', at);
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') ++depth;
    if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
  }
  return std::string();
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

UR_TEST(bypassMarkIsItsOwnAndIsRoutedAheadOfTheCaptureRule) {
  ExpectText("MarkText(kBypassMark)", "0x55524e58", urnw::MarkText(urnw::kBypassMark));
  ExpectText("MarkText(kEgressMark)", "0x55524e57", urnw::MarkText(urnw::kEgressMark));
  ExpectText("MarkText(0)", "0x00000000", urnw::MarkText(0));
  // Not the daemon's own mark: the egress witness counts kEgressMark packets in
  // the tun as the storm signature, and an excluded app must never feed it.
  UR_EXPECT_TRUE(urnw::kBypassMark != urnw::kEgressMark);
  // Decided before the capture rule sends everything unmarked to the tunnel,
  // and never at the retired priority the sweeps delete.
  UR_EXPECT_TRUE(urnw::kBypassRulePriority < urnw::kFwmarkRulePriority);
  UR_EXPECT_TRUE(urnw::kBypassRulePriority != urnw::kSuppressRulePriority);
  const std::vector<std::string> want = {"fwmark", "0x55524e58", "table", "main",
                                         "pref",   "32761"};
  UR_EXPECT_TRUE(urnw::BypassRuleSelector() == want);
}

UR_TEST(excludeSliceIsTheOwnersSliceBelowTheirUserManager) {
  ExpectText("ExcludeSliceCgroupPath(1000)",
             "user.slice/user-1000.slice/user@1000.service/urnetwork.slice/"
             "urnetwork-exclude.slice",
             urnw::ExcludeSliceCgroupPath(1000));
  ExpectText("ExcludeSliceCgroupPath(0)",
             "user.slice/user-0.slice/user@0.service/urnetwork.slice/urnetwork-exclude.slice",
             urnw::ExcludeSliceCgroupPath(0));
  // Nobody owns the tunnel: no slice at all, never a root-relative guess.
  UR_EXPECT_TRUE(urnw::ExcludeSliceCgroupPath(-1).empty());
  UR_EXPECT_EQ(5, urnw::CgroupPathLevel(urnw::ExcludeSliceCgroupPath(1000)));
  UR_EXPECT_EQ(2, urnw::CgroupPathLevel("system.slice/urnetworkd.service"));
  UR_EXPECT_EQ(0, urnw::CgroupPathLevel(""));
}

// The v1 detection. The exclusion is hidden unless /proc/self/cgroup describes
// the unified hierarchy alone.
UR_TEST(cgroupV1OnlyAndHybridHostsHideTheExclusion) {
  UR_EXPECT_TRUE(urnw::IsCgroupV2Only("0::/user.slice/user-1000.slice/session-2.scope\n"));
  UR_EXPECT_TRUE(urnw::IsCgroupV2Only("0::/system.slice/urnetworkd.service"));
  UR_EXPECT_TRUE(urnw::IsCgroupV2Only("0::/\n"));  // a container's root
  // legacy: v1 hierarchies only
  UR_EXPECT_FALSE(urnw::IsCgroupV2Only(
      "12:pids:/user.slice/user-1000.slice/session-2.scope\n"
      "11:memory:/user.slice/user-1000.slice/session-2.scope\n"
      "1:name=systemd:/user.slice/user-1000.slice/session-2.scope\n"));
  // hybrid: the unified hierarchy beside v1 controllers (net_cls among them,
  // which switches socket cgroup matching off in the kernel)
  UR_EXPECT_FALSE(urnw::IsCgroupV2Only(
      "12:net_cls,net_prio:/\n"
      "1:name=systemd:/user.slice/user-1000.slice/session-2.scope\n"
      "0::/user.slice/user-1000.slice/session-2.scope\n"));
  // unreadable or empty
  UR_EXPECT_FALSE(urnw::IsCgroupV2Only(""));
  UR_EXPECT_FALSE(urnw::IsCgroupV2Only("\n"));
}

UR_TEST(excludeRulesRenderTheBypassMark) {
  const std::string slice = urnw::ExcludeSliceCgroupPath(1000);
  ExpectText("ExcludeMarkRule",
             "socket cgroupv2 level 5 \"user.slice/user-1000.slice/user@1000.service/"
             "urnetwork.slice/urnetwork-exclude.slice\" counter meta mark set 0x55524e58",
             urnw::ExcludeMarkRule(slice, urnw::CgroupPathLevel(slice)));
  ExpectText("ExcludeAcceptRule", "meta mark 0x55524e58 counter accept",
             urnw::ExcludeAcceptRule());
  ExpectText("ExcludeMasqueradeRule",
             "meta mark 0x55524e58 oifname != \"urnet0\" oifname != \"lo\" counter masquerade",
             urnw::ExcludeMasqueradeRule("urnet0"));
}

// The rules are only as safe as where BuildNftRuleset puts them: marked in the
// mark chain, accepted in urnw_out before the blocks that would refuse them and
// after the metadata drop, masqueraded only while a tun exists, and all of it
// from the derived (cgroup-matched, quotable) list, never the raw config.
UR_TEST(buildNftRulesetPlacesTheExclusionRules) {
  const std::string body = FunctionBody(ReadTunnelSource(),
                                        "std::string BuildNftRuleset(const FilterConfig& cfg) {");
  if (body.empty()) {
    UR_FAIL("could not read BuildNftRuleset in Tunnel.cpp");
    return;
  }
  const size_t markChain = body.find("kNftMarkChainName");
  const size_t outChain = body.find("kNftOutChainName");
  const size_t inChain = body.find("kNftInChainName");
  const size_t fwdChain = body.find("kNftFwdChainName");
  const size_t mark = body.find("ExcludeMarkRule(");
  const size_t accept = body.find("ExcludeAcceptRule()");
  const size_t masquerade = body.find("ExcludeMasqueradeRule(cfg.tun_name)");
  if (mark == std::string::npos || accept == std::string::npos ||
      masquerade == std::string::npos) {
    UR_FAIL("BuildNftRuleset does not render the exclusion's mark, accept and masquerade rules");
    return;
  }
  UR_EXPECT_TRUE_MSG("the mark rule is in urnw_mark_out", markChain < mark && mark < outChain);
  UR_EXPECT_TRUE_MSG("the accept is in urnw_out", outChain < accept && accept < inChain);
  UR_EXPECT_TRUE_MSG("the metadata drop still comes first",
                     body.find("169.254.169.254") < accept);
  UR_EXPECT_TRUE_MSG("the accept precedes the helper DNS permit",
                     accept < body.find("if (d.helper_dns)"));
  UR_EXPECT_TRUE_MSG("the accept precedes the DNS floor", accept < body.find("if (d.dns_floor)"));
  UR_EXPECT_TRUE_MSG("the accept precedes the off-tunnel v6 block",
                     accept < body.find("if (d.block_v6)"));
  UR_EXPECT_TRUE_MSG("the accept precedes the kill-switch floor",
                     accept < body.find("if (cfg.floor) line(\"\\t\\tcounter reject\");"));
  UR_EXPECT_TRUE_MSG("the masquerade is its own chain after urnw_fwd",
                     fwdChain < masquerade &&
                         body.find("kNftNatChainName") < masquerade &&
                         fwdChain < body.find("kNftNatChainName"));
  UR_EXPECT_TRUE_MSG("the masquerade needs a tun",
                     Contains(body, "if (!d.excluded.empty() && d.tun_named) {"));
  const std::string acceptGate =
      "if (!d.excluded.empty()) {\n    line(\"\\t\\t\" + ExcludeAcceptRule());";
  UR_EXPECT_TRUE_MSG("the accept needs an excluded slice", Contains(body, acceptGate));
  UR_EXPECT_TRUE_MSG("the rules come from the derived list",
                     Contains(body, "for (const auto& excluded : d.excluded)") &&
                         !Contains(body, "cfg.exclude_cgroups"));

  // The derivation: only with the socket-cgroup match, only quotable paths.
  const std::string derive =
      FunctionBody(ReadTunnelSource(), "FilterDerived DeriveFilter(const FilterConfig& cfg) {");
  const size_t gate = derive.find("if (cgroupMode == NftCgroupMode::CgroupAndMark) {");
  const size_t fill = derive.find("if (CgroupQuotable(excluded)) d.excluded.push_back(excluded);");
  UR_EXPECT_TRUE_MSG("DeriveFilter fills d.excluded only under CgroupAndMark",
                     gate != std::string::npos && fill != std::string::npos && gate < fill);
}

// An exclusion never costs the floor: absent slices are dropped before the load,
// a refused load is retried without the exclusion, and the launcher's slice list
// follows what is actually in force.
UR_TEST(netFilterApplyNeverLetsTheExclusionCostTheFloor) {
  const std::string source = ReadTunnelSource();
  const std::string applySignature =
      "bool NetFilter::Apply(const FilterConfig& requested, std::string* error) {";
  const std::string apply = FunctionBody(source, applySignature);
  if (apply.empty()) {
    UR_FAIL("could not read NetFilter::Apply in Tunnel.cpp");
    return;
  }
  UR_EXPECT_TRUE_MSG("absent slices are dropped",
                     Contains(apply, "} else if (CgroupInstallable(excluded)) {"));
  const size_t firstLoad = apply.find("if (!ApplyScript(BuildNftRuleset(cfg)");
  const size_t clear = apply.find("cfg.exclude_cgroups.clear();");
  const size_t retry = firstLoad == std::string::npos
                           ? std::string::npos
                           : apply.find("if (!ApplyScript(BuildNftRuleset(cfg)", firstLoad + 1);
  UR_EXPECT_TRUE_MSG("a refused load is retried without the exclusion",
                     firstLoad != std::string::npos && clear != std::string::npos &&
                         retry != std::string::npos && firstLoad < clear && clear < retry);
  UR_EXPECT_TRUE_MSG("the slice list follows what is in force",
                     Contains(apply, "SetExcludeState(&applied_);"));
  const std::string remove = FunctionBody(source, "bool NetFilter::Remove(std::string* error) {");
  UR_EXPECT_TRUE_MSG("a teardown clears the slice list",
                     Contains(remove, "SetExcludeState(nullptr);"));
}

// The policy rule is installed with the capture rule, both families, and every
// path that removes the capture rule removes it, by its full selector.
UR_TEST(theBypassPolicyRuleComesAndGoesWithTheCaptureRule) {
  const std::string source = ReadTunnelSource();
  const std::string install =
      FunctionBody(source, "bool Tunnel::InstallPolicyRules(TunnelError* err) {");
  const size_t capture = install.find("\"not\",");
  const size_t bypass4 = install.find("installBypass(\"-4\");");
  UR_EXPECT_TRUE_MSG("InstallPolicyRules installs the v4 bypass rule after the capture rule",
                     capture != std::string::npos && bypass4 != std::string::npos &&
                         capture < bypass4);
  UR_EXPECT_TRUE_MSG("InstallPolicyRules installs the v6 bypass rule with the v6 half",
                     Contains(install, "if (ipv6Captured_) installBypass(\"-6\");"));
  const std::string addSelector =
      "for (const auto& part : BypassRuleSelector()) add.push_back(part);";
  UR_EXPECT_TRUE_MSG("the installed rule is the selector", Contains(install, addSelector));
  const std::string remove = FunctionBody(source, "void Tunnel::RemovePolicyRules() {");
  UR_EXPECT_TRUE_MSG("RemovePolicyRules removes both families",
                     Contains(remove, "DeleteBypassRules(ip, \"-4\");") &&
                         Contains(remove, "DeleteBypassRules(ip, \"-6\");"));
  const std::string sweep =
      FunctionBody(source, "bool NetFilter::SweepStaleState(bool preserveArmed) {");
  const std::string sweepBoth = "DeleteBypassRules(ip, \"-4\") + DeleteBypassRules(ip, \"-6\")";
  UR_EXPECT_TRUE_MSG("the startup sweep removes both families", Contains(sweep, sweepBoth));
  UR_EXPECT_TRUE_MSG("the startup sweep clears the slice list",
                     Contains(sweep, "SetExcludeState(nullptr);"));
  const std::string deleter =
      FunctionBody(source, "int DeleteBypassRules(const std::string& ip, const char* family) {");
  const std::string removeSelector =
      "for (const auto& part : BypassRuleSelector()) remove.push_back(part);";
  UR_EXPECT_TRUE_MSG("a delete names the full selector, never a priority alone",
                     Contains(deleter, removeSelector));
}

// Only the tunnel owner's slice, only on the unified hierarchy, only with the
// socket-cgroup match, and kept current by the reaper.
UR_TEST(onlyTheTunnelOwnersSliceIsOfferedToTheFilter) {
  const std::string host = ReadAppSource("daemon/TunnelHost.cpp");
  if (host.empty()) {
    UR_FAIL("could not read daemon/TunnelHost.cpp");
    return;
  }
  const std::string slice =
      FunctionBody(host, "CgroupRef TunnelHost::ExcludeSliceLocked(uint64_t* id) const {");
  const std::string sliceGate =
      "if (!cgroupV2Only_ || !cgroupSocketMatchSupported_) return CgroupRef();";
  UR_EXPECT_TRUE_MSG("the slice needs the unified hierarchy and the socket-cgroup match",
                     Contains(slice, sliceGate));
  UR_EXPECT_TRUE_MSG("the slice is the owner's",
                     Contains(slice, "ExcludeSliceCgroupPath(ownerUid_.load())"));
  UR_EXPECT_TRUE_MSG("a refused slice is not offered again",
                     Contains(slice, "if (inode == excludeRefusedId_) return CgroupRef();"));
  UR_EXPECT_TRUE_MSG("the hierarchy is probed with IsCgroupV2Only",
                     Contains(host, "cgroupV2Only_ = IsCgroupV2Only(text.str());"));
  const std::string preflight =
      FunctionBody(ReadAppSource("daemon/main.cpp"), "int ReportPreflight() {");
  UR_EXPECT_TRUE_MSG("the preflight (and --diagnose) says whether the exclusion is available",
                     Contains(preflight, "urnw::IsCgroupV2Only(text.str())"));
  const std::string config = FunctionBody(host, "FilterConfig TunnelHost::FilterConfigForLocked(");
  const std::string carry = "if (slice.valid) cfg.exclude_cgroups.push_back(slice);";
  UR_EXPECT_TRUE_MSG("every installed state carries the slice",
                     Contains(config, "if (state != FilterState::Off) {") &&
                         Contains(config, carry));
  const std::string maintain = FunctionBody(host, "void TunnelHost::MaintainFilterLocked() {");
  const std::string retryGate =
      "if (excludeId != excludeAppliedId_ && --excludeRetryTicks_ <= 0) {";
  UR_EXPECT_TRUE_MSG("the reaper re-installs on a slice change",
                     Contains(maintain, "ExcludeSliceLocked(&excludeId);") &&
                         Contains(maintain, retryGate));
  const std::string control = ReadAppSource("daemon/ControlServer.cpp");
  const std::string claim =
      FunctionBody(control, "void ControlServer::ClaimTunnelOwnership(Connection* conn) {");
  UR_EXPECT_TRUE_MSG("the owner is handed to the tunnel host",
                     Contains(claim, "tunnel_.SetOwnerUid(tunnelOwnerUid_);"));
  UR_EXPECT_TRUE_MSG("a stopped tunnel has no owner",
                     Contains(control, "tunnelOwnerUid_ = -1;\n        tunnel_.SetOwnerUid(-1);"));
}

// The launcher and the daemon must agree on the slice and on the list, and the
// packages must ship the launcher.
UR_TEST(theLauncherUsesTheDaemonsSliceAndSliceList) {
  const std::string launcher = ReadAppSource("../packaging/urnetwork-exclude");
  if (launcher.empty()) {
    UR_FAIL("could not read packaging/urnetwork-exclude");
    return;
  }
  std::string slice = urnw::ExcludeSliceCgroupPath(1000);
  for (size_t at = slice.find("1000"); at != std::string::npos; at = slice.find("1000", at)) {
    slice.replace(at, 4, "${uid}");
  }
  UR_EXPECT_TRUE_MSG("the launcher waits for ExcludeSliceCgroupPath",
                     Contains(launcher, "slice=\"" + slice + "\""));
  UR_EXPECT_TRUE_MSG("the launcher runs the command in that slice",
                     Contains(launcher, "--slice=urnetwork-exclude.slice -- \"$@\""));
  const std::string tunnel = ReadTunnelSource();
  const std::string decl = "constexpr const char* kExcludeStatePath = \"";
  const size_t at = tunnel.find(decl);
  if (at == std::string::npos) {
    UR_FAIL("kExcludeStatePath is not declared in Tunnel.cpp");
    return;
  }
  const size_t begin = at + decl.size();
  const std::string statePath = tunnel.substr(begin, tunnel.find('"', begin) - begin);
  UR_EXPECT_TRUE_MSG("the launcher reads the daemon's slice list",
                     Contains(launcher, "URNETWORK_EXCLUDE_STATE:-" + statePath + "}"));
  const std::string common = ReadAppSource("../../packaging/lib/common.sh");
  const std::string copy = "cp \"${src}/urnetwork-exclude\" \"${root}/usr/bin/urnetwork-exclude\"";
  UR_EXPECT_TRUE_MSG("the daemon packages install it as /usr/bin/urnetwork-exclude",
                     Contains(common, copy));
  // The one chmod 0755 line names the launcher beside the daemon.
  const std::string modes =
      "\"${root}/usr/bin/urnetwork-exclude\" \"${root}/usr/lib/urnetwork/urnetworkd\"";
  UR_EXPECT_TRUE_MSG("the installed launcher is executable", Contains(common, modes));
}

// ---- the egress witness's call sites ----------------------------------------

namespace {

size_t CountOf(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

}  // namespace

// Both treatment legs read their socket through the one verdict, so neither can
// drift back to its own reading of a socket with no route; only the bring-up
// gate demands somewhere to go, after leg 3, under its own code.
UR_TEST(bothWitnessTreatmentLegsUseTheVerdictAndOnlyBringUpDemandsARoute) {
  const std::string tunnel = ReadTunnelSource();
  const std::string witness = FunctionBody(tunnel, "bool Tunnel::VerifyEgressWitness(");
  if (witness.empty()) {
    UR_FAIL("Tunnel::VerifyEgressWitness was not found in Tunnel.cpp");
    return;
  }
  UR_EXPECT_TRUE_MSG("both families' treatment sockets go through WitnessTreatmentVerdict",
                     CountOf(witness, "WitnessTreatmentVerdict(") == 2);
  UR_EXPECT_TRUE_MSG("the v4 leg reads its socket through the verdict",
                     Contains(witness, "WitnessTreatmentVerdict(daemonBound, daemon.source, "
                                       "daemon.connect_errno,"));
  UR_EXPECT_TRUE_MSG("the v6 leg reads its socket through the verdict",
                     Contains(witness, "WitnessTreatmentVerdict(daemon6Bound, daemon6.source, "
                                       "daemon6.connect_errno,"));
  // The v6 leg's old private no-route pass, which never looked at the mark.
  UR_EXPECT_FALSE(Contains(witness, "daemon6.connect_errno == ENETUNREACH"));
  // Leg 3 (a marked packet in the tun) outranks "no network".
  const size_t legThree = witness.find("if (counters.tun != 0) {");
  const size_t demand = witness.find("if (requireOffTunnelRoute && !offTunnelRoute) {");
  UR_EXPECT_TRUE(legThree != std::string::npos && demand != std::string::npos &&
                 legThree < demand);
  UR_EXPECT_TRUE(Contains(witness, "err->code = kCodeNoNetwork;"));
  UR_EXPECT_TRUE(Contains(tunnel, "constexpr const char* kCodeNoNetwork = \"no_network\";"));
  UR_EXPECT_TRUE(Contains(ReadAppSource("ControlProtocol.hpp"),
                          "inline constexpr const char* kCodeNoNetwork = \"no_network\";"));
  UR_EXPECT_TRUE_MSG("the bring-up gate demands a route off the tunnel",
                     Contains(tunnel, "VerifyEgressWitness(\"bring-up\", "
                                      "/*requireOffTunnelRoute=*/true, err)"));
  const std::string host = ReadAppSource("daemon/TunnelHost.cpp");
  UR_EXPECT_TRUE_MSG("the post-connect gate accepts a machine between networks",
                     Contains(host, "VerifyEgressWitness(\"connected\", "
                                    "/*requireOffTunnelRoute=*/false,"));
  UR_EXPECT_TRUE_MSG("the re-check accepts a machine between networks",
                     Contains(host, "VerifyEgressWitness(\"recheck\", "
                                    "/*requireOffTunnelRoute=*/false,"));
  UR_EXPECT_TRUE(CountOf(tunnel + host, "VerifyEgressWitness(\"") == 3);
}

// ---- the direct-file DNS tier across a network switch ----------------------

UR_TEST(resolvConfNameserversAreReadTheWayGlibcReadsThem) {
  const std::string content =
      "# Generated by NetworkManager\n"
      "search lan\n"
      "nameserver 192.168.12.1\n"
      "nameserver\tfe80::1%2\n"
      "nameserver 10.0.0.1 trailing words ignored\r\n"
      "; nameserver 1.1.1.1\n"
      "# nameserver 8.8.8.8\n"
      " nameserver 9.9.9.9\n"
      "nameserver10.0.0.2\n"
      "nameserver\n"
      "nameserver   \n";
  const std::vector<std::string> servers = urnw::ResolvConfNameservers(content);
  const std::vector<std::string> expected = {"192.168.12.1", "fe80::1%2", "10.0.0.1"};
  UR_EXPECT_TRUE(servers == expected);
  UR_EXPECT_TRUE(urnw::ResolvConfNameservers("").empty());
  UR_EXPECT_TRUE(urnw::ResolvConfNameservers("nameserver 1.1.1.1").size() == 1);
}

UR_TEST(aReapplyAdoptsOnlyAnotherWritersUsableFile) {
  using urnw::ResolvConfReapply;
  const std::string marker =
      "# Managed by urnetworkd (URnetwork): the tunnel owns this file while it is up.";
  const std::vector<std::string> tunnelResolvers = {"169.254.169.253", "fd00::53"};
  // Ours: never backed up, or the user's original is lost (the double takeover).
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply(marker + "\nnameserver 169.254.169.253\n", marker,
                                               tunnelResolvers) == ResolvConfReapply::KeepBackup);
  // The marker anywhere in the file is ours, whatever else it names.
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply("nameserver 192.168.1.1\n" + marker + "\n", marker,
                                               tunnelResolvers) == ResolvConfReapply::KeepBackup);
  // NetworkManager's rewrite at a network switch: the newest truth, kept.
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply(
                     "# Generated by NetworkManager\nnameserver 10.20.0.1\n", marker,
                     tunnelResolvers) == ResolvConfReapply::AdoptAsBackup);
  // A writer caught mid-write, or a file with nothing in it to resolve with.
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply("", marker, tunnelResolvers) ==
                 ResolvConfReapply::KeepBackup);
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply("# Generated by NetworkManager\nsearch lan\n",
                                               marker,
                                               tunnelResolvers) == ResolvConfReapply::KeepBackup);
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply("nameserver10.0.0.1\n", marker,
                                               tunnelResolvers) == ResolvConfReapply::KeepBackup);
  // A copy of ours without the marker would put the tunnel's resolver back
  // after the tunnel is gone.
  UR_EXPECT_TRUE(urnw::DecideResolvConfReapply("nameserver 10.20.0.1\nnameserver fd00::53\n",
                                               marker,
                                               tunnelResolvers) == ResolvConfReapply::KeepBackup);
}

UR_TEST(theDnsRepairFollowsANetworkSwitchWithoutChurningTheFloor) {
  const std::string host = ReadAppSource("daemon/TunnelHost.cpp");
  const std::string maintain = FunctionBody(host, "void TunnelHost::MaintainDnsLocked() {");
  if (maintain.empty()) {
    UR_FAIL("TunnelHost::MaintainDnsLocked was not found");
    return;
  }
  // Every tick for the file tiers while nothing fails and nothing contests the
  // file; the slower interval otherwise: both from the host's settings, through
  // the decisions HostSettings.hpp tests.
  UR_EXPECT_TRUE(Contains(maintain, "const int verifyInterval = DnsVerifyIntervalTicks("));
  UR_EXPECT_TRUE(Contains(
      maintain, "/*forkingCheck=*/tunnel_->dnsBackend() == DnsBackend::SystemdResolved"));
  UR_EXPECT_TRUE(Contains(maintain, "/*repairFailing=*/dnsVerifyFailures_ != 0, dnsContested_"));
  UR_EXPECT_TRUE(
      Contains(maintain, "dnsContested_ = DnsRepairContested(settings_, dnsTicksSinceRepair_);"));
  UR_EXPECT_TRUE(Contains(maintain, "if (++dnsVerifyTicks_ < verifyInterval) return;"));
  UR_EXPECT_TRUE(Contains(maintain, "dnsVerifyFailures_ < settings_.dns_repair_attempts"));
  // A repair that changed nothing the floor pins returns before the nft swap;
  // the decision reads the floor actually in force.
  const size_t current = maintain.find("const bool floorCurrent =");
  const size_t skip = maintain.find("floorCurrent) {", current);
  const size_t rebuild = maintain.find("ApplyFilterLocked(FilterState::Connected, &ignored);");
  UR_EXPECT_TRUE(current != std::string::npos && skip != std::string::npos &&
                 rebuild != std::string::npos && skip < rebuild);
  UR_EXPECT_TRUE(
      Contains(maintain, "inForce.block_offtunnel_dns == tunnel_->report().dns_applied"));
  UR_EXPECT_TRUE(Contains(maintain, "inForce.tunnel_resolvers == tunnel_->resolvers()"));

  // The direct-file re-apply decides on the file it finds before it overwrites
  // it, and writes an adopted backup's bytes before the record naming them.
  const std::string direct =
      FunctionBody(ReadTunnelSource(), "bool Tunnel::ApplyDnsViaDirectFile(");
  const size_t decide =
      direct.find("DecideResolvConfReapply(current, kResolvConfMarker, dnsServers_)");
  const size_t backup = direct.find("WriteFileAtomic(kResolvConfBackupPath, current, next.mode,");
  const size_t record = direct.find("WriteTakeoverState(next, &adoptError)");
  const size_t rewrite = direct.find("WriteFileAtomic(kResolvConfPath, content, mode,");
  UR_EXPECT_TRUE(decide != std::string::npos && backup != std::string::npos &&
                 record != std::string::npos && rewrite != std::string::npos);
  UR_EXPECT_TRUE(decide < backup && backup < record && record < rewrite);
}

// ---- the network-switch path's timings (daemon/HostSettings.hpp) ------------

// The defaults, read where they are set, and the watchers' own defaults carried
// in unchanged.
UR_TEST(hostSettingsDefaultsArePinned) {
  const urnw::HostSettings settings;
  UR_EXPECT_EQ(1u, settings.reaper_interval_seconds);
  UR_EXPECT_EQ(5, settings.filter_verify_interval_ticks);
  UR_EXPECT_EQ(12, settings.filter_verify_log_every);
  UR_EXPECT_EQ(1, settings.dns_file_verify_interval_ticks);
  UR_EXPECT_EQ(10, settings.dns_verify_interval_ticks);
  UR_EXPECT_EQ(10, settings.dns_contested_window_ticks);
  UR_EXPECT_EQ(3, settings.dns_repair_attempts);
  UR_EXPECT_EQ(30, settings.egress_witness_interval_ticks);
  UR_EXPECT_EQ(2, settings.egress_witness_failures_to_stop);
  UR_EXPECT_EQ(urnw::LinuxNetworkPathSettings{}.holdoff_millis,
               settings.network_path.holdoff_millis);
  UR_EXPECT_EQ(urnw::LinuxSuspendSettings{}.threshold_millis, settings.suspend.threshold_millis);
  UR_EXPECT_EQ(urnw::watchdog::DeadTunnelSettings{}.deadSlowMillis,
               settings.dead_tunnel.deadSlowMillis);
  UR_EXPECT_EQ(urnw::watchdog::DeadTunnelSettings{}.sdkSampleIntervalMillis,
               settings.dead_tunnel.sdkSampleIntervalMillis);
  // The reaper ticks the failsafe at the interval the failsafe counts in.
  UR_EXPECT_EQ(static_cast<int64_t>(settings.reaper_interval_seconds) * 1000,
               settings.dead_tunnel.evaluateIntervalMillis);
}

// The reaper's interval, and which failures of the nftables re-check are
// logged, with what 0 means for each: a reaper interval of 0 is one second
// (a reaper that never waited would spin the main loop), and a log rate of 0
// or less logs every failure (and never divides by it).
UR_TEST(hostSettingsDecideTheReaperAndFilterCadence) {
  const urnw::HostSettings defaults;
  UR_EXPECT_EQ(1u, urnw::ReaperIntervalSeconds(defaults));
  urnw::HostSettings slow;
  slow.reaper_interval_seconds = 3;
  UR_EXPECT_EQ(3u, urnw::ReaperIntervalSeconds(slow));
  slow.reaper_interval_seconds = 0;
  UR_EXPECT_EQ(1u, urnw::ReaperIntervalSeconds(slow));

  // The first failure of a run, then one in twelve.
  int logged = 0;
  for (int failures = 0; failures < 36; ++failures) {
    if (urnw::FilterFailureLogged(defaults, failures)) ++logged;
  }
  UR_EXPECT_EQ(3, logged);
  UR_EXPECT_TRUE(urnw::FilterFailureLogged(defaults, 0));
  UR_EXPECT_FALSE(urnw::FilterFailureLogged(defaults, 11));
  UR_EXPECT_TRUE(urnw::FilterFailureLogged(defaults, 12));
  urnw::HostSettings chatty;
  chatty.filter_verify_log_every = 3;
  UR_EXPECT_TRUE(urnw::FilterFailureLogged(chatty, 3));
  UR_EXPECT_FALSE(urnw::FilterFailureLogged(chatty, 4));
  for (const int every : {0, 1, -4}) {
    chatty.filter_verify_log_every = every;
    for (int failures = 0; failures < 5; ++failures) {
      if (!urnw::FilterFailureLogged(chatty, failures)) {
        UR_FAIL("a log rate of " + std::to_string(every) + " skipped a failure");
      }
    }
  }
}

// When the next DNS check runs: every tick for a healthy, uncontested file
// check, the slower interval for anything else; and the same decisions under
// settings that are not the defaults.
UR_TEST(hostSettingsDecideTheDnsCadence) {
  struct Row {
    bool forking;
    bool failing;
    bool contested;
    int ticks;
  };
  const urnw::HostSettings defaults;
  const Row rows[] = {
      {false, false, false, 1},  {true, false, false, 10}, {false, true, false, 10},
      {false, false, true, 10},  {true, true, true, 10},
  };
  int index = 0;
  for (const Row& row : rows) {
    if (urnw::DnsVerifyIntervalTicks(defaults, row.forking, row.failing, row.contested) !=
        row.ticks) {
      UR_FAIL("dns cadence row " + std::to_string(index) + " read wrong");
    }
    ++index;
  }
  UR_EXPECT_TRUE(urnw::DnsRepairContested(defaults, 9));
  UR_EXPECT_FALSE(urnw::DnsRepairContested(defaults, 10));

  urnw::HostSettings custom;
  custom.dns_file_verify_interval_ticks = 3;
  custom.dns_verify_interval_ticks = 60;
  custom.dns_contested_window_ticks = 120;
  UR_EXPECT_EQ(3, urnw::DnsVerifyIntervalTicks(custom, false, false, false));
  UR_EXPECT_EQ(60, urnw::DnsVerifyIntervalTicks(custom, true, false, false));
  UR_EXPECT_TRUE(urnw::DnsRepairContested(custom, 119));
  UR_EXPECT_FALSE(urnw::DnsRepairContested(custom, 120));
}

// The host reads every one of them from the settings it was built with, and
// none of the constants they replaced survives anywhere on this path.
UR_TEST(theNetworkSwitchPathReadsItsTimingsFromSettings) {
  const std::string hostHeader = ReadAppSource("daemon/TunnelHost.hpp");
  UR_EXPECT_TRUE(Contains(hostHeader, "explicit TunnelHost(std::string storageRoot, "
                                      "HostSettings settings = HostSettings());"));
  UR_EXPECT_TRUE(Contains(hostHeader, "const HostSettings settings_;"));
  UR_EXPECT_TRUE(Contains(hostHeader, "deadTunnelWatch_{settings_.dead_tunnel};"));
  UR_EXPECT_TRUE(Contains(hostHeader, "networkQualityTracker_{settings_.network_path};"));
  UR_EXPECT_TRUE(Contains(hostHeader, "suspendDetector_{settings_.suspend};"));
  // Built first, so every member that takes a timing is built from it.
  UR_EXPECT_TRUE(hostHeader.find("const HostSettings settings_;") <
                 hostHeader.find("std::string storageRoot_;"));

  const std::string host = ReadAppSource("daemon/TunnelHost.cpp");
  const std::string witness = FunctionBody(host, "bool TunnelHost::CheckEgressWitnessLocked() {");
  UR_EXPECT_TRUE(
      Contains(witness, "if (++egressWitnessTicks_ < settings_.egress_witness_interval_ticks)"));
  UR_EXPECT_TRUE(
      Contains(witness, "StepEgressRecheck(settings_, passed, &egressWitnessFailures_)"));
  UR_EXPECT_TRUE(Contains(ReadAppSource("daemon/HostSettings.hpp"),
                          "*failures < settings.egress_witness_failures_to_stop"));
  const std::string message = FunctionBody(host, "std::string DeadTunnelMessage(");
  UR_EXPECT_TRUE(Contains(message, "seconds(settings.deadFastMillis)"));
  UR_EXPECT_TRUE(Contains(message, "seconds(settings.deadSlowMillis)"));
  UR_EXPECT_TRUE(Contains(message, "seconds(settings.sdkUnresponsiveMillis)"));
  const std::string watchStart =
      FunctionBody(host, "void TunnelHost::StartDeadTunnelWatchLocked() {");
  UR_EXPECT_TRUE(Contains(watchStart, "deadTunnelWatch_.settings().sdkUnresponsiveMillis / 1000"));
  // The reaper's own cadence, and the nftables re-check it runs.
  UR_EXPECT_TRUE(Contains(host, "g_timeout_add_seconds(ReaperIntervalSeconds(settings_), "
                                "&TunnelHost::OnReaperTick, this);"));
  const std::string filter = FunctionBody(host, "void TunnelHost::MaintainFilterLocked() {");
  UR_EXPECT_TRUE(Contains(filter, "excludeRetryTicks_ = settings_.filter_verify_interval_ticks;"));
  UR_EXPECT_TRUE(
      Contains(filter, "if (FilterFailureLogged(settings_, excludeRetryFailures_++)) {"));
  UR_EXPECT_TRUE(Contains(filter, "if (++filterVerifyTicks_ < "
                                  "settings_.filter_verify_interval_ticks) return;"));
  UR_EXPECT_TRUE(Contains(filter, "const bool loud = FilterFailureLogged(settings_, "
                                  "filterVerifyFailures_);"));

  const std::string watchdog = ReadAppSource("TunnelWatchdog.hpp");
  const std::string quality = ReadAppSource("NetworkQuality.hpp");
  const std::string hostSettings = ReadAppSource("daemon/HostSettings.hpp");
  const std::string sampler =
      ReadAppSource("daemon/ExitSampler.hpp") + ReadAppSource("daemon/ExitSampler.cpp");
  UR_EXPECT_TRUE(!hostSettings.empty() && !sampler.empty());
  for (const char* retired :
       {"kDeadFastMillis", "kDeadSlowMillis", "kDeadFastOutboundPackets", "kOfflineRebaseMillis",
        "kLinuxPathHoldoffMillis", "kLinuxSuspendThresholdMillis", "kDnsVerifyIntervalSeconds",
        "kDnsRepairAttempts", "kEgressWitnessIntervalSeconds", "kSdkUnresponsiveMillis",
        "kSdkUnresponsiveArmMillis", "kFailsafeNoticeMillis", "kSdkSampleIntervalMillis",
        "kEvaluateIntervalMillis", "kEvaluatorFrozenMillis", "kReaperIntervalSeconds",
        "kFilterVerifyIntervalSeconds", "kFilterVerifyLogEvery"}) {
    for (const std::string* source : {&host, &hostHeader, &watchdog, &quality, &hostSettings,
                                      &sampler}) {
      if (Contains(*source, retired)) {
        UR_FAIL(std::string(retired) + " is back: its value belongs to a settings struct");
      }
    }
  }
  UR_EXPECT_TRUE(Contains(watchdog, "tick.verdict = Evaluate(s, now, settings_);"));
  UR_EXPECT_TRUE(Contains(watchdog, "if (EvaluatorFroze(tick.tickGapMillis, settings_)) {"));
  UR_EXPECT_TRUE(Contains(watchdog, "sampleAge >= settings.sdkUnresponsiveMillis"));
  UR_EXPECT_TRUE(Contains(watchdog, "sampleAge >= settings.sdkUnresponsiveArmMillis"));
  UR_EXPECT_TRUE(Contains(watchdog, "soonest <= settings.failsafeNoticeMillis"));
  // The freeze bar's default is five evaluate ticks, in the struct itself.
  UR_EXPECT_TRUE(
      Contains(watchdog, "int64_t evaluatorFrozenMillis = 5 * evaluateIntervalMillis;"));
  UR_EXPECT_TRUE(Contains(quality, "holdUntilMillis_ = nowMillis + settings_.holdoff_millis;"));
  UR_EXPECT_TRUE(Contains(quality, "suspendedMillis - last_ >= settings_.threshold_millis;"));
}
