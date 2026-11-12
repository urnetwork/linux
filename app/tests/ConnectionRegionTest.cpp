// The connection region picker's model, its one stored value shared by the
// login control and the DNS settings row, and the can't-reach prompt's
// classification (src/ConnectionRegion.hpp). Support inbox 7956, 7957, 7973:
// users behind networks that block URnetwork's own names had no way to say
// where they connect from.
// SPDX-License-Identifier: MPL-2.0
#include "ConnectionRegion.hpp"

#include <string>
#include <vector>

#include "TestHarness.hpp"

namespace {

using urnw::region::CantReachPrompt;
using urnw::region::Option;
using urnw::region::Options;
using urnw::region::Picker;
using urnw::region::Store;

// The SDK's NetworkSpace behaviour: a code without a recommendation is stored
// as undisclosed.
class FakeStore : public Store {
 public:
  explicit FakeStore(std::vector<std::string> known) : known_(std::move(known)) {}
  std::string Get() override { return value; }
  std::string Set(const std::string& countryCode) override {
    ++sets;
    value.clear();
    for (const auto& k : known_) {
      if (k == countryCode) value = countryCode;
    }
    return value;
  }
  std::string value;
  int sets = 0;

 private:
  std::vector<std::string> known_;
};

const std::vector<std::string> kSdkCodes = {"cn", "ir", "kz", "ru", "tm", "tr"};

std::string Named(const std::string& code) { return "Country " + code; }

}  // namespace

UR_TEST(ConnectionRegion_OptionsAreExactlyTheSdkRegionsThenUndisclosed) {
  const auto options = Options({"CN", "ir", "", "cn", "kz"}, Named, "Other / Undisclosed");
  const std::vector<Option> expected = {
      {"cn", "Country cn"}, {"ir", "Country ir"}, {"kz", "Country kz"}, {"", "Other / Undisclosed"}};
  UR_EXPECT_TRUE(options == expected);
}

UR_TEST(ConnectionRegion_OptionsFallBackToTheCodeWithoutAName) {
  const auto options = Options({"tm"}, [](const std::string&) { return std::string(); }, "Other");
  UR_EXPECT_EQ(size_t(2), options.size());
  UR_EXPECT_TRUE(options[0].name == "tm");
}

UR_TEST(ConnectionRegion_SelectionIsTheStoredValueAndUndisclosedIsEmpty) {
  FakeStore store(kSdkCodes);
  Picker picker(store, Options(kSdkCodes, Named, "Other / Undisclosed"));
  // nothing stored: Other / Undisclosed, the last row
  UR_EXPECT_EQ(picker.options().size() - 1, picker.SelectedIndex());

  UR_EXPECT_TRUE(picker.Select(3) == "ru");
  UR_EXPECT_TRUE(store.value == "ru");
  UR_EXPECT_EQ(size_t(3), picker.SelectedIndex());

  UR_EXPECT_TRUE(picker.Select(picker.options().size() - 1).empty());
  UR_EXPECT_TRUE(store.value.empty());
  UR_EXPECT_TRUE(picker.Selected().code.empty());
}

UR_TEST(ConnectionRegion_AStoredCodeNoLongerOfferedReadsAsUndisclosed) {
  FakeStore store(kSdkCodes);
  store.value = "zz";
  Picker picker(store, Options(kSdkCodes, Named, "Other"));
  UR_EXPECT_TRUE(picker.Selected().code.empty());
}

UR_TEST(ConnectionRegion_LoginControlAndSettingsShareOneStoredValue) {
  FakeStore store(kSdkCodes);
  const auto options = Options(kSdkCodes, Named, "Other / Undisclosed");
  Picker login(store, options);
  Picker settings(store, options);

  login.Select(0);  // cn at sign-in
  UR_EXPECT_TRUE(settings.Selected().code == "cn");

  settings.Select(1);  // ir in Custom DNS
  UR_EXPECT_TRUE(login.Selected().code == "ir");
  UR_EXPECT_TRUE(store.value == "ir");
}

// Which failures and stalls count as can't-reach is the SDK's one rule for
// every app (sdk cant_reach.go, whose tests hold the cases this file and the
// other apps' tests used to keep in four drifting lists; support inbox 7956,
// 7973). SdkHost::NoteApiError asks urnet::isCantReachError and
// MainWindow::ApplyConnectReading asks urnet::cantReachOnStall; the prompt only
// shows what they found.
UR_TEST(ConnectionRegion_ThePromptShowsWhatTheSdkFoundAndNeverChangesTheRegion) {
  FakeStore store(kSdkCodes);
  store.value = "tr";
  CantReachPrompt prompt;
  UR_EXPECT_FALSE(prompt.visible());
  prompt.Raise();
  UR_EXPECT_TRUE(prompt.visible());
  prompt.Raise();  // a second failure or turn keeps it up
  UR_EXPECT_TRUE(prompt.visible());
  prompt.Dismiss();
  UR_EXPECT_FALSE(prompt.visible());
  UR_EXPECT_EQ(0, store.sets);
  UR_EXPECT_TRUE(store.value == "tr");
}

// Support inbox 8050: a sign-in step past the login page that got no answer
// (the password or seedphrase page) showed the bare failure in its own error
// line, while the prompt went to the login page's line, on a page the user
// could not see. Every page but the login page gets the toast, and every
// signed-out page keeps the login line for a user who steps back to it.
UR_TEST(ConnectionRegion_ThePromptReachesThePageTheUserIsOn) {
  using urnw::region::CantReachPlacement;
  using urnw::region::PlaceCantReachPrompt;
  const CantReachPlacement login = PlaceCantReachPrompt("login");
  UR_EXPECT_TRUE(login.loginLine);
  UR_EXPECT_FALSE(login.toast);
  for (const char* page : {"password", "seedphrase", "instant", "create", "verify", "reset"}) {
    const CantReachPlacement step = PlaceCantReachPrompt(page);
    UR_EXPECT_TRUE_MSG(std::string(page) + " toast", step.toast);
    UR_EXPECT_TRUE_MSG(std::string(page) + " login line", step.loginLine);
  }
  const CantReachPlacement home = PlaceCantReachPrompt("home");
  UR_EXPECT_TRUE(home.toast);
  UR_EXPECT_FALSE(home.loginLine);
}
