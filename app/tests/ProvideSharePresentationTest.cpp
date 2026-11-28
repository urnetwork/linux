// The owner's sharing limits on the connect page (ProvideSharePresentation.hpp,
// feature 10): the two pickers' options (No limit first, the list's choices in
// bytes, a value the list does not have as an option of its own in its place
// by size, selected), and the line under them (the budget reached, the month's
// data of the limit, the month's data). The page needs GTK and the SDK, so the
// last cases read its sources and SdkHost's: the section sits right after the
// provide group, the status listener rides the presentation and its event
// redraws the line, the limits are re-read wherever the extender row's status
// is, and nothing is written while the section is hidden.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "ProvideSharePresentation.hpp"
#include "WiringSource.hpp"

using urnw::provide_share::kBytesPerGib;
using urnw::provide_share::kBytesPerSecondPerMbps;
using urnw::provide_share::Line;
using urnw::provide_share::LineFor;
using urnw::provide_share::MonthlyPickerFor;
using urnw::provide_share::Option;
using urnw::provide_share::Picker;
using urnw::provide_share::SpeedPickerFor;
using urnw::provide_share::Words;
using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;
using urnw::testing::wiring::ReadRaw;

namespace {

// Tagged stand-ins for the store's words and the byte formatter.
Words TaggedWords() {
  Words words;
  words.noLimit = "<no limit>";
  words.mbps = [](int64_t mbps) { return "<" + std::to_string(mbps) + " Mbps>"; };
  words.bytes = [](int64_t byteCount) { return "<" + std::to_string(byteCount) + " B>"; };
  return words;
}

std::vector<int64_t> ValuesOf(const Picker& picker) {
  std::vector<int64_t> values;
  for (const Option& option : picker.options) values.push_back(option.value);
  return values;
}

std::vector<int64_t> Mbps(std::vector<int64_t> mbps) {
  for (int64_t& value : mbps) value *= kBytesPerSecondPerMbps;
  return mbps;
}

std::vector<int64_t> Gib(std::vector<int64_t> gib) {
  for (int64_t& value : gib) value *= kBytesPerGib;
  return gib;
}

}  // namespace

UR_TEST(ProvideShareSpeedPickerListsNoLimitThenTheMbpsChoices) {
  const Picker picker = SpeedPickerFor(0, TaggedWords());
  UR_EXPECT_TRUE(ValuesOf(picker) == Mbps({0, 1, 2, 5, 10, 20, 50, 100}));
  UR_EXPECT_EQ(0u, picker.selected);
  UR_EXPECT_TRUE(picker.options[0].label == "<no limit>");
  UR_EXPECT_TRUE(picker.options[1].label == "<1 Mbps>");
  UR_EXPECT_TRUE(picker.options[7].label == "<100 Mbps>");
  // a megabit a second is 125,000 bytes a second
  UR_EXPECT_TRUE(picker.options[3].value == 625000);
}

UR_TEST(ProvideShareSpeedPickerSelectsTheDevicesSpeed) {
  const Picker picker = SpeedPickerFor(10 * kBytesPerSecondPerMbps, TaggedWords());
  UR_EXPECT_EQ(8u, picker.options.size());
  UR_EXPECT_EQ(4u, picker.selected);
  UR_EXPECT_TRUE(picker.options[picker.selected].label == "<10 Mbps>");
}

UR_TEST(ProvideShareSpeedPickerShowsASpeedTheListLacksInItsPlace) {
  // 3 Mbps, as the CLI may set: between 2 and 5, selected
  const Picker three = SpeedPickerFor(3 * kBytesPerSecondPerMbps, TaggedWords());
  UR_EXPECT_TRUE(ValuesOf(three) == Mbps({0, 1, 2, 3, 5, 10, 20, 50, 100}));
  UR_EXPECT_EQ(3u, three.selected);
  UR_EXPECT_TRUE(three.options[3].label == "<3 Mbps>");
  // a speed that is no whole Mbps is labelled rounded, and keeps its value
  const Picker odd = SpeedPickerFor(1100000, TaggedWords());
  UR_EXPECT_EQ(9u, odd.options.size());
  UR_EXPECT_EQ(4u, odd.selected);
  UR_EXPECT_TRUE(odd.options[4].value == 1100000);
  UR_EXPECT_TRUE(odd.options[4].label == "<9 Mbps>");
  // past the largest choice: last
  const Picker fast = SpeedPickerFor(250 * kBytesPerSecondPerMbps, TaggedWords());
  UR_EXPECT_EQ(9u, fast.options.size());
  UR_EXPECT_EQ(8u, fast.selected);
  UR_EXPECT_TRUE(fast.options[8].label == "<250 Mbps>");
}

UR_TEST(ProvideShareNegativeLimitsReadAsNoLimit) {
  const Picker speed = SpeedPickerFor(-1, TaggedWords());
  UR_EXPECT_TRUE(ValuesOf(speed) == Mbps({0, 1, 2, 5, 10, 20, 50, 100}));
  UR_EXPECT_EQ(0u, speed.selected);
  const Picker monthly = MonthlyPickerFor(-5, TaggedWords());
  UR_EXPECT_EQ(9u, monthly.options.size());
  UR_EXPECT_EQ(0u, monthly.selected);
}

UR_TEST(ProvideShareMonthlyPickerListsNoLimitThenTheGibChoices) {
  const Picker picker = MonthlyPickerFor(0, TaggedWords());
  UR_EXPECT_TRUE(ValuesOf(picker) == Gib({0, 5, 10, 25, 50, 100, 250, 500, 1024}));
  UR_EXPECT_EQ(0u, picker.selected);
  UR_EXPECT_TRUE(picker.options[0].label == "<no limit>");
  // labelled by the app's byte formatter, from the bytes
  UR_EXPECT_TRUE(picker.options[1].label == "<" + std::to_string(5 * kBytesPerGib) + " B>");
  UR_EXPECT_TRUE(picker.options[8].value == 1024LL * 1024 * 1024 * 1024);
  const Picker tib = MonthlyPickerFor(1024 * kBytesPerGib, TaggedWords());
  UR_EXPECT_EQ(9u, tib.options.size());
  UR_EXPECT_EQ(8u, tib.selected);
}

UR_TEST(ProvideShareMonthlyPickerShowsAValueTheListLacksInItsPlace) {
  const Picker small = MonthlyPickerFor(3 * kBytesPerGib, TaggedWords());
  UR_EXPECT_TRUE(ValuesOf(small) == Gib({0, 3, 5, 10, 25, 50, 100, 250, 500, 1024}));
  UR_EXPECT_EQ(1u, small.selected);
  UR_EXPECT_TRUE(small.options[1].label == "<" + std::to_string(3 * kBytesPerGib) + " B>");
  const Picker large = MonthlyPickerFor(2048 * kBytesPerGib, TaggedWords());
  UR_EXPECT_EQ(10u, large.options.size());
  UR_EXPECT_EQ(9u, large.selected);
}

UR_TEST(ProvideShareLineFollowsTheStatus) {
  UR_EXPECT_TRUE(LineFor(false, false, 0) == Line::Hidden);
  UR_EXPECT_TRUE(LineFor(false, true, 50 * kBytesPerGib) == Line::Hidden);
  // the budget reached says when sharing resumes, whatever the data
  UR_EXPECT_TRUE(LineFor(true, true, 50 * kBytesPerGib) == Line::LimitReached);
  UR_EXPECT_TRUE(LineFor(true, false, 50 * kBytesPerGib) == Line::UsedOfLimit);
  UR_EXPECT_TRUE(LineFor(true, false, 0) == Line::Used);
}

// ---- the wiring -------------------------------------------------------------------

UR_TEST(ProvideShareSectionFollowsTheProvideGroup) {
  const std::string page = ReadCode("ConnectPage.cpp");
  const std::string build = FunctionBody(page, "void ConnectPage::BuildPaneA()");
  UR_EXPECT_TRUE(!build.empty());
  // after the provide group's last row, before the connect options
  UR_EXPECT_TRUE(Before(build, "moreOptionsHost_->append(*portMappingRow_);",
                        "moreOptionsHost_->append(*shareLimitsHeader_.root);"));
  UR_EXPECT_TRUE(Before(build, "moreOptionsHost_->append(*shareLimitsHeader_.root);",
                        "T_(\"provide_share_max_speed\", \"Max sharing speed\")"));
  UR_EXPECT_TRUE(Before(build, "T_(\"provide_share_max_speed\", \"Max sharing speed\")",
                        "T_(\"provide_share_monthly_limit\", \"Monthly data limit\")"));
  UR_EXPECT_TRUE(Before(build, "T_(\"provide_share_monthly_limit\", \"Monthly data limit\")",
                        "moreOptionsHost_->append(*shareStatusText_);"));
  UR_EXPECT_TRUE(Before(build, "moreOptionsHost_->append(*shareStatusText_);",
                        "moreOptionsHost_->append(*shareLimitsHelp_);"));
  UR_EXPECT_TRUE(Before(build, "moreOptionsHost_->append(*shareLimitsHelp_);",
                        "T_(\"connect_options\", \"Connect options\")"));
  // both pickers write through the echo guard, the speed's and then the month's
  UR_EXPECT_EQ(2u, CountOf(build, "if (updatingControls_) return;\n      OnShareLimitChosen("));
  const std::string raw = ReadRaw("ConnectPage.cpp");
  UR_EXPECT_TRUE(Before(raw, "OnShareLimitChosen(/*speed=*/true);",
                        "OnShareLimitChosen(/*speed=*/false);"));
}

UR_TEST(ProvideShareStatusListenerRidesThePresentation) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string drawer = FunctionBody(host, "void SdkHost::SubscribeDrawer()");
  UR_EXPECT_TRUE(Contains(drawer, "presentationSubs_.push_back(device_->addProvideShareStatusChangeListener("));
  UR_EXPECT_TRUE(Before(drawer, "addProvideShareStatusChangeListener(",
                        "EmitDrawerEvent(DrawerEvent::ProvideShareStatus);"));
  // the event redraws the line; a device arriving, a re-show and the build
  // re-read the limits, as they re-read the extender row's status
  const std::string page = ReadCode("ConnectPage.cpp");
  const std::string events = FunctionBody(page, "void ConnectPage::OnHostEvent(DrawerEvent event)");
  UR_EXPECT_TRUE(Before(events, "case DrawerEvent::ProvideShareStatus:", "ApplyShareStatus();"));
  const std::string refresh = FunctionBody(page, "void ConnectPage::RefreshFeeds(bool force)");
  UR_EXPECT_TRUE(Before(refresh, "ApplyExtenderProvideState();", "ApplyShareLimits();"));
}

UR_TEST(ProvideShareNothingIsWrittenWhileTheSectionIsHidden) {
  const std::string page = ReadCode("ConnectPage.cpp");
  const std::string chosen = FunctionBody(page, "void ConnectPage::OnShareLimitChosen(bool speed)");
  UR_EXPECT_TRUE(Before(chosen, "if (!shareLimits_) return;", "host_.SetProvideShareLimits(limits);"));
  const std::string apply = FunctionBody(page, "void ConnectPage::ApplyShareLimits()");
  UR_EXPECT_TRUE(Contains(apply, "shareLimits_ = host_.GetProvideShareLimits();"));
  UR_EXPECT_TRUE(Contains(apply, "shareLimitsHeader_.root->set_visible(shown);"));
  // with no device the host reads none and drops a write
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string get =
      FunctionBody(host, "std::optional<urnet::ProvideShareLimits> SdkHost::GetProvideShareLimits()");
  UR_EXPECT_TRUE(Contains(get, "if (!device_) return std::nullopt;"));
  const std::string set = FunctionBody(host, "void SdkHost::SetProvideShareLimits(");
  UR_EXPECT_TRUE(Before(set, "if (!device_) {", "device_->setProvideShareLimits(limits);"));
}
