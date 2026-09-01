// One actionable explanation for each control-socket reachability failure.
// Kept independent of GTK/gettext so every enum value is unit-testable and
// every UI surface translates the same key and English fallback.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ControlClient.hpp"

namespace urnw {

struct DaemonUnreachableCopy {
  const char* key;
  const char* english;
};

inline constexpr DaemonUnreachableCopy CopyForDaemonUnreachableReason(
    DaemonUnreachableReason reason) {
  switch (reason) {
    case DaemonUnreachableReason::StaleSandboxMount:
      return {"daemon_stale_sandbox_mount",
              "The URnetwork system service restarted while this app was open. Close "
              "the app and open it again to reconnect to it."};
    case DaemonUnreachableReason::PermissionDenied:
      return {"daemon_legacy_group_auth",
              "The URnetwork system service on this device is an older version that "
              "still requires group membership. Update the service, or add your user "
              "to the 'urnetwork' group and sign out and back in."};
    case DaemonUnreachableReason::Other:
      return {"daemon_not_reachable",
              "The URnetwork system service could not be reached."};
    case DaemonUnreachableReason::SocketMissing:
    case DaemonUnreachableReason::None:
      return {"daemon_unreachable",
              "The URnetwork system service is not running. Install or start it, then "
              "try again."};
  }
  return {"daemon_not_reachable",
          "The URnetwork system service could not be reached."};
}

}  // namespace urnw
