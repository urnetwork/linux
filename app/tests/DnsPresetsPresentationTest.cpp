// The words of the third-party DNS presets (DnsPresetsPresentation.hpp, connect
// ADBLOCK3RD.md F1, F2). The DNS screens show every line the sdk describes by
// the key id it returns, so each id the sdk can return needs its English here,
// and that English must be the catalog's msgid byte for byte, or T_ misses and
// every locale reads English. An id this build does not know shows nothing, an
// error id it does not know reads as the generic message (never a raw key), and
// the defaults' pill, which the sdk names by android's key, reads from this
// app's own.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "DnsPresetsPresentation.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

namespace presets = urnw::dns_presets;

std::string ReadDnsPresetsCatalog() {
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The msgid of the catalog's entry for `key`, or "" when it has none.
std::string CatalogEnglish(const std::string& catalog, const std::string& key) {
  const std::string head = "msgctxt \"" + key + "\"\nmsgid \"";
  const size_t at = catalog.find(head);
  if (at == std::string::npos) return std::string();
  const size_t start = at + head.size();
  const size_t end = catalog.find("\"\n", start);
  if (end == std::string::npos) return std::string();
  return catalog.substr(start, end - start);
}

// Every key id the sdk returns to the DNS screens (sdk dns_presets_ui.go and
// the catalog in dns_presets.go, 9a0f19ae): group headings, the labels and
// lines of the picks, the summary rows, the status and verification lines, and
// a personal preset's notes.
const char* const kSdkKeys[] = {
    "block_ads_and_trackers",
    "dns_group_block_malware",
    "dns_group_family",
    "dns_group_my_account",
    "dns_group_no_filtering",
    "dns_provider_default",
    "dns_provider_default_copy",
    "dns_provider_custom",
    "dns_provider_custom_copy",
    "dns_provider_regional",
    "dns_recommendation_message",
    "dns_preset_copy_ads_phishing",
    "dns_preset_copy_ads_malware",
    "dns_preset_copy_ads_malware_phishing",
    "dns_preset_copy_malware",
    "dns_preset_copy_malware_phishing",
    "dns_preset_copy_ads_adult_safe_search",
    "dns_preset_copy_malware_adult",
    "dns_preset_copy_ads_malware_adult_drugs",
    "dns_preset_copy_child",
    "dns_preset_copy_child_ads",
    "dns_preset_copy_nothing",
    "dns_preset_copy_court_orders",
    "dns_preset_copy_personal",
    "dns_summary_default",
    "dns_summary_ad_blocking",
    "dns_summary_malware_blocking",
    "dns_summary_family",
    "dns_summary_no_filtering",
    "dns_summary_personal",
    "dns_summary_custom",
    "dns_summary_custom_unencrypted",
    "dns_summary_regional",
    "dns_status_provider",
    "dns_status_paused",
    "dns_status_paused_detail",
    "dns_status_fallback",
    "dns_status_unreachable",
    "dns_status_unreachable_detail",
    "dns_verify_not_filtering",
    "dns_verify_unconfirmed",
    "dns_verify_rejected_controld",
    "dns_verify_malformed_adguard",
    "dns_personal_id_dashboard_link",
    "dns_personal_id_where_nextdns",
    "dns_personal_id_where_controld",
    "dns_personal_id_where_adguard",
    "dns_personal_id_format_nextdns",
    "dns_personal_id_format_controld",
    "dns_personal_id_format_adguard",
    "dns_personal_id_quota_nextdns",
    "dns_personal_id_quota_adguard",
};

// The sdk's error ids (DnsPresetError*).
const char* const kErrorIds[] = {
    "dns_preset_error_unknown",
    "dns_preset_error_personal_id_required",
    "dns_preset_error_personal_id_invalid",
    "dns_preset_error_personal_id_other_provider",
    "dns_preset_error_personal_id_not_accepted",
};

}  // namespace

UR_TEST(DnsPresets_EverySdkKeyHasItsEnglish) {
  for (const char* key : kSdkKeys) {
    const presets::Text text = presets::KeyText(key);
    UR_EXPECT_TRUE_MSG(key, text.key == key);
    UR_EXPECT_TRUE_MSG(key, !text.english.empty());
  }
}

UR_TEST(DnsPresets_TheEnglishIsTheCatalogsMsgid) {
  const std::string catalog = ReadDnsPresetsCatalog();
  UR_EXPECT_TRUE_MSG("po/en.po was read", !catalog.empty());
  for (const char* key : kSdkKeys) {
    const presets::Text text = presets::KeyText(key);
    UR_EXPECT_TRUE_MSG(key, CatalogEnglish(catalog, key) == std::string(text.english));
  }
  for (const char* id : kErrorIds) {
    const presets::Text text = presets::ErrorText(id);
    UR_EXPECT_TRUE_MSG(id, CatalogEnglish(catalog, id) == std::string(text.english));
  }
  for (const char* pill : {"dns_pill_recommended", "dns_pill_defaults"}) {
    const presets::Text text = presets::PillText(pill);
    UR_EXPECT_TRUE_MSG(pill, CatalogEnglish(catalog, std::string(text.key)) ==
                                 std::string(text.english));
  }
}

// A key of a newer sdk shows nothing rather than its raw id.
UR_TEST(DnsPresets_AnUnknownKeyShowsNothing) {
  UR_EXPECT_TRUE(presets::KeyText("dns_summary_from_a_newer_sdk").key.empty());
  UR_EXPECT_TRUE(presets::KeyText("").key.empty());
}

UR_TEST(DnsPresets_EachErrorIdHasItsMessage) {
  for (const char* id : kErrorIds) {
    UR_EXPECT_TRUE_MSG(id, presets::ErrorText(id).key == id);
  }
  // an id this build does not know, the internal one included, is generic
  const presets::Text unknown = presets::ErrorText("dns_preset_error_from_a_newer_sdk");
  UR_EXPECT_TRUE(unknown.key == "something_went_wrong");
  UR_EXPECT_TRUE(unknown.english == "Something went wrong.");
  UR_EXPECT_TRUE(presets::ErrorText("").key == "something_went_wrong");
}

// The sdk names the defaults' pill dns_pill_defaults (android's key); this
// app's catalog carries the same words as dns_pill_default.
UR_TEST(DnsPresets_ThePillsReadFromThisAppsKeys) {
  UR_EXPECT_TRUE(presets::PillText("dns_pill_recommended").key == "dns_pill_recommended");
  UR_EXPECT_TRUE(presets::PillText("dns_pill_defaults").key == "dns_pill_default");
  UR_EXPECT_TRUE(presets::PillText("").key.empty());
  UR_EXPECT_TRUE(presets::PillText("dns_pill_from_a_newer_sdk").key.empty());
}

// Provider names reach the copy only through a placeholder, so the catalog's
// translations cannot change them; a personal id's format message takes the
// provider and its example in order.
UR_TEST(DnsPresets_TheProviderIsAPlaceholder) {
  for (const char* key : {"dns_summary_ad_blocking", "dns_status_paused", "dns_status_provider",
                          "dns_verify_unconfirmed", "dns_preset_copy_personal",
                          "dns_personal_id_where_nextdns", "dns_personal_id_quota_adguard"}) {
    UR_EXPECT_TRUE_MSG(key, presets::KeyText(key).english.find("{}") != std::string::npos);
  }
  for (const char* key : {"dns_personal_id_format_nextdns", "dns_personal_id_format_controld",
                          "dns_personal_id_format_adguard"}) {
    const std::string english(presets::KeyText(key).english);
    UR_EXPECT_TRUE_MSG(key, english.find("{0}") != std::string::npos &&
                                english.find("{1}") != std::string::npos);
  }
}

// The help link is ur.io's DNS providers section, unlocalized as every ur.io
// link in the app is.
UR_TEST(DnsPresets_TheHelpLinkIsTheDnsProvidersSection) {
  UR_EXPECT_TRUE(std::string(presets::kHelpUrl) == "https://ur.io/docs/faq#dns-providers");
}
