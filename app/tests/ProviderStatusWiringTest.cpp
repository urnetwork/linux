// The provider status's wiring (support part P008). SdkHost and EarningsPage
// need GTK and the SDK, so this reads their sources and po/en.po, the way
// EarningsPageTeardownTest reads the page:
//
//   * the SDK's ProviderStatusViewController opens with the presentation next
//     to the peers (only while providing is not never), and closes with it, or
//     when the mode becomes never, through the typed close after its listener
//     is dropped (the generic close cannot release a C++ handle);
//   * it polls only while the Earnings destination is on screen;
//   * the line under the provide mode row is applied before ApplyProvideState
//     returns early, sits directly under that row, and its Change opens the
//     provide mode where the row does, with the picker revealed;
//   * the Demand chart and "Why?" follow the Blocked chart and share the
//     provider plots' gate;
//   * every "Why?" label and help line, local reason and server reason reads
//     its own store key;
//   * every English fallback is the catalog's msgid, or every locale would show
//     the stale English.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadProviderStatusSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The body of the function whose definition starts with `signature`, or ""
// (braces balanced from the first '{' after the signature).
std::string ProviderStatusFunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = source.find('{', at);
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') ++depth;
    if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
  }
  return std::string();
}

// true when `first` and `second` both occur in `text` and `first` comes first
bool InOrder(const std::string& text, const std::string& first, const std::string& second) {
  const size_t a = text.find(first);
  const size_t b = text.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

// true when the switch case `label` in `body` calls `call` before the next case
bool CaseCalls(const std::string& body, const std::string& label, const std::string& call) {
  const size_t at = body.find(label);
  if (at == std::string::npos) return false;
  const size_t next = body.find("case ", at + label.size());
  const size_t found = body.find(call, at);
  return found != std::string::npos && (next == std::string::npos || found < next);
}

// Every English fallback passed to T_("key", ...) in `source`: the adjacent
// string literals after the key, joined.
std::vector<std::string> ProviderStatusFallbacks(const std::string& source,
                                                 const std::string& key) {
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

UR_TEST(ProviderStatusControllerClosesWithTheTypedCloseAfterItsListener) {
  const std::string host = ReadProviderStatusSource("SdkHost.cpp");
  UR_EXPECT_TRUE(!host.empty());
  const std::string close =
      ProviderStatusFunctionBody(host, "void SdkHost::CloseProviderStatusLocked()");
  UR_EXPECT_TRUE_MSG("CloseProviderStatusLocked drops the listener, then the typed close",
                     InOrder(close, "providerStatusSub_.reset();",
                             "device_->closeProviderStatusViewController(*providerStatusVc_);"));
  UR_EXPECT_TRUE(
      InOrder(close, "closeProviderStatusViewController(", "providerStatusVc_.reset();"));
  // the generic close cannot release a C++ handle
  UR_EXPECT_TRUE(host.find("providerStatusVc_->close()") == std::string::npos);

  const std::string presentation =
      ProviderStatusFunctionBody(host, "void SdkHost::ClosePresentationLocked()");
  UR_EXPECT_TRUE_MSG("the presentation's close closes the provider status",
                     InOrder(presentation, "presentationSubs_.clear();",
                             "CloseProviderStatusLocked();"));
  // with no device left there is nothing to close it on: the handles go,
  // the listener first
  UR_EXPECT_TRUE(
      InOrder(presentation, "providerStatusSub_.reset();", "providerStatusVc_.reset();"));
  UR_EXPECT_TRUE(InOrder(presentation, "providerStatusVc_.reset();", "return;"));

  // a mode turned to never closes it; one turned on while presenting opens it
  const std::string mode = ProviderStatusFunctionBody(host, "void SdkHost::SetProvideControlMode(");
  UR_EXPECT_TRUE(InOrder(mode, "mode == \"never\"", "CloseProviderStatusLocked();"));
  UR_EXPECT_TRUE(InOrder(mode, "CloseProviderStatusLocked();", "OpenProviderStatusLocked(mode);"));
  UR_EXPECT_TRUE(mode.find("EmitDrawerEvent(DrawerEvent::ProviderStatus);") != std::string::npos);
}

UR_TEST(ProviderStatusControllerOpensWithThePeersWhileProvidingIsOn) {
  const std::string host = ReadProviderStatusSource("SdkHost.cpp");
  const std::string drawer = ProviderStatusFunctionBody(host, "void SdkHost::SubscribeDrawer()");
  UR_EXPECT_TRUE_MSG("SubscribeDrawer opens it next to the peers",
                     InOrder(drawer, "peerVc_->start();",
                             "OpenProviderStatusLocked(device_->getProvideControlMode());"));
  const std::string open =
      ProviderStatusFunctionBody(host, "void SdkHost::OpenProviderStatusLocked(");
  UR_EXPECT_TRUE_MSG("never is refused before the open",
                     InOrder(open, "provideControlMode == \"never\"",
                             "device_->openProviderStatusViewController()"));
  UR_EXPECT_TRUE(InOrder(open, "openProviderStatusViewController()",
                         "EmitDrawerEvent(DrawerEvent::ProviderStatus)"));
  UR_EXPECT_TRUE(open.find("if (providerStatusPolling_) providerStatusVc_->start();") !=
                 std::string::npos);
  const std::string polling =
      ProviderStatusFunctionBody(host, "void SdkHost::SetProviderStatusPolling(");
  UR_EXPECT_TRUE(polling.find("providerStatusVc_->start();") != std::string::npos);
  UR_EXPECT_TRUE(polling.find("providerStatusVc_->stop();") != std::string::npos);
}

UR_TEST(ProviderStatusPollsOnlyWhileTheEarningsDestinationShows) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  const std::string constructor =
      ProviderStatusFunctionBody(page, "EarningsPage::EarningsPage(SdkHost& host)");
  UR_EXPECT_TRUE(constructor.find(
                     "signal_map().connect([this] { host_.SetProviderStatusPolling(true); });") !=
                 std::string::npos);
  UR_EXPECT_TRUE(
      constructor.find(
          "signal_unmap().connect([this] { host_.SetProviderStatusPolling(false); });") !=
      std::string::npos);
  // the page re-reads the controller on its event and when a device comes or goes
  const std::string events =
      ProviderStatusFunctionBody(page, "void EarningsPage::OnHostEvent(DrawerEvent event)");
  UR_EXPECT_TRUE(CaseCalls(events, "case DrawerEvent::ProviderStatus:", "ApplyProviderStatus();"));
  UR_EXPECT_TRUE(CaseCalls(events, "case DrawerEvent::DeviceLifecycle:", "ApplyProviderStatus();"));
  // and the destructor still touches no widget (issue #13)
  const std::string destructor = ProviderStatusFunctionBody(page, "EarningsPage::~EarningsPage()");
  for (const char* call : {"ApplyProviderStatus(", "ApplyStatusLine(", "RebuildWhyRows(",
                           "demandChart_", "whyRows_", "statusLineRow_"}) {
    UR_EXPECT_TRUE_MSG(std::string("~EarningsPage calls ") + call,
                       destructor.find(call) == std::string::npos);
  }
}

UR_TEST(ProviderStatusLineIsAppliedBeforeTheProvideGateReturns) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  const std::string apply = ProviderStatusFunctionBody(
      page, "void EarningsPage::ApplyProvideState(const LiveStats& stats)");
  UR_EXPECT_TRUE(!apply.empty());
  UR_EXPECT_TRUE_MSG("the live provide state feeds the line before the early return",
                     InOrder(apply, "liveProvideMode_ = stats.provideMode;",
                             "if (enabled == providingEnabled_) return;"));
  UR_EXPECT_TRUE(InOrder(apply, "providePaused_ = stats.providePaused;",
                         "if (enabled == providingEnabled_) return;"));
  UR_EXPECT_TRUE_MSG("ApplyStatusLine runs before the early return",
                     InOrder(apply, "ApplyStatusLine();",
                             "if (enabled == providingEnabled_) return;"));
  // the provider bytes of the window feed it from the throughput tick
  const std::string pull =
      ProviderStatusFunctionBody(page, "void EarningsPage::PullProviderThroughput(bool forced)");
  UR_EXPECT_TRUE(InOrder(pull, "distribution->ByteCount", "ApplyStatusLine();"));
  // and this platform's gate decides the local reason: a provider device runs,
  // the tunnel session's or the daemon's provider-only device
  const std::string line = ProviderStatusFunctionBody(page, "void EarningsPage::ApplyStatusLine()");
  UR_EXPECT_TRUE(line.find("providerstatus::SessionIdleReasonFor(host_.ProviderRuns(),") !=
                 std::string::npos);
  UR_EXPECT_TRUE(line.find("controlMode_ != \"never\"") != std::string::npos);
}

UR_TEST(ProviderStatusLineSitsUnderTheProvideModeRowWithChange) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  const std::string build =
      ProviderStatusFunctionBody(page, "void EarningsPage::BuildNetworkPane()");
  UR_EXPECT_TRUE_MSG("the line follows the provide mode row",
                     InOrder(build, "content->append(*provideModeRow_);",
                             "content->append(*statusLineRow_);"));
  UR_EXPECT_TRUE_MSG("...and comes before the extender row",
                     InOrder(build, "content->append(*statusLineRow_);",
                             "content->append(*extenderRow_);"));
  const size_t from = build.find("statusLineRow_ = Gtk::make_managed");
  const size_t to = build.find("content->append(*statusLineRow_);");
  UR_EXPECT_TRUE(from != std::string::npos && to != std::string::npos && from < to);
  if (from != std::string::npos && to != std::string::npos && from < to) {
    const std::string block = build.substr(from, to - from);
    UR_EXPECT_TRUE(block.find("T_(\"change\", \"Change\")") != std::string::npos);
    UR_EXPECT_TRUE(block.find("if (on_open_provide_settings) on_open_provide_settings();") !=
                   std::string::npos);
    UR_EXPECT_TRUE(block.find("\"ur-row-note\"") != std::string::npos);
  }
}

UR_TEST(ProviderStatusChangeLandsOnTheProvidePicker) {
  // the hook the provide mode row and the Change share opens the picker, which
  // Simple mode keeps in the collapsed "More options" group
  const std::string window = ReadProviderStatusSource("MainWindow.cpp");
  const std::string hook =
      ProviderStatusFunctionBody(window, "earningsPage_->on_open_provide_settings = [this]");
  UR_EXPECT_TRUE_MSG("the hook navigates to the connect page, then reveals the picker",
                     InOrder(hook, "shell_->Navigate(\"connect\");",
                             "connectPage_->RevealProvideControls();"));
  const std::string connect = ReadProviderStatusSource("ConnectPage.cpp");
  const std::string reveal =
      ProviderStatusFunctionBody(connect, "void ConnectPage::RevealProvideControls()");
  UR_EXPECT_TRUE(InOrder(reveal, "moreOptionsExpanded_ = true;", "ApplyMoreOptionsVisibility();"));
  UR_EXPECT_TRUE(reveal.find("grab_focus();") != std::string::npos);
}

UR_TEST(ProviderStatusDemandFollowsTheBlockedChartUnderTheSameGate) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  const std::string build =
      ProviderStatusFunctionBody(page, "void EarningsPage::BuildNetworkPane()");
  UR_EXPECT_TRUE(InOrder(build, "content->append(*blockedChartRow_);",
                         "content->append(*demandChartRow_);"));
  UR_EXPECT_TRUE(
      InOrder(build, "content->append(*demandChartRow_);", "content->append(*whyRow_);"));
  UR_EXPECT_TRUE(InOrder(build, "content->append(*whyRow_);", "content->append(*whyRows_);"));
  const std::string sections =
      ProviderStatusFunctionBody(page, "void EarningsPage::ApplyStatsSections()");
  UR_EXPECT_TRUE(sections.find("blockedChartRow_->set_visible(sections.providerVisible);") !=
                 std::string::npos);
  UR_EXPECT_TRUE_MSG("the demand chart shares the plots' gate",
                     sections.find("demandChartRow_->set_visible(sections.providerVisible);") !=
                         std::string::npos);
  UR_EXPECT_TRUE(sections.find("ApplyWhyVisibility();") != std::string::npos);
  const std::string why =
      ProviderStatusFunctionBody(page, "void EarningsPage::ApplyWhyVisibility()");
  UR_EXPECT_TRUE(why.find("statsSections_.providerVisible") != std::string::npos);
  UR_EXPECT_TRUE(why.find("providerStatusView_.showWhy") != std::string::npos);
}

// What SN25 counted (feature 14) sits beside this device's status, between the
// demand chart and "Why?", and is re-read wherever the controller is: the
// device's line from its status, the unpaid data provided from the
// controller, connected (the GUI's own) or not (the daemon's, provider_stats).
UR_TEST(ProviderStatusProvidedLinesFollowTheDemandChart) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  const std::string build =
      ProviderStatusFunctionBody(page, "void EarningsPage::BuildNetworkPane()");
  UR_EXPECT_TRUE(InOrder(build, "content->append(*demandChartRow_);",
                         "content->append(*providedRow_);"));
  UR_EXPECT_TRUE(
      InOrder(build, "content->append(*providedRow_);", "content->append(*unpaidRow_);"));
  UR_EXPECT_TRUE(InOrder(build, "content->append(*unpaidRow_);", "content->append(*whyRow_);"));
  const std::string apply =
      ProviderStatusFunctionBody(page, "void EarningsPage::ApplyProviderStatus()");
  UR_EXPECT_TRUE(InOrder(apply, "ProviderStatusPollOf(host_.ProviderStatusNow())",
                         "ApplyProvidedLines();"));
  const std::string poll = ProviderStatusFunctionBody(
      page, "providerstatus::Poll ProviderStatusPollOf(const SdkHost::ProviderStatusSnapshot&");
  UR_EXPECT_TRUE(InOrder(poll, "poll.unpaidEpoch = snapshot.providedEpoch;",
                         "if (!snapshot.status) return poll;"));
  UR_EXPECT_TRUE(poll.find("poll.providedEpoch = status.provided->epoch;") != std::string::npos);

  const std::string host = ReadProviderStatusSource("SdkHost.cpp");
  const std::string now =
      ProviderStatusFunctionBody(host, "SdkHost::ProviderStatusSnapshot SdkHost::ProviderStatusNow()");
  UR_EXPECT_TRUE(InOrder(now, "snapshot.status = providerStatusVc_->getProviderStatus();",
                         "snapshot.providedEpoch = providerStatusVc_->getProvidedEpoch();"));
  UR_EXPECT_TRUE(now.find("snapshot.unpaidByteCount = providerStatusVc_->getUnpaidByteCount();") !=
                 std::string::npos);
  const std::string daemonStats =
      ProviderStatusFunctionBody(host, "void SdkHost::PollDaemonProviderStatsLocked()");
  UR_EXPECT_TRUE(daemonStats.find("stats.status.providedEpoch = reply->status_provided_epoch;") !=
                 std::string::npos);
  UR_EXPECT_TRUE(daemonStats.find(
                     "stats.status.unpaidByteCount = reply->status_unpaid_byte_count;") !=
                 std::string::npos);
  const std::string daemon = ReadProviderStatusSource("daemon/TunnelHost.cpp");
  const std::string reply =
      ProviderStatusFunctionBody(daemon, "ctl::ProviderStatsReply TunnelHost::ProviderStats(");
  UR_EXPECT_TRUE(reply.find("reply.status_provided_epoch = providerStatusVc_->getProvidedEpoch();") !=
                 std::string::npos);
  UR_EXPECT_TRUE(
      reply.find("reply.status_unpaid_byte_count = providerStatusVc_->getUnpaidByteCount();") !=
      std::string::npos);
}

// The key of the first T_("key", ...) after the case label `label` in `body`
// ("" when none): a case that shares its group's return reads that return's key.
std::string FirstKeyAfter(const std::string& body, const std::string& label) {
  const size_t at = body.find(label);
  if (at == std::string::npos) return std::string();
  const size_t call = body.find("T_(\"", at);
  if (call == std::string::npos) return std::string();
  const size_t start = call + 4;
  const size_t end = body.find('"', start);
  return end == std::string::npos ? std::string() : body.substr(start, end - start);
}

UR_TEST(ProviderStatusPageMapsEveryCaseToItsStoreKey) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  struct Case {
    const char* function;
    const char* label;
    const char* key;
  };
  const Case kCases[] = {
      // the "Why?" rows' labels and help lines (P008_APP_SPEC 3.3)
      {"Glib::ustring NumberLabelText(", "NumberLabel::Reliability5m:",
       "provider_status_number_reliability_5m"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::Reliability1h:",
       "provider_status_number_reliability_1h"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::Reliability12h:",
       "provider_status_number_reliability_12h"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::ReliabilityOther:", "reliability"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::UrlChecks:",
       "provider_status_number_url_checks"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::SpeedTest:",
       "provider_status_number_speed_test"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::Latency:", "provider_status_number_latency"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::WeightQuality:",
       "provider_status_number_weight_quality"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::WeightSpeed:",
       "provider_status_number_weight_speed"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::TierQuality:",
       "provider_status_number_tier_quality"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::TierSpeed:",
       "provider_status_number_tier_speed"},
      {"Glib::ustring NumberLabelText(", "NumberLabel::Country:", "country"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::Reliability5m:",
       "provider_status_help_reliability_5m"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::Reliability1h:",
       "provider_status_help_reliability_1h"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::Reliability12h:",
       "provider_status_help_reliability_12h"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::ReliabilityOther:",
       "provider_status_help_reliability_other"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::UrlChecks:",
       "provider_status_help_url_checks"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::SpeedTest:",
       "provider_status_help_speed_test"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::Latency:", "provider_status_help_latency"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::WeightQuality:",
       "provider_status_help_weight"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::WeightSpeed:", "provider_status_help_weight"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::TierQuality:", "provider_status_help_tier"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::TierSpeed:", "provider_status_help_tier"},
      {"Glib::ustring NumberHelpText(", "NumberLabel::Country:", "provider_status_help_country"},
      // the local reasons (1.3) and the server's, provider_status_reason_<code> (3.5)
      {"Glib::ustring IdleReasonText(", "ProviderIdleReason::AutoNotConnected:",
       "provider_idle_auto_not_connected"},
      {"Glib::ustring IdleReasonText(", "ProviderIdleReason::NetworkOnly:",
       "provider_idle_network_only"},
      {"Glib::ustring IdleReasonText(", "ProviderIdleReason::PausedWifiOnly:",
       "provider_idle_paused_wifi_only"},
      {"Glib::ustring IdleReasonText(", "ProviderIdleReason::PausedNoNetwork:",
       "provider_idle_paused_no_network"},
      {"Glib::ustring IdleReasonText(", "ProviderIdleReason::NoTrafficYet:",
       "provider_idle_no_traffic_yet"},
      {"Glib::ustring ServerReasonText(", "ServerReason::NotProviding:",
       "provider_status_reason_not_providing"},
      {"Glib::ustring ServerReasonText(", "ServerReason::NotConnected:",
       "provider_status_reason_not_connected"},
      {"Glib::ustring ServerReasonText(", "ServerReason::LocationInvalid:",
       "provider_status_reason_location_invalid"},
      {"Glib::ustring ServerReasonText(", "ServerReason::NetworkOnly:",
       "provider_status_reason_network_only"},
      {"Glib::ustring ServerReasonText(", "ServerReason::ReliabilityWarmingUp:",
       "provider_status_reason_reliability_warming_up"},
      {"Glib::ustring ServerReasonText(", "ServerReason::ReliabilityLow:",
       "provider_status_reason_reliability_low"},
      {"Glib::ustring ServerReasonText(", "ServerReason::NotEligible:",
       "provider_status_reason_not_eligible"},
      {"Glib::ustring ServerReasonText(", "ServerReason::EgressUnprobed:",
       "provider_status_reason_egress_unprobed"},
      {"Glib::ustring ServerReasonText(", "ServerReason::EgressFailing:",
       "provider_status_reason_egress_failing"},
      {"Glib::ustring ServerReasonText(", "ServerReason::SpeedTestMissing:",
       "provider_status_reason_speed_test_missing"},
      {"Glib::ustring ServerReasonText(", "ServerReason::Slow:", "provider_status_reason_slow"},
      {"Glib::ustring ServerReasonText(", "ServerReason::None:", "provider_status_reason_none"},
  };
  for (const Case& c : kCases) {
    const std::string body = ProviderStatusFunctionBody(page, c.function);
    if (body.empty()) {
      UR_FAIL(std::string("EarningsPage.cpp has no ") + c.function);
      continue;
    }
    const std::string key = FirstKeyAfter(body, std::string("case providerstatus::") + c.label);
    UR_EXPECT_TRUE_MSG(std::string(c.function) + c.label + " reads \"" + key + "\", not \"" +
                           c.key + "\"",
                       key == c.key);
  }
}

UR_TEST(ProviderStatusEnglishMatchesTheCatalogMsgids) {
  const std::string page = ReadProviderStatusSource("EarningsPage.cpp");
  const std::string catalog = ReadProviderStatusSource("../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  const char* const kKeys[] = {
      "provider_idle_auto_not_connected",
      "provider_idle_network_only",
      "provider_idle_paused_wifi_only",
      "provider_idle_paused_no_network",
      "provider_idle_no_traffic_yet",
      "change",
      "loading",
      "country",
      "reliability",
      "provider_status_demand",
      "provider_status_histogram_title",
      "provider_status_histogram_start",
      "provider_status_histogram_end",
      "provider_status_histogram_empty",
      "provider_status_unavailable",
      "provider_status_why",
      "provider_status_reason_not_providing",
      "provider_status_reason_not_connected",
      "provider_status_reason_location_invalid",
      "provider_status_reason_network_only",
      "provider_status_reason_reliability_warming_up",
      "provider_status_reason_reliability_low",
      "provider_status_reason_not_eligible",
      "provider_status_reason_egress_unprobed",
      "provider_status_reason_egress_failing",
      "provider_status_reason_speed_test_missing",
      "provider_status_reason_slow",
      "provider_status_reason_none",
      "provider_status_number_reliability_5m",
      "provider_status_number_reliability_1h",
      "provider_status_number_reliability_12h",
      "provider_status_number_url_checks",
      "provider_status_number_speed_test",
      "provider_status_number_latency",
      "provider_status_number_weight_quality",
      "provider_status_number_weight_speed",
      "provider_status_number_tier_quality",
      "provider_status_number_tier_speed",
      "provider_status_help_reliability_5m",
      "provider_status_help_reliability_1h",
      "provider_status_help_reliability_12h",
      "provider_status_help_reliability_other",
      "provider_status_help_url_checks",
      "provider_status_help_speed_test",
      "provider_status_help_latency",
      "provider_status_help_weight",
      "provider_status_help_tier",
      "provider_status_help_country",
      "provider_status_value_with_minimum",
      "provider_status_value_with_maximum",
      "provider_status_value_count_of_total",
      "provider_status_value_not_yet",
      "provider_status_value_no_history",
      "provider_status_value_not_in_pool",
  };
  for (const char* key : kKeys) {
    const std::vector<std::string> fallbacks = ProviderStatusFallbacks(page, key);
    if (fallbacks.empty()) UR_FAIL(std::string("EarningsPage.cpp does not show ") + key);
    for (const std::string& english : fallbacks) {
      const std::string entry = std::string("msgctxt \"") + key + "\"\nmsgid \"" + english + "\"\n";
      if (catalog.find(entry) == std::string::npos) {
        UR_FAIL(std::string(key) + ": \"" + english + "\" is not the catalog's msgid");
      }
    }
  }
  // the total is a plural, looked up with the catalog's two English forms
  const size_t total = page.find("TN_(\"provider_status_histogram_total\"");
  UR_EXPECT_TRUE(total != std::string::npos);
  if (total != std::string::npos) {
    const std::string call = page.substr(total, 200);
    UR_EXPECT_TRUE(call.find("\"{} time in the last hour\"") != std::string::npos);
    UR_EXPECT_TRUE(call.find("\"{} times in the last hour\"") != std::string::npos);
  }
  UR_EXPECT_TRUE(catalog.find("msgctxt \"provider_status_histogram_total\"\n"
                              "msgid \"{} time in the last hour\"\n"
                              "msgid_plural \"{} times in the last hour\"\n") != std::string::npos);
  UR_EXPECT_TRUE(page.find("T_(\"provider_status_histogram_total\"") == std::string::npos);
}

}  // namespace
