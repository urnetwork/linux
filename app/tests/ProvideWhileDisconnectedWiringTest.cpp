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

// The provider-only device is RunStart's egress marker, network space and
// DeviceLocal steps and nothing after them: no nftables transaction, no device
// RPC listener, no tun, no IoLoop. Providing must not change this machine's
// own routing while disconnected; the mark is inert with no session's `ip
// rule`, and it is there from the device's first socket so a Connect may take
// the device (TakeProviderDeviceLocked).
UR_TEST(ProvideWiring_TheProviderDeviceTouchesNoRoutingAndNoListener) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string start = FunctionBody(host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(!start.empty());
  UR_EXPECT_TRUE(Has(start, "NewDeviceLocked("));
  UR_EXPECT_TRUE(Has(start, "setProvideControlMode(request.provide_mode)"));
  // call forms, so the comment that names them does not count
  for (const char* forbidden :
       {"Tunnel::Open(", "ApplyFilterLocked(", "InstallFilterLocked(",
        "setRpcServer(", "newIoLoop(", "setTunnelStarted(", " device_ = "}) {
    UR_EXPECT_TRUE_MSG(forbidden, !Has(start, forbidden));
  }
  // and the device has its own slot, so device_ keeps meaning "the tunnel
  // session's device" everywhere else
  UR_EXPECT_TRUE(Has(start, "providerDevice_ = NewDeviceLocked("));
  UR_EXPECT_TRUE(Before(start, "egressMarker_.Attach(", "providerDevice_ = NewDeviceLocked("));
  UR_EXPECT_TRUE(Has(start, "providerMarkedFromFirst_ = true;"));
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
  UR_EXPECT_TRUE(Has(retire, "ClearProviderStatusLocked();"));
  UR_EXPECT_TRUE(Has(FunctionBody(host, "void TunnelHost::ClearProviderStatusLocked()"),
                     "status_.provider_running = false;"));
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
  // the disconnect (stop_tunnel first), then the reconcile it queues once applied
  const std::string disconnect = FunctionBody(host, "void SdkHost::Disconnect()");
  UR_EXPECT_TRUE(Has(FunctionBody(host, "void SdkHost::DisconnectNow()"),
                     "control_.StopTunnel(nullptr, /*disconnect=*/true);"));
  UR_EXPECT_TRUE(Before(disconnect, "DisconnectNow();",
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
  // the connected peers, counted as the rows that go out with the count
  UR_EXPECT_TRUE(Has(refresh, "if (peers && peers->Connected) {"));
  UR_EXPECT_TRUE(Has(refresh, "clientCount = static_cast<int64_t>(rows.size());"));
  UR_EXPECT_TRUE(Has(refresh, "status_.provider_client_count = clientCount;"));
  // a failed read is unread, the count and the list alike
  UR_EXPECT_TRUE(Has(refresh, "clientCount = -1;\n    peersJson.clear();"));
  const std::string retire = FunctionBody(host, "void TunnelHost::ClearProviderStatusLocked()");
  UR_EXPECT_TRUE(Has(retire, "status_.provider_client_count = -1;"));
}

// The peers the count counts go out in the same status, from the same reading,
// each cut to the four fields the GUI's rows show and connect with: with no
// tunnel the status is not redacted for another local user, so a peer's
// principal and roles never leave the daemon.
UR_TEST(ProvideWiring_TheStatusListsTheProviderPeersCutToTheRows) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string refresh = FunctionBody(host, "void TunnelHost::RefreshProviderStatusLocked()");
  UR_EXPECT_TRUE(Has(refresh, "row.ClientId = std::move(peer.ClientId);"));
  UR_EXPECT_TRUE(Has(refresh, "row.ProvideEnabled = peer.ProvideEnabled;"));
  UR_EXPECT_TRUE(Has(refresh, "row.DeviceName = std::move(peer.DeviceName);"));
  UR_EXPECT_TRUE(Has(refresh, "row.DeviceSpec = std::move(peer.DeviceSpec);"));
  UR_EXPECT_FALSE(Has(refresh, "Principal"));
  UR_EXPECT_FALSE(Has(refresh, "Roles"));
  UR_EXPECT_TRUE(Has(refresh, "peersJson = ctl::DumpForWire(nlohmann::json(rows));"));
  UR_EXPECT_TRUE(Before(refresh, "status_.provider_client_count = clientCount;",
                        "status_.provider_network_peers_json = std::move(peersJson);"));
  // counts only, never names, in the log
  UR_EXPECT_TRUE(Has(refresh, "connected network peer(s), %lld"));
  const std::string retire = FunctionBody(host, "void TunnelHost::ClearProviderStatusLocked()");
  UR_EXPECT_TRUE(Has(retire, "status_.provider_network_peers_json.clear();"));
}

// With no session the GUI lists the provider-only device's peers where a
// session's PeerViewController's would go, filtered as that controller filters
// them, and pushes the peers feed only when the rows or the count moved.
UR_TEST(ProvideWiring_TheGuiListsTheProviderPeersWithoutADevice) {
  const std::string host = ReadProvideSource("SdkHost.cpp");
  const std::string note = FunctionBody(host, "void SdkHost::NoteDaemonProviderLocked(");
  UR_EXPECT_TRUE(Has(note, "if (running && !status.provider_network_peers_json.empty()) {"));
  UR_EXPECT_TRUE(Has(note, "peers = ConnectablePeers("));
  UR_EXPECT_TRUE(Has(note, "!SamePeerRows(*peers, *daemonProviderPeers_)"));
  UR_EXPECT_TRUE(Has(note, "if (peersChanged || countChanged) EmitDrawerEvent(DrawerEvent::Peers);"));
  const std::string peers = FunctionBody(host, "std::optional<urnet::NetworkPeerList> SdkHost::ConnectedProvidePeers()");
  // a session's controller first; the daemon's list only with no device
  UR_EXPECT_TRUE(Before(peers, "if (peerVc_) return peerVc_->getPeers();",
                        "if (!device_ && daemonProviderPeers_) return daemonProviderPeers_;"));
  const std::string count = FunctionBody(host, "int64_t SdkHost::ConnectedPeerCount()");
  UR_EXPECT_TRUE(Before(count, "if (peerVc_)", "if (!device_ && daemonProviderPeers_) {"));
  for (const char* signature : {"void SdkHost::Shutdown()", "void SdkHost::LogoutNow()"}) {
    UR_EXPECT_TRUE_MSG(signature, Has(FunctionBody(host, signature),
                                      "daemonProviderPeers_.reset();"));
  }
  // the Network destination hears the feed, and reads it with no device too
  const std::string window = ReadProvideSource("MainWindow.cpp");
  UR_EXPECT_TRUE(Has(window, "networkPage_->OnPeersEvent();"));
  const std::string page = FunctionBody(ReadProvideSource("NetworkPage.cpp"),
                                        "void NetworkPage::OnPeersEvent()");
  UR_EXPECT_FALSE(Has(page, "hasDevice()"));
  // the empty peers group says what the line says
  const std::string list =
      FunctionBody(ReadProvideSource("ConnectPage.cpp"), "void ConnectPage::ApplyPeersList()");
  UR_EXPECT_TRUE(Has(list, "peers_ ? Glib::ustring(Format(TN_(\"network_peer_count\""));
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

// ---- one device across connect and disconnect -------------------------------

// A Connect with the provider-only device's identity takes it before its
// opening teardown would retire it, keeps its space at step 2 and makes it the
// session's device at step 3; a start that ends before then closes it.
UR_TEST(ProvideWiring_AConnectTakesTheProviderDevice) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string run = FunctionBody(host, "void TunnelHost::RunStart(");
  UR_EXPECT_TRUE(Before(run, "TakeProviderDeviceLocked(config);", "StopInternalLocked(std::string());"));
  UR_EXPECT_TRUE(Before(run, "if (self->promotedDevice_) self->ClosePromotedDeviceLocked();",
                        "StopInternalLocked(std::string());"));
  UR_EXPECT_TRUE(Before(run, "egressMarker_.Attach(", "device_ = std::move(*promotedDevice_);"));
  UR_EXPECT_TRUE(Before(run, "if (promotedDevice_ && networkSpace_) {",
                        "LoadNetworkSpaceLocked(config.network_space_json);"));
  UR_EXPECT_TRUE(Before(run, "device_ = std::move(*promotedDevice_);",
                        "device_->setRpcServer("));
  const std::string take = FunctionBody(host, "void TunnelHost::TakeProviderDeviceLocked(");
  UR_EXPECT_TRUE(Has(take, "provide::PromotesProviderDevice(true, marked, provider, start)"));
  UR_EXPECT_TRUE(Has(take, "providerMarkedFromFirst_ && egressMarker_.attached()"));
  // the controllers read the device, so they close before it changes hands
  UR_EXPECT_TRUE(Before(take, "CloseProviderViewControllersLocked();",
                        "promotedDevice_ = std::move(*providerDevice_);"));
  UR_EXPECT_FALSE(Has(take, "->close()"));
  const std::string close = FunctionBody(host, "void TunnelHost::ClosePromotedDeviceLocked()");
  UR_EXPECT_TRUE(Before(close, "promotedDevice_->close();", "ReleaseDeviceLocked(promotedDevice_);"));
  // the marker stays while a taken device can still make sockets
  const std::string stop = FunctionBody(host, "void TunnelHost::StopInternalLocked(");
  UR_EXPECT_TRUE(Has(stop, "if (!keepDevice && !promotedDevice_) egressMarker_.Detach();"));
}

// The owning user's Disconnect keeps the session's device providing when a
// provider-only device could run in its place, decided after the machine and
// the firewall are given back. Only the session comes off it.
UR_TEST(ProvideWiring_TheDisconnectKeepsTheSessionDevice) {
  const std::string host = ReadProvideSource("daemon/TunnelHost.cpp");
  const std::string disconnect = FunctionBody(host, "void TunnelHost::Disconnect()");
  UR_EXPECT_TRUE(Before(disconnect, "JoinWorker();",
                        "StopInternalLocked(\"user\", /*disconnect=*/true);"));
  const std::string plain = FunctionBody(host, "void TunnelHost::Stop(");
  UR_EXPECT_TRUE(Has(plain, "StopInternalLocked(reason);"));
  const std::string stop = FunctionBody(host, "void TunnelHost::StopInternalLocked(");
  UR_EXPECT_TRUE(Has(stop, "if (disconnect && device_ && !activeConfig_.by_jwt.empty()) {"));
  UR_EXPECT_TRUE(Before(stop, "RevertSessionMachineLocked();", "provide::KeepsSessionDevice(facts)"));
  UR_EXPECT_TRUE(Before(stop, "ApplyFilterLocked(FilterState::Off, &ignored);",
                        "facts.firewallInForce = filter_.state() != FilterState::Off;"));
  UR_EXPECT_TRUE(Before(stop, "device_->removeDestination();", "device_->closeRpcServer();"));
  UR_EXPECT_TRUE(Before(stop, "if (!keepDevice) {\n      device_->close();",
                        "ResumeProvidingLocked(endedSession, keptMarkedFromFirst);"));
  const std::string resume = FunctionBody(host, "void TunnelHost::ResumeProvidingLocked(");
  UR_EXPECT_TRUE(Has(resume, "providerConfig_.provide_mode = providerDevice_->getProvideControlMode();"));
  UR_EXPECT_TRUE(Before(resume, "OpenProviderViewControllersLocked();", "RefreshProviderStatusLocked();"));
  // the same device again applies the policy in place rather than rebuild
  const std::string start = FunctionBody(host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(Before(start, "ctl::SameProviderDevice(providerConfig_, request)",
                        "providerDevice_->setProviderTransportSettings("));
}

// Only the owning user's Disconnect asks for it: the server passes it on for
// the owner alone, and every other stop the GUI sends (a quit, a sign-out, a
// server change, a failed start, the tray's recovery) closes the device.
UR_TEST(ProvideWiring_OnlyTheOwnersDisconnectKeepsTheDevice) {
  const std::string server = ReadProvideSource("daemon/ControlServer.cpp");
  UR_EXPECT_TRUE(Has(server, "!crossUid && request.is_object() && request.get<ctl::StopTunnelRequest>().disconnect;"));
  UR_EXPECT_TRUE(Before(server, "tunnel_.Disconnect();", "tunnel_.Stop(\"user\");"));
  const std::string host = ReadProvideSource("SdkHost.cpp");
  size_t disconnects = 0;
  for (size_t at = host.find("/*disconnect=*/true"); at != std::string::npos;
       at = host.find("/*disconnect=*/true", at + 1)) {
    ++disconnects;
  }
  UR_EXPECT_EQ(1u, disconnects);
  UR_EXPECT_TRUE(Has(FunctionBody(host, "void SdkHost::DisconnectNow()"),
                     "control_.StopTunnel(nullptr, /*disconnect=*/true);"));
  UR_EXPECT_TRUE(Has(FunctionBody(host, "void SdkHost::Shutdown()"), "control_.StopTunnel();"));
}
