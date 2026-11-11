// A count in front of a noun ("3 months of Pro, free", "14 days free, then ...", "23 words",
// "5 providers") needs the noun's plural form for that count. These lines were looked up with
// T_ from one fixed string, so Russian read "1 месяца" and English "1 days free"; the
// offer terms line also passed "{} days ..." where the catalog's msgid is "{0} days ...", so
// it never matched a translation at all. They are plural catalog entries now, looked up with
// TN_ and the catalog's exact msgid / msgid_plural. The surfaces need GTK and the SDK, so this
// reads their sources and the catalog.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadCountedSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

struct CountedCopy {
  const char* file;
  const char* key;
  const char* one;
  const char* other;
};

const CountedCopy kCounted[] = {
    {"Onboarding.cpp", "offer_months_free_headline", "{} month of Pro, free",
     "{} months of Pro, free"},
    {"Onboarding.cpp", "offer_cta_start_trial_months_free", "Start free trial with {} month free",
     "Start free trial with {} months free"},
    {"OfferCard.cpp", "offer_terms_first_year",
     "{0} day free, then {1} for your first year, then {2}/year. Cancel anytime.",
     "{0} days free, then {1} for your first year, then {2}/year. Cancel anytime."},
    {"MainWindow.cpp", "seedphrase_word_count_warning",
     "That's {} word — a seedphrase is 12 or 24 words",
     "That's {} words — a seedphrase is 12 or 24 words"},
    {"LocationsSheet.cpp", "provider_count", "{} provider", "{} providers"},
    {"ConnectPage.cpp", "providing_client_count", "Providing to {} client",
     "Providing to {} clients"},
    // Claim now's period: the relay's 30 days today, any count tomorrow
    {"ClaimRelay.cpp", "sn_claim_relay_claim_now_paid", "Claim now is paid by URnetwork, once a day.",
     "Claim now is paid by URnetwork, once every {} days."},
    {"ClaimRelay.cpp", "sn_claim_relay_claim_now_paid_each",
     "Paid by URnetwork, once a day for each coldkey.",
     "Paid by URnetwork, once every {} days for each coldkey."},
    // when the next Claim now opens, under a queued one: the relay's period again
    {"ClaimRelay.cpp", "sn_claim_relay_requested_period",
     "Your next Claim now opens a day after this claim goes through.",
     "Your next Claim now opens {} days after this claim goes through."},
};

UR_TEST(CountedCopyIsLookedUpAsAPlural) {
  for (const CountedCopy& copy : kCounted) {
    const std::string source = ReadCountedSource(copy.file);
    UR_EXPECT_TRUE(!source.empty());
    const size_t at = source.find(std::string("TN_(\"") + copy.key + "\"");
    UR_EXPECT_TRUE(at != std::string::npos);
    if (at == std::string::npos) continue;
    const std::string call = source.substr(at, 400);
    UR_EXPECT_TRUE(call.find(std::string("\"") + copy.one + "\"") != std::string::npos);
    UR_EXPECT_TRUE(call.find(std::string("\"") + copy.other + "\"") != std::string::npos);
    UR_EXPECT_TRUE(source.find(std::string("T_(\"") + copy.key + "\"") == std::string::npos);
  }
  UR_EXPECT_TRUE(ReadCountedSource("LocationsSheet.cpp").find("\"provider_count_int\"") ==
                 std::string::npos);
}

UR_TEST(CountedCopyMatchesThePluralCatalogEntry) {
  const std::string catalog = ReadCountedSource("../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  for (const CountedCopy& copy : kCounted) {
    const std::string entry = std::string("msgctxt \"") + copy.key + "\"\nmsgid \"" + copy.one +
                              "\"\nmsgid_plural \"" + copy.other + "\"\n";
    UR_EXPECT_TRUE(catalog.find(entry) != std::string::npos);
  }
}

UR_TEST(RussianCountedCopyHasEachNounForm) {
  const std::string catalog = ReadCountedSource("../po/ru.po");
  UR_EXPECT_TRUE(catalog.find("msgstr[0] \"{} месяц Pro бесплатно\"\n"
                              "msgstr[1] \"{} месяца Pro бесплатно\"\n"
                              "msgstr[2] \"{} месяцев Pro бесплатно\"") != std::string::npos);
  UR_EXPECT_TRUE(catalog.find("msgstr[0] \"Это {} слово — сид-фраза состоит из 12 или 24 слов\"\n"
                              "msgstr[1] \"Это {} слова — сид-фраза состоит из 12 или 24 слов\"\n"
                              "msgstr[2] \"Это {} слов — сид-фраза состоит из 12 или 24 слов\"") !=
                 std::string::npos);
}

}  // namespace
