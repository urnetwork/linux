// The call sites of the dead-tunnel failsafe in the daemon (TunnelWatchdog.hpp):
// watched from the up edge, unwatched before any teardown, judged on the reaper
// after the guards that end a session for worse, ended through the protective
// teardown, and sampled without touching the session's device object. The
// verdict is pure and runs in TunnelWatchdogTest.cpp; TunnelHost and the
// sampler need glib and the SDK, so this reads their sources with the line
// comments blanked, so prose cannot satisfy a contract.
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
std::string ReadDeadTunnelSource(const std::string& relative) {
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

std::string DeadTunnelBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool DeadTunnelHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool InOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// Watched from the block that publishes Up, and unwatched as the first act of
// every teardown, before the device is closed.
UR_TEST(DeadTunnelWiring_WatchedFromTheUpEdgeToTheTeardown) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string start = DeadTunnelBody(host, "void TunnelHost::RunStart(");
  UR_EXPECT_TRUE(InOrder(start, {"status_.tunnel_state = ctl::TunnelState::Up;",
                                 "StartDeadTunnelWatchLocked();", "} catch ("}));
  const std::string stop = DeadTunnelBody(host, "void TunnelHost::StopInternalLocked(");
  UR_EXPECT_TRUE(InOrder(stop, {"RevertSessionMachineLocked();", "device_->close();",
                                "status_.failsafe_armed = false;"}));
  const std::string revert = DeadTunnelBody(host, "void TunnelHost::RevertSessionMachineLocked(");
  UR_EXPECT_TRUE(InOrder(revert, {"StopDeadTunnelWatchLocked();", "tunnel_.reset();"}));
  const std::string unwatch = DeadTunnelBody(host, "void TunnelHost::StopDeadTunnelWatchLocked(");
  UR_EXPECT_TRUE(DeadTunnelHas(unwatch, "exitSampler_.Stop();"));
  // A new bring-up starts with no countdown.
  const std::string request = DeadTunnelBody(host, "ctl::StatusReply TunnelHost::Start(");
  UR_EXPECT_TRUE(DeadTunnelHas(request, "status_.failsafe_armed = false;"));
}

// Judged once the storm guard, the egress witness and a bring-up have had
// their say, and never while the io loop's own death waits to be explained.
UR_TEST(DeadTunnelWiring_TheReaperJudgesAfterTheGuards) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string reap = DeadTunnelBody(host, "void TunnelHost::Reap() {");
  UR_EXPECT_TRUE(InOrder(reap, {"CheckTunnelStormLocked()", "CheckEgressWitnessLocked()",
                                "if (busy_.load()) return;", "if (CheckDeadTunnelLocked()) return;",
                                "const bool died = ioLoopDied_.load();"}));
  const std::string check = DeadTunnelBody(host, "bool TunnelHost::CheckDeadTunnelLocked() {");
  UR_EXPECT_TRUE(InOrder(check, {"ioLoopDied_.load()", "return false;"}));
}

// The hold reads the balance the app's alert reads, off the session's own device:
// ContractStatus.InsufficientBalance, read on the sampler's thread like every
// other call on that device, ahead of the window status it is paired with (so a
// status a destination change reset is never paired with the generation before
// it), and handed to the watch on every tick. There is no second source: nothing
// else writes the reading, and the reaper never asks the device itself.
UR_TEST(DeadTunnelWiring_TheHoldReadsTheBalanceOffTheSessionsDevice) {
  const std::string sampler = ReadDeadTunnelSource("daemon/ExitSampler.cpp");
  const std::string run = DeadTunnelBody(sampler, "void ExitSampler::Run(");
  UR_EXPECT_TRUE(InOrder(run, {"urnet_device_get_contract_status(channel->deviceHandle)",
                               "urnet_device_get_window_status(channel->deviceHandle)",
                               "if (contract && window) {",
                               "channel->balance.sampled = true;",
                               "channel->balance.insufficientBalance = contract->InsufficientBalance;",
                               "channel->balance.noPermission = contract->NoPermission;",
                               "channel->balance.connectionGeneration = window->ConnectionGeneration;",
                               "channel->balance.providerAddedCount = window->ProviderStateAdded;",
                               "channel->lastSampleMillis.store(now);"}));
  // one copy under the channel's lock, written and read whole
  UR_EXPECT_TRUE(InOrder(run, {"if (contract && window) {", "std::scoped_lock lock(channel->mutex);",
                               "channel->balance.sampled = true;"}));
  const std::string read = DeadTunnelBody(sampler, "ExitSampler::Reading ExitSampler::Read() const {");
  UR_EXPECT_TRUE(InOrder(read, {"std::scoped_lock lock(channel_->mutex);",
                                "reading.balance = channel_->balance;"}));
  // no sdk call under that lock: the mutex is taken, and let go, around the copy
  const size_t copied = run.find("channel->balance.providerAddedCount");
  UR_EXPECT_TRUE(copied != std::string::npos);
  UR_EXPECT_FALSE(DeadTunnelHas(run.substr(run.find("if (contract && window) {"),
                                           copied - run.find("if (contract && window) {")),
                                "urnet_"));

  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string check = DeadTunnelBody(host, "bool TunnelHost::CheckDeadTunnelLocked() {");
  UR_EXPECT_TRUE(InOrder(check, {"exitSampler_.Read()", "inputs.balance = sample.balance;",
                                 "deadTunnelWatch_.Tick(inputs)"}));
  UR_EXPECT_FALSE(DeadTunnelHas(check, "getContractStatus"));
  UR_EXPECT_FALSE(DeadTunnelHas(check, "urnet_device_get_contract_status"));
  // the log says what the hold did, on the tick it did it
  UR_EXPECT_TRUE(InOrder(check, {"tick.balanceStep == watchdog::BalanceStep::Entered",
                                 "tick.balanceStep == watchdog::BalanceStep::Released"}));
}

// The tun's counters in the right directions, the sampler's readings, and a
// dead verdict ended through the protective teardown with the failsafe's
// reason and code: the armed floor or no table, the outcome checked, published
// last. Never StopInternalLocked with a reason, which lifts the floor.
UR_TEST(DeadTunnelWiring_ADeadVerdictEndsThroughTheProtectiveTeardown) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string check = DeadTunnelBody(host, "bool TunnelHost::CheckDeadTunnelLocked() {");
  UR_EXPECT_TRUE(
      DeadTunnelHas(check, "inputs.outboundPackets = ReadIfaceCounter(iface, \"tx_packets\");"));
  UR_EXPECT_TRUE(
      DeadTunnelHas(check, "inputs.inboundPackets = ReadIfaceCounter(iface, \"rx_packets\");"));
  UR_EXPECT_TRUE(DeadTunnelHas(check, "exitSampler_.Read()"));
  UR_EXPECT_TRUE(InOrder(
      check, {"deadTunnelWatch_.Tick(inputs)", "status_.failsafe_armed =",
              "StopUnsafeSessionLocked(watchdog::StopReasonOf(tick.verdict.reason)",
              "ctl::kCodeTunnelDead", "return true;"}));
  UR_EXPECT_FALSE(DeadTunnelHas(check, "StopInternalLocked("));
  // The clock that runs through a suspend.
  const std::string clock = DeadTunnelBody(host, "int64_t WatchdogMillis() {");
  UR_EXPECT_TRUE(DeadTunnelHas(clock, "CLOCK_BOOTTIME"));
}

// The sampler holds the device's C handle, never the session's DeviceLocal,
// and the session never waits for it.
UR_TEST(DeadTunnelWiring_TheSamplerNeverTouchesTheDeviceObject) {
  const std::string sampler = ReadDeadTunnelSource("daemon/ExitSampler.cpp");
  UR_EXPECT_TRUE(!sampler.empty());
  UR_EXPECT_TRUE(DeadTunnelHas(sampler, "urnet_device_local_get_exits(channel->deviceHandle)"));
  UR_EXPECT_TRUE(DeadTunnelHas(sampler, "urnet_device_get_window_status(channel->deviceHandle)"));
  UR_EXPECT_FALSE(DeadTunnelHas(sampler, "DeviceLocal"));
  UR_EXPECT_TRUE(DeadTunnelHas(sampler, ".detach();"));
  UR_EXPECT_FALSE(DeadTunnelHas(sampler, ".join("));
  const std::string run = DeadTunnelBody(sampler, "void ExitSampler::Run(");
  // Published last: a completed sample, never an attempted one.
  UR_EXPECT_TRUE(InOrder(run, {"urnet_device_local_get_exits(", "channel->cancelled.load()",
                               "channel->provenCount.store(proven);",
                               "channel->lastSampleMillis.store(now);"}));
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string watch = DeadTunnelBody(host, "void TunnelHost::StartDeadTunnelWatchLocked() {");
  // At the interval the watch's own settings ask for, nonpositive read as the
  // default, so the sampler never spins and never paces off a constant.
  UR_EXPECT_TRUE(
      DeadTunnelHas(watch, "if (!exitSampler_.Start(device_->handle(), &WatchdogMillis,"));
  UR_EXPECT_TRUE(
      DeadTunnelHas(watch, "watchdog::SampleIntervalMillis(deadTunnelWatch_.settings())"));
  UR_EXPECT_TRUE(DeadTunnelHas(run, "channel->clock() + channel->sampleIntervalMillis;"));
  UR_EXPECT_FALSE(DeadTunnelHas(sampler, "kSdkSampleIntervalMillis"));
  UR_EXPECT_TRUE(DeadTunnelHas(sampler, "sampleIntervalMillis <= 0) return false;"));
  // The up edge's level comes from the sampler's first sample: the bring-up
  // makes no call on the device for it.
  UR_EXPECT_FALSE(DeadTunnelHas(watch, "device_->get"));
  UR_EXPECT_TRUE(DeadTunnelHas(watch, "inputs.windowSampled = false;"));
  const std::string check = DeadTunnelBody(host, "bool TunnelHost::CheckDeadTunnelLocked() {");
  UR_EXPECT_TRUE(DeadTunnelHas(check, "inputs.windowSampled = sample.sampled;"));
}

// A network change reaches a watched session's device through the sampler,
// which tells the device before it samples, and never from the reaper on the
// main loop: the call takes the device's state lock, and a wedged device held
// there would stop the verdict that ends its session. The provider-only
// device, which nothing samples, is still told by the reaper.
UR_TEST(DeadTunnelWiring_NetworkChangesReachTheSessionThroughTheSampler) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string reap = DeadTunnelBody(host, "void TunnelHost::Reap() {");
  UR_EXPECT_TRUE(InOrder(reap, {"networkQualityTracker_.Observe(",
                                "device_ && deadTunnelWatching_",
                                "exitSampler_.NoteNetworkChange(path);", "} else if (",
                                "liveDevice->networkChanged();",
                                "liveDevice->networkQualityChanged();"}));
  UR_EXPECT_FALSE(DeadTunnelHas(reap, "device_->networkChanged"));
  UR_EXPECT_FALSE(DeadTunnelHas(reap, "device_->networkQualityChanged"));
  const std::string sampler = ReadDeadTunnelSource("daemon/ExitSampler.cpp");
  const std::string run = DeadTunnelBody(sampler, "void ExitSampler::Run(");
  UR_EXPECT_TRUE(InOrder(run, {"urnet_device_local_network_changed(channel->deviceHandle);",
                               "urnet_device_local_network_quality_changed(channel->deviceHandle);",
                               "urnet_device_get_window_status(channel->deviceHandle)"}));
  // A kick wakes the sampler's wait.
  UR_EXPECT_TRUE(DeadTunnelHas(run, "channel->kick != Channel::Kick::None"));
}

// What the kicks are decided on (NetworkQuality.hpp): the SDK's own route per
// family, read with the egress witness's probe socket, against a baseline each
// device takes where it is created. A suspend is read off the suspend clock
// before the reaper's try-lock can return, and owed as a forced kick.
UR_TEST(DeadTunnelWiring_TheReaperKicksOnTheMeasuredPathAndAfterASuspend) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string reap = DeadTunnelBody(host, "void TunnelHost::Reap() {");
  UR_EXPECT_TRUE(InOrder(reap, {"suspendDetector_.Observe(SuspendedMillis())",
                                "resumePending_ = true;", "opMutex_, std::try_to_lock"}));
  UR_EXPECT_TRUE(InOrder(reap, {"if (resumePending_ && liveDevice != nullptr) {",
                                "networkQualityTracker_.ForceKickOnNextPath();",
                                "resumePending_ = false;",
                                "networkQualityTracker_.Observe(ReadNetworkPath(tunnelInterface), "
                                "WatchdogMillis())"}));
  const std::string clock = DeadTunnelBody(host, "std::int64_t SuspendedMillis() {");
  UR_EXPECT_TRUE(DeadTunnelHas(clock, "CLOCK_BOOTTIME") && DeadTunnelHas(clock, "CLOCK_MONOTONIC"));

  const std::string read =
      DeadTunnelBody(host, "LinuxNetworkQualitySnapshot ReadNetworkPath(const std::string& "
                           "tunnelInterface) {");
  UR_EXPECT_TRUE(InOrder(read, {"ReadFamilyPath(AF_INET, interfaces, tunnelInterface)",
                                "ReadFamilyPath(AF_INET6, interfaces, tunnelInterface)"}));
  const std::string family = DeadTunnelBody(host, "LinuxFamilyPath ReadFamilyPath(");
  UR_EXPECT_TRUE(InOrder(family, {"ProbeEgressSource(family)", "LinuxSourceLeavesTheLink(source)",
                                  "iface == tunnelInterface", "LinuxIpv6Prefix64(source)",
                                  "\"/proc/net/ipv6_route\"", "\"/proc/net/route\"",
                                  "ReadCarrierUps(iface)"}));
  const std::string ups = DeadTunnelBody(host, "std::uint64_t ReadCarrierUps(");
  UR_EXPECT_TRUE(InOrder(ups, {"\"carrier_up_count\"", "\"carrier_changes\""}));
  const std::string tunnel = ReadDeadTunnelSource("Tunnel.cpp");
  const std::string probe = DeadTunnelBody(tunnel, "std::string ProbeEgressSource(int family) {");
  UR_EXPECT_TRUE(DeadTunnelHas(probe, "BindWitnessSocket(family, /*clearMark=*/false,"));

  // A baseline per device, taken right where each is created.
  const std::string start = DeadTunnelBody(host, "void TunnelHost::RunStart(");
  UR_EXPECT_TRUE(
      InOrder(start, {"device_ = NewDeviceLocked(",
                      "networkQualityTracker_.Rebase(ReadNetworkPath(PlannedTunName()));",
                      "device_->setRpcServer("}));
  const std::string provider =
      DeadTunnelBody(host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(
      InOrder(provider, {"providerDevice_ = NewDeviceLocked(",
                         "networkQualityTracker_.Rebase(ReadNetworkPath(std::string()));",
                         "providerDevice_->setProvideControlMode("}));
}

// A machine with no network is not a dead tunnel: the reaper hands the path
// tracker's offline reading to the failsafe at the up edge and on every tick,
// and the watch holds its no-exit rules on it and rebases on the way back.
UR_TEST(DeadTunnelWiring_TheFailsafeHoldsWhileOfflineAndRebasesOnTheReturn) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string start =
      DeadTunnelBody(host, "void TunnelHost::StartDeadTunnelWatchLocked() {");
  UR_EXPECT_TRUE(InOrder(start, {"inputs.offline = networkQualityTracker_.offline();",
                                 "deadTunnelWatch_.Start(inputs);"}));
  const std::string check = DeadTunnelBody(host, "bool TunnelHost::CheckDeadTunnelLocked() {");
  UR_EXPECT_TRUE(InOrder(check, {"inputs.offline = networkQualityTracker_.offline();",
                                 "deadTunnelWatch_.Tick(inputs)",
                                 "tick.offlineStep == watchdog::OfflineStep::BackRebased"}));
  // The reaper judges, then reads the path: the failsafe holds on the reading
  // of the tick before, which the tracker keeps.
  const std::string reap = DeadTunnelBody(host, "void TunnelHost::Reap() {");
  UR_EXPECT_TRUE(InOrder(reap, {"if (CheckDeadTunnelLocked()) return;",
                                "networkQualityTracker_.Observe("}));
  const std::string quality = ReadDeadTunnelSource("NetworkQuality.hpp");
  UR_EXPECT_TRUE(
      DeadTunnelHas(quality, "bool offline() const { return initialized_ && !last_.online(); }"));
  // The watch: the step is decided before anything else in the tick, a long
  // stretch rebases and reaches no verdict, and the verdict is told it is
  // offline and how much offline time each no-exit window holds.
  const std::string watchdog = ReadDeadTunnelSource("TunnelWatchdog.hpp");
  const std::string tick = DeadTunnelBody(watchdog, "WatchTick Tick(const WatchInputs& in) {");
  UR_EXPECT_TRUE(
      InOrder(tick, {"StepOffline(offline_, in.offline, offlineStretchMillis_, settings_)",
                     "if (tick.offlineStep == OfflineStep::BackRebased) {", "Rebase(in);",
                     "return tick;", "epoch_.Observe(", "s.offline = in.offline;",
                     "s.provenOfflineMillis = provenOfflineMillis_;",
                     "s.inboundOfflineMillis = inboundOfflineMillis_;",
                     "tick.verdict = Evaluate(s, now, settings_);"}));
  const std::string evaluate =
      DeadTunnelBody(watchdog, "inline DeadTunnelVerdict Evaluate(const DeadTunnelSignals& s,");
  // Each no-exit rule holds offline, and is off with a window of 0.
  UR_EXPECT_TRUE(DeadTunnelHas(
      evaluate, "const bool fastApplies = settings.deadFastMillis > 0 && !s.offline &&"));
  UR_EXPECT_TRUE(DeadTunnelHas(evaluate, "settings.deadSlowMillis > 0 && !s.offline &&"));
  UR_EXPECT_TRUE(DeadTunnelHas(
      evaluate, "const bool unresponsiveApplies = settings.sdkUnresponsiveMillis > 0;"));
  // The rules' windows are the settings', never a constant of their own.
  UR_EXPECT_TRUE(DeadTunnelHas(evaluate, "inboundAge < settings.deadFastMillis"));
  UR_EXPECT_TRUE(DeadTunnelHas(evaluate, "provenOnlineAge >= settings.deadSlowMillis"));
  UR_EXPECT_TRUE(
      DeadTunnelHas(evaluate, "s.outboundSinceInbound >= settings.deadFastOutboundPackets"));
}

// The countdown's end is logged for the reason it ended: never "carrying
// again" on the tick that ends the session, nor on a rebase.
UR_TEST(DeadTunnelWiring_TheCountdownsEndIsLoggedTruthfully) {
  const std::string host = ReadDeadTunnelSource("daemon/TunnelHost.cpp");
  const std::string check = DeadTunnelBody(host, "bool TunnelHost::CheckDeadTunnelLocked() {");
  UR_EXPECT_TRUE(InOrder(check, {"armedChanged && tick.froze",
                                 "armedChanged && tick.verdict.reason == "
                                 "watchdog::DeadTunnelReason::None",
                                 "the tunnel is carrying again"}));
  UR_EXPECT_FALSE(DeadTunnelHas(check, "} else if (armedChanged) {"));
}

// C++ builds members in the order the class declares them, whatever the
// constructor's list says. deadTunnelWatch_{settings_.dead_tunnel} sat above
// settings_, so the failsafe copied its windows from memory nothing had written:
// in the released arm64 daemon, read at start-up, 16, 0, 169 and pointers where 20,
// 30 and 90 s belong. No test constructs a TunnelHost, because it needs glib and
// the SDK, so the class is read.
UR_TEST(DeadTunnelWiring_EveryMemberBuiltFromTheSettingsIsBuiltAfterThem) {
  const std::string header = ReadDeadTunnelSource("daemon/TunnelHost.hpp");
  const std::string declaration = "const HostSettings settings_;";
  const size_t settings = header.find(declaration);
  UR_EXPECT_TRUE(settings != std::string::npos);
  // Nothing in the class reads them before they are declared.
  UR_EXPECT_TRUE_MSG("settings_ is read above its declaration",
                     header.find("settings_") == settings + declaration.find("settings_"));
  // The ones that are built from them, which the search above has to have passed.
  for (const char* const member :
       {"deadTunnelWatch_{settings_.dead_tunnel};",
        "networkQualityTracker_{settings_.network_path};",
        "suspendDetector_{settings_.suspend};"}) {
    const size_t at = header.find(member);
    if (at == std::string::npos || at < settings) UR_FAIL(member);
  }
  // First of the private members, so no later edit needs to know the order.
  const size_t privateAt = header.find(" private:\n", header.find("class TunnelHost {"));
  UR_EXPECT_TRUE(privateAt != std::string::npos);
  size_t firstMember = privateAt + std::string(" private:\n").size();
  while (firstMember < header.size() &&
         (header[firstMember] == ' ' || header[firstMember] == '\n')) {
    ++firstMember;
  }
  UR_EXPECT_TRUE_MSG("settings_ opens the private section", firstMember == settings);
}
