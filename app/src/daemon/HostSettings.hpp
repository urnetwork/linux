// The daemon's timings for the network-switch path, each with its default
// here: the reaper's cadence, its re-checks of the nftables table, the DNS
// override and the egress witness, and the settings of the three pure watchers
// the reaper drives (the network path tracker, the suspend detector and the
// dead-tunnel failsafe). main.cpp builds its TunnelHost with these defaults, and
// a caller or a test can change any one of them without touching the code that
// reads it.
//
// A tick is one run of the reaper, reaper_interval_seconds apart. Header-only
// and free of the SDK, so the unit suite reads the same defaults and decisions
// the daemon does.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "NetworkQuality.hpp"
#include "TunnelWatchdog.hpp"

namespace urnw {

struct HostSettings {
  // ---- the reaper (TunnelHost::Reap) ----------------------------------------
  // Seconds between two reaper ticks, on g_timeout_add_seconds, which lets GLib
  // wake for it together with the system's other once-a-second timers. Every
  // *_ticks field below counts these ticks, and the reaper ticks the dead-tunnel
  // watch, so dead_tunnel.evaluateIntervalMillis must be the same interval
  // (asserted for the defaults; TunnelHost logs a mismatch). 1: the cadence
  // every window here was chosen against. 0 is read as 1 (ReaperIntervalSeconds):
  // a reaper that never waited would spin the main loop.
  unsigned reaper_interval_seconds = 1;

  // ---- the nftables table's re-check (TunnelHost::MaintainFilterLocked) -----
  // Ticks between two checks that `table inet urnetwork` is still in force, and
  // between two retries of an exclusion slice nft refused. nftables has no
  // tamper callback, so a poll is the whole mitigation for a foreign `nft flush
  // ruleset` (Fedora/Bazzite's shipped nftables.conf begins with one), and each
  // poll is one `nft list table` fork: 5 bounds the tamper window at 5 s for
  // ~17k execs a day instead of 86k. 0 checks every tick, as 1 does.
  int filter_verify_interval_ticks = 5;
  // Of a run of failed checks or retries, only every this-many-th is logged, so
  // an nft that keeps failing (uninstalled underneath us) is not a journal line
  // per check: 12 is about once a minute at the default interval. 0 is read as
  // 1, every failure logged (FilterFailureLogged).
  int filter_verify_log_every = 12;

  // ---- the DNS override's re-check (TunnelHost::MaintainDnsLocked) ---------
  // NetworkManager's dns=default plugin and dhcpcd rewrite /etc/resolv.conf on
  // EVERY connectivity change, so the file tiers are displaced at every network
  // switch, and the resolvectl tier can be displaced by a resolved restart.
  // Every tick a displacing file stands is a tick every lookup on the machine is
  // refused by the DNS floor.
  //
  // Ticks between two checks of the file tiers (resolvconf, the direct file)
  // while they pass and nothing contests the file. 1: the check is one file
  // read, so a switch costs names at most a tick. 0 checks every tick, as 1.
  int dns_file_verify_interval_ticks = 1;
  // Ticks between two checks that fork (systemd-resolved's resolvectl), after a
  // repair that failed, and of a contested file. 10: a fork every ten seconds,
  // and ten seconds between the failed repairs dns_repair_attempts counts. 0
  // checks every tick.
  int dns_verify_interval_ticks = 10;
  // A repair needed again within this many ticks of the last one is a writer
  // rewriting the file over us on its own schedule, not a network switch: the
  // file is contested, and checking it every tick would only turn that into a
  // rewrite and two journal lines a second. 10: one slow interval. 0: never
  // contested, so a file rewritten every tick is rewritten back every tick.
  int dns_contested_window_ticks = 10;
  // Consecutive failed re-applies before the session is stopped: 3, about
  // thirty seconds of unprotected names at the default interval, bounded by the
  // floor. 0 stops the session on the first failed re-apply, as 1 does.
  int dns_repair_attempts = 3;

  // ---- the live egress witness (TunnelHost::CheckEgressWitnessLocked) ------
  // Ticks between two re-checks: one nft read and two sockets, no packet, cheap
  // but not for every tick. 30: with two failures to stop, about a minute of
  // unproven egress against the forty the incident it exists for ran. 0
  // re-checks every tick, as 1.
  int egress_witness_interval_ticks = 30;
  // Consecutive failed re-checks that stop the session: 2, so one odd reading is
  // a warning and not a teardown. 0 stops on the first failure, as 1 does.
  int egress_witness_failures_to_stop = 2;

  // ---- the pure watchers ---------------------------------------------------
  LinuxNetworkPathSettings network_path;
  LinuxSuspendSettings suspend;
  watchdog::DeadTunnelSettings dead_tunnel;
};

// The reaper ticks the dead-tunnel watch: their default cadences agree.
static_assert(static_cast<std::int64_t>(HostSettings{}.reaper_interval_seconds) * 1000 ==
              HostSettings{}.dead_tunnel.evaluateIntervalMillis);

// The reaper's interval as `settings` asks for it, 0 read as one second.
inline unsigned ReaperIntervalSeconds(const HostSettings& settings) {
  return settings.reaper_interval_seconds > 0 ? settings.reaper_interval_seconds : 1u;
}

// Whether the `failures`-th failure of a run (counting from 0) is one that is
// logged.
inline bool FilterFailureLogged(const HostSettings& settings, int failures) {
  const int every = settings.filter_verify_log_every > 0 ? settings.filter_verify_log_every : 1;
  return failures % every == 0;
}

// What one re-check of the egress witness does to a running session. `failures`
// is the run of failed re-checks before this one: a pass ends it, and a failure
// stops the session once the run reaches egress_witness_failures_to_stop, so a
// reading a network change caught half way costs a warning and not the session.
// The run is the caller's to clear after it has logged a stop.
enum class EgressRecheckStep {
  Passed,
  Warned,
  Stop,
};

inline EgressRecheckStep StepEgressRecheck(const HostSettings& settings, bool passed,
                                           int* failures) {
  if (passed) {
    *failures = 0;
    return EgressRecheckStep::Passed;
  }
  ++*failures;
  return *failures < settings.egress_witness_failures_to_stop ? EgressRecheckStep::Warned
                                                              : EgressRecheckStep::Stop;
}

// How many ticks the next DNS check waits: the file tiers' every-tick read
// while their check passes and the file is not contested, the slower interval
// for a check that forks, after a failed repair, and for a contested file.
inline int DnsVerifyIntervalTicks(const HostSettings& settings, bool forkingCheck,
                                  bool repairFailing, bool contested) {
  return forkingCheck || repairFailing || contested ? settings.dns_verify_interval_ticks
                                                    : settings.dns_file_verify_interval_ticks;
}

// Whether a repair `ticksSinceRepair` ticks after the last one finds the file
// contested.
inline bool DnsRepairContested(const HostSettings& settings, int ticksSinceRepair) {
  return ticksSinceRepair < settings.dns_contested_window_ticks;
}

}  // namespace urnw
