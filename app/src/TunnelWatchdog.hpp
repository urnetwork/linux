// The dead-tunnel failsafe's decision (docs/linux_agent_help.md 6.4): when a
// live tunnel that carries nothing is ended so the machine is not left pointed
// at it.
//
// With the kill switch off the capture routes send everything into the tun
// whether or not the session behind it can carry it, so a session whose
// transports died used to leave the machine without internet until somebody
// acted. The daemon now ends such a session the way its other protective
// teardowns do: with the kill switch off the machine gets its internet back,
// unprotected and told so; with it on the machine stays blocked behind the
// armed floor. It never reconnects, so it cannot thrash.
//
// Three rules, any one of which ends the session, behind one veto (every time
// here is a DeadTunnelSettings default):
//   the carrying veto  a packet back out of the tunnel in the last 20 s means
//                      it carries, and nothing fires (a measurement outranks
//                      every claim below it)
//   SdkUnresponsive    no completed sample of the SDK for 30 s
//   NoInbound          8 or more packets into the tunnel since anything came
//                      back, nothing back and no proven exit, for 20 s
//   NoExit             no proven exit for 90 s, with the SDK answering
// A false teardown of a working tunnel is the failure this must never have, so
// an idle machine never trips NoInbound (it sends nothing), a suspend or a
// stalled daemon rebases every window rather than judging time nobody watched,
// and a new destination window gets its own clock while it forms.
//
// AND A MACHINE WITH NO NETWORK IS NOT A DEAD TUNNEL. While the reaper's path
// tracker (NetworkQuality.hpp) finds no usable physical path in any family,
// nothing could prove an exit or carry a packet back, so the two no-exit rules
// (NoInbound, NoExit) hold, and the time spent offline is excluded from the
// windows they measure. Back on a network after a real stretch
// (DeadTunnelSettings::offlineRebaseMillis), every window starts again, as
// after a suspend: the session gets the normal window to re-prove an exit on
// the new network rather than being stopped the moment it returns. A shorter
// blip rebases nothing; its seconds are only left out, so a link that keeps
// flapping cannot keep a dead tunnel alive.
// SdkUnresponsive is not about the network, and keeps watching throughout.
//
// AND AN ACCOUNT OUT OF BALANCE IS NOT A DEAD TUNNEL EITHER (owner decisions
// 2026-10-02 and 2026-11-02). The platform refuses its contracts, so it loses its
// exits and carries nothing, and every reading the no-exit rules take says dead.
// That is a billing state, which the app already tells the user about
// (InsufficientBalanceNotice.hpp), and stopping the session for it would let
// their traffic out in the clear. So while the session is out of balance
// (OutOfBalanceHold, below) NoInbound and NoExit hold, no countdown for them is
// shown, the traffic stays captured, and the user's own Disconnect is the way
// out. When the balance is back every window starts again, as after a suspend:
// the session is owed the normal window to prove an exit.
// SdkUnresponsive keeps watching here too. It is about a hung process, which no
// balance can bring back: a wedged device answers nothing, the balance included,
// and a Disconnect would only wait on the same lock.
//
// Pure, so every rule is a table test: TunnelHost reads the tun's packet
// counters and the exit sampler's last readings once a second on its reaper,
// hands them to DeadTunnelWatch with one clock, and acts on the verdict. The
// clock is CLOCK_BOOTTIME, which runs through a suspend, so a resume shows up
// as the tick gap it is. Ported from urnetwork/windows Service/TunnelWatchdog.h.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "ControlProtocol.hpp"

namespace urnw::watchdog {

// ---- the settings -------------------------------------------------------------

// Every timing the failsafe reads, each with its default here. The daemon builds
// its watch and its sampler from one instance (HostSettings.hpp), so a caller
// or a test can change any of them, and nothing that judges a session reads a
// timing from anywhere else. Times are milliseconds on the watch's clock.
struct DeadTunnelSettings {
  // ---- the cadence ----------------------------------------------------------
  // Between two ticks of the watch, the cadence its windows are counted in. The
  // daemon ticks it on the reaper, so it must equal
  // HostSettings::reaper_interval_seconds (TunnelHost logs a mismatch). 1000:
  // fine against the shortest window, for one sysfs read and one atomic
  // snapshot a tick. It paces nothing itself; it is the unit the freeze bar's
  // default is counted in. First, so DeadTunnelSettings{500} is the instance
  // for a half-second tick, freezing at 2.5 s; 0 there makes the freeze bar 0,
  // which turns the failsafe off.
  int64_t evaluateIntervalMillis = 1000;
  // Between two samples of the SDK (its window status and exits), taken by the
  // exit sampler's thread, which makes every call on a watched session's device
  // -- the network-change kicks included -- so a wedged device stops the
  // sampler and cannot keep the verdict from being reached. 2000: fifteen
  // samples fit in sdkUnresponsiveMillis, so one slow call is never a wedge. 0
  // (or less) is read as the default (SampleIntervalMillis): a sampler that
  // asked back to back would spin on the device's state lock.
  int64_t sdkSampleIntervalMillis = 2000;
  // A tick this late or later means nothing watched the session in between: a
  // suspend, a stopped process, a main loop held by a long stop. The session
  // is owed a fresh grace period then, not a verdict. Five evaluate ticks (5 s
  // by default): far beyond scheduling jitter, far below the shortest rule
  // window. 0 makes every tick a freeze, so no verdict is ever reached: the
  // failsafe is off.
  int64_t evaluatorFrozenMillis = 5 * evaluateIntervalMillis;

  // ---- the rules ------------------------------------------------------------
  // SdkUnresponsive's window: fifteen missed samples. A call into the SDK that
  // has not returned in 30 s is wedged, not slow, and this is the one rule that
  // needs nothing from the SDK to fire. 0 turns the rule off.
  int64_t sdkUnresponsiveMillis = 30000;
  // How overdue a sample must be before SdkUnresponsive's countdown is worth
  // showing; below 10 s is the normal cadence. 0 counts it down from every
  // sample, so with the default notice the app is always told one runs.
  int64_t sdkUnresponsiveArmMillis = 10000;
  // How close a verdict must be before the app is told a countdown runs
  // (StatusReply::failsafe_armed): 30 s, so a teardown is never a surprise and
  // a warning is never noise. 0 never warns ahead of a stop.
  int64_t failsafeNoticeMillis = 30000;
  // NoInbound's window, and the carrying veto's: a packet back within it means
  // the tunnel carries. 20 s: above the longest honest machine-wide inbound gap
  // (a DNS client's schedule across a server list runs about 13 s; a black-holed
  // TCP connect stalls one flow, not all of them), and below the 30 s after
  // which a person has decided the internet is broken. 0 turns NoInbound off,
  // and the veto with it.
  int64_t deadFastMillis = 20000;
  // Packets into the tunnel it takes to arm NoInbound: a machine that is trying
  // retransmits 8 in well under a second, and an idle one never sends them. An
  // idle blocked machine looks like an idle working one, so it is never judged.
  // 0 asks for no commitment, and an idle machine with nothing proven is judged.
  uint64_t deadFastOutboundPackets = 8;
  // NoExit's window: 90 s, about 2.5 times one honest cold attempt after a roam
  // (DNS, TCP and the handshake, about 35 s), so a roam that recovers on its
  // second attempt is never ended. 0 turns NoExit off.
  int64_t deadSlowMillis = 90000;
  // An offline stretch this long is owed a fresh grace when the network
  // returns, for the reason a freeze is: what was measured before it describes
  // another network. The freeze's bar by default, so a one-tick blip never
  // rebases. 0 rebases on every return, a blip included.
  int64_t offlineRebaseMillis = evaluatorFrozenMillis;
};

// The relations the defaults were chosen by.
static_assert(DeadTunnelSettings{}.evaluatorFrozenMillis ==
              5 * DeadTunnelSettings{}.evaluateIntervalMillis);
static_assert(DeadTunnelSettings{}.sdkUnresponsiveMillis >
              DeadTunnelSettings{}.sdkUnresponsiveArmMillis);
static_assert(DeadTunnelSettings{}.sdkUnresponsiveMillis >=
              10 * DeadTunnelSettings{}.sdkSampleIntervalMillis);
static_assert(DeadTunnelSettings{}.evaluatorFrozenMillis < DeadTunnelSettings{}.deadFastMillis);
static_assert(DeadTunnelSettings{}.offlineRebaseMillis < DeadTunnelSettings{}.deadFastMillis);

// The sampler's interval as `settings` asks for it, a nonpositive one read as
// the default.
inline constexpr int64_t SampleIntervalMillis(const DeadTunnelSettings& settings) {
  return settings.sdkSampleIntervalMillis > 0 ? settings.sdkSampleIntervalMillis
                                              : DeadTunnelSettings{}.sdkSampleIntervalMillis;
}

inline constexpr bool EvaluatorFroze(int64_t tickGapMillis, const DeadTunnelSettings& settings) {
  return tickGapMillis >= settings.evaluatorFrozenMillis;
}

// What one tick's network state does to the no-exit rules (see the file
// comment): `wasOffline` is the last tick's reading, `offline` this one's, and
// `offlineStretchMillis` how long the machine has been offline up to now.
enum class OfflineStep {
  // Online, as at the last tick: every clock runs.
  Online,
  // No usable path: the no-exit rules hold and their clocks do not run.
  Offline,
  // Online again after less than the rebase stretch: the stretch stays
  // excluded and nothing rebases.
  BackFromBlip,
  // Online again after the rebase stretch or more: every window starts again.
  BackRebased,
};

inline constexpr OfflineStep StepOffline(bool wasOffline, bool offline,
                                         int64_t offlineStretchMillis,
                                         const DeadTunnelSettings& settings) {
  if (offline) return OfflineStep::Offline;
  if (!wasOffline) return OfflineStep::Online;
  return offlineStretchMillis >= settings.offlineRebaseMillis ? OfflineStep::BackRebased
                                                              : OfflineStep::BackFromBlip;
}

// A stamp from before `sessionStartMillis` is no evidence about this session;
// -1 is how the verdict reads "never in this session".
inline constexpr int64_t StampInSession(int64_t stampMillis, int64_t sessionStartMillis) {
  return stampMillis >= sessionStartMillis ? stampMillis : -1;
}

// ---- the balance hold --------------------------------------------------------

// What one sample of the SDK says about the balance: ContractStatus.
// InsufficientBalance, the same reading the app's held alert and notification
// take from this device, and the window status read after it.
struct BalanceReading {
  // Both were read. False says nothing about the balance, and the hold stays as
  // it is.
  bool sampled = false;
  // ContractStatus::InsufficientBalance: the platform's last word on this
  // destination's contracts was that the balance falls short.
  bool insufficientBalance = false;
  // ContractStatus::NoPermission, which can push a refusal out of the status
  // with no grant in between (the SDK's status window keeps the kind that fills
  // it). It says nothing about the balance.
  bool noPermission = false;
  // WindowStatus::ConnectionGeneration. The status has to be read first, so a
  // destination change between the two reads can never pair a status it reset
  // with the generation before it.
  int64_t connectionGeneration = 0;
  // WindowStatus::ProviderStateAdded: the providers that destination's window
  // admitted. Admission takes an answered ping, which took a granted contract.
  int64_t providerAddedCount = 0;
};

// What one reading did to the hold.
enum class BalanceStep {
  None,
  // First seen out of balance: the no-exit rules hold from here.
  Entered,
  // Contracts are going through again: every window starts again.
  Released,
};

// Whether the session is out of balance, kept across the readings that cannot
// tell. Raised by the first reading of insufficient balance and kept until the
// platform has said contracts go through, because the two ways to get it wrong
// do not cost the same: stopping an account that is out of balance lets its
// traffic out in the clear, and holding one that is not leaves it captured and
// told so, with Disconnect working.
//
// The SDK's status only says that the last result on this destination was not a
// refusal, and it reads the same for a grant and for nothing yet: a refusal
// raises it, a granted contract clears it, a result about something else leaves
// it, and a new destination resets it with the connection it replaces (a location
// change, a reconnect). So:
//   - not raised, same generation as the refusal: a contract was granted.
//   - not raised, a newer generation: the reset says nothing about the balance,
//     and the hold stays until that destination's window admits a provider.
//   - no reading, or a no-permission result in its place: says nothing, either way.
//
// The failure directions, each pinned by a test (FailsafeOutOfBalanceTest.cpp):
//   - A stale or missing signal never stops an account out of balance: an
//     unreadable status, a reset, and a window that never forms all keep the hold.
//   - A dead tunnel on an account in balance is stopped as documented. A session
//     never told it is out of balance is judged by the rules alone, and this
//     includes a balance nobody has reported yet: a refusal comes back within
//     seconds of the first contract asked for, far inside the shortest window,
//     and with no answer at all there is nothing to tell a dead tunnel from a
//     billing state by, which is the failure the failsafe exists for. A start
//     out of balance is refused before it gets here (DecideStartConnect).
//   - Once the account is back in balance but no new provider has been admitted
//     since a reset, the session stays held: nothing has said so yet. A funded
//     account whose rebuilt window never forms is the one left captured until
//     its user acts: the app reads "Couldn't connect" and its Retry starts a new
//     session, judged afresh. That cost is the one this chooses over a leak.
class OutOfBalanceHold {
 public:
  BalanceStep Observe(const BalanceReading& reading) {
    if (!reading.sampled) return BalanceStep::None;
    if (reading.insufficientBalance) {
      refusedGeneration_ = reading.connectionGeneration;
      if (held_) return BalanceStep::None;
      held_ = true;
      return BalanceStep::Entered;
    }
    if (!held_ || reading.noPermission) return BalanceStep::None;
    const bool granted = reading.connectionGeneration <= refusedGeneration_ ||
                         reading.providerAddedCount > 0;
    if (!granted) return BalanceStep::None;
    held_ = false;
    return BalanceStep::Released;
  }

  bool held() const { return held_; }

 private:
  bool held_ = false;
  int64_t refusedGeneration_ = 0;
};

// ---- the verdict ------------------------------------------------------------

enum class DeadTunnelReason {
  None,
  // No proven exit for DeadTunnelSettings::deadSlowMillis: nothing to carry
  // traffic to.
  NoExit,
  // Committed traffic in and nothing back for DeadTunnelSettings::deadFastMillis:
  // the hole, measured rather than inferred.
  NoInbound,
  // The SDK has not answered for DeadTunnelSettings::sdkUnresponsiveMillis.
  // The other two read the SDK, so this outranks them.
  SdkUnresponsive,
};

// The stop_reason a teardown for `reason` publishes ("" for None).
inline const char* StopReasonOf(DeadTunnelReason reason) {
  switch (reason) {
    case DeadTunnelReason::None:
      return "";
    case DeadTunnelReason::NoExit:
      return ctl::kStopReasonFailsafeNoExit;
    case DeadTunnelReason::NoInbound:
      return ctl::kStopReasonFailsafeNoInbound;
    case DeadTunnelReason::SdkUnresponsive:
      return ctl::kStopReasonFailsafeSdkUnresponsive;
  }
  return "";
}

// Everything the verdict reads. Timestamps are milliseconds on the watchdog's
// clock, -1 for "never in this session".
struct DeadTunnelSignals {
  // The tunnel is up and its capture routes are in: the machine is pointed at
  // it, which is the only thing the verdict judges.
  bool tunnelUp = false;
  bool routesInstalled = false;
  // When the session being judged began; 0 for no session.
  int64_t upSinceMillis = 0;
  // When NoInbound's traffic clock began: a new destination window starts it
  // only once it first forms. 0 means upSinceMillis.
  int64_t trafficStartMillis = 0;
  // False only while a new destination generation first forms.
  bool providerWindowReady = true;

  // From the exit sampler: the proven exits in its last completed sample, when
  // a sample last had one, and when a sample last completed at all (the wedge
  // detector).
  int64_t provenCount = 0;
  int64_t lastProvenMillis = -1;
  int64_t lastSampleMillis = -1;

  // From the tun's counters: when a packet last came back out of the tunnel,
  // and how many went in since (or since the traffic clock began).
  int64_t lastInboundMillis = -1;
  uint64_t outboundSinceInbound = 0;

  // No usable physical path in any family: the no-exit rules hold. And the
  // time spent offline inside the two windows those rules measure (since the
  // last proven exit, since the last packet back), which they leave out.
  bool offline = false;
  int64_t provenOfflineMillis = 0;
  int64_t inboundOfflineMillis = 0;

  // The account is out of balance (OutOfBalanceHold): the no-exit rules hold.
  bool outOfBalance = false;
};

struct DeadTunnelVerdict {
  DeadTunnelReason reason = DeadTunnelReason::None;
  // A countdown runs and is within DeadTunnelSettings::failsafeNoticeMillis of
  // firing (StatusReply::failsafe_armed).
  bool armed = false;
  // Until the earliest deadline, while armed; 0 otherwise.
  int64_t millisToFailsafe = 0;
};

namespace detail {
// How long since `stampMillis`, counted from `startMillis` when it is -1.
inline constexpr int64_t AgeSince(int64_t stampMillis, int64_t startMillis, int64_t nowMillis) {
  return nowMillis - (stampMillis < 0 ? startMillis : stampMillis);
}
// An age with the time spent offline inside it left out.
inline constexpr int64_t OnlineAge(int64_t ageMillis, int64_t offlineMillis) {
  return ageMillis > offlineMillis ? ageMillis - offlineMillis : 0;
}
}  // namespace detail

// The verdict, under `settings`. Recovery is one-sided and immediate: one
// inbound packet or one proven exit clears every countdown that depends on it.
inline DeadTunnelVerdict Evaluate(const DeadTunnelSignals& s, int64_t nowMillis,
                                  const DeadTunnelSettings& settings) {
  DeadTunnelVerdict v;
  if (!s.tunnelUp || !s.routesInstalled || s.upSinceMillis <= 0) return v;

  const int64_t sampleAge = detail::AgeSince(s.lastSampleMillis, s.upSinceMillis, nowMillis);
  const int64_t provenAge = detail::AgeSince(s.lastProvenMillis, s.upSinceMillis, nowMillis);
  const int64_t trafficStartMillis =
      s.trafficStartMillis > 0 ? s.trafficStartMillis : s.upSinceMillis;
  const int64_t inboundAge =
      detail::AgeSince(s.lastInboundMillis, trafficStartMillis, nowMillis);

  // The carrying veto, first. A packet back out of the tun is this process's
  // only first-hand evidence that the tunnel carries; the proven count is the
  // SDK's claim about its transports and a completed sample is a claim about
  // the SDK answering, and neither may overrule it. It cannot mask NoInbound,
  // which needs the same window of silence, nor a wedge that stops the SDK's
  // packet path too, which delivers nothing and lets the veto lapse within
  // that window.
  //
  // Its known hole (docs/linux_agent_help.md 6.4, open task #44): the tun's
  // counters cannot tell a provider's packet from one the SDK writes itself.
  // The SDK's DNS mux claims every UDP/53 query on the tun and writes the
  // answer itself, a failure included, and resets DoT with a local RST, so a
  // machine whose apps keep resolving can hold the veto with every provider
  // dead, and then none of the three rules below fires. Closing it needs an
  // SDK count of the packets it wrote that no provider sent.
  if (s.lastInboundMillis >= 0 && inboundAge < settings.deadFastMillis) return v;

  // A rule whose window is 0 is off (DeadTunnelSettings).
  const bool unresponsiveApplies = settings.sdkUnresponsiveMillis > 0;
  if (unresponsiveApplies && sampleAge >= settings.sdkUnresponsiveMillis) {
    v.reason = DeadTunnelReason::SdkUnresponsive;
    return v;
  }

  // The two no-exit rules measure online time only, and hold while offline:
  // with no physical path nothing could prove an exit or answer a packet. The
  // veto above and SdkUnresponsive read the raw ages, so being offline neither
  // extends the veto nor hides a wedged SDK. They hold while out of balance too:
  // that session is meant to carry nothing until the user acts.
  const int64_t provenOnlineAge = detail::OnlineAge(provenAge, s.provenOfflineMillis);
  const int64_t inboundOnlineAge = detail::OnlineAge(inboundAge, s.inboundOfflineMillis);
  const bool fastApplies = settings.deadFastMillis > 0 && !s.offline && !s.outOfBalance &&
                           s.providerWindowReady && s.provenCount == 0 &&
                           s.outboundSinceInbound >= settings.deadFastOutboundPackets;
  if (fastApplies && inboundOnlineAge >= settings.deadFastMillis &&
      provenOnlineAge >= settings.deadFastMillis) {
    v.reason = DeadTunnelReason::NoInbound;
    return v;
  }

  // Only once the sampler has answered: never having a reading is
  // SdkUnresponsive's question, not this rule's.
  const bool slowApplies = settings.deadSlowMillis > 0 && !s.offline && !s.outOfBalance &&
                           s.provenCount == 0 && s.lastSampleMillis >= 0;
  if (slowApplies && provenOnlineAge >= settings.deadSlowMillis) {
    v.reason = DeadTunnelReason::NoExit;
    return v;
  }

  // Nothing fired: is a countdown accumulating, and how close is it? The
  // unresponsive one counts only once overdue, the fast one only while its
  // traffic holds, and neither no-exit one while offline or out of balance.
  int64_t soonest = -1;
  auto consider = [&soonest](int64_t remaining) {
    if (remaining < 0) remaining = 0;
    if (soonest < 0 || remaining < soonest) soonest = remaining;
  };
  if (unresponsiveApplies && sampleAge >= settings.sdkUnresponsiveArmMillis) {
    consider(settings.sdkUnresponsiveMillis - sampleAge);
  }
  if (fastApplies) {
    consider(settings.deadFastMillis -
             (inboundOnlineAge < provenOnlineAge ? inboundOnlineAge : provenOnlineAge));
  }
  if (slowApplies) consider(settings.deadSlowMillis - provenOnlineAge);
  if (soonest >= 0 && soonest <= settings.failsafeNoticeMillis) {
    v.armed = true;
    v.millisToFailsafe = soonest;
  }
  return v;
}

// ---- the trackers -----------------------------------------------------------

// Turns the tun's two monotonic packet counters into the edges the verdict
// reads: when something last came back, and how much went in since.
class TrafficTracker {
 public:
  // A new session or traffic clock: everything counted before belongs to
  // something else.
  void Reset(uint64_t outbound, uint64_t inbound) {
    lastOutbound_ = outbound;
    lastInbound_ = inbound;
    outboundAtLastInbound_ = outbound;
    lastInboundMillis_ = -1;
  }

  void Observe(uint64_t outbound, uint64_t inbound, int64_t nowMillis) {
    if (inbound > lastInbound_) {
      lastInbound_ = inbound;
      lastInboundMillis_ = nowMillis;
      // A packet back resets the commitment as well as the clock.
      outboundAtLastInbound_ = outbound;
    }
    if (outbound > lastOutbound_) lastOutbound_ = outbound;
  }

  int64_t lastInboundMillis() const { return lastInboundMillis_; }
  uint64_t outboundSinceInbound() const {
    return lastOutbound_ > outboundAtLastInbound_ ? lastOutbound_ - outboundAtLastInbound_ : 0;
  }

 private:
  uint64_t lastOutbound_ = 0;
  uint64_t lastInbound_ = 0;
  uint64_t outboundAtLastInbound_ = 0;
  int64_t lastInboundMillis_ = -1;
};

// What one reading of the SDK's window status changes. A new destination
// generation (another location, or a reconnect to the same one) restarts every
// clock; its window first being satisfied restarts only NoInbound's traffic
// clock, so a replacement window may honestly take longer than NoInbound's
// window to form without inheriting the old one's silence, and without a second
// NoExit window of grace once it has formed.
struct ConnectionEpochUpdate {
  bool verdictClockReset = false;
  bool trafficClockReset = false;
};

// Follows WindowStatus::ConnectionGeneration and MinSatisfied. An older
// generation is ignored, and readiness is one-way within a generation, so the
// window's ordinary churn cannot keep resetting the failsafe.
class ConnectionEpochTracker {
 public:
  ConnectionEpochUpdate Observe(int64_t connectionGeneration, bool minSatisfied) {
    ConnectionEpochUpdate update;
    if (!initialized_) {
      initialized_ = true;
      connectionGeneration_ = connectionGeneration;
      forming_ = !minSatisfied;
      return update;
    }
    if (connectionGeneration < connectionGeneration_) return update;
    if (connectionGeneration > connectionGeneration_) {
      connectionGeneration_ = connectionGeneration;
      forming_ = !minSatisfied;
      update.verdictClockReset = true;
      update.trafficClockReset = true;
      return update;
    }
    if (forming_ && minSatisfied) {
      forming_ = false;
      update.trafficClockReset = true;
    }
    return update;
  }

  bool FastVerdictEligible() const { return initialized_ && !forming_; }
  int64_t connectionGeneration() const { return connectionGeneration_; }

 private:
  bool initialized_ = false;
  bool forming_ = false;
  int64_t connectionGeneration_ = 0;
};

// ---- one session's watch ----------------------------------------------------

// What one tick reads: the clock, the tunnel's state, the tun's counters and
// the exit sampler's last readings.
struct WatchInputs {
  int64_t nowMillis = 0;
  bool tunnelUp = false;
  bool routesInstalled = false;
  // tx_packets (into the tunnel) and rx_packets (back out of it) of the tun.
  uint64_t outboundPackets = 0;
  uint64_t inboundPackets = 0;
  int64_t provenCount = 0;
  int64_t lastProvenMillis = -1;
  int64_t lastSampleMillis = -1;
  // The destination generation and whether its window is satisfied, as the
  // sampler last read them. Without a reading, 0 and true: one generation,
  // formed.
  int64_t connectionGeneration = 0;
  bool providerWindowMinSatisfied = true;
  // The two values above come from a completed sample. Until one has, the
  // destination clocks have no baseline and NoInbound waits: the sampler's
  // first sample, taken at the up edge, sets the level, so the session's own
  // generation is never read as a new one.
  bool windowSampled = true;
  // The reaper's path tracker finds no usable physical path in any family.
  bool offline = false;
  // The sampler's last reading of the balance.
  BalanceReading balance;
};

struct WatchTick {
  DeadTunnelVerdict verdict;
  // The tick came DeadTunnelSettings::evaluatorFrozenMillis or more after the
  // one before it, so every window was rebased and no verdict was reached.
  bool froze = false;
  int64_t tickGapMillis = 0;
  bool verdictClockReset = false;
  bool trafficClockReset = false;
  // What this tick's network state did (StepOffline), whether it is the first
  // offline tick, and how long the stretch that just ended (or still runs) has
  // lasted. BackRebased rebases every window, like a freeze, and reaches no
  // verdict on that tick.
  OfflineStep offlineStep = OfflineStep::Online;
  bool wentOffline = false;
  int64_t offlineStretchMillis = 0;
  // What this tick's reading did to the balance hold, and whether it holds now.
  // Released rebases every window, like a freeze, and reaches no verdict on
  // that tick.
  BalanceStep balanceStep = BalanceStep::None;
  bool outOfBalance = false;
  // The verdict's inputs, for the log line of a teardown.
  DeadTunnelSignals signals;
};

// The evaluator of one Up session: the freeze rebase, the destination clocks,
// the offline and balance holds and the traffic edges around Evaluate, ticked
// every evaluateIntervalMillis, every timing read from the settings it was built
// with.
class DeadTunnelWatch {
 public:
  DeadTunnelWatch() = default;
  explicit DeadTunnelWatch(const DeadTunnelSettings& settings) : settings_(settings) {}

  const DeadTunnelSettings& settings() const { return settings_; }

  // The session's up edge.
  void Start(const WatchInputs& in) {
    lastTickMillis_ = in.nowMillis;
    epoch_ = ConnectionEpochTracker();
    outOfBalance_ = OutOfBalanceHold();
    Rebase(in);
  }

  WatchTick Tick(const WatchInputs& in) {
    WatchTick tick;
    const int64_t now = in.nowMillis;
    tick.tickGapMillis = now - lastTickMillis_;
    lastTickMillis_ = now;
    // Ahead of the rebases: a suspend or a return to a network restarts the
    // windows, and tells nothing about the balance.
    tick.balanceStep = outOfBalance_.Observe(in.balance);
    tick.outOfBalance = outOfBalance_.held();
    if (EvaluatorFroze(tick.tickGapMillis, settings_)) {
      // Nothing watched the session in that gap: every age would be the length
      // of the gap, not of a failure. Rebase, and take the current readings as
      // the new baseline.
      tick.froze = true;
      Rebase(in);
      return tick;
    }
    // The time since the last tick was spent offline when the last tick found
    // the machine offline; the stretch is the run of such time.
    const int64_t offlineGap = offline_ ? tick.tickGapMillis : 0;
    offlineStretchMillis_ += offlineGap;
    tick.offlineStep = StepOffline(offline_, in.offline, offlineStretchMillis_, settings_);
    tick.wentOffline = !offline_ && in.offline;
    tick.offlineStretchMillis = offlineStretchMillis_;
    offline_ = in.offline;
    if (!in.offline) offlineStretchMillis_ = 0;
    if (tick.offlineStep == OfflineStep::BackRebased) {
      // Back on a network after long enough that everything measured before
      // describes another one: a fresh grace, exactly as after a freeze.
      Rebase(in);
      return tick;
    }
    if (tick.balanceStep == BalanceStep::Released) {
      // Everything measured while held was a session meant to carry nothing:
      // a fresh grace, or the rule the hold kept back would fire on this tick.
      Rebase(in);
      return tick;
    }
    // The first sampled reading is the baseline, never a change.
    const ConnectionEpochUpdate update =
        in.windowSampled ? epoch_.Observe(in.connectionGeneration, in.providerWindowMinSatisfied)
                         : ConnectionEpochUpdate{};
    tick.verdictClockReset = update.verdictClockReset;
    tick.trafficClockReset = update.trafficClockReset;
    if (update.verdictClockReset) sessionStartMillis_ = now;
    if (update.trafficClockReset) {
      trafficStartMillis_ = now;
      traffic_.Reset(in.outboundPackets, in.inboundPackets);
    }
    traffic_.Observe(in.outboundPackets, in.inboundPackets, now);

    // The offline time inside each no-exit window: grown by an interval spent
    // offline, and started over whenever the evidence the window is measured
    // from moves (a new proven exit, a packet back, a clock that restarted).
    provenOfflineMillis_ += offlineGap;
    inboundOfflineMillis_ += offlineGap;
    if (update.verdictClockReset || in.lastProvenMillis != provenSeenMillis_) {
      provenOfflineMillis_ = 0;
    }
    provenSeenMillis_ = in.lastProvenMillis;
    if (update.trafficClockReset || traffic_.lastInboundMillis() != inboundSeenMillis_) {
      inboundOfflineMillis_ = 0;
    }
    inboundSeenMillis_ = traffic_.lastInboundMillis();

    DeadTunnelSignals& s = tick.signals;
    s.tunnelUp = in.tunnelUp;
    s.routesInstalled = in.routesInstalled;
    s.upSinceMillis = sessionStartMillis_;
    s.trafficStartMillis = trafficStartMillis_;
    s.providerWindowReady = epoch_.FastVerdictEligible();
    s.provenCount = in.provenCount;
    // A sample taken before a rebase says nothing about the session now.
    s.lastProvenMillis = StampInSession(in.lastProvenMillis, sessionStartMillis_);
    s.lastSampleMillis = StampInSession(in.lastSampleMillis, sessionStartMillis_);
    s.lastInboundMillis = traffic_.lastInboundMillis();
    s.outboundSinceInbound = traffic_.outboundSinceInbound();
    s.offline = in.offline;
    s.provenOfflineMillis = provenOfflineMillis_;
    s.inboundOfflineMillis = inboundOfflineMillis_;
    s.outOfBalance = outOfBalance_.held();
    tick.verdict = Evaluate(s, now, settings_);
    return tick;
  }

  int64_t sessionStartMillis() const { return sessionStartMillis_; }
  int64_t trafficStartMillis() const { return trafficStartMillis_; }

 private:
  // Every window starts again from `in`: the up edge, a freeze, a return from a
  // long offline stretch, and the balance coming back. Nothing measured before
  // it is evidence now.
  void Rebase(const WatchInputs& in) {
    sessionStartMillis_ = in.nowMillis;
    trafficStartMillis_ = in.nowMillis;
    traffic_.Reset(in.outboundPackets, in.inboundPackets);
    if (in.windowSampled) epoch_.Observe(in.connectionGeneration, in.providerWindowMinSatisfied);
    offline_ = in.offline;
    offlineStretchMillis_ = 0;
    provenOfflineMillis_ = 0;
    inboundOfflineMillis_ = 0;
    provenSeenMillis_ = in.lastProvenMillis;
    inboundSeenMillis_ = traffic_.lastInboundMillis();
  }

  DeadTunnelSettings settings_;
  int64_t sessionStartMillis_ = 0;
  int64_t trafficStartMillis_ = 0;
  int64_t lastTickMillis_ = 0;
  TrafficTracker traffic_;
  ConnectionEpochTracker epoch_;
  // The offline hold: the last tick's reading, the stretch it is part of, the
  // offline time inside the proven and inbound windows, and the evidence those
  // windows were last measured from.
  bool offline_ = false;
  int64_t offlineStretchMillis_ = 0;
  int64_t provenOfflineMillis_ = 0;
  int64_t inboundOfflineMillis_ = 0;
  int64_t provenSeenMillis_ = -1;
  int64_t inboundSeenMillis_ = -1;
  // Kept across every rebase; a new session starts it again.
  OutOfBalanceHold outOfBalance_;
};

}  // namespace urnw::watchdog
