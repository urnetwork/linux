// The path every command urnetworkd prints names it by (daemon/SelfPath.hpp),
// and the egress refusal that sends people to --diagnose (TunnelPolicy.hpp).
//
// No package puts urnetworkd on $PATH: the deb, rpm and Arch packages install
// it at /usr/lib/urnetwork/urnetworkd, and the install tarball moves it to
// /usr/local/lib/urnetwork/urnetworkd on an immutable host. The refusal said
// to run `urnetworkd --diagnose`, which is "command not found" as printed; it
// now names the running binary, which is right for every packaging format.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "TunnelPolicy.hpp"
#include "daemon/SelfPath.hpp"

namespace {

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

UR_TEST(DaemonSelfPath_TheRunningBinaryIsTheAnswer) {
  // The install tarball's immutable-host layout, started by a person who typed
  // a relative name.
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("/usr/local/lib/urnetwork/urnetworkd", "urnetworkd") ==
                 "/usr/local/lib/urnetwork/urnetworkd");
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("/usr/lib/urnetwork/urnetworkd", nullptr) ==
                 "/usr/lib/urnetwork/urnetworkd");
  // A dev build runs from its build tree, and says so.
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("/home/dev/linux/app/build/urnetworkd", "./urnetworkd") ==
                 "/home/dev/linux/app/build/urnetworkd");
}

UR_TEST(DaemonSelfPath_AReplacedBinaryFallsBackToArgv0ThenTheInstalledPath) {
  UR_EXPECT_TRUE(std::string(urnw::kInstalledDaemonPath) == "/usr/lib/urnetwork/urnetworkd");
  // An upgrade replaced the file under a running daemon: the kernel link no
  // longer names a runnable path, the unit's absolute argv[0] does.
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("/usr/local/lib/urnetwork/urnetworkd (deleted)",
                                       "/usr/local/lib/urnetwork/urnetworkd") ==
                 "/usr/local/lib/urnetwork/urnetworkd");
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("/usr/lib/urnetwork/urnetworkd (deleted)", nullptr) ==
                 urnw::kInstalledDaemonPath);
  // A relative argv[0] is never the answer: it is the name $PATH does not have.
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("", "urnetworkd") == urnw::kInstalledDaemonPath);
  UR_EXPECT_TRUE(urnw::ResolveSelfPath("", nullptr) == urnw::kInstalledDaemonPath);
}

// THE DEFECT: the refusal told the person to run `urnetworkd --diagnose`, and
// that name is on no one's $PATH.
UR_TEST(DaemonEgressRefusal_NamesDiagnoseByTheRunningBinary) {
  const std::string reason =
      "a v6 socket this daemon just opened was bound to the tunnel's own address (fd00::1)";
  const std::string message = urnw::EgressRefusalMessage(
      reason, urnw::ResolveSelfPath("/usr/local/lib/urnetwork/urnetworkd", "urnetworkd"));
  UR_EXPECT_TRUE(message.rfind("this daemon cannot prove its own traffic stays out of its own "
                               "tunnel, so the tunnel is not being run: " +
                                   reason + ". ",
                               0) == 0);
  UR_EXPECT_TRUE_MSG(message, Has(message, "`/usr/local/lib/urnetwork/urnetworkd --diagnose`"));
  UR_EXPECT_FALSE(Has(message, "`urnetworkd --diagnose`"));
  UR_EXPECT_TRUE(Has(message, "IPv4 and IPv6"));
  UR_EXPECT_TRUE(Has(urnw::EgressRefusalMessage(reason, urnw::kInstalledDaemonPath),
                     "`/usr/lib/urnetwork/urnetworkd --diagnose` needs no root"));
}
