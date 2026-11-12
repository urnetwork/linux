// The account section's extender logic: the settings form's fields, the
// bootstrap DNS-over-HTTPS servers box and its messages, the share screen's
// reading of build_share and the import screen's decision from decode_share
// (EXTENDER.md K6/K7). The servers' texts are pinned against po/en.po.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ExtenderSharePresentation.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

using urnw::extender::ApplySettingsField;
using urnw::extender::ControlDohErrorText;
using urnw::extender::ControlDohImportArg;
using urnw::extender::ControlDohSaveOutcomeFor;
using urnw::extender::kControlDohChinaCountryCode;
using urnw::extender::kControlDohErrorHttpsRequired;
using urnw::extender::kControlDohErrorIpRequired;
using urnw::extender::kControlDohErrorTooMany;
using urnw::extender::kControlDohErrorUrlInvalid;
using urnw::extender::SplitControlDohLines;
using urnw::extender::ImportDecision;
using urnw::extender::ImportPresentation;
using urnw::extender::ImportPresentationFor;
using urnw::extender::JoinHostLines;
using urnw::extender::SettingsField;
using urnw::extender::ShareCodeMessageKey;
using urnw::extender::ShareCodeState;
using urnw::extender::ShareCodeStateFor;
using urnw::extender::SharePresentation;
using urnw::extender::SharePresentationFor;
using urnw::extender::SplitHostLines;

namespace {
constexpr const char* kPayload = "ur-ext:1:AAECAwQ";
constexpr const char* kHome = "bringyour.com";
constexpr const char* kForeign = "example.test";

// decode_share answered, ok, this network's own host, no settings block
ImportPresentation Local(int64_t count) {
  return ImportPresentationFor(true, true, "", kHome, false, count, false, "", false);
}
}  // namespace

// ---- the settings form ------------------------------------------------------

// "Empty means the default": a field at its default shows NOTHING and names
// the default as its placeholder.
UR_TEST(ExtenderSettings_DefaultFieldIsEmptyWithThePlaceholder) {
  const SettingsField field = ApplySettingsField({}, "extender.ur.network", true);
  UR_EXPECT_TRUE(field.text.empty());
  UR_EXPECT_TRUE(field.defaultValue == "extender.ur.network");
  UR_EXPECT_TRUE(field.hasDefault());
}

// An override shows the override -- and keeps the default it learned earlier,
// so the user can still see what they are overriding.
UR_TEST(ExtenderSettings_OverrideKeepsTheLearnedDefault) {
  const SettingsField defaulted = ApplySettingsField({}, "extender.ur.network", true);
  const SettingsField overridden = ApplySettingsField(defaulted, "ext.example.test", false);
  UR_EXPECT_TRUE(overridden.text == "ext.example.test");
  UR_EXPECT_TRUE(overridden.defaultValue == "extender.ur.network");
}

// A form that opens straight onto an override has no default to show yet, and
// says nothing rather than inventing one.
UR_TEST(ExtenderSettings_UnseenDefaultHasNoPlaceholder) {
  const SettingsField field = ApplySettingsField({}, "ext.example.test", false);
  UR_EXPECT_TRUE(field.text == "ext.example.test");
  UR_EXPECT_FALSE(field.hasDefault());
}

// Clearing the field and saving hands the default back, and the placeholder
// appears -- which is the path that teaches a form opened on an override.
UR_TEST(ExtenderSettings_ClearingRestoresTheDefaultPlaceholder) {
  SettingsField field = ApplySettingsField({}, "ext.example.test", false);
  field = ApplySettingsField(field, "extender.ur.network", true);
  UR_EXPECT_TRUE(field.text.empty());
  UR_EXPECT_TRUE(field.defaultValue == "extender.ur.network");
}

// One host per line: blanks and stray whitespace go, duplicates collapse to
// their FIRST occurrence, and the order the user typed survives.
UR_TEST(ExtenderHosts_LinesAreTrimmedDedupedAndOrdered) {
  const auto hosts = SplitHostLines("  ext1.example.test \n\n192.0.2.9\r\n\text1.example.test\n  \n");
  UR_EXPECT_EQ(2, static_cast<int>(hosts.size()));
  UR_EXPECT_TRUE(hosts[0] == "ext1.example.test");
  UR_EXPECT_TRUE(hosts[1] == "192.0.2.9");
  UR_EXPECT_TRUE(JoinHostLines(hosts) == "ext1.example.test\n192.0.2.9");
}

UR_TEST(ExtenderHosts_EmptyTextIsAnEmptyList) {
  UR_EXPECT_EQ(0, static_cast<int>(SplitHostLines("").size()));
  UR_EXPECT_EQ(0, static_cast<int>(SplitHostLines("\n\n  \n").size()));
  UR_EXPECT_TRUE(JoinHostLines({}).empty());
}

UR_TEST(ExtenderHosts_RoundTrip) {
  const std::vector<std::string> hosts = {"a.example.test", "2001:db8::1", "192.0.2.9"};
  UR_EXPECT_TRUE(SplitHostLines(JoinHostLines(hosts)) == hosts);
}

// ---- the bootstrap DNS-over-HTTPS servers ---------------------------------------

namespace {
// a regional preset in the shape of the sdk's (connect RegionalControlDohUrls),
// v4 first, on documentation addresses
const std::vector<std::string> kPresetServers = {
    "https://192.0.2.53/dns-query",
    "https://192.0.2.54/dns-query",
    "https://198.51.100.53/dns-query",
    "https://203.0.113.53/dns-query",
};
}  // namespace

// Line breaks only: a CRLF, a bare CR and an LF each end a line, blanks drop,
// lines are trimmed, and a comma stays inside its url -- the hosts box's comma
// split would cut it in two.
UR_TEST(ControlDohLines_SplitOnLineBreaksOnly) {
  const auto lines =
      SplitControlDohLines("https://192.0.2.53/q\r\n  https://198.51.100.53/q \rhttps://203.0.113.4/a,b\n\n");
  UR_EXPECT_EQ(3, static_cast<int>(lines.size()));
  if (lines.size() != 3) return;
  UR_EXPECT_TRUE(lines[0] == "https://192.0.2.53/q");
  UR_EXPECT_TRUE(lines[1] == "https://198.51.100.53/q");
  UR_EXPECT_TRUE(lines[2] == "https://203.0.113.4/a,b");
}

// The order typed, repeats kept: dropping them is the SDK's, with the
// normalizing, so every platform hands it the same list.
UR_TEST(ControlDohLines_OrderAndRepeatsAreTheSdks) {
  const auto lines = SplitControlDohLines(
      "https://192.0.2.54/dns-query\nhttps://192.0.2.53/dns-query\nhttps://192.0.2.54/dns-query");
  UR_EXPECT_TRUE(lines == std::vector<std::string>({"https://192.0.2.54/dns-query",
                                                    "https://192.0.2.53/dns-query",
                                                    "https://192.0.2.54/dns-query"}));
}

// An empty box is the built-in servers alone.
UR_TEST(ControlDohLines_AnEmptyBoxIsTheBuiltInServers) {
  UR_EXPECT_EQ(0, static_cast<int>(SplitControlDohLines("").size()));
  UR_EXPECT_EQ(0, static_cast<int>(SplitControlDohLines(" \r\n\t\n\r").size()));
}

// "Use China resolvers" fills the box one server per line, and a save sends
// the same list back.
UR_TEST(ControlDohLines_ThePresetIsOnePerLine) {
  UR_EXPECT_TRUE(std::string(kControlDohChinaCountryCode) == "cn");
  UR_EXPECT_TRUE(JoinHostLines(kPresetServers) == R"(https://192.0.2.53/dns-query
https://192.0.2.54/dns-query
https://198.51.100.53/dns-query
https://203.0.113.53/dns-query)");
  UR_EXPECT_TRUE(SplitControlDohLines(JoinHostLines(kPresetServers)) == kPresetServers);
}

// Every SDK id is its own key (the sdk's literal ids), each with its message.
UR_TEST(ControlDohErrors_EveryIdHasItsMessage) {
  UR_EXPECT_TRUE(std::string(kControlDohErrorUrlInvalid) == "control_doh_error_url_invalid");
  UR_EXPECT_TRUE(std::string(kControlDohErrorHttpsRequired) == "control_doh_error_https_required");
  UR_EXPECT_TRUE(std::string(kControlDohErrorIpRequired) == "control_doh_error_ip_required");
  UR_EXPECT_TRUE(std::string(kControlDohErrorTooMany) == "control_doh_error_too_many");
  for (const char* id : {kControlDohErrorUrlInvalid, kControlDohErrorHttpsRequired,
                         kControlDohErrorIpRequired, kControlDohErrorTooMany}) {
    UR_EXPECT_TRUE_MSG(id, ControlDohErrorText(id).key == id);
    UR_EXPECT_TRUE_MSG(id, !ControlDohErrorText(id).english.empty());
  }
}

// Any other id -- the C ABI's internal_error for a call that could not run
// (URNET_ERROR_ID_INTERNAL, of which the app keeps no copy:
// ErrorIdInternalTest.cpp), an id this build does not know -- is still a
// refusal, and none of them is about the url typed: "Something went wrong.",
// never a raw key.
UR_TEST(ControlDohErrors_AnyOtherIdIsSomethingWentWrong) {
  for (const char* id : {"internal_error", "control_doh_error_from_a_newer_sdk",
                         "vless_error_link_invalid", ""}) {
    UR_EXPECT_TRUE_MSG(id, ControlDohErrorText(id).key == "something_went_wrong");
    UR_EXPECT_TRUE_MSG(id, ControlDohErrorText(id).english == "Something went wrong.");
  }
}

// Saved is the saved line alone: the app's device remote hands a running
// urnetworkd tunnel the servers in place, so there is no next-connect note; an
// id is its message alone.
UR_TEST(ControlDohSave_TheResultLines) {
  const auto saved = ControlDohSaveOutcomeFor("");
  UR_EXPECT_TRUE(saved.saved);
  UR_EXPECT_TRUE(saved.message.key == "control_doh_urls_saved");
  UR_EXPECT_TRUE(saved.note.key.empty());
  const auto refused = ControlDohSaveOutcomeFor(kControlDohErrorIpRequired);
  UR_EXPECT_FALSE(refused.saved);
  UR_EXPECT_TRUE(refused.message.key == kControlDohErrorIpRequired);
  UR_EXPECT_TRUE(refused.note.key.empty());
  // only "" is a save: a call that could not run, or an id this build does
  // not know, saved nothing
  for (const char* id : {"internal_error", "control_doh_error_from_a_newer_sdk"}) {
    const auto failed = ControlDohSaveOutcomeFor(id);
    UR_EXPECT_TRUE_MSG(id, !failed.saved);
    UR_EXPECT_TRUE_MSG(id, failed.message.key == "something_went_wrong");
    UR_EXPECT_TRUE_MSG(id, failed.note.key.empty());
  }
}

// Every text is the catalog's own entry, key and English byte for byte: the
// English is the msgid T_ looks the translation up by.
UR_TEST(ControlDohText_KeysAreTheCatalogs) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  UR_EXPECT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string catalog = buffer.str();
  std::vector<urnw::extender::Text> texts;
  for (const char* id : {kControlDohErrorUrlInvalid, kControlDohErrorHttpsRequired,
                         kControlDohErrorIpRequired, kControlDohErrorTooMany,
                         "internal_error"}) {
    texts.push_back(ControlDohErrorText(id));
  }
  const auto saved = ControlDohSaveOutcomeFor("");
  texts.push_back(saved.message);
  // a save has no note line any more (the empty key is no text)
  UR_EXPECT_TRUE(saved.note.key.empty());
  for (const auto& text : texts) {
    const std::string entry = "msgctxt \"" + std::string(text.key) + "\"\nmsgid \"" +
                              std::string(text.english) + "\"\n";
    UR_EXPECT_TRUE_MSG(entry, catalog.find(entry) != std::string::npos);
  }
}

// The import screen names the servers a code's settings would set, whether or
// not the switch is on yet; a code without settings, or whose settings name
// none, says nothing and leaves this device's servers alone.
UR_TEST(ControlDohImport_TheSettingsServersAreNamed) {
  UR_EXPECT_TRUE(ControlDohImportArg(true, {"https://192.0.2.53/dns-query",
                                            "https://198.51.100.53/dns-query"}) ==
                 "https://192.0.2.53/dns-query, https://198.51.100.53/dns-query");
  UR_EXPECT_TRUE(ControlDohImportArg(true, {}).empty());
  UR_EXPECT_TRUE(ControlDohImportArg(false, kPresetServers).empty());
}

// ---- the share screen -------------------------------------------------------

UR_TEST(ExtenderShare_NoResultRendersNothing) {
  const SharePresentation share = SharePresentationFor(false, kPayload, 12, true);
  UR_EXPECT_FALSE(share.ready);
  UR_EXPECT_FALSE(share.canCopy);
  UR_EXPECT_FALSE(share.canRenderCode);
  UR_EXPECT_EQ(0, share.count);
}

UR_TEST(ExtenderShare_PayloadDrivesTheCodeAndTheCopyButton) {
  const SharePresentation share = SharePresentationFor(true, kPayload, 12, true);
  UR_EXPECT_TRUE(share.ready);
  UR_EXPECT_TRUE(share.text == kPayload);
  UR_EXPECT_EQ(12, share.count);
  UR_EXPECT_TRUE(share.includesSettings);
  UR_EXPECT_TRUE(share.canCopy);
  UR_EXPECT_TRUE(share.canRenderCode);
}

// A device that has discovered nothing yet still answers: zero is a reading,
// not a failure. With no payload there is simply nothing to encode or copy.
UR_TEST(ExtenderShare_EmptyPayloadStillReadsItsCount) {
  const SharePresentation share = SharePresentationFor(true, "", 0, false);
  UR_EXPECT_TRUE(share.ready);
  UR_EXPECT_EQ(0, share.count);
  UR_EXPECT_FALSE(share.canCopy);
  UR_EXPECT_FALSE(share.canRenderCode);
}

// The four readings of the code area. `canRenderCode` only says there is
// something to hand the encoder; whether a code appears is decided after it has
// run.
UR_TEST(ExtenderShare_CodeAreaHasFourReadings) {
  const SharePresentation none = SharePresentationFor(false, "", 0, false);
  UR_EXPECT_TRUE(ShareCodeStateFor(none, false) == ShareCodeState::Unavailable);
  // ...and an encoder that somehow succeeded cannot override "no answer"
  UR_EXPECT_TRUE(ShareCodeStateFor(none, true) == ShareCodeState::Unavailable);

  const SharePresentation empty = SharePresentationFor(true, "", 0, false);
  UR_EXPECT_TRUE(ShareCodeStateFor(empty, false) == ShareCodeState::Empty);

  const SharePresentation payload = SharePresentationFor(true, kPayload, 12, false);
  UR_EXPECT_TRUE(ShareCodeStateFor(payload, true) == ShareCodeState::Code);
  UR_EXPECT_TRUE(ShareCodeStateFor(payload, false) == ShareCodeState::TooLarge);
}

// A payload the encoder refuses says so ON SCREEN and points at the copyable
// text, which is then the only way to move the addresses. An empty share and a
// drawn code need no sentence of their own.
UR_TEST(ExtenderShare_CodeAreaMessages) {
  UR_EXPECT_TRUE(std::string(ShareCodeMessageKey(ShareCodeState::Unavailable)) ==
                 "something_went_wrong");
  UR_EXPECT_TRUE(std::string(ShareCodeMessageKey(ShareCodeState::TooLarge)) ==
                 "share_extenders_too_large");
  UR_EXPECT_TRUE(std::string(ShareCodeMessageKey(ShareCodeState::Empty)).empty());
  UR_EXPECT_TRUE(std::string(ShareCodeMessageKey(ShareCodeState::Code)).empty());
}

// The copy button survives a payload that cannot be drawn: it is the whole
// fallback for that case.
UR_TEST(ExtenderShare_TooLargePayloadStillCopies) {
  const SharePresentation share = SharePresentationFor(true, kPayload, 48, true);
  UR_EXPECT_TRUE(ShareCodeStateFor(share, false) == ShareCodeState::TooLarge);
  UR_EXPECT_TRUE(share.canCopy);
}

// ---- the import screen ------------------------------------------------------

UR_TEST(ExtenderImport_NothingDecodedIsWaiting) {
  const ImportPresentation view = ImportPresentationFor(false, false, "", "", false, 0, false, "",
                                                        false);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Waiting);
  UR_EXPECT_FALSE(view.showCount);
  UR_EXPECT_FALSE(view.canImport);
  UR_EXPECT_TRUE(view.messageKey.empty());
}

// The SDK's error is a localization KEY, and it is shown as-is.
UR_TEST(ExtenderImport_SdkErrorKeyIsTheMessage) {
  const ImportPresentation view = ImportPresentationFor(
      true, false, "import_extenders_invalid", "", false, 0, false, "", false);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Invalid);
  UR_EXPECT_TRUE(view.messageKey == "import_extenders_invalid");
  UR_EXPECT_FALSE(view.canImport);
  UR_EXPECT_FALSE(view.showCount);
}

// A failure with no error string still says something, rather than showing a
// blank line under a dead button.
UR_TEST(ExtenderImport_FailureWithoutAnErrorStillSpeaks) {
  const ImportPresentation view =
      ImportPresentationFor(true, false, "", kHome, false, 0, false, "", false);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Invalid);
  UR_EXPECT_TRUE(view.messageKey == "import_extenders_invalid");
}

// The ordinary case: this network's own code, no settings, import straight in
// with no confirmation and no switch.
UR_TEST(ExtenderImport_LocalCodeImportsDirectly) {
  const ImportPresentation view = Local(9);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Ready);
  UR_EXPECT_TRUE(view.showCount);
  UR_EXPECT_EQ(9, view.count);
  UR_EXPECT_TRUE(view.canImport);
  UR_EXPECT_FALSE(view.confirmBeforeImport);
  UR_EXPECT_FALSE(view.showUseSettings);
  UR_EXPECT_TRUE(view.messageKey.empty());
}

// K7: a foreign host is REFUSED unless "use extender settings" is chosen. The
// message names the operator the code belongs to.
UR_TEST(ExtenderImport_ForeignHostIsRefusedUntilSettingsAreChosen) {
  const ImportPresentation refused =
      ImportPresentationFor(true, true, "", kForeign, true, 6, true, kForeign, false);
  UR_EXPECT_TRUE(refused.decision == ImportDecision::NeedsSettings);
  UR_EXPECT_FALSE(refused.canImport);
  UR_EXPECT_TRUE(refused.showUseSettings);
  UR_EXPECT_TRUE(refused.messageKey == "import_extenders_foreign_host");
  UR_EXPECT_TRUE(refused.messageArg == kForeign);
  UR_EXPECT_TRUE(refused.showCount);
  UR_EXPECT_EQ(6, refused.count);

  const ImportPresentation allowed =
      ImportPresentationFor(true, true, "", kForeign, true, 6, true, kForeign, true);
  UR_EXPECT_TRUE(allowed.decision == ImportDecision::Confirm);
  UR_EXPECT_TRUE(allowed.canImport);
  UR_EXPECT_TRUE(allowed.confirmBeforeImport);
  UR_EXPECT_TRUE(allowed.messageKey == "import_extenders_confirm_settings");
  UR_EXPECT_TRUE(allowed.messageArg == kForeign);
}

// A foreign code with no settings block is a dead end: the switch that would
// unblock it has nothing to apply, so there is no button to press.
UR_TEST(ExtenderImport_ForeignHostWithoutSettingsIsADeadEnd) {
  const ImportPresentation view =
      ImportPresentationFor(true, true, "", kForeign, true, 4, false, "", true);
  UR_EXPECT_TRUE(view.decision == ImportDecision::NeedsSettings);
  UR_EXPECT_FALSE(view.canImport);
  UR_EXPECT_FALSE(view.showUseSettings);
  UR_EXPECT_TRUE(view.messageKey == "import_extenders_foreign_host");
}

// Replacing the dns name, gossip url and root keys is the consequential act,
// so it confirms even when the code is from this very network.
UR_TEST(ExtenderImport_ApplyingSettingsAlwaysConfirms) {
  const ImportPresentation view =
      ImportPresentationFor(true, true, "", kHome, false, 3, true, kHome, true);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Confirm);
  UR_EXPECT_TRUE(view.canImport);
  UR_EXPECT_TRUE(view.confirmBeforeImport);
  UR_EXPECT_TRUE(view.messageArg == kHome);
}

// The switch appears whenever the code carries settings, and leaving it off
// imports the addresses alone with no confirmation.
UR_TEST(ExtenderImport_SettingsSwitchIsOptionalOnALocalCode) {
  const ImportPresentation view =
      ImportPresentationFor(true, true, "", kHome, false, 3, true, kHome, false);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Ready);
  UR_EXPECT_TRUE(view.showUseSettings);
  UR_EXPECT_TRUE(view.canImport);
  UR_EXPECT_FALSE(view.confirmBeforeImport);
}

// An older SDK that leaves the settings host blank still names an operator in
// the confirmation, falling back to the code's network host.
UR_TEST(ExtenderImport_ConfirmationFallsBackToTheNetworkHost) {
  const ImportPresentation view =
      ImportPresentationFor(true, true, "", kForeign, true, 3, true, "", true);
  UR_EXPECT_TRUE(view.decision == ImportDecision::Confirm);
  UR_EXPECT_TRUE(view.messageArg == kForeign);
}

UR_TEST(ExtenderImport_NegativeCountClampsToZero) {
  const ImportPresentation view = Local(-5);
  UR_EXPECT_EQ(0, view.count);
}
