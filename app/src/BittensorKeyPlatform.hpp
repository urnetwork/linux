// What only Linux decides for the one-tap Bittensor key (apple BTEASY.md,
// "Porting"; the SDK's BittensorLocalKeyFlow decides everything else): the
// store's answer when the Secret Service fails, what polkit's answer means for
// device authentication, and where the store's non-secret notes live.
//
// THE STORE. The phrase lives in the desktop's Secret Service (libsecret), one
// item per network id. A Secret Service that answers with an error is either
// not there at all or can't answer now. Not there (no provider on the session
// bus, or, in a Flatpak, no secret portal): the store answers the SDK's
// BittensorLocalKeyStoreAbsent, and the flow offers no key on this device,
// only the wallet the user already has, so the funds disclaimer's first
// sentence ("stored locally in the app secure storage") never shows untrue.
// Anything else: BittensorLocalKeyStoreUnavailable, which the card reads as a
// store that can't answer now (Retry). Never a plain file.
//
// DEVICE AUTHENTICATION. Reveal and Delete of a stored key ask polkit for the
// action com.bringyour.network.bittensor-key (packaging/polkit), whose default
// is auth_self: the desktop's own polkit agent asks for the user's own login
// password, every time (no _keep). That is the same dialog on every desktop
// that runs an agent: GNOME, KDE Plasma, Cinnamon, MATE, Xfce, LXQt, Budgie,
// Pantheon and COSMIC. Where polkit can't ask at all (no polkitd, no agent in
// the session, a sandbox without the system bus name, or a daemon package from
// before the action), the check counts as passed, as Apple counts a device
// with no passcode: there is nothing to ask with, and the owner must still be
// able to save the phrase. A dismissed or refused dialog never passes. The
// Secret Service item itself is the user's: any program of theirs can read
// it, so this check guards the app's own screen, as on every platform.
//
// Header-only and free of GTK, GLib and the SDK
// (tests/BittensorKeyPlatformTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "PrefsFile.hpp"

namespace urnw::bittensor_key {

// ---- the store ---------------------------------------------------------------

// What the store answers for a Secret Service call that failed.
enum class StoreFailure {
  Absent,       // no secure storage on this device: the SDK's BittensorLocalKeyStoreAbsent
  Unavailable,  // it can't answer now: BittensorLocalKeyStoreUnavailable
};

// `remoteError`: the D-Bus error name the failure carries (GLib's
// g_dbus_error_get_remote_error), "" for a failure that never reached the bus.
// Only a bus with nobody to answer for org.freedesktop.secrets (or, sandboxed,
// for the secret portal) means there is no Secret Service; a timeout, a locked
// collection or a dismissed prompt is one that can't answer now.
constexpr StoreFailure StoreFailureFor(std::string_view remoteError) noexcept {
  if (remoteError == "org.freedesktop.DBus.Error.ServiceUnknown" ||
      remoteError == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
      remoteError == "org.freedesktop.DBus.Error.UnknownMethod" ||
      remoteError == "org.freedesktop.DBus.Error.UnknownInterface" ||
      remoteError == "org.freedesktop.DBus.Error.UnknownObject") {
    return StoreFailure::Absent;
  }
  return StoreFailure::Unavailable;
}

// The store's non-secret notes (the SDK's ReadNote and WriteNote): addresses
// and numbers the flow keeps by network id, never a phrase. One JSON object of
// objects, {"<network id>": {"<name>": "<value>"}}, in its own file beside
// app_prefs.json ($XDG_CONFIG_HOME/urnetwork): outside the SDK's local state,
// so a sign-out leaves it, as it leaves the Secret Service item. Written whole
// through a synced temporary file renamed over it (PrefsFile.hpp), mode 0600.
inline constexpr const char* kNotesFileName = "bittensor_key_notes.json";

// The note, or "" for none (a missing or malformed file reads as no notes).
inline std::string ReadNoteAt(const std::string& path, const std::string& networkId,
                              const std::string& name) {
  if (networkId.empty() || name.empty()) return {};
  const nlohmann::json all = prefs::ReadAllAt(path);
  const auto network = all.find(networkId);
  if (network == all.end() || !network->is_object()) return {};
  const auto value = network->find(name);
  if (value == network->end() || !value->is_string()) return {};
  return value->get<std::string>();
}

// Writes the note; "" removes it, and a network left with no note goes too.
// 0, or the errno of the write that failed (the file is then as it was).
inline int WriteNoteAt(const std::string& path, const std::string& networkId,
                       const std::string& name, const std::string& value) {
  if (networkId.empty() || name.empty()) return 0;
  std::scoped_lock lock(prefs::WriteLock());
  nlohmann::json all = prefs::ReadAllAt(path);
  const nlohmann::json before = all;
  auto network = all.find(networkId);
  if (value.empty()) {
    if (network == all.end() || !network->is_object()) return 0;
    network->erase(name);
    if (network->empty()) all.erase(networkId);
  } else {
    if (network == all.end() || !network->is_object()) all[networkId] = nlohmann::json::object();
    all[networkId][name] = value;
  }
  if (all == before) return 0;
  return prefs::ReplaceFile(path, all.dump(2) + "\n");
}

// ---- device authentication -----------------------------------------------------

// The polkit action that guards Reveal and Delete of a stored key.
inline constexpr const char* kAuthAction = "com.bringyour.network.bittensor-key";

enum class AuthOutcome {
  Authorized,  // the user typed their password
  Refused,     // dismissed, wrong, timed out, or refused by the site's policy
  CannotAsk,   // polkit can't ask on this desktop: counts as authenticated
};

// polkit's answer to CheckAuthorization with AllowUserInteraction: authorized,
// or a challenge left standing, which with interaction allowed means no agent
// could ask (pkexec's "No authentication agent found"), or a refusal.
constexpr AuthOutcome AuthOutcomeFor(bool authorized, bool challenge) noexcept {
  if (authorized) return AuthOutcome::Authorized;
  return challenge ? AuthOutcome::CannotAsk : AuthOutcome::Refused;
}

// With no system bus to ask on, polkit can't ask.
constexpr AuthOutcome AuthOutcomeWithoutBus() noexcept { return AuthOutcome::CannotAsk; }

// A CheckAuthorization that failed: `remoteError` is the D-Bus error it
// carries, "" for a failure on this side of the bus (a timeout, a cancelled
// check). No polkitd, a sandbox without the name, or an action this polkit
// doesn't know (a daemon package from before it): polkit can't ask. Anything
// else, a timeout or a cancelled check included, is no authentication.
inline AuthOutcome AuthOutcomeForError(std::string_view remoteError, std::string_view message) {
  if (remoteError == "org.freedesktop.DBus.Error.ServiceUnknown" ||
      remoteError == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
      remoteError == "org.freedesktop.DBus.Error.AccessDenied" ||
      remoteError == "org.freedesktop.DBus.Error.UnknownMethod" ||
      remoteError == "org.freedesktop.DBus.Error.UnknownInterface" ||
      remoteError == "org.freedesktop.DBus.Error.UnknownObject") {
    return AuthOutcome::CannotAsk;
  }
  if (remoteError == "org.freedesktop.PolicyKit1.Error.Failed" &&
      message.find("is not registered") != std::string_view::npos) {
    return AuthOutcome::CannotAsk;
  }
  return AuthOutcome::Refused;
}

constexpr bool CountsAsAuthenticated(AuthOutcome outcome) noexcept {
  return outcome != AuthOutcome::Refused;
}

}  // namespace urnw::bittensor_key
