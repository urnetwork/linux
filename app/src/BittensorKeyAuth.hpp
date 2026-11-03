// Device authentication before the Key screen shows or deletes a stored
// Bittensor key (sdk BittensorLocalKeyScreen.NeedsAuthentication and
// ConfirmNeedsAuthentication; apple BittensorDeviceAuthenticator). polkit asks
// for the user's own login password through the desktop's polkit agent, the
// decision BittensorKeyPlatform.hpp writes down: Authorized passes, a dismissed
// or refused dialog doesn't, and a desktop where polkit can't ask passes.
//
// The question is CheckAuthorization on the system bus, over raw GDBus (gio is
// already linked; no libpolkit): the action is
// bittensor_key::kAuthAction, with AllowUserInteraction. The subject is this
// process's own system bus name, which polkit resolves itself, so nothing a
// caller says can name another process; `details` is always empty, as the
// daemon's checks keep it (daemon/ControlServer.cpp, rule 1). The Flatpak
// reaches the name through its manifest's --system-talk-name.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

namespace urnw {

// Main loop. `done` runs once, on the main loop: true when the user
// authenticated, or when polkit can't ask on this desktop. A dialog nobody
// answers gives up after two minutes, as the daemon's do.
void AuthenticateForBittensorKey(std::function<void(bool authenticated)> done);

}  // namespace urnw
