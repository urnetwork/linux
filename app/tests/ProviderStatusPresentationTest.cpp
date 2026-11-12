// The network's provider status for this device on the Earnings page
// (ProviderStatusPresentation.hpp): the 60 bars of the demand histogram (the
// fractions, the zero maximum, the total, empty detection), the line under the
// provide mode row (local reasons first, then the server's, "none" with and
// without NoTrafficYet, an unknown code falling back to the server's text),
// the "Why?" rows for every number name (label, value text, passes; unknown
// names skipped, missing values, the country last), and the states of the
// demand area.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ProviderStatusPresentation.hpp"

using urnw::providerstatus::ChartState;
using urnw::providerstatus::Country;
using urnw::providerstatus::Histogram;
using urnw::providerstatus::HistogramFor;
using urnw::providerstatus::kHistogramBars;
using urnw::providerstatus::LineKind;
using urnw::providerstatus::NumberLabel;
using urnw::providerstatus::NumberRow;
using urnw::providerstatus::NumberRowsFor;
using urnw::providerstatus::NumberWords;
using urnw::providerstatus::Poll;
using urnw::providerstatus::ProviderIdleReason;
using urnw::providerstatus::RankingNumber;
using urnw::providerstatus::ServerReason;
using urnw::providerstatus::ServerReasonOf;
using urnw::providerstatus::StatusLine;
using urnw::providerstatus::StatusLineFor;
using urnw::providerstatus::StatusView;
using urnw::providerstatus::StatusViewFor;

namespace {

// ---- the histogram -------------------------------------------------------------

std::vector<int64_t> Minutes(int64_t fill) { return std::vector<int64_t>(kHistogramBars, fill); }

UR_TEST(ProviderStatusHistogramScalesEveryBarToTheLargestCount) {
  std::vector<int64_t> counts = Minutes(0);
  counts[0] = 2;   // the oldest minute
  counts[30] = 8;  // the busiest
  counts[59] = 4;  // the current, partial minute
  const Histogram histogram = HistogramFor(counts);
  UR_EXPECT_EQ(kHistogramBars, histogram.fractions.size());
  UR_EXPECT_NEAR(0.25, histogram.fractions[0], 1e-9);
  UR_EXPECT_NEAR(1.0, histogram.fractions[30], 1e-9);
  UR_EXPECT_NEAR(0.5, histogram.fractions[59], 1e-9);
  UR_EXPECT_NEAR(0.0, histogram.fractions[1], 1e-9);
  UR_EXPECT_EQ(14, histogram.total);
  UR_EXPECT_FALSE(histogram.empty);
}

UR_TEST(ProviderStatusHistogramAllZeroIsEmptyWithNoBars) {
  const Histogram histogram = HistogramFor(Minutes(0));
  UR_EXPECT_EQ(kHistogramBars, histogram.fractions.size());
  for (const double fraction : histogram.fractions) UR_EXPECT_NEAR(0.0, fraction, 1e-12);
  UR_EXPECT_EQ(0, histogram.total);
  UR_EXPECT_TRUE(histogram.empty);
  // no counts at all reads the same, with all 60 bars present
  const Histogram none = HistogramFor({});
  UR_EXPECT_EQ(kHistogramBars, none.fractions.size());
  UR_EXPECT_TRUE(none.empty);
}

UR_TEST(ProviderStatusHistogramAlwaysDrawsSixtyBarsNewestOnTheRight) {
  // a short answer is right-aligned: the newest count is the last bar
  const Histogram shorter = HistogramFor({3, 6});
  UR_EXPECT_EQ(kHistogramBars, shorter.fractions.size());
  UR_EXPECT_NEAR(0.5, shorter.fractions[58], 1e-9);
  UR_EXPECT_NEAR(1.0, shorter.fractions[59], 1e-9);
  UR_EXPECT_NEAR(0.0, shorter.fractions[0], 1e-9);
  UR_EXPECT_EQ(9, shorter.total);
  // a long one keeps the newest 60
  std::vector<int64_t> longer = Minutes(1);
  longer.insert(longer.begin(), 1000);  // a 61st, older minute is dropped
  const Histogram kept = HistogramFor(longer);
  UR_EXPECT_EQ(kHistogramBars, kept.fractions.size());
  UR_EXPECT_EQ(60, kept.total);
  UR_EXPECT_NEAR(1.0, kept.fractions[0], 1e-9);
  // a negative count reads as zero
  std::vector<int64_t> negative = Minutes(0);
  negative[10] = -5;
  negative[20] = 5;
  const Histogram clamped = HistogramFor(negative);
  UR_EXPECT_NEAR(0.0, clamped.fractions[10], 1e-9);
  UR_EXPECT_NEAR(1.0, clamped.fractions[20], 1e-9);
  UR_EXPECT_EQ(5, clamped.total);
}

// ---- the line under the provide mode row ------------------------------------------

bool IsIdle(const StatusLine& line, ProviderIdleReason idle) {
  return line.kind == LineKind::Idle && line.idle == idle;
}

UR_TEST(ProviderStatusLineLocalReasonsWinOverTheServer) {
  for (const ProviderIdleReason idle :
       {ProviderIdleReason::AutoNotConnected, ProviderIdleReason::NetworkOnly,
        ProviderIdleReason::PausedWifiOnly, ProviderIdleReason::PausedNoNetwork}) {
    UR_EXPECT_TRUE(IsIdle(StatusLineFor(idle, "", ""), idle));
    UR_EXPECT_TRUE(IsIdle(StatusLineFor(idle, "reliability_low", "server text"), idle));
    UR_EXPECT_TRUE(IsIdle(StatusLineFor(idle, "network_only", "server text"), idle));
    UR_EXPECT_TRUE(IsIdle(StatusLineFor(idle, "none", "server text"), idle));
  }
}

UR_TEST(ProviderStatusLineShowsTheServerReasonUnlessItIsNone) {
  const struct {
    const char* code;
    ServerReason reason;
  } kCodes[] = {
      {"not_providing", ServerReason::NotProviding},
      {"not_connected", ServerReason::NotConnected},
      {"location_invalid", ServerReason::LocationInvalid},
      {"network_only", ServerReason::NetworkOnly},
      {"reliability_warming_up", ServerReason::ReliabilityWarmingUp},
      {"reliability_low", ServerReason::ReliabilityLow},
      {"not_eligible", ServerReason::NotEligible},
      {"egress_unprobed", ServerReason::EgressUnprobed},
      {"egress_failing", ServerReason::EgressFailing},
      {"speed_test_missing", ServerReason::SpeedTestMissing},
      {"slow", ServerReason::Slow},
  };
  for (const auto& entry : kCodes) {
    UR_EXPECT_TRUE_MSG(entry.code, ServerReasonOf(entry.code) == entry.reason);
    for (const ProviderIdleReason idle :
         {ProviderIdleReason::None, ProviderIdleReason::NoTrafficYet}) {
      const StatusLine line = StatusLineFor(idle, entry.code, "the server's English");
      UR_EXPECT_TRUE_MSG(entry.code, line.kind == LineKind::Server);
      UR_EXPECT_TRUE_MSG(entry.code, line.server == entry.reason);
    }
  }
  UR_EXPECT_TRUE(ServerReasonOf("none") == ServerReason::None);
  UR_EXPECT_TRUE(ServerReasonOf("") == ServerReason::Unknown);
  UR_EXPECT_TRUE(ServerReasonOf("Slow") == ServerReason::Unknown);
}

UR_TEST(ProviderStatusLineServerNoneLeavesNoTrafficYetOrNothing) {
  UR_EXPECT_TRUE(IsIdle(StatusLineFor(ProviderIdleReason::NoTrafficYet, "none", "Everything"),
                        ProviderIdleReason::NoTrafficYet));
  UR_EXPECT_TRUE(StatusLineFor(ProviderIdleReason::None, "none", "Everything").kind ==
                 LineKind::Hidden);
  // without a status: the local reason alone
  UR_EXPECT_TRUE(IsIdle(StatusLineFor(ProviderIdleReason::NoTrafficYet, "", ""),
                        ProviderIdleReason::NoTrafficYet));
  UR_EXPECT_TRUE(StatusLineFor(ProviderIdleReason::None, "", "").kind == LineKind::Hidden);
}

UR_TEST(ProviderStatusLineUnknownCodeShowsTheServerText) {
  const StatusLine line =
      StatusLineFor(ProviderIdleReason::None, "region_full", "Your region has enough providers.");
  UR_EXPECT_TRUE(line.kind == LineKind::ServerText);
  UR_EXPECT_TRUE(line.text == "Your region has enough providers.");
  UR_EXPECT_TRUE(StatusLineFor(ProviderIdleReason::NoTrafficYet, "region_full", "text").kind ==
                 LineKind::ServerText);
  // an unknown code with no text says nothing of its own
  UR_EXPECT_TRUE(IsIdle(StatusLineFor(ProviderIdleReason::NoTrafficYet, "region_full", ""),
                        ProviderIdleReason::NoTrafficYet));
  UR_EXPECT_TRUE(StatusLineFor(ProviderIdleReason::None, "region_full", "").kind ==
                 LineKind::Hidden);
}

// ---- the "Why?" rows ----------------------------------------------------------------

// tagged stand-ins for the store's templates and the byte rate
NumberWords TaggedWords() {
  NumberWords words;
  words.withMinimum = [](const std::string& value, const std::string& minimum) {
    return "min(" + value + "," + minimum + ")";
  };
  words.withMaximum = [](const std::string& value, const std::string& maximum) {
    return "max(" + value + "," + maximum + ")";
  };
  words.countOfTotal = [](int64_t count, int64_t total) {
    return "of(" + std::to_string(count) + "," + std::to_string(total) + ")";
  };
  words.rate = [](double bytesPerSecond) {
    return "rate(" + std::to_string(static_cast<int64_t>(bytesPerSecond)) + ")";
  };
  words.reasonLineTcp = [](int64_t count) { return "tcp(" + std::to_string(count) + ")"; };
  words.reasonLineIpv4 = [](int64_t count) { return "ipv4(" + std::to_string(count) + ")"; };
  words.notYet = "NOT_YET";
  words.noHistory = "NO_HISTORY";
  words.notInPool = "NOT_IN_POOL";
  return words;
}

RankingNumber Number(const std::string& name, bool hasValue, double value, bool passes = true) {
  RankingNumber number;
  number.name = name;
  number.hasValue = hasValue;
  number.value = value;
  number.passes = passes;
  return number;
}

RankingNumber WithMinimum(RankingNumber number, double minimum) {
  number.hasMinimum = true;
  number.minimum = minimum;
  return number;
}

RankingNumber WithMaximum(RankingNumber number, double maximum) {
  number.hasMaximum = true;
  number.maximum = maximum;
  return number;
}

NumberRow Row(NumberLabel label, const std::string& value, bool passes = true) {
  NumberRow row;
  row.label = label;
  row.value = value;
  row.passes = passes;
  return row;
}

UR_TEST(ProviderStatusWhyRowsForEveryNumberInServerOrder) {
  RankingNumber urlChecks = WithMinimum(Number("url_checks", true, 0.95, true), 0.8);
  urlChecks.count = 19;
  urlChecks.total = 20;
  const std::vector<RankingNumber> numbers = {
      Number("reliability_5m", true, 0.82),
      WithMinimum(Number("reliability_1h", true, 0.97), 0.95),
      WithMinimum(Number("reliability_12h", true, 0.5, false), 0.7),
      WithMinimum(Number("reliability_lookback_3", true, 0.61), 0.6),
      urlChecks,
      WithMinimum(Number("speed_test", true, 2500000, true), 1000000),
      WithMaximum(Number("latency", true, 35, true), 200),
      Number("weight_quality", true, 0.4213, true),
      Number("tier_quality", true, 1, true),
      Number("weight_speed", true, 0.2, false),
      Number("tier_speed", true, 3, false),
  };
  const std::vector<NumberRow> rows = NumberRowsFor(numbers, std::nullopt, TaggedWords());
  const std::vector<NumberRow> expected = {
      Row(NumberLabel::Reliability5m, "82%"),
      Row(NumberLabel::Reliability1h, "min(97%,95%)"),
      Row(NumberLabel::Reliability12h, "min(50%,70%)", false),
      Row(NumberLabel::ReliabilityOther, "min(61%,60%)"),
      Row(NumberLabel::UrlChecks, "min(of(19,20),80%)"),
      Row(NumberLabel::SpeedTest, "min(rate(2500000),rate(1000000))"),
      Row(NumberLabel::Latency, "max(35 ms,200 ms)"),
      Row(NumberLabel::WeightQuality, "0.42"),
      Row(NumberLabel::TierQuality, "1"),
      Row(NumberLabel::WeightSpeed, "NOT_IN_POOL", false),
      Row(NumberLabel::TierSpeed, "3", false),
  };
  UR_EXPECT_EQ(expected.size(), rows.size());
  for (size_t i = 0; i < expected.size() && i < rows.size(); ++i) {
    UR_EXPECT_TRUE_MSG("row " + std::to_string(i) + " " + rows[i].value, rows[i] == expected[i]);
  }
}

UR_TEST(ProviderStatusWhyRowsMissingValuesSayNoHistoryOrNotYet) {
  const std::vector<RankingNumber> numbers = {
      Number("reliability_5m", false, 0),
      WithMinimum(Number("reliability_1h", false, 0), 0.95),
      WithMinimum(Number("reliability_12h", false, 0), 0.7),
      WithMinimum(Number("url_checks", false, 0, false), 0.8),
      WithMinimum(Number("speed_test", false, 0, false), 1000000),
      WithMaximum(Number("latency", false, 0, false), 200),
  };
  const std::vector<NumberRow> rows = NumberRowsFor(numbers, std::nullopt, TaggedWords());
  const std::vector<NumberRow> expected = {
      Row(NumberLabel::Reliability5m, "NO_HISTORY"),
      Row(NumberLabel::Reliability1h, "NO_HISTORY"),
      Row(NumberLabel::Reliability12h, "NO_HISTORY"),
      Row(NumberLabel::UrlChecks, "NOT_YET", false),
      Row(NumberLabel::SpeedTest, "NOT_YET", false),
      Row(NumberLabel::Latency, "NOT_YET", false),
  };
  UR_EXPECT_EQ(expected.size(), rows.size());
  for (size_t i = 0; i < expected.size() && i < rows.size(); ++i) {
    UR_EXPECT_TRUE_MSG("row " + std::to_string(i) + " " + rows[i].value, rows[i] == expected[i]);
  }
}

UR_TEST(ProviderStatusWhyRowsSkipUnknownNamesAndEndWithTheCountry) {
  const std::vector<RankingNumber> numbers = {
      Number("demand_share", true, 0.3),
      Number("reliability_lookback_", true, 0.5),
      Number("reliability_lookback_x", true, 0.5),
      Number("Reliability_5m", true, 0.5),
      Number("reliability_5m", true, 0.996),
  };
  const std::vector<NumberRow> rows =
      NumberRowsFor(numbers, Country{"de", "Germany"}, TaggedWords());
  UR_EXPECT_EQ(2u, rows.size());
  if (rows.size() == 2) {
    UR_EXPECT_TRUE(rows[0] == Row(NumberLabel::Reliability5m, "100%"));
    UR_EXPECT_TRUE(rows[1] == Row(NumberLabel::Country, "Germany"));
  }
  // with no country name, the upper-cased code
  const std::vector<NumberRow> codeOnly = NumberRowsFor({}, Country{"de", ""}, TaggedWords());
  UR_EXPECT_EQ(1u, codeOnly.size());
  if (codeOnly.size() == 1) UR_EXPECT_TRUE(codeOnly[0] == Row(NumberLabel::Country, "DE"));
  // no numbers and no country: no rows
  UR_EXPECT_TRUE(NumberRowsFor({}, std::nullopt, TaggedWords()).empty());
}

// a downgraded provider's network checks (server FP2-PROTOCOL.md §5.5): the
// sdk's lines in its order, tcp and ipv4 in the store's words with their
// counts, another label's English, and nothing without lines
UR_TEST(ProviderStatusWhyRowsTellADowngradedDeviceWhy) {
  using urnw::providerstatus::ReasonLine;
  RankingNumber urlChecks = WithMinimum(Number("url_checks", true, 0.65, false), 0.8);
  urlChecks.count = 13;
  urlChecks.total = 20;
  urlChecks.reasonLines = {ReasonLine{"tcp", 3, ""}, ReasonLine{"ipv4", 2, ""}};
  RankingNumber newer = Number("url_checks", true, 0.65, false);
  newer.reasonLines = {ReasonLine{"text", 0, "A newer server's sentence."},
                       ReasonLine{"pq", 0, ""}};
  const std::vector<NumberRow> rows =
      NumberRowsFor({urlChecks, newer, Number("reliability_5m", true, 0.82)}, std::nullopt,
                    TaggedWords());
  UR_EXPECT_EQ(3u, rows.size());
  if (rows.size() == 3) {
    NumberRow downgraded = Row(NumberLabel::UrlChecks, "min(of(13,20),80%)", false);
    downgraded.reasons = {"tcp(3)", "ipv4(2)"};
    UR_EXPECT_TRUE(rows[0] == downgraded);
    UR_EXPECT_TRUE(rows[1].reasons == std::vector<std::string>{"A newer server's sentence."});
    UR_EXPECT_TRUE(rows[2].reasons.empty());
  }
}

UR_TEST(ProviderStatusWhyRowsFormatPercentDelayAndWeight) {
  using urnw::providerstatus::MillisecondsText;
  using urnw::providerstatus::PercentText;
  using urnw::providerstatus::WeightText;
  UR_EXPECT_TRUE(PercentText(0.82) == "82%");
  UR_EXPECT_TRUE(PercentText(0.0) == "0%");
  UR_EXPECT_TRUE(PercentText(1.0) == "100%");
  UR_EXPECT_TRUE(PercentText(0.954) == "95%");
  UR_EXPECT_TRUE(MillisecondsText(35.4) == "35 ms");
  UR_EXPECT_TRUE(WeightText(0.4213) == "0.42");
  UR_EXPECT_TRUE(WeightText(1.0) == "1.00");
}

// ---- the states -----------------------------------------------------------------------

Poll Loaded() {
  Poll poll;
  poll.open = true;
  poll.loaded = true;
  poll.hasStatus = true;
  poll.reason = "none";
  poll.hasAppearances = true;
  poll.appearancesPerMinute = Minutes(0);
  poll.appearancesPerMinute[42] = 3;
  return poll;
}

bool ViewIs(const StatusView& view, ChartState chart, bool showWhy) {
  return view.chart == chart && view.showWhy == showWhy;
}

UR_TEST(ProviderStatusStatesBeforeTheFirstSuccessfulPoll) {
  Poll poll;
  poll.open = true;
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(poll), ChartState::Loading, false));
  poll.lastFetchError = "404 page not found";
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(poll), ChartState::Unavailable, false));
  // no controller (it could not open): unavailable, never a spinner forever
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(Poll{}), ChartState::Unavailable, false));
}

UR_TEST(ProviderStatusStatesAfterAPoll) {
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(Loaded()), ChartState::Bars, true));
  Poll empty = Loaded();
  empty.appearancesPerMinute = Minutes(0);
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(empty), ChartState::Empty, true));
  Poll noHistogram = Loaded();
  noHistogram.hasAppearances = false;
  noHistogram.appearancesPerMinute.clear();
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(noHistogram), ChartState::Unavailable, true));
  // loaded with no status for this device: the chart area stays (unavailable)
  // so the layout does not jump, and there are no numbers to explain
  Poll noStatus;
  noStatus.open = true;
  noStatus.loaded = true;
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(noStatus), ChartState::Unavailable, false));
  // a failed poll after a success keeps the last snapshot
  Poll stale = Loaded();
  stale.lastFetchError = "timeout";
  UR_EXPECT_TRUE(ViewIs(StatusViewFor(stale), ChartState::Bars, true));
}

}  // namespace
