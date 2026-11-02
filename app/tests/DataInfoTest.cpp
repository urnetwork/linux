// The "About your data" sheet (DataInfo.hpp): when the free data refreshes
// (the next 00:00 UTC, whatever the machine's zone), the countdown to it, the
// Used, Pending and Available split from a balance, the daily amount from the
// server's start_balance_byte_count, and when each entry point shows. The
// wiring cases read the sources the GUI build compiles.
//
// SPDX-License-Identifier: MPL-2.0
#include "DataInfo.hpp"
#include "TestHarness.hpp"

#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::data_info::AlertShowsFreeRefresh;
using urnw::data_info::DataInfoFrom;
using urnw::data_info::FormatRefreshCountdown;
using urnw::data_info::FreeRefreshCountdown;
using urnw::data_info::MillisUntilCountdownChanges;
using urnw::data_info::NextFreeRefreshMillis;
using urnw::data_info::RefreshCountdown;
using urnw::data_info::ShowsFreeRefresh;
using urnw::data_info::UpgradeShowsFreeRefresh;

constexpr int64_t kSecond = 1000;
constexpr int64_t kMinute = 60 * kSecond;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kDay = 24 * kHour;
constexpr int64_t kGib = 1024LL * 1024 * 1024;

// days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), so the cases read as UTC wall-clock times
constexpr int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

constexpr int64_t Utc(int64_t y, int64_t m, int64_t d, int64_t hour = 0, int64_t minute = 0,
                      int64_t second = 0, int64_t millis = 0) {
  return DaysFromCivil(y, m, d) * kDay + hour * kHour + minute * kMinute + second * kSecond +
         millis;
}

void ExpectText(const std::string& expected, const std::string& actual, const std::string& what) {
  if (expected != actual) {
    UR_FAIL(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

void ExpectCountdown(RefreshCountdown expected, RefreshCountdown actual, const std::string& what) {
  if (!(expected == actual)) {
    UR_FAIL(what + ": expected " + std::to_string(expected.hours) + "h " +
            std::to_string(expected.minutes) + "m, got " + std::to_string(actual.hours) + "h " +
            std::to_string(actual.minutes) + "m");
  }
}

// the provider_connected_duration_hours / _minutes English msgids
std::string HoursAndMinutes(int64_t hours, int64_t minutes) {
  return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
}

std::string MinutesOnly(int64_t minutes) { return std::to_string(minutes) + "m"; }

std::string Label(int64_t nowMillis) {
  return FormatRefreshCountdown(FreeRefreshCountdown(nowMillis), HoursAndMinutes, MinutesOnly);
}

std::string Gib(int64_t bytes) { return std::to_string(bytes / kGib) + " GiB"; }

std::string Raw(int64_t bytes) { return std::to_string(bytes); }

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The body of the definition that starts with `signature`, up to the first
// closing brace at the start of a line.
std::string Body(const std::string& source, const std::string& signature) {
  const auto start = source.find(signature);
  if (start == std::string::npos) return {};
  const auto end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

UR_TEST(DataInfo_NextRefreshIsTheNextUtcMidnight) {
  UR_EXPECT_EQ(Utc(2026, 10, 5), NextFreeRefreshMillis(Utc(2026, 10, 4, 12, 34, 56)));
  UR_EXPECT_EQ(Utc(2026, 10, 5), NextFreeRefreshMillis(Utc(2026, 10, 4, 0, 0, 0, 1)));
  // a clock before 1970 still lands on a UTC midnight
  UR_EXPECT_EQ(0, NextFreeRefreshMillis(-1));
  UR_EXPECT_EQ(-kDay, NextFreeRefreshMillis(-kDay - 1));
}

UR_TEST(DataInfo_NextRefreshAroundMidnight) {
  // just before midnight the refresh is a millisecond away
  UR_EXPECT_EQ(Utc(2026, 10, 5), NextFreeRefreshMillis(Utc(2026, 10, 4, 23, 59, 59, 999)));
  // at midnight it has just happened: the next one is a day away
  UR_EXPECT_EQ(Utc(2026, 10, 6), NextFreeRefreshMillis(Utc(2026, 10, 5)));
  UR_EXPECT_EQ(Utc(2026, 10, 6), NextFreeRefreshMillis(Utc(2026, 10, 5, 0, 0, 0, 1)));
}

UR_TEST(DataInfo_NextRefreshRollsOverMonthsYearsAndLeapDays) {
  UR_EXPECT_EQ(Utc(2026, 11, 1), NextFreeRefreshMillis(Utc(2026, 10, 31, 18)));
  UR_EXPECT_EQ(Utc(2027, 1, 1), NextFreeRefreshMillis(Utc(2026, 12, 31, 23, 30)));
  UR_EXPECT_EQ(Utc(2028, 2, 29), NextFreeRefreshMillis(Utc(2028, 2, 28, 10)));
  UR_EXPECT_EQ(Utc(2028, 3, 1), NextFreeRefreshMillis(Utc(2028, 2, 29, 10)));
}

UR_TEST(DataInfo_CountdownInHoursAndMinutes) {
  ExpectCountdown({5, 12}, FreeRefreshCountdown(Utc(2026, 10, 4, 18, 48)), "18:48 UTC");
  ExpectCountdown({23, 59}, FreeRefreshCountdown(Utc(2026, 10, 4, 0, 1)), "00:01 UTC");
  // 20:15 UTC is 3h 45m away whatever the local zone: the input is epoch time
  ExpectCountdown({3, 45}, FreeRefreshCountdown(Utc(2026, 10, 4, 20, 15)), "20:15 UTC");
  ExpectCountdown({5, 13}, FreeRefreshCountdown(Utc(2026, 10, 4, 18, 47, 30)),
                  "a partial minute rounds up");
}

UR_TEST(DataInfo_CountdownAroundMidnight) {
  // just before midnight it never reads zero
  ExpectCountdown({0, 1}, FreeRefreshCountdown(Utc(2026, 10, 4, 23, 59, 59, 999)), "23:59:59.999");
  ExpectCountdown({0, 1}, FreeRefreshCountdown(Utc(2026, 10, 4, 23, 59)), "23:59");
  ExpectCountdown({0, 2}, FreeRefreshCountdown(Utc(2026, 10, 4, 23, 58, 59, 999)), "23:58:59.999");
  // at and just after midnight it rolls over to the next day's refresh
  ExpectCountdown({24, 0}, FreeRefreshCountdown(Utc(2026, 10, 5)), "00:00");
  ExpectCountdown({24, 0}, FreeRefreshCountdown(Utc(2026, 10, 5, 0, 0, 0, 1)), "00:00:00.001");
  ExpectCountdown({23, 59}, FreeRefreshCountdown(Utc(2026, 10, 5, 0, 1)), "00:01 the next day");
}

UR_TEST(DataInfo_CountdownTicksOncePerDisplayedMinute) {
  UR_EXPECT_EQ(kMinute, MillisUntilCountdownChanges(Utc(2026, 10, 4, 18, 48)));
  UR_EXPECT_EQ(30 * kSecond, MillisUntilCountdownChanges(Utc(2026, 10, 4, 18, 47, 30)));
  UR_EXPECT_EQ(1, MillisUntilCountdownChanges(Utc(2026, 10, 4, 23, 59, 59, 999)));
  for (int64_t now : {Utc(2026, 10, 4, 18, 48), Utc(2026, 10, 4, 18, 47, 30, 250),
                      Utc(2026, 10, 4, 23, 59, 59, 999), Utc(2026, 10, 4, 23, 59),
                      Utc(2026, 10, 5)}) {
    const int64_t wake = MillisUntilCountdownChanges(now);
    UR_EXPECT_TRUE(FreeRefreshCountdown(now) == FreeRefreshCountdown(now + wake - 1));
    UR_EXPECT_FALSE(FreeRefreshCountdown(now) == FreeRefreshCountdown(now + wake));
  }
}

UR_TEST(DataInfo_CountdownReadsAsACompactDuration) {
  ExpectText("5h 12m", Label(Utc(2026, 10, 4, 18, 48)), "hours and minutes");
  ExpectText("1h 0m", Label(Utc(2026, 10, 4, 23)), "a whole hour");
  ExpectText("59m", Label(Utc(2026, 10, 4, 23, 1)), "the last hour");
  ExpectText("1m", Label(Utc(2026, 10, 4, 23, 59, 59, 999)), "the last minute");
  ExpectText("24h 0m", Label(Utc(2026, 10, 5)), "at midnight");
}

UR_TEST(DataInfo_UsedIsWhatIsNeitherAvailableNorPending) {
  const auto info = DataInfoFrom(30 * kGib, 20 * kGib, 2 * kGib, Gib);
  ExpectText("8 GiB", info.used, "used");
  ExpectText("2 GiB", info.pending, "pending");
  ExpectText("20 GiB", info.available, "available");
  ExpectText("30 GiB", info.daily, "daily");
  // the reported case: Pending fills the bar and nothing is available
  const auto pending = DataInfoFrom(30 * kGib, 0, 29 * kGib, Gib);
  ExpectText("1 GiB", pending.used, "all pending: used");
  ExpectText("29 GiB", pending.pending, "all pending: pending");
  ExpectText("0 GiB", pending.available, "all pending: available");
}

UR_TEST(DataInfo_UsedNeverGoesNegative) {
  // the server samples the values independently
  const auto sampled = DataInfoFrom(kGib, kGib, kGib / 2, Raw);
  ExpectText("0", sampled.used, "used clamps at 0");
  ExpectText(Raw(kGib / 2), sampled.pending, "pending as reported");
  const auto negative = DataInfoFrom(0, -1, -1, Raw);
  ExpectText("0", negative.pending, "negative pending clamps");
  ExpectText("0", negative.available, "negative available clamps");
}

UR_TEST(DataInfo_DailyAmountIsTheServersStartBalance) {
  // never a hard-coded allowance: whatever start_balance_byte_count says
  for (int64_t start : {30 * kGib, 60 * kGib, 33 * kGib, 10 * 1024 * kGib, int64_t{12345678901}}) {
    ExpectText(Raw(start), DataInfoFrom(start, 0, 0, Raw).daily, "daily " + Raw(start));
  }
}

UR_TEST(DataInfo_WhenEachEntryPointShows) {
  UR_EXPECT_TRUE(ShowsFreeRefresh(false));
  // Pro gets no free daily grant
  UR_EXPECT_FALSE(ShowsFreeRefresh(true));
  // the Connect page alert leads with the refresh whenever it shows
  UR_EXPECT_TRUE(AlertShowsFreeRefresh(true));
  UR_EXPECT_FALSE(AlertShowsFreeRefresh(false));
  // the upgrade sheet only when a blocked connect opened it, never for Pro
  UR_EXPECT_TRUE(UpgradeShowsFreeRefresh(true, false));
  UR_EXPECT_FALSE(UpgradeShowsFreeRefresh(false, false));
  UR_EXPECT_FALSE(UpgradeShowsFreeRefresh(true, true));
}

// Only the start-connect block marks the upgrade sheet: the window sets its
// mark around that one OpenUpgrade, and its opening of the sheet reads it.
UR_TEST(DataInfo_OnlyTheStartConnectBlockMarksTheUpgradeSheet) {
  const std::string window = ReadSource("MainWindow.cpp");
  const std::string gate =
      Body(window, "bool MainWindow::ConnectBlockedByBalance(std::function<void()> retry) {");
  const std::string mark =
      "nextUpgradeFreeRefresh_ = data_info::UpgradeShowsFreeRefresh(true, balance_.IsPro());";
  const auto markAt = gate.find(mark);
  const auto openAt = gate.find("OpenUpgrade();");
  const auto clearAt = gate.find("nextUpgradeFreeRefresh_ = false;");
  UR_EXPECT_TRUE(markAt != std::string::npos);
  UR_EXPECT_TRUE(markAt < openAt);
  UR_EXPECT_TRUE(openAt != std::string::npos && openAt < clearAt && clearAt != std::string::npos);
  // nothing else marks it: Account's Upgrade, the alert's Upgrade and the
  // onboarding link open the sheet without the refresh line
  size_t writes = 0;
  for (size_t at = window.find("nextUpgradeFreeRefresh_ = "); at != std::string::npos;
       at = window.find("nextUpgradeFreeRefresh_ = ", at + 1)) {
    ++writes;
  }
  UR_EXPECT_EQ(size_t{2}, writes);
  UR_EXPECT_TRUE(Contains(Body(window, "void MainWindow::OpenUpgrade() {"),
                          "upgradeSheet_->Open(nextUpgradeFreeRefresh_);"));
  const std::string sheet = ReadSource("UpgradeSheet.cpp");
  const std::string open = Body(sheet, "void UpgradeSheet::Open(bool freeRefresh) {");
  UR_EXPECT_TRUE(Contains(open, "freeRefreshBox_->set_visible(freeRefresh);"));
  UR_EXPECT_TRUE(Contains(open, "T_(\"insufficient_balance_refreshes_in\", \"Free data refreshes in {}.\")"));
  UR_EXPECT_TRUE(Contains(Body(sheet, "void UpgradeSheet::BuildUi() {"),
                          "T_(\"wait_for_refresh\", \"Wait for refresh\")"));
}

// The Connect page alert leads with the refresh and links Why? to the sheet;
// Account's data-usage group opens it too; the sheet reads the server's
// balance and leaves the refresh line out for Pro.
UR_TEST(DataInfo_EntryPointsReachTheSheet) {
  const std::string page = ReadSource("ConnectPage.cpp");
  const auto alertAt = page.find("heldAlert_ = Gtk::make_managed");
  const auto endAt = page.find("paneAContent_->append(*heldAlert_)", alertAt);
  UR_EXPECT_TRUE(alertAt != std::string::npos && endAt != std::string::npos);
  const std::string alert = page.substr(alertAt, endAt - alertAt);
  UR_EXPECT_TRUE(Contains(alert, "T_(\"data_info_why\", \"Why?\")"));
  UR_EXPECT_TRUE(Contains(alert, "on_open_data_info()"));
  // the refresh line comes before the held text
  UR_EXPECT_TRUE(alert.find("freeRefreshLabel_") < alert.find("heldAlertTraffic_"));
  UR_EXPECT_TRUE(Contains(
      Body(page, "void ConnectPage::ApplyBalanceNotice(const balance_notice::Signals& signals) {"),
      "data_info::AlertShowsFreeRefresh(balance_notice::HeldAlert(signals))"));
  const std::string account = ReadSource("AccountPage.cpp");
  UR_EXPECT_TRUE(Contains(account, "T_(\"data_info_title\", \"About your data\")"));
  UR_EXPECT_TRUE(Contains(account, "on_open_data_info()"));
  const std::string window = ReadSource("MainWindow.cpp");
  UR_EXPECT_TRUE(Contains(window, "connectPage_->on_open_data_info = [this] { OpenDataInfo(); };"));
  UR_EXPECT_TRUE(Contains(window, "accountPage_->on_open_data_info = [this] { OpenDataInfo(); };"));
  const std::string sheet = Body(ReadSource("DataInfoSheet.cpp"), "void DataInfoSheet::Open() {");
  UR_EXPECT_TRUE(Contains(sheet, "balance_.StartBalanceByteCount()"));
  UR_EXPECT_TRUE(Contains(sheet, "data_info::ShowsFreeRefresh(balance_.IsPro())"));
}

// T_ looks the copy up by msgctxt and msgid, so every English fallback the
// sheet's strings use must be the catalog's msgid exactly.
UR_TEST(DataInfo_EnglishMatchesTheCatalogMsgids) {
  const std::string catalog = ReadSource("../po/en.po");
  for (const auto& entry : std::initializer_list<std::pair<const char*, const char*>>{
           {"data_info_title", "About your data"},
           {"data_info_used", "Data you've used so far."},
           {"data_info_pending",
            "Data reserved for your open connections. What they don't use is returned when they close."},
           {"data_info_available", "Data you can still use."},
           {"data_info_refresh_at", "Free data refreshes daily at 00:00 UTC (in {})."},
           {"data_info_why", "Why?"},
           {"insufficient_balance_refreshes_in", "Free data refreshes in {}."},
           {"wait_for_refresh", "Wait for refresh"},
           {"provider_connected_duration_hours", "{0}h {1}m"},
           {"provider_connected_duration_minutes", "{}m"},
       }) {
    const std::string pair =
        std::string("msgctxt \"") + entry.first + "\"\nmsgid \"" + entry.second + "\"\n";
    if (catalog.find(pair) == std::string::npos) {
      UR_FAIL(std::string("po/en.po has no ") + entry.first + " with msgid \"" + entry.second + "\"");
    }
  }
}
