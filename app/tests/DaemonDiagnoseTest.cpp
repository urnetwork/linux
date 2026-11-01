// What urnetworkd --diagnose shows to explain an egress refusal
// (daemon/DiagnoseReport.hpp), with a fake `ip`:
//
//   * the policy rules, routes and addresses the daemon's own sockets meet,
//     IPv4 and IPv6, and a pointer to the self-test for the one question a read
//     cannot answer. It used to show none of it, so an IPv6 egress refusal
//     could not be explained from its output;
//   * it only reads, and it prints no address.
//
// Host shapes are made up, with documentation addresses.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <map>
#include <string>
#include <vector>

#include "Tunnel.hpp"
#include "daemon/DiagnoseReport.hpp"

namespace {

using urnw::CommandResult;

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

CommandResult Printed(const std::string& output) {
  CommandResult r;
  r.spawned = true;
  r.exit_code = 0;
  r.output = output;
  return r;
}

CommandResult Exited(int exitCode, const std::string& output) {
  CommandResult r;
  r.spawned = true;
  r.exit_code = exitCode;
  r.output = output;
  return r;
}

// `ip`'s answers by the argv after the binary; an argv nobody set prints
// nothing. Every call is kept.
struct FakeIp {
  std::map<std::string, CommandResult> answers;
  std::vector<std::vector<std::string>> calls;

  void Set(const std::string& args, const CommandResult& answer) { answers[args] = answer; }

  urnw::diagnose::CommandRunner Runner() {
    return [this](const std::vector<std::string>& argv) {
      calls.push_back(argv);
      std::string args;
      for (size_t i = 1; i < argv.size(); ++i) args += (i > 1 ? " " : "") + argv[i];
      const auto it = answers.find(args);
      return it == answers.end() ? Printed("") : it->second;
    };
  }
};

constexpr const char* kIp = "/usr/sbin/ip";
constexpr const char* kSelf = "/usr/local/lib/urnetwork/urnetworkd";

// A dual-stack laptop on Wi-Fi with no tunnel up: IPv4 from DHCP, and an IPv6
// default route from a router advertisement on an interface with no global
// IPv6 address. Something else on the host keeps a rule at 5270.
FakeIp LaptopWithoutAGlobalIpv6Address() {
  FakeIp ip;
  ip.Set("-4 rule show", Printed("0:\tfrom all lookup local\n"
                                 "32766:\tfrom all lookup main\n"
                                 "32767:\tfrom all lookup default\n"));
  ip.Set("-4 route show table 51821",
         Exited(2, "Error: ipv4: FIB table does not exist.\nDump terminated\n"));
  ip.Set("-4 route show default table all",
         Printed("default via 192.0.2.1 dev wlan0 proto dhcp src 192.0.2.23 metric 600\n"));
  ip.Set("-4 -o addr show scope global",
         Printed("3: wlan0    inet 192.0.2.23/24 brd 192.0.2.255 scope global dynamic "
                 "noprefixroute wlan0\\       valid_lft 86000sec preferred_lft 86000sec\n"));
  ip.Set("-6 rule show", Printed("0:\tfrom all lookup local\n"
                                 "5270:\tfrom all lookup 52\n"
                                 "32766:\tfrom all lookup main\n"));
  ip.Set("-6 route show default table all",
         Printed("default via fe80::1 dev wlan0 proto ra metric 600 pref medium\n"));
  ip.Set("-6 -o addr show scope global", Printed(""));
  return ip;
}

std::string Report(FakeIp& ip, bool hostIpv6 = true, const std::string& detail = "") {
  return urnw::diagnose::RoutingReport(kIp, hostIpv6, detail, kSelf, ip.Runner());
}

// The IPv6 section, to the end of the report.
std::string Ipv6Section(const std::string& report) {
  const size_t at = report.find("\nIPv6\n");
  return at == std::string::npos ? std::string() : report.substr(at);
}

// One address-count row as printed: six spaces, the name, then the count in
// the column at 22 (an interface name is at most 15 bytes).
std::string CountRow(const std::string& name, const std::string& rest) {
  return std::string(6, ' ') + name + std::string(16 - name.size(), ' ') + rest + "\n";
}

}  // namespace

// ---- --diagnose: the routing policy the daemon's own sockets meet ----------

// THE GAP: an IPv6 refusal needs the IPv6 rules, the capture table, the default
// routes and the addresses source selection can choose from, and the pointer
// to the self-test for the mark on the daemon's IPv6 sockets.
UR_TEST(DaemonDiagnose_TheIpv6SectionShowsWhatTheDaemonsSocketsMeet) {
  FakeIp ip = LaptopWithoutAGlobalIpv6Address();
  const std::string report = Report(ip);
  const std::string v6 = Ipv6Section(report);
  UR_EXPECT_TRUE_MSG(report, !v6.empty());
  UR_EXPECT_TRUE_MSG(v6, Has(v6, "IPv6 is on: a tunnel adds its IPv6 half"));
  UR_EXPECT_TRUE(Has(v6, "  rules (ip -6 rule show):\n"
                         "      0:\tfrom all lookup local\n"
                         "      5270:\tfrom all lookup 52\n"
                         "      32766:\tfrom all lookup main\n"));
  UR_EXPECT_TRUE(Has(v6, "  the tunnel's capture table (ip -6 route show table 51821):\n"
                         "      none: no tunnel is up, so its capture routes are not installed\n"));
  UR_EXPECT_TRUE(Has(v6, "  default routes (ip -6 route show default table all):\n"
                         "      default via fe80::1 dev wlan0 proto ra metric 600 pref medium\n"));
  UR_EXPECT_TRUE_MSG(v6, Has(v6, CountRow("wlan0", "0  carries a default route")));
  UR_EXPECT_TRUE(Has(v6, "      sudo /usr/local/lib/urnetwork/urnetworkd --selftest-egress\n"));
  UR_EXPECT_TRUE(Has(v6, "  In step 7 of its output, both AF_INET6 lines must say \"marked\".\n"));
  // The rule the tunnel would add, named from the constants it is added from.
  UR_EXPECT_TRUE_MSG(report, Has(report, "      32763:  not from all fwmark 0x55524e57 lookup 51821\n"));
  UR_EXPECT_TRUE(Has(report, "      32761:  from all fwmark 0x55524e58 lookup main"));
}

// The host shape behind "bound to the tunnel's own address" with the mark in
// place: the IPv6 default route leaves by an interface with no global address,
// so source selection takes another interface's, the tunnel's once it is up.
UR_TEST(DaemonDiagnose_AnUplinkWithoutAGlobalAddressIsNamed) {
  FakeIp bare = LaptopWithoutAGlobalIpv6Address();
  const std::string v6 = Ipv6Section(Report(bare));
  UR_EXPECT_TRUE_MSG(v6, Has(v6, "      wlan0 carries a default route but has no usable global "
                                 "address: a socket\n"));
  UR_EXPECT_TRUE(Has(v6, "the tunnel's own (the egress witness\n      then reads it as captured)\n"));

  // The same laptop once its address is there, and one still in DAD.
  FakeIp addressed = LaptopWithoutAGlobalIpv6Address();
  addressed.Set("-6 -o addr show scope global",
                Printed("3: wlan0    inet6 2001:db8:5::7/64 scope global dynamic noprefixroute "
                        "\\       valid_lft 86000sec preferred_lft 14000sec\n"));
  const std::string fine = Ipv6Section(Report(addressed));
  UR_EXPECT_TRUE_MSG(fine, Has(fine, CountRow("wlan0", "1  carries a default route")));
  UR_EXPECT_FALSE(Has(fine, "has no usable global address"));

  FakeIp tentative = LaptopWithoutAGlobalIpv6Address();
  tentative.Set("-6 -o addr show scope global",
                Printed("3: wlan0    inet6 2001:db8:5::7/64 scope global tentative dynamic "
                        "\\       valid_lft 86000sec preferred_lft 14000sec\n"));
  UR_EXPECT_TRUE(Has(Ipv6Section(Report(tentative)), "wlan0 carries a default route but has no "
                                                     "usable global address"));
  // The IPv4 half of that laptop has its address: no note there.
  UR_EXPECT_FALSE(Has(Report(bare).substr(0, Report(bare).find("\nIPv6\n")),
                      "has no usable global address"));
}

// Counted, never printed: the addresses source selection chooses from are a
// person's, and a count answers the question.
UR_TEST(DaemonDiagnose_AddressesAreCountedNeverPrinted) {
  FakeIp ip = LaptopWithoutAGlobalIpv6Address();
  ip.Set("-6 -o addr show scope global",
         Printed("2: eth0    inet6 2001:db8:5::7/64 scope global dynamic \\       valid_lft "
                 "86000sec preferred_lft 14000sec\n"
                 "2: eth0    inet6 2001:db8:5::8/64 scope global temporary dynamic \\       "
                 "valid_lft 86000sec preferred_lft 14000sec\n"
                 "9: wg0    inet6 fd00:abcd::2/64 scope global \\       valid_lft forever "
                 "preferred_lft forever\n"));
  ip.Set("-4 -o addr show scope global",
         Printed("3: wlan0    inet 198.51.100.7/24 brd 198.51.100.255 scope global wlan0\\   "
                 "    valid_lft forever preferred_lft forever\n"));
  const std::string report = Report(ip);
  for (const char* address : {"2001:db8:5::7", "2001:db8:5::8", "fd00:abcd::2", "198.51.100.7"}) {
    UR_EXPECT_TRUE_MSG(address, !Has(report, address));
  }
  UR_EXPECT_TRUE_MSG(report, Has(report, CountRow("eth0", "2")));
  UR_EXPECT_TRUE(Has(report, CountRow("wg0", "1")));
  UR_EXPECT_TRUE(Has(report, CountRow("wlan0", "1  carries a default route")));
}

// Read-only: `ip ... show`, each once, and nothing that adds, deletes,
// flushes or replaces.
UR_TEST(DaemonDiagnose_OnlyReads) {
  FakeIp ip = LaptopWithoutAGlobalIpv6Address();
  Report(ip);
  UR_EXPECT_EQ(8, static_cast<int>(ip.calls.size()));
  std::map<std::string, int> seen;
  for (const auto& argv : ip.calls) {
    std::string args;
    for (size_t i = 1; i < argv.size(); ++i) args += (i > 1 ? " " : "") + argv[i];
    ++seen[args];
    UR_EXPECT_TRUE_MSG(args, !argv.empty() && argv[0] == kIp);
    bool shows = false;
    for (const auto& word : argv) {
      shows = shows || word == "show";
      for (const char* write : {"add", "del", "delete", "flush", "replace", "change", "append",
                                "prepend", "set", "restore", "save"}) {
        UR_EXPECT_TRUE_MSG(args, word != write);
      }
    }
    UR_EXPECT_TRUE_MSG(args, shows);
  }
  for (const char* family : {"-4", "-6"}) {
    const std::string f = family;
    for (const std::string& args :
         {f + " rule show", f + " route show table 51821", f + " route show default table all",
          f + " -o addr show scope global"}) {
      UR_EXPECT_TRUE_MSG(args, seen[args] == 1);
    }
  }
}

// IPv6 off: the daemon installs no IPv6 half and skips the IPv6 legs, so the
// report says why and asks for no IPv6 measurement.
UR_TEST(DaemonDiagnose_IPv6OffSaysWhy) {
  FakeIp ip = LaptopWithoutAGlobalIpv6Address();
  const std::string v6 =
      Ipv6Section(Report(ip, /*hostIpv6=*/false, "net.ipv6.conf.all.disable_ipv6 is 1"));
  UR_EXPECT_TRUE_MSG(v6, Has(v6, "IPv6 is off (net.ipv6.conf.all.disable_ipv6 is 1): a tunnel "
                                 "adds no IPv6 half, and the egress witness skips its IPv6 "
                                 "legs\n"));
  UR_EXPECT_FALSE(Has(v6, "--selftest-egress"));
  UR_EXPECT_TRUE(Has(v6, "  rules (ip -6 rule show):\n"));
}

UR_TEST(DaemonDiagnose_WithoutIpNothingRuns) {
  FakeIp ip = LaptopWithoutAGlobalIpv6Address();
  const std::string report =
      urnw::diagnose::RoutingReport("", /*hostIpv6=*/true, "", kSelf, ip.Runner());
  UR_EXPECT_TRUE(ip.calls.empty());
  UR_EXPECT_TRUE_MSG(report, Has(report, "iproute2 `ip` is not installed"));
}

// A table never filled reads as no tunnel; a read that failed says how, with
// what ip printed; an installed table is printed as it is.
UR_TEST(DaemonDiagnose_EachListingSaysWhatItsAnswerMeans) {
  FakeIp ip = LaptopWithoutAGlobalIpv6Address();
  ip.Set("-6 rule show", Exited(255, "RTNETLINK answers: Operation not permitted\n"));
  ip.Set("-6 route show table 51821", Printed("::/1 dev urnet0 metric 1024 pref medium\n"
                                              "8000::/1 dev urnet0 metric 1024 pref medium\n"));
  ip.Set("-6 route show default table all", Printed(""));
  const std::string report = Report(ip);
  const std::string v6 = Ipv6Section(report);
  UR_EXPECT_TRUE_MSG(report, Has(report, "  the tunnel's capture table (ip -4 route show table "
                                         "51821):\n      none: no tunnel is up"));
  UR_EXPECT_FALSE(Has(report, "FIB table does not exist"));
  UR_EXPECT_TRUE_MSG(v6, Has(v6, "      could not be read (exit 255)\n"
                                 "      RTNETLINK answers: Operation not permitted\n"));
  UR_EXPECT_TRUE(Has(v6, "      ::/1 dev urnet0 metric 1024 pref medium\n"
                         "      8000::/1 dev urnet0 metric 1024 pref medium\n"));
  UR_EXPECT_TRUE(Has(v6, "      none: this family has no route off the tunnel"));
  // No default route: no interface to name.
  UR_EXPECT_FALSE(Has(v6, "carries a default route"));
}

UR_TEST(DaemonDiagnose_DefaultRouteInterfacesSkipLoAndRepeats) {
  const std::vector<std::string> names = urnw::diagnose::DefaultRouteInterfaces(
      "default via fe80::1 dev wlan0 proto ra metric 600 pref medium\n"
      "default dev wg0 table 51820 metric 1024 pref medium\n"
      "unreachable default dev lo proto kernel metric 4294967295 error -101 pref medium\n"
      "default proto ra metric 1024 pref medium\n"
      "\tnexthop via fe80::1 dev wlan0 weight 1\n"
      "\tnexthop via fe80::2 dev eth0 weight 1\n");
  UR_EXPECT_EQ(3, static_cast<int>(names.size()));
  UR_EXPECT_TRUE(names.size() == 3 && names[0] == "wlan0" && names[1] == "wg0" &&
                 names[2] == "eth0");
}
