// The device list's words (ManageDevicesSheet; owner, 2026-11-08: the linux app
// at parity with apple, android and windows). The list is the sdk's
// ManageDevicesViewController (client limit SDK-API.md): every client that takes
// a slot of the network's concurrent client limit, this device first, then the
// connected ones, then by the time last seen, and a removal that frees a slot
// adds this device again (it ends the device's client limit hold, or runs a
// refused sign-in's registration again). Which devices there are, their order,
// what can be removed and what happens after are the sdk's. This header holds
// only how a snapshot reads: each row's title and the line under it, what the
// sheet says about the list's state, and the store's words for it, as the
// windows app's ManageDevicesPresentation.h and the apple app's
// ManageDevicesLabels word them.
//
// Pure, with no GTK or sdk header, so tests/ManageDevicesPresentationTest.cpp
// runs it anywhere; ManageDevicesSheet.cpp copies the sdk's snapshot into the
// plain values below, reading the sdk's own spellings of its kinds and errors.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ProFeaturesNotice.hpp"

namespace urnw {
namespace manage_devices {

// A line the sheet shows: a store key and its English (the gettext msgid).
struct Text {
  const char* key;
  const char* english;
};

inline constexpr Text kTitleText{"manage_devices", "Manage devices"};
inline constexpr Text kHelpText{
    "manage_devices_help",
    "Each connected device or proxy uses a slot of your plan. Remove one you no longer use to "
    "free its slot."};
inline constexpr Text kThisDeviceText{"manage_devices_this_device", "This device"};
inline constexpr Text kProxyText{"manage_devices_proxy", "Hosted proxy"};
inline constexpr Text kUnknownDeviceText{"sessions_device_unknown", "Unknown device"};
inline constexpr Text kConnectedText{"connected", "Connected"};
// {} is how long ago (Formatters.hpp RelativeTime), as the sessions screen says it
inline constexpr Text kLastSeenText{"manage_devices_last_seen", "Last seen {}"};
inline constexpr Text kConnectingText{"manage_devices_connecting", "Connecting this device…"};
inline constexpr Text kRemoveTitleText{"manage_devices_remove_title", "Remove this device?"};
// {} is the row's title
inline constexpr Text kRemoveBodyText{"sessions_confirm_body_no_place", "{} will be signed out."};
inline constexpr Text kSignInRequiredText{"manage_devices_sign_in_required",
                                          "Sign in again to manage devices."};
inline constexpr Text kLoadFailedText{"manage_devices_load_failed", "Couldn't load your devices."};
inline constexpr Text kRemoveFailedText{"manage_devices_remove_failed",
                                        "Couldn't remove the device. Try again."};

// The separator between a line's parts, the sessions screen's.
inline constexpr const char* kSeparator = " · ";

// The snapshot's ErrorId (the sdk's ManageDevicesError*).
enum class Error { None, SignInRequired, LoadFailed, RemoveFailed };

// One ManagedDevice in plain values.
struct Device {
  std::string clientId;
  // Kind is ManagedDeviceKindProxy: a hosted proxy, which takes a slot while
  // connected
  bool proxy = false;
  // the device's name, else its description; may be empty
  std::string name;
  // its system and model; may be empty
  std::string spec;
  bool thisDevice = false;
  bool connected = false;
  // its last sign-in or renewal, unix epoch milliseconds; 0 when unknown
  int64_t lastSeenMillis = 0;
};

// One ManageDevicesSnapshot in plain values.
struct Snapshot {
  std::vector<Device> devices;
  bool loaded = false;
  bool loading = false;
  // the client a removal is in flight for, "" with none
  std::string removingClientId;
  Error error = Error::None;
  // removals that freed a slot and added this device again
  int64_t addRetryCount = 0;
  // the network's Pro features in use, read from the same proxies list
  // (ProFeaturesStatus.Prompt): what the sheet says above the rows
  // (ProFeaturesNotice.hpp)
  pro_features::Prompt proFeatures = pro_features::Prompt::None;
};

// What the platform lends the words: the catalog (T_), the store's {}
// substitution (I18n.hpp FormatArgs), and how long ago a time was (Formatters.hpp
// RelativeTime, against the time the sheet renders at).
struct Words {
  std::function<std::string(const Text&)> lookup;
  std::function<std::string(const std::string& pattern, const std::string& arg)> format;
  std::function<std::string(int64_t unixMillis)> relative;
};

// The row's first line: the device's name, else what it is.
inline std::string TitleOf(const Device& device, const Words& words) {
  if (!device.name.empty()) return device.name;
  return words.lookup(device.proxy ? kProxyText : kUnknownDeviceText);
}

// The line under it: "This device · Ubuntu 24.04 x86_64 · Connected", or
// "Last seen 5m ago". Parts that say nothing are left out.
inline std::string DetailOf(const Device& device, const Words& words) {
  std::vector<std::string> parts;
  if (device.thisDevice) parts.push_back(words.lookup(kThisDeviceText));
  // a proxy with a name says what it is here; without one its title does
  if (device.proxy && !device.name.empty()) parts.push_back(words.lookup(kProxyText));
  if (!device.spec.empty()) parts.push_back(device.spec);
  if (device.connected) {
    parts.push_back(words.lookup(kConnectedText));
  } else if (0 < device.lastSeenMillis) {
    parts.push_back(
        words.format(words.lookup(kLastSeenText), words.relative(device.lastSeenMillis)));
  }
  std::string detail;
  for (const std::string& part : parts) {
    if (!detail.empty()) detail += kSeparator;
    detail += part;
  }
  return detail;
}

// What the sheet shows about the list's state, above the rows.
struct Status {
  // the line under the help: a failure, or "Connecting this device…"; null for
  // none
  const Text* line = nullptr;
  // the line is a failure
  bool error = false;
  // Retry (Refresh) beside it: the list failed to load
  bool offersRetry = false;
  // the first load is in flight: a spinner in the list's place
  bool loading = false;
  // the rows, or none while there is no credential to show them with
  bool showsRows = true;
  // Remove is offered: no removal is in flight (one at a time)
  bool removeEnabled = true;
};

inline Status StatusOf(const Snapshot& snapshot, bool canManage) {
  Status status;
  status.removeEnabled = snapshot.removingClientId.empty();
  if (!canManage || snapshot.error == Error::SignInRequired) {
    // no network credential, or the server refused it: sign in again
    status.line = &kSignInRequiredText;
    status.error = true;
    status.showsRows = false;
    return status;
  }
  if (snapshot.error == Error::LoadFailed) {
    status.line = &kLoadFailedText;
    status.error = true;
    status.offersRetry = true;
  } else if (snapshot.error == Error::RemoveFailed) {
    status.line = &kRemoveFailedText;
    status.error = true;
  } else if (0 < snapshot.addRetryCount) {
    status.line = &kConnectingText;
  }
  status.loading = !snapshot.loaded && snapshot.loading;
  return status;
}

// A row offers Remove: never this device (it signs out instead), and only with
// a client to name.
inline bool Removable(const Device& device) {
  return !device.thisDevice && !device.clientId.empty();
}

}  // namespace manage_devices
}  // namespace urnw
