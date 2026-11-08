// A device the platform holds off for its network's concurrent client limit is
// never stopped by the dead-tunnel failsafe (owner, 2026-11-08; TunnelWatchdog.hpp):
// its traffic stays captured, the app's notice says what to do, and Disconnect is
// the way out.
//
// The root cause these pin: the watch knew nothing of the device's client limit
// hold (ClientLimitStatus), so a session the sdk held off the platform for 15 to
// 20 minutes read as a dead tunnel, and NoExit stopped it at its window. The stop
// let the traffic out with the kill switch off, and the device that took the
// session over dialed the platform at once, past the hold the sdk keeps per
// device: the provider-only device the GUI's health poll brings back after the
// drop, or the user's next Connect. Each was another refusal.
//
// Every session here replays what the daemon's reaper reads, a simulated second
// at a time: the sampler's client limit status, window status and exits, and the
// tun's counters.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>

#include "TunnelWatchdog.hpp"

namespace wd = urnw::watchdog;

namespace {

const wd::DeadTunnelSettings kClientLimitSettings{};

// What the sampler reads each interval, and what the tun's counters do between.
struct ClientLimitReadings {
  // WindowStatus and the exits.
  int64_t generation = 1;
  int64_t added = 0;
  int64_t proven = 0;
  bool minSatisfied = false;
  // ClientLimitStatus: the platform closed the device for its network's client
  // limit; unread for a status that could not be read.
  bool exceeded = false;
  bool clientLimitRead = true;
  // The sampler answers at all (a wedged SDK does not).
  bool answering = true;
  // Packets a tick into the tunnel, and back out of it.
  uint64_t outPerTick = 50;
  uint64_t inPerTick = 0;
};

// A session as the reaper sees it, the sampler's readings refreshed on the
// interval it takes them.
struct HeldSession {
  wd::DeadTunnelWatch watch;
  wd::WatchInputs in;
  ClientLimitReadings readings;
  int64_t ticks = 0;
  bool armedEver = false;

  HeldSession() : watch(kClientLimitSettings) {
    in.nowMillis = 1000000;
    in.tunnelUp = true;
    in.routesInstalled = true;
    in.windowSampled = false;
    watch.Start(in);
  }

  void Sample() {
    if (!readings.answering) return;
    in.lastSampleMillis = in.nowMillis;
    in.provenCount = readings.proven;
    if (readings.proven > 0) in.lastProvenMillis = in.nowMillis;
    in.connectionGeneration = readings.generation;
    in.providerWindowMinSatisfied = readings.minSatisfied;
    in.windowSampled = true;
    if (readings.clientLimitRead) {
      in.clientLimit.sampled = true;
      in.clientLimit.exceeded = readings.exceeded;
    }
  }

  wd::WatchTick Tick(int64_t stepMillis = kClientLimitSettings.evaluateIntervalMillis) {
    in.nowMillis += stepMillis;
    in.outboundPackets += readings.outPerTick;
    in.inboundPackets += readings.inPerTick;
    // As the reaper does: the verdict reads the sample before this tick.
    const wd::WatchTick tick = watch.Tick(in);
    if (++ticks % (kClientLimitSettings.sdkSampleIntervalMillis /
                   kClientLimitSettings.evaluateIntervalMillis) ==
        0) {
      Sample();
    }
    if (tick.verdict.armed) armedEver = true;
    return tick;
  }

  // Ticks until a verdict or `millis` have passed: the elapsed time at the
  // verdict and its reason, or -1 for none.
  int64_t RunUntilDead(int64_t millis, wd::DeadTunnelReason* reason = nullptr) {
    for (int64_t elapsed = kClientLimitSettings.evaluateIntervalMillis; elapsed <= millis;
         elapsed += kClientLimitSettings.evaluateIntervalMillis) {
      const wd::WatchTick tick = Tick();
      if (tick.verdict.reason != wd::DeadTunnelReason::None) {
        if (reason != nullptr) *reason = tick.verdict.reason;
        return elapsed;
      }
    }
    return -1;
  }

  void Run(int64_t millis) {
    for (int64_t elapsed = 0; elapsed < millis;
         elapsed += kClientLimitSettings.evaluateIntervalMillis) {
      Tick();
    }
  }
};

// A session whose window formed and carried, up for a minute.
void FormAndCarryHeld(HeldSession& session) {
  session.readings.added = 2;
  session.readings.proven = 2;
  session.readings.minSatisfied = true;
  session.readings.inPerTick = 40;
  session.Sample();
  session.Run(60000);
}

const int64_t kClientLimitLong = 10 * kClientLimitSettings.deadSlowMillis;
// The sdk's hold: 15 minutes at least (connect/transport_client_limit.go).
const int64_t kClientLimitHold = 15 * 60000;

}  // namespace

// ---- the rule ---------------------------------------------------------------

// A session the platform holds off from its first dial: the window never forms,
// the sampler answers, and this machine keeps sending. Past NoExit's window, ten
// times over, it is not stopped, and no countdown ever said it would be.
UR_TEST(FailsafeClientLimit_NoExitPastItsWindowIsNotStopped) {
  HeldSession session;
  session.readings.exceeded = true;
  session.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(-1, session.RunUntilDead(kClientLimitLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(session.armedEver);
}

// A session held off once up: its window had formed, so NoInbound is the rule in
// reach once the exits go. Neither it nor NoExit stops the session.
UR_TEST(FailsafeClientLimit_NoInboundPastItsWindowIsNotStopped) {
  HeldSession session;
  FormAndCarryHeld(session);
  session.readings.exceeded = true;
  session.readings.added = 0;
  session.readings.proven = 0;
  session.readings.inPerTick = 0;
  session.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(-1, session.RunUntilDead(kClientLimitLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(session.armedEver);
}

// The same session never held is stopped exactly as documented: the hold took
// nothing away from a dead tunnel the limit has no part in.
UR_TEST(FailsafeClientLimit_TheSameSessionNotHeldIsStopped) {
  HeldSession never;
  never.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kClientLimitSettings.deadSlowMillis, never.RunUntilDead(kClientLimitLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
  UR_EXPECT_TRUE(never.armedEver);
}

// ---- the hold ending ----------------------------------------------------------

// The device's hold ends and it dials again. The session is owed the normal
// window: the rule the hold kept back does not fire on the tick it ends, and a
// tunnel still dead a full window later is stopped.
UR_TEST(FailsafeClientLimit_TheHoldEndingStartsEveryWindowAgain) {
  HeldSession session;
  session.readings.exceeded = true;
  session.Sample();
  session.Run(kClientLimitHold);

  session.readings.exceeded = false;
  session.Sample();
  const wd::WatchTick released = session.Tick();
  UR_EXPECT_TRUE(released.clientLimitStep == wd::ClientLimitStep::Released);
  UR_EXPECT_FALSE(released.clientLimitHeld);
  UR_EXPECT_TRUE(released.verdict.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(released.verdict.armed);

  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kClientLimitSettings.deadSlowMillis,
               session.RunUntilDead(kClientLimitLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
}

// And the usual ending once a slot is free: the window forms, so nothing fires.
UR_TEST(FailsafeClientLimit_TheHoldEndingWithAWindowStopsNothing) {
  HeldSession session;
  session.readings.exceeded = true;
  session.Sample();
  session.Run(kClientLimitHold);

  session.readings.exceeded = false;
  session.readings.added = 2;
  session.readings.proven = 2;
  session.readings.minSatisfied = true;
  session.readings.inPerTick = 40;
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kClientLimitLong));
}

// The platform refuses the device again each time its hold ends: a few seconds
// dialing out of every hold, round after round. No round costs a stop or shows a
// countdown.
UR_TEST(FailsafeClientLimit_AHoldThatReturnsEachTimeItEndsCostsNoStop) {
  HeldSession session;
  for (int round = 0; round < 8; ++round) {
    session.readings.exceeded = true;
    session.Sample();
    UR_EXPECT_EQ(-1, session.RunUntilDead(kClientLimitHold));
    session.readings.exceeded = false;
    session.Sample();
    UR_EXPECT_EQ(-1, session.RunUntilDead(5000));
  }
  UR_EXPECT_FALSE(session.armedEver);
}

// ---- the readings that cannot tell -----------------------------------------------

// A status that can no longer be read says nothing about the hold, so it keeps.
UR_TEST(FailsafeClientLimit_AnUnreadableStatusKeepsTheHold) {
  HeldSession session;
  session.readings.exceeded = true;
  session.Sample();
  session.Run(60000);
  session.readings.clientLimitRead = false;
  session.readings.exceeded = false;
  session.in.clientLimit = wd::ClientLimitReading{};
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kClientLimitLong));
  UR_EXPECT_FALSE(session.armedEver);
}

// A status never readable at all is judged by the rules: there is nothing to tell
// a dead tunnel from a held device by, which is the failure the failsafe is for.
UR_TEST(FailsafeClientLimit_AStatusNeverReadIsJudgedAsDocumented) {
  HeldSession session;
  session.readings.clientLimitRead = false;
  session.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kClientLimitSettings.deadSlowMillis,
               session.RunUntilDead(kClientLimitLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
}

// A suspend restarts every window and keeps the hold, and a new session starts
// it again.
UR_TEST(FailsafeClientLimit_TheHoldOutlivesARebaseNotASession) {
  HeldSession session;
  session.readings.exceeded = true;
  session.Sample();
  session.Run(60000);
  const wd::WatchTick frozen = session.Tick(2 * kClientLimitSettings.evaluatorFrozenMillis);
  UR_EXPECT_TRUE(frozen.froze);
  UR_EXPECT_TRUE(frozen.clientLimitHeld);
  UR_EXPECT_EQ(-1, session.RunUntilDead(kClientLimitLong));

  wd::WatchInputs fresh = session.in;
  fresh.clientLimit = wd::ClientLimitReading{};
  session.watch.Start(fresh);
  session.in = fresh;
  session.readings.clientLimitRead = false;
  const wd::WatchTick first = session.Tick();
  UR_EXPECT_FALSE(first.clientLimitHeld);
}

// ---- the rule it never takes ----------------------------------------------------

// A wedged SDK answers nothing, its hold included, and no free slot brings it
// back: SdkUnresponsive stops the held session as it stops any other.
UR_TEST(FailsafeClientLimit_AHungSdkStillStopsTheSession) {
  HeldSession session;
  session.readings.exceeded = true;
  session.Sample();
  session.Run(10000);
  session.readings.answering = false;
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  const int64_t at = session.RunUntilDead(kClientLimitLong, &reason);
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::SdkUnresponsive);
  UR_EXPECT_TRUE(at >= kClientLimitSettings.sdkUnresponsiveMillis -
                           kClientLimitSettings.sdkSampleIntervalMillis &&
                 at <= kClientLimitSettings.sdkUnresponsiveMillis);
  UR_EXPECT_TRUE(session.armedEver);
}

// ---- the verdict and the hold, alone ------------------------------------------

UR_TEST(FailsafeClientLimit_TheVerdictHoldsTheNoExitRules) {
  wd::DeadTunnelSignals s;
  s.tunnelUp = true;
  s.routesInstalled = true;
  s.upSinceMillis = 1000;
  const int64_t now = 1000 + kClientLimitSettings.deadSlowMillis;
  s.lastSampleMillis = now;
  s.clientLimitHeld = true;
  wd::DeadTunnelVerdict v = wd::Evaluate(s, now, kClientLimitSettings);
  UR_EXPECT_TRUE(v.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(v.armed);
  s.outboundSinceInbound = kClientLimitSettings.deadFastOutboundPackets;
  v = wd::Evaluate(s, now, kClientLimitSettings);
  UR_EXPECT_TRUE(v.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(v.armed);
  s.lastSampleMillis = now - kClientLimitSettings.sdkUnresponsiveMillis;
  UR_EXPECT_TRUE(wd::Evaluate(s, now, kClientLimitSettings).reason ==
                 wd::DeadTunnelReason::SdkUnresponsive);
  s.clientLimitHeld = false;
  s.lastSampleMillis = now;
  UR_EXPECT_TRUE(wd::Evaluate(s, now, kClientLimitSettings).reason ==
                 wd::DeadTunnelReason::NoInbound);
}

UR_TEST(FailsafeClientLimit_TheHoldsTable) {
  const auto reading = [](bool exceeded) {
    wd::ClientLimitReading r;
    r.sampled = true;
    r.exceeded = exceeded;
    return r;
  };
  wd::ClientLimitHold hold;
  UR_EXPECT_TRUE(hold.Observe(wd::ClientLimitReading{}) == wd::ClientLimitStep::None);
  UR_EXPECT_TRUE(hold.Observe(reading(false)) == wd::ClientLimitStep::None);
  UR_EXPECT_FALSE(hold.held());
  UR_EXPECT_TRUE(hold.Observe(reading(true)) == wd::ClientLimitStep::Entered);
  UR_EXPECT_TRUE(hold.Observe(reading(true)) == wd::ClientLimitStep::None);
  UR_EXPECT_TRUE(hold.held());
  UR_EXPECT_TRUE(hold.Observe(wd::ClientLimitReading{}) == wd::ClientLimitStep::None);
  UR_EXPECT_TRUE(hold.held());
  UR_EXPECT_TRUE(hold.Observe(reading(false)) == wd::ClientLimitStep::Released);
  UR_EXPECT_FALSE(hold.held());
  UR_EXPECT_TRUE(hold.Observe(reading(true)) == wd::ClientLimitStep::Entered);
  UR_EXPECT_TRUE(hold.held());
}
