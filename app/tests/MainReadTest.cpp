// No read the GTK main loop makes waits on SdkHost's mutex_ while a command
// holds it across an rpc (MainRead.hpp). The policy is pure and read here;
// the call sites are read from the sources: every public getter that takes
// mutex_ answers through SdkHost::ReadOnMain, IsLoggedIn takes no lock at all,
// a synchronous save keeps what it saved for the reads before the queue
// settles, and the Developer page's bridge holds the lock while it uses the
// device. Before, each of those getters took mutex_ with scoped_lock on
// whatever thread called it, so the window waited for a held command: the
// Settings, Account, Network and Developer pages, the location chooser, the
// editors' Open, IsLoggedIn, the balance recovery's selection and the Pro
// upgrade's provide mode.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>
#include <vector>

#include "MainRead.hpp"

namespace {

namespace wiring = urnw::testing::wiring;
using urnw::main_read::Source;
using urnw::main_read::SourceFor;

// The getters that take mutex_, every one through ReadOnMain now.
const std::vector<std::string>& MainReadGetters() {
  static const std::vector<std::string> names = {
      "CurrentNetworkServer", "GetVlessSettings", "GetControlDohUrls", "HasPendingWalletAuth",
      "ParseByJwt", "HasPendingSsoAuth", "LastTunnelError", "RpcHostPort", "CurrentConnectReading",
      "ProviderStatusNow", "SelectedLocation", "ConnectedLocation", "GetPerformanceProfile",
      "GetBlockerEnabled", "GetRouteLocal", "CurrentKillSwitch", "CurrentKillSwitchStatus",
      "GetDnsResolverSettings", "ThroughputPoints", "ThroughputWindowSeconds",
      "ClientTransportDistribution", "ProviderTransportDistribution", "ProviderThroughputPoints",
      "ExtenderThroughputPoints", "HasProviderStats", "DeviceHasProviderStats",
      "GetTransportSettings", "GetProviderTransportSettings", "GetTransportStatus",
      "GetProviderTransportStatus", "BlockActions", "BlockStatsSnapshot", "BlockActionOverrides",
      "ClientId", "ContractRows", "ContractsPendingCount", "GetFilteredLocations",
      "GetFilteredLocationState", "ConnectedProvidePeers", "ConnectedPeerCount",
      "ProviderIdentities", "ConnectedProviderLocations", "SelectedProviderClientId",
      "GetExtenderStatus", "GetExtenderProvideStatus", "ProviderExtenderProvideStatus",
      "GetProvideExtender", "MeasurementsSwitchWritable", "GetMeasurementsEnabled",
      "GetExtenderSettings", "GetPrivateExtender", "GetProvideControlMode", "ProvideEnabled"};
  return names;
}

}  // namespace

UR_TEST(MainRead_OffTheMainLoopAReadWaitsForTheLock) {
  for (const bool busy : {false, true}) {
    for (const bool lockFree : {false, true}) {
      UR_EXPECT_TRUE(SourceFor(/*onMainLoop=*/false, busy, lockFree) == Source::Locked);
    }
  }
}

UR_TEST(MainRead_OnTheMainLoopAReadNeverWaits) {
  // commands outstanding: a command may hold the lock, and an edit queued
  // from the main loop may not have applied, so its kept value answers
  UR_EXPECT_TRUE(SourceFor(true, /*commandsBusy=*/true, /*lockFree=*/true) == Source::Snapshot);
  UR_EXPECT_TRUE(SourceFor(true, /*commandsBusy=*/true, /*lockFree=*/false) == Source::Snapshot);
  // the lock taken (a worker, a listener thread's read): no wait either
  UR_EXPECT_TRUE(SourceFor(true, /*commandsBusy=*/false, /*lockFree=*/false) == Source::Snapshot);
  // idle and free: a fresh read, which is kept
  UR_EXPECT_TRUE(SourceFor(true, /*commandsBusy=*/false, /*lockFree=*/true) == Source::Locked);
}

UR_TEST(MainReadWiring_EveryLockedGetterAnswersThroughReadOnMain) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  for (const std::string& name : MainReadGetters()) {
    const size_t at = host.find("SdkHost::" + name + "() {");
    UR_EXPECT_TRUE_MSG(name, at != std::string::npos);
    if (at == std::string::npos) continue;
    const std::string body = wiring::FunctionBody(host, host.substr(at, name.size() + 13));
    UR_EXPECT_TRUE_MSG(name, wiring::Contains(body, "return ReadOnMain<"));
    UR_EXPECT_TRUE_MSG(name, wiring::Contains(body, "(\"" + name + "\", [&]()"));
    UR_EXPECT_FALSE(wiring::Before(body, "std::scoped_lock lock(mutex_);", "ReadOnMain<"));
  }
  // the read: off the main loop under the lock, on it by the pure policy,
  // with a try_lock and never a wait
  const std::string read = wiring::FunctionBody(host, "T SdkHost::ReadOnMain(const char* key,");
  UR_EXPECT_TRUE(wiring::Before(read, "if (!OnMainLoop()) {", "std::scoped_lock lock(mutex_);"));
  UR_EXPECT_TRUE(wiring::Before(read, "lock.try_lock()", "main_read::SourceFor("));
  UR_EXPECT_EQ(1, int(wiring::CountOf(read, "scoped_lock")));
}

UR_TEST(MainReadWiring_IsLoggedInTakesNoLock) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string loggedIn = wiring::FunctionBody(host, "bool SdkHost::IsLoggedIn() {");
  UR_EXPECT_FALSE(wiring::Contains(loggedIn, "mutex_"));
  UR_EXPECT_TRUE(wiring::Contains(loggedIn, "return !signedOut_.load() && hasClientJwt_.load();"));
  // the mirror follows every place the stored client jwt changes
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(host, "bool SdkHost::Initialize("),
      "hasClientJwt_.store(!localState_->getByClientJwt().empty());"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "bool SdkHost::ApplyNetworkServer("),
                                  "hasClientJwt_.store(loggedIn);"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::RegisterNetworkClient("),
                                  "hasClientJwt_.store(true);"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::LogoutNow() {"),
                                  "hasClientJwt_.store(false);"));
}

UR_TEST(MainReadWiring_ASynchronousSaveKeepsWhatItSaved) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::pair<const char*, const char*> saves[] = {
      {"std::optional<std::string> SdkHost::SetVlessSettings(", "(\"GetVlessSettings\","},
      {"std::optional<std::string> SdkHost::SetControlDohUrls(", "\"GetControlDohUrls\","},
      {"std::optional<urnet::ExtenderSettings> SdkHost::SetExtenderSettings(", "(\"GetExtenderSettings\","},
      {"bool SdkHost::SetPrivateExtender(", "(\"GetPrivateExtender\","}};
  for (const auto& [signature, key] : saves) {
    const std::string body = wiring::FunctionBody(host, signature);
    UR_EXPECT_TRUE_MSG(signature, wiring::Before(body, "KeepOnMain<", key));
  }
}

UR_TEST(MainReadWiring_TheDeveloperBridgeHoldsTheLockOverTheDevice) {
  const std::string page = wiring::ReadCode("DeveloperPage.cpp");
  UR_EXPECT_FALSE(wiring::Contains(page, "host_.device()"));
  UR_EXPECT_FALSE(wiring::Contains(page, "host_.hasDevice()"));
  UR_EXPECT_EQ(6, int(wiring::CountOf(page, "host_.LockedDevice(deviceLock)")));
  // a block's lock ends before the snapshot takes it again
  UR_EXPECT_EQ(4, int(wiring::CountOf(page, "if (std::unique_lock<std::mutex> deviceLock;")));
  UR_EXPECT_TRUE(wiring::Before(
      wiring::FunctionBody(page, "void DeveloperPage::EditSettings(std::function<void(urnet::ReliabilitySettings&)> mutate) {"),
      "if (deviceLock.owns_lock()) deviceLock.unlock();", "const Snapshot snap = ReadSnapshot();"));
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE(wiring::Before(
      wiring::FunctionBody(host, "urnet::DeviceRemote* SdkHost::LockedDevice(std::unique_lock<std::mutex>& lock) {"),
      "lock = std::unique_lock<std::mutex>(mutex_);", "return &*device_;"));
}
