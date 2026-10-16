// The call sites of the provider-only device (support inbox 1521, P008), and
// of its client count on the way to the provide line. The lifecycle itself is
// pure and tested in ProvideLifecycleTest.cpp; what keeps a
// Linux provider earning while disconnected — and keeps its device from ever
// touching this machine's own routing — lives in the daemon and the GUI, which
// need glib, GTK and the SDK, so this reads their sources: a decision with no
// caller is this project's most-repeated defect.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadProvideSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// `first` occurs, and before `second` (which must occur too).
bool Before(const std::string& text, const std::string& first, const std::string& second) {
  const size_t a = text.find(first);
  const size_t b = text.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

}  // namespace

// The provider-only device is RunStart's network space and DeviceLocal steps
// and nothing after them: no egress marker, no nftables transaction, no device
// RPC listener, no tun, no IoLoop. Providing must not change this machine's
// own routing while disconnected.
UR_TEST(ProvideWiring_TheProviderDeviceTouchesNoRoutingAndNoListener) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string start = FunctionBody(host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(!start.empty());
  UR_EXPECT_TRUE(Has(start, "NewDeviceLocked("));
  UR_EXPECT_TRUE(Has(start, "setProvideControlMode(request.provide_mode)"));
  // call forms, so the comment that names them does not count
  for (const char* forbidden :
       {"Tunnel::Open(", "egressMarker_.", "ApplyFilterLocked(", "InstallFilterLocked(",
        "setRpcServer(", "newIoLoop(", "setTunnelStarted(", " device_ = "}) {
    UR_EXPECT_TRUE_MSG(forbidden, !Has(start, forbidden));
  }
  // and the device has its own slot, so device_ keeps meaning "the tunnel
  // session's device" everywhere else
  UR_EXPECT_TRUE(Has(start, "providerDevice_ = NewDeviceLocked("));
}

// Never beside a tunnel session (two devices under one identity) and never
// while the kill-switch floor is armed.
UR_TEST(ProvideWiring_TheDaemonRefusesBesideATunnelAndUnderTheArmedFloor) {
  const std::string start = FunctionBody(ReadProvideSource("daemon/TunnelHost.cpp"),
                                         "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(Has(start, "ctl::ValidateStartProviderRequest(request)"));
  UR_EXPECT_TRUE(Has(start, "ctl::kCodeStartInProgress"));
  UR_EXPECT_TRUE(Before(start, "device_.has_value() || tunnel_ != nullptr || ioLoop_.has_value()",
                        "ctl::kCodeTunnelSessionActive"));
  UR_EXPECT_TRUE(Before(start, "filter_.state() == FilterState::Armed", "ctl::kCodeKillSwitchArmed"));
  // both refusals come before anything is built
  UR_EXPECT_TRUE(Before(start, "ctl::kCodeKillSwitchArmed", "NewDeviceLocked("));
}

// Every teardown — and so the head of every bring-up, which opens with one —
// retires the provider-only device first, before the egress marker, the
// capture routes or the session's own DeviceLocal exist.
UR_TEST(ProvideWiring_EveryTeardownRetiresTheProviderFirst) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string stop = FunctionBody(host, "void TunnelHost::StopInternalLocked(");
  UR_EXPECT_TRUE(Before(stop, "RetireProviderDeviceLocked();", "RevertSessionMachineLocked();"));
  const std::string run = FunctionBody(host, "void TunnelHost::RunStart(");
  UR_EXPECT_TRUE(Before(run, "StopInternalLocked(std::string());", "egressMarker_.Attach("));
  // one copy of the identity rules for both devices
  UR_EXPECT_TRUE(Has(run, "device_ = NewDeviceLocked(config.by_jwt, config.instance_id"));
  UR_EXPECT_TRUE(!Has(run, "newDeviceLocalWithMemoryTarget"));
  const std::string retire = FunctionBody(host, "void TunnelHost::RetireProviderDeviceLocked()");
  UR_EXPECT_TRUE(Before(retire, "providerDevice_->close();", "providerDevice_->waitForClose("));
  UR_EXPECT_TRUE(Has(retire, "status_.provider_running = false;"));
}

// Never stops it: a set_provide whose mode does not provide while disconnected
// retires the device; any other mode is applied to it.
UR_TEST(ProvideWiring_SetProvideDecidesTheProviderDevice) {
  const std::string set = FunctionBody(ReadProvideSource("daemon/TunnelHost.cpp"),
                                       "bool TunnelHost::SetProvideMode(");
  UR_EXPECT_TRUE(Before(set, "provide::ProviderRuns(controlMode, /*connected=*/false)",
                        "RetireProviderDeviceLocked();"));
  UR_EXPECT_TRUE(Has(set, "providerDevice_->setProvideControlMode(mode);"));
}

// The daemon publishes the device in `status`, keeps it current on the reaper
// tick and tells it about network changes like the tunnel's device.
UR_TEST(ProvideWiring_TheReaperKeepsTheProviderCurrent) {
  const std::string reap = FunctionBody(ReadProvideSource("daemon/TunnelHost.cpp"),
                                        "void TunnelHost::Reap()");
  UR_EXPECT_TRUE(Has(reap, "providerDevice_ ? &*providerDevice_ : nullptr"));
  UR_EXPECT_TRUE(Has(reap, "liveDevice->networkChanged();"));
  UR_EXPECT_TRUE(Has(reap, "RefreshProviderStatusLocked();"));
}

// start_provider goes through the owner gate and claims the session, and a
// running provider counts as something another uid may not touch.
UR_TEST(ProvideWiring_TheControlServerGatesAndOwnsTheProvider) {
  const std::string server = ReadProvideSource("daemon/ControlServer.cpp");
  const std::string dispatch = FunctionBody(server, "void ControlServer::DispatchAuthorized(");
  UR_EXPECT_TRUE(Has(dispatch, "case ctl::Verb::StartProvider:"));
  UR_EXPECT_TRUE(Has(dispatch, "HandleStartProvider(conn, id, request, authorizedCrossUid)"));
  const std::string handle = FunctionBody(server, "nlohmann::json ControlServer::HandleStartProvider(");
  UR_EXPECT_TRUE(Before(handle, "CheckTunnelOwner(", "tunnel_.StartProvider(req)"));
  UR_EXPECT_TRUE(Before(handle, "tunnel_.StartProvider(req)", "ClaimTunnelOwnership(conn);"));
  const std::string owned = FunctionBody(server, "bool ControlServer::TunnelOwnedByOtherUid(");
  UR_EXPECT_TRUE(Has(owned, "status.provider_running"));
}

// Disconnect keeps providing: after stop_tunnel the GUI reconciles the provider.
// The provide mode and the provider policy reconcile too, and the health poll
// does it while disconnected (a launch without auto-connect, a service restart).
UR_TEST(ProvideWiring_TheGuiReconcilesAfterDisconnectAndModeChanges) {
  const std::string host = ReadProvideSource("SdkHost.cpp");
  const std::string disconnect = FunctionBody(host, "void SdkHost::DisconnectNow()");
  UR_EXPECT_TRUE(Before(disconnect, "control_.StopTunnel();",
                        "ReconcileProvider(\"disconnect\", /*userInitiated=*/true)"));
  UR_EXPECT_TRUE(Has(FunctionBody(host, "void SdkHost::SetProvideControlMode("),
                     "ReconcileProviderLocked(\"provide mode changed\""));
  UR_EXPECT_TRUE(Has(FunctionBody(host, "void SdkHost::SetProviderTransportSettingsLocked("),
                     "/*settingsChanged=*/true"));
  const std::string poll =
      FunctionBody(ReadProvideSource("MainWindow.cpp"),
                   "void MainWindow::ApplyDaemonHealth("
                   "const std::optional<ctl::StatusReply>& status)");
  const size_t idle = poll.find("if (!connected_) {");
  const size_t idleEnd = poll.find("return;", idle);
  UR_EXPECT_TRUE(idle != std::string::npos && idleEnd != std::string::npos);
  if (idle == std::string::npos || idleEnd == std::string::npos) return;
  UR_EXPECT_TRUE(Has(poll.substr(idle, idleEnd - idle), "host_.ReconcileProvider("));
}

// The reconcile decides with the shared step, starts with start_provider, drops
// a device bound to a session the daemon no longer runs, and leaves a live
// tunnel session alone.
UR_TEST(ProvideWiring_TheReconcileUsesTheSharedStep) {
  const std::string reconcile = FunctionBody(ReadProvideSource("SdkHost.cpp"),
                                             "void SdkHost::ReconcileProviderLocked(");
  UR_EXPECT_TRUE(Has(reconcile, "provide::DisconnectedProviderStep(mode, ctl::ProviderFactsFrom(*status)"));
  UR_EXPECT_TRUE(Has(reconcile, "control_.StartProvider(request, &after, &error, &code)"));
  UR_EXPECT_TRUE(Has(reconcile, "control_.SetProvide(mode, &error)"));
  UR_EXPECT_TRUE(Has(reconcile, "providerBackoff_.Allows(nowMillis)"));
  UR_EXPECT_TRUE(Before(reconcile, "if (live ||", "TeardownDeviceLocked();"));
  // signed out (SignOut.hpp) reads as a mode that does not provide, decided
  // before the status is read
  UR_EXPECT_TRUE(Before(reconcile, "!signedOut_.load() && !clientJwt.empty() && !instanceId.empty()",
                        "control_.Status("));
}

// With no DeviceRemote the provide dot and the discoverable line read the
// daemon's provider-only device, not a hard "not providing".
UR_TEST(ProvideWiring_TheStatsShowTheProviderWithoutADevice) {
  const std::string stats = FunctionBody(ReadProvideSource("SdkHost.cpp"), "LiveStats SdkHost::ReadStats()");
  UR_EXPECT_TRUE(Has(stats, "s.provideMode = daemonProviderMode_.load();"));
  UR_EXPECT_TRUE(Has(stats, "s.provideEnabled = daemonProviderRunning_.load() && s.provideMode != 0;"));
  UR_EXPECT_TRUE(Has(stats, "s.provideHasNetworkKey = daemonProviderNetworkKey_.load();"));
}

// The daemon counts the provider-only device's clients where it reads its tier
// and keys, as a tunnel session's device counts them (its connected network
// peers), and a count it could not read stays unread rather than 0.
UR_TEST(ProvideWiring_TheStatusCountsTheProviderClients) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string refresh = FunctionBody(host, "void TunnelHost::RefreshProviderStatusLocked()");
  UR_EXPECT_TRUE(Before(refresh, "int64_t clientCount = -1;", "try {"));
  UR_EXPECT_TRUE(Before(refresh, "providerDevice_->getNetworkPeers()", "} catch ("));
  UR_EXPECT_TRUE(Has(refresh, "peers && peers->Connected ? static_cast<int64_t>(peers->Connected->size()) : 0"));
  UR_EXPECT_TRUE(Has(refresh, "status_.provider_client_count = clientCount;"));
  const std::string retire = FunctionBody(host, "void TunnelHost::RetireProviderDeviceLocked()");
  UR_EXPECT_TRUE(Has(retire, "status_.provider_client_count = -1;"));
}

// With no DeviceRemote the provide line's count is the provider-only device's,
// from the same status as the provide dot, and a count the status did not give
// is unknown, never 0. Quit and sign-out forget it with the rest.
UR_TEST(ProvideWiring_TheStatsCountTheProviderClientsWithoutADevice) {
  const std::string host = ReadProvideSource("SdkHost.cpp");
  const std::string stats = FunctionBody(host, "LiveStats SdkHost::ReadStats()");
  // the branch with no DeviceRemote, from its first daemon read on
  const size_t noDeviceStart = stats.find("s.provideMode = daemonProviderMode_.load();");
  UR_EXPECT_TRUE(noDeviceStart != std::string::npos);
  if (noDeviceStart == std::string::npos) return;
  const std::string noDevice = stats.substr(noDeviceStart);
  UR_EXPECT_TRUE(Has(noDevice, "const int64_t clientCount = daemonProviderClientCount_.load();"));
  UR_EXPECT_TRUE(Has(noDevice, "s.provideClients = clientCount < 0 ? 0 : clientCount;"));
  UR_EXPECT_TRUE(Before(noDevice, "s.provideEnabled = daemonProviderRunning_.load()",
                        "s.provideClientsUnknown = s.provideEnabled && clientCount < 0;"));
  const std::string note = FunctionBody(host, "void SdkHost::NoteDaemonProviderLocked(");
  UR_EXPECT_TRUE(Has(note, "const int64_t clientCount = running ? status.provider_client_count : -1;"));
  UR_EXPECT_TRUE(Before(note, "daemonProviderClientCount_.exchange(clientCount) != clientCount",
                        "if (changed) PublishStats();"));
  for (const char* signature : {"void SdkHost::Shutdown()", "void SdkHost::LogoutNow()"}) {
    UR_EXPECT_TRUE_MSG(signature, Has(FunctionBody(host, signature),
                                      "daemonProviderClientCount_.store(-1);"));
  }
}
