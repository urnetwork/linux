// ONE rendering of the kill switch's real state, shared by the three surfaces
// that carry the toggle (ConnectPage's connect options, ConnectDrawer's
// connection controls, SettingsPage's Connections pane). It exists so those
// three cannot disagree about what the same SdkHost::KillSwitchStatus means —
// on this control a surface that says "on" while another says "not in force"
// is worse than either answer alone.
//
// THE RULE THIS ENCODES (owner decision 1, and the reason the toggles were
// rewired at all): the switch position is the user's REQUEST, and the line
// below is what is actually IN FORCE. Those are different facts and the UI
// must never collapse them — the shipped bug was a toggle that read "on" while
// the daemon had installed nothing at all.
//
// AND THE RULE ADDED HERE: "we cannot tell" is a THIRD fact, distinct from
// both "not in force" and "the service is not running", and it is the state
// the user is in at exactly the moment they are cut off by our own floor. The
// nftables table is not process-bound: it outlives the daemon, so a dead or
// unreachable daemon means the block may still be standing. Copy that answers
// an unknown with "your traffic is not being blocked" — and then blames a
// service that is running — is the opposite of the truth twice over. Every
// state that can leave the user cut off now carries the way out
// (kKillSwitchRecoveryCommand) instead of a diagnosis we did not verify.
//
// `attention` marks the states where the user's protection or their network is
// not what the control implies: they must be rendered in the surface's warning
// treatment, not as ordinary supporting text, and they must never be silently
// swallowed by flipping the switch back.
//
// Store keys: `kill_switch` exists upstream; every `adv_kill_switch_*` and
// `daemon_*` key here is one this port introduces and renders through its
// fallback English until the key lands in the localization store — the same
// arrangement docs/parity/settings.md already documents for
// adv_kill_switch_note / _deliberate / _dns_window. The three shared with
// MainWindow's daemon notice (daemon_too_old, app_too_old_for_daemon,
// daemon_sdk_mismatch) are reproduced BYTE FOR BYTE from MainWindow.cpp: the
// English text is the msgid, so a character of drift is a second catalog
// entry and an untranslated string.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include "DaemonUnreachableCopy.hpp"
#include "I18n.hpp"
#include "SdkHost.hpp"

namespace urnw {

// THE WAY OUT, and the only one that works when this app cannot reach the
// daemon at all. It is the exact string urnetworkd prints for the same purpose
// — NetFilter::RecoveryCommand() (Tunnel.cpp:815) builds
// "sudo nft delete table inet " + kNftTableName, and kNftTableName is
// "urnetwork" (Tunnel.hpp:153) — and Tunnel.hpp:342-345 names "the kill-switch
// UI copy" as one of its three intended sinks.
//
// DUPLICATED, NOT INCLUDED, and that is a defect worth naming: Tunnel.cpp is
// built into urnetworkd only (meson.build:234), so NetFilter::RecoveryCommand
// would not link into the GUI and pulling the daemon's Tunnel.hpp into a GUI
// header would be worse. REQUEST TO THE DAEMON OWNER: hoist kNftTableName (or
// the whole command) into ControlProtocol.hpp, which both targets already
// share, and derive both sides from it. This is the one string in the product
// that must never drift — it is what a user with no network types.
inline constexpr const char* kKillSwitchRecoveryCommand =
    "sudo nft delete table inet urnetwork";

struct KillSwitchCopy {
  std::string line;        // "" = there is nothing to say beyond the row's own note
  bool attention = false;  // the control's promise and the machine's state disagree
};

// WHY the daemon could not be asked — never "it is not running" unless that is
// what the transport actually said. DaemonUnreachableReason exists precisely
// because Unreachable is three different problems with three different fixes,
// and the common one on a fresh install is PermissionDenied: the installers
// create the `urnetwork` group EMPTY, so connect(2) returns EACCES at a socket
// that is right there, served by a daemon that is running perfectly.
inline std::string DaemonReachFailureCause(const KillSwitchStatus& status) {
  switch (status.session) {
    case DaemonSessionState::Unreachable: {
      const auto copy = CopyForDaemonUnreachableReason(status.unreachable_reason);
      return T_(copy.key, copy.english);
    }
    case DaemonSessionState::DaemonTooOld:
      return T_("daemon_too_old",
                "The URnetwork system service is out of date. Update it to connect.");
    case DaemonSessionState::ClientTooOld:
      return T_("app_too_old_for_daemon",
                "This app is older than the installed URnetwork system service. Update "
                "the app to connect.");
    case DaemonSessionState::SdkMismatch:
      return T_("daemon_sdk_mismatch",
                "The app and the URnetwork system service are different builds. Update "
                "both to the same version.");
    case DaemonSessionState::Ok:
    case DaemonSessionState::Error:
      break;
  }
  // Reachable, or reachable-and-then-not: the channel is not the story, the
  // missing answer is.
  return T_("daemon_no_answer", "The URnetwork system service did not answer.");
}

// The trailing sentence for every state where the user may be cut off by our
// own floor. Conditional on purpose ("if this device cannot reach the
// network"): it must be useful to the user who IS blocked without alarming the
// one who is not, and it must never be the app's only claim about a state the
// app did not verify.
inline std::string KillSwitchRecoveryLine() {
  return Format(T_("adv_kill_switch_recovery",
                   "If this device cannot reach the network, a block installed earlier may "
                   "still be in place; remove it with: {}"),
                kKillSwitchRecoveryCommand);
}

inline KillSwitchCopy KillSwitchStateLine(const KillSwitchStatus& status) {
  KillSwitchCopy out;
  switch (ClassifyKillSwitch(status)) {
    case KillSwitchDisplay::Off:
      return out;  // the row's own note already says what the switch does

    case KillSwitchDisplay::Applying:
      out.line = T_("adv_kill_switch_applying", "Applying…");
      return out;

    case KillSwitchDisplay::InForce:
      // Armed and Connected are both "in force", but they block different
      // things and the user is entitled to know which one they are in.
      if (status.installed == ctl::KillSwitchState::Armed) {
        out.line = T_("adv_kill_switch_state_armed",
                      "In force: nothing but local network traffic leaves this device "
                      "until you connect.");
        // Armed is the in-force state a user can mistake for a broken machine:
        // no tunnel, and nothing reaches the internet. The daemon IS reachable
        // here (that is how we know the state), so the cheap way out is the
        // switch itself — the nft command stays for the states where it is
        // not.
        out.line += " ";
        out.line += T_("adv_kill_switch_armed_way_out",
                       "Turn the kill switch off to restore normal access without "
                       "connecting.");
      } else {
        out.line = T_("adv_kill_switch_state_connected",
                      "In force: if the tunnel drops, this device's traffic is blocked "
                      "instead of going out unprotected.");
      }
      return out;

    case KillSwitchDisplay::InForceUnrequested:
      // The switch says off and the machine is blocked anyway: a removal that
      // failed, or a floor re-armed from the crash marker. This used to render
      // as nothing at all, which is the worst possible answer — the user has
      // no network and the one control that explains it reads "off".
      out.attention = true;
      out.line = T_("adv_kill_switch_state_still_installed",
                    "The kill switch is off, but the URnetwork system service still has a "
                    "block installed, so this device's traffic is being blocked.");
      out.line += " ";
      out.line += Format(T_("adv_kill_switch_still_installed_fix",
                            "Turning the kill switch on and then off again asks the "
                            "service to clear it; if that does not work, remove it with: "
                            "{}"),
                         kKillSwitchRecoveryCommand);
      break;

    case KillSwitchDisplay::ArmedAtNextStart:
      // The daemon's DELIBERATE behaviour, not a failure: switching the kill
      // switch on with nothing connected must never cut a machine off the
      // network it has not left yet.
      out.line = T_("adv_kill_switch_state_pending",
                    "Nothing is blocked yet — this takes effect the moment you connect.");
      return out;

    case KillSwitchDisplay::Failed:
      out.attention = true;
      out.line = T_("adv_kill_switch_state_failed",
                    "The URnetwork system service could not install the kill switch, so "
                    "your traffic is NOT being blocked.");
      break;

    case KillSwitchDisplay::Unknown:
      // WE CANNOT TELL. Say that, say why, and hand over the command — because
      // this is the state in which the app is powerless to lift a floor that
      // may be blocking the machine right now, and the old copy answered it
      // with a flat "your traffic is not being blocked" plus a false
      // "the service is not running".
      out.attention = true;
      out.line = T_("adv_kill_switch_state_unknown",
                    "Whether the kill switch is in force cannot be checked right now.");
      out.line += " ";
      out.line += DaemonReachFailureCause(status);
      out.line += " ";
      out.line += KillSwitchRecoveryLine();
      break;

    case KillSwitchDisplay::NotInForce:
      // Reachable, answered, tunnel up, and nothing installed. The ONLY state
      // entitled to assert that traffic is not being blocked.
      out.attention = true;
      out.line = T_("adv_kill_switch_state_not_in_force",
                    "The kill switch is NOT in force: your traffic is not being blocked.");
      break;
  }
  // The daemon's own explanation, appended to whichever attention line was
  // chosen. Never rendered alone — an untranslated diagnostic is not an
  // answer — and never dropped, because it is the only thing that says WHY.
  if (!status.detail.empty()) out.line += " (" + status.detail + ")";
  return out;
}

}  // namespace urnw
