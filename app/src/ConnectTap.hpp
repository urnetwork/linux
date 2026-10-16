// A connect or disconnect tap's own state, shown at once and held until the
// command queue settles (apple 8a8d0658). The commands run on SdkHost's queue,
// off the GTK thread, so the reading the window holds can be from before the
// tap for as long as the command is queued or inside its rpc: a connect would
// read Disconnected and a disconnect Connected, and a report from before the
// tap would flip the view back. While a tap is pending the window reads it
// through Fold instead, and once the queue settles the device's own reading
// takes over (SdkHost::SetCommandsSettledHandler).
//
// A start waiting on its balance (MainWindow::CheckBalanceThen) is a pending
// connect too, so its Disconnect shows and ends the wait. Refused, it goes
// back to the reading before the upgrade path opens.
//
// Pure, so the tests read it without GTK or the SDK. Main loop owned.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Health.hpp"

namespace urnw {
namespace connect_tap {

enum class Pending {
  None,
  Connect,     // a connect, or a start waiting on its balance
  Disconnect,  // a disconnect
};

// The signals the window renders while `pending` holds. A connect reads as a
// session coming up with no provider proven yet (the controller's own first
// status, DESTINATION_SET, is Connecting here), whatever the reading says; a
// disconnect reads as the teardown the user asked for. A block on the balance
// outranks a connect, as it outranks any session.
inline health::Signals Fold(health::Signals s, Pending pending) {
  switch (pending) {
    case Pending::None:
      break;
    case Pending::Connect:
      s.disconnectRequested = false;
      s.destinationSelected = true;
      s.tunnelBound = true;
      s.sdk = health::SdkStatus::DestinationSet;
      s.providerCount = 0;
      s.proofLoss = health::ProofLoss::None;
      break;
    case Pending::Disconnect:
      s.disconnectRequested = true;
      break;
  }
  return s;
}

// Whether the tray reads the status as observed (health::TrayReading). With
// the window hidden there is no controller, and an unobserved Connecting
// reads as the session's own claim, Connected. A pending tap is the window's
// own word about where the connection is going, never such a claim: a connect
// just tapped reads connecting, not connected.
inline bool StatusObserved(bool readingObserved, Pending pending) {
  return readingObserved || pending != Pending::None;
}

}  // namespace connect_tap
}  // namespace urnw
