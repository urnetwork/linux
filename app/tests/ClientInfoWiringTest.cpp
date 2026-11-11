// The client info this app reports (REVOKE-UI-FINAL.md §1.13): every Api the
// GUI takes from its space, and the space Api the daemon builds its
// DeviceLocal from, report device type "linux", the build version and the
// build's distribution through the sdk's Api.setClientInfo. Before this both
// processes sent the sdk's unknown client, so the account's Sessions list read
// every use from this machine as "Unknown device" with no version. SdkHost and
// TunnelHost need the SDK, so this reads their sources.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cctype>
#include <string>
#include <vector>

#include "WiringSource.hpp"

using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;

namespace {

bool IsIdentifierChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The statements that directly follow each assignment to `name` ("api_ =",
// not "api_ ==", not "otherApi_ ="), trimmed of leading whitespace.
std::vector<std::string> StatementsAfterAssignments(const std::string& code,
                                                    const std::string& name) {
  std::vector<std::string> next;
  const std::string assignment = name + " =";
  for (size_t at = code.find(assignment); at != std::string::npos;
       at = code.find(assignment, at + 1)) {
    if (at > 0 && IsIdentifierChar(code[at - 1])) continue;
    const size_t after = at + assignment.size();
    if (after < code.size() && code[after] == '=') continue;
    const size_t end = code.find(';', after);
    if (end == std::string::npos) continue;
    size_t start = end + 1;
    while (start < code.size() && std::isspace(static_cast<unsigned char>(code[start]))) ++start;
    const size_t stop = code.find(';', start);
    next.push_back(code.substr(start, stop == std::string::npos ? std::string::npos
                                                                : stop - start + 1));
  }
  return next;
}

}  // namespace

// Initialize, ApplyNetworkServer and SetPrivateExtender each take a new Api
// from the space; every one is adopted before anything uses it.
UR_TEST(ClientInfo_EveryApiTheGuiTakesIsAdopted) {
  const std::string host = ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE_MSG("SdkHost.cpp was read", !host.empty());
  const std::vector<std::string> next = StatementsAfterAssignments(host, "api_");
  UR_EXPECT_TRUE_MSG("the three places the GUI takes an Api", next.size() >= 3);
  for (const std::string& statement : next) {
    UR_EXPECT_TRUE_MSG("after api_ =: " + statement, statement == "AdoptSpaceApiLocked();");
  }
  UR_EXPECT_EQ(next.size(), CountOf(host, "api_ = networkSpace_->getApi();"));
}

UR_TEST(ClientInfo_TheAdoptionReportsLinuxAndTheBuildVersion) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string adopt = FunctionBody(host, "void SdkHost::AdoptSpaceApiLocked()");
  UR_EXPECT_TRUE_MSG("SdkHost::AdoptSpaceApiLocked is defined", !adopt.empty());
  UR_EXPECT_TRUE(Contains(adopt, "ReportClientInfo(*api_, kAppVersion);"));
  // the build version the release passes (-Dapp_version)
  UR_EXPECT_TRUE(Contains(host, "constexpr const char* kAppVersion = UR_APP_VERSION;"));

  const std::string config = ReadCode("NetworkSpaceConfig.hpp");
  UR_EXPECT_TRUE(Contains(config, "inline constexpr const char* kClientInfoDeviceType = \"linux\";"));
  const std::string report = FunctionBody(config, "inline void ReportClientInfo(");
  UR_EXPECT_TRUE_MSG("NetworkSpaceConfig.hpp defines ReportClientInfo", !report.empty());
  UR_EXPECT_TRUE(Contains(
      report,
      "api.setClientInfo(urnet::newClientInfoWithDistribution(kClientInfoDeviceType, appVersion, UrDistribution()));"));
}

// The distribution both processes report with it (sdk ClientDistribution*),
// which the server reads for the account's onboarding platform: the one the
// packaging stamped (-Ddistribution, compiled in as UR_DISTRIBUTION), mapped
// onto the sdk's values, and linux, the package not known, for anything else.
UR_TEST(ClientInfo_TheDistributionIsTheStampedPackage) {
  const std::string config = ReadCode("NetworkSpaceConfig.hpp");
  const std::string distribution = FunctionBody(config, "inline std::string UrDistribution()");
  UR_EXPECT_TRUE_MSG("NetworkSpaceConfig.hpp defines UrDistribution", !distribution.empty());
  UR_EXPECT_TRUE(Contains(distribution, "const std::string stamped = UR_DISTRIBUTION;"));
  UR_EXPECT_TRUE(Contains(distribution,
                          "{urnet::ClientDistributionLinuxAppimage, urnet::ClientDistributionLinuxFlatpak}"));
  UR_EXPECT_TRUE(Contains(distribution, "return urnet::ClientDistributionLinux;"));
  UR_EXPECT_TRUE(Contains(config, "#define UR_DISTRIBUTION \"linux\""));

  // both binaries are compiled with the stamp, and the option offers only
  // the values UrDistribution knows
  const std::string meson = urnw::testing::wiring::ReadRaw("../meson.build");
  UR_EXPECT_TRUE(
      Contains(meson, "distribution_args = ['-DUR_DISTRIBUTION=\"' + get_option('distribution') + '\"']"));
  UR_EXPECT_EQ(CountOf(meson, "version_args + distribution_args +"), static_cast<size_t>(2));
  const std::string options = urnw::testing::wiring::ReadRaw("../meson_options.txt");
  UR_EXPECT_TRUE(Contains(options,
                          "option('distribution', type : 'combo', choices : ['linux', 'linux_appimage', "
                          "'linux_flatpak'], value : 'linux',"));
}

// The daemon's devices are built from its own NetworkSpace (the space json the
// GUI hands over at start), so its Api is not the GUI's and reports for itself,
// before any device reads it.
UR_TEST(ClientInfo_TheDaemonReportsBeforeBuildingADevice) {
  const std::string tunnel = ReadCode("daemon/TunnelHost.cpp");
  UR_EXPECT_TRUE_MSG("daemon/TunnelHost.cpp was read", !tunnel.empty());
  const std::string build = FunctionBody(tunnel, "urnet::DeviceLocal TunnelHost::NewDeviceLocked(");
  UR_EXPECT_TRUE_MSG("TunnelHost::NewDeviceLocked is defined", !build.empty());
  UR_EXPECT_TRUE(Before(build, "ReportClientInfo(networkSpace_->getApi(), appVersion);",
                        "urnet::newDeviceLocalWithMemoryTarget("));
  // the version reported is the one the device is built with
  UR_EXPECT_TRUE(Before(build, "const std::string appVersion =", "ReportClientInfo("));
  // ...and NewDeviceLocked is where every daemon device is built (the key
  // material constructor builds no device)
  for (const char* constructor : {"urnet::newDeviceLocal(", "urnet::newDeviceLocalWith"}) {
    UR_EXPECT_EQ(CountOf(tunnel, constructor), CountOf(build, constructor));
  }
  UR_EXPECT_TRUE(CountOf(build, "urnet::newDeviceLocalWith") >= 2);
}
