// The egress witness's v6 leg on a host with an IPv6 route and no IPv6 of its own
// (TunnelPolicy.hpp, ReadCapturedV6). A marked socket there is bound to the
// tunnel's own address with a route that leaves by the physical interface, and
// the leg used to refuse it as captured, blaming the mark. The host shapes below
// are the ones measured on kernel 7.0 in a network namespace, with documentation
// addresses, and each is read the way Tunnel::VerifyEgressWitness reads it:
//
//   * a route and no global address: the tunnel comes up;
//   * a route, a global address and a healthy witness: comes up;
//   * a route, a global address and a captured or unmarked socket: refuses;
//   * a socket truly routed into the tunnel, or one nothing explains: refuses,
//     with or without a global address;
//   * a global address arriving or leaving mid-session is read again at every
//     check, never stops a healthy session, and a real leak still stops it.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Tunnel.hpp"
#include "TunnelPolicy.hpp"
#include "daemon/HostSettings.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::CapturedV6;
using urnw::Ipv6HostReads;

constexpr std::uint32_t kMark = urnw::kEgressMark;
const char* const kTun6 = "fd00:7572:6e65:ffff::2";
const char* const kTunName = "urnet0";
const char* const kGlobal = "2001:db8:1:2::20";

const char* const kTunLine =
    "14: urnet0    inet6 fd00:7572:6e65:ffff::2/64 scope global nodad \\       valid_lft forever "
    "preferred_lft forever\n";

std::string AddressLine(const std::string& name, const std::string& address,
                        const std::string& flags = "") {
  return "13: " + name + "    inet6 " + address + "/64 scope global " + flags +
         " \\       valid_lft 86000sec preferred_lft 14000sec\n";
}

// `ip -6 route get 2001:db8::1 mark 0x55524e57`, as the kernel answered it.
std::string RouteBy(const std::string& device, const std::string& source = kTun6) {
  return "2001:db8::1 from :: via fe80::1 dev " + device + " proto ra src " + source +
         " metric 600 pref medium";
}

Ipv6HostReads Host(const std::string& route, const std::string& addresses) {
  Ipv6HostReads reads;
  reads.route_ok = true;
  reads.route = route;
  reads.addresses_ok = true;
  reads.addresses = addresses;
  return reads;
}

// One treatment socket, as BindWitnessSocket reports it.
struct Socket {
  bool bound = true;
  std::string source = kTun6;
  int connectErrno = 0;
  bool markKnown = true;
  std::uint32_t mark = kMark;
};

Socket OnTheTunnel() { return Socket(); }

Socket OnTheGlobal() {
  Socket socket;
  socket.source = kGlobal;
  return socket;
}

Socket UnmarkedOn(const std::string& source) {
  Socket socket;
  socket.source = source;
  socket.mark = 0;
  return socket;
}

Socket NoRoute() {
  Socket socket;
  socket.bound = false;
  socket.source.clear();
  socket.connectErrno = ENETUNREACH;
  return socket;
}

// The v6 treatment leg, composed as Tunnel::VerifyEgressWitness composes it (the
// wiring test below holds the two together). True: the leg passes.
bool LegPasses(const Socket& socket, const Ipv6HostReads& host, std::string* why = nullptr) {
  switch (urnw::WitnessTreatmentVerdict(socket.bound, socket.source, socket.connectErrno,
                                        socket.markKnown, socket.mark, kMark, kTun6)) {
    case urnw::WitnessTreatment::Escaped:
    case urnw::WitnessTreatment::Unroutable:
      return true;
    case urnw::WitnessTreatment::Captured: {
      const urnw::CapturedV6Reading captured =
          urnw::ReadCapturedV6(socket.markKnown && socket.mark == kMark, socket.source, kTunName,
                               host);
      if (captured.verdict == CapturedV6::NoIpv6Egress) return true;
      if (why != nullptr) {
        *why = urnw::CapturedV6RefusalReason(captured, socket.source, kTunName);
      }
      return false;
    }
    case urnw::WitnessTreatment::Unmarked:
    case urnw::WitnessTreatment::Failed:
      return false;
  }
  return false;
}

CapturedV6 Read(const Ipv6HostReads& host, bool marked = true) {
  return urnw::ReadCapturedV6(marked, kTun6, kTunName, host).verdict;
}

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string BodyOf(const std::string& source, const std::string& signature) {
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

}  // namespace

// ---- the host shapes ---------------------------------------------------------

// THE REPORT: a default route learned from a router advertisement, no global
// address on any interface. The socket carries the mark, takes the tun's ULA as
// its source and leaves by the physical interface.
UR_TEST(EgressWitnessV6_ARouteWithNoGlobalAddressComesUp) {
  const Ipv6HostReads host = Host(RouteBy("wlan0"), kTunLine);
  UR_EXPECT_TRUE(LegPasses(OnTheTunnel(), host));
  const urnw::CapturedV6Reading reading = urnw::ReadCapturedV6(true, kTun6, kTunName, host);
  UR_EXPECT_TRUE(reading.verdict == CapturedV6::NoIpv6Egress);
  UR_EXPECT_TRUE(reading.route_device == "wlan0");
  // With nothing listed at all as well: the host's own listing, no tunnel in it.
  UR_EXPECT_TRUE(LegPasses(OnTheTunnel(), Host(RouteBy("wlan0"), "")));
}

// What does not count as a usable global address, each measured or read from the
// kernel's source selection: none of them gets a new socket off this host.
UR_TEST(EgressWitnessV6_AnAddressTheKernelWillNotUseIsNoGlobalAddress) {
  const struct {
    const char* what;
    std::string line;
  } rows[] = {
      {"still in DAD", AddressLine("wlan0", kGlobal, "tentative dynamic")},
      {"failed DAD", AddressLine("wlan0", kGlobal, "tentative dadfailed")},
      {"deprecated", AddressLine("wlan0", kGlobal, "deprecated dynamic")},
      {"optimistic", AddressLine("wlan0", kGlobal, "optimistic dynamic")},
      {"a ULA on another interface", AddressLine("docker0", "fd11:2233::1")},
      {"a ULA in the range's lower half", AddressLine("docker0", "fc00:1::1")},
  };
  for (const auto& row : rows) {
    if (!LegPasses(OnTheTunnel(), Host(RouteBy("wlan0"), kTunLine + row.line))) {
      UR_FAIL(std::string("refused with ") + row.what);
    }
  }
}

UR_TEST(EgressWitnessV6_ARouteWithAGlobalAddressAndAHealthyWitnessComesUp) {
  // The healthy socket binds the global address; nothing further is asked, so
  // reads that could not have run must not matter.
  UR_EXPECT_TRUE(LegPasses(OnTheGlobal(), Ipv6HostReads()));
  // A host with no route at all, what the reporter's passing bring-ups had.
  UR_EXPECT_TRUE(LegPasses(NoRoute(), Ipv6HostReads()));
}

UR_TEST(EgressWitnessV6_ARouteWithAGlobalAddressAndACapturedOrUnmarkedSocketRefuses) {
  const std::string global = kTunLine + AddressLine("wlan0", kGlobal, "dynamic");
  // Captured, marked: the address was there for it, and it took the tunnel's.
  UR_EXPECT_FALSE(LegPasses(OnTheTunnel(), Host(RouteBy("wlan0"), global)));
  UR_EXPECT_TRUE(Read(Host(RouteBy("wlan0"), global)) == CapturedV6::GlobalAddressPresent);
  // On any interface off the tunnel, another VPN's included.
  UR_EXPECT_FALSE(LegPasses(OnTheTunnel(),
                            Host(RouteBy("wlan0"), kTunLine + AddressLine("wg0", kGlobal))));
  UR_EXPECT_FALSE(LegPasses(
      OnTheTunnel(), Host(RouteBy("wlan0"), kTunLine + AddressLine("eth0.10@eth0", kGlobal))));
  // Captured, marked, and truly in the tunnel.
  UR_EXPECT_FALSE(LegPasses(OnTheTunnel(), Host(RouteBy(kTunName), global)));
  // Captured and unmarked.
  UR_EXPECT_FALSE(LegPasses(UnmarkedOn(kTun6), Host(RouteBy("wlan0"), global)));
  // A socket that really got a global source off the tunnel without the mark:
  // nothing this daemon installed explains that.
  UR_EXPECT_FALSE(LegPasses(UnmarkedOn(kGlobal), Host(RouteBy("wlan0"), global)));
  Socket unreadable = OnTheGlobal();
  unreadable.markKnown = false;
  UR_EXPECT_FALSE(LegPasses(unreadable, Host(RouteBy("wlan0"), global)));
}

// THE FAIL-CLOSED SIDE: "no global address" is not a pass on its own. Every one
// of these reads "bound to the tunnel's address, no global address anywhere".
UR_TEST(EgressWitnessV6_NoGlobalAddressPassesNothingElse) {
  const Ipv6HostReads bare = Host(RouteBy("wlan0"), kTunLine);
  // No mark, or none that could be read: the capture table claimed it, whatever
  // the route of a marked socket would be.
  UR_EXPECT_FALSE(LegPasses(UnmarkedOn(kTun6), bare));
  UR_EXPECT_TRUE(Read(bare, /*marked=*/false) == CapturedV6::Unmarked);
  Socket unreadable = OnTheTunnel();
  unreadable.markKnown = false;
  UR_EXPECT_FALSE(LegPasses(unreadable, bare));
  Socket otherMark = OnTheTunnel();
  otherMark.mark = urnw::kBypassMark;
  UR_EXPECT_FALSE(LegPasses(otherMark, bare));

  // The true capture: a default through the tun in main, or a rule that sends
  // the mark to the capture table. Both measured: the socket reads the same.
  UR_EXPECT_TRUE(Read(Host("2001:db8::1 from :: dev urnet0 src fd00:7572:6e65:ffff::2 metric 100 "
                           "pref medium",
                           kTunLine)) == CapturedV6::RoutedIntoTunnel);
  UR_EXPECT_TRUE(Read(Host("2001:db8::1 from :: dev urnet0 table 51821 src "
                           "fd00:7572:6e65:ffff::2 metric 1024 pref medium",
                           kTunLine)) == CapturedV6::RoutedIntoTunnel);

  // Nothing that could not be read lets a tunnel up.
  Ipv6HostReads routeFailed = bare;
  routeFailed.route_ok = false;
  routeFailed.route = "RTNETLINK answers: Network is unreachable";
  UR_EXPECT_TRUE(Read(routeFailed) == CapturedV6::Unexplained);
  Ipv6HostReads addressesFailed = bare;
  addressesFailed.addresses_ok = false;
  UR_EXPECT_TRUE(Read(addressesFailed) == CapturedV6::Unexplained);
  UR_EXPECT_TRUE(Read(Ipv6HostReads()) == CapturedV6::Unexplained);
  // A route answer with no interface, or one that is about no way off anything.
  UR_EXPECT_TRUE(Read(Host("", kTunLine)) == CapturedV6::Unexplained);
  UR_EXPECT_TRUE(Read(Host("2001:db8::1 from :: src fd00:7572:6e65:ffff::2", kTunLine)) ==
                 CapturedV6::Unexplained);
  UR_EXPECT_TRUE(Read(Host(RouteBy("lo"), kTunLine)) == CapturedV6::Unexplained);
  // The hypothetical must describe the socket that was measured: another source
  // means another decision than the one being explained.
  UR_EXPECT_TRUE(Read(Host(RouteBy("wlan0", kGlobal), kTunLine)) == CapturedV6::Unexplained);
  UR_EXPECT_TRUE(Read(Host("2001:db8::1 from :: via fe80::1 dev wlan0", kTunLine)) ==
                 CapturedV6::Unexplained);
  // Address listings that cannot be read as one.
  for (const char* listing : {"Error: something else\n", "14: urnet0\n",
                              "13: wlan0    inet 192.0.2.7/24 scope global\n",
                              "13: wlan0    inet6 not-an-address/64 scope global\n"}) {
    if (Read(Host(RouteBy("wlan0"), listing)) != CapturedV6::Unexplained) {
      UR_FAIL(std::string("read as an answer: ") + listing);
    }
  }
}

// The two reads, against what iproute2 prints.
UR_TEST(EgressWitnessV6_TheKernelsAnswersAreReadAsPrinted) {
  const std::string route =
      "2001:db8::1 from :: via fe80::1 dev wlan0 proto ra src "
      "fd00:7572:6e65:ffff::2 metric 600 pref medium\n    cache\n";
  UR_EXPECT_TRUE(urnw::RouteGetField(route, "dev") == "wlan0");
  UR_EXPECT_TRUE(urnw::RouteGetField(route, "src") == kTun6);
  UR_EXPECT_TRUE(urnw::RouteGetField("2001:db8::1 from :: dev", "dev").empty());
  // Only the first line is the answer.
  UR_EXPECT_TRUE(urnw::RouteGetField("2001:db8::1\ndev wlan0", "dev").empty());

  const auto interfaceIn = [](const std::string& listing) {
    return urnw::UsableGlobalIpv6Interface(listing, kTunName);
  };
  UR_EXPECT_TRUE(interfaceIn("") == std::string());
  UR_EXPECT_TRUE(interfaceIn(kTunLine) == std::string());
  UR_EXPECT_TRUE(interfaceIn(AddressLine("wlan0", kGlobal)) == std::string("wlan0"));
  UR_EXPECT_TRUE(interfaceIn(AddressLine("eth0.10@eth0", kGlobal)) == std::string("eth0.10"));
  // The tunnel's own name excludes only itself, and past a line that doesn't count.
  UR_EXPECT_TRUE(interfaceIn(kTunLine + AddressLine("wlan0", kGlobal, "tentative") +
                             AddressLine("eth0", kGlobal)) == std::string("eth0"));
  UR_EXPECT_TRUE(interfaceIn(AddressLine("urnet01", kGlobal)) == std::string("urnet01"));
  // A flag is a word of its own, not a piece of an interface's name.
  UR_EXPECT_TRUE(interfaceIn(AddressLine("nottentative0", kGlobal)) ==
                 std::string("nottentative0"));
  UR_EXPECT_TRUE(
      urnw::AddressLineIsUsableSource(AddressLine("wlan0", kGlobal, "temporary dynamic")));
  UR_EXPECT_TRUE(urnw::AddressLineIsUsableSource(AddressLine("wlan0", kGlobal, "mngtmpaddr")));
  UR_EXPECT_FALSE(urnw::AddressLineIsUsableSource(AddressLine("wlan0", kGlobal, "deprecated")));
}

// ---- mid-session -------------------------------------------------------------

namespace {

struct SessionCheck {
  const char* what;
  Socket socket;
  Ipv6HostReads host;
};

// The re-check's steps for a session that sees `checks`, thirty seconds apart.
std::vector<urnw::EgressRecheckStep> Run(const std::vector<SessionCheck>& checks,
                                         int* failuresLeft = nullptr) {
  const urnw::HostSettings settings;
  int failures = 0;
  std::vector<urnw::EgressRecheckStep> steps;
  for (const SessionCheck& check : checks) {
    steps.push_back(
        urnw::StepEgressRecheck(settings, LegPasses(check.socket, check.host), &failures));
  }
  if (failuresLeft != nullptr) *failuresLeft = failures;
  return steps;
}

bool AnyStop(const std::vector<urnw::EgressRecheckStep>& steps) {
  for (const urnw::EgressRecheckStep step : steps) {
    if (step == urnw::EgressRecheckStep::Stop) return true;
  }
  return false;
}

const urnw::EgressRecheckStep kPassed = urnw::EgressRecheckStep::Passed;
const urnw::EgressRecheckStep kWarned = urnw::EgressRecheckStep::Warned;
const urnw::EgressRecheckStep kStop = urnw::EgressRecheckStep::Stop;

}  // namespace

// A router starts offering a prefix under a running session, as measured: the
// address is tentative, then the socket binds it. Every check passes, for its
// own reason, and nothing in between counts against the session.
UR_TEST(EgressWitnessV6_AGlobalAddressAppearingMidSessionIsReadAgainAndDoesNotFlap) {
  const std::string tentative = kTunLine + AddressLine("wlan0", kGlobal, "tentative dynamic");
  const std::string usable = kTunLine + AddressLine("wlan0", kGlobal, "dynamic");
  int failures = -1;
  const std::vector<urnw::EgressRecheckStep> steps =
      Run({{"no global address yet", OnTheTunnel(), Host(RouteBy("wlan0"), kTunLine)},
           {"in DAD: the socket still takes the tunnel's", OnTheTunnel(),
            Host(RouteBy("wlan0"), tentative)},
           {"usable: the socket binds it", OnTheGlobal(), Host(RouteBy("wlan0", kGlobal), usable)},
           {"and goes on binding it", OnTheGlobal(), Host(RouteBy("wlan0", kGlobal), usable)}},
          &failures);
  UR_EXPECT_TRUE(steps ==
                 std::vector<urnw::EgressRecheckStep>({kPassed, kPassed, kPassed, kPassed}));
  UR_EXPECT_EQ(0, failures);
}

// The same in reverse: the prefix goes away (deprecated, then gone) and the
// session has no IPv6 of its own again. It used to stop two checks later.
UR_TEST(EgressWitnessV6_AGlobalAddressLeavingMidSessionDoesNotStopIt) {
  const std::string usable = kTunLine + AddressLine("wlan0", kGlobal, "dynamic");
  const std::string deprecated = kTunLine + AddressLine("wlan0", kGlobal, "deprecated dynamic");
  UR_EXPECT_FALSE(AnyStop(Run({{"usable", OnTheGlobal(), Host(RouteBy("wlan0", kGlobal), usable)},
                               {"deprecated", OnTheTunnel(), Host(RouteBy("wlan0"), deprecated)},
                               {"gone", OnTheTunnel(), Host(RouteBy("wlan0"), kTunLine)},
                               {"gone", OnTheTunnel(), Host(RouteBy("wlan0"), kTunLine)}})));
}

// The address finishes DAD between the socket and the listing: one check reads
// a usable address beside a socket still on the tunnel's. That is one warning,
// cleared by the next check, and the run of failures starts over.
UR_TEST(EgressWitnessV6_OneCheckCaughtHalfWayIsAWarningNotAStop) {
  const std::string usable = kTunLine + AddressLine("wlan0", kGlobal, "dynamic");
  int failures = -1;
  const std::vector<urnw::EgressRecheckStep> steps =
      Run({{"half way", OnTheTunnel(), Host(RouteBy("wlan0"), usable)},
           {"settled", OnTheGlobal(), Host(RouteBy("wlan0", kGlobal), usable)},
           {"half way again", OnTheTunnel(), Host(RouteBy("wlan0"), usable)},
           {"settled again", OnTheGlobal(), Host(RouteBy("wlan0", kGlobal), usable)}},
          &failures);
  UR_EXPECT_TRUE(steps ==
                 std::vector<urnw::EgressRecheckStep>({kWarned, kPassed, kWarned, kPassed}));
  UR_EXPECT_EQ(0, failures);
}

// A real leak still stops the session, on the second check running, whatever the
// session read before it.
UR_TEST(EgressWitnessV6_ARealLeakStillStopsASessionThatHadNoGlobalAddress) {
  const SessionCheck bare = {"no global address", OnTheTunnel(), Host(RouteBy("wlan0"), kTunLine)};
  const std::string usable = kTunLine + AddressLine("wlan0", kGlobal, "dynamic");
  const std::vector<urnw::EgressRecheckStep> expected = {kPassed, kWarned, kStop};
  const struct {
    const char* what;
    Socket socket;
    Ipv6HostReads host;
  } leaks[] = {
      {"an address arrived and the socket stays on the tunnel's", OnTheTunnel(),
       Host(RouteBy("wlan0"), usable)},
      {"the marked socket is now routed through the tun, no global address", OnTheTunnel(),
       Host(RouteBy(kTunName), kTunLine)},
      {"the marked socket is now routed through the tun, a global address", OnTheTunnel(),
       Host(RouteBy(kTunName), usable)},
      {"the mark stopped arriving, no global address", UnmarkedOn(kTun6),
       Host(RouteBy("wlan0"), kTunLine)},
      {"a socket got a global source off the tunnel without the mark", UnmarkedOn(kGlobal),
       Host(RouteBy("wlan0", kGlobal), usable)},
      {"the kernel's answers can no longer be read", OnTheTunnel(), Ipv6HostReads()},
  };
  for (const auto& leak : leaks) {
    const SessionCheck check = {leak.what, leak.socket, leak.host};
    if (Run({bare, check, check}) != expected) UR_FAIL(leak.what);
  }
}

UR_TEST(EgressWitnessV6_TheRecheckStepCountsARunOfFailures) {
  urnw::HostSettings settings;
  int failures = 0;
  UR_EXPECT_TRUE(urnw::StepEgressRecheck(settings, false, &failures) == kWarned);
  UR_EXPECT_EQ(1, failures);
  UR_EXPECT_TRUE(urnw::StepEgressRecheck(settings, true, &failures) == kPassed);
  UR_EXPECT_EQ(0, failures);
  UR_EXPECT_TRUE(urnw::StepEgressRecheck(settings, false, &failures) == kWarned);
  UR_EXPECT_TRUE(urnw::StepEgressRecheck(settings, false, &failures) == kStop);
  // The caller logs the run it stopped on.
  UR_EXPECT_EQ(2, failures);
  // 0 stops on the first failure, as 1 does (HostSettings.hpp).
  for (const int bar : {0, 1}) {
    settings.egress_witness_failures_to_stop = bar;
    failures = 0;
    UR_EXPECT_TRUE(urnw::StepEgressRecheck(settings, false, &failures) == kStop);
  }
}

// ---- the refusal's text ------------------------------------------------------

// THE DEFECT: on a host whose kernel marks every socket it said "the mark is not
// reaching this process's IPv6 sockets", which sends the reader after the one
// thing that works. Only a socket that has no mark may be told that.
UR_TEST(EgressWitnessV6_TheRefusalBlamesTheMarkOnlyWhenItIsMissing) {
  const char* const kBlame = "The mark is not reaching";
  const std::string usable = kTunLine + AddressLine("eth0", kGlobal, "dynamic");

  std::string why;
  UR_EXPECT_FALSE(LegPasses(UnmarkedOn(kTun6), Host(RouteBy("wlan0"), kTunLine), &why));
  UR_EXPECT_TRUE_MSG(why, Has(why, kBlame));
  UR_EXPECT_TRUE(Has(why, std::string("was bound to the tunnel's own address (") + kTun6 + ")"));

  why.clear();
  UR_EXPECT_FALSE(LegPasses(OnTheTunnel(), Host(RouteBy(kTunName), kTunLine), &why));
  UR_EXPECT_FALSE(Has(why, kBlame));
  UR_EXPECT_TRUE_MSG(why, Has(why, "so the mark is working"));
  UR_EXPECT_TRUE(Has(why, "routes it through urnet0"));

  why.clear();
  UR_EXPECT_FALSE(LegPasses(OnTheTunnel(), Host(RouteBy("wlan0"), usable), &why));
  UR_EXPECT_FALSE(Has(why, kBlame));
  UR_EXPECT_TRUE_MSG(why, Has(why, "so the mark is working"));
  UR_EXPECT_TRUE(Has(why, "while eth0 holds a usable global IPv6 address"));
  UR_EXPECT_TRUE(Has(why, "its route leaves by wlan0"));

  why.clear();
  UR_EXPECT_FALSE(LegPasses(OnTheTunnel(), Ipv6HostReads(), &why));
  UR_EXPECT_FALSE(Has(why, kBlame));
  UR_EXPECT_TRUE_MSG(why, Has(why, "so the mark is working"));
  UR_EXPECT_TRUE(Has(why, "cannot be shown to stay out of the tunnel"));

  // The v4 half: the same sentence about the same mark.
  const std::string v4Marked = urnw::CapturedV4RefusalReason(true, "169.254.2.1");
  UR_EXPECT_FALSE(Has(v4Marked, "not reaching"));
  UR_EXPECT_TRUE_MSG(v4Marked, Has(v4Marked, "so the mark is working"));
  const std::string v4Unmarked = urnw::CapturedV4RefusalReason(false, "169.254.2.1");
  UR_EXPECT_TRUE_MSG(v4Unmarked,
                     Has(v4Unmarked, "The mark is not reaching this process's sockets"));
  UR_EXPECT_TRUE(Has(v4Unmarked, "(169.254.2.1)"));
}

// ---- the wiring --------------------------------------------------------------

// Tunnel.cpp needs the kernel and the SDK's host, so its call sites are read.
UR_TEST(EgressWitnessV6_TheLegReadsItsCapturedSocketThroughThePolicy) {
  const std::string tunnel = ReadSource("Tunnel.cpp");
  const std::string witness = BodyOf(tunnel, "bool Tunnel::VerifyEgressWitness(");
  UR_EXPECT_TRUE_MSG("Tunnel::VerifyEgressWitness is read", !witness.empty());

  // The v6 treatment's Captured case, and nothing else in it.
  const size_t v6Leg = witness.find("WitnessTreatmentVerdict(daemon6Bound");
  const size_t capturedAt = witness.find("case WitnessTreatment::Captured: {", v6Leg);
  const size_t unmarkedAt = witness.find("case WitnessTreatment::Unmarked: {", capturedAt);
  if (v6Leg == std::string::npos || capturedAt == std::string::npos ||
      unmarkedAt == std::string::npos) {
    UR_FAIL("the v6 leg's Captured case was not found in Tunnel::VerifyEgressWitness");
    return;
  }
  const std::string captured = witness.substr(capturedAt, unmarkedAt - capturedAt);
  // The mark as this socket reads it, the socket's source and the tun's own name.
  UR_EXPECT_TRUE_MSG(captured,
                     Has(captured, "ReadCapturedV6(daemon6.mark_known && daemon6.mark == "
                                   "kEgressMark, daemon6.source,"));
  UR_EXPECT_TRUE(Has(captured, "name_, ReadIpv6HostForWitness())"));
  // Every reading but the one refuses, under the refusal's own code.
  UR_EXPECT_TRUE(Has(captured, "if (captured.verdict != CapturedV6::NoIpv6Egress) {"));
  UR_EXPECT_TRUE(Has(captured, "return fail(kCodeEgressUnprotected,"));
  UR_EXPECT_TRUE(Has(captured, "CapturedV6RefusalReason(captured, daemon6.source, name_)"));
  // The sentence that blamed the mark no longer stands on its own in the leg.
  UR_EXPECT_FALSE(Has(witness, "The mark is not reaching"));
  // The pass only leaves the switch: it is no route off the tunnel, so the
  // bring-up gate still wants another family, and leg 3 still runs.
  UR_EXPECT_FALSE(Has(captured, "offTunnelRoute = true"));
  UR_EXPECT_FALSE(Has(captured, "return true"));
  UR_EXPECT_TRUE(Has(captured, "break;"));
  const size_t legThree = witness.find("if (counters.tun != 0) {");
  UR_EXPECT_TRUE(legThree != std::string::npos && capturedAt < legThree);
  // The control leg still comes first: a socket without the mark that bound
  // anything but the tun, a global address off the tunnel included, refuses.
  const size_t control = witness.find("if (control6.source != localAddr6_) {");
  UR_EXPECT_TRUE(control != std::string::npos && control < v6Leg);
  UR_EXPECT_TRUE(Has(witness, "the IPv6 capture policy is NOT in force"));
  // IPv4 has no such reading: its marked socket on the tun's address refuses.
  const size_t v4Leg = witness.find("WitnessTreatmentVerdict(daemonBound");
  UR_EXPECT_TRUE(v4Leg != std::string::npos && v4Leg < v6Leg);
  UR_EXPECT_FALSE(Has(witness.substr(0, v6Leg), "ReadCapturedV6("));
  UR_EXPECT_TRUE(Has(witness, "CapturedV4RefusalReason(daemon.mark_known && daemon.mark == "
                              "kEgressMark,"));

  // The reads are the kernel's, about the witness's own destination and mark.
  const std::string reads = BodyOf(tunnel, "Ipv6HostReads ReadIpv6HostForWitness() {");
  UR_EXPECT_TRUE_MSG("ReadIpv6HostForWitness is read", !reads.empty());
  UR_EXPECT_TRUE(Has(reads, "{ip, \"-6\", \"route\", \"get\", kEgressProbeAddressV6, \"mark\", "
                            "MarkHex(kEgressMark)}"));
  UR_EXPECT_TRUE(Has(reads, "{ip, \"-o\", \"-6\", \"addr\", \"show\", \"scope\", \"global\"}"));
  UR_EXPECT_TRUE(Has(reads, "reads.route_ok = route.ok();"));
  UR_EXPECT_TRUE(Has(reads, "reads.addresses_ok = addresses.ok();"));

  // The re-check runs the whole witness and counts its answer through the step
  // tested above.
  const std::string host = ReadSource("daemon/TunnelHost.cpp");
  const std::string recheck = BodyOf(host, "bool TunnelHost::CheckEgressWitnessLocked() {");
  UR_EXPECT_TRUE(Has(recheck, "StepEgressRecheck(settings_, passed, &egressWitnessFailures_)"));
  UR_EXPECT_TRUE(Has(recheck, "if (step == EgressRecheckStep::Passed) return false;"));
  UR_EXPECT_TRUE(Has(recheck, "StopUnsafeSessionLocked("));
}
