// The daemon's main loop never waits for a bring-up (TunnelHost.hpp, its
// threading notes): a bring-up holds opMutex_ for the whole of its run, so every
// main-loop path try-locks it. TunnelHost needs glib and the SDK, so this reads
// its source with the line comments blanked, so prose cannot satisfy it.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// The source with every // comment blanked; string literals are kept.
std::string ReadLockSource(const std::string& relative) {
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

// The name of the TunnelHost member function whose body holds `at`.
std::string EnclosingMember(const std::string& source, size_t at) {
  const std::string marker = " TunnelHost::";
  const size_t start = source.rfind(marker, at);
  if (start == std::string::npos) return std::string();
  const size_t nameStart = start + marker.size();
  const size_t paren = source.find('(', nameStart);
  return source.substr(nameStart, paren - nameStart);
}

}  // namespace

// A blocking lock of opMutex_ only where nothing can hold it on another thread
// (AdoptArmedFloor runs before the loop, Stop and Disconnect after joining the
// worker) or in the bring-up itself.
UR_TEST(TunnelHostLock_OnlyTheBringUpAndTheJoinedStopWait) {
  const std::string host = ReadLockSource("daemon/TunnelHost.cpp");
  UR_EXPECT_TRUE(!host.empty());
  std::vector<std::string> waiting;
  for (size_t at = host.find("lock(opMutex_)"); at != std::string::npos;
       at = host.find("lock(opMutex_)", at + 1)) {
    waiting.push_back(EnclosingMember(host, at));
  }
  UR_EXPECT_EQ(4u, waiting.size());
  for (const std::string& member : waiting) {
    if (member != "AdoptArmedFloor" && member != "RunStart" && member != "Stop" &&
        member != "Disconnect") {
      UR_FAIL("opMutex_ is waited for in TunnelHost::" + member);
    }
  }
}

UR_TEST(TunnelHostLock_TheReaperTryLocksBeforeAnyGuard) {
  const std::string host = ReadLockSource("daemon/TunnelHost.cpp");
  const size_t start = host.find("void TunnelHost::Reap() {");
  UR_EXPECT_TRUE(start != std::string::npos);
  const size_t tryLock = host.find("opMutex_, std::try_to_lock", start);
  const size_t storm = host.find("CheckTunnelStormLocked()", start);
  const size_t witness = host.find("CheckEgressWitnessLocked()", start);
  UR_EXPECT_TRUE(tryLock != std::string::npos && tryLock < storm && tryLock < witness);
  UR_EXPECT_TRUE(host.find("if (!lock.owns_lock()) return;", tryLock) < storm);
}
