// THE ONE CONNECT READING — the whole of what pane A's status row is allowed
// to say, computed in one pure function from one snapshot.
//
// WHY THIS FILE EXISTS. The hero used to be written from THREE copies of the
// connection state, each with its own writer, its own freshness and its own
// meaning:
//
//   ConnectPage::connected_    <- MainWindow::SetConnected(SdkHost::Connected())
//   ConnectPage::stats_.connected <- LiveStats, and only while the window was
//                                 visible (MainWindow.cpp gated ApplyStats on
//                                 windowVisible_, and did NOT gate the status
//                                 push beside it)
//   ConnectPage::connectStatus_ <- SdkHost's ConnectionStatusHandler push
//
// and the branch that printed "Connected" required TWO of them to agree while
// the branch that printed "Connecting to providers" required the third to hold
// a particular string. Three fixes rearranged those three copies; none of them
// asked what the copies MEAN. They mean this:
//
//   * ConnectViewController::GetConnected() (sdk/connect_view_controller.go:154)
//     returns `self.connected`, and `self.connected` is written in exactly one
//     place — ConnectLocationChanged (:116-125). It is "a destination is
//     SELECTED". It is NOT "providers are attached". LiveStats::connected is a
//     copy of it (SdkHost::ReadStats), and SdkHost::Connected() is that same
//     bit ANDed with "a DeviceRemote is bound over the current control
//     session".
//   * SdkHost's ConnectionStatusHandler — the feed that wrote connectStatus_ —
//     emits exactly TWO strings from its five call sites: "DESTINATION_SET"
//     and "DISCONNECTED". It NEVER emits CONNECTING, CONNECTED or
//     CONNECT_FAILED, so `status == "CONNECTING"` and the whole CONNECT_FAILED
//     branch were unreachable, and DESTINATION_SET — which stands for the
//     ENTIRE life of a carrying session, because the destination stays
//     selected while the tunnel carries — was being read as "in flight".
//
// So none of the three inputs could tell "connecting" from "connected", and
// the one signal that can — ConnectViewController::GetConnectionStatus(),
// which is CONNECTING until the provider window reports MinSatisfied and
// CONNECTED after (connect_view_controller.go:1055-1063) — was fetched into
// LiveStats::connectionStatus and then never read by the status writer.
//
// This header takes the fixed vocabulary instead:
//
//   sessionUp  = a destination is selected AND the daemon's tunnel is still
//                ours. There is something to disconnect from.
//   sdk        = the connect controller's OWN status. CONNECTED is the only
//                token that means providers are attached.
//
// and returns text, dot, hero pose and button action TOGETHER, in one value,
// so no caller can take them from different readings. That is the property the
// four reports were about; it is not a fourth condition.
//
// Pure C++17 on purpose — no GTK, no SDK, no glib. It is a decision table, and
// it is exercised without a display by app/tests/HealthTest.cpp.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cctype>
#include <string>

namespace urnw {
namespace health {

// The connect controller's own status. Unknown is NOT Disconnected: a
// controller that has just been opened reports Disconnected until its first
// window-monitor event (connect_view_controller.go:89), and reading that as
// "not connected" over a carrying tunnel is the same lie pointing the other
// way. SdkHost latches the last KNOWN status for the life of a session so this
// value cannot regress mid-session; Unknown is what is left before any status
// has ever landed.
enum class SdkStatus { Unknown, Disconnected, Connecting, DestinationSet, Connected, Failed };

// What the row says. One value per row of the table below; nothing else is
// representable.
enum class State {
  Disconnected,   // nothing to disconnect from
  Connecting,     // a session is up, no provider has been proven yet
  Evaluating,     // a session is up, providers are in the window, none proven
  Connected,      // a session is up and the controller says providers are attached
  Failed,         // the provider window settled on failure
  Disconnecting,  // a teardown THE USER ASKED FOR is in flight
  Blocked,        // out of balance (overrides the connection reading)
};

// The hero pose. Mirrors ConnectCanvas::State one-for-one; a separate enum so
// this header stays free of GTK and the canvas cannot be written from here.
enum class Hero { Disconnected, Connecting, Connected, Error, Processing };

// The status dot. A token, not a hex: the page owns the palette (§8.1).
enum class Dot { Idle, Connecting, Green, Coral, Amber };

// The one action the hero and the button share.
enum class Action { Connect, Disconnect };

inline SdkStatus ParseSdkStatus(const std::string& raw) {
  if (raw.empty()) return SdkStatus::Unknown;
  std::string s;
  s.reserve(raw.size());
  for (const char c : raw) s += static_cast<char>(::toupper(static_cast<unsigned char>(c)));
  if (s == "CONNECTED") return SdkStatus::Connected;
  if (s == "CONNECTING") return SdkStatus::Connecting;
  if (s == "DESTINATION_SET") return SdkStatus::DestinationSet;
  if (s == "CONNECT_FAILED") return SdkStatus::Failed;
  if (s == "DISCONNECTED") return SdkStatus::Disconnected;
  return SdkStatus::Unknown;  // an unrecognised token is not evidence of anything
}

// The snapshot. Every field is sampled at ONE instant from live getters (see
// SdkHost::ReadConnectReading) — never from a cached copy of another field —
// so two fields of one Signals value can never describe two different moments.
struct Signals {
  SdkStatus sdk = SdkStatus::Unknown;
  // A destination is selected (ConnectViewController::GetConnected()).
  bool destinationSelected = false;
  // A DeviceRemote is still bound over the CURRENT control session: the
  // daemon's tunnel is the one we started, and it is still there.
  bool tunnelBound = false;
  // The provider window's current size (grid.getWindowCurrentSize()). Only
  // meaningful while a grid exists; 0 means "none active yet", which is what
  // separates "Connecting to providers" from "Finding providers…".
  int64_t providerCount = 0;
  // Billing, not connection. It replaces the headline but never the action:
  // an out-of-balance session still has to be disconnectable.
  bool insufficientBalance = false;
  // The user pressed Disconnect and the teardown is still in flight. Bounded
  // by the caller (ConnectPage::DisconnectIntentLive: it clears the moment the
  // session actually reads down, and in any case after 8s).
  bool disconnectRequested = false;
};

// THE ONE PREDICATE. "There is something to disconnect from." It selects the
// button's word, the hero's action and the settled-idle row, so those cannot
// be answers to three different questions.
inline bool SessionUp(const Signals& s) { return s.destinationSelected && s.tunnelBound; }

inline State Aggregate(const Signals& s) {
  // The user's intent outranks the controller's token, and it has to: the SDK
  // goes on reporting a selected destination right through a teardown, so a
  // token-only page answers a Disconnect press with "Connecting to providers".
  if (s.disconnectRequested) return State::Disconnecting;
  // Balance outranks the connection reading (an error the user must act on
  // beats a description of the transport), but never the teardown above.
  if (s.insufficientBalance) return State::Blocked;
  // NOTHING TO DISCONNECT FROM ⇒ Disconnected, whatever the controller says.
  // This is the half that makes a stale token harmless: DESTINATION_SET with
  // no bound tunnel is not "connecting", it is a leftover.
  if (!SessionUp(s)) return State::Disconnected;
  // A session IS up. Only the controller's own status can say whether
  // providers are attached.
  if (s.sdk == SdkStatus::Connected) return State::Connected;
  if (s.sdk == SdkStatus::Failed) return State::Failed;
  // Connecting, DestinationSet, Disconnected and Unknown all mean the same
  // thing over a live session: it is coming up and no provider has been proven
  // yet. The provider window says how far along.
  return s.providerCount > 0 ? State::Evaluating : State::Connecting;
}

// ONE value. Text, dot, hero and action leave this function together or not at
// all — there is no path on which they can be taken from different readings.
struct Reading {
  State state = State::Disconnected;
  const char* textKey = "disconnected";
  const char* textEnglish = "Disconnected";
  Dot dot = Dot::Idle;
  Hero hero = Hero::Disconnected;
  Action action = Action::Connect;
  bool showNotProtected = false;
};

inline Reading Render(const Signals& s) {
  Reading r;
  r.state = Aggregate(s);
  switch (r.state) {
    case State::Connected:
      r.textKey = "connected";
      r.textEnglish = "Connected";
      r.dot = Dot::Green;
      r.hero = Hero::Connected;
      break;
    case State::Evaluating:
      r.textKey = "conn_finding_providers";
      r.textEnglish = "Finding providers…";
      r.dot = Dot::Connecting;
      r.hero = Hero::Connecting;
      break;
    case State::Connecting:
      r.textKey = "connecting_status_indicator";
      r.textEnglish = "Connecting to providers";
      r.dot = Dot::Connecting;
      r.hero = Hero::Connecting;
      break;
    case State::Failed:
      r.textKey = "conn_failed";
      r.textEnglish = "Couldn't connect";
      r.dot = Dot::Coral;
      r.hero = Hero::Error;
      break;
    case State::Disconnecting:
      r.textKey = "site_app_disconnecting";
      r.textEnglish = "Disconnecting…";
      r.dot = Dot::Amber;
      r.hero = Hero::Processing;
      break;
    case State::Blocked:
      r.textKey = "insufficient_balance_add_balance_or_plan";
      r.textEnglish = "Insufficient balance — add balance or a plan";
      r.dot = Dot::Coral;
      r.hero = Hero::Error;
      break;
    case State::Disconnected:
      r.textKey = "disconnected";
      r.textEnglish = "Disconnected";
      r.dot = Dot::Idle;
      r.hero = Hero::Disconnected;
      r.showNotProtected = true;
      break;
  }
  // THE ACTION COMES OUT OF THE SAME CALL AS THE WORD ABOVE IT. It is the one
  // predicate, asked once: a press means "stop" exactly when there is a
  // session to stop, plus while a teardown the user already asked for is still
  // running (so a second press cannot start a tunnel out of a disconnect).
  r.action = (SessionUp(s) || s.disconnectRequested) ? Action::Disconnect : Action::Connect;
  return r;
}

}  // namespace health
}  // namespace urnw
