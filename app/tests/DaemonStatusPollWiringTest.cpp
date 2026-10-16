// The window's daemon health poll reads the daemon's status on a worker
// (SdkHost::RequestDaemonStatus), as Windows' D4 keeps control and device rpc
// waits off the UI thread: a daemon that accepts the socket but no longer
// answers held the window for the control client's 30 s receive timeout on
// every 5 s tick. One single-flight read serves every follow-up, its reply is
// applied on the main loop, and a reply that was in flight across a start, a
// Disconnect or a sign-in or -out is dropped. MainWindow and SdkHost need
// gtkmm, glib and the SDK, so this reads their sources with the comments
// blanked.
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

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadPollSource(const std::string& relative) {
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

std::string PollBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool PollHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool PollInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// The tick only asks; the reply is applied when it comes, if nothing since
// made it describe another session.
UR_TEST(DaemonStatusPollWiring_TheTickAsksAWorker) {
  const std::string window = ReadPollSource("MainWindow.cpp");
  const std::string tick = PollBody(window, "bool MainWindow::PollDaemonHealth() {");
  UR_EXPECT_TRUE(!tick.empty());
  UR_EXPECT_TRUE(PollInOrder(tick, {"const uint64_t epoch = daemonStatusEpoch_;",
                                    "host_.RequestDaemonStatus(",
                                    "if (epoch != daemonStatusEpoch_) return;",
                                    "ApplyDaemonHealth(status);"}));
  for (const char* blocking : {"Control().Status(", "ReconcileProvider(", "FollowDaemon"}) {
    UR_EXPECT_TRUE_MSG(blocking, !PollHas(tick, blocking));
  }
  // no read of the daemon is left in the reply's handler itself
  const std::string reply = PollBody(
      window, "void MainWindow::ApplyDaemonHealth("
      "const std::optional<ctl::StatusReply>& status) {");
  UR_EXPECT_TRUE(!reply.empty());
  UR_EXPECT_FALSE(PollHas(reply, "Control().Status("));
  // nor in the reconcile it runs while disconnected: the reply serves it, and
  // with no reply the tick ends there
  UR_EXPECT_TRUE(PollInOrder(reply, {"if (!connected_) {", "if (status && !host_.CommandsBusy()) {",
                                     "host_.ReconcileProvider(\"health poll\", *status);",
                                     "}\n    return;"}));
  UR_EXPECT_FALSE(PollHas(reply, "host_.ReconcileProvider(\"health poll\");"));
  // every drop of the last reply moves the epoch
  UR_EXPECT_TRUE(PollInOrder(PollBody(window, "void MainWindow::ForgetDaemonStatus() {"),
                             {"daemonStatus_.reset();", "++daemonStatusEpoch_;"}));
  size_t resets = 0;
  for (size_t at = window.find("daemonStatus_.reset()"); at != std::string::npos;
       at = window.find("daemonStatus_.reset()", at + 1)) {
    ++resets;
  }
  UR_EXPECT_EQ(1u, resets);
}

// One read in flight at most, joined before the next and with the host, and
// its gate cleared before the marshal so a main loop that never runs the
// completion cannot wedge the poll.
UR_TEST(DaemonStatusPollWiring_TheHostReadsOnASingleFlightWorker) {
  const std::string host = ReadPollSource("SdkHost.cpp");
  const std::string request = PollBody(host, "bool SdkHost::RequestDaemonStatus(");
  UR_EXPECT_TRUE(PollInOrder(
      request, {"daemonStatusBusy_.compare_exchange_strong(expected, true)", "return false;",
                "std::scoped_lock lock(daemonStatusWorkerMutex_);",
                "if (daemonStatusWorker_.joinable()) daemonStatusWorker_.join();",
                "daemonStatusWorker_ = std::thread(", "status = control_.Status();",
                "daemonStatusBusy_.store(false);", "PostToMain(", "done(std::move(status));"}));
  UR_EXPECT_TRUE(PollInOrder(PollBody(host, "SdkHost::~SdkHost() {"),
                             {"std::scoped_lock statusLock(daemonStatusWorkerMutex_);",
                              "if (daemonStatusWorker_.joinable()) daemonStatusWorker_.join();"}));
  // the poll's reconcile reads nothing while no device is bound
  UR_EXPECT_TRUE(PollHas(PollBody(host, "void SdkHost::ReconcileProvider(const char* reason, "
                                        "const ctl::StatusReply& polled) {"),
                         "ReconcileProviderLocked(reason, /*userInitiated=*/false, "
                         "/*settingsChanged=*/false, &polled);"));
  UR_EXPECT_TRUE(PollInOrder(PollBody(host, "void SdkHost::ReconcileProviderLocked("),
                             {"polled && !device_ ? std::optional<ctl::StatusReply>(*polled)",
                              ": control_.Status(&statusError);", "if (!status) return;"}));
  // the follow-ups that read for themselves are gone
  for (const char* gone : {"void SdkHost::FollowDaemonNetworkCountry() {",
                           "void SdkHost::FollowDaemonLogUpload() {",
                           "void SdkHost::FollowDaemonExtenderReset() {"}) {
    UR_EXPECT_TRUE_MSG(gone, !PollHas(host, gone));
  }
}
