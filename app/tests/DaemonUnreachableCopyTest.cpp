// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <array>
#include <string>

#include "DaemonUnreachableCopy.hpp"

namespace urnw {
namespace {

UR_TEST(DaemonUnreachableCopyCoversEveryReason) {
  constexpr std::array reasons{
      DaemonUnreachableReason::None,
      DaemonUnreachableReason::SocketMissing,
      DaemonUnreachableReason::PermissionDenied,
      DaemonUnreachableReason::StaleSandboxMount,
      DaemonUnreachableReason::Other,
  };
  for (const auto reason : reasons) {
    const auto copy = CopyForDaemonUnreachableReason(reason);
    UR_EXPECT_TRUE(copy.key != nullptr && std::string(copy.key).size() > 0);
    UR_EXPECT_TRUE(copy.english != nullptr && std::string(copy.english).size() > 0);
  }
}

UR_TEST(StaleSandboxMountTellsTheUserToRelaunch) {
  const auto copy =
      CopyForDaemonUnreachableReason(DaemonUnreachableReason::StaleSandboxMount);
  UR_EXPECT_TRUE(std::string(copy.key) == "daemon_stale_sandbox_mount");
  UR_EXPECT_TRUE(std::string(copy.english).find("Close the app and open it again") !=
                 std::string::npos);
}

UR_TEST(LegacyGroupCopyIsSharedAndActionable) {
  const auto copy =
      CopyForDaemonUnreachableReason(DaemonUnreachableReason::PermissionDenied);
  UR_EXPECT_TRUE(std::string(copy.key) == "daemon_legacy_group_auth");
  UR_EXPECT_TRUE(std::string(copy.english).find("older version") != std::string::npos);
  UR_EXPECT_TRUE(std::string(copy.english).find("sign out and back in") !=
                 std::string::npos);
}

}  // namespace
}  // namespace urnw
