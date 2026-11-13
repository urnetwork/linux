// The SN payout line under the points on the Earnings page
// (SnPayoutPresentation.hpp): which line shows (no coldkey, nothing claimable,
// claimable), and the current epoch's end, claim-open and expiry times
// formatted from the SDK's epoch schedule. The page needs GTK and the SDK, so
// its wiring and its English fallbacks (which must be the catalog's msgids, or
// every locale shows the stale English) are read from the sources and
// po/en.po.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "SnPayoutPresentation.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

using urnw::snpayout::EpochSchedule;
using urnw::snpayout::FormatScheduleTime;
using urnw::snpayout::LineKind;
using urnw::snpayout::PayoutLineFor;
using urnw::snpayout::PayoutLineView;

namespace {

// The SDK's schedule for an epoch closing 2026-10-13 00:00 UTC on the mainnet
// policy: claims open 14,400 blocks (two days) later, and the share expires at
// the end of epoch e+9 (453,600 blocks after the close, less one block).
constexpr int64_t kEnd = 1'791'849'600'000;
constexpr int64_t kClaimOpen = kEnd + 14'400LL * 12'000;
constexpr int64_t kExpiry = kEnd + (453'600LL - 1) * 12'000;
// 2026-10-06 00:00 UTC, a week before the close
constexpr int64_t kNow = 1'791'244'800'000;

EpochSchedule Schedule() {
  EpochSchedule schedule;
  schedule.epoch = 9;
  schedule.endMillis = kEnd;
  schedule.claimOpenMillis = kClaimOpen;
  schedule.expiryMillis = kExpiry;
  return schedule;
}

// names each time by the millis it was formatted from
std::string Tag(int64_t millis) { return "t" + std::to_string(millis); }

PayoutLineView Line(bool walletKnown, bool hasColdkey, int64_t claimableRao,
                    std::optional<EpochSchedule> schedule, int64_t now = kNow) {
  return PayoutLineFor(walletKnown, hasColdkey, claimableRao, schedule, now, Tag);
}

bool HasTaggedTimes(const PayoutLineView& view) {
  return view.showTimes && view.epochEnd == Tag(kEnd) && view.claimOpen == Tag(kClaimOpen) &&
         view.expiry == Tag(kExpiry);
}

UR_TEST(SnPayoutNoColdkeyAsksForOne) {
  for (const int64_t rao : {int64_t{0}, int64_t{3'241'000'000}}) {
    const PayoutLineView view = Line(true, false, rao, Schedule());
    UR_EXPECT_TRUE(view.kind == LineKind::SetColdkey);
    UR_EXPECT_FALSE(view.showTimes);
    UR_EXPECT_FALSE(view.showClaim);
  }
  UR_EXPECT_TRUE(Line(true, false, 0, std::nullopt).kind == LineKind::SetColdkey);
}

UR_TEST(SnPayoutNothingShowsWhileTheColdkeyIsUnknown) {
  UR_EXPECT_TRUE(Line(false, false, 0, Schedule()) == PayoutLineView{});
  UR_EXPECT_TRUE(Line(false, true, 3'241'000'000, Schedule()) == PayoutLineView{});
}

UR_TEST(SnPayoutNothingClaimableExplainsTheScheduleWithoutClaim) {
  const PayoutLineView view = Line(true, true, 0, Schedule());
  UR_EXPECT_TRUE(view.kind == LineKind::Schedule);
  UR_EXPECT_TRUE(HasTaggedTimes(view));
  UR_EXPECT_FALSE(view.showClaim);
}

UR_TEST(SnPayoutSomethingClaimableAddsClaim) {
  const PayoutLineView view = Line(true, true, 3'241'000'000, Schedule());
  UR_EXPECT_TRUE(view.kind == LineKind::Schedule);
  UR_EXPECT_TRUE(HasTaggedTimes(view));
  UR_EXPECT_TRUE(view.showClaim);
}

UR_TEST(SnPayoutWithoutTheScheduleTheExplanationStandsAlone) {
  const PayoutLineView view = Line(true, true, 1, std::nullopt);
  UR_EXPECT_TRUE(view.kind == LineKind::Schedule);
  UR_EXPECT_FALSE(view.showTimes);
  UR_EXPECT_TRUE(view.epochEnd.empty() && view.claimOpen.empty() && view.expiry.empty());
  UR_EXPECT_TRUE(view.showClaim);
}

UR_TEST(SnPayoutAnEndedEpochWaitsForTheNextRead) {
  UR_EXPECT_FALSE(Line(true, true, 0, Schedule(), kEnd).showTimes);
  UR_EXPECT_TRUE(HasTaggedTimes(Line(true, true, 0, Schedule(), kEnd - 1)));
}

UR_TEST(SnPayoutTimesAreLocalDatesAndTimesFromTheEpochData) {
  UR_EXPECT_TRUE(FormatScheduleTime(kEnd, 0) == "2026-10-13 00:00");
  UR_EXPECT_TRUE(FormatScheduleTime(kClaimOpen, 0) == "2026-10-15 00:00");
  UR_EXPECT_TRUE(FormatScheduleTime(kExpiry, 0) == "2026-12-14 23:59");
  // Berlin in summer time, then in winter time by the expiry
  UR_EXPECT_TRUE(FormatScheduleTime(kEnd, 120) == "2026-10-13 02:00");
  UR_EXPECT_TRUE(FormatScheduleTime(kExpiry, 60) == "2026-12-15 00:59");
  // west of UTC the close is the evening before
  UR_EXPECT_TRUE(FormatScheduleTime(kEnd, -420) == "2026-10-12 17:00");
  UR_EXPECT_TRUE(FormatScheduleTime(-60'000, 0) == "1969-12-31 23:59");

  const PayoutLineView view = PayoutLineFor(
      true, true, 0, Schedule(), kNow,
      [](int64_t millis) { return FormatScheduleTime(millis, 120); });
  UR_EXPECT_TRUE(view.epochEnd == "2026-10-13 02:00");
  UR_EXPECT_TRUE(view.claimOpen == "2026-10-15 02:00");
  UR_EXPECT_TRUE(view.expiry == "2026-12-15 01:59");
}

// ---- the page's wiring and English, read from the sources ----------------------

std::string ReadSnPayoutSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Every English fallback passed to T_("key", ...) in `source`: the adjacent
// string literals after the key, joined.
std::vector<std::string> Fallbacks(const std::string& source, const std::string& key) {
  std::vector<std::string> out;
  const std::string open = "T_(\"" + key + "\",";
  for (size_t at = source.find(open); at != std::string::npos; at = source.find(open, at + 1)) {
    std::string text;
    size_t p = at + open.size();
    while (p < source.size()) {
      while (p < source.size() && (source[p] == ' ' || source[p] == '\n')) ++p;
      if (p >= source.size() || source[p] != '"') break;
      for (++p; p < source.size() && source[p] != '"'; ++p) {
        if (source[p] == '\\' && p + 1 < source.size()) ++p;
        text += source[p];
      }
      ++p;
    }
    out.push_back(text);
  }
  return out;
}

UR_TEST(SnPayoutPageDrawsTheDecisionWithTheClaimsSchedule) {
  const std::string page = ReadSnPayoutSource("EarningsPage.cpp");
  UR_EXPECT_TRUE(!page.empty());
  UR_EXPECT_TRUE(page.find("snpayout::PayoutLineFor(") != std::string::npos);
  UR_EXPECT_TRUE(page.find("result->schedule->claim_open_millis") != std::string::npos);
  // the actions are the existing coldkey flow and claim sheet
  UR_EXPECT_TRUE(page.find("setColdkeyButton_->signal_clicked().connect(\n"
                           "        sigc::mem_fun(*this, &EarningsPage::OnConnectWithBridge));") !=
                 std::string::npos);
  UR_EXPECT_TRUE(page.find("payoutClaimButton_->signal_clicked().connect(sigc::mem_fun(*this, "
                           "&EarningsPage::OnClaim));") != std::string::npos);
}

UR_TEST(SnPayoutEnglishMatchesTheCatalogMsgids) {
  const std::string page = ReadSnPayoutSource("EarningsPage.cpp");
  const std::string catalog = ReadSnPayoutSource("../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  for (const char* key : {"sn_payout_schedule", "sn_payout_schedule_times",
                          "set_coldkey_to_get_paid", "set_coldkey",
                          "claims_open_after_finalization"}) {
    const std::vector<std::string> fallbacks = Fallbacks(page, key);
    if (fallbacks.empty()) UR_FAIL(std::string("EarningsPage.cpp does not show ") + key);
    for (const std::string& english : fallbacks) {
      const std::string entry =
          std::string("msgctxt \"") + key + "\"\nmsgid \"" + english + "\"\n";
      if (catalog.find(entry) == std::string::npos) {
        UR_FAIL(std::string(key) + ": \"" + english + "\" is not the catalog's msgid");
      }
    }
  }
  UR_EXPECT_TRUE(catalog.find("msgctxt \"payouts_amount_threshold\"") == std::string::npos);
  UR_EXPECT_TRUE(catalog.find("Claims open 48 hours") == std::string::npos);
}

}  // namespace
