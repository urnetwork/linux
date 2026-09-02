// Deterministic ownership tests for the native-to-Go tunnel descriptor.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "IoLoopFd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadTunnelHostSource() {
  const std::string candidates[] = {
      std::string(UR_SRC_DIR) + "/daemon/TunnelHost.cpp",
      "app/src/daemon/TunnelHost.cpp",
      "../app/src/daemon/TunnelHost.cpp",
      "linux/app/src/daemon/TunnelHost.cpp",
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

UR_TEST(ioLoopRetirementCannotCloseAReusedTunnelDescriptor) {
  const int sourceFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (sourceFd < 0) {
    UR_FAIL("could not open the descriptor source");
    return;
  }
  const int tunnelFd = ::fcntl(sourceFd, F_DUPFD_CLOEXEC, 0);
  if (tunnelFd < 0) {
    ::close(sourceFd);
    UR_FAIL("could not create the tunnel descriptor stand-in");
    return;
  }

  const int ioLoopFd = urnw::DuplicateIoLoopFd(tunnelFd);
  UR_EXPECT_TRUE(ioLoopFd >= 0);
  UR_EXPECT_TRUE(ioLoopFd != tunnelFd);
  if (ioLoopFd >= 0) {
    UR_EXPECT_TRUE((::fcntl(ioLoopFd, F_GETFD) & FD_CLOEXEC) != 0);
  }

  // Force a new resource to reuse the native descriptor number. A late Go
  // IoLoop close must close only its own descriptor, never this replacement.
  ::close(tunnelFd);
  const int replacementFd = ::dup2(sourceFd, tunnelFd);
  UR_EXPECT_TRUE(replacementFd == tunnelFd);
  if (ioLoopFd >= 0) ::close(ioLoopFd);
  UR_EXPECT_TRUE(::fcntl(replacementFd, F_GETFD) >= 0);

  if (replacementFd >= 0) ::close(replacementFd);
  ::close(sourceFd);
}

UR_TEST(tunnelHostPassesAnOwnedDescriptorToTheGoIoLoop) {
  const std::string source = ReadTunnelHostSource();
  if (source.empty()) {
    UR_FAIL("could not read TunnelHost.cpp to check IoLoop fd ownership");
    return;
  }
  UR_EXPECT_TRUE(source.find("DuplicateIoLoopFd(tunnel_->fd())") != std::string::npos);
  UR_EXPECT_TRUE(source.find("newIoLoop(*device_, tunnel_->fd()") == std::string::npos);
}
