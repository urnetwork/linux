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
#include <cstdint>
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
  Degraded,       // was Connected this session, and has not been for kDegradeHoldMillis
  Failed,         // the provider window settled on failure
  Disconnecting,  // a teardown THE USER ASKED FOR is in flight
  Blocked,        // out of balance (overrides the connection reading)
};

// The hero pose. Mirrors ConnectCanvas::State one-for-one; a separate enum so
// this header stays free of GTK and the canvas cannot be written from here.
enum class Hero { Disconnected, Connecting, Connected, Error, Processing };

// The status dot. A token, not a hex: the page owns the palette (§8.1).
enum class Dot { Idle, Connecting, Green, Coral, Amber };

// The one action the hero and the button share. Retry is the Failed state's:
// it disconnects and connects to the same selection again (Windows 70d682a).
enum class Action { Connect, Disconnect, Retry };

// Proof this session had and no longer has (DegradeHold, below): Held while
// the hold runs, Lost after it.
enum class ProofLoss { None, Held, Lost };

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
  // The refusal is a paying account's data limit, not a lack of balance
  // (balance_notice::DataLimitGate): the same state, in words for the limit.
  bool dataLimit = false;
  // The user pressed Disconnect and the teardown is still in flight. Bounded
  // by the caller (ConnectPage::DisconnectIntentLive: it clears the moment the
  // session actually reads down, and in any case after 8s).
  bool disconnectRequested = false;
  // This session was Connected and the controller says otherwise now: the
  // hold's verdict, stamped by SdkHost (DegradeHold).
  ProofLoss proofLoss = ProofLoss::None;
};

// THE ONE PREDICATE. "There is something to disconnect from." It selects the
// button's word, the hero's action and the settled-idle row, so those cannot
// be answers to three different questions.
inline bool SessionUp(const Signals& s) { return s.destinationSelected && s.tunnelBound; }

// The session is driving its selection: it is up and its provider window has
// not settled on failure, so a row click on that selection is no new connect.
// A failed window is driven nowhere, and a click on its row connects again, as
// Windows' RowClickIsCurrent counts only CONNECTED, CONNECTING and
// DESTINATION_SET.
inline bool DrivesSelection(const Signals& s) {
  return SessionUp(s) && s.sdk != SdkStatus::Failed;
}

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
  // It said so earlier in this session: a blip stays Connected for the hold,
  // and a loss past it is Degraded, which outranks a window that has settled
  // on failure since (a session that was working is not one that never did).
  if (s.proofLoss == ProofLoss::Held) return State::Connected;
  if (s.proofLoss == ProofLoss::Lost) return State::Degraded;
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
    case State::Degraded:
      // Windows' wording: the SDK reconnects on its own, so the line says so
      // rather than asking for a press the recovery does not need
      r.textKey = "conn_degraded";
      r.textEnglish = "Connection degraded — reconnecting";
      r.dot = Dot::Coral;
      r.hero = Hero::Connecting;
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
      if (s.dataLimit) {
        r.textKey = "data_limit_reached";
        r.textEnglish = "Data limit reached";
      } else {
        r.textKey = "insufficient_balance_add_balance_or_plan";
        r.textEnglish = "Insufficient balance — add balance or a plan";
      }
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
  // A failure whose one control is Disconnect leaves the user a step from the
  // retry that usually works, so the failed state offers that instead. It is
  // still a session to stop: a Retry press stops it first.
  if (r.state == State::Failed) r.action = Action::Retry;
  return r;
}

// The plan's say over a refusal, which the reading cannot know. A paying account
// is not told to add balance: its refusal is a data limit, or one from before the
// purchase that has not cleared yet and says nothing (DataLimitGate), and then the
// connection reading stands.
inline Signals WithPlan(Signals s, bool pro, bool dataLimit) {
  s.dataLimit = dataLimit;
  if (pro && !dataLimit) s.insufficientBalance = false;
  return s;
}

// The line under the status that says why a connect is not there yet, from
// the SDK's diagnosis of the forming window (WindowStatus.StallReason:
// evaluating, platform-unreachable, providers-unresponsive, rate-limited,
// auth-failing). Only while the attempt is building, held, degraded or
// failed; "evaluating", an unknown reason and the empty one of an SDK that
// predates the field say nothing the headline does not. A failure with no
// reason says what Retry does. A null key is no line.
struct ReasonLine {
  const char* key = nullptr;
  const char* english = nullptr;
};

inline ReasonLine ReasonLineFor(State state, const std::string& stallReason) {
  const bool stalled = state == State::Connecting || state == State::Evaluating ||
                       state == State::Degraded || state == State::Failed;
  if (stalled) {
    if (stallReason == "platform-unreachable") {
      return {"conn_reason_platform", "Contacting the platform…"};
    }
    if (stallReason == "providers-unresponsive") {
      return {"conn_reason_providers", "Providers not responding — retrying…"};
    }
    if (stallReason == "rate-limited") {
      return {"conn_reason_rate_limited", "Rate limited — waiting…"};
    }
    if (stallReason == "auth-failing") {
      return {"conn_reason_auth", "Signing in to the platform is failing…"};
    }
  }
  if (state == State::Failed) {
    return {"conn_failed_detail",
            "No providers could be reached. Retry rebuilds the connection from scratch."};
  }
  return {};
}

// A provider is proven to carry the session: the tray's connected icon, as
// Windows' tray has it, where it used to mean only that a session was up.
inline bool Proven(const Reading& r) { return r.state == State::Connected; }

// The tray's reading of the window's one. The controller is open only while
// the window presents, so a session started while it was hidden has no status
// at all, and reads Connecting for as long as it stays hidden. With no
// evidence the session's own claim stands, Connected in icon and words, as
// Windows' tray has it. A status that was observed is never upgraded, and a
// teardown, a block or no session reads as the window's.
inline Reading TrayReading(const Reading& r, const Signals& s, bool statusObserved) {
  if (statusObserved || r.state != State::Connecting) return r;
  Signals claimed = s;
  claimed.sdk = SdkStatus::Connected;
  return Render(claimed);
}

// Nothing proven carries the session's traffic: it is routed into the tunnel
// and held there. True for Evaluating, Degraded and Failed over a session.
inline bool TrafficHeld(const Reading& r, const Signals& s) {
  return SessionUp(s) && (r.state == State::Evaluating || r.state == State::Degraded ||
                          r.state == State::Failed);
}

// The line under the status row while traffic is held, as Windows words it.
// "Blocked, not exposed" is true only while the kill switch's floor is in
// force beside the tunnel (ctl::KillSwitchState::Connected); otherwise, or
// with no answer from the daemon, traffic that does not follow the capture
// routes can still leave, and the line says so.
struct HeldLine {
  const char* key;
  const char* english;
};

inline HeldLine HeldLineFor(bool floorInForce) {
  if (floorInForce) {
    return {"conn_traffic_blocked",
            "No working provider right now — your traffic is blocked, not exposed. Disconnect "
            "to go back to your normal connection."};
  }
  return {"conn_traffic_blocked_unprotected",
          "No working provider right now — traffic sent into the tunnel is going nowhere, and "
          "leak protection is off, so some traffic may bypass it. Disconnect to go back to "
          "your normal connection."};
}

// The degrade hold, as Windows' Tracker has it, over this app's proof (the
// controller's CONNECTED, its MinSatisfied). Once a session has been
// Connected, a controller that stops saying so is held at Connected for
// kDegradeHoldMillis, because a provider migration or a probe cycle takes it
// away for a second or two routinely and a headline that flickers on every
// one is noise; past the hold the session is Degraded. Recovery is immediate
// and one-sided, and a session that goes down, or a deliberate connect or
// disconnect (NoteNewAttempt), starts over: a new location's window building
// up is Connecting, not a loss.
//
// The clock is injected (monotonic milliseconds), so the hold is tested
// without one. Not thread safe: SdkHost holds it under its own lock.
class DegradeHold {
 public:
  static constexpr int64_t kDegradeHoldMillis = 7000;

  // `sessionUp` and `connected` (the controller says CONNECTED) of one reading.
  ProofLoss Update(bool sessionUp, bool connected, int64_t nowMillis) {
    reevalAtMillis_ = 0;
    if (!sessionUp) {
      NoteNewAttempt();
      return ProofLoss::None;
    }
    if (connected) {
      proven_ = true;
      lostAtMillis_ = -1;
      return ProofLoss::None;
    }
    if (!proven_) return ProofLoss::None;
    if (lostAtMillis_ < 0) lostAtMillis_ = nowMillis;
    if (nowMillis - lostAtMillis_ < kDegradeHoldMillis) {
      reevalAtMillis_ = lostAtMillis_ + kDegradeHoldMillis;
      return ProofLoss::Held;
    }
    return ProofLoss::Lost;
  }

  // When the clock, not a new reading, ends a running hold; 0 with none.
  // Readings stop arriving exactly when everything is stuck, so the holder
  // has to read again then.
  int64_t ReevalAtMillis() const { return reevalAtMillis_; }

  void NoteNewAttempt() {
    proven_ = false;
    lostAtMillis_ = -1;
    reevalAtMillis_ = 0;
  }

 private:
  bool proven_ = false;
  int64_t lostAtMillis_ = -1;
  int64_t reevalAtMillis_ = 0;
};

}  // namespace health
}  // namespace urnw
