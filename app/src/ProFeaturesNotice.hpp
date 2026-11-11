// The Pro features notice, as the GUI shows it (owner decision 2026-11-11:
// SOCKS and WireGuard hosted proxies are Pro features, pro.yml). A network
// without Pro whose hosted proxies hold SOCKS or WireGuard is told so on the
// Connect page, and above the device list (ManageDevicesSheet), which lists the
// network's proxies: while the server's enforce_features switch is off, that
// they are part of Pro and an upgrade keeps them working; once it is on, that
// they stopped working and an upgrade turns them back on.
//
// The decisions are the sdk's, the same on every platform
// (pro_features_status.go, pro_features_view_controller.go): the prompt
// (ProFeaturesStatus.Prompt, which the device list's snapshot carries as
// ProFeatures), and when the Connect page's notice shows
// (ProFeaturesSnapshot.MainPrompt: it alerts once per sign-in, the user's close
// holds while the prompt stays the same, and a new prompt shows again).
// MainWindow holds the sdk's ProFeaturesViewController, one per sign-in. This
// holds only what the GUI adds: the words for each prompt, and where Get Pro
// goes beside them. Nothing here disconnects or holds a connect back, and no
// desktop notification says it.
//
// Pure and dependency-free (the sdk's prompt arrives as the enum below, read by
// MainWindow and ManageDevicesSheet, which have the sdk), so
// tests/ProFeaturesNoticeTest.cpp runs it without GTK or the SDK.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {
namespace pro_features {

// A line the GUI shows: a store key and its English (the gettext msgid).
struct Text {
  const char* key;
  const char* english;
};

// Before enforcement (the sdk's ProFeaturesPromptUpgrade).
inline constexpr Text kInUseTitle{"pro_features_in_use", "Pro features in use"};
inline constexpr Text kUpgradeBody{
    "pro_features_upgrade_message",
    "SOCKS and WireGuard proxies are part of Pro. Upgrade to keep using them."};
// After enforcement (ProFeaturesPromptStopped): HTTP and HTTPS still work.
inline constexpr Text kStoppedTitle{"pro_features_stopped", "SOCKS and WireGuard stopped working"};
inline constexpr Text kStoppedBody{
    "pro_features_stopped_message",
    "SOCKS and WireGuard are part of Pro, so they no longer work on your proxies. HTTP and HTTPS "
    "still work. Upgrade to turn them back on. WireGuard can take up to 30 minutes to reconnect."};

// The sdk's prompt: ProFeaturesPromptUpgrade, ProFeaturesPromptStopped, or none
// (a Pro network, no SOCKS or WireGuard proxy, a server that does not report
// the plan, a list not loaded yet, a daemon or sdk too old to say, or a
// Connect page notice the user closed).
enum class Prompt { None, Upgrade, Stopped };

// What a surface shows for a prompt.
struct Notice {
  // a prompt to say: the title and the body below, with Get Pro beside them
  bool shows = false;
  Text title = kInUseTitle;
  Text body = kUpgradeBody;
};

inline constexpr Notice NoticeFor(Prompt prompt) {
  Notice notice;
  switch (prompt) {
    case Prompt::Upgrade:
      notice.shows = true;
      notice.title = kInUseTitle;
      notice.body = kUpgradeBody;
      break;
    case Prompt::Stopped:
      notice.shows = true;
      notice.title = kStoppedTitle;
      notice.body = kStoppedBody;
      break;
    case Prompt::None:
      break;
  }
  return notice;
}

// The device list says its prompt above the rows with Get Pro beside it, and
// no close: closing is the Connect page notice's alone. The list of a sign-in
// the client limit refused has no account signed in to buy for, so it offers
// no Get Pro, as that sign-in's refusal offers none
// (MainWindow::OfferSignInManageDevices).
inline constexpr bool ListOffersUpgrade(bool forSignIn) { return !forSignIn; }

}  // namespace pro_features
}  // namespace urnw
