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
//     what the device took and a bound device's sync takes the mirror when it
//     differs, where before the edit lived only in the GUI's local state. The
//     mirror's mark (DnsMirror.hpp) is cleared while the two are written, is
//     per network space, and a write with no device never sets it, so a copy
//     from before the daemon split takes the daemon's settings instead of
//     replacing them;
//   - the card's pill and summary are the sdk's description, not the app's
//     whole-value equality, which showed the defaults pill under every preset;
//   - the resolver's status reaches the card and the sheet, the lines under
//     the selection follow it on every rebuild of the picks (a pick tapped and
//     then left showed "Use URnetwork DNS" under a provider that answered),
//     and the sheet reads the blocked count only while it shows.
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
  const std::string equal =
      wiring::FunctionBody(wiring::ReadCode("DnsSettingsEqual.hpp"), "inline bool DnsSettingsEqual(");
  UR_EXPECT_TRUE(wiring::Contains(equal, "a.PresetId == b.PresetId"));
  UR_EXPECT_TRUE(wiring::Contains(equal, "a.PresetPersonalId == b.PresetPersonalId"));
  // the sheet's comparison and the host's sync of its mirror are the one
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(sheet, "bool DnsSheet::SettingsEqual("),
                                  "return DnsSettingsEqual(a, b);"));
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  UR_EXPECT_TRUE(wiring::Contains(wiring::FunctionBody(host, "void SdkHost::ReconcileDnsMirrorLocked() {"),
                                  "DnsSettingsEqual(*mirror, *daemon)"));
}

UR_TEST(DnsPresetsWiring_AnEditWithTheTunnelDownReachesTheTunnel) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  // a write clears the mark, goes to the device when there is one, mirrors
  // what the device took (it refuses a selection without a word), and only
  // then marks the mirror, unless it was a write with no device over an
  // unmarked mirror (DnsMirror.hpp)
  const std::string set = wiring::FunctionBody(host, "SdkHost::SetDnsResolverSettingsLocked(");
  UR_EXPECT_TRUE(wiring::Before(set, "const bool wasMarked = DnsMirrorMarkedLocked();",
                                "SetDnsMirrorMarkedLocked(false);"));
  UR_EXPECT_TRUE(wiring::Before(set, "SetDnsMirrorMarkedLocked(false);",
                                "device_->setDnsResolverSettings(settings);"));
  UR_EXPECT_TRUE(wiring::Before(set, "device_->setDnsResolverSettings(settings);",
                                "device_->getDnsResolverSettings()"));
  UR_EXPECT_TRUE(wiring::Before(set, "device_->getDnsResolverSettings()",
                                "localState_->setDnsResolverSettings(taken);"));
  UR_EXPECT_TRUE(wiring::Before(set, "localState_->setDnsResolverSettings(taken);",
                                "dns_mirror::MarkedAfterWrite(device_.has_value(), wasMarked)"));
  UR_EXPECT_TRUE(wiring::Before(set, "dns_mirror::MarkedAfterWrite(device_.has_value(), wasMarked)",
                                "SetDnsMirrorMarkedLocked(true);"));
  // the compare-and-set reports what the device took, and a main-loop read
  // before the queue settles says it
  const std::string cas = wiring::FunctionBody(host, "bool SdkHost::SetDnsResolverSettingsIf(");
  UR_EXPECT_TRUE(wiring::Before(cas, "= SetDnsResolverSettingsLocked(settings);",
                                "KeepOnMain<std::optional<urnet::DnsResolverSettings>>(\"GetDnsResolverSettings\", taken)"));
  UR_EXPECT_TRUE(wiring::Before(cas, "KeepOnMain<", "if (current) *current = std::move(taken);"));
  // one mark per network space, as each has its own LocalState
  const std::string key = wiring::FunctionBody(host, "std::string SdkHost::DnsMirrorMarkKeyLocked() {");
  UR_EXPECT_TRUE(wiring::Before(key, "networkSpace_->getKey()",
                                "prefs::DnsSettingsMirroredKey(*key->host_name, *key->env_name)"));
  UR_EXPECT_FALSE(wiring::Contains(host, "prefs::kDnsSettingsMirroredKey"));
  // the bound device's sync, not the start, meets the mirror with the
  // daemon's settings: an unmarked mirror takes them, a marked one that
  // differs goes to the device, and one that matches changes nothing
  const std::string bind =
      wiring::FunctionBody(host, "TunnelStartResult SdkHost::BindRemoteDeviceLocked(");
  UR_EXPECT_FALSE(wiring::Contains(bind, "localState_->getDnsResolverSettings()"));
  const size_t afterTransport = bind.find("device_->setProviderTransportSettings(settings);");
  UR_EXPECT_TRUE(afterTransport != std::string::npos);
  const std::string sync =
      afterTransport == std::string::npos ? std::string() : bind.substr(afterTransport);
  UR_EXPECT_TRUE(wiring::Before(sync, "subs_.push_back(device_->addRemoteChangeListener(",
                                "RunCommand(ConnectCommandQueue::Kind::Device"));
  UR_EXPECT_TRUE(wiring::Before(sync, "if (rpcGeneration != rpcSessionGeneration_.load()) return;",
                                "ReconcileDnsMirrorLocked();"));
  UR_EXPECT_TRUE(wiring::Before(sync, "ReconcileDnsMirrorLocked();", "SubscribeDrawer();"));
  const std::string reconcile =
      wiring::FunctionBody(host, "void SdkHost::ReconcileDnsMirrorLocked() {");
  UR_EXPECT_TRUE(wiring::Before(reconcile, "device_->getDnsResolverSettings()",
                                "dns_mirror::SyncAction("));
  UR_EXPECT_TRUE(wiring::Before(reconcile, "case dns_mirror::Sync::Adopt:",
                                "localState_->setDnsResolverSettings(daemon);"));
  UR_EXPECT_TRUE(wiring::Before(reconcile, "localState_->setDnsResolverSettings(daemon);",
                                "SetDnsMirrorMarkedLocked(true);"));
  UR_EXPECT_TRUE(wiring::Before(reconcile, "case dns_mirror::Sync::Apply:",
                                "SetDnsResolverSettingsLocked(*mirror);"));
  UR_EXPECT_EQ(1, int(wiring::CountOf(host, "ReconcileDnsMirrorLocked();")));
}

// The lines under the selected preset are made hidden and say what the status
// says after every rebuild of the picks. Before, only a seed rendered them: a
// pick tapped and then left (Custom, then the selected preset again) rebuilt
// them in GTK's default, shown, with "Use URnetwork DNS" and "Change location"
// under a provider that answered, an empty status line, and the count's poll
// stopped.
UR_TEST(DnsPresetsWiring_TheStatusLinesFollowEveryRebuild) {
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  const std::string rebuild = wiring::FunctionBody(sheet, "void DnsSheet::RebuildPicks() {");
  UR_EXPECT_TRUE(wiring::Before(rebuild, "fastDnsPresetNote_->set_visible(preset);", "RenderStatus();"));
  const std::string panel = wiring::FunctionBody(sheet, "void DnsSheet::BuildStatusPanel(");
  for (const char* line : {"statusLine_->set_visible(false);", "statusDetail_->set_visible(false);",
                           "verificationLine_->set_visible(false);",
                           "statusActions_->set_visible(false);", "useDefaultBtn_->set_visible(false);",
                           "changeLocationBtn_->set_visible(false);",
                           "blockedCountLine_->set_visible(false);",
                           "blockedFloorNote_->set_visible(false);"}) {
    UR_EXPECT_TRUE_MSG(line, wiring::Contains(panel, line));
  }
  // a seed reads the status before its rebuild renders it
  UR_EXPECT_TRUE(wiring::Before(wiring::FunctionBody(sheet, "void DnsSheet::Seed("),
                                "status_ = host_.GetDnsResolverStatus();", "RebuildPicks();"));
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
