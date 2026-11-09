// The third-party DNS presets on the Custom DNS sheet and the Connect page's
// DNS card (connect ADBLOCK3RD.md F1, F2). The surfaces need GTK and the SDK,
// so this reads their sources, the way the other wiring tests do.
//
// What it pins, each the cause of a defect the presets would have met:
//   - every save starts from one of the sdk's helpers on the settings the sheet
//     was seeded from, never a struct the app fills in: an editor that kept a
//     preset's selection under its edits saved lists the device then ignored,
//     and one that built its own struct dropped every field it did not model;
//   - the editors' comparison counts the selection, so a change of selection
//     whose lists are the same is a change;
//   - an edit made with the tunnel down reaches the tunnel: the GUI mirrors
//     every write and applies the mirror to the daemon's device it binds, where
//     before the edit lived only in the GUI's local state;
//   - the card's pill and summary are the sdk's description, not the app's
//     whole-value equality, which showed the defaults pill under every preset;
//   - the resolver's status reaches the card and the sheet, and the sheet reads
//     the blocked count only while it shows.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>

namespace {

namespace wiring = urnw::testing::wiring;

// The statements of one case of a switch: from its label to its first break.
std::string CaseBody(const std::string& body, const std::string& label) {
  const size_t at = body.find(label);
  if (at == std::string::npos) return std::string();
  const size_t end = body.find("break;", at);
  return body.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

UR_TEST(DnsPresetsWiring_EverySaveStartsFromAnSdkHelper) {
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  // Custom drafts from the settings as custom: the lists in force, the
  // selection cleared, every other field kept
  const std::string seed = wiring::FunctionBody(sheet, "void DnsSheet::Seed(");
  UR_EXPECT_TRUE(wiring::Contains(seed, "urnet::dnsResolverSettingsAsCustom(settings)"));
  UR_EXPECT_TRUE(wiring::Before(seed, "urnet::dnsResolverSettingsAsCustom(settings)",
                                "draft_ = customSeed_;"));
  // a pick, a profile id and Fast DNS on connect each through their helper,
  // on what the sheet was seeded from
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(sheet, "void DnsSheet::SelectPick("),
                                  "urnet::dnsResolverSettingsWithPreset(original_, presetId, "
                                  "std::string())"));
  const std::string personal = wiring::FunctionBody(sheet, "void DnsSheet::SavePersonalId(");
  UR_EXPECT_TRUE(wiring::Before(personal, "urnet::validateDnsPresetPersonalId(presetId, input)",
                                "urnet::dnsResolverSettingsWithPreset("));
  UR_EXPECT_TRUE(wiring::Contains(personal, "urnet::normalizeDnsPresetPersonalId(presetId, input)"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(sheet, "void DnsSheet::SaveFallback("),
                                  "urnet::dnsResolverSettingsWithFallback(original_, "
                                  "enableFallback)"));
  // the four saves, and only they, reach the one compare-and-set: its
  // definition and four calls
  UR_EXPECT_EQ(5, int(wiring::CountOf(sheet, "Write(")));
  UR_EXPECT_EQ(3, int(wiring::CountOf(sheet, "Write(*next, ")));
  UR_EXPECT_EQ(1, int(wiring::CountOf(sheet, "Write(draft_, AfterWrite::Close, Edits::Drop);")));
  // no settings struct is built in the sheet
  UR_EXPECT_FALSE(wiring::Contains(sheet, "urnet::DnsResolverSettings{"));
  UR_EXPECT_FALSE(wiring::Contains(sheet, "urnet::DnsResolverSettings()"));
}

UR_TEST(DnsPresetsWiring_TheComparisonCountsTheSelection) {
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  const std::string equal = wiring::FunctionBody(sheet, "bool DnsSheet::SettingsEqual(");
  UR_EXPECT_TRUE(wiring::Contains(equal, "a.PresetId == b.PresetId"));
  UR_EXPECT_TRUE(wiring::Contains(equal, "a.PresetPersonalId == b.PresetPersonalId"));
}

UR_TEST(DnsPresetsWiring_AnEditWithTheTunnelDownReachesTheTunnel) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  // every write goes to the device when there is one AND to the GUI's mirror,
  // which is marked as holding every write since; a missed write clears it
  const std::string set =
      wiring::FunctionBody(host, "void SdkHost::SetDnsResolverSettingsLocked(");
  UR_EXPECT_TRUE(wiring::Contains(set, "if (device_) device_->setDnsResolverSettings(settings);"));
  UR_EXPECT_FALSE(wiring::Contains(set, "return;"));
  UR_EXPECT_TRUE(wiring::Before(set, "localState_->setDnsResolverSettings(settings);",
                                "{prefs::kDnsSettingsMirroredKey, true}"));
  UR_EXPECT_TRUE(wiring::Before(set, "catch (const std::exception& e)",
                                "{prefs::kDnsSettingsMirroredKey, false}"));
  // and the device a start binds gets the marked mirror, beside the transport
  // policies and before the drawer subscribes to it
  const std::string bind =
      wiring::FunctionBody(host, "TunnelStartResult SdkHost::BindRemoteDeviceLocked(");
  UR_EXPECT_TRUE(wiring::Before(bind, "device_->setProviderTransportSettings(settings);",
                                "if (prefs::Get<bool>(prefs::kDnsSettingsMirroredKey, false)) {"));
  UR_EXPECT_TRUE(wiring::Before(bind,
                                "if (prefs::Get<bool>(prefs::kDnsSettingsMirroredKey, false)) {",
                                "if (auto settings = localState_->getDnsResolverSettings(); "
                                "settings) {"));
  UR_EXPECT_TRUE(wiring::Before(bind,
                                "if (auto settings = localState_->getDnsResolverSettings(); "
                                "settings) {",
                                "SubscribeDrawer();"));
  UR_EXPECT_EQ(1, int(wiring::CountOf(host, "prefs::Get<bool>(prefs::kDnsSettingsMirroredKey")));
}

UR_TEST(DnsPresetsWiring_TheCardSaysWhatTheSdkDescribes) {
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  // the pill the sdk asks for, none while a preset is selected
  const std::string pill =
      wiring::FunctionBody(page, "void ConnectPage::ApplyDnsRecommendationPill() {");
  UR_EXPECT_TRUE(wiring::Contains(pill, "urnet::describeDnsResolverSettings(dnsSettings_, countryCode_)"));
  UR_EXPECT_TRUE(wiring::Contains(pill, "dns_presets::PillText(summary->PillKey)"));
  UR_EXPECT_FALSE(wiring::Contains(page, "urnet::getRecommendedDnsResolverSettings("));
  UR_EXPECT_FALSE(wiring::Contains(page, "urnet::getDefaultDnsResolverSettings("));
  // the summary row, and under it a dropped selection or a failing provider
  const std::string summary = wiring::FunctionBody(page, "void ConnectPage::ApplyDnsSummary() {");
  UR_EXPECT_TRUE(wiring::Contains(summary, "dns_presets::KeyText(summary->SummaryKey)"));
  UR_EXPECT_TRUE(wiring::Before(summary, "summary && summary->Dropped",
                                "urnet::describeDnsResolverStatus(dnsSettings_, dnsStatus_)"));
  UR_EXPECT_TRUE(wiring::Contains(summary, "status && status->ShowUseDefault"));
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(page, "void ConnectPage::ApplyDnsCard() {"),
                                  "ApplyDnsSummary();"));
}

UR_TEST(DnsPresetsWiring_TheStatusReachesTheCardAndTheSheet) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string drawer = wiring::FunctionBody(host, "void SdkHost::SubscribeDrawer() {");
  UR_EXPECT_TRUE(wiring::Before(drawer, "device_->addDnsResolverStatusChangeListener(",
                                "EmitDrawerEvent(DrawerEvent::DnsStatus);"));
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  const std::string events =
      wiring::FunctionBody(page, "void ConnectPage::OnHostEvent(DrawerEvent event) {");
  const std::string status = CaseBody(events, "case DrawerEvent::DnsStatus:");
  UR_EXPECT_TRUE(wiring::Before(status, "dnsStatus_ = host_.GetDnsResolverStatus();",
                                "ApplyDnsCard();"));
  UR_EXPECT_TRUE(wiring::Contains(status, "dnsSheet_->RefreshStatus();"));
  UR_EXPECT_TRUE(wiring::Contains(CaseBody(events, "case DrawerEvent::DnsSettings:"),
                                  "dnsSheet_->RefreshSettings();"));
  // a device that came or went takes its status with it
  UR_EXPECT_TRUE(wiring::Contains(CaseBody(events, "case DrawerEvent::DeviceLifecycle:"),
                                  "dnsSheet_->RefreshStatus();"));
  // the poll re-reads the status too, for a push that landed while hidden
  const std::string feeds = wiring::FunctionBody(page, "void ConnectPage::RefreshFeeds(bool force) {");
  UR_EXPECT_TRUE(wiring::Contains(feeds, "!DnsStatusSame(status, dnsStatus_)"));
  // the earnings page has nothing to show for it
  const std::string earnings = wiring::ReadCode("EarningsPage.cpp");
  UR_EXPECT_TRUE(wiring::Contains(
      wiring::FunctionBody(earnings, "void EarningsPage::OnHostEvent(DrawerEvent event) {"),
      "case DrawerEvent::DnsStatus:"));
  // the sheet reads the blocked count only while it shows: a poll that ends
  // when the sheet hides, or when the count is no longer on it
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  const std::string poll = wiring::FunctionBody(sheet, "void DnsSheet::StartBlockedCountPoll() {");
  UR_EXPECT_TRUE(wiring::Before(poll, "if (!get_visible() || !blockedCountLine_) return false;",
                                "blockedCount_ = host_.GetDnsBlockedCount();"));
  UR_EXPECT_TRUE(wiring::Contains(sheet, "signal_hide().connect([this] { StopBlockedCountPoll(); });"));
  const std::string render = wiring::FunctionBody(sheet, "void DnsSheet::RenderStatus() {");
  UR_EXPECT_TRUE(wiring::Contains(render, "urnet::describeDnsResolverStatus(original_, status_)"));
  UR_EXPECT_TRUE(wiring::Contains(render, "summary.ShowBlockedCount && status_.has_value()"));
}

UR_TEST(DnsPresetsWiring_TheSheetsActions) {
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  // "Use URnetwork DNS" and the dropped notice's Got it save URnetwork's dns
  UR_EXPECT_EQ(2, int(wiring::CountOf(sheet, "SelectPick(urnet::DnsPresetIdDefault);")));
  // "Change location" is the window's chooser, which the page wires
  UR_EXPECT_TRUE(wiring::Contains(sheet, "if (on_change_location) on_change_location();"));
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  UR_EXPECT_TRUE(wiring::Before(wiring::FunctionBody(page, "void ConnectPage::OpenDnsSheet() {"),
                                "dnsSheet_->on_change_location = [this] {",
                                "if (on_open_locations) on_open_locations();"));
  // a control in a pick's row acts after its signal: the action re-seeds and
  // rebuilds the row
  const std::string row = wiring::FunctionBody(sheet, "Gtk::Widget* DnsSheet::BuildPickRow(");
  UR_EXPECT_TRUE(wiring::Contains(row, "Defer([this, pick] { OnPick(pick); });"));
  // the help is ur.io's DNS providers section
  UR_EXPECT_TRUE(wiring::Contains(sheet, "dns_presets::kHelpUrl"));
}

// URnetwork's own blocker is named apart from the providers that block ads,
// whose group heading is now "Block ads and trackers" (F1 item 6).
UR_TEST(DnsPresetsWiring_TheBuiltInBlockerIsUrnetworksOwn) {
  const std::string page = wiring::ReadCode("ConnectPage.cpp");
  UR_EXPECT_TRUE(wiring::Contains(page, "T_(\"dns_builtin_blocker_label\", \"URnetwork ad blocker\")"));
  UR_EXPECT_FALSE(wiring::Contains(page, "T_(\"block_ads_and_trackers\""));
}
