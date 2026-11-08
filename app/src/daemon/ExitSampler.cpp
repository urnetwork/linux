// SPDX-License-Identifier: MPL-2.0
#include "daemon/ExitSampler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <urnetwork_sdk.hpp>

#include "daemon/DaemonLog.hpp"

namespace urnw {

// What the sampler thread and its owner share. The thread holds a share, so a
// sampler that outlives its session's interest still writes to live memory.
struct ExitSampler::Channel {
  // A network change the device has not been told about yet.
  enum class Kick { None, Quality, Path };

  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> cancelled{false};
  uint64_t deviceHandle = 0;
  Clock clock = nullptr;
  int64_t sampleIntervalMillis = 0;
  // Guarded by mutex.
  Kick kick = Kick::None;
  watchdog::BalanceReading balance;
  watchdog::ClientLimitReading clientLimit;

  std::atomic<int64_t> provenCount{0};
  std::atomic<int64_t> lastProvenMillis{-1};
  std::atomic<int64_t> lastSampleMillis{-1};
  std::atomic<int64_t> connectionGeneration{0};
  std::atomic<bool> providerWindowMinSatisfied{true};
  std::atomic<bool> sampled{false};
};

namespace {

// One SDK read through the C ABI: the json the call returns, nullopt for none.
template <typename T>
std::optional<T> ReadSdkJson(char* json) {
  const std::optional<std::string> text = urnet::detail::takeStringOpt(json);
  if (!text) return std::nullopt;
  return urnet::detail::parseJson<T>(text->c_str());
}

// Logged once per process: a device that cannot answer cannot answer every
// two seconds, and the log is read by people.
void NoteSampleFailure(const char* what, const char* error) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) return;
  DaemonLogf("[tunnel] failsafe: reading the %s failed (%s); a sample that throws still "
             "counts as answered\n",
             what, error);
}

}  // namespace

ExitSampler::~ExitSampler() { Stop(); }

bool ExitSampler::Start(uint64_t deviceHandle, Clock clock, int64_t sampleIntervalMillis) {
  Stop();
  if (deviceHandle == 0 || clock == nullptr || sampleIntervalMillis <= 0) return false;
  auto channel = std::make_shared<Channel>();
  channel->deviceHandle = deviceHandle;
  channel->clock = clock;
  channel->sampleIntervalMillis = sampleIntervalMillis;
  try {
    // Detached: Stop never waits for it (see the header).
    std::thread([channel] { Run(channel); }).detach();
  } catch (const std::exception& e) {
    DaemonLogf("[tunnel] failsafe: the exit sampler could not start (%s); this tunnel is not "
               "watched\n",
               e.what());
    return false;
  }
  channel_ = channel;
  return true;
}

void ExitSampler::Stop() {
  if (!channel_) return;
  {
    // Under the lock, so the store cannot fall between the sampler's check of
    // the predicate and its wait.
    std::scoped_lock lock(channel_->mutex);
    channel_->cancelled.store(true);
  }
  channel_->wake.notify_all();
  channel_.reset();
}

void ExitSampler::NoteNetworkChange(bool path) {
  if (!channel_) return;
  {
    std::scoped_lock lock(channel_->mutex);
    if (path) {
      channel_->kick = Channel::Kick::Path;
    } else if (channel_->kick == Channel::Kick::None) {
      channel_->kick = Channel::Kick::Quality;
    }
  }
  channel_->wake.notify_all();
}

ExitSampler::Reading ExitSampler::Read() const {
  Reading reading;
  if (!channel_) return reading;
  reading.provenCount = channel_->provenCount.load();
  reading.lastProvenMillis = channel_->lastProvenMillis.load();
  reading.lastSampleMillis = channel_->lastSampleMillis.load();
  reading.connectionGeneration = channel_->connectionGeneration.load();
  reading.providerWindowMinSatisfied = channel_->providerWindowMinSatisfied.load();
  reading.sampled = channel_->sampled.load();
  {
    std::scoped_lock lock(channel_->mutex);
    reading.balance = channel_->balance;
    reading.clientLimit = channel_->clientLimit;
  }
  return reading;
}

void ExitSampler::Run(const std::shared_ptr<Channel>& channel) {
  // Nothing may escape a thread of the root daemon: std::terminate would end
  // it with the capture routes in.
  try {
    int64_t nextSampleMillis = channel->clock();
    for (;;) {
      Channel::Kick kick = Channel::Kick::None;
      {
        // Until the next sample is due or a network change arrives.
        std::unique_lock<std::mutex> lock(channel->mutex);
        const int64_t waitMillis = nextSampleMillis - channel->clock();
        if (waitMillis > 0) {
          channel->wake.wait_for(lock, std::chrono::milliseconds(waitMillis), [&] {
            return channel->cancelled.load() || channel->kick != Channel::Kick::None;
          });
        }
        if (channel->cancelled.load()) return;
        kick = channel->kick;
        channel->kick = Channel::Kick::None;
      }
      // Here and not on the reaper: both take the device's state lock, and a
      // device stuck behind it must hold up this thread, whose silence is the
      // failsafe's evidence, never the main loop that reaches the verdict.
      if (kick == Channel::Kick::Path) {
        urnet_device_local_network_changed(channel->deviceHandle);
      } else if (kick == Channel::Kick::Quality) {
        urnet_device_local_network_quality_changed(channel->deviceHandle);
      }
      if (channel->cancelled.load()) return;
      if (channel->clock() < nextSampleMillis) continue;
      nextSampleMillis = channel->clock() + channel->sampleIntervalMillis;

      // The contract status before the window status: a destination change
      // resets the one and moves the other's generation, so in this order a
      // status it reset is never read with the generation before it
      // (BalanceReading).
      std::optional<urnet::ContractStatus> contract;
      try {
        contract = ReadSdkJson<urnet::ContractStatus>(
            urnet_device_get_contract_status(channel->deviceHandle));
      } catch (const std::exception& e) {
        NoteSampleFailure("contract status", e.what());
      }
      if (channel->cancelled.load()) return;
      std::optional<urnet::WindowStatus> window;
      try {
        window = ReadSdkJson<urnet::WindowStatus>(
            urnet_device_get_window_status(channel->deviceHandle));
      } catch (const std::exception& e) {
        NoteSampleFailure("window status", e.what());
      }
      if (channel->cancelled.load()) return;
      // The client limit hold (watchdog::ClientLimitHold), off the device the
      // app's notice reads it from: whether the platform closed it for its
      // network's concurrent client limit and the sdk holds it off the platform.
      std::optional<urnet::ClientLimitStatus> clientLimit;
      try {
        clientLimit = ReadSdkJson<urnet::ClientLimitStatus>(
            urnet_device_get_client_limit_status(channel->deviceHandle));
      } catch (const std::exception& e) {
        NoteSampleFailure("client limit status", e.what());
      }
      if (channel->cancelled.load()) return;
      int64_t proven = 0;
      try {
        if (const auto exits = ReadSdkJson<urnet::ExitList>(
                urnet_device_local_get_exits(channel->deviceHandle))) {
          for (const urnet::Exit& exit : *exits) {
            if (exit.Proven) ++proven;
          }
        }
      } catch (const std::exception& e) {
        NoteSampleFailure("exits", e.what());
      }
      // Checked again on the far side of the calls: a sample published after
      // the session ended would be evidence about a tunnel that is gone.
      if (channel->cancelled.load()) return;
      const int64_t now = channel->clock();
      if (window) {
        channel->connectionGeneration.store(window->ConnectionGeneration);
        channel->providerWindowMinSatisfied.store(window->MinSatisfied);
      }
      if (contract && window) {
        std::scoped_lock lock(channel->mutex);
        channel->balance.sampled = true;
        channel->balance.insufficientBalance = contract->InsufficientBalance;
        channel->balance.noPermission = contract->NoPermission;
        channel->balance.connectionGeneration = window->ConnectionGeneration;
        channel->balance.providerAddedCount = window->ProviderStateAdded;
      }
      if (clientLimit) {
        std::scoped_lock lock(channel->mutex);
        channel->clientLimit.sampled = true;
        channel->clientLimit.exceeded = clientLimit->Status == urnet::ClientLimitStatusExceeded;
      }
      channel->provenCount.store(proven);
      if (proven >= 1) channel->lastProvenMillis.store(now);
      channel->sampled.store(true);
      // Last: this stamp means a sample completed and its readings are already
      // published, never that one was attempted.
      channel->lastSampleMillis.store(now);
    }
  } catch (...) {
    // With the sampler gone the session is judged by its packets: the carrying
    // veto holds while anything comes back, and SdkUnresponsive fires once
    // nothing has for its window.
    DaemonLogf("[tunnel] failsafe: the exit sampler stopped on an exception\n");
  }
}

}  // namespace urnw
