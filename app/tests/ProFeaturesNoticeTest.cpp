// The Pro features notice's words (ProFeaturesNotice.hpp). The decisions are
// the sdk's (the prompt, and when the Connect page's notice shows, tested in
// sdk pro_features_status_test.go and pro_features_view_controller_test.go);
// what is pinned here is what the GUI adds to them: the store's words for each
// prompt, nothing for none, and where the device list offers Get Pro.
//
// SPDX-License-Identifier: MPL-2.0
#include <cstring>
#include <string>

#include "ProFeaturesNotice.hpp"
#include "TestHarness.hpp"

namespace pf = urnw::pro_features;

namespace {

bool SameText(const pf::Text& a, const pf::Text& b) {
  return std::strcmp(a.key, b.key) == 0 && std::strcmp(a.english, b.english) == 0;
}

}  // namespace

// Before enforcement: "Pro features in use", and an upgrade keeps SOCKS and
// WireGuard working.
UR_TEST(ProFeaturesNotice_TheUpgradePromptSaysTheyArePartOfPro) {
  const pf::Notice notice = pf::NoticeFor(pf::Prompt::Upgrade);
  UR_EXPECT_TRUE(notice.shows);
  UR_EXPECT_TRUE(SameText(notice.title, pf::kInUseTitle));
  UR_EXPECT_TRUE(SameText(notice.body, pf::kUpgradeBody));
}

// After enforcement: what stopped, that HTTP and HTTPS still work, and that an
// upgrade turns SOCKS and WireGuard back on.
UR_TEST(ProFeaturesNotice_TheStoppedPromptSaysWhatStopped) {
  const pf::Notice notice = pf::NoticeFor(pf::Prompt::Stopped);
  UR_EXPECT_TRUE(notice.shows);
  UR_EXPECT_TRUE(SameText(notice.title, pf::kStoppedTitle));
  UR_EXPECT_TRUE(SameText(notice.body, pf::kStoppedBody));
}

// No prompt (a Pro network, no SOCKS or WireGuard proxy, a closed notice):
// nothing shows.
UR_TEST(ProFeaturesNotice_NoPromptShowsNothing) {
  UR_EXPECT_FALSE(pf::NoticeFor(pf::Prompt::None).shows);
  static_assert(!pf::NoticeFor(pf::Prompt::None).shows);
  static_assert(pf::NoticeFor(pf::Prompt::Stopped).shows);
}

// The two prompts never share a line: the stopped notice never reads as the
// upgrade one, nor the other way round.
UR_TEST(ProFeaturesNotice_ThePromptsShareNoLine) {
  const pf::Notice upgrade = pf::NoticeFor(pf::Prompt::Upgrade);
  const pf::Notice stopped = pf::NoticeFor(pf::Prompt::Stopped);
  UR_EXPECT_FALSE(SameText(upgrade.title, stopped.title));
  UR_EXPECT_FALSE(SameText(upgrade.body, stopped.body));
}

// The device list offers Get Pro beside its prompt, except in the list of a
// sign-in the client limit refused, where no account is signed in to buy for.
UR_TEST(ProFeaturesNotice_TheDeviceListOffersGetProWhenSignedIn) {
  UR_EXPECT_TRUE(pf::ListOffersUpgrade(/*forSignIn=*/false));
  UR_EXPECT_FALSE(pf::ListOffersUpgrade(/*forSignIn=*/true));
}

// The words are the store's four keys (CatalogLookupTest holds the English to
// po/urnetwork.pot byte for byte, and each key's linux tag).
UR_TEST(ProFeaturesNotice_TheWordsAreTheStoresKeys) {
  UR_EXPECT_TRUE(std::string(pf::kInUseTitle.key) == "pro_features_in_use");
  UR_EXPECT_TRUE(std::string(pf::kUpgradeBody.key) == "pro_features_upgrade_message");
  UR_EXPECT_TRUE(std::string(pf::kStoppedTitle.key) == "pro_features_stopped");
  UR_EXPECT_TRUE(std::string(pf::kStoppedBody.key) == "pro_features_stopped_message");
}
