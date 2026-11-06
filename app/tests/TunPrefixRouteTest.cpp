// The tun's address carries no prefix route (TunnelPolicy.hpp, TunAddressArgv).
// An ordinary `ip address replace 10.0.0.89/24 dev urnet0` puts
// `10.0.0.0/24 dev urnet0` in MAIN, the table the egress mark sends the
// daemon's own sockets to, so its traffic to anything in that /24 entered the
// tun, and the egress witness's re-check counted it and stopped the session a
// minute after every connect. Measured in a network namespace (kernel 6.18):
//
//   plain address:    ip route get 10.0.0.5 mark 0x55524e57  ->  dev urnet0
//   noprefixroute:    ip route get 10.0.0.5 mark 0x55524e57  ->  via the gateway
//   replace in place: keeps the old flags; the route returns with the link
//
// tests/tun-prefix-route-netns.sh repeats those measurements on a real kernel
// when run as root. This file holds the argv, the reading of `ip route show`,
// and the call sites in Tunnel::Configure and the witness.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TunnelPolicy.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

bool ArgvHas(const std::vector<std::string>& argv, const std::string& word) {
  for (const auto& w : argv) {
    if (w == word) return true;
  }
  return false;
}

// The source with every // comment blanked; string literals are kept, so prose
// cannot satisfy a contract.
std::string ReadPrefixRouteSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  bool inString = false;
  for (size_t at = 0; at < text.size(); ++at) {
    const char c = text[at];
    if (inString) {
      if (c == '\\') {
        ++at;
      } else if (c == '"' || c == '\n') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
      while (at < text.size() && text[at] != '\n') text[at++] = ' ';
    }
  }
  return text;
}

std::string PrefixRouteBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool PrefixRouteInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(TunPrefixRoute_TheV4AddressCarriesNoPrefixRoute) {
  const std::vector<std::string> argv =
      urnw::TunAddressArgv("/usr/sbin/ip", false, "10.0.0.89/24", "urnet0");
  const std::vector<std::string> expected = {"/usr/sbin/ip", "-4",     "address",
                                             "replace",      "10.0.0.89/24", "dev",
                                             "urnet0",       "noprefixroute"};
  UR_EXPECT_TRUE(argv == expected);
  UR_EXPECT_FALSE(ArgvHas(argv, "nodad"));
}

UR_TEST(TunPrefixRoute_TheV6AddressCarriesNoPrefixRouteAndSkipsDad) {
  const std::vector<std::string> argv =
      urnw::TunAddressArgv("ip", true, "fd00:7572:6e65:1::2/64", "urnet0");
  UR_EXPECT_TRUE(std::string("-6") == argv[1]);
  UR_EXPECT_TRUE(std::string("fd00:7572:6e65:1::2/64") == argv[4]);
  UR_EXPECT_TRUE(ArgvHas(argv, "noprefixroute"));
  UR_EXPECT_TRUE(ArgvHas(argv, "nodad"));
}

// The listing exactly as `ip -4 route show table main dev urnet0` printed it on
// the namespace above, and the v6 shapes.
UR_TEST(TunPrefixRoute_MainRoutesAreReadAsPrinted) {
  UR_EXPECT_TRUE(urnw::MainRoutesIntoTunnel("").empty());
  UR_EXPECT_TRUE(urnw::MainRoutesIntoTunnel("\n\n").empty());

  const auto v4 = urnw::MainRoutesIntoTunnel(
      "10.0.0.0/24 proto kernel scope link src 10.0.0.89 linkdown \n");
  UR_EXPECT_EQ(static_cast<size_t>(1), v4.size());
  if (v4.size() == 1) {
    UR_EXPECT_TRUE(std::string("10.0.0.0/24 proto kernel scope link src 10.0.0.89 linkdown") == v4[0]);
  }

  // fe80::/64 cannot carry an unbound socket; the ULA prefix can.
  const auto v6 = urnw::MainRoutesIntoTunnel(
      "fd00:7572:6e65:1::/64 proto kernel metric 256 pref medium\n"
      "fe80::/64 proto kernel metric 256 pref medium\n");
  UR_EXPECT_EQ(static_cast<size_t>(1), v6.size());
  if (v6.size() == 1) {
    UR_EXPECT_TRUE(std::string("fd00:7572:6e65:1::/64 proto kernel metric 256 pref medium") == v6[0]);
  }

  // Something else's route through our tun counts too: it is still in main.
  UR_EXPECT_EQ(static_cast<size_t>(2),
               urnw::MainRoutesIntoTunnel("10.0.0.0/24 proto kernel scope link src 10.0.0.89\n"
                                          "192.0.2.0/24 scope link\n")
                   .size());
}

// Configure flushes a stale address, adds both halves through TunAddressArgv,
// and removes any main-table route through the tun after `up` (when the kernel
// would add one) and before the policy rule sends anything there.
UR_TEST(TunPrefixRoute_ConfigureAddsTheAddressWithoutAPrefixRoute) {
  const std::string tunnel = ReadPrefixRouteSource("Tunnel.cpp");
  const std::string configure = PrefixRouteBody(tunnel, "bool Tunnel::Configure(");
  UR_EXPECT_FALSE(configure.empty());
  UR_EXPECT_TRUE(PrefixRouteInOrder(
      configure, {"RunCommand({ip, \"-4\", \"address\", \"flush\", \"dev\", name_});",
                  "TunAddressArgv(ip, /*ipv6=*/false, addr, name_)",
                  "steps.push_back(TunAddressArgv(ip, /*ipv6=*/true, addr6, name_));",
                  "steps.push_back({ip, \"link\", \"set\", \"dev\", name_, \"up\"});",
                  "for (const auto& step : steps)", "DropMainRoutesThroughTun(ip, \"-4\");",
                  "DropMainRoutesThroughTun(ip, \"-6\");", "InstallPolicyRules(err)"}));
  // No other address step, so nothing adds the tun's address with a prefix route.
  UR_EXPECT_TRUE(tunnel.find("\"address\", \"replace\"") == std::string::npos);
  UR_EXPECT_TRUE(tunnel.find("\"address\", \"add\"") == std::string::npos);
  UR_EXPECT_TRUE(tunnel.find("\"addr\", \"add\"") == std::string::npos);
  UR_EXPECT_TRUE(tunnel.find("\"addr\", \"replace\"") == std::string::npos);
}

// The re-check names the routes that explain a nonzero tun counter.
UR_TEST(TunPrefixRoute_TheWitnessNamesMainRoutesThroughTheTun) {
  const std::string tunnel = ReadPrefixRouteSource("Tunnel.cpp");
  const std::string witness = PrefixRouteBody(tunnel, "bool Tunnel::VerifyEgressWitness(");
  UR_EXPECT_TRUE(PrefixRouteInOrder(
      witness, {"if (counters.tun != 0) {", "MainRoutesThroughTun(ip, \"-4\")",
                "MainRoutesThroughTun(ip, \"-6\")", "return fail(kCodeEgressUnprotected, message);"}));
  const std::string drop = PrefixRouteBody(tunnel, "void Tunnel::DropMainRoutesThroughTun(");
  UR_EXPECT_TRUE(PrefixRouteInOrder(
      drop, {"MainRoutesThroughTun(ip, family)",
             "{ip, family, \"route\", \"flush\", \"table\", \"main\", \"dev\", name_}"}));
}
