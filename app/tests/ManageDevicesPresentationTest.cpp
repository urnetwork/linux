// The device list's words (ManageDevicesPresentation.hpp): each row's title
// and the line under it, what the sheet says about the list's state, and which
// rows offer Remove, as the windows app's ManageDevicesPresentation.h and the
// apple app's ManageDevicesLabels say them. Which devices there are and what a
// removal does are the sdk's (manage_devices_view_controller_test.go).
//
// SPDX-License-Identifier: MPL-2.0
#include <cstring>
#include <string>

#include "ManageDevicesPresentation.hpp"
#include "TestHarness.hpp"

namespace md = urnw::manage_devices;

namespace {

// The store's English for every key, {} substituted, and a fixed "ago".
md::Words EnglishWords() {
  md::Words words;
  words.lookup = [](const md::Text& text) { return std::string(text.english); };
  words.format = [](const std::string& pattern, const std::string& arg) {
    std::string out = pattern;
    const size_t at = out.find("{}");
    if (at != std::string::npos) out.replace(at, 2, arg);
    return out;
  };
  words.relative = [](int64_t unixMillis) { return std::to_string(unixMillis) + "ms-ago"; };
  return words;
}

bool SameText(const md::Text* a, const md::Text& b) {
  return a != nullptr && std::strcmp(a->key, b.key) == 0;
}

}  // namespace

// ---- a row --------------------------------------------------------------------

// The title is the device's name, else what it is: a hosted proxy, or an
// unknown device.
UR_TEST(ManageDevicesPresentation_TheTitleIsTheNameElseWhatItIs) {
  const md::Words words = EnglishWords();
  md::Device named;
  named.name = "Brien's laptop";
  UR_EXPECT_TRUE(md::TitleOf(named, words) == "Brien's laptop");
  md::Device proxy;
  proxy.proxy = true;
  UR_EXPECT_TRUE(md::TitleOf(proxy, words) == "Hosted proxy");
  md::Device unknown;
  UR_EXPECT_TRUE(md::TitleOf(unknown, words) == "Unknown device");
}

// The line under it, its parts joined by " · ": this device, a named proxy's
// kind, the spec, then connected or when it was last seen; parts that say
// nothing are left out.
UR_TEST(ManageDevicesPresentation_TheDetailSaysWhatTheDeviceIsAndWhenItWasSeen) {
  const md::Words words = EnglishWords();
  md::Device self;
  self.thisDevice = true;
  self.spec = "Ubuntu 24.04 x86_64";
  self.connected = true;
  UR_EXPECT_TRUE(md::DetailOf(self, words) == "This device · Ubuntu 24.04 x86_64 · Connected");

  md::Device seen;
  seen.lastSeenMillis = 42;
  UR_EXPECT_TRUE(md::DetailOf(seen, words) == "Last seen 42ms-ago");

  md::Device namedProxy;
  namedProxy.proxy = true;
  namedProxy.name = "scraper";
  namedProxy.connected = true;
  UR_EXPECT_TRUE(md::DetailOf(namedProxy, words) == "Hosted proxy · Connected");
  // an unnamed proxy's title already says what it is
  md::Device proxy;
  proxy.proxy = true;
  UR_EXPECT_TRUE(md::DetailOf(proxy, words).empty());

  // connected wins over a last-seen time; an unknown time says nothing
  md::Device both;
  both.connected = true;
  both.lastSeenMillis = 42;
  UR_EXPECT_TRUE(md::DetailOf(both, words) == "Connected");
  UR_EXPECT_TRUE(md::DetailOf(md::Device{}, words).empty());
}

// Remove is offered for every other device with a client to name, never this
// one (it signs out instead).
UR_TEST(ManageDevicesPresentation_RemoveIsForAnotherDevice) {
  md::Device other;
  other.clientId = "c1";
  UR_EXPECT_TRUE(md::Removable(other));
  md::Device self = other;
  self.thisDevice = true;
  UR_EXPECT_FALSE(md::Removable(self));
  UR_EXPECT_FALSE(md::Removable(md::Device{}));
}

// ---- the list's state ---------------------------------------------------------

// No credential, or the server refused it: sign in again, and no rows.
UR_TEST(ManageDevicesPresentation_NoCredentialSaysSignInAgain) {
  md::Snapshot snapshot;
  snapshot.loaded = true;
  snapshot.devices.push_back(md::Device{});
  md::Status status = md::StatusOf(snapshot, /*canManage=*/false);
  UR_EXPECT_TRUE(SameText(status.line, md::kSignInRequiredText));
  UR_EXPECT_TRUE(status.error);
  UR_EXPECT_FALSE(status.showsRows);
  snapshot.error = md::Error::SignInRequired;
  status = md::StatusOf(snapshot, /*canManage=*/true);
  UR_EXPECT_TRUE(SameText(status.line, md::kSignInRequiredText));
  UR_EXPECT_FALSE(status.showsRows);
}

// A failed load says so, with Retry; a failed removal says so, and the device
// stays listed; neither hides the rows.
UR_TEST(ManageDevicesPresentation_FailuresSayWhatFailed) {
  md::Snapshot snapshot;
  snapshot.loaded = true;
  snapshot.error = md::Error::LoadFailed;
  md::Status status = md::StatusOf(snapshot, true);
  UR_EXPECT_TRUE(SameText(status.line, md::kLoadFailedText));
  UR_EXPECT_TRUE(status.error && status.offersRetry && status.showsRows);
  snapshot.error = md::Error::RemoveFailed;
  status = md::StatusOf(snapshot, true);
  UR_EXPECT_TRUE(SameText(status.line, md::kRemoveFailedText));
  UR_EXPECT_TRUE(status.error && !status.offersRetry && status.showsRows);
}

// A removal freed a slot and the sdk is adding this device again.
UR_TEST(ManageDevicesPresentation_ARemovalThatFreedASlotSaysConnecting) {
  md::Snapshot snapshot;
  snapshot.loaded = true;
  snapshot.addRetryCount = 1;
  const md::Status status = md::StatusOf(snapshot, true);
  UR_EXPECT_TRUE(SameText(status.line, md::kConnectingText));
  UR_EXPECT_FALSE(status.error);
}

// The spinner only for the first load; a refresh over a loaded list keeps it.
UR_TEST(ManageDevicesPresentation_TheSpinnerIsOnlyForTheFirstLoad) {
  md::Snapshot snapshot;
  snapshot.loading = true;
  UR_EXPECT_TRUE(md::StatusOf(snapshot, true).loading);
  snapshot.loaded = true;
  UR_EXPECT_FALSE(md::StatusOf(snapshot, true).loading);
  UR_EXPECT_TRUE(md::StatusOf(snapshot, true).line == nullptr);
}

// One removal at a time: Remove waits while one is in flight.
UR_TEST(ManageDevicesPresentation_OneRemovalAtATime) {
  md::Snapshot snapshot;
  snapshot.loaded = true;
  UR_EXPECT_TRUE(md::StatusOf(snapshot, true).removeEnabled);
  snapshot.removingClientId = "c1";
  UR_EXPECT_FALSE(md::StatusOf(snapshot, true).removeEnabled);
}

// The words are the store's keys (CatalogLookupTest holds each English to
// po/urnetwork.pot byte for byte, and each key's linux tag).
UR_TEST(ManageDevicesPresentation_TheWordsAreTheStoresKeys) {
  UR_EXPECT_TRUE(std::string(md::kTitleText.key) == "manage_devices");
  UR_EXPECT_TRUE(std::string(md::kHelpText.key) == "manage_devices_help");
  UR_EXPECT_TRUE(std::string(md::kLastSeenText.key) == "manage_devices_last_seen");
  UR_EXPECT_TRUE(std::string(md::kRemoveBodyText.key) == "sessions_confirm_body_no_place");
  UR_EXPECT_TRUE(std::string(md::kUnknownDeviceText.key) == "sessions_device_unknown");
}
