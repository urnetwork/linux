// The concurrent client limit, as the GUI shows it (owner, 2026-11-08: Linux
// at parity with the apple and android apps).
//
// The platform closes a device over its network's concurrent client limit, and
// the sdk then holds the device off the platform for 15 to 20 minutes and
// reports it (Device.GetClientLimitStatus). While a connection is requested the
// user's traffic stays in the tunnel with nowhere to go, so the Connect page
// says what to do, and a desktop notification says it while the app is out of
// the user's sight. Nothing here disconnects: Disconnect stays where it is, and
// the daemon's dead-tunnel failsafe holds such a session too
// (TunnelWatchdog.hpp ClientLimitHold), so the notice stays true.
//
// The decisions are the sdk's, the same on every platform: what to offer
// (ClientLimitStatus.Prompt, and AuthNetworkClientError.ClientLimitPrompt for a
// refused sign-in) and when the notification shows (ClientLimitNoticeTracker).
// This holds only what the GUI adds: the words for each prompt, which buttons
// go beside them, and SdkHost's feed of the hold (its staleness and order).
//
// Pure and dependency-free (the sdk's values arrive as the enums below, read by
// SdkHost and MainWindow, which have the sdk), so
// tests/ClientLimitNoticeTest.cpp runs it without GTK or the SDK. Not thread
// safe: the window drives it from the GTK main loop, SdkHost's feed under its
// own lock.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace urnw {
namespace client_limit {

// A line the GUI shows: a store key and its English (the gettext msgid).
struct Text {
  const char* key;
  const char* english;
};

inline constexpr Text kTitle{"client_limit_reached", "Device limit reached"};
// A free plan: UR Pro connects more (Get Pro beside it).
inline constexpr Text kUpgradeBody{
    "client_limit_upgrade_message",
    "Your free plan connects a limited number of devices at once. Upgrade to UR Pro to connect "
    "more, or disconnect URnetwork on another device."};
// A Pro plan, already the highest: only a freed slot lifts it.
inline constexpr Text kRemoveClientBody{
    "client_limit_remove_client_message",
    "Your plan's limit of devices connected at once is reached. Sign out of URnetwork on a device "
    "you no longer use, then try again."};

// The sdk's prompt: ClientLimitPromptUpgrade, ClientLimitPromptRemoveClient, or
// none (no hold or refusal, or a device process too old to send one).
enum class Prompt { None, Upgrade, RemoveClient };

// A free plan is offered UR Pro; a Pro plan, already the highest, is told to
// free a slot. A device process too old to send a prompt reads as free, as the
// apple app reads it (ClientLimitHold.offersUpgrade).
inline constexpr bool OffersUpgrade(Prompt prompt) { return prompt != Prompt::RemoveClient; }

inline constexpr const Text& BodyFor(Prompt prompt) {
  return OffersUpgrade(prompt) ? kUpgradeBody : kRemoveClientBody;
}

// What a refused sign-in says when the client limit refused it: the body for
// the refusal's prompt. Null for every other refusal, which keeps the server's
// words or the screen's own line.
inline constexpr const Text* SignInRefusalBody(Prompt prompt) {
  switch (prompt) {
    case Prompt::Upgrade:
      return &kUpgradeBody;
    case Prompt::RemoveClient:
      return &kRemoveClientBody;
    case Prompt::None:
      break;
  }
  return nullptr;
}

// One readout of the device's ClientLimitStatus.
struct Status {
  // Status is ClientLimitStatusExceeded: the platform closed the device for its
  // network's client limit, and the sdk holds it off until retryTimeMillis.
  bool exceeded = false;
  // RetryTime: when the hold ends, unix epoch milliseconds; 0 with none.
  int64_t retryTimeMillis = 0;
  Prompt prompt = Prompt::None;

  bool operator==(const Status& other) const {
    return exceeded == other.exceeded && retryTimeMillis == other.retryTimeMillis &&
           prompt == other.prompt;
  }
  bool operator!=(const Status& other) const { return !(*this == other); }
};

// What SdkHost hands the window.
struct Reading {
  // A device is bound and its remote is connected: `status` is the device's
  // own. Otherwise there is nothing to show (a DeviceRemote whose daemon is gone
  // answers with its last readout, which can be a hold that has ended).
  bool available = false;
  Status status;
  // SdkHost's order: a reading marshaled late behind a newer one is dropped.
  uint64_t serial = 0;

  bool SameAs(const Reading& other) const {
    return available == other.available && status == other.status;
  }
};

// ---- SdkHost's feed ----------------------------------------------------------
// What the bound device last said and whether its remote is connected, by
// device generation, and the reading that follows from them: the slow-internet
// feed's rules (SlowInternet.hpp Feed). A push or an edge from a device that has
// since been torn down is dropped. While the remote is disconnected nothing
// shows and its last status is forgotten, so a hold the daemon no longer has
// never shows; the sync that reconnects the remote replays the current status.
//
// Registration replays nothing, so SdkHost reads both values once after
// registering. Such a read applies only when no push of its kind landed since
// the revision taken before it: a push is always newer than the read.
class Feed {
 public:
  // A device was bound: nothing is known about it yet. Its generation.
  uint64_t Bind() {
    Reset();
    return generation_;
  }
  // The device is gone, or its daemon's tunnel is: nothing shows until the next
  // Bind.
  void Clear() { Reset(); }

  // The device's listener. True when the reading changed.
  bool NoteStatus(uint64_t generation, const Status& status) {
    if (generation != generation_) return false;
    ++statusRevision_;
    if (status == status_) return false;
    status_ = status;
    return remoteConnected_;
  }
  // The remote's connection edge. True when the reading changed.
  bool NoteRemote(uint64_t generation, bool connected) {
    if (generation != generation_) return false;
    ++remoteRevision_;
    if (connected == remoteConnected_) return false;
    remoteConnected_ = connected;
    if (!connected) status_ = Status{};
    return true;
  }

  uint64_t Generation() const { return generation_; }
  uint64_t StatusRevision() const { return statusRevision_; }
  uint64_t RemoteRevision() const { return remoteRevision_; }
  // The reads after registering, taken after `revision` was.
  bool SeedStatus(uint64_t generation, uint64_t revision, const Status& status) {
    if (revision != statusRevision_) return false;
    return NoteStatus(generation, status);
  }
  bool SeedRemote(uint64_t generation, uint64_t revision, bool connected) {
    if (revision != remoteRevision_) return false;
    return NoteRemote(generation, connected);
  }

  Reading Current() const {
    Reading reading;
    reading.available = remoteConnected_;
    if (remoteConnected_) reading.status = status_;
    return reading;
  }

 private:
  void Reset() {
    ++generation_;
    remoteConnected_ = false;
    status_ = Status{};
  }

  uint64_t generation_ = 0;
  uint64_t statusRevision_ = 0;
  uint64_t remoteRevision_ = 0;
  bool remoteConnected_ = false;
  Status status_;
};

// The device is held now, as far as the window can tell.
inline bool Held(const Reading& reading) { return reading.available && reading.status.exceeded; }

// ---- the Connect page's notice -------------------------------------------------

struct Notice {
  // Under the connect action while the device is held and a connection is
  // requested (the sdk's tracker shows its notification on the same two facts).
  bool shows = false;
  // Get Pro beside Retry: a free plan. A Pro plan gets Retry alone.
  bool offersUpgrade = true;
  Text body = kUpgradeBody;
};

// `connectRequested`: a destination is selected (balance_notice::Signals'
// reading of it), so the tunnel is asked to carry traffic.
inline Notice NoticeFor(const Reading& reading, bool connectRequested) {
  Notice notice;
  notice.shows = Held(reading) && connectRequested;
  notice.offersUpgrade = OffersUpgrade(reading.status.prompt);
  notice.body = BodyFor(reading.status.prompt);
  return notice;
}

// ---- the desktop notification --------------------------------------------------

// The sdk's ClientLimitNoticeTracker's answer (ClientLimitNotice*), as
// MainWindow reads it off the sdk's strings.
enum class TrackerAction {
  None,
  // show the notice and alert the user
  Post,
  // show it, or replace the one showing, without alerting
  Update,
  // withdraw it
  Remove,
};

// What the window does with the notification.
enum class NotificationStep {
  None,
  Send,
  SendQuietly,
  Withdraw,
};

// `background`: the app is out of the user's sight, its window hidden to the
// tray or another app active. In front the Connect page's notice says it, so
// nothing is sent; a withdraw always goes through, whatever is on screen.
inline constexpr NotificationStep NotificationStepFor(TrackerAction action, bool background) {
  switch (action) {
    case TrackerAction::Post:
      return background ? NotificationStep::Send : NotificationStep::None;
    case TrackerAction::Update:
      return background ? NotificationStep::SendQuietly : NotificationStep::None;
    case TrackerAction::Remove:
      return NotificationStep::Withdraw;
    case TrackerAction::None:
      break;
  }
  return NotificationStep::None;
}

}  // namespace client_limit
}  // namespace urnw
