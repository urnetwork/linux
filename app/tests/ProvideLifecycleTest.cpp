// The provider lifecycle (ProvideLifecycle.hpp): connected or disconnected x
// provide mode -> does this device provide, at which tier, and what the GUI
// asks the daemon while it has no tunnel session (support inbox 1521, P008).
// Before the fix Linux provided only while connected, in every mode.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "ProvideLifecycle.hpp"

namespace provide = urnw::provide;

namespace {

struct LifecycleRow {
  const char* mode;
  bool connected;
  bool runs;
  provide::Tier tier;
};

// The sdk's DeviceLocal.applyProvideControlModeWithLock, row by row.
const LifecycleRow kLifecycle[] = {
    {"never", true, false, provide::Tier::None},
    {"never", false, false, provide::Tier::None},
    {"always", true, true, provide::Tier::Public},
    {"always", false, true, provide::Tier::Public},
    {"network", true, true, provide::Tier::Network},
    {"network", false, true, provide::Tier::Network},
    {"auto", true, true, provide::Tier::Public},
    {"auto", false, true, provide::Tier::Network},
    // never offered by this app; its explicit provide mode lives in a device
    // that does not exist while disconnected
    {"manual", true, false, provide::Tier::None},
    {"manual", false, false, provide::Tier::None},
    {"", false, false, provide::Tier::None},
    {"ALWAYS", false, false, provide::Tier::None},
};

provide::DaemonProviderFacts IdleDaemon() {
  provide::DaemonProviderFacts facts;
  facts.answered = true;
  return facts;
}

provide::DaemonProviderFacts RunningProvider(const char* mode) {
  provide::DaemonProviderFacts facts = IdleDaemon();
  facts.providerRunning = true;
  facts.ownerConnected = true;
  facts.providerControlMode = mode;
  return facts;
}

}  // namespace

UR_TEST(provideLifecycleConnectedOrNotByMode) {
  for (const LifecycleRow& row : kLifecycle) {
    const provide::ControlMode mode = provide::ControlModeFrom(row.mode);
    const std::string label = std::string(row.mode) + (row.connected ? " connected" : " disconnected");
    UR_EXPECT_TRUE_MSG(label, provide::ProviderRuns(mode, row.connected) == row.runs);
    UR_EXPECT_TRUE_MSG(label, provide::SdkTierFor(mode, row.connected) == row.tier);
  }
}

// The defect, as one assertion each: disconnecting must not end providing for
// the modes that provide while disconnected.
UR_TEST(provideLifecycleDisconnectingKeepsAlwaysNetworkAndAuto) {
  UR_EXPECT_TRUE(provide::ProviderRuns(provide::ControlMode::Always, /*connected=*/false));
  UR_EXPECT_TRUE(provide::ProviderRuns(provide::ControlMode::Network, /*connected=*/false));
  UR_EXPECT_TRUE(provide::ProviderRuns(provide::ControlMode::Auto, /*connected=*/false));
  UR_EXPECT_FALSE(provide::ProviderRuns(provide::ControlMode::Never, /*connected=*/false));
}

// Auto shares publicly only while connected and with the user's own devices
// otherwise; Always stays public.
UR_TEST(provideLifecycleTiersMatchTheSdk) {
  UR_EXPECT_EQ(3, static_cast<int64_t>(provide::Tier::Public));
  UR_EXPECT_EQ(1, static_cast<int64_t>(provide::Tier::Network));
  UR_EXPECT_EQ(0, static_cast<int64_t>(provide::Tier::None));
  UR_EXPECT_TRUE(provide::SdkTierFor(provide::ControlMode::Auto, false) ==
                 provide::Tier::Network);
  UR_EXPECT_TRUE(provide::SdkTierFor(provide::ControlMode::Auto, true) == provide::Tier::Public);
  UR_EXPECT_TRUE(provide::SdkTierFor(provide::ControlMode::Always, false) ==
                 provide::Tier::Public);
}

UR_TEST(provideLifecycleModeNamesAreTheWireStrings) {
  for (const char* mode : {"never", "always", "network", "auto"}) {
    UR_EXPECT_TRUE_MSG(mode, std::string(provide::ToString(provide::ControlModeFrom(mode))) == mode);
  }
  UR_EXPECT_TRUE(provide::ControlModeFrom("manual") == provide::ControlMode::Unknown);
  UR_EXPECT_TRUE(std::string(provide::ToString(provide::ControlMode::Unknown)) == "unknown");
}

UR_TEST(provideStepStartsTheProviderAfterDisconnectOrLaunch) {
  for (const char* mode : {"always", "network", "auto"}) {
    UR_EXPECT_TRUE_MSG(mode, provide::DisconnectedProviderStep(mode, IdleDaemon()) ==
                                 provide::DisconnectedStep::Start);
  }
  for (const char* mode : {"never", "manual", ""}) {
    UR_EXPECT_TRUE_MSG(mode, provide::DisconnectedProviderStep(mode, IdleDaemon()) ==
                                 provide::DisconnectedStep::None);
  }
}

UR_TEST(provideStepNeverStopsARunningProvider) {
  UR_EXPECT_TRUE(provide::DisconnectedProviderStep("never", RunningProvider("always")) ==
                 provide::DisconnectedStep::Stop);
  UR_EXPECT_TRUE(provide::DisconnectedProviderStep("manual", RunningProvider("auto")) ==
                 provide::DisconnectedStep::Stop);
}

UR_TEST(provideStepAppliesAChangedModeAndKeepsAMatchingOne) {
  UR_EXPECT_TRUE(provide::DisconnectedProviderStep("auto", RunningProvider("always")) ==
                 provide::DisconnectedStep::Apply);
  UR_EXPECT_TRUE(provide::DisconnectedProviderStep("always", RunningProvider("always")) ==
                 provide::DisconnectedStep::None);
  // an edited provider policy needs a new device
  UR_EXPECT_TRUE(provide::DisconnectedProviderStep("always", RunningProvider("always"),
                                                   /*settingsChanged=*/true) ==
                 provide::DisconnectedStep::Start);
}

// A relaunched GUI finds the provider its previous run left with no owner and
// adopts it with an idempotent start, so the session is owned again.
UR_TEST(provideStepAdoptsAnOwnerlessProvider) {
  provide::DaemonProviderFacts facts = RunningProvider("always");
  facts.ownerConnected = false;
  UR_EXPECT_TRUE(provide::DisconnectedProviderStep("always", facts) ==
                 provide::DisconnectedStep::Start);
}

// Never beside a tunnel session (its device provides), never on another user's
// session, never on no answer, and not while the kill switch holds the machine.
UR_TEST(provideStepLeavesTunnelsOtherUsersAndTheArmedFloorAlone) {
  provide::DaemonProviderFacts tunnel = IdleDaemon();
  tunnel.tunnelSession = true;
  provide::DaemonProviderFacts foreign = IdleDaemon();
  foreign.redacted = true;
  provide::DaemonProviderFacts unknown;
  provide::DaemonProviderFacts armed = IdleDaemon();
  armed.killSwitchArmed = true;
  for (const char* mode : {"always", "network", "auto", "never"}) {
    UR_EXPECT_TRUE_MSG(mode, provide::DisconnectedProviderStep(mode, tunnel) ==
                                 provide::DisconnectedStep::None);
    UR_EXPECT_TRUE_MSG(mode, provide::DisconnectedProviderStep(mode, foreign) ==
                                 provide::DisconnectedStep::None);
    UR_EXPECT_TRUE_MSG(mode, provide::DisconnectedProviderStep(mode, unknown) ==
                                 provide::DisconnectedStep::None);
    UR_EXPECT_TRUE_MSG(mode, provide::DisconnectedProviderStep(mode, armed) ==
                                 provide::DisconnectedStep::None);
  }
}

UR_TEST(provideStepBackoffDoublesToFiveMinutesAndResets) {
  provide::ProviderStepBackoff backoff;
  UR_EXPECT_TRUE(backoff.Allows(0));
  backoff.NoteFailure(1000);
  UR_EXPECT_EQ(5000, backoff.DelayMillis());
  UR_EXPECT_FALSE(backoff.Allows(5999));
  UR_EXPECT_TRUE(backoff.Allows(6000));
  backoff.NoteFailure(6000);
  UR_EXPECT_EQ(10000, backoff.DelayMillis());
  UR_EXPECT_FALSE(backoff.Allows(15999));
  for (int i = 0; i < 20; ++i) backoff.NoteFailure(20000);
  UR_EXPECT_EQ(provide::ProviderStepBackoff::kMaxDelayMillis, backoff.DelayMillis());
  backoff.NoteSuccess();
  UR_EXPECT_EQ(0, backoff.DelayMillis());
  UR_EXPECT_TRUE(backoff.Allows(20000));
}

// ---- the provider-only device's statistics (provider_stats) ----------------

// Disconnected, the provider statistics come from the daemon's provider-only
// device; a tunnel session's DeviceRemote is the source while one is bound,
// and with no provider-only device (or a daemon that predates the verb) there
// is nothing to read.
UR_TEST(provideStatsFetchedOnlyFromARunningProviderWithoutASession) {
  UR_EXPECT_TRUE(provide::DaemonProviderStatsStep(/*sessionDevice=*/false,
                                                  /*providerRunning=*/true,
                                                  /*verbUnsupported=*/false) ==
                 provide::ProviderStatsStep::Fetch);
  UR_EXPECT_TRUE(provide::DaemonProviderStatsStep(true, true, false) ==
                 provide::ProviderStatsStep::Drop);
  UR_EXPECT_TRUE(provide::DaemonProviderStatsStep(false, false, false) ==
                 provide::ProviderStatsStep::Drop);
  UR_EXPECT_TRUE(provide::DaemonProviderStatsStep(false, true, true) ==
                 provide::ProviderStatsStep::Drop);
  UR_EXPECT_TRUE(provide::DaemonProviderStatsStep(true, false, true) ==
                 provide::ProviderStatsStep::Drop);
}

// The daemon polls the provider status only while a GUI that shows it keeps
// asking: the first ask starts the controller, later asks only renew, and the
// controller stops once, after a lease with no ask.
UR_TEST(provideStatusLeaseStartsOnceAndStopsAfterTheLastAsk) {
  provide::ProviderStatusLease lease;
  UR_EXPECT_FALSE(lease.Held());
  UR_EXPECT_FALSE(lease.Expire(0));  // nothing to stop
  UR_EXPECT_TRUE(lease.Renew(1000));
  UR_EXPECT_TRUE(lease.Held());
  UR_EXPECT_FALSE(lease.Renew(2000));  // already polling
  const int64_t lastAsk = 2000;
  UR_EXPECT_FALSE(lease.Expire(lastAsk + provide::ProviderStatusLease::kLeaseMillis - 1));
  UR_EXPECT_TRUE(lease.Expire(lastAsk + provide::ProviderStatusLease::kLeaseMillis));
  UR_EXPECT_FALSE(lease.Held());
  UR_EXPECT_FALSE(lease.Expire(lastAsk + 10 * provide::ProviderStatusLease::kLeaseMillis));
  // the next ask starts it again
  UR_EXPECT_TRUE(lease.Renew(60000));
}

// A rebuilt device has a new controller, which polls only once asked again.
UR_TEST(provideStatusLeaseStartsANewControllerAfterARelease) {
  provide::ProviderStatusLease lease;
  UR_EXPECT_TRUE(lease.Renew(1000));
  lease.Release();
  UR_EXPECT_FALSE(lease.Held());
  UR_EXPECT_FALSE(lease.Expire(1000000));
  UR_EXPECT_TRUE(lease.Renew(1500));
}

// The connect page's Extender switch while disconnected writes the setting
// where the next start reads it: through the device that runs, and with none
// into the space the last device ran in. Only a daemon with nothing to keep it
// in refuses, and never says it wrote.
UR_TEST(provideExtenderSettingGoesWhereTheNextStartReadsIt) {
  struct Row {
    bool providerDevice;
    bool sessionDevice;
    bool lastSpace;
    provide::ExtenderSettingTarget want;
  };
  const Row kRows[] = {
      {true, false, true, provide::ExtenderSettingTarget::ProviderDevice},
      {true, false, false, provide::ExtenderSettingTarget::ProviderDevice},
      {false, true, true, provide::ExtenderSettingTarget::SessionDevice},
      {false, true, false, provide::ExtenderSettingTarget::SessionDevice},
      {false, false, true, provide::ExtenderSettingTarget::NetworkSpace},
      {false, false, false, provide::ExtenderSettingTarget::None},
      // never both in the daemon; the provider-only device wins the tie
      {true, true, true, provide::ExtenderSettingTarget::ProviderDevice},
  };
  for (const Row& row : kRows) {
    const provide::ExtenderSettingTarget got =
        provide::ExtenderSettingTargetFor(row.providerDevice, row.sessionDevice, row.lastSpace);
    UR_EXPECT_TRUE_MSG(provide::ToString(row.want), got == row.want);
  }
  for (const provide::ExtenderSettingTarget target :
       {provide::ExtenderSettingTarget::ProviderDevice,
        provide::ExtenderSettingTarget::SessionDevice,
        provide::ExtenderSettingTarget::NetworkSpace, provide::ExtenderSettingTarget::None}) {
    UR_EXPECT_TRUE(std::string(provide::ToString(target)).size() > 5);
  }
}

// The switch reads and writes a bound device whenever there is one, and with
// none the provider-only device only while the daemon said it takes the write:
// a daemon that predates the verb leaves the switch hidden (N1).
UR_TEST(provideExtenderSwitchShowsOnlyWhereItsWriteIsTaken) {
  for (const bool reading : {false, true}) {
    for (const bool writes : {false, true}) {
      UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(/*sessionDevice=*/true, reading, writes) ==
                     provide::ExtenderSwitchSource::Device);
    }
  }
  UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(false, true, true) ==
                 provide::ExtenderSwitchSource::Daemon);
  UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(false, true, false) ==
                 provide::ExtenderSwitchSource::None);
  UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(false, false, true) ==
                 provide::ExtenderSwitchSource::None);
  UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(false, false, false) ==
                 provide::ExtenderSwitchSource::None);
}

// The lag.dev speed tests switch under it (lag.dev/PLAN.md C4): a bound device
// whenever there is one, and with none the provider-only device only while the
// daemon said it takes both writes. A daemon that takes only the Extender
// switch's leaves this switch hidden (N1), never dead.
UR_TEST(provideMeasurementsSwitchShowsOnlyWhereItsWriteIsTaken) {
  for (const bool reading : {false, true}) {
    for (const bool extender : {false, true}) {
      for (const bool measurements : {false, true}) {
        UR_EXPECT_TRUE(provide::MeasurementsSwitchSourceFor(/*sessionDevice=*/true, reading,
                                                            extender, measurements) ==
                       provide::ExtenderSwitchSource::Device);
        const bool daemon = reading && extender && measurements;
        UR_EXPECT_TRUE(provide::MeasurementsSwitchSourceFor(false, reading, extender,
                                                            measurements) ==
                       (daemon ? provide::ExtenderSwitchSource::Daemon
                               : provide::ExtenderSwitchSource::None));
        // never with no Extender switch to sit under
        if (provide::MeasurementsSwitchSourceFor(false, reading, extender, measurements) !=
            provide::ExtenderSwitchSource::None) {
          UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(false, reading, extender) !=
                         provide::ExtenderSwitchSource::None);
        }
      }
    }
  }
}

// The router port mapping switch under the speed tests switch (connect
// EXTENDER.md S8), by the same rule: a bound device whenever there is one, and
// with none the provider-only device only while the daemon said it takes both
// the Extender switch's write and this one. A daemon that takes only the
// Extender switch's, or the speed tests switch's too, leaves this switch hidden
// (N1), never dead.
UR_TEST(providePortMappingSwitchShowsOnlyWhereItsWriteIsTaken) {
  for (const bool reading : {false, true}) {
    for (const bool extender : {false, true}) {
      for (const bool portMapping : {false, true}) {
        UR_EXPECT_TRUE(provide::PortMappingSwitchSourceFor(/*sessionDevice=*/true, reading,
                                                           extender, portMapping) ==
                       provide::ExtenderSwitchSource::Device);
        const bool daemon = reading && extender && portMapping;
        UR_EXPECT_TRUE(provide::PortMappingSwitchSourceFor(false, reading, extender,
                                                           portMapping) ==
                       (daemon ? provide::ExtenderSwitchSource::Daemon
                               : provide::ExtenderSwitchSource::None));
        // never with no Extender switch to sit under
        if (provide::PortMappingSwitchSourceFor(false, reading, extender, portMapping) !=
            provide::ExtenderSwitchSource::None) {
          UR_EXPECT_TRUE(provide::ExtenderSwitchSourceFor(false, reading, extender) !=
                         provide::ExtenderSwitchSource::None);
        }
      }
    }
  }
  // a daemon that takes the speed tests switch's write but predates this one
  UR_EXPECT_TRUE(provide::MeasurementsSwitchSourceFor(false, true, true, true) ==
                 provide::ExtenderSwitchSource::Daemon);
  UR_EXPECT_TRUE(provide::PortMappingSwitchSourceFor(false, true, true, false) ==
                 provide::ExtenderSwitchSource::None);
}

// ---- one device across connect and disconnect -------------------------------

// A Connect takes the provider-only device only when it was built for the
// start's identity and every socket it made carries the egress mark.
UR_TEST(provideConnectPromotesOnlyAMarkedDeviceOfTheSameIdentity) {
  const provide::DeviceIdentity provider{"client:n/c", "{}", "instance", "2026.11.4"};
  provide::DeviceIdentity start = provider;
  UR_EXPECT_TRUE(provide::PromotesProviderDevice(true, true, provider, start));
  UR_EXPECT_FALSE(provide::PromotesProviderDevice(false, true, provider, start));
  // a socket made unmarked would follow the session's routes into the tun
  UR_EXPECT_FALSE(provide::PromotesProviderDevice(true, false, provider, start));
  start.jwtSubject = "client:n/other";
  UR_EXPECT_FALSE(provide::PromotesProviderDevice(true, true, provider, start));
  start = provider;
  start.networkSpaceJson = R"({"values":{}})";
  UR_EXPECT_FALSE(provide::PromotesProviderDevice(true, true, provider, start));
  start = provider;
  start.instanceId = "another";
  UR_EXPECT_FALSE(provide::PromotesProviderDevice(true, true, provider, start));
  start = provider;
  start.appVersion = "2026.11.5";
  UR_EXPECT_FALSE(provide::PromotesProviderDevice(true, true, provider, start));
  UR_EXPECT_TRUE(provide::DifferingIdentityFields(provider, start) == "app_version");
  UR_EXPECT_TRUE(provide::DifferingIdentityFields(provider, provider).empty());
}

// A Disconnect keeps the session's device exactly when a provider-only device
// could run in its place: the user's own Disconnect, a mode that provides while
// disconnected, and nothing of the firewall left in force.
UR_TEST(provideDisconnectKeepsTheDeviceOnlyWhereAProviderWouldRun) {
  provide::SessionStopFacts facts;
  facts.sessionDevice = true;
  facts.disconnect = true;
  facts.mode = provide::ControlMode::Always;
  UR_EXPECT_TRUE(provide::KeepsSessionDevice(facts));
  facts.mode = provide::ControlMode::Network;
  UR_EXPECT_TRUE(provide::KeepsSessionDevice(facts));
  facts.mode = provide::ControlMode::Auto;
  UR_EXPECT_TRUE(provide::KeepsSessionDevice(facts) ==
                 provide::ProviderRuns(provide::ControlMode::Auto, /*connected=*/false));
  facts.mode = provide::ControlMode::Never;
  UR_EXPECT_FALSE(provide::KeepsSessionDevice(facts));
  facts.mode = provide::ControlMode::Unknown;
  UR_EXPECT_FALSE(provide::KeepsSessionDevice(facts));

  facts.mode = provide::ControlMode::Always;
  provide::SessionStopFacts other = facts;
  other.disconnect = false;  // a quit, a sign-out, the failsafe, another user
  UR_EXPECT_FALSE(provide::KeepsSessionDevice(other));
  other = facts;
  other.sessionDevice = false;
  UR_EXPECT_FALSE(provide::KeepsSessionDevice(other));
  other = facts;
  other.firewallInForce = true;  // start_provider refuses under the floor
  UR_EXPECT_FALSE(provide::KeepsSessionDevice(other));
}
