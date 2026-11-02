// An account out of balance is never stopped by the dead-tunnel failsafe (owner
// decisions 2026-10-02 and 2026-11-02; TunnelWatchdog.hpp): its traffic stays
// captured, the app's held alert and notification tell the user, and Disconnect
// is the way out. Every session here replays what the daemon's reaper reads, a
// simulated second at a time: the sampler's contract status, window status and
// exits, and the tun's counters.
//
// The readings are the ones the SDK gives such a session (connect's
// TestOutOfBalanceWindowAdmitsNoExit and the sdk's contract status tests): a
// window that never admits a provider, or one that loses its exits once its
// contracts run out, with the sampler answering all along.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>

#include "ControlProtocol.hpp"
#include "FailsafeNotice.hpp"
#include "Health.hpp"
#include "InsufficientBalanceNotice.hpp"
#include "TunnelWatchdog.hpp"

namespace wd = urnw::watchdog;
namespace ctl = urnw::ctl;
namespace balance_notice = urnw::balance_notice;
namespace failsafe_notice = urnw::failsafe_notice;
namespace health = urnw::health;

namespace {

const wd::DeadTunnelSettings kSettings{};

// What the sampler reads each interval, and what the tun's counters do between.
struct Readings {
  // WindowStatus and the exits.
  int64_t generation = 1;
  int64_t added = 0;
  int64_t proven = 0;
  bool minSatisfied = false;
  // ContractStatus.InsufficientBalance and NoPermission; unread for a status that
  // could not be read.
  bool insufficientBalance = false;
  bool noPermission = false;
  bool balanceRead = true;
  // The sampler answers at all (a wedged SDK does not).
  bool answering = true;
  // Packets a tick into the tunnel, and back out of it.
  uint64_t outPerTick = 50;
  uint64_t inPerTick = 0;
};

// A session as the reaper sees it, the sampler's readings refreshed on the
// interval it takes them.
struct Session {
  wd::DeadTunnelWatch watch;
  wd::WatchInputs in;
  Readings readings;
  int64_t ticks = 0;
  bool armedEver = false;

  explicit Session(const wd::DeadTunnelSettings& settings = kSettings) : watch(settings) {
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
    if (readings.balanceRead) {
      in.balance.sampled = true;
      in.balance.insufficientBalance = readings.insufficientBalance;
      in.balance.noPermission = readings.noPermission;
      in.balance.connectionGeneration = readings.generation;
      in.balance.providerAddedCount = readings.added;
    }
  }

  wd::WatchTick Tick(int64_t stepMillis = kSettings.evaluateIntervalMillis) {
    in.nowMillis += stepMillis;
    in.outboundPackets += readings.outPerTick;
    in.inboundPackets += readings.inPerTick;
    // As the reaper does: the verdict reads the sample before this tick.
    const wd::WatchTick tick = watch.Tick(in);
    if (++ticks % (kSettings.sdkSampleIntervalMillis / kSettings.evaluateIntervalMillis) == 0) {
      Sample();
    }
    if (tick.verdict.armed) armedEver = true;
    return tick;
  }

  // Ticks until a verdict or `millis` have passed: the elapsed time at the
  // verdict and its reason, or -1 for none.
  int64_t RunUntilDead(int64_t millis, wd::DeadTunnelReason* reason = nullptr) {
    for (int64_t elapsed = kSettings.evaluateIntervalMillis; elapsed <= millis;
         elapsed += kSettings.evaluateIntervalMillis) {
      const wd::WatchTick tick = Tick();
      if (tick.verdict.reason != wd::DeadTunnelReason::None) {
        if (reason != nullptr) *reason = tick.verdict.reason;
        return elapsed;
      }
    }
    return -1;
  }

  void Run(int64_t millis) {
    for (int64_t elapsed = 0; elapsed < millis; elapsed += kSettings.evaluateIntervalMillis) {
      Tick();
    }
  }
};

// A session whose window formed and carried, up for a minute.
void FormAndCarry(Session& session) {
  session.readings.added = 2;
  session.readings.proven = 2;
  session.readings.minSatisfied = true;
  session.readings.inPerTick = 40;
  session.Sample();
  session.Run(60000);
}

// Its contracts run out: nothing comes back, and the window has lost its exits.
void LoseExits(Session& session) {
  session.readings.added = 0;
  session.readings.proven = 0;
  session.readings.inPerTick = 0;
}

const int64_t kLong = 10 * kSettings.deadSlowMillis;

}  // namespace

// ---- the rule ---------------------------------------------------------------

// A session that starts out of balance: the platform refuses every contract, so
// the window admits no provider, the sampler answers, and this machine keeps
// sending. Past NoExit's window, by ten times over, it is not stopped, and no
// countdown ever said it would be.
UR_TEST(FailsafeOutOfBalance_NoExitPastItsWindowIsNotStopped) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(session.armedEver);
}

// A session that runs out while up: its window had formed, so NoInbound is the
// rule in reach, twenty seconds after the exits are gone. Neither it nor NoExit
// stops the session.
UR_TEST(FailsafeOutOfBalance_NoInboundPastItsWindowIsNotStopped) {
  Session session;
  FormAndCarry(session);
  session.readings.insufficientBalance = true;
  LoseExits(session);
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(session.armedEver);
}

// The refusal comes while the exits still read proven (a qualification outlives
// the contracts: it ages out over thirty minutes), and the exits go later. The
// hold is already up when they do.
UR_TEST(FailsafeOutOfBalance_ExitsLostLongAfterTheRefusalAreNotStopped) {
  Session session;
  FormAndCarry(session);
  session.readings.insufficientBalance = true;
  session.readings.inPerTick = 0;
  UR_EXPECT_EQ(-1, session.RunUntilDead(30 * 60000));
  LoseExits(session);
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
  UR_EXPECT_FALSE(session.armedEver);
}

// The same two sessions in balance are stopped exactly as documented: the rule
// took nothing away from a dead tunnel the balance has no part in.
UR_TEST(FailsafeOutOfBalance_TheSameSessionInBalanceIsStopped) {
  Session never;
  never.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kSettings.deadSlowMillis, never.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
  UR_EXPECT_TRUE(never.armedEver);

  Session formed;
  FormAndCarry(formed);
  LoseExits(formed);
  reason = wd::DeadTunnelReason::None;
  const int64_t atFast = formed.RunUntilDead(kLong, &reason);
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoInbound);
  // the fast window, and the sample that took the exit away
  UR_EXPECT_TRUE(atFast >= kSettings.deadFastMillis &&
                 atFast <= kSettings.deadFastMillis + kSettings.sdkSampleIntervalMillis);
}

// ---- the balance returning --------------------------------------------------

// A contract goes through on the same destination, and the status clears. The
// session is owed the normal window to prove an exit, and gets it in full: the
// rule the hold kept back does not fire on the first tick the hold lifts.
UR_TEST(FailsafeOutOfBalance_TheBalanceReturningStartsEveryWindowAgain) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(5 * 60000);

  session.readings.insufficientBalance = false;
  session.Sample();
  const wd::WatchTick released = session.Tick();
  UR_EXPECT_TRUE(released.balanceStep == wd::BalanceStep::Released);
  UR_EXPECT_FALSE(released.outOfBalance);
  UR_EXPECT_TRUE(released.verdict.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(released.verdict.armed);

  // Still dead a full window later: an account in balance again is stopped.
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kSettings.deadSlowMillis, session.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
}

// And the usual ending: the exits come back with the contracts, so nothing fires.
UR_TEST(FailsafeOutOfBalance_TheBalanceReturningWithExitsStopsNothing) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(5 * 60000);

  session.readings.insufficientBalance = false;
  session.readings.added = 2;
  session.readings.proven = 2;
  session.readings.minSatisfied = true;
  session.readings.inPerTick = 40;
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
}

// Near zero a small grant and a refusal take turns. The refusal holds again, and
// a grant that does not last costs no stop.
UR_TEST(FailsafeOutOfBalance_ARefusalAfterTheReturnHoldsAgain) {
  Session session;
  for (int round = 0; round < 8; ++round) {
    session.readings.insufficientBalance = true;
    session.Sample();
    UR_EXPECT_EQ(-1, session.RunUntilDead(2 * kSettings.deadSlowMillis));
    session.readings.insufficientBalance = false;
    session.Sample();
    UR_EXPECT_EQ(-1, session.RunUntilDead(kSettings.deadSlowMillis / 2));
  }
}

// ---- a stale or missing signal ----------------------------------------------

// A location change or a reconnect resets the contract status, and a held
// connection's window then has nothing to report: no candidate in that location,
// or the platform out of reach. That reset says nothing about the balance.
UR_TEST(FailsafeOutOfBalance_AResetDoesNotEndTheHold) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(60000);

  session.readings.insufficientBalance = false;
  session.readings.generation = 2;
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
  UR_EXPECT_FALSE(session.armedEver);

  // The refusal comes back on the new destination: still the same hold.
  session.readings.insufficientBalance = true;
  session.Sample();
  const wd::WatchTick again = session.Tick();
  UR_EXPECT_TRUE(again.balanceStep == wd::BalanceStep::None);
  UR_EXPECT_TRUE(again.outOfBalance);
}

// The new destination's window admitting a provider is what ends it after a
// reset, as it is after the rebuild the balance recovery makes.
UR_TEST(FailsafeOutOfBalance_AnAdmittedProviderAfterAResetEndsTheHold) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(60000);

  session.readings.insufficientBalance = false;
  session.readings.generation = 2;
  session.Sample();
  session.Run(60000);

  session.readings.added = 1;
  session.Sample();
  const wd::WatchTick released = session.Tick();
  UR_EXPECT_TRUE(released.balanceStep == wd::BalanceStep::Released);
  // That one admitted provider then goes silent and never proves itself: an
  // account in balance, with a dead tunnel, stopped as documented.
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kSettings.deadSlowMillis, session.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
}

// A status that can no longer be read changes nothing: the last word stands.
UR_TEST(FailsafeOutOfBalance_AnUnreadableStatusKeepsTheHold) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(60000);

  session.readings.balanceRead = false;
  session.readings.insufficientBalance = false;
  session.in.balance = wd::BalanceReading{};
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
  UR_EXPECT_FALSE(session.armedEver);
}

// The status window keeps the kind of refusal that fills it, so a run of
// no-permission results takes the insufficient balance out of the status with no
// contract granted. Only a grant, which clears both, is news about the balance.
UR_TEST(FailsafeOutOfBalance_ANoPermissionResultDoesNotEndTheHold) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(60000);

  session.readings.insufficientBalance = false;
  session.readings.noPermission = true;
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
  UR_EXPECT_FALSE(session.armedEver);

  session.readings.noPermission = false;
  session.Sample();
  UR_EXPECT_TRUE(session.Tick().balanceStep == wd::BalanceStep::Released);
}

// The window's readings alone never end it: providers still counted Added while
// the status says refused are the ones admitted before the balance ran out.
UR_TEST(FailsafeOutOfBalance_ProvidersFromBeforeTheRefusalDoNotEndTheHold) {
  Session session;
  FormAndCarry(session);
  session.readings.insufficientBalance = true;
  session.readings.proven = 0;
  session.readings.inPerTick = 0;
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
  UR_EXPECT_FALSE(session.armedEver);
}

// A suspend and a return to a network restart the windows. Neither is news about
// the balance.
UR_TEST(FailsafeOutOfBalance_TheHoldOutlivesARebase) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(60000);

  const wd::WatchTick frozen = session.Tick(10 * kSettings.evaluatorFrozenMillis);
  UR_EXPECT_TRUE(frozen.froze);
  UR_EXPECT_TRUE(frozen.outOfBalance);
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));

  session.in.offline = true;
  session.Run(60000);
  session.in.offline = false;
  const wd::WatchTick back = session.Tick();
  UR_EXPECT_TRUE(back.offlineStep == wd::OfflineStep::BackRebased);
  UR_EXPECT_TRUE(back.outOfBalance);
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
  UR_EXPECT_FALSE(session.armedEver);
}

// ---- the balance unknown ----------------------------------------------------

// Nothing has ever said this session is out of balance: an unreadable status for
// its whole life, or a status that never rose. That is every dead tunnel the
// failsafe exists for (no network to the platform, a hostile network), and they
// are stopped on time.
UR_TEST(FailsafeOutOfBalance_ABalanceNeverReportedIsJudgedAsDocumented) {
  Session unread;
  unread.readings.balanceRead = false;
  unread.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kSettings.deadSlowMillis, unread.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);

  Session never;
  never.Sample();
  reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kSettings.deadSlowMillis, never.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
}

// A refusal comes back within seconds of the first contract asked for, long
// before the shortest window is up, so a session out of balance from its start
// is held before any rule can reach it.
UR_TEST(FailsafeOutOfBalance_ARefusalAheadOfTheWindowIsInTime) {
  Session session;
  session.Sample();
  // the last tick NoExit would still not fire on
  UR_EXPECT_EQ(-1, session.RunUntilDead(kSettings.deadSlowMillis -
                                        2 * kSettings.sdkSampleIntervalMillis));
  session.readings.insufficientBalance = true;
  session.Sample();
  UR_EXPECT_EQ(-1, session.RunUntilDead(kLong));
}

// The hold belongs to the session that was told. A new session starts knowing
// nothing; the start-connect gate is what stops one being started out of balance.
UR_TEST(FailsafeOutOfBalance_ANewSessionStartsTheHoldAgain) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  session.Run(60000);

  session.in.nowMillis += kSettings.evaluatorFrozenMillis;
  session.in.balance = wd::BalanceReading{};
  session.in.windowSampled = false;
  session.in.lastSampleMillis = -1;
  session.watch.Start(session.in);
  session.readings.insufficientBalance = false;
  session.readings.generation = 1;
  session.Sample();
  wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
  UR_EXPECT_EQ(kSettings.deadSlowMillis, session.RunUntilDead(kLong, &reason));
  UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::NoExit);
}

// ---- a hung process ---------------------------------------------------------

// SdkUnresponsive stays, out of balance as in balance. It is not about traffic:
// a wedged SDK cannot be brought back by a balance, answers nothing (the balance
// included), and would hold the user's own Disconnect behind the same lock. A
// countdown runs for it, and it alone.
UR_TEST(FailsafeOutOfBalance_AHungSdkStillStopsTheSession) {
  for (const bool outOfBalance : {true, false}) {
    Session session;
    session.readings.insufficientBalance = outOfBalance;
    session.readings.proven = 2;
    session.readings.added = 2;
    session.readings.minSatisfied = true;
    session.Sample();
    session.Run(10000);

    session.readings.answering = false;
    wd::DeadTunnelReason reason = wd::DeadTunnelReason::None;
    const int64_t at = session.RunUntilDead(kLong, &reason);
    UR_EXPECT_TRUE(reason == wd::DeadTunnelReason::SdkUnresponsive);
    // the window, from the last answer that landed
    UR_EXPECT_TRUE(at >= kSettings.sdkUnresponsiveMillis - kSettings.sdkSampleIntervalMillis &&
                   at <= kSettings.sdkUnresponsiveMillis);
    UR_EXPECT_TRUE(session.armedEver);
  }
}

// ---- the verdict and the hold, on their own -----------------------------------

UR_TEST(FailsafeOutOfBalance_TheVerdictHoldsTheNoExitRules) {
  wd::DeadTunnelSignals s;
  s.tunnelUp = true;
  s.routesInstalled = true;
  s.upSinceMillis = 1000;
  s.provenCount = 0;
  s.lastSampleMillis = 1000 + kSettings.deadSlowMillis;
  s.outboundSinceInbound = kSettings.deadFastOutboundPackets;
  const int64_t now = 1000 + kSettings.deadSlowMillis;

  UR_EXPECT_TRUE(wd::Evaluate(s, now, kSettings).reason == wd::DeadTunnelReason::NoInbound);
  s.outboundSinceInbound = 0;
  UR_EXPECT_TRUE(wd::Evaluate(s, now, kSettings).reason == wd::DeadTunnelReason::NoExit);

  s.outOfBalance = true;
  s.outboundSinceInbound = kSettings.deadFastOutboundPackets;
  wd::DeadTunnelVerdict v = wd::Evaluate(s, now, kSettings);
  UR_EXPECT_TRUE(v.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(v.armed);
  s.outboundSinceInbound = 0;
  v = wd::Evaluate(s, now, kSettings);
  UR_EXPECT_TRUE(v.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_FALSE(v.armed);

  // ...and only those two
  s.lastSampleMillis = now - kSettings.sdkUnresponsiveMillis;
  UR_EXPECT_TRUE(wd::Evaluate(s, now, kSettings).reason == wd::DeadTunnelReason::SdkUnresponsive);
  s.lastSampleMillis = now - kSettings.sdkUnresponsiveArmMillis;
  v = wd::Evaluate(s, now, kSettings);
  UR_EXPECT_TRUE(v.reason == wd::DeadTunnelReason::None);
  UR_EXPECT_TRUE(v.armed);
  UR_EXPECT_EQ(kSettings.sdkUnresponsiveMillis - kSettings.sdkUnresponsiveArmMillis,
               v.millisToFailsafe);
}

UR_TEST(FailsafeOutOfBalance_TheHoldsTable) {
  const auto reading = [](bool insufficient, int64_t generation, int64_t added) {
    wd::BalanceReading r;
    r.sampled = true;
    r.insufficientBalance = insufficient;
    r.connectionGeneration = generation;
    r.providerAddedCount = added;
    return r;
  };
  wd::OutOfBalanceHold hold;
  // not told: nothing about a status that was not read, or that never rose
  UR_EXPECT_TRUE(hold.Observe(wd::BalanceReading{}) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.Observe(reading(false, 1, 0)) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.Observe(reading(false, 1, 3)) == wd::BalanceStep::None);
  UR_EXPECT_FALSE(hold.held());
  // told, once
  UR_EXPECT_TRUE(hold.Observe(reading(true, 1, 3)) == wd::BalanceStep::Entered);
  UR_EXPECT_TRUE(hold.Observe(reading(true, 1, 3)) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.held());
  // a grant on the destination that was refused
  UR_EXPECT_TRUE(hold.Observe(reading(false, 1, 0)) == wd::BalanceStep::Released);
  UR_EXPECT_FALSE(hold.held());
  // a reset is not a grant, and an unreadable status is neither
  UR_EXPECT_TRUE(hold.Observe(reading(true, 1, 0)) == wd::BalanceStep::Entered);
  UR_EXPECT_TRUE(hold.Observe(reading(false, 2, 0)) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.Observe(wd::BalanceReading{}) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.held());
  // nor is a refusal of another kind, even with a provider admitted
  wd::BalanceReading noPermission = reading(false, 2, 1);
  noPermission.noPermission = true;
  UR_EXPECT_TRUE(hold.Observe(noPermission) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.held());
  // a provider admitted past the reset is
  UR_EXPECT_TRUE(hold.Observe(reading(false, 2, 1)) == wd::BalanceStep::Released);
  UR_EXPECT_FALSE(hold.held());
  // a refusal on the newer destination moves the bar with it
  UR_EXPECT_TRUE(hold.Observe(reading(true, 2, 1)) == wd::BalanceStep::Entered);
  UR_EXPECT_TRUE(hold.Observe(reading(false, 3, 0)) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.Observe(reading(true, 3, 0)) == wd::BalanceStep::None);
  UR_EXPECT_TRUE(hold.Observe(reading(false, 3, 0)) == wd::BalanceStep::Released);
}

// ---- the alert and the notification -------------------------------------------

namespace {

struct CountingSink {
  int posts = 0;
  int withdraws = 0;
  void Post() { ++posts; }
  void Withdraw() { ++withdraws; }
};

// The GUI's reading of a session the daemon runs: the status it reads from the
// same device, until the session ends and the device with it.
struct Gui {
  balance_notice::Tracker tracker;
  CountingSink sink;
  bool sessionEnded = false;

  balance_notice::Signals Signals(bool insufficientBalance) const {
    balance_notice::Signals s;
    s.insufficientBalance = insufficientBalance && !sessionEnded;
    s.connectRequested = !sessionEnded;
    return s;
  }

  void Observe(bool insufficientBalance) {
    const balance_notice::Signals s = Signals(insufficientBalance);
    tracker.Observe(s, sink);
  }

  bool HeldAlert(bool insufficientBalance) const {
    return balance_notice::HeldAlert(Signals(insufficientBalance));
  }
};

}  // namespace

// The whole of what the user is promised, for as long as the session is out of
// balance: the notification once and still up, the held alert still showing,
// Disconnect as the action, and no warning that the tunnel is about to be turned
// off. Stopping the session would take all of it away and leave the sentence
// "Your traffic is held in the tunnel" untrue.
UR_TEST(FailsafeOutOfBalance_TheAlertAndNotificationStayForTheWholeHold) {
  Session session;
  session.readings.insufficientBalance = true;
  session.Sample();
  Gui gui;
  bool warned = false;

  for (int64_t elapsed = 0; elapsed < kLong; elapsed += kSettings.evaluateIntervalMillis) {
    const wd::WatchTick tick = session.Tick();
    if (tick.verdict.reason != wd::DeadTunnelReason::None) gui.sessionEnded = true;
    if (failsafe_notice::ShowsArmedWarning(tick.verdict.armed, ctl::TunnelState::Up)) warned = true;
    gui.Observe(true);
  }

  UR_EXPECT_EQ(1, gui.sink.posts);
  UR_EXPECT_EQ(0, gui.sink.withdraws);
  UR_EXPECT_TRUE(gui.tracker.Shown());
  UR_EXPECT_TRUE(gui.HeldAlert(true));
  UR_EXPECT_FALSE(gui.sessionEnded);
  UR_EXPECT_FALSE(warned);

  health::Signals signals;
  signals.sdk = health::SdkStatus::Connecting;
  signals.destinationSelected = !gui.sessionEnded;
  signals.tunnelBound = !gui.sessionEnded;
  signals.insufficientBalance = !gui.sessionEnded;
  const health::Reading reading = health::Render(signals);
  UR_EXPECT_TRUE(reading.state == health::State::Blocked);
  UR_EXPECT_TRUE(reading.action == health::Action::Disconnect);
}

// A session that runs out while up gets them the moment it is refused, not when
// the exits go: the status is what they read, as the hold does.
UR_TEST(FailsafeOutOfBalance_TheAlertAndTheHoldReadOneSignal) {
  Session session;
  FormAndCarry(session);
  Gui gui;
  gui.Observe(session.in.balance.insufficientBalance);
  UR_EXPECT_EQ(0, gui.sink.posts);

  session.readings.insufficientBalance = true;
  session.Sample();
  const wd::WatchTick entered = session.Tick();
  UR_EXPECT_TRUE(entered.balanceStep == wd::BalanceStep::Entered);
  gui.Observe(session.in.balance.insufficientBalance);
  UR_EXPECT_EQ(1, gui.sink.posts);
  UR_EXPECT_TRUE(gui.HeldAlert(session.in.balance.insufficientBalance));

  // the balance comes back: the notice is withdrawn on the reading that lifts the hold
  session.readings.insufficientBalance = false;
  session.Sample();
  const wd::WatchTick released = session.Tick();
  UR_EXPECT_TRUE(released.balanceStep == wd::BalanceStep::Released);
  gui.Observe(session.in.balance.insufficientBalance);
  UR_EXPECT_EQ(1, gui.sink.withdraws);
  UR_EXPECT_FALSE(gui.tracker.Shown());
  UR_EXPECT_FALSE(gui.HeldAlert(session.in.balance.insufficientBalance));
}
