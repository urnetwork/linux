// The dead-tunnel failsafe's view of the SDK (TunnelWatchdog.hpp): a thread of
// its own that asks a session's device for its contract status, its window
// status and its exits every sample interval
// (DeadTunnelSettings::sdkSampleIntervalMillis, handed to Start), publishes what
// the last completed sample said, and tells the device when the physical network
// changed.
//
// A thread, not the reaper, because every one of those calls can block on the
// device's state lock while the reaper runs on the main loop that answers
// `status` and reaches the failsafe's verdict. A sampler stuck inside the SDK
// is what the SdkUnresponsive rule detects, by its silence, so it must never be
// waited for, and the main loop must never be the thread that is stuck.
//
// It never touches the session's urnet::DeviceLocal object. It holds the
// device's C handle by value and calls the C ABI with it: the SDK never reuses a
// handle (cgo/handles.go), so after the session releases its device a late call
// resolves nothing and returns empty, and a call already inside the SDK keeps
// the Go object alive by itself. Stop therefore only cancels. A sampler still
// inside a call comes out of it later and touches nothing but its own shared
// block, which it owns a share of.
//
// Start, Stop, Read and NoteNetworkChange are called under TunnelHost's
// opMutex_; the published readings are atomics, or a copy under the channel's
// lock that no SDK call is ever made inside, and a kick is a flag, so none of
// them waits for the sampler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>

#include "TunnelWatchdog.hpp"

namespace urnw {

class ExitSampler {
 public:
  // The last completed sample. Timestamps are on the clock Start was given, -1
  // for none yet.
  struct Reading {
    int64_t provenCount = 0;
    int64_t lastProvenMillis = -1;
    int64_t lastSampleMillis = -1;
    // WindowStatus::ConnectionGeneration and MinSatisfied as the last sample
    // read them; 0 and true before one has, or when its read failed.
    int64_t connectionGeneration = 0;
    bool providerWindowMinSatisfied = true;
    // A sample has completed, so the two values above are its readings.
    bool sampled = false;
    // The balance as the last sample that could read it found it. One copy, so
    // a status is never paired with another sample's generation.
    watchdog::BalanceReading balance;
  };
  using Clock = int64_t (*)();

  ExitSampler() = default;
  ~ExitSampler();
  ExitSampler(const ExitSampler&) = delete;
  ExitSampler& operator=(const ExitSampler&) = delete;

  // Samples the device behind `deviceHandle` at once and then every
  // `sampleIntervalMillis` (positive: the caller passes
  // watchdog::SampleIntervalMillis), stamping on `clock`, until Stop. Stops a
  // previous sampler first. False when no thread could be started, or for a
  // nonpositive interval: then nothing samples, and the caller must not judge
  // the session on the silence.
  bool Start(uint64_t deviceHandle, Clock clock, int64_t sampleIntervalMillis);
  // Cancels without waiting. Idempotent.
  void Stop();
  // The physical network changed: the sampler tells the device before its next
  // sample, networkChanged for a new path and networkQualityChanged for a
  // quality change alone. Kicks that arrive before it gets to them coalesce,
  // a path change winning. Never waits; a no-op while nothing runs.
  void NoteNetworkChange(bool path);
  // The last completed sample's readings, or the defaults while nothing runs.
  Reading Read() const;

 private:
  struct Channel;
  // The thread's body. Touches only `channel` and the C ABI.
  static void Run(const std::shared_ptr<Channel>& channel);

  std::shared_ptr<Channel> channel_;
};

}  // namespace urnw
