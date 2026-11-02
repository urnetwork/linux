// urnetworkd --diagnose: the routing policy this daemon's own sockets meet, for
// IPv4 and IPv6. The egress witness (Tunnel.cpp, VerifyEgressWitness) refuses
// a tunnel on measurements this state decides, and --diagnose used to show
// none of it, so a refusal could not be explained from its output:
//
//   * a rule another tool numbered below the capture rule is read first, by
//     every socket, the daemon's own included;
//   * a family with no default route leaves the daemon's own sockets nowhere
//     to go off the tunnel (the witness reads that as unroutable);
//   * an interface that carries a default route but has no usable global
//     address makes source selection take another interface's address, the
//     tunnel's own once one is up. With no usable global address on any other
//     interface either, the witness reads that as no IPv6 to protect and the
//     tunnel comes up (TunnelPolicy.hpp, ReadCapturedV6);
//   * whether the mark reaches the daemon's IPv6 sockets at creation, which no
//     read can show: --selftest-egress measures it, and this says so.
//
// READ-ONLY, and checkable by reading it: `ip <family> rule show`,
// `ip <family> route show ...` and `ip <family> -o addr show scope global`,
// each run once and without a shell; none needs root. Rules and routes are
// printed as ip printed them. Addresses are counted per interface and never
// printed.
//
// Pure apart from the injected runner, so the unit suite checks every line
// (tests/DaemonDiagnoseTest.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Tunnel.hpp"
#include "TunnelPolicy.hpp"

namespace urnw {
namespace diagnose {

// One argv, run without a shell: RunCommand in the daemon, a fake in the
// unit suite.
using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;

inline constexpr const char* kListingIndent = "      ";

// `text`, one indented line per line.
inline std::string IndentLines(const std::string& text) {
  std::string out;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out += kListingIndent + line + "\n";
  }
  return out;
}

// What one listing printed, or why there is nothing to show. `absent` says
// what an empty answer means for this listing.
inline std::string FormatListing(const CommandResult& r, const char* absent) {
  // A route table nobody ever filled is "FIB table does not exist" (and a
  // failed exit) from newer iproute2: the same answer as an empty one.
  if ((r.ok() && r.output.empty()) || r.output.find("does not exist") != std::string::npos) {
    return std::string(kListingIndent) + absent + "\n";
  }
  if (r.ok()) return IndentLines(r.output);
  std::string why = "it could not be run";
  if (r.spawned && r.term_signal != 0) {
    why = "killed by signal " + std::to_string(r.term_signal);
  } else if (r.spawned) {
    why = "exit " + std::to_string(r.exit_code);
  }
  return std::string(kListingIndent) + "could not be read (" + why + ")\n" +
         IndentLines(r.output);
}

// The interfaces the default routes leave by ("dev <name>"), in order, once
// each. lo is where an unreachable or blackhole default "leaves", which is no
// interface.
inline std::vector<std::string> DefaultRouteInterfaces(const std::string& listing) {
  std::vector<std::string> names;
  std::istringstream words(listing);
  std::string word;
  bool next = false;
  while (words >> word) {
    if (next && word != "lo") {
      bool seen = false;
      for (const auto& name : names) seen = seen || name == word;
      if (!seen) names.push_back(word);
    }
    next = word == "dev";
  }
  return names;
}

// Usable global addresses per interface, from `ip -o addr show scope global`:
// one line per address, "<index>: <name> <inet|inet6> <address>/<length> ...".
// An address the kernel would not give a new socket is not counted
// (AddressLineIsUsableSource). The address itself is never kept.
inline std::map<std::string, int> UsableGlobalAddressCounts(const std::string& listing) {
  std::map<std::string, int> counts;
  std::istringstream lines(listing);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words(line);
    std::string index;
    std::string name;
    std::string family;
    words >> index >> name >> family;
    if (index.empty() || index.back() != ':' || name.empty()) continue;
    if (family != "inet" && family != "inet6") continue;
    if (!AddressLineIsUsableSource(line)) continue;
    const std::string::size_type at = name.find('@');  // a VLAN's "eth0.10@eth0"
    ++counts[at == std::string::npos ? name : name.substr(0, at)];
  }
  return counts;
}

// One family's rules, capture table, default routes and address counts.
// `family` is "-4" or "-6".
inline std::string FamilyReport(const std::string& ip, const std::string& family,
                                const CommandRunner& run) {
  const std::string table = std::to_string(kTunnelRouteTable);
  std::string s;
  s += "  rules (ip " + family + " rule show):\n";
  s += FormatListing(run({ip, family, "rule", "show"}), "none");
  s += "  the tunnel's capture table (ip " + family + " route show table " + table + "):\n";
  s += FormatListing(run({ip, family, "route", "show", "table", table}),
                     "none: no tunnel is up, so its capture routes are not installed");
  s += "  default routes (ip " + family + " route show default table all):\n";
  const CommandResult defaults = run({ip, family, "route", "show", "default", "table", "all"});
  s += FormatListing(defaults,
                     "none: this family has no route off the tunnel, so this daemon's own "
                     "sockets have nowhere to go");
  const std::vector<std::string> uplinks =
      defaults.ok() ? DefaultRouteInterfaces(defaults.output) : std::vector<std::string>();

  s += "  usable global addresses per interface (counted, never printed):\n";
  const CommandResult addresses = run({ip, family, "-o", "addr", "show", "scope", "global"});
  if (!addresses.ok()) return s + FormatListing(addresses, "none");
  std::map<std::string, int> counts = UsableGlobalAddressCounts(addresses.output);
  for (const auto& uplink : uplinks) counts.emplace(uplink, 0);
  if (counts.empty()) s += std::string(kListingIndent) + "none\n";
  for (const auto& [name, count] : counts) {
    bool carriesDefault = false;
    for (const auto& uplink : uplinks) carriesDefault = carriesDefault || uplink == name;
    // One column for the counts: an interface name is at most 15 bytes.
    std::string row = kListingIndent + name;
    while (row.size() < 22) row += ' ';
    row += std::to_string(count);
    if (carriesDefault) row += "  carries a default route";
    s += row + "\n";
  }
  for (const auto& uplink : uplinks) {
    if (counts[uplink] != 0) continue;
    s += std::string(kListingIndent) + uplink +
         " carries a default route but has no usable global address: a socket\n" +
         kListingIndent +
         "routed through it takes its source address from another interface, which\n" +
         kListingIndent + "once a tunnel is up can be the tunnel's own\n";
    // Only IPv6 has the reading that lets a tunnel come up over this shape.
    if (family == "-6") {
      s += std::string(kListingIndent) +
           "(with no usable global IPv6 address on any other interface, the egress\n" +
           kListingIndent + "witness reads that as no IPv6 to protect and the tunnel comes up)\n";
    } else {
      s += std::string(kListingIndent) + "(the egress witness then reads it as captured)\n";
    }
  }
  return s;
}

// The --diagnose section. `ip` is FindTool("ip") ("" when it is missing),
// `hostIpv6` and `hostIpv6Detail` are HostIpv6Available's answer, and `self`
// is the running binary's own path (daemon/SelfPath.hpp).
inline std::string RoutingReport(const std::string& ip, bool hostIpv6,
                                 const std::string& hostIpv6Detail, const std::string& self,
                                 const CommandRunner& run) {
  const std::string capture = std::to_string(kFwmarkRulePriority);
  const std::string table = std::to_string(kTunnelRouteTable);
  const std::string mark = MarkText(kEgressMark);
  std::string s =
      "\nrouting policy: what this daemon's own sockets meet (read-only, no root needed)\n";
  if (ip.empty()) {
    return s +
           "  iproute2 `ip` is not installed (see the preflight), so the rules and routes "
           "cannot be read\n";
  }
  s += "  While a tunnel is up it adds two rules to each family:\n";
  s += "      " + capture + ":  not from all fwmark " + mark + " lookup " + table + "\n";
  s += "      " + std::to_string(kBypassRulePriority) + ":  from all fwmark " +
       MarkText(kBypassMark) + " lookup main   (apps run with urnetwork-exclude)\n";
  s += "  A socket without the mark " + mark + " is routed by the capture table " + table +
       ".\n";
  s += "  This daemon's own sockets carry the mark, pass " + capture +
       " and are routed by the rules\n";
  s += "  after it: main (32766) on most hosts. Every other rule numbered below " + capture +
       "\n";
  s += "  (0, local, is the kernel's own) is read first, by every socket. A refused start\n";
  s += "  leaves the tunnel down, so these two rules and table " + table +
       " are absent below.\n";

  s += "\nIPv4\n";
  s += FamilyReport(ip, "-4", run);

  s += "\nIPv6\n";
  if (hostIpv6) {
    s += "  IPv6 is on: a tunnel adds its IPv6 half (address, rules, capture routes), and the\n"
         "  egress witness checks this daemon's own IPv6 sockets\n";
  } else {
    s += "  IPv6 is off (" +
         (hostIpv6Detail.empty() ? std::string("disabled on this host") : hostIpv6Detail) +
         "): a tunnel adds no IPv6 half, and the egress witness skips its IPv6 legs\n";
  }
  s += FamilyReport(ip, "-6", run);
  if (hostIpv6) {
    s += "  Whether the mark reaches this daemon's own IPv6 sockets at creation is measured,\n"
         "  not read:\n";
    s += "      sudo " + self + " --selftest-egress\n";
    s += "  In step 7 of its output, both AF_INET6 lines must say \"marked\".\n";
  }
  return s;
}

}  // namespace diagnose
}  // namespace urnw
