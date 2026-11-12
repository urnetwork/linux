// The provider card's status on the Earnings page (support part P008), decided
// in pure steps so every display rule is deterministic and testable; the page
// only draws what this decided.
//
// A provider that was enabled but idle saw empty charts and no reason, and
// read it as "the app doesn't pay". Two sources explain it now:
//
//   * the local reason (phase 1): the provide control mode against the live
//     provide state, one muted line under the provide mode row with a Change
//     action. It needs nothing from the server, so it is immediate;
//   * the server's provider status (phase 2), from the SDK's
//     ProviderStatusViewController (GET /network/provider-status): how often
//     the network offered this device to clients in each minute of the last
//     hour (the "Demand" histogram beside the provider plots), the first
//     reason holding it back, and the numbers it is ranked by (the expandable
//     "Why?").
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/ProviderIdleReasonTest.cpp,
// tests/ProviderStatusPresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace urnw::providerstatus {

// ---- the local idle reason (phase 1) ------------------------------------------

// The SDK's ProvideModePublic: the live provide mode while this device
// provides to everyone (a bit set: 0 none, 1 network, 2 friends-and-family,
// 3 public).
inline constexpr int64_t kProvideModePublic = 3;

// Where a mobile device may provide. The desktop has no such setting and
// passes All, so PausedWifiOnly never occurs here.
enum class ProvideNetworkMode { WiFi, All };

enum class ProviderIdleReason {
  None,
  AutoNotConnected,  // provider_idle_auto_not_connected
  NetworkOnly,       // provider_idle_network_only
  PausedWifiOnly,    // provider_idle_paused_wifi_only
  PausedNoNetwork,   // provider_idle_paused_no_network
  NoTrafficYet,      // provider_idle_no_traffic_yet
};

// Why an enabled provider carries no public traffic (providerIdleReason), from
// the provide control mode ("auto", "always", "network" or "never"), the live
// provide mode, the pause flag, the network mode and the provider bytes of the
// throughput window. The first match wins:
//   Never, or a mode this app does not know (the SDK's unexposed manual among
//     them): None. The providing_disabled text already covers Never.
//   Network: NetworkOnly.
//   Auto without the live public mode: AutoNotConnected. Auto provides to
//     everyone only while this device's VPN is connected, and only to the
//     user's own devices otherwise (sdk applyProvideControlModeWithLock).
//   Paused with Wi-Fi only: PausedWifiOnly. Paused otherwise: PausedNoNetwork.
//   Public with no bytes in the window: NoTrafficYet.
inline ProviderIdleReason ProviderIdleReasonFor(const std::string& controlMode,
                                                int64_t liveProvideMode, bool providePaused,
                                                ProvideNetworkMode networkMode,
                                                int64_t recentProviderBytes) {
  if (controlMode != "auto" && controlMode != "always" && controlMode != "network") {
    return ProviderIdleReason::None;
  }
  if (controlMode == "network") return ProviderIdleReason::NetworkOnly;
  if (controlMode == "auto" && liveProvideMode != kProvideModePublic) {
    return ProviderIdleReason::AutoNotConnected;
  }
  if (providePaused && networkMode == ProvideNetworkMode::WiFi) {
    return ProviderIdleReason::PausedWifiOnly;
  }
  if (providePaused) return ProviderIdleReason::PausedNoNetwork;
  if (liveProvideMode == kProvideModePublic && recentProviderBytes == 0) {
    return ProviderIdleReason::NoTrafficYet;
  }
  return ProviderIdleReason::None;
}

// The reason on this platform. Linux provides through the daemon: connected,
// the tunnel session's DeviceLocal; disconnected, the provider-only device
// that runs for a mode that provides then (ProvideLifecycle.hpp), whose live
// tier the daemon's status carries. `providerRuns` is either of them. With
// neither there is no live provide mode to read (the daemon refused the
// provider-only device, predates it or cannot be reached) and nothing is
// provided, so no reason is derived: "Choose Always to earn while idle" or
// "own devices" would not be true then. `recentProviderBytes` is nullopt
// while no throughput window has been read from either device, and "no
// traffic yet" is said only of a window that was read and is empty.
inline ProviderIdleReason SessionIdleReasonFor(bool providerRuns, const std::string& controlMode,
                                               int64_t liveProvideMode, bool providePaused,
                                               std::optional<int64_t> recentProviderBytes) {
  if (!providerRuns) return ProviderIdleReason::None;
  const ProviderIdleReason reason =
      ProviderIdleReasonFor(controlMode, liveProvideMode, providePaused, ProvideNetworkMode::All,
                            recentProviderBytes.value_or(0));
  if (reason == ProviderIdleReason::NoTrafficYet && !recentProviderBytes) {
    return ProviderIdleReason::None;
  }
  return reason;
}

// ---- the server's reason and the line under the provide mode row --------------

// The server's reason codes (ProviderStatusReason*). Each one's store key is
// provider_status_reason_<code>.
enum class ServerReason {
  Unknown,  // a code this app does not know: the server's English text shows
  NotProviding,
  NotConnected,
  LocationInvalid,
  NetworkOnly,
  ReliabilityWarmingUp,
  ReliabilityLow,
  NotEligible,
  EgressUnprobed,
  EgressFailing,
  SpeedTestMissing,
  Slow,
  None,
};

inline ServerReason ServerReasonOf(const std::string& code) {
  struct Entry {
    const char* code;
    ServerReason reason;
  };
  static const Entry kCodes[] = {
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
      {"none", ServerReason::None},
  };
  for (const Entry& entry : kCodes) {
    if (code == entry.code) return entry.reason;
  }
  return ServerReason::Unknown;
}

enum class LineKind {
  Hidden,
  Idle,        // the local reason's provider_idle_* text
  Server,      // the server reason's provider_status_reason_* text
  ServerText,  // a server code this app does not know: its English reason_text
};

// The one line under the provide mode row.
struct StatusLine {
  LineKind kind = LineKind::Hidden;
  ProviderIdleReason idle = ProviderIdleReason::None;  // with Idle
  ServerReason server = ServerReason::Unknown;          // with Server
  std::string text;                                     // with ServerText

  bool operator==(const StatusLine& other) const {
    return kind == other.kind && idle == other.idle && server == other.server &&
           text == other.text;
  }
  bool operator!=(const StatusLine& other) const { return !(*this == other); }
};

// The line (providerStatusLine). Local state wins because it is immediate: the
// server's ranking is cached for about five minutes, so right after a mode
// change it can still say network_only. Then the server's reason, unless it is
// "none"; then NoTrafficYet; otherwise no line. `serverReason` is "" without a
// status. A code this app does not know shows the server's English text, which
// keeps the line forward-compatible.
inline StatusLine StatusLineFor(ProviderIdleReason idle, const std::string& serverReason,
                                const std::string& serverReasonText) {
  StatusLine line;
  switch (idle) {
    case ProviderIdleReason::AutoNotConnected:
    case ProviderIdleReason::NetworkOnly:
    case ProviderIdleReason::PausedWifiOnly:
    case ProviderIdleReason::PausedNoNetwork:
      line.kind = LineKind::Idle;
      line.idle = idle;
      return line;
    case ProviderIdleReason::None:
    case ProviderIdleReason::NoTrafficYet:
      break;
  }
  if (!serverReason.empty() && serverReason != "none") {
    const ServerReason server = ServerReasonOf(serverReason);
    if (server != ServerReason::Unknown) {
      line.kind = LineKind::Server;
      line.server = server;
      return line;
    }
    if (!serverReasonText.empty()) {
      line.kind = LineKind::ServerText;
      line.text = serverReasonText;
      return line;
    }
  }
  if (idle == ProviderIdleReason::NoTrafficYet) {
    line.kind = LineKind::Idle;
    line.idle = idle;
  }
  return line;
}

// ---- the demand histogram -------------------------------------------------------

inline constexpr size_t kHistogramBars = 60;

struct Histogram {
  // kHistogramBars bar heights, each count / max(largest count, 1), oldest
  // first; the last bar is the current, partial minute
  std::vector<double> fractions;
  int64_t total = 0;
  bool empty = true;  // every count is 0: the baseline, the axis and the empty line
};

// The bars for the server's per-minute counts (60, oldest first). Exactly
// kHistogramBars bars whatever arrives: the newest counts, right-aligned, with
// the minutes before them as zeros. A negative count reads as zero.
inline Histogram HistogramFor(const std::vector<int64_t>& countsPerMinute) {
  Histogram histogram;
  histogram.fractions.assign(kHistogramBars, 0.0);
  const size_t kept = std::min(countsPerMinute.size(), kHistogramBars);
  const size_t from = countsPerMinute.size() - kept;
  int64_t maxCount = 0;
  for (size_t i = from; i < countsPerMinute.size(); ++i) {
    const int64_t count = std::max<int64_t>(0, countsPerMinute[i]);
    histogram.total += count;
    maxCount = std::max(maxCount, count);
  }
  const double scale = static_cast<double>(std::max<int64_t>(maxCount, 1));
  for (size_t i = 0; i < kept; ++i) {
    const int64_t count = std::max<int64_t>(0, countsPerMinute[from + i]);
    histogram.fractions[kHistogramBars - kept + i] = static_cast<double>(count) / scale;
  }
  histogram.empty = histogram.total == 0;
  return histogram;
}

// ---- the controller's state -----------------------------------------------------

// One number the network ranks this device by (the SDK's ProviderRankingNumber).
// One line the sdk derived from a number's reason (its
// ProviderStatusReasonLine, server FP2-PROTOCOL.md §5.5): why the network
// checks downgraded this device, by a label this app has words for and the
// count of checks behind it, or the server's English for any other label.
struct ReasonLine {
  std::string label;  // "tcp", "ipv4", or a label this app does not know
  int64_t count = 0;  // the checks a tcp or ipv4 line counts
  std::string text;   // the server's English, for any other label

  bool operator==(const ReasonLine& other) const {
    return label == other.label && count == other.count && text == other.text;
  }
};

struct RankingNumber {
  std::string name;
  bool hasValue = false;
  double value = 0;
  bool hasMinimum = false;
  double minimum = 0;
  bool hasMaximum = false;
  double maximum = 0;
  bool passes = true;
  int64_t count = 0;
  int64_t total = 0;
  // the sdk's lines, in the order they show (url_checks only)
  std::vector<ReasonLine> reasonLines;
};

// Where clients find this device (the SDK's ProviderStatusCountry).
struct Country {
  std::string countryCode;
  std::string country;
};

// The controller's state, read together (SdkHost::ProviderStatusNow), in the
// presentation's own terms.
struct Poll {
  bool open = false;           // a controller exists
  bool loaded = false;         // a poll has succeeded
  std::string lastFetchError;  // the last failed poll's error, "" once one succeeds
  bool hasStatus = false;      // this device is one of the network's provider clients
  std::string reason;          // the server's reason code, "" without a status
  std::string reasonText;      // its English text
  bool hasAppearances = false;
  std::vector<int64_t> appearancesPerMinute;
  std::vector<RankingNumber> ranking;
  std::optional<Country> country;
};

// ---- the states -----------------------------------------------------------------

enum class ChartState {
  Loading,      // loading, in place of the bars
  Unavailable,  // provider_status_unavailable, in place of the bars
  Empty,        // the baseline, the axis and provider_status_histogram_empty
  Bars,         // the 60 bars and the total
};

struct StatusView {
  ChartState chart = ChartState::Loading;
  bool showWhy = false;  // the "Why?" disclosure and its rows

  bool operator==(const StatusView& other) const {
    return chart == other.chart && showWhy == other.showWhy;
  }
};

// What the demand area shows; it shows at all only while the provider plots
// do. Before the first successful poll: loading, or unavailable once a poll
// failed (a 404 before the server route is deployed). The chart area is always
// there while providing is enabled, so the layout does not jump: a poll that
// has no status for this device shows unavailable too. With a status, "Why?"
// shows its numbers, and the chart is unavailable without a histogram, the
// empty state when every count is 0, and the bars otherwise. A failed poll
// after a success keeps the last snapshot.
inline StatusView StatusViewFor(const Poll& poll) {
  StatusView view;
  if (!poll.open) {
    view.chart = ChartState::Unavailable;
    return view;
  }
  if (!poll.loaded) {
    view.chart = poll.lastFetchError.empty() ? ChartState::Loading : ChartState::Unavailable;
    return view;
  }
  if (!poll.hasStatus) {
    view.chart = ChartState::Unavailable;
    return view;
  }
  view.showWhy = true;
  if (!poll.hasAppearances) {
    view.chart = ChartState::Unavailable;
  } else if (HistogramFor(poll.appearancesPerMinute).empty) {
    view.chart = ChartState::Empty;
  } else {
    view.chart = ChartState::Bars;
  }
  return view;
}

// ---- the "Why?" rows -------------------------------------------------------------

// Each row's label and help line; the page maps them to their store keys.
enum class NumberLabel {
  Reliability5m,     // provider_status_number_reliability_5m, provider_status_help_reliability_5m
  Reliability1h,     // provider_status_number_reliability_1h, provider_status_help_reliability_1h
  Reliability12h,    // provider_status_number_reliability_12h, provider_status_help_reliability_12h
  ReliabilityOther,  // reliability, provider_status_help_reliability_other
  UrlChecks,         // provider_status_number_url_checks, provider_status_help_url_checks
  SpeedTest,         // provider_status_number_speed_test, provider_status_help_speed_test
  Latency,           // provider_status_number_latency, provider_status_help_latency
  WeightQuality,     // provider_status_number_weight_quality, provider_status_help_weight
  WeightSpeed,       // provider_status_number_weight_speed, provider_status_help_weight
  TierQuality,       // provider_status_number_tier_quality, provider_status_help_tier
  TierSpeed,         // provider_status_number_tier_speed, provider_status_help_tier
  Country,           // country, provider_status_help_country
};

// The words a value is built from: the store's templates and the app's byte
// rate, supplied by the page (the tests pass tagged stand-ins).
struct NumberWords {
  // provider_status_value_with_minimum, "{0} (needs {1})"
  std::function<std::string(const std::string& value, const std::string& minimum)> withMinimum;
  // provider_status_value_with_maximum, "{0} (at most {1})"
  std::function<std::string(const std::string& value, const std::string& maximum)> withMaximum;
  // provider_status_value_count_of_total, "{0} of {1} loaded"
  std::function<std::string(int64_t count, int64_t total)> countOfTotal;
  // the app's byte-rate formatter, for bytes per second
  std::function<std::string(double bytesPerSecond)> rate;
  // provider_status_reason_line_tcp and _ipv4, plurals on the count
  std::function<std::string(int64_t count)> reasonLineTcp;
  std::function<std::string(int64_t count)> reasonLineIpv4;
  std::string notYet;     // provider_status_value_not_yet
  std::string noHistory;  // provider_status_value_no_history
  std::string notInPool;  // provider_status_value_not_in_pool
};

struct NumberRow {
  NumberLabel label = NumberLabel::Reliability5m;
  std::string value;
  bool passes = true;  // false tints the value amber
  // under the help line, why the network checks downgraded this device
  std::vector<std::string> reasons;

  bool operator==(const NumberRow& other) const {
    return label == other.label && value == other.value && passes == other.passes &&
           reasons == other.reasons;
  }
};

// The lines of a number's reason, in the sdk's order: tcp and ipv4 in the
// store's words with their counts, any other label as the server's English,
// and a line with neither left out.
inline std::vector<std::string> ReasonsFor(const std::vector<ReasonLine>& lines,
                                           const NumberWords& words) {
  std::vector<std::string> reasons;
  for (const ReasonLine& line : lines) {
    if (line.label == "tcp") {
      reasons.push_back(words.reasonLineTcp(line.count));
    } else if (line.label == "ipv4") {
      reasons.push_back(words.reasonLineIpv4(line.count));
    } else if (!line.text.empty()) {
      reasons.push_back(line.text);
    }
  }
  return reasons;
}

// "82%": a 0-1 ratio as a whole percent, the app's percent text (the transport
// bar's): the store owns the words, the digits are the same in every locale.
inline std::string PercentText(double ratio) {
  return std::to_string(std::llround(ratio * 100.0)) + "%";
}

// "35 ms", not localized: the store has no key for it.
inline std::string MillisecondsText(double millis) {
  return std::to_string(std::llround(millis)) + " ms";
}

// "0.42": a selection weight with two decimals.
inline std::string WeightText(double weight) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", weight);
  return buffer;
}

// reliability_lookback_<n>: a reliability window past the first three.
inline bool IsReliabilityLookbackName(const std::string& name) {
  static const std::string kPrefix = "reliability_lookback_";
  if (name.size() <= kPrefix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0) {
    return false;
  }
  for (size_t i = kPrefix.size(); i < name.size(); ++i) {
    if (name[i] < '0' || '9' < name[i]) return false;
  }
  return true;
}

// The rows of "Why?": one per ranking number in the server's order, then the
// country row when the status has a country. A name this app does not know is
// skipped. The server sends no weight or tier rows for not_eligible,
// not_connected, location_invalid or not_providing; whatever arrives is shown
// and nothing is inferred from what does not. A reliability without history
// reads no_history (missing history does not count against the device); a
// check, speed or delay not measured yet reads not_yet.
inline std::vector<NumberRow> NumberRowsFor(const std::vector<RankingNumber>& numbers,
                                            const std::optional<Country>& country,
                                            const NumberWords& words) {
  // the share of steady uptime, with the selection's minimum when it has one
  auto reliability = [&words](const RankingNumber& number) {
    if (!number.hasValue) return words.noHistory;
    if (!number.hasMinimum) return PercentText(number.value);
    return words.withMinimum(PercentText(number.value), PercentText(number.minimum));
  };
  std::vector<NumberRow> rows;
  for (const RankingNumber& number : numbers) {
    NumberRow row;
    row.passes = number.passes;
    if (number.name == "reliability_5m") {
      row.label = NumberLabel::Reliability5m;
      row.value = reliability(number);
    } else if (number.name == "reliability_1h") {
      row.label = NumberLabel::Reliability1h;
      row.value = reliability(number);
    } else if (number.name == "reliability_12h") {
      row.label = NumberLabel::Reliability12h;
      row.value = reliability(number);
    } else if (IsReliabilityLookbackName(number.name)) {
      row.label = NumberLabel::ReliabilityOther;
      row.value = reliability(number);
    } else if (number.name == "url_checks") {
      row.label = NumberLabel::UrlChecks;
      if (!number.hasValue) {
        row.value = words.notYet;
      } else {
        const std::string loaded = words.countOfTotal(number.count, number.total);
        row.value = number.hasMinimum ? words.withMinimum(loaded, PercentText(number.minimum))
                                      : loaded;
      }
    } else if (number.name == "speed_test") {
      row.label = NumberLabel::SpeedTest;
      if (!number.hasValue) {
        row.value = words.notYet;
      } else {
        row.value = number.hasMinimum
                        ? words.withMinimum(words.rate(number.value), words.rate(number.minimum))
                        : words.rate(number.value);
      }
    } else if (number.name == "latency") {
      row.label = NumberLabel::Latency;
      if (!number.hasValue) {
        row.value = words.notYet;
      } else {
        row.value = number.hasMaximum ? words.withMaximum(MillisecondsText(number.value),
                                                          MillisecondsText(number.maximum))
                                      : MillisecondsText(number.value);
      }
    } else if (number.name == "weight_quality" || number.name == "weight_speed") {
      row.label = number.name == "weight_quality" ? NumberLabel::WeightQuality
                                                  : NumberLabel::WeightSpeed;
      row.value = number.passes ? WeightText(number.value) : words.notInPool;
    } else if (number.name == "tier_quality" || number.name == "tier_speed") {
      // 0 is best; past the speed or delay cutoff is the last tier
      row.label = number.name == "tier_quality" ? NumberLabel::TierQuality
                                                : NumberLabel::TierSpeed;
      row.value = std::to_string(std::llround(number.value));
    } else {
      continue;
    }
    row.reasons = ReasonsFor(number.reasonLines, words);
    rows.push_back(row);
  }
  if (country) {
    NumberRow row;
    row.label = NumberLabel::Country;
    if (!country->country.empty()) {
      row.value = country->country;
    } else {
      row.value = country->countryCode;
      for (char& c : row.value) {
        if ('a' <= c && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      }
    }
    rows.push_back(row);
  }
  return rows;
}

}  // namespace urnw::providerstatus
