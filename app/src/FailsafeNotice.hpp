// What the app says about the daemon's dead-tunnel failsafe (TunnelWatchdog.hpp):
// the warning under the connect controls while a countdown runs on the live
// session, and the line after the failsafe ended one, in the two versions that
// differ on what matters: with the kill switch on the machine is still blocked,
// with it off it is back on the internet, unprotected. Windows' connect page
// says the same in the same words (ConnectPage::ApplyConnectStatus).
//
// And the tray's two recovery items, Windows' TrayIcon's: the tray is the one
// surface left while the window is closed to it, so it is where a machine the
// daemon still captures, or that the kill switch blocks, can be given back
// from.
//
// GTK-free, so every case is a table test; the surfaces translate each key and
// its English with T_.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include "ControlProtocol.hpp"

namespace urnw::failsafe_notice {

struct Copy {
  const char* key;
  const char* english;
};

// The countdown warning (StatusReply::failsafe_armed). An account out of balance
// is told it is held instead, and gets this only for a hung SDK: the rules about
// traffic hold for it and run no countdown (TunnelWatchdog.hpp OutOfBalanceHold).
inline Copy ArmedCopy() {
  return {"conn_failsafe_armed",
          "If nothing gets through shortly, URnetwork will turn the tunnel off automatically "
          "so you keep your internet."};
}

// The warning shows while the live tunnel's countdown runs, and only then.
inline bool ShowsArmedWarning(bool failsafeArmed, ctl::TunnelState state) {
  return failsafeArmed && state == ctl::TunnelState::Up;
}

// The line after a stop, by what the daemon reports the kill switch is now.
// nullopt for a stop the failsafe did not make, and for a kill switch that
// could not be armed: the daemon's own sentence says what was left behind
// then, and neither version here would be true.
inline std::optional<Copy> StoppedCopy(const std::string& stopReason,
                                       ctl::KillSwitchState killSwitch) {
  if (!ctl::IsFailsafeStop(stopReason)) return std::nullopt;
  switch (killSwitch) {
    case ctl::KillSwitchState::Armed:
      return Copy{"conn_failsafe_blocked",
                  "The tunnel could not carry traffic, so URnetwork shut it down. The kill "
                  "switch is on, so nothing leaves this machine until you connect again or "
                  "turn the kill switch off — nothing is leaking."};
    case ctl::KillSwitchState::Off:
      return Copy{"conn_failsafe_restored",
                  "URnetwork disconnected you to keep you online: the tunnel was up but "
                  "nothing was getting through. Your traffic is going out normally now and is "
                  "NOT protected. Press Connect to try again."};
    case ctl::KillSwitchState::Connected:
    case ctl::KillSwitchState::Failed:
      break;
  }
  return std::nullopt;
}

// The tray's recovery items, each offered only while it is the answer to
// something and the window holds no session, whose own Disconnect is then the
// answer instead.
struct TrayRecovery {
  // "Force the tunnel off (recovery)": stop_tunnel, for a tunnel the daemon
  // still runs or whose capture routes are still in.
  bool forceTunnelOff = false;
  // "Turn off the kill switch (unblock this machine)": the kill switch off,
  // for a floor that blocks this machine with no tunnel up, armed or left
  // behind by an arm that failed.
  bool liftKillSwitch = false;

  bool operator==(const TrayRecovery& other) const {
    return forceTunnelOff == other.forceTunnelOff && liftKillSwitch == other.liftKillSwitch;
  }
  bool operator!=(const TrayRecovery& other) const { return !(*this == other); }

  static Copy ForceTunnelOffCopy() {
    return {"conn_tray_force_tunnel_off", "Force the tunnel off (recovery)"};
  }
  static Copy LiftKillSwitchCopy() {
    return {"conn_tray_lift_kill_switch", "Turn off the kill switch (unblock this machine)"};
  }
};

// The items for the daemon's `status`, read while the window is
// `windowConnected` or not. Another user's session (a redacted status) is
// theirs to end: taking it over is its own action, never a recovery item.
inline TrayRecovery TrayRecoveryFor(bool windowConnected, const ctl::StatusReply& status) {
  TrayRecovery recovery;
  if (windowConnected || status.redacted) return recovery;
  recovery.forceTunnelOff =
      status.tunnel_state == ctl::TunnelState::Up || status.routes_installed;
  recovery.liftKillSwitch = status.tunnel_state != ctl::TunnelState::Up &&
                            (status.kill_switch == ctl::KillSwitchState::Armed ||
                             status.kill_switch == ctl::KillSwitchState::Failed);
  return recovery;
}

}  // namespace urnw::failsafe_notice
