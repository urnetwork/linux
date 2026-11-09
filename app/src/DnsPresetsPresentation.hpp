// The words of the third-party DNS presets on the Custom DNS sheet and the
// Connect page's DNS card (connect ADBLOCK3RD.md F1, F2): the English of every
// localization key the sdk hands the screens (sdk dns_presets_ui.go), so each
// is looked up by its store key and its msgid (I18n.hpp), and the ur.io page
// that explains the presets. Which picks there are, in what order, and what a
// selection and a status say are the sdk's (GetDnsPicks,
// DescribeDnsResolverSettings, DescribeDnsResolverStatus); nothing here decides
// one, it only names the key ids those answers carry.
//
// Placeholders are the store's C++ forms (I18n.hpp Format): `{}` where a text
// takes one value, the provider's name or the country's, and `{0}` `{1}` for
// a personal id's format message, which takes the provider and its example.
//
// Header-only and free of GTK and the SDK, so the unit tests reach it
// (tests/DnsPresetsPresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::dns_presets {

// A store key and its English (the gettext msgid), as VlessPresentation.hpp's.
// An empty key is a key this build does not know: nothing is shown for it.
struct Text {
  std::string_view key;
  std::string_view english;
};

// ur.io's help on DNS providers (the docs FAQ's dns-providers section, under
// the same anchor in every language). The app links ur.io pages as they are,
// never localized.
inline constexpr const char* kHelpUrl = "https://ur.io/docs/faq#dns-providers";
// The same page's section on personal ids (where each provider shows the id,
// and the block response to keep), linked under the "Use my account" field as
// the android and apple apps link it.
inline constexpr const char* kPersonalIdsHelpUrl = "https://ur.io/docs/faq#dns-personal-ids";

// The English of a key id the sdk returns: a group heading (DnsPick.GroupKey),
// a pick's label and the line under it (DnsPick.LabelKey, CopyKey), the
// summary row (DnsSelectionSummary.SummaryKey), the status line, the line
// under it, the verification and its link (DnsStatusSummary.StatusKey,
// StatusDetailKey, VerificationKey, VerificationLinkKey), and a personal
// preset's notes (DnsPreset.PersonalIdWhereKey, PersonalIdFormatKey,
// QuotaNoteKey). An id of a newer sdk has an empty key.
inline Text KeyText(std::string_view key) {
  static constexpr Text kTexts[] = {
      // the group headings, in the screen's order
      {"block_ads_and_trackers", "Block ads and trackers"},
      {"dns_group_block_malware", "Block malware"},
      {"dns_group_family", "Family"},
      {"dns_group_my_account", "Use my account"},
      {"dns_group_no_filtering", "No filtering"},
      // the picks that are not presets, and the line under each; the regional
      // pick's two take the country
      {"dns_provider_default", "URnetwork (default)"},
      {"dns_provider_default_copy", "Encrypted DNS through the tunnel, with no filtering"},
      {"dns_provider_custom", "Custom"},
      {"dns_provider_custom_copy", "Your own DNS over HTTPS URLs and DNS servers"},
      {"dns_provider_regional", "Regional ({}), unencrypted"},
      {"dns_recommendation_message",
       "The strongest security rules are known not to work in {}. There are less secure "
       "recommended DNS settings that work better."},
      // what each preset blocks; a personal preset's takes the provider
      {"dns_preset_copy_ads_phishing", "Blocks ads, trackers and phishing"},
      {"dns_preset_copy_ads_malware", "Blocks ads, trackers and malware"},
      {"dns_preset_copy_ads_malware_phishing", "Blocks ads, trackers, malware and phishing"},
      {"dns_preset_copy_malware", "Blocks malware, not ads"},
      {"dns_preset_copy_malware_phishing", "Blocks malware and phishing, not ads"},
      {"dns_preset_copy_ads_adult_safe_search",
       "Blocks ads, trackers and adult sites, and turns on safe search"},
      {"dns_preset_copy_malware_adult", "Blocks malware and adult sites, not ads"},
      {"dns_preset_copy_ads_malware_adult_drugs",
       "Blocks ads, trackers, malware, adult sites and drugs"},
      {"dns_preset_copy_child", "Blocks malware, adult sites, gambling, drugs and violence, not ads"},
      {"dns_preset_copy_child_ads",
       "Blocks ads, trackers, malware, adult sites, gambling, drugs and violence"},
      {"dns_preset_copy_nothing", "Blocks nothing"},
      {"dns_preset_copy_court_orders", "Blocks only what courts order it to block"},
      {"dns_preset_copy_personal", "Uses the filters you set up in your {} account"},
      // the summary row; a preset's takes the provider
      {"dns_summary_default", "DNS: URnetwork"},
      {"dns_summary_ad_blocking", "DNS: {} · Ad blocking"},
      {"dns_summary_malware_blocking", "DNS: {} · Malware blocking"},
      {"dns_summary_family", "DNS: {} · Family filter"},
      {"dns_summary_no_filtering", "DNS: {} · No filtering"},
      {"dns_summary_personal", "DNS: {} · your profile"},
      {"dns_summary_custom", "DNS: Custom"},
      {"dns_summary_custom_unencrypted", "DNS: Custom (unencrypted)"},
      {"dns_summary_regional", "DNS: Regional (unencrypted)"},
      // the status line, which takes the provider, and the line under it
      {"dns_status_provider", "DNS: {}"},
      {"dns_status_paused", "Ad blocking paused: {} isn't answering"},
      {"dns_status_paused_detail",
       "Using URnetwork DNS until it answers. Ad blocking comes back on its own."},
      {"dns_status_fallback", "{} isn't answering. Using URnetwork DNS."},
      {"dns_status_unreachable", "{} isn't answering"},
      {"dns_status_unreachable_detail",
       "To keep filtering on, URnetwork doesn't switch DNS for you. Sites may not load until it "
       "answers."},
      // what verification found out, each taking the provider, and the
      // dashboard link beside an unconfirmed profile
      {"dns_verify_not_filtering", "{} isn't filtering from this location"},
      {"dns_verify_unconfirmed",
       "Saved, but we can't confirm this profile. If the ID is wrong, {} answers without "
       "filtering."},
      {"dns_verify_rejected_controld", "{} doesn't recognize this resolver ID"},
      {"dns_verify_malformed_adguard", "{} rejected this device ID"},
      {"dns_personal_id_dashboard_link", "Open your {} dashboard"},
      // a personal preset's field: where the provider shows the id, its format
      // (the provider and its example), and its free plan's limit
      {"dns_personal_id_where_nextdns", "Find it on the Setup page of your {} account."},
      {"dns_personal_id_where_controld",
       "In your {} dashboard, choose Show Resolvers on your endpoint."},
      {"dns_personal_id_where_adguard", "Find it on your device's page in your {} dashboard."},
      {"dns_personal_id_format_nextdns",
       "{0} IDs are 6 characters: digits and the letters a to f, like {1}."},
      {"dns_personal_id_format_controld", "{0} resolver IDs use only letters and digits, like {1}."},
      {"dns_personal_id_format_adguard",
       "{0} device IDs are 8 characters: digits and the letters a to f, like {1}."},
      {"dns_personal_id_quota_nextdns",
       "{}'s free plan stops filtering after 300,000 queries a month."},
      {"dns_personal_id_quota_adguard", "{}'s free plan is limited to 300,000 requests a month."},
  };
  for (const Text& text : kTexts) {
    if (text.key == key) return text;
  }
  return {};
}

// The sdk's error ids of a selection and a personal id (DnsPresetError*,
// ValidateDnsPresetPersonalId), named as the sdk header names them.
inline constexpr const char* kErrorUnknown = "dns_preset_error_unknown";
inline constexpr const char* kErrorPersonalIdRequired = "dns_preset_error_personal_id_required";
inline constexpr const char* kErrorPersonalIdInvalid = "dns_preset_error_personal_id_invalid";
inline constexpr const char* kErrorPersonalIdOtherProvider =
    "dns_preset_error_personal_id_other_provider";
inline constexpr const char* kErrorPersonalIdNotAccepted =
    "dns_preset_error_personal_id_not_accepted";

// The message of an sdk error id. An id this build does not know -- the C
// ABI's internal error id for a call that could not run, or an id of a newer
// sdk -- is no refusal of what the user typed, so it reads as the generic
// message: never a raw key. No copy of the internal id is kept here: the SDK
// header is its one source (tests/ErrorIdInternalTest.cpp).
inline Text ErrorText(std::string_view errorId) {
  static constexpr Text kErrors[] = {
      {kErrorUnknown, "This preset isn't available. Choose another one."},
      {kErrorPersonalIdRequired, "Enter your profile ID."},
      {kErrorPersonalIdInvalid, "This ID isn't in the right format."},
      {kErrorPersonalIdOtherProvider,
       "This address belongs to another adblock provider. Choose that adblock provider instead."},
      {kErrorPersonalIdNotAccepted, "This preset doesn't use a profile ID."},
  };
  for (const Text& error : kErrors) {
    if (error.key == errorId) return error;
  }
  return {"something_went_wrong", "Something went wrong."};
}

// The pill over the DNS card (DnsSelectionSummary.PillKey): the regional
// recommendation's, which takes the country, or the defaults'. The sdk names
// the defaults' pill dns_pill_defaults, android's key; this app's catalog has
// the same words as dns_pill_default, as windows' has. An empty key is no
// pill.
inline Text PillText(std::string_view pillKey) {
  if (pillKey == "dns_pill_recommended") {
    return {"dns_pill_recommended", "There are unapplied recommended settings for {}"};
  }
  if (pillKey == "dns_pill_defaults") {
    return {"dns_pill_default", "The default safe settings are not applied"};
  }
  return {};
}

}  // namespace urnw::dns_presets
