// The call sites behind the provider-only device's statistics and settings
// (support inbox 1521, P008, the follow-up to providing while disconnected).
// While disconnected the daemon's provider-only device provides, and it has no
// DeviceRemote, so:
//
//   * the daemon runs the SDK's contract and provider status view controllers
//     on it, opened with the device and closed before it, polls the provider
//     status only while a GUI keeps asking, and answers provider_stats like
//     status (never gated, empty for a foreign uid);
//   * the GUI reads provider_stats while the Earnings destination shows, and
//     its provider statistics accessors and ProviderStatusNow fall back to that
//     snapshot only while no DeviceRemote is bound; the local idle reason is
//     derived while either device runs;
//   * the device's extender role travels with them, read where the connected
//     path reads it (the device's status, the contract controller's extender
//     series), so the Earnings extender row and plot show while disconnected;
//   * the connect page's Extender switch shows over that role while the daemon
//     says it takes the switch's write (set_provide_extender), which it writes
//     where the next start reads it, and the page polls while it shows;
//   * a saved network space value (DoH servers, VLESS, the private extender,
//     the server) sends start_provider again, and the daemon replaces a device
//     whose request changed.
//
// The pure halves are tested in ControlProtocolTest.cpp, ProvideLifecycleTest.cpp
// and ProviderIdleReasonTest.cpp; these need glib, GTK and the SDK, so this
// reads their sources: a decision with no caller is this project's
// most-repeated defect.
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

std::string ReadProviderOnlySource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// From the definition that starts with `signature` to the closing brace in
// column 0 that ends it.
std::string ProviderOnlyBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// `first` occurs, and before `second` (which must occur too).
bool Precedes(const std::string& text, const std::string& first, const std::string& second) {
  const size_t a = text.find(first);
  const size_t b = text.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

// From `from` up to the first `to` after it, or to the end when `to` is empty;
// empty when either is missing, which the callers check.
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t start = text.find(from);
  if (start == std::string::npos) return std::string();
  if (to.empty()) return text.substr(start);
  const size_t end = text.find(to, start + from.size());
  if (end == std::string::npos) return std::string();
  return text.substr(start, end - start);
}

}  // namespace

// The controllers come up with the device and go before it: they read it.
UR_TEST(ProviderOnlyStatus_TheDaemonOpensTheControllersWithTheDevice) {
  const std::string host = ReadProviderOnlySource("daemon/TunnelHost.cpp");
  const std::string start =
      ProviderOnlyBody(host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(Precedes(start, "providerDevice_ = NewDeviceLocked(",
                          "OpenProviderViewControllersLocked();"));
  const std::string open =
      ProviderOnlyBody(host, "void TunnelHost::OpenProviderViewControllersLocked()");
  UR_EXPECT_TRUE(Contains(open, "providerContractVc_ = providerDevice_->openContractViewController();"));
  UR_EXPECT_TRUE(Contains(
      open, "providerStatusVc_ = providerDevice_->openProviderStatusViewController();"));
  // opening polls nothing: the lease does
  UR_EXPECT_TRUE(!Contains(open, "->start()"));

  const std::string retire = ProviderOnlyBody(host, "void TunnelHost::RetireProviderDeviceLocked()");
  UR_EXPECT_TRUE(Precedes(retire, "CloseProviderViewControllersLocked();", "providerDevice_->close();"));
  const std::string close =
      ProviderOnlyBody(host, "void TunnelHost::CloseProviderViewControllersLocked()");
  UR_EXPECT_TRUE(Contains(close, "providerStatusLease_.Release();"));
  UR_EXPECT_TRUE(
      Contains(close, "providerDevice_->closeProviderStatusViewController(*providerStatusVc_);"));
  UR_EXPECT_TRUE(Contains(close, "providerDevice_->closeContractViewController(*providerContractVc_);"));
  // the generic close cannot release a C++ handle
  UR_EXPECT_TRUE(!Contains(host, "providerStatusVc_->close()"));
  UR_EXPECT_TRUE(!Contains(host, "providerContractVc_->close()"));
}

// A changed request replaces the running device through the shared rule.
UR_TEST(ProviderOnlyStatus_TheDaemonReplacesADeviceWhoseRequestChanged) {
  const std::string start = ProviderOnlyBody(ReadProviderOnlySource("daemon/TunnelHost.cpp"),
                                             "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(Contains(start, "ctl::SameProviderDevice(providerConfig_, request)"));
  UR_EXPECT_TRUE(Precedes(start, "ctl::SameProviderDevice(providerConfig_, request)",
                          "RetireProviderDeviceLocked();"));
}

// provider_stats reads the controllers without blocking behind a bring-up, and
// the status controller polls only while the lease is held.
UR_TEST(ProviderOnlyStatus_TheDaemonReadsTheControllersAndLeasesThePolling) {
  const std::string host = ReadProviderOnlySource("daemon/TunnelHost.cpp");
  const std::string stats = ProviderOnlyBody(host, "ctl::ProviderStatsReply TunnelHost::ProviderStats(");
  UR_EXPECT_TRUE(!stats.empty());
  UR_EXPECT_TRUE(Contains(stats, "std::try_to_lock"));
  UR_EXPECT_TRUE(Precedes(stats, "!providerDevice_", "reply.running = true;"));
  for (const char* read :
       {"providerDevice_->getProviderPacketStats()", "providerContractVc_->getProviderThroughputPoints()",
        "providerContractVc_->getProviderTransportDistribution()",
        "providerStatusVc_->getIsLoaded()", "providerStatusVc_->getLastFetchError()",
        "providerStatusVc_->getProviderStatus()"}) {
    UR_EXPECT_TRUE_MSG(read, Contains(stats, read));
  }
  UR_EXPECT_TRUE(Precedes(stats, "pollStatus && providerStatusLease_.Renew(",
                          "providerStatusVc_->start();"));
  const std::string reap = ProviderOnlyBody(host, "void TunnelHost::Reap()");
  UR_EXPECT_TRUE(Precedes(reap, "providerStatusLease_.Expire(", "providerStatusVc_->stop();"));
}

// Answered like status, before the polkit gate, and empty for a caller whose
// status is redacted, which must not renew the polling either.
UR_TEST(ProviderOnlyStatus_TheControlServerAnswersItLikeStatus) {
  const std::string server = ReadProviderOnlySource("daemon/ControlServer.cpp");
  const std::string dispatch = ProviderOnlyBody(server, "void ControlServer::Dispatch(");
  UR_EXPECT_TRUE(Precedes(dispatch, "verb == ctl::Verb::ProviderStats", "ctl::ActionIdForVerb("));
  UR_EXPECT_TRUE(Precedes(dispatch, "if (!StatusMustBeRedactedFor(conn))", "tunnel_.ProviderStats("));
  UR_EXPECT_TRUE(Contains(dispatch, "request.get<ctl::ProviderStatsRequest>().poll_status"));
  const std::string client = ReadProviderOnlySource("ControlClient.cpp");
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(client, "ControlClient::ProviderStats("),
                          "CallLocked(ctl::Verb::ProviderStats, nlohmann::json(request), error)"));
}

// The GUI asks while the Earnings destination shows, with the shared step,
// stops asking an older daemon, and raises the events the DeviceRemote's
// controllers raise.
UR_TEST(ProviderOnlyStatus_TheGuiPollsWhileTheEarningsDestinationShows) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  const std::string polling = ProviderOnlyBody(host, "void SdkHost::SetProviderStatusPolling(");
  UR_EXPECT_TRUE(Precedes(polling, "providerStatusPolling_ = polling;",
                          "ScheduleProviderStatsPollLocked(/*readNow=*/polling);"));
  const std::string schedule =
      ProviderOnlyBody(host, "void SdkHost::ScheduleProviderStatsPollLocked(bool readNow)");
  UR_EXPECT_TRUE(Precedes(schedule, "providerStatsPollId_ = g_timeout_add(",
                          "self->PollDaemonProviderStatsLocked();"));
  // at once, not a second later
  UR_EXPECT_TRUE(Precedes(schedule, "providerStatsPollId_ = g_timeout_add(",
                          "if (readNow) PollDaemonProviderStatsLocked();"));
  UR_EXPECT_TRUE(Contains(schedule, "g_source_remove(providerStatsPollId_);"));
  // armed while either destination wants it, and only then
  UR_EXPECT_TRUE(Precedes(schedule, "if (providerStatusPolling_ || providerExtenderPolling_) {",
                          "providerStatsPollId_ = g_timeout_add("));

  const std::string poll = ProviderOnlyBody(host, "void SdkHost::PollDaemonProviderStatsLocked()");
  UR_EXPECT_TRUE(Contains(
      poll, "provide::DaemonProviderStatsStep(device_.has_value(), daemonProviderRunning_.load(),"));
  UR_EXPECT_TRUE(Precedes(poll, "DropDaemonProviderStatsLocked();", "control_.ProviderStats("));
  UR_EXPECT_TRUE(Precedes(poll, "request.poll_status = providerStatusPolling_;",
                          "control_.ProviderStats(request, &error)"));
  UR_EXPECT_TRUE(Precedes(poll, "error == ctl::kErrorUnknownVerb",
                          "providerStatsUnsupportedGeneration_ = control_.SessionGeneration();"));
  UR_EXPECT_TRUE(Precedes(poll, "if (!reply->running) {", "daemonProviderStats_ = std::move(stats);"));
  UR_EXPECT_TRUE(Precedes(poll, "daemonProviderStats_ = std::move(stats);",
                          "EmitDrawerEvent(DrawerEvent::Throughput);"));
  UR_EXPECT_TRUE(Contains(poll, "if (statusChanged) EmitDrawerEvent(DrawerEvent::ProviderStatus);"));
  // the timeout holds `this`
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(host, "SdkHost::~SdkHost()"),
                          "g_source_remove(providerStatsPollId_);"));
  // and a sign-out or a quit forgets the snapshot
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(host, "void SdkHost::LogoutNow()"),
                          "DropDaemonProviderStatsLocked();"));
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(host, "void SdkHost::Shutdown()"),
                          "DropDaemonProviderStatsLocked();"));
}

// Every accessor the Earnings page's provider statistics read falls back to the
// daemon's snapshot, and only while no DeviceRemote is bound.
UR_TEST(ProviderOnlyStatus_TheProviderAccessorsReadTheDaemonWithoutADevice) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  struct Accessor {
    const char* signature;
    const char* fallback;
  };
  const Accessor kAccessors[] = {
      {"std::optional<urnet::ThroughputPointList> SdkHost::ProviderThroughputPoints()",
       "if (!device_ && daemonProviderStats_) return daemonProviderStats_->providerPoints;"},
      {"std::optional<urnet::TransportDistribution> SdkHost::ProviderTransportDistribution()",
       "if (!device_ && daemonProviderStats_) return daemonProviderStats_->providerDistribution;"},
      {"bool SdkHost::HasProviderStats()",
       "return !device_ && daemonProviderStats_ && daemonProviderStats_->hasProviderStats;"},
      {"bool SdkHost::DeviceHasProviderStats()",
       "if (!device_) return daemonProviderStats_ && daemonProviderStats_->hasProviderStats;"},
      {"SdkHost::ProviderStatusSnapshot SdkHost::ProviderStatusNow()",
       "if (!device_ && daemonProviderStats_) return daemonProviderStats_->status;"},
  };
  for (const Accessor& accessor : kAccessors) {
    const std::string body = ProviderOnlyBody(host, accessor.signature);
    UR_EXPECT_TRUE_MSG(accessor.signature, Contains(body, accessor.fallback));
  }
  // the session's own controllers still come first
  const std::string now = ProviderOnlyBody(host, kAccessors[4].signature);
  UR_EXPECT_TRUE(Precedes(now, "if (!providerStatusVc_) {", "daemonProviderStats_->status"));
  const std::string points = ProviderOnlyBody(host, kAccessors[0].signature);
  UR_EXPECT_TRUE(Precedes(points, "if (contractVc_) return contractVc_->getProviderThroughputPoints();",
                          "daemonProviderStats_"));
}

// Disconnected, the provider-only device counts as a provider, so Auto and
// Network show their lines; "no traffic yet" needs a window that was read.
UR_TEST(ProviderOnlyStatus_TheIdleLineCountsTheProviderOnlyDevice) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(host, "bool SdkHost::ProviderRuns()"),
                          "return hasDevice() || daemonProviderRunning_.load();"));
  const std::string page = ReadProviderOnlySource("EarningsPage.cpp");
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(page, "void EarningsPage::ApplyStatusLine()"),
                          "SessionIdleReasonFor(host_.ProviderRuns(),"));
  const std::string pull =
      ProviderOnlyBody(page, "void EarningsPage::PullProviderThroughput(bool forced)");
  UR_EXPECT_TRUE(Contains(
      pull, "distribution ? std::optional<int64_t>(distribution->ByteCount) : std::nullopt"));
}

// A saved network space value reaches a running provider-only device: each
// writer reconciles with settingsChanged after a real save, posted.
UR_TEST(ProviderOnlyStatus_ASavedSpaceValueReachesTheProvider) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  const std::string doh = ProviderOnlyBody(host, "std::optional<std::string> SdkHost::SetControlDohUrls(");
  UR_EXPECT_TRUE(Precedes(doh, "networkSpace_->setControlDohUrls(",
                          "if (answer && answer->empty()) {\n      ReconcileProviderAfterSpaceChange("));
  const std::string vless = ProviderOnlyBody(host, "std::optional<std::string> SdkHost::SetVlessSettings(");
  UR_EXPECT_TRUE(Precedes(vless, "networkSpace_->setVlessSettings(settings)",
                          "if (answer && answer->empty()) {\n      ReconcileProviderAfterSpaceChange("));
  const std::string extender = ProviderOnlyBody(host, "bool SdkHost::SetPrivateExtender(");
  UR_EXPECT_TRUE(Precedes(extender, "spaceManager_->updateNetworkSpaceValues(stored->key, values);",
                          "ReconcileProviderAfterSpaceChange("));
  // saving what is stored writes nothing, and reconciles nothing
  UR_EXPECT_TRUE(Precedes(extender, "return true;\n    }", "ReconcileProviderAfterSpaceChange("));
  const std::string server = ProviderOnlyBody(host, "bool SdkHost::ApplyNetworkServer(");
  UR_EXPECT_TRUE(Precedes(server, "control_.StopTunnel();", "ReconcileProviderAfterSpaceChange("));

  const std::string after =
      ProviderOnlyBody(host, "void SdkHost::ReconcileProviderAfterSpaceChange(const char* reason)");
  UR_EXPECT_TRUE(Precedes(after, "PostToMain(",
                          "ReconcileProvider(reason, /*userInitiated=*/true, /*settingsChanged=*/true);"));
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(host, "void SdkHost::ReconcileProvider(const char* reason,"),
                          "ReconcileProviderLocked(reason, userInitiated, settingsChanged);"));
  // and settingsChanged is what makes the step start a running provider again
  const std::string reconcile = ProviderOnlyBody(host, "void SdkHost::ReconcileProviderLocked(");
  UR_EXPECT_TRUE(Contains(reconcile, "ctl::ProviderFactsFrom(*status), settingsChanged)"));
}

// The provider-only device's extender role: the daemon reports what the
// connected path reads, the device's own extender status and the contract view
// controller's extender series, each read guarded on its own so a failure
// costs only its own field.
UR_TEST(ProviderOnlyStatus_TheDaemonReportsTheExtenderRole) {
  const std::string stats = ProviderOnlyBody(ReadProviderOnlySource("daemon/TunnelHost.cpp"),
                                             "ctl::ProviderStatsReply TunnelHost::ProviderStats(");
  UR_EXPECT_TRUE(Precedes(stats, "reply.running = true;",
                          "providerDevice_->getExtenderProvideStatus()"));
  UR_EXPECT_TRUE(Contains(
      stats, "reply.extender_provide_status_json = ctl::DumpForWire(nlohmann::json(*status));"));
  UR_EXPECT_TRUE(Precedes(stats, "if (providerContractVc_) {",
                          "providerContractVc_->getExtenderThroughputPoints()"));
  UR_EXPECT_TRUE(Contains(
      stats, "reply.extender_throughput_points_json = ctl::DumpForWire(nlohmann::json(*points));"));
  UR_EXPECT_TRUE(Contains(stats, "noteReadFailure(\"extender status\", e);"));
  UR_EXPECT_TRUE(Contains(stats, "noteReadFailure(\"extender series\", e);"));
  // the extender series' failure is its own: it never clears the provider series
  UR_EXPECT_TRUE(Precedes(stats, "noteReadFailure(\"series\", e);",
                          "providerContractVc_->getExtenderThroughputPoints()"));
}

// The GUI keeps both in its snapshot, and raises the event the DeviceRemote's
// listener raises when the role changes, and when the snapshot goes.
UR_TEST(ProviderOnlyStatus_TheGuiKeepsTheExtenderRole) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  const std::string poll = ProviderOnlyBody(host, "void SdkHost::PollDaemonProviderStatsLocked()");
  UR_EXPECT_TRUE(
      Contains(poll, "stats.extenderProvideStatus = ProviderStatsPart<urnet::ExtenderProvideStatus>("));
  UR_EXPECT_TRUE(Contains(poll, "reply->extender_provide_status_json, \"extender status\")"));
  UR_EXPECT_TRUE(Contains(poll, "stats.extenderPoints = ProviderStatsPart<urnet::ThroughputPointList>("));
  UR_EXPECT_TRUE(Contains(poll, "reply->extender_throughput_points_json, \"extender series\")"));
  UR_EXPECT_TRUE(
      Contains(poll, "daemonProviderStats_->extenderProvideStatusJson != stats.extenderProvideStatusJson"));
  UR_EXPECT_TRUE(Precedes(poll, "daemonProviderStats_ = std::move(stats);",
                          "if (extenderChanged) EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);"));
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(host, "void SdkHost::DropDaemonProviderStatsLocked()"),
                          "EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);"));
}

// The Earnings extender row, the running state behind its extender statistics
// and its extender plot read the snapshot while no DeviceRemote is bound, and
// the session device first. The connect page's row keeps the device alone: its
// switch writes the setting through the device, so it hides without one (N1).
UR_TEST(ProviderOnlyStatus_TheEarningsExtenderReadsTheDaemonWithoutADevice) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  const std::string points =
      ProviderOnlyBody(host, "std::optional<urnet::ThroughputPointList> SdkHost::ExtenderThroughputPoints()");
  UR_EXPECT_TRUE(Precedes(points, "if (contractVc_) return contractVc_->getExtenderThroughputPoints();",
                          "if (!device_ && daemonProviderStats_) return daemonProviderStats_->extenderPoints;"));
  const std::string role = ProviderOnlyBody(
      host, "std::optional<urnet::ExtenderProvideStatus> SdkHost::ProviderExtenderProvideStatus()");
  UR_EXPECT_TRUE(Precedes(role, "if (device_) return DeviceExtenderProvideStatusLocked();",
                          "if (daemonProviderStats_) return daemonProviderStats_->extenderProvideStatus;"));
  // the connect page's row, the switch, reads the snapshot only while the
  // daemon takes its write (ProviderOnlyStatus_TheConnectSwitch...)
  const std::string switchRow = ProviderOnlyBody(
      host, "std::optional<urnet::ExtenderProvideStatus> SdkHost::GetExtenderProvideStatus()");
  UR_EXPECT_TRUE(Precedes(switchRow, "switch (ExtenderSwitchSourceLocked()) {",
                          "return DeviceExtenderProvideStatusLocked();"));
  UR_EXPECT_TRUE(Precedes(switchRow, "case provide::ExtenderSwitchSource::Daemon:",
                          "return daemonProviderStats_->extenderProvideStatus;"));

  const std::string page = ReadProviderOnlySource("EarningsPage.cpp");
  const std::string apply = ProviderOnlyBody(page, "void EarningsPage::ApplyExtenderProvideState()");
  UR_EXPECT_TRUE(Contains(apply, "host_.ProviderExtenderProvideStatus()"));
  UR_EXPECT_TRUE(!Contains(apply, "GetExtenderProvideStatus"));
  UR_EXPECT_TRUE(Contains(apply, "const bool running = status && status->Enabled;"));
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(page, "void EarningsPage::PullProviderThroughput(bool forced)"),
                          "host_.ExtenderThroughputPoints()"));
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(page, "void EarningsPage::OnHostEvent(DrawerEvent event)"),
                          "case DrawerEvent::ExtenderProvideStatus:\n      ApplyExtenderProvideState();"));

  const std::string connect = ProviderOnlyBody(ReadProviderOnlySource("ConnectPage.cpp"),
                                               "void ConnectPage::ApplyExtenderProvideState()");
  UR_EXPECT_TRUE(Contains(connect, "host_.GetExtenderProvideStatus()"));
  UR_EXPECT_TRUE(!Contains(connect, "ProviderExtenderProvideStatus"));
}

// The daemon takes the connect page's Extender switch behind set_provide's
// owner gate, writes it where the next start reads it, and says so beside the
// setting it reports.
UR_TEST(ProviderOnlyStatus_TheDaemonTakesTheExtenderSwitch) {
  const std::string server = ReadProviderOnlySource("daemon/ControlServer.cpp");
  // polkit first: nothing answers it before the gate, as status is answered
  const std::string dispatch = ProviderOnlyBody(server, "void ControlServer::Dispatch(");
  UR_EXPECT_TRUE(Contains(dispatch, "ctl::ActionIdForVerb(verb, isLogTail, crossUid)"));
  UR_EXPECT_TRUE(!Contains(dispatch, "Verb::SetProvideExtender"));
  const std::string authorized = ProviderOnlyBody(server, "void ControlServer::DispatchAuthorized(");
  const std::string branch = Between(authorized, "case ctl::Verb::SetProvideExtender: {",
                                     "case ctl::Verb::SetKillSwitch: {");
  UR_EXPECT_TRUE(Precedes(branch, "CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)",
                          "tunnel_.SetProvideExtender(req.provide_extender, &error)"));
  UR_EXPECT_TRUE(Precedes(branch, "request.get<ctl::SetProvideExtenderRequest>()",
                          "tunnel_.SetProvideExtender(req.provide_extender, &error)"));
  // a write nothing took is an error reply, never ok
  UR_EXPECT_TRUE(Precedes(branch, "if (!tunnel_.SetProvideExtender(req.provide_extender, &error)) {",
                          "ctl::MakeErrorReply("));
  UR_EXPECT_TRUE(Precedes(branch, "ctl::MakeErrorReply(", "reply(ctl::MakeReply(id, true));"));

  const std::string host = ReadProviderOnlySource("daemon/TunnelHost.cpp");
  const std::string write =
      ProviderOnlyBody(host, "bool TunnelHost::SetProvideExtender(bool on, std::string* error)");
  // never behind a bring-up, which owns the session
  UR_EXPECT_TRUE(Precedes(write, "std::try_to_lock", "provide::ExtenderSettingTargetFor("));
  UR_EXPECT_TRUE(Contains(
      write, "providerDevice_.has_value(), device_.has_value(), networkSpace_.has_value())"));
  for (const char* target :
       {"providerDevice_->setProvideExtender(on);", "device_->setProvideExtender(on);",
        "networkSpace_->getAsyncLocalState().getLocalState().setProvideExtender(on);"}) {
    UR_EXPECT_TRUE_MSG(target, Contains(write, target));
  }
  // nothing to keep it in is refused, never reported as written
  const std::string refused =
      Between(write, "case provide::ExtenderSettingTarget::None:", "} catch (");
  UR_EXPECT_TRUE(Precedes(refused, "*error = ", "return false;"));
  UR_EXPECT_TRUE(!Contains(refused, "return true;"));
  // Where the next start reads it: both starts import the GUI's space into
  // networkSpace_ and build their device in it, and neither teardown drops it,
  // so a write through either device, or into that space with neither
  // running, is what the other device reads when it starts.
  const std::string run = ProviderOnlyBody(host, "void TunnelHost::RunStart(");
  UR_EXPECT_TRUE(Precedes(run, "LoadNetworkSpaceLocked(config.network_space_json);",
                          "device_ = NewDeviceLocked("));
  const std::string start =
      ProviderOnlyBody(host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(Precedes(start, "LoadNetworkSpaceLocked(request.network_space_json);",
                          "providerDevice_ = NewDeviceLocked("));
  const std::string build = ProviderOnlyBody(host, "urnet::DeviceLocal TunnelHost::NewDeviceLocked(");
  UR_EXPECT_TRUE(Contains(build, "*networkSpace_, byJwt"));
  for (const char* teardown :
       {"void TunnelHost::StopInternalLocked(", "void TunnelHost::RetireProviderDeviceLocked()"}) {
    const std::string body = ProviderOnlyBody(host, teardown);
    UR_EXPECT_TRUE_MSG(teardown, !body.empty() && !Contains(body, "networkSpace_.reset()"));
  }

  const std::string stats = ProviderOnlyBody(host, "ctl::ProviderStatsReply TunnelHost::ProviderStats(");
  UR_EXPECT_TRUE(Precedes(stats, "reply.running = true;",
                          "reply.provide_extender = providerDevice_->getProvideExtender();"));
  // the writer is said only beside a setting to show
  UR_EXPECT_TRUE(Precedes(stats, "reply.provide_extender = providerDevice_->getProvideExtender();",
                          "reply.provide_extender_writable = true;"));

  const std::string client = ReadProviderOnlySource("ControlClient.cpp");
  UR_EXPECT_TRUE(Contains(ProviderOnlyBody(client, "bool ControlClient::SetProvideExtender("),
                          "CallLocked(ctl::Verb::SetProvideExtender, nlohmann::json(req), error,"));
}

// With no DeviceRemote the connect page's Extender switch reads and writes the
// daemon's provider-only device, only while the daemon takes the write, and
// reads the setting back at once after the write.
UR_TEST(ProviderOnlyStatus_TheConnectSwitchReadsAndWritesTheDaemonWithoutADevice) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  const std::string source =
      ProviderOnlyBody(host, "provide::ExtenderSwitchSource SdkHost::ExtenderSwitchSourceLocked() const");
  UR_EXPECT_TRUE(Contains(source, "provide::ExtenderSwitchSourceFor("));
  UR_EXPECT_TRUE(Contains(source, "daemonProviderStats_ && daemonProviderStats_->provideExtenderWritable"));
  const std::string get = ProviderOnlyBody(host, "bool SdkHost::GetProvideExtender()");
  UR_EXPECT_TRUE(Precedes(get, "switch (ExtenderSwitchSourceLocked()) {",
                          "return daemonProviderStats_->provideExtender;"));
  const std::string set = ProviderOnlyBody(host, "void SdkHost::SetProvideExtender(bool on)");
  UR_EXPECT_TRUE(Contains(set, "switch (ExtenderSwitchSourceLocked()) {"));
  UR_EXPECT_TRUE(Contains(set, "case provide::ExtenderSwitchSource::Device:\n      device_->setProvideExtender(on);"));
  const std::string daemonWrite = Between(set, "case provide::ExtenderSwitchSource::Daemon: {",
                                          "case provide::ExtenderSwitchSource::None:");
  UR_EXPECT_TRUE(Precedes(daemonWrite, "control_.SetProvideExtender(on, &error)",
                          "PollDaemonProviderStatsLocked();"));
  UR_EXPECT_TRUE(Precedes(daemonWrite, "PollDaemonProviderStatsLocked();",
                          "EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);"));

  // the snapshot keeps the setting and the writer, and a change of either
  // redraws the switch
  const std::string poll = ProviderOnlyBody(host, "void SdkHost::PollDaemonProviderStatsLocked()");
  UR_EXPECT_TRUE(Contains(poll, "stats.provideExtender = reply->provide_extender;"));
  UR_EXPECT_TRUE(Contains(poll, "stats.provideExtenderWritable = reply->provide_extender_writable;"));
  UR_EXPECT_TRUE(Contains(poll, "daemonProviderStats_->provideExtender != stats.provideExtender"));
  UR_EXPECT_TRUE(
      Contains(poll, "daemonProviderStats_->provideExtenderWritable != stats.provideExtenderWritable"));

  // the page reads the switch through these, and writes only a shown row
  const std::string page = ReadProviderOnlySource("ConnectPage.cpp");
  const std::string apply = ProviderOnlyBody(page, "void ConnectPage::ApplyExtenderProvideState()");
  UR_EXPECT_TRUE(Contains(apply, "host_.GetExtenderProvideStatus()"));
  UR_EXPECT_TRUE(Contains(apply, "host_.GetProvideExtender()"));
  const std::string toggled = ProviderOnlyBody(page, "void ConnectPage::OnExtenderToggled()");
  UR_EXPECT_TRUE(Precedes(toggled, "if (!extenderRowDrawn_.visible) return;", "host_.SetProvideExtender(on);"));
}

// The connect destination keeps provider_stats coming while it shows, read at
// once before its re-seed, and never keeps the API polling for it.
UR_TEST(ProviderOnlyStatus_TheConnectPagePollsWhileItShows) {
  const std::string host = ReadProviderOnlySource("SdkHost.cpp");
  const std::string want = ProviderOnlyBody(host, "void SdkHost::SetProviderExtenderPolling(bool polling)");
  UR_EXPECT_TRUE(Precedes(want, "providerExtenderPolling_ = polling;",
                          "ScheduleProviderStatsPollLocked(/*readNow=*/polling);"));
  const std::string page = ReadProviderOnlySource("ConnectPage.cpp");
  const std::string map = Between(page, "signal_map().connect([this] {", "});");
  UR_EXPECT_TRUE(Precedes(map, "host_.SetProviderExtenderPolling(true);", "Resync();"));
  const std::string unmap = Between(page, "signal_unmap().connect([this] {", "});");
  UR_EXPECT_TRUE(Contains(unmap, "host_.SetProviderExtenderPolling(false);"));
  const std::string poll = ProviderOnlyBody(host, "void SdkHost::PollDaemonProviderStatsLocked()");
  UR_EXPECT_TRUE(Contains(poll, "request.poll_status = providerStatusPolling_;"));
  UR_EXPECT_TRUE(!Contains(poll, "providerExtenderPolling_"));
}
