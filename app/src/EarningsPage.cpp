// SPDX-License-Identifier: MPL-2.0
#include "ProvideModeGlyph.hpp"
#include "EarningsPage.hpp"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "BittensorManualSheet.hpp"
#include "BittensorWalletFlow.hpp"
#include "DisplayText.hpp"
#include "EmojiKeyboard.hpp"
#include "EmojiTagSheet.hpp"
#include "ExtenderProvideRowPaint.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "LeaderboardIndicator.hpp"
#include "SolanaWalletSheet.hpp"
#include "Top200SignSheet.hpp"
#include "UrTheme.hpp"
#include "WalletBridgeRoute.hpp"

namespace urnw {
namespace {

// ---- the numbers the destination is built from ------------------------------
constexpr int kPaneAWidth = 360;
constexpr int kPaneCWidth = 380;
constexpr int kThreePaneDip = 1500;  // earnings | history | network
constexpr int kTwoPaneDip = 900;     // earnings | history
constexpr int kApiTimeoutMs = 20000;      // plain api calls
constexpr int kChainTimeoutMs = 180000;   // a claim waits for a receipt
constexpr int kValidateDebounceMs = 300;
constexpr int kSheetMinWidth = 480;
constexpr int kClaimSheetMaxHeight = 560;
constexpr int kReliabilityChartHeight = 110;
// the provider and extender statistics' chart rows (EXTENDER.md O8): the
// pane's 132 px chart rows, and the Blocked chart at half that
constexpr int kStatsChartHeight = 132;
constexpr int kBlockedChartHeight = 66;
constexpr int kCopiedResetMs = 1800;
constexpr double kMultiplierHighlight = 2.0;   // a country multiplier goes lime at >= 2.0
// A v1 claim's gas in the gas key, in TAO. The SDK signs a v1 claim with its
// estimate plus a quarter and at least 2,700,000 gas (below that the vault
// defers the payment), and sends it only when the key holds that limit at 1.1 x
// the gas price, else needs_gas: about 0.0149 TAO at 5 gwei. The top-up is a v2
// claim's (snledger::kClaimGasTao).
constexpr double kMinGasTao = 0.015;           // below this the SDK refuses a claim
constexpr double kSuggestedGasTao = 0.02;      // the top-up the dialog suggests
constexpr double kHeadDemotionMargin = 1.10;   // a score within 10% of the floor is warned
// The one untranslatable string on this surface (store key sn_alpha_symbol,
// translatable: false, so it is not in the catalog).
constexpr const char* kAlphaSymbol = "SN25α";
constexpr const char* kUrXyzUrl = "https://ur.xyz";
constexpr const char* kTop200Url = "https://ur.io/app/account/top200";
// The explorer used until the SDK's chain settings carry one.
constexpr const char* kExplorerTxUrlFallback = "https://evm.taostats.io/tx/";
// The preview sample's coldkey: base58-shaped (no 0/O/I/l), 48 characters,
// short form "5F3s…kQ9v" as in the design review. Obviously synthetic.
constexpr const char* kSampleColdkey = "5F3sSAMPLEsampeSAMPLEsampeSAMPLEsampeSAMPLE1kQ9v";
// The preview sample's Solana payout wallet: base58-shaped, 44 characters, short
// form "7Xk9…3fQa"; and the email's "3.87 USDC waiting". Obviously synthetic.
constexpr const char* kSampleSolanaAddress = "7Xk9SAMPLEsampeSAMPLEsampeSAMPLEsampeSAM3fQa";
constexpr int64_t kSamplePendingNanoCents = 3870000000;

// ---- presentation helpers ----------------------------------------------------

// Integer when it rounds clean, else two decimals; then hand-inserted thousands
// separators — locale-independent by design (the store owns the words, not the
// digits), leading '-' safe.
std::string FormatPointsValue(double value) {
  char buffer[64];
  if (std::fabs(value - std::round(value)) < 0.005) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  }
  std::string text = buffer;
  const size_t start = (!text.empty() && text[0] == '-') ? 1u : 0u;
  size_t end = text.find('.');
  if (end == std::string::npos) end = text.size();
  for (size_t insertAt = end; insertAt > start + 3;) {
    insertAt -= 3;
    text.insert(insertAt, ",");
  }
  return text;
}

// The SDK owns the render rules (parity across every app): "3.2410 SN25α"
// from rao, "0.71%" from basis points, "5F3s…kQ9v" for an ss58 address.
std::string FormatAlphaRao(int64_t rao) { return urnet::formatAlpha(rao); }

std::string FormatShareBps(int64_t shareBps) { return urnet::formatShareBps(shareBps); }

std::string ShortSs58(const std::string& address) { return urnet::shortSs58(address); }

std::string FormatTao(double tao) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4f", tao);
  return buffer;
}

// "0x9a1c…e07f" for the EVM gas key.
std::string ShortHex(const std::string& address) {
  if (address.size() <= 12) return address;
  return address.substr(0, 6) + "…" + address.substr(address.size() - 4);
}

std::string FormatMiB(double mibCount) {
  return FormatByteCountCompact(static_cast<int64_t>(mibCount * 1024.0 * 1024.0));
}

std::string Lowercase(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// ---- the SDK's stable error codes ------------------------------------------
// A coded failure arrives as "code" or "code: detail". The known codes map to
// the store's words; anything else is shown verbatim (often the only
// diagnostic).
std::string SnErrorCode(const std::string& text) {
  const size_t colon = text.find(':');
  std::string code = colon == std::string::npos ? text : text.substr(0, colon);
  while (!code.empty() && code.back() == ' ') code.pop_back();
  for (const char c : code) {
    if (!(std::islower(static_cast<unsigned char>(c)) || c == '_')) return {};
  }
  return code;
}

Glib::ustring SnErrorMessage(const std::string& text, const Glib::ustring& fallback) {
  const std::string code = SnErrorCode(text);
  if (code == "invalid_ss58_address") {
    return T_("invalid_ss58_address", "That is not a valid Bittensor address.");
  }
  if (code == "wallet_blocked") {
    return T_("wallet_blocked", "This wallet can't be used with URnetwork.");
  }
  if (code == "connect_wallet_first") {
    return T_("connect_wallet_first", "Connect a Bittensor wallet first.");
  }
  if (code == "chain_rpc_unreachable" || code == "chain_rpc_error") {
    return T_("chain_rpc_unreachable", "The chain RPC is unreachable. Try again.");
  }
  if (code == "needs_gas") return T_("add_tao_for_gas", "Add TAO for gas");
  if (text.empty()) return fallback;
  if (!code.empty()) {
    // a code the store has no words for: the detail, or the fallback
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon + 1 >= text.size()) return fallback;
    std::string detail = text.substr(colon + 1);
    while (!detail.empty() && detail.front() == ' ') detail.erase(detail.begin());
    return detail.empty() ? fallback : Glib::ustring(detail);
  }
  return Glib::ustring(text);
}

// ---- small typography factories --------------------------------------------

// A right-aligned body-face figure at an explicit size + SemiBold.
Gtk::Label* MakeStrongValue(int sizePx) {
  auto* label = Gtk::make_managed<Gtk::Label>();
  label->add_css_class("ur-value");
  label->set_xalign(1.f);
  label->set_valign(Gtk::Align::CENTER);
  label->set_ellipsize(Pango::EllipsizeMode::END);
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(sizePx * PANGO_SCALE);
  attrs.insert(size);
  auto weight = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
  attrs.insert(weight);
  label->set_attributes(attrs);
  return label;
}

// The condensed metric face (.ur-stat-value) at an explicit size.
Gtk::Label* MakeCondensedValue(const Glib::ustring& text, int sizePx, float xalign = 0.f) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class("ur-stat-value");
  label->set_xalign(xalign);
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(sizePx * PANGO_SCALE);
  attrs.insert(size);
  label->set_attributes(attrs);
  return label;
}

// A muted label at an explicit class that WRAPS (the note blocks on this
// surface wrap, unlike the kit's trimmed row note).
Gtk::Label* MakeWrappedNote(const Glib::ustring& text, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  label->set_ellipsize(Pango::EllipsizeMode::NONE);
  return label;
}

// A body-face line at an explicit size (sheet headings and values).
Gtk::Label* MakeSizedLabel(const Glib::ustring& text, int sizePx, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(sizePx * PANGO_SCALE);
  attrs.insert(size);
  label->set_attributes(attrs);
  return label;
}

// A label in the referral gold (a pango attribute, not a CSS class, so it
// outranks the value classes' own colour). Condensed = the metric face.
Gtk::Label* MakeGoldLabel(const Glib::ustring& text, int sizePx, bool condensed = false,
                          float xalign = 0.f) {
  auto* label = Gtk::make_managed<Gtk::Label>();
  if (condensed) label->add_css_class("ur-stat-value");
  label->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) + "' size='" +
                    std::to_string(sizePx * PANGO_SCALE) + "'>" +
                    Glib::Markup::escape_text(text) + "</span>");
  label->set_xalign(xalign);
  label->set_wrap(!condensed);
  return label;
}

// A 12sp muted caption over a condensed-22 value.
Gtk::Box* MakeStatCell(const Glib::ustring& label, const Glib::ustring& value) {
  auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  cell->set_hexpand(true);
  auto* caption = Gtk::make_managed<Gtk::Label>(label);
  caption->add_css_class("ur-caption");
  caption->set_xalign(0);
  caption->set_ellipsize(Pango::EllipsizeMode::END);
  cell->append(*caption);
  cell->append(*MakeCondensedValue(value, 22));
  return cell;
}

// A pane row whose height is its CONTENT (padding 12,N).
struct PaddedRow {
  Gtk::Box* root = nullptr;
  Gtk::Box* content = nullptr;
};
PaddedRow MakePaddedRow(int padY) {
  PaddedRow out;
  out.root = kit::MakePaneRow(0);
  out.content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  out.content->set_hexpand(true);
  out.content->set_margin_top(padY);
  out.content->set_margin_bottom(padY);
  if (auto* inner = dynamic_cast<Gtk::Box*>(out.root->get_first_child())) {
    inner->append(*out.content);
  }
  return out;
}

// The gold tile (unclaimed SN25α, Top 200): a rounded faint-gold fill with a
// gold hairline, laid out as a column.
Gtk::Box* MakeGoldTile() {
  auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  tile->add_css_class("ur-earn-gold-tile");
  tile->set_hexpand(true);
  return tile;
}

Gtk::Button* MakeGoldButton(const Glib::ustring& text) {
  auto* button = Gtk::make_managed<Gtk::Button>(text);
  button->add_css_class("ur-earn-gold-button");
  button->set_halign(Gtk::Align::START);
  return button;
}

// The user's zone's offset from UTC at `millis`, in minutes, by GLib's rules
// for that date (daylight time included); 0 when GLib cannot say.
int32_t LocalUtcOffsetMinutes(int64_t millis) {
  GDateTime* local = g_date_time_new_from_unix_local(millis / 1000);
  if (local == nullptr) return 0;
  const GTimeSpan offset = g_date_time_get_utc_offset(local);
  g_date_time_unref(local);
  return static_cast<int32_t>(offset / G_TIME_SPAN_MINUTE);
}

// A status chip on a history row / claim row: "unclaimed", "Claimed", ...
Gtk::Label* MakeStatusChip(const Glib::ustring& text) {
  auto* chip = Gtk::make_managed<Gtk::Label>(text);
  chip->add_css_class("ur-earn-tag");
  chip->set_valign(Gtk::Align::CENTER);
  return chip;
}

// The provide control mode's label; the mode strings are the store keys of
// their labels.
Glib::ustring ProvideModeValueText(const std::string& mode) {
  if (mode == "auto") return T_("auto", "Auto");
  if (mode == "always") return T_("always", "Always");
  if (mode == "network") return T_("network", "Network");
  return T_("never", "Never");
}

// A fixed-height pane row holding a transfer chart in its inset, the way
// ConnectPage::BuildPaneC adds its charts.
Gtk::Box* MakeChartRow(int height, TransferChart* chart) {
  auto* row = kit::MakePaneRow(height);
  chart->set_hexpand(true);
  chart->set_vexpand(true);
  if (auto* inner = dynamic_cast<Gtk::Box*>(row->get_first_child())) inner->append(*chart);
  return row;
}

// ---- the provider status's words (support part P008) --------------------------

static_assert(providerstatus::kProvideModePublic == urnet::ProvideModePublic,
              "the idle reason compares the live provide mode with the SDK's public mode");

Glib::ustring IdleReasonText(providerstatus::ProviderIdleReason reason) {
  switch (reason) {
    case providerstatus::ProviderIdleReason::None:
      return {};
    case providerstatus::ProviderIdleReason::AutoNotConnected:
      return T_("provider_idle_auto_not_connected",
                "Auto shares with everyone only while you're connected. Choose Always to earn "
                "while idle.");
    case providerstatus::ProviderIdleReason::NetworkOnly:
      return T_("provider_idle_network_only",
                "Shared only with your own devices. Choose Always to share with everyone.");
    case providerstatus::ProviderIdleReason::PausedWifiOnly:
      return T_("provider_idle_paused_wifi_only",
                "Paused: providing is set to Wi-Fi only, and this device isn't on Wi-Fi.");
    case providerstatus::ProviderIdleReason::PausedNoNetwork:
      return T_("provider_idle_paused_no_network",
                "Paused: this device can't provide on its current network.");
    case providerstatus::ProviderIdleReason::NoTrafficYet:
      return T_("provider_idle_no_traffic_yet",
                "New providers need several hours of steady uptime and a speed test before "
                "clients are sent to them. Traffic also depends on demand in your region.");
  }
  return {};
}

// provider_status_reason_<code>, for every code the server sends
Glib::ustring ServerReasonText(providerstatus::ServerReason reason) {
  switch (reason) {
    case providerstatus::ServerReason::Unknown:
      return {};
    case providerstatus::ServerReason::NotProviding:
      return T_("provider_status_reason_not_providing",
                "This device isn't set to share its connection.");
    case providerstatus::ServerReason::NotConnected:
      return T_("provider_status_reason_not_connected",
                "This device isn't connected right now. Clients are offered only connected "
                "devices.");
    case providerstatus::ServerReason::LocationInvalid:
      return T_("provider_status_reason_location_invalid",
                "This device is connecting from more than one address, or its location is "
                "unknown. Clients are offered only devices with one address and a known "
                "location.");
    case providerstatus::ServerReason::NetworkOnly:
      return T_("provider_status_reason_network_only",
                "This device provides only to your own devices. Choose Always to share with "
                "everyone.");
    case providerstatus::ServerReason::ReliabilityWarmingUp:
      return T_("provider_status_reason_reliability_warming_up",
                "Building reliability. This device has been steady for the last hour; clients "
                "are offered it once its 12-hour reliability reaches the minimum, about 8 hours "
                "after a fresh start.");
    case providerstatus::ServerReason::ReliabilityLow:
      return T_("provider_status_reason_reliability_low",
                "Reliability over the last hour is below what clients need. It rises with every "
                "minute this device stays connected; disconnects lower it.");
    case providerstatus::ServerReason::NotEligible:
      return T_("provider_status_reason_not_eligible",
                "This connection isn't eligible to provide right now.");
    case providerstatus::ServerReason::EgressUnprobed:
      return T_("provider_status_reason_egress_unprobed",
                "The network check hasn't run on this connection yet. Until it passes, clients "
                "are offered this device only when checked providers run out.");
    case providerstatus::ServerReason::EgressFailing:
      return T_("provider_status_reason_egress_failing",
                "Too many test sites failed to load through this connection in the last 8 "
                "hours. Clients are offered this device only when checked providers run out.");
    case providerstatus::ServerReason::SpeedTestMissing:
      return T_("provider_status_reason_speed_test_missing",
                "No speed test yet. Until one completes, clients are offered this device much "
                "less often.");
    case providerstatus::ServerReason::Slow:
      return T_("provider_status_reason_slow",
                "This connection measured slower than clients need, so it is offered much less "
                "often.");
    case providerstatus::ServerReason::None:
      return T_("provider_status_reason_none",
                "Everything checks out. How often clients are offered this device depends on "
                "demand in its region and on how it ranks against nearby providers.");
  }
  return {};
}

Glib::ustring NumberLabelText(providerstatus::NumberLabel label) {
  switch (label) {
    case providerstatus::NumberLabel::Reliability5m:
      return T_("provider_status_number_reliability_5m", "Reliability, last 5 minutes");
    case providerstatus::NumberLabel::Reliability1h:
      return T_("provider_status_number_reliability_1h", "Reliability, last hour");
    case providerstatus::NumberLabel::Reliability12h:
      return T_("provider_status_number_reliability_12h", "Reliability, last 12 hours");
    case providerstatus::NumberLabel::ReliabilityOther:
      return T_("reliability", "Reliability");
    case providerstatus::NumberLabel::UrlChecks:
      return T_("provider_status_number_url_checks", "Network checks");
    case providerstatus::NumberLabel::SpeedTest:
      return T_("provider_status_number_speed_test", "Speed test");
    case providerstatus::NumberLabel::Latency:
      return T_("provider_status_number_latency", "Delay");
    case providerstatus::NumberLabel::WeightQuality:
      return T_("provider_status_number_weight_quality", "Selection weight (quality)");
    case providerstatus::NumberLabel::WeightSpeed:
      return T_("provider_status_number_weight_speed", "Selection weight (speed)");
    case providerstatus::NumberLabel::TierQuality:
      return T_("provider_status_number_tier_quality", "Tier (quality)");
    case providerstatus::NumberLabel::TierSpeed:
      return T_("provider_status_number_tier_speed", "Tier (speed)");
    case providerstatus::NumberLabel::Country:
      return T_("country", "Country");
  }
  return {};
}

Glib::ustring NumberHelpText(providerstatus::NumberLabel label) {
  switch (label) {
    case providerstatus::NumberLabel::Reliability5m:
      return T_("provider_status_help_reliability_5m",
                "How steadily this device stayed connected over the last 5 minutes. It scales "
                "how often clients are offered this device; staying connected raises it.");
    case providerstatus::NumberLabel::Reliability1h:
      return T_("provider_status_help_reliability_1h",
                "How steadily this device stayed connected over the last hour. It must reach "
                "the minimum to be offered to clients; disconnects lower it and steady uptime "
                "raises it.");
    case providerstatus::NumberLabel::Reliability12h:
      return T_("provider_status_help_reliability_12h",
                "How steadily this device stayed connected over the last 12 hours. It must "
                "reach the minimum to be offered to clients; from a fresh start this takes "
                "about 8 hours of steady uptime.");
    case providerstatus::NumberLabel::ReliabilityOther:
      return T_("provider_status_help_reliability_other",
                "How steadily this device stayed connected over a longer window. It must reach "
                "the minimum to be offered to clients.");
    case providerstatus::NumberLabel::UrlChecks:
      return T_("provider_status_help_url_checks",
                "Test sites that loaded through this connection in the last 8 hours. Most must "
                "load; filtering by your internet provider lowers it.");
    case providerstatus::NumberLabel::SpeedTest:
      return T_("provider_status_help_speed_test",
                "Speed measured through this device. Faster connections are offered more "
                "often.");
    case providerstatus::NumberLabel::Latency:
      return T_("provider_status_help_latency",
                "Delay measured above what is expected for this location. Lower delay is "
                "offered more often.");
    case providerstatus::NumberLabel::WeightQuality:
    case providerstatus::NumberLabel::WeightSpeed:
      return T_("provider_status_help_weight",
                "Combines reliability, speed and passed network checks. Clients are offered "
                "providers with a higher weight more often.");
    case providerstatus::NumberLabel::TierQuality:
    case providerstatus::NumberLabel::TierSpeed:
      return T_("provider_status_help_tier",
                "Clients try lower tiers first. 0 is best; 3 means past the speed or delay "
                "cutoff.");
    case providerstatus::NumberLabel::Country:
      return T_("provider_status_help_country",
                "Clients who choose this country can be offered this device.");
  }
  return {};
}

// The store's value templates and the app's byte rate, for NumberRowsFor.
providerstatus::NumberWords ProviderStatusNumberWords() {
  providerstatus::NumberWords words;
  words.withMinimum = [](const std::string& value, const std::string& minimum) {
    return Format(T_("provider_status_value_with_minimum", "{0} (needs {1})"), value, minimum);
  };
  words.withMaximum = [](const std::string& value, const std::string& maximum) {
    return Format(T_("provider_status_value_with_maximum", "{0} (at most {1})"), value,
                  maximum);
  };
  words.countOfTotal = [](int64_t count, int64_t total) {
    return Format(T_("provider_status_value_count_of_total", "{0} of {1} loaded"), count, total);
  };
  words.rate = [](double bytesPerSecond) {
    return FormatByteRate(static_cast<int64_t>(std::llround(bytesPerSecond)));
  };
  words.notYet = T_("provider_status_value_not_yet", "Not yet");
  words.noHistory = T_("provider_status_value_no_history", "No history yet");
  words.notInPool = T_("provider_status_value_not_in_pool", "Not in this pool");
  return words;
}

// The controller's reading in the presentation's terms.
providerstatus::Poll ProviderStatusPollOf(const SdkHost::ProviderStatusSnapshot& snapshot) {
  providerstatus::Poll poll;
  poll.open = snapshot.open;
  poll.loaded = snapshot.loaded;
  poll.lastFetchError = snapshot.lastFetchError;
  if (!snapshot.status) return poll;
  const urnet::ProviderStatus& status = *snapshot.status;
  poll.hasStatus = true;
  poll.reason = status.reason;
  poll.reasonText = status.reason_text;
  if (status.appearances) {
    poll.hasAppearances = true;
    poll.appearancesPerMinute =
        status.appearances->appearances_per_minute.value_or(urnet::Int64List());
  }
  if (status.ranking) {
    for (const urnet::ProviderRankingNumber& number : *status.ranking) {
      providerstatus::RankingNumber out;
      out.name = number.name;
      out.hasValue = number.has_value;
      out.value = number.value;
      out.hasMinimum = number.has_minimum;
      out.minimum = number.minimum;
      out.hasMaximum = number.has_maximum;
      out.maximum = number.maximum;
      out.passes = number.passes;
      out.count = number.count;
      out.total = number.total;
      poll.ranking.push_back(out);
    }
  }
  if (status.country) {
    poll.country = providerstatus::Country{status.country->country_code, status.country->country};
  }
  return poll;
}

// A label whose text is a link: the whole line opens `url` in the browser.
Gtk::Label* MakeLinkLabel(const Glib::ustring& text, const std::string& url,
                          std::function<void(const std::string&)> open) {
  auto* label = Gtk::make_managed<Gtk::Label>();
  label->set_markup("<a href=\"" + Glib::Markup::escape_text(url) + "\">" +
                    Glib::Markup::escape_text(text) + "</a>");
  label->set_xalign(0);
  label->set_wrap(true);
  label->add_css_class("ur-key");
  label->signal_activate_link().connect(
      [open](const Glib::ustring& uri) -> bool {
        if (open) open(uri.raw());
        return true;  // handled; GTK must not launch a second time
      },
      false);
  return label;
}

// ---- points -----------------------------------------------------------------

struct PointsBreakdown {
  double net = 0;
  double providing = 0;
  double referral = 0;
  double multiplier = 0;
  double reliability = 0;
};

// The buckets, by the server's own event ids. NOTE the SDK naming trap:
// nanoPointsToPoints divides by 1e6, NOT 1e9 despite the name — always the SDK
// helper, never a hand-rolled divisor.
PointsBreakdown AggregatePoints(const urnet::AccountPointsList& points) {
  PointsBreakdown out;
  for (const auto& point : points) {
    const double value = urnet::nanoPointsToPoints(point.point_value);
    out.net += value;
    if (point.event == "payout") {
      out.providing += value;  // "payout" is the server's id for points from providing
    } else if (point.event == "payout_linked_account") {
      out.referral += value;
    } else if (point.event == "payout_multiplier") {
      out.multiplier += value;
    } else if (point.event == "payout_reliability") {
      out.reliability += value;
    }
  }
  return out;
}

// The points headline: the net figure over "net points earned", then the
// Providing / Referral / Reliability cells, then the Seeker multiplier row
// when one is earning — its points 2x never touches the alpha.
Gtk::Widget* BuildPointsBreakdown(const PointsBreakdown& points) {
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* total = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  total->append(*MakeCondensedValue(FormatPointsValue(points.net), 38));
  auto* caption = Gtk::make_managed<Gtk::Label>(T_("net_points_earned", "net points earned"));
  caption->add_css_class("ur-caption");
  caption->set_xalign(0);
  total->append(*caption);
  column->append(*total);

  auto* cells = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
  cells->set_homogeneous(true);
  cells->set_margin_top(12);
  cells->append(*MakeStatCell(T_("providing", "Providing"), FormatPointsValue(points.providing)));
  cells->append(*MakeStatCell(T_("referral", "Referral"), FormatPointsValue(points.referral)));
  cells->append(
      *MakeStatCell(T_("reliability", "Reliability"), FormatPointsValue(points.reliability)));
  column->append(*cells);

  if (points.multiplier > 0) {
    auto* rule = kit::MakeDivider();
    rule->set_margin_top(8);
    rule->set_margin_bottom(8);
    column->append(*rule);
    // GREEN, not gold — gold is the protocol's colour on this page
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    text->set_hexpand(true);
    auto* verified =
        MakeSizedLabel(T_("seeker_token_verified", "Seeker Token Verified!"), 14, "ur-value");
    verified->add_css_class("ur-value-on");
    text->append(*verified);
    text->append(*MakeSizedLabel(T_("you_re_earning_2x_points", "You're earning 2x points"),
                                 12, "ur-caption"));
    // points AND the free daily and referral data grants (server pro.yml
    // seeker.data_multiplier, subsidy seeker_holder_multiplier); never Pro
    text->append(*MakeSizedLabel(
        T_("seeker_multiplier_benefit",
           "Doubles your points, free daily data and referral data."),
        12, "ur-caption"));
    row->append(*text);
    auto* bonus = MakeCondensedValue(
        Format(T_("plus_amount", "+{}"), FormatPointsValue(points.multiplier)), 22, 1.f);
    bonus->set_valign(Gtk::Align::CENTER);
    row->append(*bonus);
    column->append(*row);
  }
  return column;
}

// ---- the reliability chart ---------------------------------------------------
// Three polylines on one canvas with INDEPENDENT scales: weights and the mean
// normalize against max(mean, max(weights)); clients against max(clients). The
// series are captured BY VALUE.
Gtk::Widget* MakeReliabilityChart(std::vector<double> weights, std::vector<double> clients,
                                  double mean) {
  auto* area = Gtk::make_managed<Gtk::DrawingArea>();
  area->set_content_height(kReliabilityChartHeight);
  area->set_hexpand(true);
  kit::MarkDecorative(*area);  // the legend beside it carries the words
  area->set_draw_func([weights, clients, mean](const Cairo::RefPtr<Cairo::Context>& cr,
                                               int width, int height) {
    const double w = static_cast<double>(width);
    const double h = static_cast<double>(height);
    double weightMax = mean;
    for (const double value : weights) weightMax = std::max(weightMax, value);
    double clientMax = 0.0;
    for (const double value : clients) clientMax = std::max(clientMax, value);

    auto plot = [&](const std::vector<double>& series, double scale, const Rgba& color,
                    double lineWidth, bool dashed) {
      if (series.size() < 2 || scale <= 0.0) return;
      cr->save();
      cr->set_line_width(lineWidth);
      cr->set_source_rgba(color.r, color.g, color.b, color.a);
      if (dashed) cr->set_dash(std::vector<double>{5.0, 3.0}, 0.0);
      const double step = w / static_cast<double>(series.size() - 1);
      for (size_t index = 0; index < series.size(); ++index) {
        const double norm = std::min(1.0, std::max(0.0, series[index] / scale));
        const double x = step * static_cast<double>(index);
        const double y = h - norm * h;
        if (index == 0) {
          cr->move_to(x, y);
        } else {
          cr->line_to(x, y);
        }
      }
      cr->stroke();
      cr->restore();
    };

    if (weightMax > 0.0 && weights.size() >= 2) {
      plot(std::vector<double>(weights.size(), mean), weightMax, kUrTextMuted, 1.5, true);
    }
    plot(clients, clientMax, kUrGreen, 2.0, false);
    plot(weights, weightMax, kUrPink, 2.0, false);
  });
  return area;
}

// One legend entry: an 8px dot in the series colour beside its 12sp word.
Gtk::Widget* MakeChartLegendEntry(const Rgba& color, const Glib::ustring& label) {
  auto* entry = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* dot = Gtk::make_managed<Gtk::Label>();
  dot->set_markup("<span size='" + std::to_string(8 * PANGO_SCALE) + "' foreground='" +
                  HexForMarkup(color) + "'>●</span>");
  dot->set_valign(Gtk::Align::CENTER);
  kit::MarkDecorative(*dot);
  entry->append(*dot);
  auto* text = Gtk::make_managed<Gtk::Label>(label);
  text->add_css_class("ur-caption");
  entry->append(*text);
  return entry;
}

// ---- the SDK earnings surface ------------------------------------------------
// Every call the points history, the wallet and the subnet layer make goes
// through these thin functions, so the page reads the same whichever host
// object answers: the Api (server-backed: epochs, the wallet setting, the
// head-spot estimate, the unauthenticated validate call) or the device (the
// SDK's sn state in THIS process: the gas key, the vault reads, the claim
// transaction). Callbacks may land on any thread; the page marshals them with
// PostToMain and drops them when its epoch moved.
namespace sn {

// The device (its gas key and vault client) exists once the tunnel has been
// started in this session; before that the subnet layer can read the wallet
// but cannot claim.
constexpr const char* kNoDevice = "no device";

bool ValidateSs58(const std::string& address) { return urnet::validateSs58(address); }

std::string ExplorerTxUrl(SdkHost& host, const std::string& txHash) {
  if (txHash.empty()) return {};
  std::string pattern;
  std::optional<urnet::SnChainSettings> settings;
  if (host.DeviceSteady()) settings = host.device().getSnChainSettings();
  if (!settings) settings = urnet::defaultSnChainSettings();
  if (settings) pattern = settings->explorer_tx_url;
  const size_t at = pattern.find("%s");
  if (pattern.empty() || at == std::string::npos) {
    return std::string(kExplorerTxUrlFallback) + txHash;
  }
  return pattern.substr(0, at) + txHash + pattern.substr(at + 2);
}

bool ClaimsAvailable(SdkHost& host) { return host.hasDevice(); }

// "code: detail" for a coded SnError (the page and the claim sheet map the
// stable codes to the store's words), else the transport error, else fallback.
std::string ErrorText(const std::optional<urnet::SnError>& error, const std::optional<std::string>& err,
                      const char* fallback) {
  if (error) {
    const std::string code = error->code.value_or(std::string());
    if (!code.empty()) return error->message.empty() ? code : code + ": " + error->message;
    if (!error->message.empty()) return error->message;
  }
  if (err && !err->empty()) return *err;
  return fallback;
}

using EpochsDone =
    std::function<void(std::optional<std::vector<AccountEpochRow>> epochs, std::string err)>;
void FetchEpochs(SdkHost& host, EpochsDone done) {
  host.api().accountEpochs([done](std::optional<urnet::AccountEpochsResult> result,
                                  std::optional<std::string> err) {
    if (err || !result || result->error) {
      done(std::nullopt, ErrorText(result ? result->error : std::nullopt, err, "no result"));
      return;
    }
    std::vector<AccountEpochRow> rows;
    if (result->epochs) {
      for (const auto& epoch : *result->epochs) {
        AccountEpochRow row;
        row.epoch = epoch.epoch;
        row.startMillis = epoch.start_millis;
        row.endMillis = epoch.end_millis;
        row.points = epoch.points;
        row.shareBps = epoch.share_bps;
        rows.push_back(row);
      }
    }
    done(std::move(rows), std::string());
  });
}

SnWalletInfo ToWalletInfo(const urnet::SnWallet& wallet) {
  SnWalletInfo out;
  out.coldkeySs58 = wallet.coldkey_ss58;
  out.clientId = wallet.client_id.value_or(std::string());
  out.setAtMillis = wallet.set_at_millis;
  return out;
}

// The wallet that counts for THIS device: the one attached to its provider
// client, else the network-level one. ok=false is a transport/server failure;
// ok=true with no wallet is the definite answer "none attached".
using WalletDone =
    std::function<void(bool ok, std::optional<SnWalletInfo> wallet, std::string err)>;
void FetchWallet(SdkHost& host, const std::string& clientId, WalletDone done) {
  host.api().snGetWallet([done, clientId](std::optional<urnet::SnGetWalletResult> result,
                                          std::optional<std::string> err) {
    if (err || !result || result->error) {
      done(false, std::nullopt, ErrorText(result ? result->error : std::nullopt, err, "no result"));
      return;
    }
    std::optional<SnWalletInfo> chosen;
    if (!clientId.empty() && result->wallets) {
      for (const auto& wallet : *result->wallets) {
        if (wallet.client_id.value_or(std::string()) == clientId && !wallet.coldkey_ss58.empty()) {
          chosen = ToWalletInfo(wallet);
          break;
        }
      }
    }
    if (!chosen && result->wallet && !result->wallet->coldkey_ss58.empty()) {
      chosen = ToWalletInfo(*result->wallet);
    }
    done(true, std::move(chosen), std::string());
  });
}

using HeadDone = std::function<void(std::optional<SnHeadInfo> head, std::string err)>;
void FetchHead(SdkHost& host, HeadDone done) {
  host.api().snHead([done](std::optional<urnet::SnHeadResult> result,
                           std::optional<std::string> err) {
    if (err || !result || result->error) {
      done(std::nullopt, ErrorText(result ? result->error : std::nullopt, err, "no result"));
      return;
    }
    SnHeadInfo head;
    head.eligible = result->eligible;
    head.score = result->score;
    head.floor = result->floor;
    head.rankEstimate = result->rank_estimate;
    head.cutoff = result->cutoff > 0 ? result->cutoff : 200;
    head.bound = result->bound;
    head.hotkey = result->hotkey.value_or(std::string());
    head.uid = result->uid.value_or(0);
    head.rank = result->rank.value_or(0);
    head.epoch = result->epoch;
    head.source = result->source;
    done(std::move(head), std::string());
  });
}

// The vault's view of this network's epochs, read by the SDK in this process
// (eth_call + the published payout artifact), after the wallet cache is
// synced from the server so the scan starts at the wallet's first epoch.
// The claims come with the current epoch's schedule (the coordinator's policy)
// when the SDK could read it.
using ClaimsDone = std::function<void(std::optional<std::vector<SnClaimRow>> claims,
                                      int64_t totalClaimableRao, std::string err,
                                      std::optional<snpayout::EpochSchedule> schedule)>;
void FetchClaims(SdkHost& host, ClaimsDone done) {
  if (!host.DeviceSteady()) {
    done(std::nullopt, 0, kNoDevice, std::nullopt);
    return;
  }
  urnet::DeviceRemote& device = host.device();
  // The vault / coordinator addresses are not in any repo: the SDK's defaults
  // ship them EMPTY and SnClaims answers chain_not_configured until the chain
  // settings were synced from GET /sn/epoch once. Then the wallet cache (the
  // scan starts at the wallet's first epoch), then the vault read.
  device.syncSnChainSettings([&device, done](std::optional<urnet::SnEpochResult> epoch,
                                             std::optional<std::string> epochErr) {
    if (epochErr || !epoch) {
      g_message("earnings: sn chain settings sync failed: %s",
                epochErr ? epochErr->c_str() : "(no result)");
    }
    device.syncSnWallet([&device, done](std::optional<urnet::SnGetWalletResult> synced,
                                        std::optional<std::string> syncErr) {
      (void)synced;
      (void)syncErr;  // a stale cache still scans; the server answer is best effort
      device.snClaims([done](std::optional<urnet::SnClaimsResult> result,
                             std::optional<std::string> err) {
      if (err || !result || result->error) {
        done(std::nullopt, 0, ErrorText(result ? result->error : std::nullopt, err, "no result"),
             std::nullopt);
        return;
      }
      std::vector<SnClaimRow> rows;
      if (result->claims) {
        for (const auto& claim : *result->claims) {
          SnClaimRow row;
          row.epoch = claim.epoch;
          row.shareBps = claim.share_bps;
          row.amountRao = claim.amount_rao;
          row.status = claim.status;
          row.claimOpenBlock = claim.claim_open_block;
          row.expiryBlock = claim.expiry_block;
          row.txHash = claim.tx_hash.value_or(std::string());
          row.message = claim.message.value_or(std::string());
          rows.push_back(row);
        }
      }
      std::optional<snpayout::EpochSchedule> schedule;
      if (result->schedule) {
        snpayout::EpochSchedule s;
        s.epoch = result->schedule->epoch;
        s.endMillis = result->schedule->end_millis;
        s.claimOpenMillis = result->schedule->claim_open_millis;
        s.expiryMillis = result->schedule->expiry_millis;
        schedule = s;
      }
      done(std::move(rows), result->total_claimable_rao, std::string(), schedule);
      });
    });
  });
}

using GasDone = std::function<void(std::optional<SnGasInfo> gas, std::string err)>;
void FetchGas(SdkHost& host, GasDone done) {
  if (!host.DeviceSteady()) {
    done(std::nullopt, kNoDevice);
    return;
  }
  urnet::DeviceRemote& device = host.device();
  auto key = device.getSnGasKey();  // creates the key on first use
  if (!key || key->address.empty()) {
    done(std::nullopt, "no gas key");
    return;
  }
  SnGasInfo gas;
  gas.address = key->address;
  gas.mirrorSs58 = key->mirror_ss58;
  device.snGasBalance([done, gas](std::optional<urnet::SnGasBalanceResult> result,
                                  std::optional<std::string> err) mutable {
    if (!err && result && !result->error) {
      gas.balanceKnown = true;
      gas.tao = result->tao;
    }
    // the key is still useful without a balance: the mirror address funds it
    done(std::move(gas), err.value_or(std::string()));
  });
}

// POST /sn/wallet/validate — unauthenticated; the address goes nowhere else.
using CheckDone = std::function<void(std::optional<SnWalletCheck> check, std::string err)>;
void CheckWallet(SdkHost& host, const std::string& address, CheckDone done) {
  host.api().snValidateWallet(
      address, [done](std::optional<urnet::SnValidateWalletResult> result,
                      std::optional<std::string> err) {
        if (err || !result || result->error) {
          done(std::nullopt, ErrorText(result ? result->error : std::nullopt, err, "no result"));
          return;
        }
        SnWalletCheck check;
        check.validSyntax = result->valid_syntax;
        check.existsOnChain = result->exists_on_chain;
        check.banned = result->banned;
        check.message = result->message.value_or(std::string());
        done(std::move(check), std::string());
      });
}

// Attach the coldkey: through the device when one is bound (the SDK sets it
// with this device's client id, then caches and notifies), else the plain
// network-level set through the Api.
using SetWalletDone = std::function<void(bool ok, std::string err)>;
void SetWallet(SdkHost& host, const std::string& address, const std::string& clientId,
               const std::string& signature, const std::string& message, SetWalletDone done) {
  if (host.DeviceSteady() && !clientId.empty()) {
    host.device().connectSnWallet(
        address, signature, message,
        [done](std::optional<urnet::SnConnectWalletResult> result,
               std::optional<std::string> err) {
          if (err || !result || result->error) {
            done(false, ErrorText(result ? result->error : std::nullopt, err, "no result"));
            return;
          }
          done(true, std::string());
        });
    return;
  }
  urnet::SnSetWalletArgs args;
  args.coldkey_ss58 = address;
  if (!clientId.empty()) args.client_id = clientId;
  args.signature = signature;
  args.message = message;
  host.api().snSetWallet(args, [done](std::optional<urnet::SnSetWalletResult> result,
                                      std::optional<std::string> err) {
    if (err || !result || result->error) {
      std::string detail = err.value_or(std::string());
      if (detail.empty() && result && result->error) {
        // "code: message" for a coded refusal, as ErrorText reads the device's
        const std::string code = result->error->code.value_or(std::string());
        detail = code.empty() ? result->error->message : code + ": " + result->error->message;
      }
      done(false, detail.empty() ? std::string("no result") : detail);
      return;
    }
    done(true, std::string());
  });
}

struct ClaimEvents {
  std::function<void(int64_t epoch, std::string txHash)> sent;
  std::function<void(int64_t epoch, std::string txHash, int64_t amountRao)> confirmed;
  std::function<void(int64_t epoch, std::string message)> failed;
  std::function<void()> done;
};
// The claim itself: the SDK builds claim(epoch, noId, coldkey, shareBps, proof),
// signs with the gas key and sends it to the vault; the events arrive as the
// receipts do.
void Claim(SdkHost& host, const std::vector<int64_t>& epochs, ClaimEvents events) {
  if (!host.DeviceSteady()) {
    for (const int64_t epoch : epochs) {
      if (events.failed) events.failed(epoch, kNoDevice);
    }
    if (events.done) events.done();
    return;
  }
  urnet::SnClaimCallback callback;
  callback.sent = events.sent;
  callback.confirmed = events.confirmed;
  callback.failed = events.failed;
  callback.done = events.done;
  host.device().snClaim(urnet::Int64List(epochs.begin(), epochs.end()), callback);
}

// Settlement v2: the coldkey's accounts, read by the SDK in this process from
// the operator's ledger (GET /sn/ledger/account) once the wallet cache is
// synced, as for the v1 claims: the read is the cached coldkey's. The chain
// settings say whether the release configures v2 claims: the v2 vault's
// address, which the release pins and no server sets. Any failure is the
// ledger unavailable: the page shows no balance rather than a zero.
using LedgerDone = std::function<void(std::optional<std::vector<snledger::Account>> accounts,
                                      bool claimConfigured, std::string err)>;
void FetchLedger(SdkHost& host, LedgerDone done) {
  if (!host.DeviceSteady()) {
    done(std::nullopt, false, kNoDevice);
    return;
  }
  urnet::DeviceRemote& device = host.device();
  device.syncSnWallet([&device, done](std::optional<urnet::SnGetWalletResult> synced,
                                      std::optional<std::string> syncErr) {
    (void)synced;
    (void)syncErr;  // a stale cache still reads
    device.snLedgerAccounts([&device, done](std::optional<urnet::SnLedgerAccountsResult> result,
                                            std::optional<std::string> err) {
      const std::optional<urnet::SnChainSettings> settings = device.getSnChainSettings();
      const bool configured = settings && !settings->vault2_address.empty();
      // a result without its list is as unreadable as an error: never a zero
      if (err || !result || result->error || !result->accounts) {
        done(std::nullopt, configured,
             ErrorText(result ? result->error : std::nullopt, err, "no accounts"));
        return;
      }
      std::vector<snledger::Account> accounts;
      for (const auto& account : *result->accounts) {
        snledger::Account row;
        row.vault = account.vault;
        row.noId = account.no_id;
        row.index = account.index;
        row.finalizedRao = account.finalized_rao;
        row.claimableRao = account.claimable_rao;
        row.paidRao = account.paid_rao;
        row.pending = account.pending;
        row.pendingRao = account.pending_rao;
        accounts.push_back(row);
      }
      done(std::move(accounts), configured, std::string());
    });
  });
}

// The v2 claim: the SDK claims every finalized v2 account of the coldkey whose
// claimable it verified against the v2 vault, one transaction each, signed with
// the gas key, and reports each as the receipts arrive, then Done.
void LedgerClaim(SdkHost& host, urnet::SnLedgerClaimCallback callback) {
  if (!host.DeviceSteady()) {
    if (callback.done) callback.done();
    return;
  }
  host.device().snLedgerClaim(std::move(callback));
}

// The network payout consent (NetworkConsent.hpp, CONSENT.md §9): the Api's
// three consent calls, which send the network credential (the challenge and the
// submit refuse "network_owner_only: ..." without one). Every error keeps the
// SDK's "code: message" form, a refused submit's code included, so the flow
// reads network_owner_only, challenge_expired and signature_mismatch.
using ConsentStatusDone =
    std::function<void(std::optional<consent::Status> status, std::string err)>;
void FetchConsentStatus(SdkHost& host, ConsentStatusDone done) {
  host.api().snNetworkConsentStatus([done](std::optional<urnet::SnNetworkConsentStatus> result,
                                           std::optional<std::string> err) {
    if (err || !result) {
      done(std::nullopt, err.value_or(std::string("no status")));
      return;
    }
    consent::Status status;
    status.active = result->active;
    status.coldkeySs58 = result->coldkey_ss58;
    status.fromEpoch = result->from_epoch;
    status.throughEpoch = result->through_epoch;
    done(std::move(status), std::string());
  });
}

using ConsentChallengeDone =
    std::function<void(std::optional<consent::Challenge> challenge, std::string err)>;
void RequestConsentChallenge(SdkHost& host, const std::string& coldkeySs58,
                             ConsentChallengeDone done) {
  host.api().snNetworkConsentChallenge(
      coldkeySs58, [done](std::optional<urnet::SnNetworkConsentChallengeResult> result,
                          std::optional<std::string> err) {
        if (err || !result || result->message.empty()) {
          done(std::nullopt, err.value_or(std::string("no challenge")));
          return;
        }
        consent::Challenge challenge;
        challenge.message = result->message;
        challenge.coldkeySs58 = result->coldkey_ss58;
        challenge.fromEpoch = result->from_epoch;
        challenge.throughEpoch = result->through_epoch;
        challenge.expiresAt = result->expires_at;
        done(std::move(challenge), std::string());
      });
}

// The challenge back as the SDK's own, every field as it came, for the
// consent session.
urnet::SnNetworkConsentChallengeResult ToSdkChallenge(const consent::Challenge& challenge) {
  urnet::SnNetworkConsentChallengeResult result;
  result.message = challenge.message;
  result.coldkey_ss58 = challenge.coldkeySs58;
  result.from_epoch = challenge.fromEpoch;
  result.through_epoch = challenge.throughEpoch;
  result.expires_at = challenge.expiresAt;
  return result;
}

using ConsentSubmitDone = std::function<void(bool ok, std::string err)>;
void SubmitConsent(SdkHost& host, const std::string& coldkeySs58, const std::string& message,
                   const std::string& signature, ConsentSubmitDone done) {
  host.api().snNetworkConsentSubmit(
      coldkeySs58, message, signature,
      [done](std::optional<urnet::SnSetWalletResult> result, std::optional<std::string> err) {
        if (err || !result || result->error) {
          std::string detail = err.value_or(std::string());
          if (detail.empty() && result && result->error) {
            // signature_mismatch: a signature that does not verify for the coldkey
            const std::string code = result->error->code.value_or(std::string());
            detail = code.empty() ? result->error->message : code + ": " + result->error->message;
          }
          done(false, detail.empty() ? std::string("no result") : detail);
          return;
        }
        done(true, std::string());
      });
}

}  // namespace sn

// The claim dialogs' gas key and, when it cannot pay a claim, the top-up: the
// address to send about `suggestedTao` TAO to, with Copy. The panel is rebuilt
// with its button, so a pending "Copied!" reset goes with the old one.
void BuildGasPanel(Gtk::Box& panel, Gtk::Window& sheet, const std::optional<SnGasInfo>& gas,
                   bool needsGas, double suggestedTao, sigc::connection& copiedReset) {
  copiedReset.disconnect();
  RemoveAllChildren(panel);
  auto* keyLine = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* key = MakeSizedLabel(T_("gas_key", "Gas key"), 12, "ur-caption");
  key->set_hexpand(true);
  keyLine->append(*key);
  Glib::ustring value = "-";
  if (gas) {
    value = ShortHex(gas->address);
    if (gas->balanceKnown) {
      value += Glib::ustring(" · ") +
               Format(T_("tao_amount_linux", "{} TAO"), FormatTao(gas->tao));
    }
  }
  auto* valueLabel = MakeSizedLabel(value, 13, "ur-value");
  valueLabel->set_wrap(false);
  if (gas) valueLabel->set_tooltip_text(gas->address);
  keyLine->append(*valueLabel);
  panel.append(*keyLine);

  if (!needsGas) return;
  // the needs-gas state: the mirror address is what the user must fund
  auto* box = MakeGoldTile();
  box->append(*MakeGoldLabel(T_("add_tao_for_gas", "Add TAO for gas"), 14));
  const std::string mirror = gas ? gas->mirrorSs58 : std::string();
  box->append(*MakeWrappedNote(
      Format(T_("send_tao_to_mirror", "Send about {0} TAO to {1} to cover gas."),
             FormatTao(suggestedTao), mirror.empty() ? std::string("-") : mirror),
      "ur-key"));
  if (!mirror.empty()) {
    auto* copy = Gtk::make_managed<Gtk::Button>(T_("copy", "Copy"));
    copy->add_css_class("flat");
    copy->set_halign(Gtk::Align::START);
    copy->signal_clicked().connect([&sheet, &copiedReset, copy, mirror] {
      sheet.get_clipboard()->set_text(mirror);
      copy->set_label(T_("copied", "Copied!"));
      copiedReset.disconnect();
      copiedReset = Glib::signal_timeout().connect(
          [copy]() -> bool {
            copy->set_label(T_("copy", "Copy"));
            return false;
          },
          kCopiedResetMs);
    });
    box->append(*copy);
  }
  panel.append(*box);
}

}  // namespace

// ---- ClaimAlphaSheet ---------------------------------------------------------
// The claim dialog. It OPENS AND READS with no session (the preview harness
// needs that); the one action is the claim, which the page gates. Every
// failure renders ON THE SHEET: a snackbar behind a modal is unreadable.
//
// States: claimable (the button carries the total) / needs gas (the gas key's
// mirror address with the suggested top-up, the button disabled) / sending /
// sent (the tx hash is the explorer link) / claimed / expired / failed.
class ClaimAlphaSheet : public Gtk::Window {
 public:
  ClaimAlphaSheet(Gtk::Window& parent, std::string coldkey, std::vector<SnClaimRow> claims,
                  std::optional<SnGasInfo> gas, bool claimsAvailable);
  ~ClaimAlphaSheet() override;

  std::function<void(std::vector<int64_t> epochs)> on_claim;  // page: StartClaim
  std::function<void(const std::string& url)> on_open_link;
  std::function<std::string(const std::string& txHash)> explorer_url;  // page: the chain settings

  void SetGas(std::optional<SnGasInfo> gas);
  void OnSending(const std::vector<int64_t>& epochs);
  void OnSent(int64_t epoch, const std::string& txHash);
  void OnConfirmed(int64_t epoch, const std::string& txHash, int64_t amountRao);
  void OnFailed(int64_t epoch, const std::string& message);
  void OnDone();
  void ShowError(const Glib::ustring& message);

 private:
  enum class Phase { Open, Claimable, Sending, Sent, Confirmed, Failed, Expired, Claimed };
  struct Row {
    SnClaimRow claim;
    Phase phase = Phase::Open;
    std::string txHash;
    Glib::ustring message;
  };
  static Phase PhaseForStatus(const std::string& status);
  Row* FindRow(int64_t epoch);
  std::vector<int64_t> ClaimableEpochs() const;
  int64_t ClaimableRao() const;
  bool NeedsGas() const;
  void RebuildRows();
  void RebuildGas();
  void RebuildAction();
  void OnClaimPressed();

  std::string coldkey_;
  std::vector<Row> rows_;
  std::optional<SnGasInfo> gas_;
  bool claimsAvailable_ = false;
  bool busy_ = false;
  bool gasFailure_ = false;  // a claim came back "needs gas"
  sigc::connection copiedReset_;
  Gtk::Label* totalValue_ = nullptr;
  Gtk::Label* totalCaption_ = nullptr;
  Gtk::Box* rowsPanel_ = nullptr;
  Gtk::Label* openNote_ = nullptr;
  Gtk::Box* gasPanel_ = nullptr;
  Gtk::Label* errorText_ = nullptr;
  Gtk::Button* claimButton_ = nullptr;
  Gtk::Button* closeButton_ = nullptr;
};

ClaimAlphaSheet::ClaimAlphaSheet(Gtk::Window& parent, std::string coldkey,
                                 std::vector<SnClaimRow> claims, std::optional<SnGasInfo> gas,
                                 bool claimsAvailable)
    : coldkey_(std::move(coldkey)), gas_(std::move(gas)), claimsAvailable_(claimsAvailable) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("claim_alpha_title", "Claim SN25α"));
  set_default_size(kSheetMinWidth, -1);
  add_css_class("ur-sheet");

  for (auto& claim : claims) {
    if (claim.status == "not-finalized") continue;  // nothing to show for it yet
    Row row;
    row.claim = claim;
    row.phase = PhaseForStatus(claim.status);
    row.txHash = claim.txHash;
    if (row.phase == Phase::Open || row.phase == Phase::Expired) row.message = claim.message;
    rows_.push_back(std::move(row));
  }
  std::stable_sort(rows_.begin(), rows_.end(),
                   [](const Row& a, const Row& b) { return a.claim.epoch > b.claim.epoch; });

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  column->set_margin(24);
  column->set_size_request(kSheetMinWidth, -1);

  // 1. the total, in gold, over the epoch count
  totalValue_ = MakeGoldLabel({}, 36, /*condensed=*/true);
  column->append(*totalValue_);
  totalCaption_ = MakeSizedLabel({}, 12, "ur-caption");
  column->append(*totalCaption_);

  // 2. one row per epoch
  rowsPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  rowsPanel_->set_margin_top(6);
  column->append(*rowsPanel_);
  openNote_ = MakeSizedLabel(
      T_("claims_open_after_finalization",
         "Each epoch's claims open after its payout list is finalized and stay open until "
         "they expire."),
      12, "ur-caption");
  column->append(*openNote_);

  column->append(*kit::MakeDivider());

  // 3. where it lands and what pays for it
  auto* to = MakeSizedLabel(Format(T_("claim_to_address_linux", "To {}"), ShortSs58(coldkey_)),
                            13, "ur-value");
  to->set_tooltip_text(coldkey_);
  column->append(*to);
  gasPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  column->append(*gasPanel_);

  errorText_ = MakeSizedLabel({}, 12, "ur-danger-text");
  errorText_->set_visible(false);
  column->append(*errorText_);

  column->append(*kit::MakeDivider());

  // 4. the action and the plain statement of what it does
  claimButton_ = MakeGoldButton({});
  claimButton_->signal_clicked().connect([this] { OnClaimPressed(); });
  column->append(*claimButton_);
  column->append(*MakeSizedLabel(
      T_("claim_sends_from_device",
         "Your device sends the claim to the vault contract. Gas is paid in TAO from your "
         "gas key. Alpha lands on your coldkey."),
      12, "ur-caption"));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(kClaimSheetMaxHeight);
  scroller->set_child(*column);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->append(*scroller);
  closeButton_ = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  closeButton_->set_halign(Gtk::Align::END);
  closeButton_->set_margin(16);
  closeButton_->signal_clicked().connect([this] { set_visible(false); });
  root->append(*closeButton_);
  set_child(*root);

  RebuildRows();
  RebuildGas();
  RebuildAction();
  if (!claimsAvailable_) {
    ShowError(T_("claim_unavailable_linux", "Claiming is not available on this device yet."));
  }
}

ClaimAlphaSheet::~ClaimAlphaSheet() { copiedReset_.disconnect(); }

ClaimAlphaSheet::Phase ClaimAlphaSheet::PhaseForStatus(const std::string& status) {
  if (status == "claimable") return Phase::Claimable;
  if (status == "claimed") return Phase::Claimed;
  if (status == "expired") return Phase::Expired;
  return Phase::Open;
}

ClaimAlphaSheet::Row* ClaimAlphaSheet::FindRow(int64_t epoch) {
  for (auto& row : rows_) {
    if (row.claim.epoch == epoch) return &row;
  }
  return nullptr;
}

std::vector<int64_t> ClaimAlphaSheet::ClaimableEpochs() const {
  std::vector<int64_t> out;
  for (const auto& row : rows_) {
    // a failed claim stays claimable: the user may top up gas and try again
    if (row.phase == Phase::Claimable || row.phase == Phase::Failed) out.push_back(row.claim.epoch);
  }
  return out;
}

int64_t ClaimAlphaSheet::ClaimableRao() const {
  int64_t total = 0;
  for (const auto& row : rows_) {
    if (row.phase == Phase::Claimable || row.phase == Phase::Failed) total += row.claim.amountRao;
  }
  return total;
}

bool ClaimAlphaSheet::NeedsGas() const {
  if (gasFailure_) return true;
  return gas_ && gas_->balanceKnown && gas_->tao < kMinGasTao;
}

void ClaimAlphaSheet::RebuildRows() {
  RemoveAllChildren(*rowsPanel_);
  bool anyOpen = false;
  for (const auto& row : rows_) {
    if (row.phase == Phase::Open) anyOpen = true;
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    line->set_margin_top(6);
    line->set_margin_bottom(6);
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    left->set_hexpand(true);
    left->append(*MakeSizedLabel(
        Glib::ustring(Format(T_("epoch_row_title", "Epoch {}"), row.claim.epoch)) + " · " +
            Format(T_("epoch_share_of_block", "{} of block"), FormatShareBps(row.claim.shareBps)),
        13, "ur-value"));
    if (!row.message.empty()) {
      left->append(*MakeSizedLabel(row.message, 12,
                                   row.phase == Phase::Failed ? "ur-danger-text" : "ur-caption"));
    }
    if (!row.txHash.empty()) {
      const std::string url = explorer_url ? explorer_url(row.txHash) : std::string();
      if (!url.empty() && g_uri_is_valid(url.c_str(), G_URI_FLAGS_NONE, nullptr)) {
        auto* link = MakeLinkLabel(ShortHex(row.txHash), url, on_open_link);
        link->add_css_class("ur-caption");
        left->append(*link);
      } else {
        auto* hash = MakeSizedLabel(ShortHex(row.txHash), 12, "ur-caption");
        hash->set_tooltip_text(row.txHash);
        left->append(*hash);
      }
    }
    line->append(*left);
    auto* amount = MakeSizedLabel(FormatAlphaRao(row.claim.amountRao), 13, "ur-value");
    amount->set_valign(Gtk::Align::CENTER);
    amount->set_wrap(false);
    line->append(*amount);
    Glib::ustring chipText;
    const char* chipClass = nullptr;
    switch (row.phase) {
      case Phase::Open:
      case Phase::Claimable:
        chipText = T_("unclaimed", "Unclaimed");
        break;
      case Phase::Sending:
        chipText = T_("claim_sending_linux", "Sending…");
        break;
      case Phase::Sent:
        chipText = T_("claim_sent", "Sent");
        break;
      case Phase::Confirmed:
      case Phase::Claimed:
        chipText = T_("claim_confirmed", "Claimed");
        chipClass = "ur-value-on";
        break;
      case Phase::Failed:
        chipText = T_("claim_failed", "Failed");
        chipClass = "ur-danger-text";
        break;
      case Phase::Expired:
        chipText = T_("claim_expired", "Expired");
        break;
    }
    auto* chip = MakeStatusChip(chipText);
    if (chipClass != nullptr) chip->add_css_class(chipClass);
    line->append(*chip);
    rowsPanel_->append(*line);
  }
  openNote_->set_visible(anyOpen);
  totalValue_->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) + "' size='" +
                          std::to_string(36 * PANGO_SCALE) + "'>" +
                          Glib::Markup::escape_text(FormatAlphaRao(ClaimableRao())) + "</span>");
  const size_t count = ClaimableEpochs().size();
  totalCaption_->set_text(
      Format(T_("claim_across_epochs", "Across {} finalized epochs"), static_cast<int64_t>(count)));
}

void ClaimAlphaSheet::RebuildGas() {
  BuildGasPanel(*gasPanel_, *this, gas_, NeedsGas(), kSuggestedGasTao, copiedReset_);
}

void ClaimAlphaSheet::RebuildAction() {
  const int64_t rao = ClaimableRao();
  claimButton_->set_label(Format(T_("claim_amount_button", "Claim {}"), FormatAlphaRao(rao)));
  claimButton_->set_sensitive(claimsAvailable_ && !busy_ && rao > 0 && !NeedsGas());
  bool anyConfirmed = false;
  for (const auto& row : rows_) {
    if (row.phase == Phase::Confirmed) anyConfirmed = true;
  }
  closeButton_->set_label(anyConfirmed && !busy_ ? T_("done", "Done") : T_("close", "Close"));
}

void ClaimAlphaSheet::OnClaimPressed() {
  if (busy_) return;
  const std::vector<int64_t> epochs = ClaimableEpochs();
  if (epochs.empty()) return;
  ShowError({});
  if (!on_claim) {
    ShowError(T_("something_went_wrong", "Something went wrong."));
    return;
  }
  on_claim(epochs);
}

void ClaimAlphaSheet::SetGas(std::optional<SnGasInfo> gas) {
  gas_ = std::move(gas);
  RebuildRows();  // the explorer resolver may have been wired after construction
  RebuildGas();
  RebuildAction();
}

void ClaimAlphaSheet::OnSending(const std::vector<int64_t>& epochs) {
  busy_ = true;
  gasFailure_ = false;
  for (const int64_t epoch : epochs) {
    if (Row* row = FindRow(epoch)) {
      row->phase = Phase::Sending;
      row->message.clear();
    }
  }
  RebuildRows();
  RebuildGas();
  RebuildAction();
}

void ClaimAlphaSheet::OnSent(int64_t epoch, const std::string& txHash) {
  if (Row* row = FindRow(epoch)) {
    row->phase = Phase::Sent;
    row->txHash = txHash;
  }
  RebuildRows();
}

void ClaimAlphaSheet::OnConfirmed(int64_t epoch, const std::string& txHash, int64_t amountRao) {
  if (Row* row = FindRow(epoch)) {
    row->phase = Phase::Confirmed;
    if (!txHash.empty()) row->txHash = txHash;
    if (amountRao > 0) row->claim.amountRao = amountRao;
  }
  RebuildRows();
  RebuildAction();
}

void ClaimAlphaSheet::OnFailed(int64_t epoch, const std::string& message) {
  // the SDK's messages start with one of its stable codes ("code: detail")
  const std::string code = SnErrorCode(message);
  const std::string lower = Lowercase(message);
  Row* row = FindRow(epoch);
  if (row != nullptr) {
    row->phase = Phase::Failed;
    if (code == "needs_gas" || (code.empty() && lower.find("gas") != std::string::npos)) {
      gasFailure_ = true;
      row->message = T_("add_tao_for_gas", "Add TAO for gas");
    } else if (code == "claims_for_epoch_expired" ||
               (code.empty() && lower.find("expired") != std::string::npos)) {
      row->phase = Phase::Expired;
      row->message = Format(T_("claims_for_epoch_expired", "Claims for epoch {} have expired."),
                            epoch);
    } else if (code == "already_claimed") {
      row->phase = Phase::Confirmed;  // the chain already holds it: nothing to retry
      row->message.clear();
    } else if (code == "chain_rpc_unreachable" || code == "chain_rpc_error" ||
               (code.empty() && (lower.find("rpc") != std::string::npos ||
                                 lower.find("unreachable") != std::string::npos))) {
      row->message = T_("chain_rpc_unreachable", "The chain RPC is unreachable. Try again.");
    } else {
      row->message = SnErrorMessage(message, T_("claim_failed", "Failed"));
    }
  } else if (!message.empty()) {
    ShowError(SnErrorMessage(message, T_("something_went_wrong", "Something went wrong.")));
  }
  RebuildRows();
  RebuildGas();
  RebuildAction();
}

void ClaimAlphaSheet::OnDone() {
  busy_ = false;
  RebuildAction();
}

void ClaimAlphaSheet::ShowError(const Glib::ustring& message) {
  if (!errorText_) return;
  kit::SetTextOrCollapse(*errorText_, message);
}

// ---- LedgerClaimSheet --------------------------------------------------------
// The settlement v2 claim dialog, drawn like the v1 one: the total in gold, one
// row per account (its pool, the amount, the progress and, once sent, the
// transaction's explorer link), where the alpha lands, the gas key with the
// needs-gas top-up, and the one action, which claims every claimable account
// (SnLedgerClaim). What the rows say is snledger::ClaimProgress's. Like the v1
// dialog it opens and reads with no session, and every failure renders ON THE
// SHEET.
class LedgerClaimSheet : public Gtk::Window {
 public:
  LedgerClaimSheet(Gtk::Window& parent, std::string coldkey,
                   const std::vector<snledger::Account>& accounts, std::optional<SnGasInfo> gas,
                   bool claimsAvailable);
  ~LedgerClaimSheet() override;

  std::function<void()> on_claim;  // page: StartLedgerClaim
  std::function<void(const std::string& url)> on_open_link;
  std::function<std::string(const std::string& txHash)> explorer_url;  // page: the chain settings

  void SetGas(std::optional<SnGasInfo> gas);
  void OnSending();
  void OnSent(int64_t noId, int64_t index, const std::string& txHash);
  void OnConfirmed(int64_t noId, int64_t index, const std::string& txHash, int64_t amountRao);
  void OnFailed(int64_t noId, int64_t index, const std::string& message);
  void OnDone();
  void ShowError(const Glib::ustring& message);

 private:
  bool NeedsGas() const;
  void Rebuild();

  std::string coldkey_;
  snledger::ClaimProgress progress_;
  std::optional<SnGasInfo> gas_;
  bool claimsAvailable_ = false;
  sigc::connection copiedReset_;
  Gtk::Label* totalValue_ = nullptr;
  Gtk::Box* rowsPanel_ = nullptr;
  Gtk::Box* gasPanel_ = nullptr;
  Gtk::Label* errorText_ = nullptr;
  Gtk::Button* claimButton_ = nullptr;
  Gtk::Button* closeButton_ = nullptr;
};

LedgerClaimSheet::LedgerClaimSheet(Gtk::Window& parent, std::string coldkey,
                                   const std::vector<snledger::Account>& accounts,
                                   std::optional<SnGasInfo> gas, bool claimsAvailable)
    : coldkey_(std::move(coldkey)),
      progress_(accounts),
      gas_(std::move(gas)),
      claimsAvailable_(claimsAvailable) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("claim_alpha_title", "Claim SN25α"));
  set_default_size(kSheetMinWidth, -1);
  add_css_class("ur-sheet");

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  column->set_margin(24);
  column->set_size_request(kSheetMinWidth, -1);

  // 1. the total, in gold, over which settlement pays it
  totalValue_ = MakeGoldLabel({}, 36, /*condensed=*/true);
  column->append(*totalValue_);
  column->append(*MakeSizedLabel(T_("sn_ledger_title", "Settlement v2"), 12, "ur-caption"));

  // 2. one row per account
  rowsPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  rowsPanel_->set_margin_top(6);
  column->append(*rowsPanel_);

  column->append(*kit::MakeDivider());

  // 3. where it lands and what pays for it
  auto* to = MakeSizedLabel(Format(T_("claim_to_address_linux", "To {}"), ShortSs58(coldkey_)),
                            13, "ur-value");
  to->set_tooltip_text(coldkey_);
  column->append(*to);
  gasPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  column->append(*gasPanel_);

  errorText_ = MakeSizedLabel({}, 12, "ur-danger-text");
  errorText_->set_visible(false);
  column->append(*errorText_);

  column->append(*kit::MakeDivider());

  // 4. the action and the plain statement of what it does
  claimButton_ = MakeGoldButton({});
  claimButton_->signal_clicked().connect([this] {
    if (progress_.Busy() || progress_.ClaimableRao() <= 0) return;
    ShowError({});
    if (!on_claim) {
      ShowError(T_("something_went_wrong", "Something went wrong."));
      return;
    }
    on_claim();
  });
  column->append(*claimButton_);
  column->append(*MakeSizedLabel(
      T_("claim_sends_from_device",
         "Your device sends the claim to the vault contract. Gas is paid in TAO from your "
         "gas key. Alpha lands on your coldkey."),
      12, "ur-caption"));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(kClaimSheetMaxHeight);
  scroller->set_child(*column);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->append(*scroller);
  closeButton_ = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  closeButton_->set_halign(Gtk::Align::END);
  closeButton_->set_margin(16);
  closeButton_->signal_clicked().connect([this] { set_visible(false); });
  root->append(*closeButton_);
  set_child(*root);

  Rebuild();
  if (!claimsAvailable_) {
    ShowError(T_("claim_unavailable_linux", "Claiming is not available on this device yet."));
  }
}

LedgerClaimSheet::~LedgerClaimSheet() { copiedReset_.disconnect(); }

bool LedgerClaimSheet::NeedsGas() const {
  return snledger::NeedsGas(progress_.NeedsGas(), gas_ && gas_->balanceKnown,
                            gas_ ? gas_->tao : 0.0);
}

void LedgerClaimSheet::Rebuild() {
  RemoveAllChildren(*rowsPanel_);
  for (const snledger::ClaimRow& row : progress_.Rows()) {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    line->set_margin_top(6);
    line->set_margin_bottom(6);
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    left->set_hexpand(true);
    left->append(*MakeSizedLabel(Format(T_("sn_ledger_pool", "Pool {}"), row.noId), 13,
                                 "ur-value"));
    if (row.phase == snledger::Phase::Failed) {
      // needs_gas reads "Add TAO for gas", with the top-up below
      left->append(*MakeSizedLabel(SnErrorMessage(row.message, T_("claim_failed", "Failed")), 12,
                                   "ur-danger-text"));
    }
    if (!row.txHash.empty()) {
      const std::string url = explorer_url ? explorer_url(row.txHash) : std::string();
      if (!url.empty() && g_uri_is_valid(url.c_str(), G_URI_FLAGS_NONE, nullptr)) {
        auto* link = MakeLinkLabel(ShortHex(row.txHash), url, on_open_link);
        link->add_css_class("ur-caption");
        left->append(*link);
      } else {
        auto* hash = MakeSizedLabel(ShortHex(row.txHash), 12, "ur-caption");
        hash->set_tooltip_text(row.txHash);
        left->append(*hash);
      }
    }
    line->append(*left);
    auto* amount = MakeSizedLabel(FormatAlphaRao(row.amountRao), 13, "ur-value");
    amount->set_valign(Gtk::Align::CENTER);
    amount->set_wrap(false);
    line->append(*amount);
    Glib::ustring chipText;
    const char* chipClass = nullptr;
    switch (row.phase) {
      case snledger::Phase::Claimable:
        chipText = T_("unclaimed", "Unclaimed");
        break;
      case snledger::Phase::Sending:
        chipText = T_("claim_sending_linux", "Sending…");
        break;
      case snledger::Phase::Sent:
        chipText = T_("claim_sent", "Sent");
        break;
      case snledger::Phase::Claimed:
        chipText = T_("claim_confirmed", "Claimed");
        chipClass = "ur-value-on";
        break;
      case snledger::Phase::Failed:
        chipText = T_("claim_failed", "Failed");
        chipClass = "ur-danger-text";
        break;
    }
    auto* chip = MakeStatusChip(chipText);
    if (chipClass != nullptr) chip->add_css_class(chipClass);
    line->append(*chip);
    rowsPanel_->append(*line);
  }
  const int64_t rao = progress_.ClaimableRao();
  totalValue_->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) + "' size='" +
                          std::to_string(36 * PANGO_SCALE) + "'>" +
                          Glib::Markup::escape_text(FormatAlphaRao(rao)) + "</span>");
  BuildGasPanel(*gasPanel_, *this, gas_, NeedsGas(), snledger::kClaimGasTao, copiedReset_);
  claimButton_->set_label(Format(T_("claim_amount_button", "Claim {}"), FormatAlphaRao(rao)));
  claimButton_->set_sensitive(claimsAvailable_ && !progress_.Busy() && rao > 0 && !NeedsGas());
  closeButton_->set_label(progress_.AnyClaimed() && !progress_.Busy() ? T_("done", "Done")
                                                                       : T_("close", "Close"));
}

void LedgerClaimSheet::SetGas(std::optional<SnGasInfo> gas) {
  gas_ = std::move(gas);
  Rebuild();
}

void LedgerClaimSheet::OnSending() {
  progress_.Start();
  Rebuild();
}

void LedgerClaimSheet::OnSent(int64_t noId, int64_t index, const std::string& txHash) {
  progress_.Sent(noId, index, txHash);
  Rebuild();
}

void LedgerClaimSheet::OnConfirmed(int64_t noId, int64_t index, const std::string& txHash,
                                   int64_t amountRao) {
  progress_.Confirmed(noId, index, txHash, amountRao);
  Rebuild();
}

void LedgerClaimSheet::OnFailed(int64_t noId, int64_t index, const std::string& message) {
  if (!progress_.Failed(noId, index, message)) {
    ShowError(SnErrorMessage(message, T_("something_went_wrong", "Something went wrong.")));
  }
  Rebuild();
}

void LedgerClaimSheet::OnDone() {
  progress_.Done();
  Rebuild();
}

void LedgerClaimSheet::ShowError(const Glib::ustring& message) {
  if (!errorText_) return;
  kit::SetTextOrCollapse(*errorText_, message);
}

// ---- EarningsPage ------------------------------------------------------------

EarningsPage::EarningsPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host), consent_(ConsentPorts()) {
  EnsureBrandCss();
  EnsureDrawerCss();
  // the consent section redraws with the wallet block, whose bridge it shares
  // (RebuildWalletBlock); a failure it does not word itself is the snackbar's
  consent_.on_changed = [this] { RebuildWalletBlock(); };
  consent_.on_error = [this](const std::string& error) {
    Notify(SnErrorMessage(error, T_("something_went_wrong", "Something went wrong.")),
           kit::Snackbar::Severity::Error);
  };
  // the wallet overflow menus, built once: one "earnings" action group on the
  // page, one menu per overflow (the popovers resolve the actions up the tree)
  walletActions_ = Gio::SimpleActionGroup::create();
  connectSolanaAction_ = walletActions_->add_action(
      "connect-solana", sigc::mem_fun(*this, &EarningsPage::OnConnectSolanaWallet));
  removeSolanaAction_ = walletActions_->add_action(
      "remove-solana", sigc::mem_fun(*this, &EarningsPage::OnRemoveSolanaWallet));
  insert_action_group("earnings", walletActions_);
  connectSolanaMenu_ = Gio::Menu::create();
  connectSolanaMenu_->append(T_("connect_solana_wallet", "Connect Solana wallet"),
                             "earnings.connect-solana");
  solanaCardMenu_ = Gio::Menu::create();
  solanaCardMenu_->append(T_("remove_wallet", "Remove wallet"), "earnings.remove-solana");
  BuildEarningsPane();
  append(*paneA_.root);
  ruleB_ = kit::MakePaneVRule();
  append(*ruleB_);

  BuildLedgerPane();
  // the points board's position indicator floats over the pane's scroller
  paneBOverlay_ = Gtk::make_managed<Gtk::Overlay>();
  paneBOverlay_->set_hexpand(true);
  paneBOverlay_->set_child(*paneB_.root);
  BuildPointsIndicator();
  append(*paneBOverlay_);
  ruleC_ = kit::MakePaneVRule();
  append(*ruleC_);

  BuildNetworkPane();
  append(*paneC_.root);
  // the statistics groups settle on what the host has now (nothing before a
  // session); the drawer feed keeps them current from there
  ApplyExtenderProvideState();
  PullProviderThroughput(/*forced=*/true);
  ApplyStatsSections();
  ApplyProviderStatus();
  // The provider status polls while this destination is on screen: the
  // mapped stack child of a presented window (ConnectPage's rule for its
  // clock). Hidden, the controller stops and keeps its last snapshot.
  signal_map().connect([this] { host_.SetProviderStatusPolling(true); });
  signal_unmap().connect([this] { host_.SetProviderStatusPolling(false); });

  // every panel opens on its LOADING state and the stat values on the faint
  // dash: an unloaded blank destination would read "there is nothing"
  SetStatValue(netProvidedValue_, T_("net_provided", "Net Provided"), {}, false);
  SetStatValue(rankValue_, T_("current_ranking", "Current Ranking"), {}, false);
  RebuildWalletBlock();
  RebuildUnclaimedTile();
  RebuildLedgerTile();
  RebuildTop200();
  RenderPointsHeader();
  RenderPointsFooter();
}

EarningsPage::~EarningsPage() {
  ++*epoch_;        // orphan every in-flight completion
  *alive_ = false;  // ... and every marshaled cleanup
  checkDebounce_.disconnect();
  connectFlow_.timer.disconnect();
  setWalletFlow_.timer.disconnect();
  claimFlow_.timer.disconnect();
  ledgerClaimFlow_.timer.disconnect();
  pointsPublicFlow_.timer.disconnect();
  pointsScrollConn_.disconnect();
  // controller only: by now the window's teardown may have disposed the
  // page's widgets, so nothing here may touch them (issue #13: rebuilding
  // the rows on a disposed Gtk::Box segfaulted every quit)
  ReleasePointsBoard(/*deviceAlive=*/true);
  rankingFlow_.timer.disconnect();
  legacyFlow_.timer.disconnect();
  removeSolanaFlow_.timer.disconnect();
  consentStep_.timer.disconnect();
  consent_.on_changed = nullptr;
  consent_.on_error = nullptr;
  sheet_.reset();
}

// ---- flows -------------------------------------------------------------------

uint32_t EarningsPage::BeginFlow(Flow& flow, int timeoutMs, std::function<void()> onTimeout) {
  flow.timer.disconnect();
  ++flow.generation;
  const uint32_t generation = flow.generation;
  flow.timer = Glib::signal_timeout().connect(
      [&flow, generation, onTimeout = std::move(onTimeout)]() -> bool {
        if (flow.generation != generation) return false;  // already settled
        // bump AGAIN so the give-up is final: a late answer must not undo what
        // the user was already told
        ++flow.generation;
        g_warning("earnings: a request never answered - giving up on it");
        if (onTimeout) onTimeout();
        return false;
      },
      timeoutMs);
  return generation;
}

bool EarningsPage::SettleFlow(Flow& flow, uint32_t generation, const char* what) {
  if (flow.generation != generation) {
    g_message("earnings: dropping a result for an abandoned %s", what);
    return false;
  }
  flow.timer.disconnect();
  return true;
}

// ---- gating + messaging ------------------------------------------------------

bool EarningsPage::CanCallApi() {
  // the preview gate is first for a reason: preview-mode actions once reached
  // production authenticated
  return !previewMode_ && host_.IsLoggedIn();
}

void EarningsPage::RefuseNoSession() {
  g_warning("earnings: refusing an action with no session");
  Notify(T_("please_login_to_urnetwork", "Please login to URnetwork"),
         kit::Snackbar::Severity::Error);
}

void EarningsPage::Notify(const Glib::ustring& message, kit::Snackbar::Severity severity) {
  const bool error = severity == kit::Snackbar::Severity::Error ||
                     severity == kit::Snackbar::Severity::Warning;
  // the message must land beside content the user is looking at
  const bool leaderboardShowing = leaderboardHost_ != nullptr && leaderboardHost_->get_visible();
  if (leaderboardShowing && paneC_.root != nullptr && paneC_.root->get_visible()) {
    leaderboardInfo_.Show(message, severity);
    return;
  }
  if (paneA_.root != nullptr && paneA_.root->get_visible()) {
    walletInfo_.Show(message, severity);
    return;
  }
  if (on_snackbar) {
    on_snackbar(message, error);
    return;
  }
  g_warning("earnings: no visible snackbar surface; dropping message: %s", message.c_str());
}

void EarningsPage::SetStatValue(Gtk::Label* value, const Glib::ustring& key,
                                const Glib::ustring& text, bool loaded) {
  if (value == nullptr) return;
  const Glib::ustring shown = loaded ? text : Glib::ustring("-");
  value->set_text(shown);
  if (loaded) {
    value->remove_css_class("ur-label-faint");
  } else {
    value->add_css_class("ur-label-faint");
  }
  kit::SetAccessibleLabel(*value, key + ", " + shown);
}

std::string EarningsPage::ProviderClientId() {
  return host_.hasDevice() ? host_.ClientId() : std::string();
}

void EarningsPage::OpenLink(const std::string& url) {
  GError* err = nullptr;
  if (!g_app_info_launch_default_for_uri(url.c_str(), nullptr, &err)) {
    g_warning("earnings: could not open %s: %s", url.c_str(), err ? err->message : "?");
    if (err) g_error_free(err);
    Notify(T_("something_went_wrong", "Something went wrong."), kit::Snackbar::Severity::Error);
  }
}

// ---- lifecycle ---------------------------------------------------------------

void EarningsPage::Load() {
  ++*epoch_;  // drop every completion armed for the previous session
  if (samplePinned_) return;
  LoadEarnings();
  LoadLegacyWallets();  // the Solana payout wallet: its own reads, never chained
  // the leaderboard is a one-shot per LOOK, never per PROCESS
  if (leaderboardTab_ != nullptr && leaderboardTab_->get_active()) {
    leaderboardRequested_ = true;
    LoadLeaderboard();
    if (pointsBoardShowing_) EnsurePointsBoard();
  } else {
    leaderboardRequested_ = false;
  }
}

void EarningsPage::ResetForSignOut() {
  // what marks the own row on the boards; the next load reads it from the jwt
  ownNetworkId_.clear();
  // A switch's set still in flight was the departed account's. Load's epoch
  // drops its answer, so it is abandoned here: its watchdog would otherwise
  // report it failed to whoever signs in next, and the switch would stay
  // insensitive until then.
  ++pointsPublicFlow_.generation;
  pointsPublicFlow_.timer.disconnect();
  settingPointsPublic_ = false;
  if (pointsPublicToggle_ != nullptr) pointsPublicToggle_->set_sensitive(true);
  ++rankingFlow_.generation;
  rankingFlow_.timer.disconnect();
  settingRankingPublic_ = false;
  publicToggle_->set_sensitive(true);
  // the points header's emoji and switch; with the edit markers, or the old
  // account's last edit would keep the next one's first `me` from applying
  pointsPublic_ = false;
  emojiTag_.clear();
  ownFlagsClock_ = 0;
  ownFlagsEditedAt_ = 0;
  ownFlagsAppliedAt_ = 0;
  SetPointsToggle(false);
  // the public leaderboard switch: ApplyRanking writes it only with a ranking
  rankingPublic_ = false;
  SetRankingToggle(false);
  Load();
}

void EarningsPage::ApplyBreakpoint(int widthDip) {
  const int lanes = widthDip >= kThreePaneDip ? 3 : (widthDip >= kTwoPaneDip ? 2 : 1);
  if (lanes_ == lanes) return;
  lanes_ = lanes;
  // the HISTORY survives to the smallest width
  paneA_.root->set_visible(lanes >= 2);
  ruleB_->set_visible(lanes >= 2);
  paneC_.root->set_visible(lanes >= 3);
  ruleC_->set_visible(lanes >= 3);
}

void EarningsPage::SetBalanceState(bool isPro, bool guest) {
  isPro_ = isPro;
  isGuest_ = guest;
}

void EarningsPage::SetPreviewMode(bool on) { previewMode_ = on; }

void EarningsPage::ShowPreviewState() { SettleAllEmpty(); }

void EarningsPage::ShowPreviewSnackbar() {
  Notify(T_("wallet_connect_failed", "Failed to connect the wallet."),
         kit::Snackbar::Severity::Error);
}

void EarningsPage::SettleAllEmpty() {
  // no session: no consent to show, and nothing in flight counts any more
  consent_.Reset();
  ApplyPoints(urnet::AccountPointsList{}, Fetch::Ready);
  ApplyEpochs(std::vector<AccountEpochRow>{}, Fetch::Ready);
  ApplySnWallet(std::nullopt, Fetch::Ready);
  ApplyClaims(std::vector<SnClaimRow>{}, 0, Fetch::Ready);
  ApplyGas(std::nullopt);
  ApplyHead(std::nullopt, Fetch::Ready);
  ApplyReliability(std::nullopt, Fetch::Ready);
  ApplyRanking(std::nullopt, false);
  // no session: nothing is known about any network's Solana payout wallet, and
  // the reads still out are refused
  legacyLoad_.Abandon();
  legacyCommitted_ = solana::LegacyCommitted{};
  RebuildSolanaCard();
  // the leaderboard is not this network's data: "no session" is not an answer
  // about it, so it must never be settled Ready+empty here (the preview
  // harness is the only caller that wants the REAL empty state)
  if (previewMode_) {
    ApplyLeaderboard(urnet::LeaderboardEarnersList{}, Fetch::Ready);
  } else {
    ApplyLeaderboard(std::nullopt, Fetch::NoSession);
  }
  SettlePointsBoardPreview();
}

// ---- PANE A: earnings (360) --------------------------------------------------

void EarningsPage::BuildEarningsPane() {
  paneA_ = kit::MakePane(T_("earnings", "Earnings"));
  paneA_.root->set_size_request(kPaneAWidth, -1);
  paneA_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneA_.root, T_("earnings", "Earnings"));
  Gtk::Box* content = paneA_.content;

  // 1. points earned — the headline, always
  {
    auto group = kit::MakePaneGroupHeader(T_("points_earned", "Points earned"),
                                          T_("loading", "Loading..."));
    pointsStatus_ = group.meta;
    content->append(*group.root);
  }
  {
    auto row = MakePaddedRow(12);
    pointsCard_ = row.root;
    pointsPanel_ = row.content;
    pointsCard_->set_visible(false);  // collapsed until Ready
    content->append(*row.root);
  }

  // 2. how and when SN payouts happen — collapsed until the coldkey is known
  {
    auto row = MakePaddedRow(10);
    row.content->set_spacing(6);
    payoutCard_ = row.root;
    payoutText_ = MakeWrappedNote({}, "ur-row-note");
    row.content->append(*payoutText_);
    payoutTimes_ = MakeWrappedNote({}, "ur-row-note");
    payoutTimes_->set_visible(false);
    row.content->append(*payoutTimes_);
    // the coldkey flow the Bittensor block's button opens
    setColdkeyButton_ = Gtk::make_managed<Gtk::Button>(T_("set_coldkey", "Set coldkey"));
    setColdkeyButton_->add_css_class("flat");
    setColdkeyButton_->set_halign(Gtk::Align::START);
    setColdkeyButton_->set_visible(false);
    setColdkeyButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConnectWithBridge));
    row.content->append(*setColdkeyButton_);
    // the claim sheet the unclaimed tile opens; the app never claims by itself
    payoutClaimButton_ = MakeGoldButton(T_("claim", "Claim"));
    payoutClaimButton_->set_visible(false);
    payoutClaimButton_->signal_clicked().connect(sigc::mem_fun(*this, &EarningsPage::OnClaim));
    row.content->append(*payoutClaimButton_);
    payoutCard_->set_visible(false);
    content->append(*row.root);
  }


  // 3. the unclaimed tile — wallet only
  {
    auto row = MakePaddedRow(10);
    unclaimedCard_ = row.root;
    auto* tile = MakeGoldTile();
    tile->append(*MakeGoldLabel(T_("unclaimed", "Unclaimed"), 12));
    unclaimedValue_ = MakeGoldLabel("-", 34, /*condensed=*/true);
    tile->append(*unclaimedValue_);
    unclaimedStatus_ = MakeWrappedNote({}, "ur-row-note");
    unclaimedStatus_->set_visible(false);
    tile->append(*unclaimedStatus_);
    claimButton_ = MakeGoldButton(T_("claim", "Claim"));
    claimButton_->set_margin_top(4);
    claimButton_->set_sensitive(false);
    claimButton_->signal_clicked().connect(sigc::mem_fun(*this, &EarningsPage::OnClaim));
    tile->append(*claimButton_);
    row.content->append(*tile);
    unclaimedCard_->set_visible(false);
    content->append(*row.root);
  }

  // 3b. settlement v2 — the coldkey's v2 accounts, apart from the v1 claims
  //     above (snledger::SectionFor decides what shows)
  {
    auto row = MakePaddedRow(10);
    ledgerCard_ = row.root;
    auto* tile = MakeGoldTile();
    tile->append(*MakeGoldLabel(T_("sn_ledger_title", "Settlement v2"), 12));
    ledgerRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    tile->append(*ledgerRows_);
    ledgerStatus_ = MakeWrappedNote({}, "ur-row-note");
    ledgerStatus_->set_visible(false);
    tile->append(*ledgerStatus_);
    ledgerClaimButton_ = MakeGoldButton(T_("claim", "Claim"));
    ledgerClaimButton_->set_margin_top(4);
    ledgerClaimButton_->set_sensitive(false);
    ledgerClaimButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnLedgerClaim));
    tile->append(*ledgerClaimButton_);
    row.content->append(*tile);
    ledgerCard_->set_visible(false);
    content->append(*row.root);
  }

  // 3c. the payout consent — one signature by the network's coldkey pays every
  //     provider client of the network (consent::Flow decides what shows)
  {
    auto group = kit::MakePaneGroupHeader(T_("sn_consent_title", "Payout consent"));
    consentHeader_ = group.root;
    consentHeader_->set_visible(false);
    content->append(*group.root);
  }
  {
    auto row = MakePaddedRow(12);
    row.content->set_spacing(6);
    consentCard_ = row.root;
    consentLine_ = MakeWrappedNote({}, "ur-key");
    row.content->append(*consentLine_);
    consentBody_ = MakeWrappedNote(
        T_("sn_consent_body",
           "Sign once with your Bittensor wallet so this network's earnings are paid to it."),
        "ur-row-note");
    row.content->append(*consentBody_);
    consentNotice_ = MakeWrappedNote({}, "ur-row-note");
    consentNotice_->add_css_class("ur-danger-text");
    consentNotice_->set_visible(false);
    row.content->append(*consentNotice_);
    consentSignButton_ = Gtk::make_managed<Gtk::Button>(T_("sn_consent_sign", "Sign with wallet"));
    consentSignButton_->add_css_class("ur-pane-primary");
    consentSignButton_->set_hexpand(true);
    consentSignButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConsentAction));
    row.content->append(*consentSignButton_);
    consentChangeButton_ = Gtk::make_managed<Gtk::Button>(T_("sn_consent_change", "Change wallet"));
    consentChangeButton_->add_css_class("flat");
    consentChangeButton_->set_halign(Gtk::Align::START);
    consentChangeButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConsentAction));
    row.content->append(*consentChangeButton_);
    consentChangeNote_ = MakeWrappedNote(
        T_("sn_consent_change_note", "A new wallet takes effect from the next epoch."),
        "ur-row-note");
    row.content->append(*consentChangeNote_);
    consentCard_->set_visible(false);
    content->append(*row.root);
  }

  // 4. the Bittensor wallet block
  {
    auto group = kit::MakePaneGroupHeader(T_("bittensor_wallet", "Bittensor wallet"),
                                          T_("loading", "Loading..."));
    walletStatus_ = group.meta;
    content->append(*group.root);
  }
  {
    // connected: the address, Change, and what being connected means
    auto row = MakePaddedRow(12);
    row.content->set_spacing(6);
    walletConnectedPanel_ = row.root;
    // the USDC waiting for a network with no Solana payout wallet (RebuildSolanaCard)
    usdcWaitingLineConnected_ = MakeWrappedNote({}, "ur-row-note");
    usdcWaitingLineConnected_->set_visible(false);
    row.content->append(*usdcWaitingLineConnected_);
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    walletAddressLabel_ = Gtk::make_managed<Gtk::Label>();
    walletAddressLabel_->add_css_class("ur-earn-address");
    walletAddressLabel_->set_xalign(0);
    walletAddressLabel_->set_hexpand(true);
    walletAddressLabel_->set_selectable(true);
    line->append(*walletAddressLabel_);
    changeWalletButton_ = Gtk::make_managed<Gtk::Button>(T_("change", "Change"));
    changeWalletButton_->add_css_class("flat");
    changeWalletButton_->set_valign(Gtk::Align::CENTER);
    changeWalletButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnChangeWallet));
    line->append(*changeWalletButton_);
    walletMoreConnected_ = BuildWalletOverflow(connectSolanaMenu_);
    line->append(*walletMoreConnected_);
    row.content->append(*line);
    row.content->append(*MakeWrappedNote(
        T_("wallet_connected_to_protocol",
           "Connected to the UR protocol. Claims land here. Alpha accrues from the next "
           "epoch after connecting."),
        "ur-row-note"));
    walletConnectedPanel_->set_visible(false);
    content->append(*row.root);
  }
  {
    // not connected (or changing): the bridge, and the manual entry behind it
    auto row = MakePaddedRow(12);
    row.content->set_spacing(10);
    walletConnectPanel_ = row.root;
    usdcWaitingLine_ = MakeWrappedNote({}, "ur-row-note");
    usdcWaitingLine_->set_visible(false);
    row.content->append(*usdcWaitingLine_);
    // plain note (not a link), then a "Learn more" link in the pink accent with
    // an outward arrow that opens the protocol site; inline, so it shares the
    // last line when it fits
    walletConnectNote_ = Gtk::make_managed<Gtk::Label>();
    walletConnectNote_->set_markup(
        Glib::Markup::escape_text(
            T_("wallet_not_retroactive",
               "Connect a wallet to earn SN25α from the next epoch. Earlier epochs are not "
               "settled retroactively.")) +
        "\xC2\xA0<a href=\"" + Glib::Markup::escape_text(kUrXyzUrl) + "\">" +
        Glib::Markup::escape_text(T_("learn_more", "Learn more")) + "\xC2\xA0\xE2\x86\x97</a>");
    walletConnectNote_->add_css_class("ur-row-note");
    walletConnectNote_->add_css_class("ur-learn-more");
    walletConnectNote_->set_xalign(0);
    walletConnectNote_->set_wrap(true);
    walletConnectNote_->set_ellipsize(Pango::EllipsizeMode::NONE);
    walletConnectNote_->signal_activate_link().connect(
        [this](const Glib::ustring& uri) -> bool {
          OpenLink(uri.raw());
          return true;  // handled; GTK must not launch a second time
        },
        false);
    row.content->append(*walletConnectNote_);
    connectBridgeButton_ =
        Gtk::make_managed<Gtk::Button>(T_("connect_bittensor_wallet", "Connect Bittensor wallet"));
    connectBridgeButton_->add_css_class("ur-pane-primary");
    connectBridgeButton_->set_hexpand(true);
    connectBridgeButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConnectWithBridge));
    // the Bittensor action and, beside it, the overflow that reaches the
    // Solana payout wallet
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->append(*connectBridgeButton_);
    walletMoreDisconnected_ = BuildWalletOverflow(connectSolanaMenu_);
    actions->append(*walletMoreDisconnected_);
    row.content->append(*actions);
    manualToggleButton_ =
        Gtk::make_managed<Gtk::Button>(T_("enter_address_manually", "Enter address manually"));
    manualToggleButton_->add_css_class("flat");
    manualToggleButton_->set_halign(Gtk::Align::START);
    manualToggleButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnToggleManualEntry));
    row.content->append(*manualToggleButton_);

    manualPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    walletAddressBox_ = Gtk::make_managed<Gtk::Entry>();
    walletAddressBox_->add_css_class("ur-input");
    walletAddressBox_->set_placeholder_text(T_("enter_wallet_address", "Enter wallet address"));
    kit::SetAccessibleLabel(*walletAddressBox_,
                            T_("enter_wallet_address", "Enter wallet address"));
    walletAddressBox_->signal_changed().connect(
        sigc::mem_fun(*this, &EarningsPage::OnWalletAddressChanged));
    manualPanel_->append(*walletAddressBox_);
    walletSupportingText_ = MakeWrappedNote({}, "ur-row-note");
    walletSupportingText_->set_visible(false);
    manualPanel_->append(*walletSupportingText_);
    manualPanel_->append(*MakeWrappedNote(
        T_("wallet_connect_signed_note_linux",
           "Your wallet signs a message to prove it is yours. Nothing is sent on chain."),
        "ur-row-note"));
    connectManualButton_ = Gtk::make_managed<Gtk::Button>(T_("connect", "Connect"));
    connectManualButton_->set_sensitive(false);  // nothing is validated yet
    connectManualButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConnectManual));
    manualPanel_->append(*connectManualButton_);
    manualPanel_->set_visible(false);
    row.content->append(*manualPanel_);

    // "waiting" must be VISIBLE — a silently greyed button is indistinguishable
    // from a broken one
    connectingStatus_ = MakeWrappedNote({}, "ur-row-note");
    connectingStatus_->set_visible(false);
    row.content->append(*connectingStatus_);
    content->append(*row.root);
  }

  // 4b. the Solana payout wallet: USDC payouts continue to it until the
  //     migration to Bittensor completes (hidden until the legacy reads land
  //     with a payout wallet)
  {
    auto row = MakePaddedRow(12);
    row.content->set_spacing(6);
    solanaCard_ = row.root;
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    // "Solana wallet", or "Wallet" for a legacy Polygon payout wallet (RebuildSolanaCard)
    solanaTitle_ = MakeSizedLabel(T_("solana_wallet", "Solana wallet"), 12, "ur-caption");
    solanaTitle_->set_hexpand(true);
    solanaTitle_->set_valign(Gtk::Align::CENTER);
    header->append(*solanaTitle_);
    // the card only ever shows the payout wallet, so the chip is always there
    solanaDefaultTag_ =
        MakeStatusChip(Glib::ustring(T_("default_wallet", "Default")).uppercase());
    header->append(*solanaDefaultTag_);
    solanaMore_ = BuildWalletOverflow(solanaCardMenu_);
    header->append(*solanaMore_);
    row.content->append(*header);
    solanaAddressLabel_ = Gtk::make_managed<Gtk::Label>();
    solanaAddressLabel_->add_css_class("ur-earn-address");
    solanaAddressLabel_->set_xalign(0);
    row.content->append(*solanaAddressLabel_);
    row.content->append(*MakeWrappedNote(
        T_("usdc_payouts_until_migration",
           "USDC payouts continue to this wallet until the migration to Bittensor is complete."),
        "ur-row-note"));
    solanaPendingLabel_ = MakeWrappedNote({}, "ur-key");
    solanaPendingLabel_->set_visible(false);
    row.content->append(*solanaPendingLabel_);
    solanaCard_->set_visible(false);
    content->append(*row.root);
  }

  // 5. Top 200: the head-spot tile or the bound status
  {
    auto row = MakePaddedRow(10);
    top200Card_ = row.root;
    top200Panel_ = row.content;
    top200Card_->set_visible(false);
    content->append(*row.root);
  }

  // 6. the pane's snackbar surface
  walletInfo_.root().set_margin_top(8);
  walletInfo_.root().set_margin_bottom(8);
  content->append(walletInfo_.root());
}

// ---- PANE B: the history ledger (star column) --------------------------------

void EarningsPage::BuildLedgerPane() {
  paneB_ = kit::MakePane(T_("epoch_history", "History"));
  paneB_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneB_.root, T_("epoch_history", "History"));

  // the header strip carries a 2-item segmented switch instead of a title
  paneB_.title->set_visible(false);
  auto* tabs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  tabs->add_css_class("linked");
  tabs->set_valign(Gtk::Align::CENTER);
  tabs->set_hexpand(true);
  tabs->set_halign(Gtk::Align::START);
  historyTab_ = Gtk::make_managed<Gtk::ToggleButton>(T_("epoch_history", "History"));
  leaderboardTab_ = Gtk::make_managed<Gtk::ToggleButton>(T_("leaderboard", "Leaderboard"));
  leaderboardTab_->set_group(*historyTab_);
  historyTab_->set_active(true);  // History is the default selection
  for (Gtk::ToggleButton* tab : {historyTab_, leaderboardTab_}) {
    tabs->append(*tab);
    tab->signal_toggled().connect([this, tab] {
      if (tab->get_active()) OnLedgerTabChanged();
    });
  }
  paneB_.header->prepend(*tabs);

  historyHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  historyHost_->set_vexpand(true);
  historyPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  historyHost_->append(*historyPanel_);
  historyStatus_ = kit::MakePaneEmptyLine(T_("loading", "Loading..."));
  historyHost_->append(*historyStatus_);
  paneB_.content->append(*historyHost_);

  // Two boards behind one more switch: Data (the last-4-payments board) and
  // Points (the all-time points board, android/POINTSLEADERBOARD.md).
  leaderboardHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  leaderboardHost_->set_vexpand(true);
  leaderboardHost_->set_visible(false);
  {
    auto* boards = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    boards->add_css_class("linked");
    boards->set_halign(Gtk::Align::START);
    boards->set_margin_start(12);
    boards->set_margin_end(12);
    boards->set_margin_top(6);
    boards->set_margin_bottom(6);
    dataBoardTab_ = Gtk::make_managed<Gtk::ToggleButton>(T_("data", "Data"));
    pointsBoardTab_ = Gtk::make_managed<Gtk::ToggleButton>(T_("points", "Points"));
    pointsBoardTab_->set_group(*dataBoardTab_);
    dataBoardTab_->set_active(true);  // Data is the default board
    for (Gtk::ToggleButton* tab : {dataBoardTab_, pointsBoardTab_}) {
      boards->append(*tab);
      tab->signal_toggled().connect([this, tab] {
        if (tab->get_active()) OnBoardTabChanged();
      });
      // activating a tab -- the already-active one included -- scrolls its
      // list to the top (mmm/DESIGNSTYLE.md "Long ranked lists"); a press on
      // the active tab of a group toggles nothing, so this hangs on clicked
      tab->signal_clicked().connect([this, tab] {
        if (tab->get_active()) ResetBoardList(tab == pointsBoardTab_);
      });
    }
    leaderboardHost_->append(*boards);
  }
  leaderboardDataHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  leaderboardDataHost_->set_vexpand(true);
  leaderboardRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  leaderboardDataHost_->append(*leaderboardRows_);
  leaderboardStatus_ = kit::MakePaneEmptyLine(T_("loading", "Loading..."));
  leaderboardDataHost_->append(*leaderboardStatus_);
  leaderboardHost_->append(*leaderboardDataHost_);
  BuildPointsBoard();
  paneB_.content->append(*leaderboardHost_);
}

// ---- PANE C: network (380) ---------------------------------------------------

void EarningsPage::BuildNetworkPane() {
  paneC_ = kit::MakePane(T_("network_earnings", "Network earnings"));
  paneC_.root->set_size_request(kPaneCWidth, -1);
  paneC_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneC_.root, T_("network_earnings", "Network earnings"));
  Gtk::Box* content = paneC_.content;

  // 1 + 2. own ranking. The rank rides the group header as its meta
  // ("Current Ranking … #42"), so the Net Provided row under it is one label
  // and one figure, not two figures merging on one baseline.
  {
    auto header = kit::MakePaneGroupHeader(T_("current_ranking", "Current Ranking"));
    rankValue_ = header.meta;
    rankValue_->set_visible(true);  // always a rank or the faint dash
    content->append(*header.root);
    dataRankingWidgets_.push_back(header.root);
  }
  {
    auto row = MakePaddedRow(12);
    auto* grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* key = Gtk::make_managed<Gtk::Label>(T_("net_provided", "Net Provided"));
    key->add_css_class("ur-key");
    key->set_xalign(0);
    key->set_hexpand(true);
    key->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*key);
    grid->append(*key);
    netProvidedValue_ = MakeStrongValue(18);
    grid->append(*netProvidedValue_);
    row.content->append(*grid);
    content->append(*row.root);
    dataRankingWidgets_.push_back(row.root);
  }

  // 3. the public-leaderboard switch
  {
    auto row = kit::MakePaneTwoLineRow(
        T_("display_network_on_leaderboard", "Display network on leaderboard"), {}, 44);
    publicToggle_ = Gtk::make_managed<Gtk::Switch>();
    publicToggle_->set_valign(Gtk::Align::CENTER);
    kit::SetAccessibleLabel(
        *publicToggle_, T_("display_network_on_leaderboard", "Display network on leaderboard"));
    publicToggle_->property_active().signal_changed().connect(
        sigc::mem_fun(*this, &EarningsPage::OnLeaderboardPublicToggled));
    row.trailing->append(*publicToggle_);
    content->append(*row.root);
    dataRankingWidgets_.push_back(row.root);
  }

  // 4. what the board actually measures
  {
    auto row = MakePaddedRow(8);
    row.content->append(*MakeWrappedNote(
        T_("leaderboard_description",
           "The leaderboard is the sum of the last 4 payments. It is updated each payment "
           "cycle."),
        "ur-row-note"));
    content->append(*row.root);
    dataRankingWidgets_.push_back(row.root);
  }

  // 4b. the points board's block, in the data block's place while that board shows
  BuildPointsNetworkBlock();

  // 5. the network pane's snackbar surface
  leaderboardInfo_.root().set_margin_top(8);
  leaderboardInfo_.root().set_margin_bottom(8);
  content->append(leaderboardInfo_.root());

  // 6 + 7. network reliability
  {
    auto group = kit::MakePaneGroupHeader(
        T_("site_app_network_reliability", "Network reliability"), T_("loading", "Loading..."));
    reliabilityStatus_ = group.meta;
    content->append(*group.root);
  }
  {
    auto row = MakePaddedRow(10);
    row.content->set_spacing(8);
    reliabilityCard_ = row.root;
    reliabilityPanel_ = row.content;
    reliabilityCard_->set_visible(false);
    content->append(*row.root);
  }

  // 8. extender statistics (EXTENDER.md O4, O8): the traffic this device's
  // extender role relayed, egress toward clients above the axis and ingress
  // toward the operator below. Shown only with the provider statistics and a
  // running role; hidden otherwise with no placeholder, since the extender row
  // in the provider group already says why.
  extenderStatsHeader_ =
      kit::MakePaneGroupHeader(T_("extender_statistics", "Extender statistics"));
  content->append(*extenderStatsHeader_.root);
  extenderChart_ = Gtk::make_managed<TransferChart>(
      T_("extender", "Extender"), TransferChart::Route::Remote, kUrLightBlue, kUrPink,
      TransferChart::CountUnit::Reads);
  extenderChartRow_ = MakeChartRow(kStatsChartHeight, extenderChart_);
  content->append(*extenderChartRow_);

  // 9. provider statistics (O5, O8). The header is static: this platform has
  // no provider contracts feed yet. While the statistics are not visible its
  // meta says providing_disabled and the chart rows collapse; the provide mode
  // row and the extender row stay, which is when their text matters.
  providerStatsHeader_ =
      kit::MakePaneGroupHeader(T_("provider_statistics", "Provider statistics"));
  content->append(*providerStatsHeader_.root);

  // 9a. provide mode, moved here from above the reliability group as macOS
  // keeps it (EXTENDER.md O8): the connect page's indicator + label with the
  // current mode; the whole row opens the connect page where it is changed
  {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    provideModeDot_.set_valign(Gtk::Align::CENTER);
    rowBox->append(provideModeDot_);
    auto* label = Gtk::make_managed<Gtk::Label>(T_("provide_mode", "Provide mode"));
    label->set_xalign(0);
    label->set_hexpand(true);
    rowBox->append(*label);
    controlMode_ = host_.GetProvideControlMode();
    provideModeValue_ = Gtk::make_managed<Gtk::Label>(ProvideModeValueText(controlMode_));
    provideModeValue_->add_css_class("dim-label");
    rowBox->append(*provideModeValue_);
    auto* chevron = Gtk::make_managed<Gtk::Image>();
    chevron->set_from_icon_name("go-next-symbolic");
    chevron->add_css_class("dim-label");
    rowBox->append(*chevron);
    provideModeRow_ = Gtk::make_managed<Gtk::Button>();
    provideModeRow_->set_child(*rowBox);
    provideModeRow_->add_css_class("flat");
    provideModeRow_->set_has_frame(false);
    provideModeRow_->set_margin_bottom(8);
    provideModeRow_->signal_clicked().connect([this] {
      if (on_open_provide_settings) on_open_provide_settings();
    });
    content->append(*provideModeRow_);
  }

  // 9a'. why an enabled provider is idle (P008): one muted line directly under
  // the provide mode row, the local reason or the server's (ApplyStatusLine),
  // and a Change that opens the provide mode where the row does. It only
  // navigates; the mode is changed there. Hidden while there is no reason.
  {
    statusLineRow_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    statusLineRow_->set_margin_start(12);
    statusLineRow_->set_margin_end(12);
    statusLineRow_->set_margin_bottom(8);
    statusLineText_ = MakeWrappedNote({}, "ur-row-note");
    statusLineText_->set_hexpand(true);
    statusLineRow_->append(*statusLineText_);
    auto* change = Gtk::make_managed<Gtk::Button>(T_("change", "Change"));
    change->add_css_class("flat");
    change->set_valign(Gtk::Align::CENTER);
    change->signal_clicked().connect([this] {
      if (on_open_provide_settings) on_open_provide_settings();
    });
    statusLineRow_->append(*change);
    statusLineRow_->set_visible(false);
    content->append(*statusLineRow_);
  }

  // 9b. the read-only extender row (N7): the connect page's row with the
  // provide mode row's chevron in place of the switch, opening the same page
  {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    extenderDot_ = Gtk::make_managed<Gtk::Label>();
    extenderDot_->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*extenderDot_);
    rowBox->append(*extenderDot_);
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    text->set_hexpand(true);
    text->set_valign(Gtk::Align::CENTER);
    auto* title = Gtk::make_managed<Gtk::Label>(T_("extender", "Extender"));
    title->add_css_class("ur-row-title");
    title->set_xalign(0);
    kit::MarkDecorative(*title);  // the button carries the name
    text->append(*title);
    extenderState_ = Gtk::make_managed<Gtk::Label>();
    extenderState_->add_css_class("ur-row-note");
    extenderState_->set_xalign(0);
    extenderState_->set_single_line_mode(true);
    extenderState_->set_ellipsize(Pango::EllipsizeMode::END);
    // the 380 rail holds only if a long listen failure does not ask for more;
    // the tooltip carries the whole text
    extenderState_->set_max_width_chars(34);
    kit::MarkDecorative(*extenderState_);
    text->append(*extenderState_);
    rowBox->append(*text);
    auto* chevron = Gtk::make_managed<Gtk::Image>();
    chevron->set_from_icon_name("go-next-symbolic");
    chevron->add_css_class("dim-label");
    rowBox->append(*chevron);
    extenderRow_ = Gtk::make_managed<Gtk::Button>();
    extenderRow_->set_child(*rowBox);
    extenderRow_->add_css_class("flat");
    extenderRow_->set_has_frame(false);
    extenderRow_->set_margin_bottom(8);
    extenderRow_->signal_clicked().connect([this] {
      if (on_open_provide_settings) on_open_provide_settings();
    });
    extenderRow_->set_visible(false);
    content->append(*extenderRow_);
  }

  // 9c. the Local chart of the provider series
  localChart_ = Gtk::make_managed<TransferChart>(T_("local", "Local"), TransferChart::Route::Local,
                                                 kUrGreen, kUrPink);
  localChartRow_ = MakeChartRow(kStatsChartHeight, localChart_);
  content->append(*localChartRow_);

  // 9d. the provider's relayed traffic by transport, the connect page's pane B
  // bar in its provider kind; its click opens the provider transport settings
  providerTransportRow_ = kit::MakePaneRow(-1);
  providerTransportBar_ = Gtk::make_managed<TransportBar>();
  providerTransportBar_->set_hexpand(true);
  providerTransportBar_->set_margin_top(10);
  providerTransportBar_->set_margin_bottom(10);
  providerTransportBar_->SetSurfaceColor(kUrBackground);  // the pane fill, not the card
  providerTransportBar_->on_activate = [this] { OpenProviderTransportSheet(); };
  if (auto* inner = dynamic_cast<Gtk::Box*>(providerTransportRow_->get_first_child())) {
    inner->append(*providerTransportBar_);
  }
  content->append(*providerTransportRow_);

  // 9e. the Blocked chart at half height
  blockedChart_ = Gtk::make_managed<TransferChart>(
      T_("blocked", "Blocked"), TransferChart::Route::Block, kUrCoral, kUrMutedCoral);
  // a drawing area's content height is its minimum height too, so the
  // half-height chart is fitted inside its row's hairline, or the row would
  // grow to the chart's default 128
  blockedChart_->set_content_height(kBlockedChartHeight - 1);
  blockedChartRow_ = MakeChartRow(kBlockedChartHeight, blockedChart_);
  content->append(*blockedChartRow_);

  // 9f. how often the network offered this device to clients in each minute of
  // the last hour (P008), next to the other provider plots and under their
  // gate: the Demand histogram, with the loading or unavailable line in its
  // place until there is one, so the layout does not jump. Its height follows
  // the caption's wrap (DemandChart measures it).
  demandChart_ = Gtk::make_managed<DemandChart>(
      T_("provider_status_demand", "Demand"),
      T_("provider_status_histogram_title",
         "Times clients were offered this device, per minute, last hour"),
      T_("provider_status_histogram_start", "60 min ago"),
      T_("provider_status_histogram_end", "Now"));
  demandChart_->set_margin_bottom(6);
  demandChartRow_ = kit::MakePaneRow(-1);
  if (auto* inner = dynamic_cast<Gtk::Box*>(demandChartRow_->get_first_child())) {
    inner->append(*demandChart_);
  }
  content->append(*demandChartRow_);

  // 9g. "Why?": a disclosure, collapsed by default; expanded, one row per number
  // the network ranks this device by, in the server's order, then the country
  // (RebuildWhyRows)
  {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* label = Gtk::make_managed<Gtk::Label>(T_("provider_status_why", "Why?"));
    label->set_xalign(0);
    label->set_hexpand(true);
    rowBox->append(*label);
    whyChevron_ = Gtk::make_managed<Gtk::Image>();
    whyChevron_->set_from_icon_name("go-next-symbolic");
    whyChevron_->add_css_class("dim-label");
    kit::MarkDecorative(*whyChevron_);
    rowBox->append(*whyChevron_);
    whyRow_ = Gtk::make_managed<Gtk::Button>();
    whyRow_->set_child(*rowBox);
    whyRow_->add_css_class("flat");
    whyRow_->set_has_frame(false);
    kit::SetAccessibleLabel(*whyRow_, T_("provider_status_why", "Why?"));
    gtk_accessible_update_state(GTK_ACCESSIBLE(whyRow_->gobj()), GTK_ACCESSIBLE_STATE_EXPANDED,
                                FALSE, -1);
    whyRow_->signal_clicked().connect(sigc::mem_fun(*this, &EarningsPage::OnWhyToggled));
    whyRow_->set_visible(false);
    content->append(*whyRow_);
    whyRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    whyRows_->set_visible(false);
    content->append(*whyRows_);
  }
}

// ---- loads -------------------------------------------------------------------

// Independent requests, never chained: each settles its own panel, so one
// failure cannot blank the rest. Every callback may land on an SDK thread and
// is marshaled with PostToMain, then dropped if the epoch moved.
void EarningsPage::LoadEarnings() {
  if (!CanCallApi()) {
    g_message("earnings: no session; settling the panels on their empty state");
    SettleAllEmpty();
    return;
  }
  if (auto jwt = host_.ParseByJwt()) {
    ownNetworkId_ = jwt->NetworkId.value_or(std::string());
  }

  ApplyPoints(std::nullopt, Fetch::Loading);
  ApplyEpochs(std::nullopt, Fetch::Loading);
  ApplySnWallet(std::nullopt, Fetch::Loading);
  ApplyReliability(std::nullopt, Fetch::Loading);
  headState_ = Fetch::Loading;

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;

  // 1. account points (the headline)
  host_.api().getAccountPoints(
      [this, epoch, seen](std::optional<urnet::AccountPointsResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result) {
            g_warning("earnings: getAccountPoints failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplyPoints(std::nullopt, Fetch::Failed);
            return;
          }
          ApplyPoints(result->network_points, Fetch::Ready);
        });
      });

  // 2. the per-epoch history
  sn::FetchEpochs(host_, [this, epoch, seen](std::optional<std::vector<AccountEpochRow>> rows,
                                              std::string err) {
    PostToMain([this, epoch, seen, rows = std::move(rows), err = std::move(err)] {
      if (*epoch != seen) return;
      if (!rows) {
        g_warning("earnings: account epochs failed: %s", err.c_str());
        ApplyEpochs(std::nullopt, Fetch::Failed);
        return;
      }
      ApplyEpochs(std::move(rows), Fetch::Ready);
    });
  });

  // 4. the attached wallet — and, once known, the claims behind it
  sn::FetchWallet(host_, ProviderClientId(),
                  [this, epoch, seen](bool ok, std::optional<SnWalletInfo> wallet,
                                      std::string err) {
    PostToMain([this, epoch, seen, ok, wallet = std::move(wallet), err = std::move(err)] {
      if (*epoch != seen) return;
      if (!ok) {
        g_warning("earnings: sn wallet failed: %s", err.c_str());
        ApplySnWallet(std::nullopt, Fetch::Failed);
        return;
      }
      ApplySnWallet(std::move(wallet), Fetch::Ready);
      LoadWalletLayer();
    });
  });

  // 4b. the payout consent: the status, whatever the wallet above
  consent_.Load();

  // 5. the head-spot eligibility
  sn::FetchHead(host_, [this, epoch, seen](std::optional<SnHeadInfo> head, std::string err) {
    PostToMain([this, epoch, seen, head = std::move(head), err = std::move(err)] {
      if (*epoch != seen) return;
      if (!head) {
        // the tile simply does not show: a failure here must not shout
        g_warning("earnings: sn head failed: %s", err.c_str());
        ApplyHead(std::nullopt, Fetch::Failed);
        return;
      }
      ApplyHead(std::move(head), Fetch::Ready);
    });
  });

  // 6. the reliability window
  host_.api().getNetworkReliability(
      [this, epoch, seen](std::optional<urnet::GetNetworkReliabilityResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result || result->error) {
            g_warning("earnings: getNetworkReliability failed: %s",
                      err ? err->c_str() : "(server error)");
            ApplyReliability(std::nullopt, Fetch::Failed);
            return;
          }
          ApplyReliability(result->reliability_window, Fetch::Ready);
        });
      });
}

// The subnet layer: the vault's view of this network's epochs, the coldkey's
// v2 accounts and the gas key. Only with an attached wallet — without one
// there is nothing to read.
void EarningsPage::LoadWalletLayer() {
  if (!wallet_ || !CanCallApi()) return;
  ApplyClaims(std::nullopt, 0, Fetch::Loading);
  ApplyLedger(snledger::Load::Loading, {}, ledgerConfigured_);
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  sn::FetchClaims(host_, [this, epoch, seen](std::optional<std::vector<SnClaimRow>> claims,
                                              int64_t total, std::string err,
                                              std::optional<snpayout::EpochSchedule> schedule) {
    PostToMain([this, epoch, seen, claims = std::move(claims), total, err = std::move(err),
                schedule] {
      if (*epoch != seen) return;
      if (!claims) {
        g_warning("earnings: sn claims failed: %s", err.c_str());
        Glib::ustring text;
        if (err == sn::kNoDevice) {
          // no device this session yet: the wallet is known, the vault is not
          text = T_("claim_unavailable_linux", "Claiming is not available on this device yet.");
        } else {
          const std::string code = SnErrorCode(err);
          const std::string lower = Lowercase(err);
          const bool rpc = code == "chain_rpc_unreachable" || code == "chain_rpc_error" ||
                           (code.empty() && (lower.find("rpc") != std::string::npos ||
                                             lower.find("unreachable") != std::string::npos ||
                                             lower.find("timeout") != std::string::npos));
          text = rpc ? Glib::ustring(T_("chain_rpc_unreachable",
                                        "The chain RPC is unreachable. Try again."))
                     : Glib::ustring(T_("something_went_wrong", "Something went wrong."));
        }
        ApplyClaims(std::nullopt, 0, Fetch::Failed, text);
        return;
      }
      ApplyClaims(std::move(claims), total, Fetch::Ready, {}, schedule);
    });
  });
  sn::FetchLedger(host_, [this, epoch, seen](std::optional<std::vector<snledger::Account>> accounts,
                                              bool configured, std::string err) {
    PostToMain([this, epoch, seen, accounts = std::move(accounts), configured,
                err = std::move(err)] {
      if (*epoch != seen) return;
      if (!accounts) {
        g_warning("earnings: sn ledger accounts failed: %s", err.c_str());
        ApplyLedger(snledger::Load::Unavailable, {}, configured);
        return;
      }
      ApplyLedger(snledger::Load::Ready, *accounts, configured);
    });
  });
  LoadGas();
}

void EarningsPage::LoadGas() {
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  sn::FetchGas(host_, [this, epoch, seen](std::optional<SnGasInfo> gas, std::string err) {
    PostToMain([this, epoch, seen, gas = std::move(gas), err = std::move(err)] {
      if (*epoch != seen) return;
      if (!gas) g_message("earnings: sn gas key unavailable: %s", err.c_str());
      ApplyGas(std::move(gas));
    });
  });
}

void EarningsPage::LoadLeaderboard() {
  if (!CanCallApi()) {
    g_message("earnings: no session; the leaderboard is left unasked");
    ApplyRanking(std::nullopt, false);
    ApplyLeaderboard(std::nullopt, Fetch::NoSession);
    return;
  }
  leaderboardState_ = Fetch::Loading;
  kit::SetTextOrCollapse(*leaderboardStatus_, T_("loading", "Loading..."));
  if (auto jwt = host_.ParseByJwt()) {
    ownNetworkId_ = jwt->NetworkId.value_or(std::string());
  }

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;

  host_.api().getNetworkLeaderboardRanking(
      [this, epoch, seen](std::optional<urnet::GetNetworkRankingResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result || result->error || !result->network_ranking) {
            g_warning("earnings: getNetworkLeaderboardRanking failed: %s",
                      err ? err->c_str()
                          : (result && result->error ? result->error->message.c_str()
                                                     : "(no ranking)"));
            ApplyRanking(std::nullopt, false);
            return;
          }
          ApplyRanking(result->network_ranking, true);
        });
      });

  host_.api().getLeaderboard(
      urnet::GetLeaderboardArgs{},
      [this, epoch, seen](std::optional<urnet::LeaderboardResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          // `!result->earners` is part of the FAILURE predicate: a nil Go
          // result arrives as an engaged optional whose fields are all unset
          if (err || !result || result->error || !result->earners) {
            g_warning("earnings: getLeaderboard failed: %s",
                      err ? err->c_str()
                          : (result && result->error ? result->error->message.c_str()
                                                     : "(no earners in the result)"));
            ApplyLeaderboard(std::nullopt, Fetch::Failed);
            return;
          }
          g_message("earnings: getLeaderboard ok: %zu earner(s)", result->earners->size());
          ApplyLeaderboard(result->earners, Fetch::Ready);
        });
      });
}

// ---- appliers ----------------------------------------------------------------

void EarningsPage::ApplyPoints(std::optional<urnet::AccountPointsList> points, Fetch state) {
  pointsState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*pointsStatus_, T_("loading", "Loading..."));
    pointsCard_->set_visible(false);
    return;
  }
  if (state == Fetch::Failed) {
    points_.clear();
    kit::SetTextOrCollapse(*pointsStatus_, T_("something_went_wrong", "Something went wrong."));
    pointsCard_->set_visible(false);
    RemoveAllChildren(*pointsPanel_);
    return;
  }
  points_ = points.value_or(urnet::AccountPointsList{});
  kit::SetTextOrCollapse(*pointsStatus_, {});
  pointsCard_->set_visible(true);
  RebuildPointsCard();
}

void EarningsPage::ApplyEpochs(std::optional<std::vector<AccountEpochRow>> epochs, Fetch state) {
  epochsState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*historyStatus_, T_("loading", "Loading..."));
    return;
  }
  if (state == Fetch::Failed) {
    epochs_.clear();
    kit::SetTextOrCollapse(*historyStatus_, T_("something_went_wrong", "Something went wrong."));
    RemoveAllChildren(*historyPanel_);
    ApplyLedgerMeta();
    return;
  }
  epochs_ = epochs.value_or(std::vector<AccountEpochRow>{});
  // newest first, whatever order the server promised
  std::stable_sort(epochs_.begin(), epochs_.end(),
                   [](const AccountEpochRow& a, const AccountEpochRow& b) {
                     return a.epoch > b.epoch;
                   });
  kit::SetTextOrCollapse(*historyStatus_, {});
  RebuildHistory();
}

void EarningsPage::ApplySnWallet(std::optional<SnWalletInfo> wallet, Fetch state) {
  walletState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*walletStatus_, T_("loading", "Loading..."));
    RebuildWalletBlock();
    return;
  }
  if (state == Fetch::Failed) {
    wallet_.reset();
    kit::SetTextOrCollapse(*walletStatus_, T_("something_went_wrong", "Something went wrong."));
  } else {
    wallet_ = std::move(wallet);
    if (wallet_ && wallet_->coldkeySs58.empty()) wallet_.reset();
    kit::SetTextOrCollapse(*walletStatus_, {});
  }
  if (!wallet_) {
    // no wallet: the subnet layer is empty by definition
    claims_.clear();
    totalClaimableRao_ = 0;
    claimsState_ = Fetch::Ready;
    claimsFailure_.clear();
    schedule_.reset();
    ledgerAccounts_.clear();
    ledgerLoad_ = snledger::Load::Ready;
  }
  RebuildWalletBlock();
  RebuildPayoutLine();
  RebuildUnclaimedTile();
  RebuildLedgerTile();
  RebuildHistory();  // the alpha column follows the wallet
}

void EarningsPage::ApplyClaims(std::optional<std::vector<SnClaimRow>> claims,
                               int64_t totalClaimableRao, Fetch state,
                               const Glib::ustring& failure,
                               std::optional<snpayout::EpochSchedule> schedule) {
  claimsState_ = state;
  claimsFailure_ = failure;
  if (state == Fetch::Ready) {
    claims_ = claims.value_or(std::vector<SnClaimRow>{});
    totalClaimableRao_ = totalClaimableRao;
    schedule_ = schedule;
  } else if (state == Fetch::Failed) {
    claims_.clear();
    totalClaimableRao_ = 0;
    schedule_.reset();
  }
  RebuildPayoutLine();
  RebuildUnclaimedTile();
  RebuildHistory();
}

void EarningsPage::ApplyGas(std::optional<SnGasInfo> gas) {
  gas_ = std::move(gas);
  if (auto sheet = claimSheet_.lock()) sheet->SetGas(gas_);
  if (auto sheet = ledgerClaimSheet_.lock()) sheet->SetGas(gas_);
}

void EarningsPage::ApplyLedger(snledger::Load load, std::vector<snledger::Account> accounts,
                               bool claimConfigured) {
  ledgerLoad_ = load;
  ledgerAccounts_ = std::move(accounts);
  ledgerConfigured_ = claimConfigured;
  RebuildLedgerTile();
}

void EarningsPage::ApplyHead(std::optional<SnHeadInfo> head, Fetch state) {
  headState_ = state;
  head_ = state == Fetch::Ready ? std::move(head) : std::nullopt;
  RebuildTop200();
}

void EarningsPage::ApplyReliability(std::optional<urnet::ReliabilityWindow> window,
                                    Fetch state) {
  reliabilityState_ = state;
  lastReliability_ = window;
  lastReliabilityState_ = state;
  if (!providingEnabled_) {
    // providing is off: the chart hides and the group says so, the same gate
    // and message as the stats widget
    kit::SetTextOrCollapse(*reliabilityStatus_, T_("providing_disabled", "Providing is disabled"));
    reliabilityCard_->set_visible(false);
    return;
  }
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*reliabilityStatus_, T_("loading", "Loading..."));
    reliabilityCard_->set_visible(false);
    return;
  }
  if (state == Fetch::Failed) {
    reliability_.reset();
    kit::SetTextOrCollapse(*reliabilityStatus_,
                           T_("something_went_wrong", "Something went wrong."));
    reliabilityCard_->set_visible(false);
    return;
  }
  reliability_ = std::move(window);
  if (!reliability_) {
    kit::SetTextOrCollapse(*reliabilityStatus_,
                           T_("site_app_no_reliability", "No reliability data yet."));
    reliabilityCard_->set_visible(false);
    return;
  }
  kit::SetTextOrCollapse(*reliabilityStatus_, {});
  reliabilityCard_->set_visible(true);
  RebuildReliabilityCard();
}

void EarningsPage::ApplyRanking(std::optional<urnet::NetworkRanking> ranking, bool ok) {
  if (!ok || !ranking) {
    SetStatValue(netProvidedValue_, T_("net_provided", "Net Provided"), {}, false);
    SetStatValue(rankValue_, T_("current_ranking", "Current Ranking"), {}, false);
    return;
  }
  SetStatValue(netProvidedValue_, T_("net_provided", "Net Provided"),
               FormatMiB(ranking->net_mib_count), true);
  const bool ranked = ranking->leaderboard_rank > 0;
  SetStatValue(rankValue_, T_("current_ranking", "Current Ranking"),
               ranked ? Glib::ustring("#" + std::to_string(ranking->leaderboard_rank))
                      : Glib::ustring(),
               ranked);
  rankingPublic_ = ranking->leaderboard_public;
  SetRankingToggle(rankingPublic_);  // through the echo guard, never bare
}

void EarningsPage::ApplyLeaderboard(std::optional<urnet::LeaderboardEarnersList> earners,
                                    Fetch state) {
  leaderboardState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*leaderboardStatus_, T_("loading", "Loading..."));
    return;
  }
  if (state == Fetch::NoSession) {
    leaderboardRequested_ = false;
    leaderboard_.clear();
    leaderboardCount_ = 0;
    RemoveAllChildren(*leaderboardRows_);
    kit::SetTextOrCollapse(*leaderboardStatus_,
                           T_("please_login_to_urnetwork", "Please login to URnetwork"));
    ApplyLedgerMeta();
    return;
  }
  if (state == Fetch::Failed) {
    leaderboardRequested_ = false;  // retryable on the next look
    leaderboard_.clear();
    leaderboardCount_ = 0;
    RemoveAllChildren(*leaderboardRows_);
    kit::SetTextOrCollapse(*leaderboardStatus_,
                           T_("something_went_wrong", "Something went wrong."));
    ApplyLedgerMeta();
    return;
  }
  leaderboard_ = earners.value_or(urnet::LeaderboardEarnersList{});
  leaderboardCount_ = static_cast<int>(leaderboard_.size());
  if (leaderboard_.empty()) {
    RemoveAllChildren(*leaderboardRows_);
    kit::SetTextOrCollapse(
        *leaderboardStatus_,
        T_("site_app_leaderboard_empty", "No networks on the leaderboard yet."));
    ApplyLedgerMeta();
    return;
  }
  kit::SetTextOrCollapse(*leaderboardStatus_, {});
  RebuildLeaderboard();
}

// ---- rebuilders --------------------------------------------------------------

void EarningsPage::RebuildPointsCard() {
  RemoveAllChildren(*pointsPanel_);
  pointsPanel_->append(*BuildPointsBreakdown(AggregatePoints(points_)));
}

// How and when SN payouts happen, under the points. The decision is
// snpayout::PayoutLineFor's; the times are the reader's local time, with the
// zone's offset taken at each instant. Claim opens the claim sheet and Set
// coldkey the coldkey flow, the same handlers as the unclaimed tile's button
// and the Bittensor block's.
void EarningsPage::RebuildPayoutLine() {
  if (payoutCard_ == nullptr) return;
  const snpayout::PayoutLineView view = snpayout::PayoutLineFor(
      walletState_ == Fetch::Ready, wallet_.has_value(), totalClaimableRao_, schedule_,
      g_get_real_time() / 1000, [](int64_t millis) {
        return snpayout::FormatScheduleTime(millis, LocalUtcOffsetMinutes(millis));
      });
  const bool shown = view.kind != snpayout::LineKind::Hidden;
  payoutCard_->set_visible(shown);
  if (!shown) return;
  const bool setColdkey = view.kind == snpayout::LineKind::SetColdkey;
  payoutText_->set_text(
      setColdkey ? Glib::ustring(T_("set_coldkey_to_get_paid",
                                    "Set your Bittensor coldkey to get paid"))
                 : Glib::ustring(T_("sn_payout_schedule",
                                    "Earnings settle every epoch and are paid in SN25α to your "
                                    "Bittensor coldkey when you claim them.")));
  kit::SetTextOrCollapse(
      *payoutTimes_,
      view.showTimes
          ? Glib::ustring(Format(T_("sn_payout_schedule_times",
                                    "This epoch ends {0}. Claim your share from {1} until {2}."),
                                 view.epochEnd, view.claimOpen, view.expiry))
          : Glib::ustring());
  setColdkeyButton_->set_visible(setColdkey);
  payoutClaimButton_->set_visible(view.showClaim);
  payoutClaimButton_->set_sensitive(view.showClaim && !claiming_);
}

void EarningsPage::RebuildUnclaimedTile() {
  if (unclaimedCard_ == nullptr) return;
  const bool connected = wallet_.has_value() && walletState_ == Fetch::Ready;
  unclaimedCard_->set_visible(connected);
  if (!connected) return;
  const bool ready = claimsState_ == Fetch::Ready;
  unclaimedValue_->set_markup(
      "<span foreground='" + HexForMarkup(kReferralGoldLight) + "' size='" +
      std::to_string(34 * PANGO_SCALE) + "'>" +
      Glib::Markup::escape_text(ready ? FormatAlphaRao(totalClaimableRao_) : std::string("-")) +
      "</span>");
  kit::SetAccessibleLabel(*unclaimedValue_,
                          Glib::ustring(T_("unclaimed", "Unclaimed")) + ", " +
                              (ready ? FormatAlphaRao(totalClaimableRao_) : std::string("-")));
  Glib::ustring status;
  if (claimsState_ == Fetch::Loading) {
    status = T_("loading", "Loading...");
  } else if (claimsState_ == Fetch::Failed) {
    status = claimsFailure_.empty()
                 ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                 : claimsFailure_;
  } else if (totalClaimableRao_ <= 0) {
    status = T_("no_claims_yet_linux",
                "Nothing to claim yet. Alpha accrues from the next finalized epoch.");
  }
  kit::SetTextOrCollapse(*unclaimedStatus_, status);
  claimButton_->set_sensitive(ready && totalClaimableRao_ > 0 && !claiming_);
}

// The Settlement v2 tile under the unclaimed one: snledger::SectionFor's
// decision, with the alpha in the app's formatting. Claim opens the v2 claim
// dialog.
void EarningsPage::RebuildLedgerTile() {
  if (ledgerCard_ == nullptr) return;
  const bool connected = wallet_.has_value() && walletState_ == Fetch::Ready;
  const snledger::SectionView view =
      snledger::SectionFor(connected, ledgerLoad_, ledgerAccounts_, ledgerConfigured_, claiming_,
                           FormatAlphaRao);
  ledgerCard_->set_visible(view.visible);
  if (!view.visible) return;
  RemoveAllChildren(*ledgerRows_);
  for (const snledger::AccountView& account : view.accounts) {
    auto* block = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    if (view.showPools) {
      block->append(*MakeSizedLabel(Format(T_("sn_ledger_pool", "Pool {}"), account.noId), 12,
                                    "ur-caption"));
    }
    const Glib::ustring claimableLabel = T_("sn_ledger_claimable", "Claimable");
    block->append(*MakeSizedLabel(claimableLabel, 12, "ur-caption"));
    auto* claimable = MakeGoldLabel(account.claimable, 34, /*condensed=*/true);
    kit::SetAccessibleLabel(*claimable, claimableLabel + ", " + account.claimable);
    block->append(*claimable);
    auto* cells = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    cells->append(*MakeStatCell(T_("sn_ledger_paid", "Paid"), account.paid));
    cells->append(*MakeStatCell(T_("sn_ledger_earned", "Earned"), account.earned));
    block->append(*cells);
    if (!account.pendingIncrease.empty()) {
      block->append(*MakeWrappedNote(
          Format(T_("sn_ledger_pending", "Pending: +{}, awaiting finalization"),
                 account.pendingIncrease),
          "ur-row-note"));
    }
    ledgerRows_->append(*block);
  }
  Glib::ustring status;
  switch (view.status) {
    case snledger::Status::None:
      break;
    case snledger::Status::Loading:
      status = T_("loading", "Loading...");
      break;
    case snledger::Status::Unavailable:
      status = T_("sn_ledger_unavailable", "Unavailable");
      break;
    case snledger::Status::NothingYet:
      status = T_("no_claims_yet_linux",
                  "Nothing to claim yet. Alpha accrues from the next finalized epoch.");
      break;
  }
  kit::SetTextOrCollapse(*ledgerStatus_, status);
  ledgerClaimButton_->set_visible(view.showClaim);
  ledgerClaimButton_->set_sensitive(view.claimEnabled);
}

void EarningsPage::RebuildWalletBlock() {
  if (walletConnectedPanel_ == nullptr) return;
  const bool connected = wallet_.has_value() && walletState_ == Fetch::Ready;
  walletConnectedPanel_->set_visible(connected);
  if (connected) {
    walletAddressLabel_->set_text(ShortSs58(wallet_->coldkeySs58));
    walletAddressLabel_->set_tooltip_text(wallet_->coldkeySs58);
    kit::SetAccessibleLabel(*walletAddressLabel_,
                            Glib::ustring(T_("bittensor_wallet", "Bittensor wallet")) + ", " +
                                wallet_->coldkeySs58);
    changeWalletButton_->set_sensitive(!connecting_ && !consent_.Busy());
  }
  const bool offerConnect = walletState_ != Fetch::Loading && (!connected || changingWallet_);
  walletConnectPanel_->set_visible(offerConnect);
  // "Change" opens the connect panel under a connected wallet: its overflow
  // would repeat the one beside "Change"
  walletMoreDisconnected_->set_visible(!connected);
  // a link label keeps its markup; only its visibility follows the wallet state
  walletConnectNote_->set_visible(!connected);
  // the bridge carries one wallet flow at a time: a consent signing holds it too
  const bool bridgeBusy = connecting_ || consent_.Busy();
  connectBridgeButton_->set_sensitive(!bridgeBusy);
  manualToggleButton_->set_sensitive(!bridgeBusy);
  manualPanel_->set_visible(manualEntryOpen_);
  const std::string typed = TrimWhitespace(walletAddressBox_->get_text().raw());
  const bool checked = check_.has_value() && !typed.empty() && checkedAddress_ == typed &&
                       check_->validSyntax && !check_->banned;
  connectManualButton_->set_sensitive(checked && !bridgeBusy);
  kit::SetTextOrCollapse(
      *connectingStatus_,
      connecting_ ? Glib::ustring(T_("opening_bittensor_wallet_in_browser",
                                     "Opening your Bittensor wallet in the browser…"))
                  : Glib::ustring());
  // the overflow stays live while the bridge is out: a Solana connect
  // supersedes the Bittensor one (SdkHost::ConnectSolanaWallet)
  RebuildSolanaCard();  // the waiting line follows the Bittensor panel it sits in
  RebuildConsentSection();  // its button follows the connect's hold on the bridge
}

void EarningsPage::RebuildTop200() {
  if (top200Panel_ == nullptr) return;
  RemoveAllChildren(*top200Panel_);
  const bool show = headState_ == Fetch::Ready && head_ && (head_->eligible || head_->bound);
  top200Card_->set_visible(show);
  if (!show) return;
  // Sign fleet binding, in both states: bindings renew per epoch window and
  // new devices join the fleet after the spot is bound
  const auto signButton = [this] {
    auto* sign = Gtk::make_managed<Gtk::Button>(T_("top200_sign_binding", "Sign fleet binding"));
    sign->add_css_class("ur-btn-secondary");
    sign->set_halign(Gtk::Align::START);
    sign->set_margin_top(4);
    sign->signal_clicked().connect([this] { OpenTop200SignSheet(std::nullopt, false); });
    return sign;
  };
  if (head_->bound) {
    // the status row: this network holds a head spot
    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* title = MakeGoldLabel(T_("top200", "Top 200"), 14);
    title->set_hexpand(true);
    line->append(*title);
    auto* status = MakeSizedLabel(
        Format(T_("top200_bound_status", "UID {0} · rank #{1}"), head_->uid, head_->rank), 13,
        "ur-value");
    status->set_wrap(false);
    line->append(*status);
    card->append(*line);
    if (!head_->hotkey.empty()) {
      auto* hotkey = MakeSizedLabel(ShortSs58(head_->hotkey), 12, "ur-caption");
      hotkey->set_tooltip_text(head_->hotkey);
      card->append(*hotkey);
    }
    card->append(*MakeWrappedNote(
        T_("top200_bound_detail",
           "Emission is paid to your coldkey directly. Bindings renew per epoch."),
        "ur-row-note"));
    if (head_->floor > 0 && head_->score < head_->floor * kHeadDemotionMargin) {
      card->append(*MakeWrappedNote(
          T_("top200_demotion_warning",
             "Your score is close to the eviction floor. Add routable IPs to keep the spot."),
          "ur-danger-text"));
    }
    card->append(*signButton());
    top200Panel_->append(*card);
    return;
  }
  // the gold tile: eligible, not yet bound — the spot is claimed on ur.io
  auto* tile = MakeGoldTile();
  auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* title = MakeGoldLabel(T_("top200", "Top 200"), 14);
  title->set_hexpand(true);
  line->append(*title);
  auto* qualify = MakeSizedLabel(T_("top200_you_qualify", "You qualify"), 13, "ur-value");
  qualify->set_wrap(false);
  line->append(*qualify);
  tile->append(*line);
  tile->append(*MakeWrappedNote(
      Format(T_("top200_detail",
                "Your network's routable IP breadth ranks about #{0} of {1} head spots. Head "
                "miners earn SN25α natively, every tempo."),
             head_->rankEstimate, head_->cutoff),
      "ur-key"));
  auto* button = MakeGoldButton(T_("claim_your_spot", "Claim your spot"));
  button->set_margin_top(4);
  button->signal_clicked().connect([this] { OpenLink(kTop200Url); });
  tile->append(*button);
  tile->append(*signButton());
  top200Panel_->append(*tile);
}

void EarningsPage::RebuildHistory() {
  if (historyPanel_ == nullptr) return;
  RemoveAllChildren(*historyPanel_);
  if (epochsState_ != Fetch::Ready) {
    ApplyLedgerMeta();
    return;
  }
  if (epochs_.empty()) {
    auto* empty = kit::MakeEmptyStateCard(
        "", T_("no_points_yet", "No epochs yet. Points appear after your first finalized epoch."));
    empty->set_margin(16);
    historyPanel_->append(*empty);
  } else {
    const bool withAlpha = wallet_.has_value() && walletState_ == Fetch::Ready;
    // textColumns = 2: epoch and share read left as text, the figures right
    std::vector<int> weights{2, 3, 3};
    std::vector<Glib::ustring> titles{T_("epoch_column_epoch_linux", "Epoch"),
                                      T_("epoch_column_share_linux", "Share of block"),
                                      T_("epoch_column_points_linux", "Points")};
    if (withAlpha) {
      weights = {2, 3, 3, 3, 2};
      titles.push_back(kAlphaSymbol);
      titles.push_back(T_("epoch_column_status_linux", "Status"));
    }
    historyPanel_->append(*kit::MakePaneTableHeader(weights, titles, 2));
    for (const auto& row : epochs_) {
      auto cells = kit::MakePaneTableRow(weights, 36, 2);
      cells.cells[0]->set_text(std::to_string(row.epoch));
      cells.cells[1]->set_text(FormatShareBps(row.shareBps));
      cells.cells[2]->set_text(FormatPointsValue(row.points));
      cells.cells[2]->remove_css_class("dim-label");
      Glib::ustring name = Format(T_("epoch_row_title", "Epoch {}"), row.epoch);
      name += Glib::ustring(", ") +
              Format(T_("points_short", "{} pts"), FormatPointsValue(row.points));
      if (withAlpha) {
        const SnClaimRow* claim = nullptr;
        for (const auto& candidate : claims_) {
          if (candidate.epoch == row.epoch) {
            claim = &candidate;
            break;
          }
        }
        if (claim == nullptr || claim->status == "not-finalized") {
          // before the wallet was attached (or not settled yet): points only
          cells.cells[3]->set_text("-");
          cells.cells[3]->add_css_class("ur-label-faint");
          cells.cells[4]->set_text("");
        } else {
          cells.cells[3]->set_text(FormatAlphaRao(claim->amountRao));
          Glib::ustring status;
          if (claim->status == "claimed") {
            status = T_("claim_confirmed", "Claimed");
            cells.cells[4]->remove_css_class("dim-label");
            cells.cells[4]->add_css_class("ur-value-on");
          } else if (claim->status == "expired") {
            status = T_("claim_expired", "Expired");
          } else {
            status = T_("unclaimed", "Unclaimed");
          }
          cells.cells[4]->set_text(status);
          name += Glib::ustring(", ") + FormatAlphaRao(claim->amountRao) + ", " + status;
        }
      }
      kit::SetAccessibleLabel(*cells.root, name);
      historyPanel_->append(*cells.root);
    }
    if (withAlpha) {
      auto note = MakePaddedRow(8);
      note.content->append(*MakeWrappedNote(
          T_("claims_open_after_finalization",
             "Each epoch's claims open after its payout list is finalized and stay open until "
             "they expire."),
          "ur-row-note"));
      historyPanel_->append(*note.root);
    }
  }
  ApplyLedgerMeta();
}

void EarningsPage::RebuildLeaderboard() {
  RemoveAllChildren(*leaderboardRows_);
  const std::vector<int> weights{1, 5, 2};
  leaderboardRows_->append(*kit::MakePaneTableHeader(
      weights,
      {T_("current_ranking", "Current Ranking"), T_("network", "Network"),
       T_("net_provided", "Net Provided")},
      2));
  int rank = 0;
  for (const auto& earner : leaderboard_) {
    ++rank;
    auto row = kit::MakePaneTableRow(weights, 36, 2);
    const bool isOwn = !ownNetworkId_.empty() && earner.network_id == ownNetworkId_;
    const bool masked = !isOwn && (!earner.is_public || earner.contains_profanity);
    row.cells[0]->set_text("#" + std::to_string(rank));
    row.cells[1]->set_text(masked ? Glib::ustring(T_("private_network", "Private Network"))
                                  : Glib::ustring(SanitizeExternalDisplayText(earner.network_name)));
    row.cells[2]->set_text(FormatMiB(earner.net_mib_count));
    if (masked) {
      for (Gtk::Label* cell : row.cells) cell->add_css_class("dim-label");
    }
    if (isOwn) {
      for (Gtk::Label* cell : row.cells) {
        cell->remove_css_class("dim-label");
        cell->add_css_class("ur-value-on");
      }
      row.root->add_css_class("ur-earn-own-row");
    }
    leaderboardRows_->append(*row.root);
  }
  ApplyLedgerMeta();
}

void EarningsPage::RebuildReliabilityCard() {
  RemoveAllChildren(*reliabilityPanel_);
  if (!reliability_) return;
  const urnet::ReliabilityWindow& window = *reliability_;

  auto* stats = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 24);
  stats->set_homogeneous(true);
  char meanText[32];
  std::snprintf(meanText, sizeof(meanText), "%.2f", window.mean_reliability_weight);
  stats->append(*MakeStatCell(T_("average_reliability", "Average reliability"), meanText));
  stats->append(*MakeStatCell(T_("total_clients", "Total Clients"),
                              std::to_string(window.max_total_client_count)));
  reliabilityPanel_->append(*stats);

  const std::vector<double> weights =
      window.reliability_weights.value_or(urnet::Float64List{});
  std::vector<double> clients;
  if (window.total_client_counts) {
    for (const int64_t count : *window.total_client_counts) {
      clients.push_back(static_cast<double>(count));
    }
  }
  if (weights.size() >= 2 || clients.size() >= 2) {
    reliabilityPanel_->append(
        *MakeReliabilityChart(weights, clients, window.mean_reliability_weight));
    auto* legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    legend->append(
        *MakeChartLegendEntry(kUrPink, T_("reliability_weight", "Reliability Weight")));
    legend->append(*MakeChartLegendEntry(kUrGreen, T_("total_clients", "Total Clients")));
    legend->append(
        *MakeChartLegendEntry(kUrTextMuted, T_("average_reliability_2", "Average Reliability")));
    reliabilityPanel_->append(*legend);
  }

  std::vector<urnet::CountryMultiplier> multipliers;
  if (window.country_multipliers) {
    for (const auto& entry : *window.country_multipliers) {
      if (entry.reliability_multiplier > 1.0) multipliers.push_back(entry);
    }
  }
  if (multipliers.empty()) return;
  std::stable_sort(multipliers.begin(), multipliers.end(),
                   [](const urnet::CountryMultiplier& a, const urnet::CountryMultiplier& b) {
                     return a.reliability_multiplier > b.reliability_multiplier;
                   });
  auto* rule = kit::MakeDivider();
  rule->set_margin_top(4);
  rule->set_margin_bottom(4);
  reliabilityPanel_->append(*rule);
  reliabilityPanel_->append(
      *MakeSizedLabel(T_("country_multipliers", "Country multipliers"), 15, "ur-value"));
  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* countryHeader = MakeSizedLabel(T_("country", "Country"), 12, "ur-caption");
  countryHeader->set_hexpand(true);
  header->append(*countryHeader);
  header->append(*MakeSizedLabel(T_("multiplier", "Multiplier"), 12, "ur-caption"));
  reliabilityPanel_->append(*header);
  for (const auto& entry : multipliers) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    const bool high = entry.reliability_multiplier >= kMultiplierHighlight;
    auto* country = MakeSizedLabel(entry.country, 13, "ur-value");
    country->set_hexpand(true);
    char factor[32];
    std::snprintf(factor, sizeof(factor), "x%.2f", entry.reliability_multiplier);
    auto* value = MakeSizedLabel(factor, 13, "ur-value");
    if (high) {
      country->add_css_class("ur-value-on");
      value->add_css_class("ur-value-on");
    }
    row->append(*country);
    row->append(*value);
    reliabilityPanel_->append(*row);
  }
}

void EarningsPage::ApplyLedgerMeta() {
  const int boardCount =
      pointsBoardShowing_ ? static_cast<int>(pointsRowsUi_.size()) : leaderboardCount_;
  const int count = (leaderboardTab_ != nullptr && leaderboardTab_->get_active())
                        ? boardCount
                        : static_cast<int>(epochs_.size());
  kit::SetTextOrCollapse(
      *paneB_.meta, count > 0 ? Glib::ustring(std::to_string(count)) : Glib::ustring());
}

void EarningsPage::OnLedgerTabChanged() {
  const bool history = !leaderboardTab_->get_active();
  historyHost_->set_visible(history);
  leaderboardHost_->set_visible(!history);
  ApplyLedgerMeta();
  if (!history && !leaderboardRequested_) {
    leaderboardRequested_ = true;
    LoadLeaderboard();
  }
  if (!history && pointsBoardShowing_) EnsurePointsBoard();
  UpdatePointsIndicator();
}

// ---- attach a Bittensor wallet -----------------------------------------------

void EarningsPage::OnConnectWithBridge() {
  if (connecting_ || consent_.Busy()) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  // whichever account the wallet picks
  ChooseBittensorWallet(
      [this](const std::string& walletId) { StartWalletSignature(walletId, std::string()); });
}

void EarningsPage::ChooseBittensorWallet(
    std::function<void(const std::string& walletId)> chosen) {
  auto* window = dynamic_cast<Gtk::Window*>(get_root());
  GtkWidget* dialog = NewBittensorWalletChooser(window ? window->gobj() : nullptr);
  struct Ctx {
    std::function<void(const std::string& walletId)> chosen;
    std::shared_ptr<uint64_t> epoch;
    uint64_t seen;
  };
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
        auto* ctx = static_cast<Ctx*>(data);
        if (*ctx->epoch != ctx->seen) return;  // the page was reset meanwhile
        const std::string_view walletId = bittensor::ChosenWallet(response ? response : "");
        if (!walletId.empty()) ctx->chosen(std::string(walletId));
      }),
      new Ctx{std::move(chosen), epoch_, *epoch_},
      +[](gpointer data, GClosure*) { delete static_cast<Ctx*>(data); }, G_CONNECT_DEFAULT);
  gtk_window_present(GTK_WINDOW(dialog));
}

void EarningsPage::OnToggleManualEntry() {
  manualEntryOpen_ = !manualEntryOpen_;
  RebuildWalletBlock();
  if (manualEntryOpen_ && walletAddressBox_ != nullptr) walletAddressBox_->grab_focus();
}

void EarningsPage::OnChangeWallet() {
  if (connecting_) return;
  changingWallet_ = !changingWallet_;
  RebuildWalletBlock();
}

void EarningsPage::OnWalletAddressChanged() {
  // every keystroke: forget the verdict, drop the in-flight answer, disarm
  check_.reset();
  checkedAddress_.clear();
  checkInFlight_ = false;
  ++checkGeneration_;
  kit::SetTextOrCollapse(*walletSupportingText_, {});
  RebuildWalletBlock();
  checkDebounce_.disconnect();
  checkDebounce_ = Glib::signal_timeout().connect(
      [this]() -> bool {
        ValidateWalletAddress();
        return false;  // non-repeating
      },
      kValidateDebounceMs);
}

// Syntax first, locally, before ANY network call; then the unauthenticated
// validate endpoint, which may warn (a new wallet) or block (a banned one).
void EarningsPage::ValidateWalletAddress() {
  const std::string address = TrimWhitespace(walletAddressBox_->get_text().raw());
  if (address.empty()) return;  // nothing typed yet: silent
  if (!sn::ValidateSs58(address)) {
    SnWalletCheck check;
    check.validSyntax = false;
    checkedAddress_ = address;
    check_ = check;
    kit::ApplySupportingText(*walletSupportingText_,
                             T_("invalid_ss58_address", "That is not a valid Bittensor address."),
                             kit::ValidationState::Invalid);
    walletSupportingText_->set_visible(true);
    RebuildWalletBlock();
    return;
  }
  // The ONE affordance allowed to decline SILENTLY: the user did not ask for
  // anything, so with no session the box says nothing at all.
  if (!CanCallApi()) return;
  const uint64_t generation = ++checkGeneration_;
  checkInFlight_ = true;
  kit::ApplySupportingText(*walletSupportingText_,
                           T_("checking_wallet_address", "Checking address…"),
                           kit::ValidationState::Validating);
  walletSupportingText_->set_visible(true);
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  sn::CheckWallet(host_, address,
                  [this, epoch, seen, generation, address](std::optional<SnWalletCheck> check,
                                                           std::string err) {
                    PostToMain([this, epoch, seen, generation, address, check = std::move(check),
                                err = std::move(err)] {
                      if (*epoch != seen) return;
                      ApplyWalletCheck(generation, address, check, err);
                    });
                  });
}

void EarningsPage::ApplyWalletCheck(uint64_t generation, const std::string& address,
                                    std::optional<SnWalletCheck> check, const std::string& err) {
  if (generation != checkGeneration_) return;  // the box moved on
  checkInFlight_ = false;
  if (!check) {
    // the validate call itself failed: the address is NOT sent anywhere until
    // it can be checked; retyping retries
    g_warning("earnings: sn wallet validate failed: %s", err.c_str());
    kit::ApplySupportingText(*walletSupportingText_,
                             T_("something_went_wrong", "Something went wrong."),
                             kit::ValidationState::Invalid);
    walletSupportingText_->set_visible(true);
    RebuildWalletBlock();
    return;
  }
  checkedAddress_ = address;
  check_ = check;
  Glib::ustring text;
  kit::ValidationState state = kit::ValidationState::Valid;
  if (!check->validSyntax) {
    text = T_("invalid_ss58_address", "That is not a valid Bittensor address.");
    state = kit::ValidationState::Invalid;
  } else if (check->banned) {
    // blocked: the Connect button stays off and nothing is sent
    text = T_("wallet_blocked", "This wallet can't be used with URnetwork.");
    state = kit::ValidationState::Invalid;
  } else if (!check->existsOnChain) {
    // a warning, not a block: the user may continue
    text = T_("wallet_looks_new_warning",
              "This address has no activity on the Bittensor chain yet. It looks like a new "
              "wallet. Make sure it is yours before continuing.");
    state = kit::ValidationState::Validating;
  } else {
    text = check->message;
  }
  if (text.empty()) {
    kit::SetTextOrCollapse(*walletSupportingText_, {});
  } else {
    kit::ApplySupportingText(*walletSupportingText_, text, state);
    walletSupportingText_->set_visible(true);
  }
  RebuildWalletBlock();
}

void EarningsPage::OnConnectManual() {
  if (connecting_ || consent_.Busy()) return;
  const std::string address = TrimWhitespace(walletAddressBox_->get_text().raw());
  if (address.empty()) return;
  if (!check_ || checkedAddress_ != address || !check_->validSyntax || check_->banned) {
    ValidateWalletAddress();  // the verdict is stale: ask again, never send unchecked
    return;
  }
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  // still signed: the wallet must sign with THIS account
  ChooseBittensorWallet(
      [this, address](const std::string& walletId) { StartWalletSignature(walletId, address); });
}

void EarningsPage::StartWalletSignature(const std::string& walletId,
                                        const std::string& expectedAddress) {
  if (connecting_) return;
  connecting_ = true;
  connectWalletId_ = walletId;
  RebuildWalletBlock();
  // minutes, not 20s: the bridge reports errors only when a deep link comes
  // BACK, and a closed browser tab produces nothing, ever; the manual sheet
  // lasts as long as its challenge
  const std::string transport =
      urnet::bittensorWalletTransportFor(walletId, std::string(bittensor::kPlatform));
  const uint32_t timeoutMs = bittensor::TimeoutMsFor(transport, walletId);
  NotifyContinueInBrowser(walletId);
  const uint32_t generation = BeginFlow(connectFlow_, static_cast<int>(timeoutMs), [this] {
    FinishConnecting();
    Notify(T_("wallet_connect_failed", "Failed to connect the wallet."),
           kit::Snackbar::Severity::Error);
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.SignBittensorConnect(
      walletId, expectedAddress,
      [this, epoch, seen, generation, expectedAddress](SdkHost::WalletSignature signature) {
        PostToMain([this, epoch, seen, generation, expectedAddress,
                    signature = std::move(signature)] {
          if (*epoch != seen) return;
          OnWalletSigned(generation, signature, expectedAddress);
        });
      });
}

void EarningsPage::NotifyContinueInBrowser(const std::string& walletId) {
  const std::string transport =
      urnet::bittensorWalletTransportFor(walletId, std::string(bittensor::kPlatform));
  if (transport != bittensor::kTransportBrowserBridge) return;
  // the page in the browser carries the flow now: say where to look
  const bittensor::ContinueText text = bittensor::ContinueTextFor(walletId);
  const std::string key(text.key);
  const std::string english(text.english);
  const char* localized = g_dpgettext2(GETTEXT_PACKAGE, key.c_str(), english.c_str());
  Notify(text.takesWalletName ? Format(localized, urnet::bittensorWalletDisplayName(walletId))
                              : std::string(localized),
         kit::Snackbar::Severity::Info);
}

void EarningsPage::OnWalletSigned(uint32_t generation, const SdkHost::WalletSignature& signature,
                                  const std::string& expectedAddress) {
  if (!SettleFlow(connectFlow_, generation, "wallet signature")) return;
  if (!signature.ok) {
    FinishConnecting();
    if (bittensor::IsCancelled(signature.error)) {
      // the user closed the manual sheet: the block is simply ready again
      return;
    }
    if (bridge::IsSuperseded(signature.error)) {
      // the user started another wallet flow (the Solana sheet): this attempt
      // ended by their choice, and the block is simply ready again
      g_message("earnings: the Bittensor wallet connect was superseded (%s)",
                signature.error.c_str());
      return;
    }
    Notify(signature.error.empty()
               ? Glib::ustring(T_("wallet_connect_failed", "Failed to connect the wallet."))
               : Glib::ustring(signature.error),
           kit::Snackbar::Severity::Error);
    return;
  }
  if (!expectedAddress.empty() && signature.address != expectedAddress) {
    FinishConnecting();
    Notify(T_("wallet_signature_mismatch_linux",
              "The wallet that signed is not the address you entered."),
           kit::Snackbar::Severity::Error);
    return;
  }
  // validation runs BEFORE the send, whichever way the address arrived
  if (!sn::ValidateSs58(signature.address)) {
    FinishConnecting();
    Notify(T_("invalid_ss58_address", "That is not a valid Bittensor address."),
           kit::Snackbar::Severity::Error);
    return;
  }
  if (check_ && checkedAddress_ == signature.address && check_->validSyntax) {
    // the manual path already checked this exact address
    if (check_->banned) {
      FinishConnecting();
      Notify(T_("wallet_blocked", "This wallet can't be used with URnetwork."),
             kit::Snackbar::Severity::Error);
      return;
    }
    SetSnWallet(signature.address, signature.signature, signature.message);
    return;
  }
  const uint32_t checkGeneration = BeginFlow(setWalletFlow_, kApiTimeoutMs, [this] {
    FinishConnecting();
    Notify(T_("something_went_wrong", "Something went wrong."), kit::Snackbar::Severity::Error);
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  const std::string address = signature.address;
  const std::string sig = signature.signature;
  const std::string message = signature.message;
  sn::CheckWallet(host_, address,
                  [this, epoch, seen, checkGeneration, address, sig, message](
                      std::optional<SnWalletCheck> check, std::string err) {
                    PostToMain([this, epoch, seen, checkGeneration, address, sig, message,
                                check = std::move(check), err = std::move(err)] {
                      if (*epoch != seen) return;
                      if (!SettleFlow(setWalletFlow_, checkGeneration, "wallet validate")) return;
                      if (!check) {
                        g_warning("earnings: sn wallet validate failed: %s", err.c_str());
                        FinishConnecting();
                        Notify(T_("something_went_wrong", "Something went wrong."),
                               kit::Snackbar::Severity::Error);
                        return;
                      }
                      if (!check->validSyntax) {
                        FinishConnecting();
                        Notify(T_("invalid_ss58_address",
                                  "That is not a valid Bittensor address."),
                               kit::Snackbar::Severity::Error);
                        return;
                      }
                      if (check->banned) {
                        // blocked: the address goes nowhere
                        FinishConnecting();
                        Notify(T_("wallet_blocked", "This wallet can't be used with URnetwork."),
                               kit::Snackbar::Severity::Error);
                        return;
                      }
                      if (!check->existsOnChain) {
                        Notify(T_("wallet_looks_new_warning",
                                  "This address has no activity on the Bittensor chain yet. "
                                  "It looks like a new wallet. Make sure it is yours before "
                                  "continuing."),
                               kit::Snackbar::Severity::Warning);
                      }
                      SetSnWallet(address, sig, message);
                    });
                  });
}

void EarningsPage::SetSnWallet(const std::string& address, const std::string& signature,
                               const std::string& message) {
  const uint32_t generation = BeginFlow(setWalletFlow_, kApiTimeoutMs, [this] {
    FinishConnecting();
    Notify(T_("wallet_connect_failed", "Failed to connect the wallet."),
           kit::Snackbar::Severity::Error);
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  sn::SetWallet(host_, address, ProviderClientId(), signature, message,
                [this, epoch, seen, generation, address](bool ok, std::string err) {
                  PostToMain([this, epoch, seen, generation, address, ok, err = std::move(err)] {
                    if (*epoch != seen) return;
                    ApplyWalletConnectResult(generation, ok, err, address);
                  });
                });
}

void EarningsPage::ApplyWalletConnectResult(uint32_t generation, bool ok,
                                            const std::string& serverError,
                                            const std::string& address) {
  if (!SettleFlow(setWalletFlow_, generation, "wallet connect")) return;
  if (!ok) {
    FinishConnecting();
    // a signature pasted from the manual wallet that is not from the entered
    // address: say so, and what to do in that wallet
    const bittensor::ErrorText mismatch = bittensor::ConnectErrorTextFor(
        SnErrorCode(serverError),
        connectWalletId_.empty() ? std::string()
                                 : urnet::bittensorWalletTransportFor(
                                       connectWalletId_, std::string(bittensor::kPlatform)));
    if (!mismatch.key.empty()) {
      const std::string key(mismatch.key);
      const std::string english(mismatch.english);
      Notify(Format(g_dpgettext2(GETTEXT_PACKAGE, key.c_str(), english.c_str()),
                    urnet::bittensorWalletDisplayName(connectWalletId_)),
             kit::Snackbar::Severity::Error);
      return;
    }
    // a coded refusal in the store's words (wallet_blocked, invalid address),
    // else the raw server error VERBATIM (often the only diagnostic); Error
    // severity persists until dismissed
    Notify(SnErrorMessage(serverError,
                          T_("wallet_connect_failed", "Failed to connect the wallet.")),
           kit::Snackbar::Severity::Error);
    return;
  }
  SnWalletInfo wallet;
  wallet.coldkeySs58 = address;
  wallet.clientId = ProviderClientId();
  wallet.setAtMillis = g_get_real_time() / 1000;
  wallet_ = wallet;
  walletState_ = Fetch::Ready;
  kit::SetTextOrCollapse(*walletStatus_, {});
  changingWallet_ = false;
  manualEntryOpen_ = false;
  check_.reset();
  checkedAddress_.clear();
  walletAddressBox_->set_text("");  // resets the verdict through changed()
  FinishConnecting();
  Notify(T_("wallet_connected", "Wallet connected."), kit::Snackbar::Severity::Success);
  RebuildUnclaimedTile();
  RebuildHistory();
  LoadWalletLayer();
}

void EarningsPage::FinishConnecting() {
  connecting_ = false;
  RebuildWalletBlock();
}

// ---- the network payout consent ------------------------------------------------
// consent::Flow decides; the section draws its view, and its ports answer it
// through the SDK's consent calls and the wallet's consent session over the
// issued message. Sign with wallet asks the wallet, then the coldkey.

void EarningsPage::RebuildConsentSection() {
  if (consentCard_ == nullptr) return;
  const consent::View view = consent_.Current();
  consentHeader_->set_visible(view.visible);
  consentCard_->set_visible(view.visible);
  if (!view.visible) return;
  Glib::ustring line;
  switch (view.line) {
    case consent::Line::Loading:
      line = T_("loading", "Loading...");
      break;
    case consent::Line::Unavailable:
      line = T_("something_went_wrong", "Something went wrong.");
      break;
    case consent::Line::None:
      line = T_("sn_consent_none", "No payout wallet yet. Earnings aren't paid until you sign.");
      break;
    case consent::Line::Active:
      line = Format(T_("sn_consent_active", "Earnings are paid to {}"),
                    ShortSs58(view.coldkeySs58));
      break;
    case consent::Line::Signing:
      line = T_("sn_consent_signing", "Waiting for your wallet's signature…");
      break;
  }
  consentLine_->set_text(line);
  // the whole coldkey behind its short form
  consentLine_->set_tooltip_text(view.coldkeySs58);
  consentLine_->set_has_tooltip(!view.coldkeySs58.empty());
  consentBody_->set_visible(view.body);
  Glib::ustring notice;
  switch (view.notice) {
    case consent::Notice::None:
      break;
    case consent::Notice::Expired:
      notice = T_("sn_consent_expired", "The request expired. Try again.");
      break;
    case consent::Notice::Failed:
      notice = T_("sn_consent_failed", "The signature didn't match this wallet. Try again.");
      break;
    case consent::Notice::OwnerOnly:
      notice = T_("sn_consent_owner_only", "Only the network owner can choose the payout wallet.");
      break;
  }
  kit::SetTextOrCollapse(*consentNotice_, notice);
  consentSignButton_->set_visible(view.action == consent::Action::Sign);
  consentChangeButton_->set_visible(view.action == consent::Action::Change);
  // the coldkey connect above holds the bridge while it runs
  consentSignButton_->set_sensitive(!connecting_);
  consentChangeButton_->set_sensitive(!connecting_);
  consentChangeNote_->set_visible(view.changeNote);
}

void EarningsPage::OnConsentAction() {
  // the bridge carries one wallet flow at a time
  if (connecting_ || consent_.Busy()) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  ChooseBittensorWallet([this](const std::string& walletId) { AskConsentColdkey(walletId); });
}

// The coldkey the consent names, typed after the wallet is chosen. The payout
// coldkey is a deliberate choice (it may not be the wallet's sign-in account),
// so the field starts empty, and Continue waits for an address the SDK's ss58
// check accepts: nothing is asked of the server before. The dialog is the
// app's AdwMessageDialog idiom (OnRemoveSolanaWallet), its deprecation
// warnings silenced here only.
void EarningsPage::AskConsentColdkey(const std::string& walletId) {
  auto* window = dynamic_cast<Gtk::Window*>(get_root());
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkWidget* dialog = adw_message_dialog_new(
      window ? window->gobj() : nullptr, T_("sn_consent_title", "Payout consent"),
      T_("sn_consent_body",
         "Sign once with your Bittensor wallet so this network's earnings are paid to it."));
  auto* fields = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  auto* field = Gtk::make_managed<Gtk::Entry>();
  field->add_css_class("ur-input");
  field->set_placeholder_text(T_("earnings_address_placeholder", "Enter a Bittensor address"));
  kit::SetAccessibleLabel(*field, T_("earnings_address_placeholder", "Enter a Bittensor address"));
  fields->append(*field);
  auto* invalid = MakeWrappedNote(
      T_("invalid_ss58_address", "That is not a valid Bittensor address."), "ur-row-note");
  invalid->add_css_class("ur-danger-text");
  invalid->set_visible(false);
  fields->append(*invalid);
  adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(dialog), GTK_WIDGET(fields->gobj()));
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", T_("cancel", "Cancel"),
                                   "continue", T_("continue_txt", "Continue"), nullptr);
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "continue",
                                             ADW_RESPONSE_SUGGESTED);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "continue");
  adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
  adw_message_dialog_set_response_enabled(ADW_MESSAGE_DIALOG(dialog), "continue", FALSE);
  field->signal_changed().connect([dialog, field, invalid] {
    const std::string address = TrimWhitespace(field->get_text().raw());
    const bool valid = sn::ValidateSs58(address);
    adw_message_dialog_set_response_enabled(ADW_MESSAGE_DIALOG(dialog), "continue", valid);
    invalid->set_visible(!address.empty() && !valid);
  });
  // Enter continues once the address is valid
  field->signal_activate().connect([dialog, field] {
    if (sn::ValidateSs58(TrimWhitespace(field->get_text().raw()))) {
      adw_message_dialog_response(ADW_MESSAGE_DIALOG(dialog), "continue");
    }
  });
  G_GNUC_END_IGNORE_DEPRECATIONS
  struct Ctx {
    EarningsPage* self;
    Gtk::Entry* field;
    std::string walletId;
    std::shared_ptr<uint64_t> epoch;
    uint64_t seen;
  };
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
        auto* ctx = static_cast<Ctx*>(data);
        if (*ctx->epoch != ctx->seen) return;  // the page was reset meanwhile
        if (g_strcmp0(response, "continue") != 0 || ctx->self->connecting_) return;
        ctx->self->consent_.Start(ctx->walletId, TrimWhitespace(ctx->field->get_text().raw()));
      }),
      new Ctx{this, field, walletId, epoch_, *epoch_},
      +[](gpointer data, GClosure*) { delete static_cast<Ctx*>(data); }, G_CONNECT_DEFAULT);
  gtk_window_present(GTK_WINDOW(dialog));
}

consent::Flow::Ports EarningsPage::ConsentPorts() {
  consent::Flow::Ports ports;
  // a read: it settles when it answers, like the page's other reads
  ports.status = [this](consent::Flow::StatusDone done) {
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    sn::FetchConsentStatus(host_, [epoch, seen, done](std::optional<consent::Status> status,
                                                      std::string err) {
      PostToMain([epoch, seen, done, status = std::move(status), err = std::move(err)] {
        if (*epoch != seen) return;
        if (!status) g_warning("earnings: sn consent status failed: %s", err.c_str());
        done(status, err);
      });
    });
  };
  ports.validSs58 = [](const std::string& address) { return sn::ValidateSs58(address); };
  ports.challenge = [this](const std::string& coldkeySs58, consent::Flow::ChallengeDone done) {
    const uint32_t generation = BeginFlow(consentStep_, kApiTimeoutMs, [done] {
      done(std::nullopt, T_("something_went_wrong", "Something went wrong."));
    });
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    sn::RequestConsentChallenge(
        host_, coldkeySs58,
        [this, epoch, seen, generation, done](std::optional<consent::Challenge> challenge,
                                              std::string err) {
          PostToMain([this, epoch, seen, generation, done, challenge = std::move(challenge),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!SettleFlow(consentStep_, generation, "consent challenge")) return;
            if (!challenge) g_warning("earnings: sn consent challenge failed: %s", err.c_str());
            done(challenge, err);
          });
        });
  };
  // the session takes the challenge as the SDK returned it: its message is
  // what the wallet signs. It waits as long as the coldkey connect's does:
  // minutes on the bridge, the challenge's life on the manual sheet and for
  // WalletConnect.
  ports.sign = [this](const std::string& walletId, const consent::Challenge& challenge,
                      consent::Flow::SignDone done) {
    const std::string transport =
        urnet::bittensorWalletTransportFor(walletId, std::string(bittensor::kPlatform));
    NotifyContinueInBrowser(walletId);
    const uint32_t generation = BeginFlow(
        consentStep_, static_cast<int>(bittensor::TimeoutMsFor(transport, walletId)), [done] {
          done(consent::Signed{false, {}, {}, {},
                               T_("wallet_connect_failed", "Failed to connect the wallet.")});
        });
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.SignBittensorConsent(
        walletId, sn::ToSdkChallenge(challenge),
        [this, epoch, seen, generation, done](SdkHost::WalletSignature signature) {
          PostToMain([this, epoch, seen, generation, done, signature = std::move(signature)] {
            if (*epoch != seen) return;
            if (!SettleFlow(consentStep_, generation, "consent signature")) return;
            // the session's proof: the flow submits it only for the issued
            // message and its coldkey
            done(consent::Signed{signature.ok, signature.address, signature.message,
                                 signature.signature, signature.error});
          });
        });
  };
  ports.submit = [this](const std::string& coldkeySs58, const std::string& message,
                        const std::string& signature, consent::Flow::SubmitDone done) {
    const uint32_t generation = BeginFlow(consentStep_, kApiTimeoutMs, [done] {
      done(consent::Submitted{false, T_("something_went_wrong", "Something went wrong.")});
    });
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    sn::SubmitConsent(host_, coldkeySs58, message, signature,
                      [this, epoch, seen, generation, done](bool ok, std::string err) {
                        PostToMain([this, epoch, seen, generation, done, ok, err = std::move(err)] {
                          if (*epoch != seen) return;
                          if (!SettleFlow(consentStep_, generation, "consent submit")) return;
                          if (!ok) g_warning("earnings: sn consent submit failed: %s", err.c_str());
                          done(consent::Submitted{ok, err});
                        });
                      });
  };
  return ports;
}

// ---- the Solana payout wallet --------------------------------------------------
// USDC payouts continue to a Solana payout wallet until the migration to
// Bittensor completes, and a network whose payouts are held for want of one is
// emailed "N USDC waiting". The overflow beside the Bittensor actions opens the
// connect sheet; the card under the Bittensor block shows the payout wallet and
// removes it. What shows, and when, is SolanaWalletPresentation.hpp's.

Gtk::MenuButton* EarningsPage::BuildWalletOverflow(
    const Glib::RefPtr<const Gio::MenuModel>& menu) {
  auto* button = Gtk::make_managed<Gtk::MenuButton>();
  button->set_icon_name("view-more-symbolic");
  button->add_css_class("flat");
  button->set_valign(Gtk::Align::CENTER);  // the row keeps its height
  button->set_menu_model(menu);
  // Icon-only: the glyph names nothing, so the button carries the name. A
  // GtkMenuButton hands keyboard focus to the toggle button inside it and does
  // not pass its own label on, so the name goes on that inner button too: it is
  // the control a keyboard or screen-reader user actually reaches.
  const Glib::ustring name = T_("wallet_options", "Wallet options");
  kit::SetAccessibleLabel(*button, name);
  for (auto* child = button->get_first_child(); child != nullptr;
       child = child->get_next_sibling()) {
    if (GTK_IS_TOGGLE_BUTTON(child->gobj())) kit::SetAccessibleLabel(*child, name);
  }
  button->set_tooltip_text(name);
  return button;
}

// Three independent requests, never chained and never touching the Bittensor
// block; each is marshaled with PostToMain and refused when the epoch moved or a
// newer round began. The round lands when all three have answered
// (solana::LegacyLoad), so the card never flashes in and out, and each read
// fails on its own terms (solana::CardFor). A plain reload keeps what is shown
// while the reads are out; a write passes `reset` and hides the card until they
// land, and so does another network.
void EarningsPage::LoadLegacyWallets(bool reset) {
  if (!CanCallApi()) {
    legacyLoad_.Abandon();
    legacyCommitted_ = solana::LegacyCommitted{};
    RebuildSolanaCard();
    return;
  }
  std::string networkId;
  if (auto jwt = host_.ParseByJwt()) networkId = jwt->NetworkId.value_or(std::string());
  const uint64_t round = legacyLoad_.Begin(legacyCommitted_, networkId, reset);
  RebuildSolanaCard();
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;

  // 1. the account wallets
  host_.api().getAccountWallets(
      [this, epoch, seen, round](std::optional<urnet::GetAccountWalletsResult> result,
                                 std::optional<std::string> err) {
        std::optional<std::vector<solana::LegacyWallet>> wallets;
        if (!err && result) {
          wallets.emplace();
          for (const auto& wallet : result->wallets.value_or(urnet::AccountWalletsList{})) {
            solana::LegacyWallet out;
            out.id = wallet.wallet_id.value_or(std::string());
            out.blockchain = wallet.blockchain;
            out.address = wallet.wallet_address;
            out.circleWalletId = wallet.circle_wallet_id.value_or(std::string());
            out.active = wallet.active;
            wallets->push_back(std::move(out));
          }
        }
        PostToMain([this, epoch, seen, round, wallets = std::move(wallets),
                    err = std::move(err)]() mutable {
          if (*epoch != seen) return;
          const bool ok = wallets.has_value();
          if (!ok) {
            g_warning("earnings: getAccountWallets failed: %s",
                      err ? err->c_str() : "(no result)");
          }
          if (!legacyLoad_.AnswerWallets(
                  round, ok, ok ? std::move(*wallets) : std::vector<solana::LegacyWallet>{})) {
            return;
          }
          ApplyLegacyWallets();
        });
      });

  // 2. which of them is the payout wallet (null when the network has none)
  host_.api().getPayoutWallet(
      [this, epoch, seen, round](std::optional<urnet::GetPayoutWalletIdResult> result,
                                 std::optional<std::string> err) {
        PostToMain([this, epoch, seen, round, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          const bool ok = !err && result.has_value();
          if (!ok) {
            // the last known id stands (LegacyLoad::Commit)
            g_message("earnings: getPayoutWallet failed: %s", err ? err->c_str() : "(no result)");
          }
          const std::string payoutWalletId =
              ok ? result->wallet_id.value_or(std::string()) : std::string();
          if (!legacyLoad_.AnswerPayout(round, ok, payoutWalletId)) return;
          ApplyLegacyWallets();
        });
      });

  // 3. the held payouts: every payment neither completed nor canceled
  host_.api().getAccountPayments(
      [this, epoch, seen, round](std::optional<urnet::GetNetworkAccountPaymentsResult> result,
                                 std::optional<std::string> err) {
        std::optional<int64_t> pending;
        std::string failure = err.value_or(std::string());
        if (!err && result && !result->error) {
          std::vector<solana::HeldPayment> payments;
          for (const auto& payment :
               result->account_payments.value_or(urnet::AccountPaymentsList{})) {
            solana::HeldPayment held;
            held.payoutNanoCents = payment.payout_nano_cents;
            held.completed = payment.completed.value_or(false);
            held.canceled = payment.canceled;
            payments.push_back(held);
          }
          pending = solana::PendingUsdcNanoCents(payments);
        } else if (failure.empty() && result && result->error) {
          failure = result->error->message;
        }
        PostToMain([this, epoch, seen, round, pending, failure] {
          if (*epoch != seen) return;
          if (!pending) {
            // the figure is unknown: a card still shows, without it
            g_warning("earnings: getAccountPayments failed: %s",
                      failure.empty() ? "(no result)" : failure.c_str());
          }
          if (!legacyLoad_.AnswerPayments(round, pending.has_value(), pending.value_or(0))) return;
          ApplyLegacyWallets();
        });
      });
}

void EarningsPage::ApplyLegacyWallets() {
  if (legacyLoad_.Commit(legacyCommitted_)) {
    RebuildSolanaCard();
    NotifyPromotedPayoutWallet();
  }
}

void EarningsPage::NotifyPromotedPayoutWallet() {
  if (!payoutRemoval_) return;
  const solana::PayoutRemoval removal = *payoutRemoval_;
  payoutRemoval_.reset();
  // the card shows the promoted wallet now; the line says payouts moved to it
  if (const auto promoted = solana::PromotedPayoutWallet(removal, legacyCommitted_)) {
    Notify(Format(T_("payouts_now_go_to", "Payouts now go to {}."),
                  solana::ShortAddress(promoted->address)),
           kit::Snackbar::Severity::Success);
  }
}

void EarningsPage::RebuildSolanaCard() {
  if (solanaCard_ == nullptr || usdcWaitingLine_ == nullptr ||
      usdcWaitingLineConnected_ == nullptr) {
    return;
  }
  const solana::CardView view = solana::CardFor(legacyCommitted_);
  const Glib::ustring waiting =
      view.pendingUsd.empty()
          ? Glib::ustring()
          : Glib::ustring(Format(T_("usdc_waiting", "Final USDC payout: {} USDC waiting"),
                                 view.pendingUsd));

  solanaCard_->set_visible(view.showCard);
  solanaCard_->set_sensitive(!removingSolanaWallet_);
  // a removal or a payout switch is out: neither item may start another write,
  // or the server could take a link and a removal of the same wallet in either
  // order
  const bool writing = removingSolanaWallet_ || switchingPayoutWallet_;
  connectSolanaAction_->set_enabled(!writing);
  removeSolanaAction_->set_enabled(!writing);
  if (view.showCard) {
    // a legacy Polygon payout wallet still receives USDC and keeps its card,
    // under the plain "Wallet" rather than a chain it is not on
    const Glib::ustring title = view.solana
                                    ? Glib::ustring(T_("solana_wallet", "Solana wallet"))
                                    : Glib::ustring(T_("wallet", "Wallet"));
    solanaTitle_->set_text(title);
    // the short form is visual only, and not selectable (copying it would not
    // give an address): the full address is the tooltip and the name
    solanaAddressLabel_->set_text(view.shortAddress);
    solanaAddressLabel_->set_tooltip_text(view.address);
    kit::SetAccessibleLabel(*solanaAddressLabel_, title + ", " + view.address);
  }
  kit::SetTextOrCollapse(*solanaPendingLabel_, view.showPending ? waiting : Glib::ustring());

  // The one waiting line sits in whichever Bittensor panel shows. "Change"
  // opens the connect panel under a connected wallet, and then the line stays
  // with the connected panel rather than showing twice.
  const bool bittensorConnected = wallet_.has_value() && walletState_ == Fetch::Ready;
  kit::SetTextOrCollapse(*usdcWaitingLine_, view.showWaitingLine && !bittensorConnected
                                                ? waiting
                                                : Glib::ustring());
  kit::SetTextOrCollapse(*usdcWaitingLineConnected_, view.showWaitingLine && bittensorConnected
                                                         ? waiting
                                                         : Glib::ustring());
}

void EarningsPage::OnConnectSolanaWallet() {
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: solana wallet sheet suppressed — a modal is already open");
    return;
  }
  if (removingSolanaWallet_ || switchingPayoutWallet_) {
    // the menu item is already insensitive; checked again at press time, out loud
    g_message("earnings: solana wallet sheet held while a payout wallet write is out");
    return;
  }
  // the sheet ends in API writes, so it opens on a signed-in page only; the
  // preview harness opens it with its actions off
  if (!previewMode_ && !CanCallApi()) {
    RefuseNoSession();
    return;
  }
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr) {
    g_warning("earnings: no window root; the solana wallet sheet was not opened");
    return;
  }
  auto sheet = std::make_shared<SolanaWalletSheet>(*root, host_, CanCallApi());
  sheet->on_connected = [this](std::string walletId) { OnSolanaConnected(walletId); };
  // dismissed while its create call was out: that answer is gone, so look again
  sheet->on_abandoned_link = [this] { LoadLegacyWallets(/*reset=*/true); };
  // a Bittensor connect still out does not hold the providers: pressing one
  // supersedes it in the host, and OnWalletSigned settles it quietly
  PresentSheet(sheet);
}

void EarningsPage::OnSolanaConnected(const std::string& walletId) {
  if (!CanCallApi()) {
    RefuseNoSession();
    LoadLegacyWallets(/*reset=*/true);
    return;
  }
  // The switch is decided on a FRESH read of the payout wallet, as android and
  // apple decide it: the committed id may be stale (another device may have
  // changed it), and the server adopts a new wallet on its own only when the
  // network had none. A failed read counts as none, which switches: harmless
  // for a wallet just linked. Until the flow settles the overflow's items wait.
  switchingPayoutWallet_ = true;
  RebuildSolanaCard();
  // the SDK drops setPayoutWallet silently (no callback, ever) when the id is
  // not a UUID: this watchdog is what reports it
  const uint32_t generation = BeginFlow(legacyFlow_, kApiTimeoutMs, [this] {
    switchingPayoutWallet_ = false;
    Notify(SolanaFailureText({}), kit::Snackbar::Severity::Error);
    LoadLegacyWallets(/*reset=*/true);
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().getPayoutWallet(
      [this, epoch, seen, generation, walletId](
          std::optional<urnet::GetPayoutWalletIdResult> result, std::optional<std::string> err) {
        std::string current;
        std::string failure;
        if (!err && result) {
          current = result->wallet_id.value_or(std::string());
        } else {
          failure = err.value_or(std::string("no result"));
        }
        PostToMain([this, epoch, seen, generation, walletId, current, failure] {
          if (*epoch != seen || legacyFlow_.generation != generation) return;
          if (!failure.empty()) {
            g_message("earnings: the payout wallet read before the switch failed (%s); switching",
                      failure.c_str());
          }
          if (!solana::NeedsPayoutSwitch(walletId, current)) {
            SettleFlow(legacyFlow_, generation, "payout wallet read");
            switchingPayoutWallet_ = false;
            Notify(T_("payout_wallet_updated", "Payout wallet updated"),
                   kit::Snackbar::Severity::Success);
            LoadLegacyWallets(/*reset=*/true);
            return;
          }
          SwitchPayoutWallet(walletId, generation);
        });
      });
}

void EarningsPage::SwitchPayoutWallet(const std::string& walletId, uint32_t generation) {
  urnet::SetPayoutWalletArgs args;
  args.wallet_id = walletId;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().setPayoutWallet(
      args, [this, epoch, seen, generation](std::optional<urnet::SetPayoutWalletResult> result,
                                            std::optional<std::string> err) {
        const bool ok = result.has_value() && !err.has_value();
        const std::string detail = err.value_or(std::string());
        PostToMain([this, epoch, seen, generation, ok, detail] {
          if (*epoch != seen) return;
          if (!SettleFlow(legacyFlow_, generation, "payout wallet switch")) return;
          switchingPayoutWallet_ = false;
          if (ok) {
            Notify(T_("payout_wallet_updated", "Payout wallet updated"),
                   kit::Snackbar::Severity::Success);
          } else {
            // the wallet exists either way; the switch is the second half of
            // linking, so it fails in linking's words
            g_warning("earnings: setPayoutWallet failed: %s",
                      detail.empty() ? "(no result)" : detail.c_str());
            Notify(SolanaFailureText(detail), kit::Snackbar::Severity::Error);
          }
          LoadLegacyWallets(/*reset=*/true);
        });
      });
}

void EarningsPage::OnRemoveSolanaWallet() {
  const solana::CardView card = solana::CardFor(legacyCommitted_);
  // no card, nothing to remove; a write out holds the item
  if (removingSolanaWallet_ || switchingPayoutWallet_ || !card.showCard) return;
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: wallet removal suppressed — a modal is already open");
    return;
  }
  // CanCallApi BEFORE the confirmation: it ends in an API write. The preview
  // harness opens the confirmation anyway, to be looked at, with Remove disabled
  // (RemoveSolanaWallet is gated again).
  const bool allowActions = CanCallApi();
  if (!allowActions && !previewMode_) {
    RefuseNoSession();
    return;
  }
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr) {
    g_warning("earnings: no window root; the wallet removal was not confirmed");
    return;
  }
  // The app's confirmation idiom (MainWindow::OnSolanaChooser). AdwMessageDialog
  // is deprecated from libadwaita 1.6 in favour of AdwAlertDialog; it stays the
  // one idiom this app uses, so its warnings are silenced here only. The body
  // covers both outcomes of removing the payout wallet: another of the network's
  // Solana or Polygon wallets takes over when there is one (the server picks it),
  // and USDC payouts are held when there is none.
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkWidget* dialog = adw_message_dialog_new(
      GTK_WINDOW(root->gobj()), T_("remove_wallet", "Remove wallet"),
      T_("remove_wallet_moves_or_holds_payouts",
         "USDC payouts move to another of your Solana or Polygon wallets, or are held until "
         "you connect one."));
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel", T_("cancel", "Cancel"),
                                   "remove", T_("remove", "Remove"), nullptr);
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
  adw_message_dialog_set_close_response(ADW_MESSAGE_DIALOG(dialog), "cancel");
  if (!allowActions) {
    adw_message_dialog_set_response_enabled(ADW_MESSAGE_DIALOG(dialog), "remove", FALSE);
  }
  G_GNUC_END_IGNORE_DEPRECATIONS
  struct Ctx {
    EarningsPage* self;
    std::shared_ptr<bool> alive;
    std::string walletId;
  };
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
        auto* ctx = static_cast<Ctx*>(data);
        if (!*ctx->alive || g_strcmp0(response, "remove") != 0) return;
        ctx->self->RemoveSolanaWallet(ctx->walletId);
      }),
      new Ctx{this, alive_, card.walletId},
      +[](gpointer data, GClosure*) { delete static_cast<Ctx*>(data); }, G_CONNECT_DEFAULT);
  gtk_window_present(GTK_WINDOW(dialog));
}

void EarningsPage::RemoveSolanaWallet(const std::string& walletId) {
  if (removingSolanaWallet_ || walletId.empty()) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  removingSolanaWallet_ = true;
  RebuildSolanaCard();  // the card goes insensitive while the removal is out
  const uint32_t generation = BeginFlow(removeSolanaFlow_, kApiTimeoutMs, [this] {
    removingSolanaWallet_ = false;
    Notify(SolanaFailureText({}), kit::Snackbar::Severity::Error);
    LoadLegacyWallets();
  });
  urnet::RemoveWalletArgs args;
  args.wallet_id = walletId;
  // the payout wallet now, to tell a promotion from no payout wallet afterwards
  const solana::PayoutRemoval removal{legacyCommitted_.networkId, walletId,
                                      legacyCommitted_.payoutWalletId};
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().removeWallet(
      args, [this, epoch, seen, generation, removal](std::optional<urnet::RemoveWalletResult> result,
                                                     std::optional<std::string> err) {
        const bool ok = !err.has_value() && result.has_value() && result->success;
        std::string detail;
        if (result && result->error) detail = result->error->message;
        if (detail.empty() && err) detail = *err;
        PostToMain([this, epoch, seen, generation, ok, detail, removal] {
          if (*epoch != seen) return;
          if (!SettleFlow(removeSolanaFlow_, generation, "wallet removal")) return;
          removingSolanaWallet_ = false;
          if (!ok) {
            g_warning("earnings: removeWallet failed: %s",
                      detail.empty() ? "(no detail)" : detail.c_str());
            Notify(SolanaFailureText(detail), kit::Snackbar::Severity::Error);
            RebuildSolanaCard();
            return;
          }
          // Removing the payout wallet makes another active Solana or Polygon
          // wallet the payout wallet when the network has one: the round after
          // it names it (NotifyPromotedPayoutWallet). Otherwise the removal
          // reports itself by the card leaving: the store has no "wallet
          // removed" sentence, and inventing English is banned.
          payoutRemoval_ = removal;
          LoadLegacyWallets(/*reset=*/true);
        });
      });
}

// ---- claim -------------------------------------------------------------------

void EarningsPage::OnClaim() {
  if (!wallet_) {
    Notify(T_("connect_wallet_first", "Connect a Bittensor wallet first."),
           kit::Snackbar::Severity::Error);
    return;
  }
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: claim dialog suppressed — a modal is already open");
    return;
  }
  OpenClaimSheet(CanCallApi());
}

void EarningsPage::OpenClaimSheet(bool allowActions) {
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr || !wallet_) {
    g_warning("earnings: no window root; the claim dialog was not opened");
    return;
  }
  // the preview shows the dialog as it would be with a device that can claim;
  // the action itself is gated in StartClaim
  auto sheet = std::make_shared<ClaimAlphaSheet>(*root, wallet_->coldkeySs58, claims_, gas_,
                                                 sn::ClaimsAvailable(host_) || previewMode_);
  sheet->on_claim = [this](std::vector<int64_t> epochs) { StartClaim(std::move(epochs)); };
  sheet->on_open_link = [this](const std::string& url) { OpenLink(url); };
  sheet->explorer_url = [this](const std::string& txHash) {
    return sn::ExplorerTxUrl(host_, txHash);
  };
  sheet->SetGas(gas_);  // re-renders the rows with the explorer resolver in place
  claimSheet_ = sheet;
  PresentSheet(sheet);
  if (allowActions && !gas_) LoadGas();
}

void EarningsPage::StartClaim(std::vector<int64_t> epochs) {
  auto sheet = claimSheet_.lock();
  if (epochs.empty() || claiming_) return;
  if (!CanCallApi()) {
    // the sheet is modal: the refusal renders ON it
    g_warning("earnings: refusing a claim with no session");
    if (sheet) sheet->ShowError(T_("please_login_to_urnetwork", "Please login to URnetwork"));
    return;
  }
  if (!sn::ClaimsAvailable(host_)) {
    if (sheet) {
      sheet->ShowError(
          T_("claim_unavailable_linux", "Claiming is not available on this device yet."));
    }
    return;
  }
  claiming_ = true;
  if (sheet) sheet->OnSending(epochs);
  RebuildUnclaimedTile();
  RebuildLedgerTile();
  const uint32_t generation = BeginFlow(claimFlow_, kChainTimeoutMs, [this] {
    claiming_ = false;
    if (auto open = claimSheet_.lock()) {
      open->ShowError(T_("chain_rpc_unreachable", "The chain RPC is unreachable. Try again."));
      open->OnDone();
    }
    RebuildUnclaimedTile();
    RebuildLedgerTile();
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  sn::ClaimEvents events;
  events.sent = [this, epoch, seen](int64_t claimEpoch, std::string txHash) {
    PostToMain([this, epoch, seen, claimEpoch, txHash = std::move(txHash)] {
      if (*epoch != seen) return;
      for (auto& row : claims_) {
        if (row.epoch == claimEpoch) row.txHash = txHash;
      }
      if (auto open = claimSheet_.lock()) open->OnSent(claimEpoch, txHash);
    });
  };
  events.confirmed = [this, epoch, seen](int64_t claimEpoch, std::string txHash,
                                         int64_t amountRao) {
    PostToMain([this, epoch, seen, claimEpoch, txHash = std::move(txHash), amountRao] {
      if (*epoch != seen) return;
      totalClaimableRao_ = 0;
      for (auto& row : claims_) {
        if (row.epoch == claimEpoch) {
          row.status = "claimed";
          if (!txHash.empty()) row.txHash = txHash;
          if (amountRao > 0) row.amountRao = amountRao;
        }
        if (row.status == "claimable") totalClaimableRao_ += row.amountRao;
      }
      RebuildUnclaimedTile();
      RebuildHistory();
      if (auto open = claimSheet_.lock()) open->OnConfirmed(claimEpoch, txHash, amountRao);
    });
  };
  events.failed = [this, epoch, seen](int64_t claimEpoch, std::string message) {
    PostToMain([this, epoch, seen, claimEpoch, message = std::move(message)] {
      if (*epoch != seen) return;
      g_warning("earnings: claim for epoch %lld failed: %s",
                static_cast<long long>(claimEpoch), message.c_str());
      if (auto open = claimSheet_.lock()) open->OnFailed(claimEpoch, message);
    });
  };
  events.done = [this, epoch, seen, generation] {
    PostToMain([this, epoch, seen, generation] {
      if (*epoch != seen) return;
      if (!SettleFlow(claimFlow_, generation, "claim")) return;
      claiming_ = false;
      if (auto open = claimSheet_.lock()) open->OnDone();
      RebuildUnclaimedTile();
      RebuildLedgerTile();
      LoadWalletLayer();  // the chain's answer replaces the optimistic rows
    });
  };
  sn::Claim(host_, epochs, std::move(events));
}

// ---- the settlement v2 claim -----------------------------------------------------

void EarningsPage::OnLedgerClaim() {
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr || !wallet_) {
    g_warning("earnings: no window root or wallet; the v2 claim dialog was not opened");
    return;
  }
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: v2 claim dialog suppressed — a modal is already open");
    return;
  }
  // the preview shows the dialog as it would be with a device that can claim;
  // the action itself is gated in StartLedgerClaim
  auto sheet = std::make_shared<LedgerClaimSheet>(*root, wallet_->coldkeySs58, ledgerAccounts_,
                                                  gas_, sn::ClaimsAvailable(host_) || previewMode_);
  sheet->on_claim = [this] { StartLedgerClaim(); };
  sheet->on_open_link = [this](const std::string& url) { OpenLink(url); };
  sheet->explorer_url = [this](const std::string& txHash) {
    return sn::ExplorerTxUrl(host_, txHash);
  };
  ledgerClaimSheet_ = sheet;
  PresentSheet(sheet);
  if (CanCallApi() && !gas_) LoadGas();
}

void EarningsPage::StartLedgerClaim() {
  auto sheet = ledgerClaimSheet_.lock();
  if (claiming_) return;
  if (!CanCallApi()) {
    // the sheet is modal: the refusal renders ON it
    g_warning("earnings: refusing a v2 claim with no session");
    if (sheet) sheet->ShowError(T_("please_login_to_urnetwork", "Please login to URnetwork"));
    return;
  }
  if (!sn::ClaimsAvailable(host_)) {
    if (sheet) {
      sheet->ShowError(
          T_("claim_unavailable_linux", "Claiming is not available on this device yet."));
    }
    return;
  }
  claiming_ = true;
  if (sheet) sheet->OnSending();
  RebuildUnclaimedTile();
  RebuildLedgerTile();
  // past the SDK's own bounds for every account it may claim
  const int watchdogMs = snledger::ClaimWatchdogMs(ledgerAccounts_);
  const uint32_t generation = BeginFlow(ledgerClaimFlow_, watchdogMs, [this] {
    claiming_ = false;
    if (auto open = ledgerClaimSheet_.lock()) {
      open->ShowError(T_("chain_rpc_unreachable", "The chain RPC is unreachable. Try again."));
      open->OnDone();
    }
    RebuildUnclaimedTile();
    RebuildLedgerTile();
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  urnet::SnLedgerClaimCallback callback;
  callback.sent = [this, epoch, seen](int64_t noId, int64_t index, std::string txHash) {
    PostToMain([this, epoch, seen, noId, index, txHash = std::move(txHash)] {
      if (*epoch != seen) return;
      if (auto open = ledgerClaimSheet_.lock()) open->OnSent(noId, index, txHash);
    });
  };
  callback.confirmed = [this, epoch, seen](int64_t noId, int64_t index, std::string txHash,
                                           int64_t amountRao) {
    PostToMain([this, epoch, seen, noId, index, txHash = std::move(txHash), amountRao] {
      if (*epoch != seen) return;
      if (auto open = ledgerClaimSheet_.lock()) open->OnConfirmed(noId, index, txHash, amountRao);
    });
  };
  callback.failed = [this, epoch, seen](int64_t noId, int64_t index, std::string message) {
    PostToMain([this, epoch, seen, noId, index, message = std::move(message)] {
      if (*epoch != seen) return;
      g_warning("earnings: v2 claim for pool %lld account %lld failed: %s",
                static_cast<long long>(noId), static_cast<long long>(index), message.c_str());
      if (auto open = ledgerClaimSheet_.lock()) open->OnFailed(noId, index, message);
    });
  };
  callback.done = [this, epoch, seen, generation] {
    PostToMain([this, epoch, seen, generation] {
      if (*epoch != seen) return;
      if (!SettleFlow(ledgerClaimFlow_, generation, "v2 claim")) return;
      claiming_ = false;
      if (auto open = ledgerClaimSheet_.lock()) open->OnDone();
      RebuildUnclaimedTile();
      RebuildLedgerTile();
      LoadWalletLayer();  // the accounts again, as the chain now has them
    });
  };
  sn::LedgerClaim(host_, std::move(callback));
}

// ---- the public-leaderboard switch -------------------------------------------

void EarningsPage::SetRankingToggle(bool on) {
  // THE ECHO GUARD: the handler cannot tell a user flip from the programmatic
  // render of the server's answer, so every programmatic write goes here.
  applyingRankingToggle_ = true;
  publicToggle_->set_active(on);
  applyingRankingToggle_ = false;
}

void EarningsPage::OnLeaderboardPublicToggled() {
  if (applyingRankingToggle_) return;
  const bool requested = publicToggle_->get_active();
  if (requested == rankingPublic_) return;
  if (settingRankingPublic_) {
    SetRankingToggle(rankingPublic_);
    return;
  }
  if (!CanCallApi()) {
    SetRankingToggle(rankingPublic_);
    RefuseNoSession();
    return;
  }
  settingRankingPublic_ = true;
  publicToggle_->set_sensitive(false);
  const uint32_t generation = BeginFlow(rankingFlow_, kApiTimeoutMs, [this] {
    settingRankingPublic_ = false;
    publicToggle_->set_sensitive(true);
    SetRankingToggle(rankingPublic_);
    Notify(T_("something_went_wrong", "Something went wrong."),
           kit::Snackbar::Severity::Error);
  });

  urnet::SetNetworkRankingPublicArgs args;
  args.is_public = requested;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().setNetworkLeaderboardPublic(
      args, [this, epoch, seen, generation, requested](
                std::optional<urnet::SetNetworkRankingPublicResult> result,
                std::optional<std::string> err) {
        std::string detail = err.value_or(std::string());
        if (detail.empty() && result && result->error) detail = result->error->message;
        const bool ok = !err.has_value() && result.has_value() && !result->error;
        PostToMain([this, epoch, seen, generation, ok, requested, detail] {
          if (*epoch != seen) return;
          ApplyRankingPublicResult(generation, ok, requested, detail);
        });
      });
}

void EarningsPage::ApplyRankingPublicResult(uint32_t generation, bool ok, bool requested,
                                            const std::string& serverError) {
  if (!SettleFlow(rankingFlow_, generation, "leaderboard visibility set")) return;
  settingRankingPublic_ = false;
  publicToggle_->set_sensitive(true);
  if (!ok) {
    SetRankingToggle(rankingPublic_);
    Notify(serverError.empty()
               ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
               : Glib::ustring(serverError),
           kit::Snackbar::Severity::Error);
    return;
  }
  rankingPublic_ = requested;
  LoadLeaderboard();  // the board ITSELF changes: our row masks or unmasks
}

// ---- sheets ------------------------------------------------------------------

void EarningsPage::PresentSheet(const std::shared_ptr<Gtk::Window>& sheet) {
  if (!sheet) return;
  if (auto* root = dynamic_cast<Gtk::Window*>(get_root())) {
    sheet->set_transient_for(*root);
  }
  sheet->set_modal(true);
  sheet->add_css_class("ur-sheet");
  AddEscapeToClose(*sheet);
  sheet_ = sheet;
  if (on_sheet_open_changed) on_sheet_open_changed(true);
  auto alive = alive_;
  sheet->signal_hide().connect([this, alive] {
    // never destroy a window inside its own signal
    PostToMain([this, alive] {
      if (!*alive) return;
      CloseSheet();
    });
  });
  sheet->present();
}

void EarningsPage::OpenTop200SignSheet(const std::optional<std::string>& prefill, bool fromLink) {
  if (auto open = top200SignSheet_.lock()) {
    if (fromLink) open->Prefill(prefill);
    open->present();
    return;
  }
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: top 200 sign sheet suppressed — a modal is already open");
    return;
  }
  if (dynamic_cast<Gtk::Window*>(get_root()) == nullptr) {
    g_warning("earnings: no window root; the top 200 sign sheet was not opened");
    return;
  }
  auto sheet = std::make_shared<Top200SignSheet>(host_);
  if (fromLink) sheet->Prefill(prefill);
  top200SignSheet_ = sheet;
  PresentSheet(sheet);
}

void EarningsPage::CloseSheet() {
  if (!sheet_) return;
  sheet_.reset();
  claimSheet_.reset();
  ledgerClaimSheet_.reset();
  top200SignSheet_.reset();
  if (on_sheet_open_changed) on_sheet_open_changed(false);
}

// ---- preview sample ----------------------------------------------------------
// URNETWORK_PREVIEW_SAMPLE=1 on top of --preview-ui (BOTH gates; the window
// checks them). Obviously-synthetic rows flow through the SAME Apply*
// functions the server's answers do. URNETWORK_PREVIEW_WALLET=1 adds the
// attached-wallet layer; URNETWORK_PREVIEW_TOP200=bound the bound status.
void EarningsPage::ApplyPreviewSample() {
  g_warning("EarningsPage: preview sample pinned — earnings content is SYNTHETIC");
  samplePinned_ = true;

  auto point = [](const char* event, int64_t points) {
    urnet::AccountPoint out;
    out.event = event;
    out.point_value = points * 1000000;  // nano points: the helper divides by 1e6
    return out;
  };
  urnet::AccountPointsList samplePoints;
  samplePoints.push_back(point("payout", 5120));
  samplePoints.push_back(point("payout_linked_account", 640));
  samplePoints.push_back(point("payout_reliability", 315));

  std::vector<AccountEpochRow> sampleEpochs;
  const int64_t epochShare[6][3] = {{42, 71, 1240}, {41, 64, 1105}, {40, 58, 990},
                                    {39, 80, 1380}, {38, 45, 760},  {37, 52, 880}};
  for (const auto& sample : epochShare) {
    AccountEpochRow row;
    row.epoch = sample[0];
    row.shareBps = sample[1];
    row.points = static_cast<double>(sample[2]);
    row.startMillis = 1756000000000LL + sample[0] * 86400000LL;
    row.endMillis = row.startMillis + 86400000LL;
    sampleEpochs.push_back(row);
  }

  urnet::ReliabilityWindow window;
  window.mean_reliability_weight = 0.72;
  window.max_total_client_count = 184;
  window.bucket_duration_seconds = 3600;
  urnet::Float64List weights;
  urnet::IntList clients;
  for (int bucket = 0; bucket < 24; ++bucket) {
    weights.push_back(0.45 + 0.35 * std::sin(bucket * 0.5));
    clients.push_back(60 + (bucket * 7) % 120);
  }
  window.reliability_weights = weights;
  window.total_client_counts = clients;
  urnet::CountryMultiplierList multipliers;
  auto multiplier = [](const char* country, const char* code, double factor) {
    urnet::CountryMultiplier out;
    out.country = country;
    out.country_code = code;
    out.reliability_multiplier = factor;
    return out;
  };
  multipliers.push_back(multiplier("Sample Republic", "sr", 2.40));
  multipliers.push_back(multiplier("Sample Kingdom", "sk", 1.75));
  multipliers.push_back(multiplier("Sample Islands", "si", 1.10));
  multipliers.push_back(multiplier("Sample Plains", "sp", 1.00));  // proves the filter
  window.country_multipliers = multipliers;

  auto earner = [](const std::string& id, const char* name, float mib, bool isPublic,
                   bool profane) {
    urnet::LeaderboardEarner out;
    out.network_id = id;
    out.network_name = name;
    out.net_mib_count = mib;
    out.is_public = isPublic;
    out.contains_profanity = profane;
    return out;
  };
  ownNetworkId_ = "sample-network-own";
  urnet::LeaderboardEarnersList sampleEarners;
  sampleEarners.push_back(earner("sample-network-1", "sample-alpha", 4194304.f, true, false));
  sampleEarners.push_back(earner("sample-network-2", "sample-hidden", 3145728.f, false, false));
  sampleEarners.push_back(earner("sample-network-3", "sample-flagged", 2097152.f, true, true));
  sampleEarners.push_back(
      earner("sample-network-own", "sample-your-network", 786432.f, true, false));
  sampleEarners.push_back(earner("sample-network-4", "sample-omega", 524288.f, true, false));

  urnet::NetworkRanking ranking;
  ranking.leaderboard_rank = 42;
  ranking.net_mib_count = 786432.f;
  ranking.leaderboard_public = true;

  SnHeadInfo head;
  head.eligible = true;
  head.score = 0.84;
  head.floor = 0.61;
  head.rankEstimate = 118;
  head.cutoff = 200;
  head.epoch = 42;
  head.source = "server";
  if (const char* top200 = g_getenv("URNETWORK_PREVIEW_TOP200");
      top200 && std::string(top200) == "bound") {
    head.bound = true;
    head.uid = 143;
    head.rank = 118;
    head.hotkey = "5DqSAMPLEsampleSAMPLEsampleSAMPLEsampleSAMP9n";
    head.score = 0.64;  // within the margin: shows the demotion warning
  }

  ApplyPoints(samplePoints, Fetch::Ready);
  // URNETWORK_PREVIEW_POINTS=1: the Leaderboard tab on its Points board, so
  // the position indicator can be looked at (ApplyPointsBoardSample)
  if (g_getenv("URNETWORK_PREVIEW_POINTS") != nullptr) {
    if (leaderboardTab_ != nullptr) leaderboardTab_->set_active(true);
    if (pointsBoardTab_ != nullptr) pointsBoardTab_->set_active(true);
  }
  ApplyEpochs(sampleEpochs, Fetch::Ready);
  ApplyReliability(window, Fetch::Ready);
  ApplyRanking(ranking, true);
  ApplyLeaderboard(sampleEarners, Fetch::Ready);
  ApplyHead(head, Fetch::Ready);
  leaderboardRequested_ = true;  // the sample IS the leaderboard answer

  // URNETWORK_PREVIEW_SOLANA=1: a Solana payout wallet with 3.87 USDC waiting
  // (the card); URNETWORK_PREVIEW_USDC_WAITING=1: the same figure with no payout
  // wallet (the waiting line). Through the same round of reads the server's
  // answers take.
  const bool previewSolana = g_getenv("URNETWORK_PREVIEW_SOLANA") != nullptr;
  if (previewSolana || g_getenv("URNETWORK_PREVIEW_USDC_WAITING") != nullptr) {
    solana::LegacyWallet sample;
    sample.id = "sample-solana-wallet";
    sample.blockchain = urnet::SOL;
    sample.address = kSampleSolanaAddress;
    sample.active = true;
    const uint64_t round =
        legacyLoad_.Begin(legacyCommitted_, ownNetworkId_, /*reset=*/true);
    legacyLoad_.AnswerWallets(round, true,
                              previewSolana ? std::vector<solana::LegacyWallet>{sample}
                                            : std::vector<solana::LegacyWallet>{});
    legacyLoad_.AnswerPayout(round, true, previewSolana ? sample.id : std::string());
    legacyLoad_.AnswerPayments(round, true, kSamplePendingNanoCents);
    ApplyLegacyWallets();
  }

  if (g_getenv("URNETWORK_PREVIEW_WALLET") == nullptr) {
    ApplySnWallet(std::nullopt, Fetch::Ready);
    if (g_getenv("URNETWORK_PREVIEW_MANUAL") != nullptr) {
      // the manual entry with an address that passed the syntax check and
      // came back from the validate call as a new wallet (warn, allow)
      manualEntryOpen_ = true;
      walletAddressBox_->set_text(kSampleColdkey);
      SnWalletCheck check;
      check.validSyntax = true;
      check.existsOnChain = false;
      ApplyWalletCheck(checkGeneration_, kSampleColdkey, check, std::string());
    }
    return;
  }
  // the attached-wallet layer: an obviously synthetic coldkey whose short form
  // matches the design review ("5F3s…kQ9v")
  SnWalletInfo wallet;
  wallet.coldkeySs58 = kSampleColdkey;
  wallet.clientId = "sample-client";
  wallet.setAtMillis = 1756000000000LL;
  ApplySnWallet(wallet, Fetch::Ready);
  auto claim = [](int64_t epoch, int64_t shareBps, int64_t rao, const char* status,
                  const char* tx) {
    SnClaimRow out;
    out.epoch = epoch;
    out.shareBps = shareBps;
    out.amountRao = rao;
    out.status = status;
    out.claimOpenBlock = 5000000 + epoch * 7200;
    out.expiryBlock = out.claimOpenBlock + 7200 * 14;
    out.txHash = tx;
    return out;
  };
  std::vector<SnClaimRow> sampleClaims;
  sampleClaims.push_back(claim(42, 71, 2031000000, "claimable", ""));
  sampleClaims.push_back(claim(41, 64, 1210000000, "claimable", ""));
  sampleClaims.push_back(
      claim(40, 58, 950000000, "claimed", "0xSAMPLEtxSAMPLEtxSAMPLEtxSAMPLEtxSAMPLEtxSAMPLE40"));
  sampleClaims.push_back(claim(39, 80, 1380000000, "expired", ""));
  // epoch 43 closes three days out on the sample's 12 s blocks: the
  // 14,400-block finalize offset, then 8 claim epochs plus 1 grace epoch
  snpayout::EpochSchedule schedule;
  schedule.epoch = 43;
  schedule.endMillis = g_get_real_time() / 1000 + 21'600LL * 12'000;
  schedule.claimOpenMillis = schedule.endMillis + 14'400LL * 12'000;
  schedule.expiryMillis = schedule.endMillis + (9LL * 50'400 - 1) * 12'000;
  ApplyClaims(sampleClaims, 2031000000LL + 1210000000LL, Fetch::Ready, {}, schedule);
  SnGasInfo gas;
  gas.address = "0x9a1cSAMPLEsampleSAMPLEsampleSAMPLEsamplee07f";
  gas.mirrorSs58 = "5GhSAMPLEsampleSAMPLEsampleSAMPLEsampleSAMPL2q";
  gas.balanceKnown = true;
  // URNETWORK_PREVIEW_GAS=low: the needs-gas state (the mirror address + top-up);
  // otherwise enough for a v1 and a v2 claim
  const char* gasPreview = g_getenv("URNETWORK_PREVIEW_GAS");
  gas.tao = (gasPreview && std::string(gasPreview) == "low") ? 0.0002 : 0.0251;
  ApplyGas(gas);
  // the settlement v2 tile: one pool account, partly paid, that a pending root raises
  snledger::Account ledger;
  ledger.vault = "0x5a3e0000000000000000000000000000000000a2";
  ledger.noId = 1;
  ledger.index = 42;
  ledger.finalizedRao = 5120000000;
  ledger.paidRao = 1879000000;
  ledger.claimableRao = 3241000000;
  ledger.pending = true;
  ledger.pendingRao = 5870000000;
  ApplyLedger(snledger::Load::Ready, {ledger}, /*claimConfigured=*/true);
}

void EarningsPage::ShowPreviewClaimDialog() {
  if (!wallet_) {
    g_warning("earnings: URNETWORK_PREVIEW_CLAIM needs URNETWORK_PREVIEW_WALLET=1");
    return;
  }
  OpenClaimSheet(/*allowActions=*/false);
}

// ---- the points board --------------------------------------------------------
//
// The all-time points leaderboard (android/POINTSLEADERBOARD.md), the Android
// screen's structure on the ledger pane: a Data | Points switch above the
// board, sort chips above the rows, rows paged in by the SDK controller as the
// list nears its end, and this network's own block beside it on pane C. The
// controller (PointsLeaderboardViewController) is the ONLY source of rows,
// ranks, sort and pages; this file only mirrors its state and forwards the
// sort, load-more and refresh intents.

namespace {

// two lines: the emoji tag over the network name
constexpr int kPointsRowHeight = 52;

EarningsPage::PointsRowUi ToPointsRowUi(const urnet::PointsLeaderboardRow& row) {
  EarningsPage::PointsRowUi out;
  out.position = row.position;
  out.networkId = row.network_id.value_or(std::string());
  out.displayName = row.display_name.value_or(std::string());
  out.emojiTag = row.emoji_tag.value_or(std::string());
  out.anonymous = row.anonymous;
  out.totalPointsText = row.total_points_text.value_or(std::string());
  out.blocksText = row.blocks_with_points_text.value_or(std::string());
  out.streakText = row.streak_text.value_or(std::string());
  out.longestStreakText = row.longest_streak_text.value_or(std::string());
  out.rankPointsText = row.rank_points_text.value_or(std::string());
  out.rankBlocksText = row.rank_blocks_text.value_or(std::string());
  out.rankStreakText = row.rank_streak_text.value_or(std::string());
  return out;
}

Gtk::ScrolledWindow* PaneScroller(Gtk::Box* content) {
  if (content == nullptr) return nullptr;
  return dynamic_cast<Gtk::ScrolledWindow*>(content->get_ancestor(GTK_TYPE_SCROLLED_WINDOW));
}

}  // namespace

void EarningsPage::BuildPointsBoard() {
  pointsHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  pointsHost_->set_vexpand(true);
  pointsHost_->set_visible(false);

  // the sort chips: the board re-sorts through the controller
  auto* sorts = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  sorts->add_css_class("linked");
  sorts->set_halign(Gtk::Align::START);
  sorts->set_margin_start(12);
  sorts->set_margin_end(12);
  sorts->set_margin_bottom(6);
  const char* const sortIds[3] = {urnet::PointsLeaderboardSortPoints,
                                  urnet::PointsLeaderboardSortBlocks,
                                  urnet::PointsLeaderboardSortStreak};
  const Glib::ustring sortLabels[3] = {T_("points", "Points"), T_("blocks", "Blocks"),
                                       T_("streak", "Streak")};
  for (int i = 0; i < 3; ++i) {
    auto* tab = Gtk::make_managed<Gtk::ToggleButton>(sortLabels[i]);
    if (i > 0) tab->set_group(*pointsSortTabs_[0]);
    pointsSortTabs_[i] = tab;
    const std::string sort = sortIds[i];
    tab->signal_toggled().connect([this, tab, sort] {
      if (tab->get_active()) OnPointsSortChanged(sort);
    });
    sorts->append(*tab);
  }
  pointsSortTabs_[0]->set_active(true);
  pointsHost_->append(*sorts);

  pointsRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  pointsHost_->append(*pointsRows_);

  // the footer: the page spinner, or the error with its retry
  pointsFooter_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  pointsFooter_->set_margin(12);
  pointsFooter_->set_halign(Gtk::Align::CENTER);
  pointsFooterSpinner_ = Gtk::make_managed<Gtk::Spinner>();
  pointsFooterSpinner_->set_visible(false);
  pointsFooter_->append(*pointsFooterSpinner_);
  pointsFooterLabel_ = Gtk::make_managed<Gtk::Label>();
  pointsFooterLabel_->add_css_class("dim-label");
  pointsFooterLabel_->set_wrap(true);
  pointsFooterLabel_->set_justify(Gtk::Justification::CENTER);
  pointsFooterLabel_->set_visible(false);
  pointsFooter_->append(*pointsFooterLabel_);
  pointsRetryButton_ = Gtk::make_managed<Gtk::Button>(T_("try_again", "Try again"));
  pointsRetryButton_->set_halign(Gtk::Align::CENTER);
  pointsRetryButton_->set_visible(false);
  pointsRetryButton_->signal_clicked().connect([this] { OnPointsRetry(); });
  pointsFooter_->append(*pointsRetryButton_);
  pointsHost_->append(*pointsFooter_);

  pointsBoardStatus_ = kit::MakePaneEmptyLine(T_("loading", "Loading..."));
  pointsHost_->append(*pointsBoardStatus_);
  leaderboardHost_->append(*pointsHost_);

  // the next page is asked for as the pane scrolls near the end of the rows
  if (Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content)) {
    pointsScrollConn_ =
        scroller->get_vadjustment()->signal_value_changed().connect([this] { OnPointsScrolled(); });
    // the range changes as pages land and the pane resizes: the thumb re-fits
    scroller->get_vadjustment()->signal_changed().connect([this] { RefreshPointsPosition(); });
  }
}

// Pane C's block for the Points board: the group strip with the ranked count,
// the identity line (emoji tag, own name, the pencil), the three dimensions
// each with its rank chip, the longest streak, the opt-in switch with its
// hint, and what the board measures. The Android header card, on the pane's
// row rhythm.
void EarningsPage::BuildPointsNetworkBlock() {
  pointsGroup_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  pointsGroup_->set_visible(false);

  auto group = kit::MakePaneGroupHeader(T_("points", "Points"));
  pointsGroupMeta_ = group.meta;
  pointsGroup_->append(*group.root);

  // identity
  {
    auto row = MakePaddedRow(10);
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    pointsEmojiLabel_ = Gtk::make_managed<Gtk::Label>();
    pointsEmojiLabel_->set_valign(Gtk::Align::CENTER);
    pointsEmojiLabel_->set_visible(false);
    {
      Pango::AttrList attrs;
      auto size = Pango::Attribute::create_attr_size_absolute(26 * PANGO_SCALE);
      attrs.insert(size);
      pointsEmojiLabel_->set_attributes(attrs);
    }
    pointsEmojiLabel_->set_xalign(0);
    pointsNameLabel_ = Gtk::make_managed<Gtk::Label>();
    pointsNameLabel_->add_css_class("ur-row-title");
    pointsNameLabel_->set_xalign(0);
    pointsNameLabel_->set_hexpand(true);
    pointsNameLabel_->set_valign(Gtk::Align::CENTER);
    pointsNameLabel_->set_ellipsize(Pango::EllipsizeMode::END);
    line->append(*pointsNameLabel_);
    editEmojiButton_ = Gtk::make_managed<Gtk::Button>();
    editEmojiButton_->set_icon_name("document-edit-symbolic");
    editEmojiButton_->add_css_class("flat");
    editEmojiButton_->set_valign(Gtk::Align::CENTER);
    editEmojiButton_->signal_clicked().connect([this] { OnEditEmoji(); });
    line->append(*editEmojiButton_);
    // the name line with the pencil, the emoji tag on its own line beneath it,
    // then the ranked count, all left-aligned
    auto* identity = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    identity->append(*line);
    identity->append(*pointsEmojiLabel_);
    pointsRankedLabel_ = Gtk::make_managed<Gtk::Label>();
    pointsRankedLabel_->add_css_class("dim-label");
    pointsRankedLabel_->set_xalign(0);
    pointsRankedLabel_->set_visible(false);
    identity->append(*pointsRankedLabel_);
    row.content->append(*identity);
    pointsGroup_->append(*row.root);
  }

  // the three dimensions, each with its own rank
  {
    auto row = MakePaddedRow(10);
    auto* tiles = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    const Glib::ustring labels[3] = {T_("points", "Points"), T_("blocks", "Blocks"),
                                     T_("streak", "Streak")};
    for (int i = 0; i < 3; ++i) {
      auto* tile = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
      tile->set_hexpand(true);
      auto* caption = Gtk::make_managed<Gtk::Label>(labels[i]);
      caption->add_css_class("ur-caption");
      caption->set_xalign(0);
      caption->set_ellipsize(Pango::EllipsizeMode::END);
      tile->append(*caption);
      pointsTiles_[i].value = MakeCondensedValue("-", 22);
      pointsTiles_[i].value->add_css_class("ur-label-faint");
      tile->append(*pointsTiles_[i].value);
      pointsTiles_[i].rank = MakeStatusChip("-");
      pointsTiles_[i].rank->set_halign(Gtk::Align::START);
      tile->append(*pointsTiles_[i].rank);
      tiles->append(*tile);
    }
    row.content->append(*tiles);
    pointsLongestLabel_ = MakeWrappedNote({}, "ur-row-note");
    pointsLongestLabel_->set_margin_top(6);
    pointsLongestLabel_->set_visible(false);
    row.content->append(*pointsLongestLabel_);
    pointsGroup_->append(*row.root);
  }

  // the opt-in switch
  {
    auto row = kit::MakePaneTwoLineRow(
        T_("show_on_points_leaderboard", "Show my network name"), {}, 44);
    pointsPublicToggle_ = Gtk::make_managed<Gtk::Switch>();
    pointsPublicToggle_->set_valign(Gtk::Align::CENTER);
    kit::SetAccessibleLabel(*pointsPublicToggle_,
                            T_("show_on_points_leaderboard", "Show my network name"));
    pointsPublicToggle_->property_active().signal_changed().connect(
        sigc::mem_fun(*this, &EarningsPage::OnPointsPublicToggled));
    row.trailing->append(*pointsPublicToggle_);
    pointsGroup_->append(*row.root);
  }
  {
    auto row = MakePaddedRow(8);
    row.content->append(*MakeWrappedNote(
        T_("points_leaderboard_private_hint",
           "Your network appears as Anonymous until you turn this on."),
        "ur-row-note"));
    pointsPrivateHintRow_ = row.root;
    pointsGroup_->append(*row.root);
  }

  // what the board measures
  {
    auto row = MakePaddedRow(8);
    row.content->append(*MakeWrappedNote(
        T_("points_leaderboard_description",
           "All-time points. A block is one finalized epoch; the streak counts consecutive "
           "blocks with points, ending at the latest one."),
        "ur-row-note"));
    pointsGroup_->append(*row.root);
  }

  paneC_.content->append(*pointsGroup_);
}

void EarningsPage::OnBoardTabChanged() {
  const bool points = pointsBoardTab_ != nullptr && pointsBoardTab_->get_active();
  pointsBoardShowing_ = points;
  if (leaderboardDataHost_ != nullptr) leaderboardDataHost_->set_visible(!points);
  if (pointsHost_ != nullptr) pointsHost_->set_visible(points);
  for (Gtk::Widget* widget : dataRankingWidgets_) widget->set_visible(!points);
  if (pointsGroup_ != nullptr) pointsGroup_->set_visible(points);
  ApplyLedgerMeta();
  if (points) EnsurePointsBoard();
  UpdatePointsIndicator();
}

void EarningsPage::EnsurePointsBoard() {
  if (previewMode_) {
    SettlePointsBoardPreview();
    return;
  }
  // While connect commands are outstanding the device can be closed on the
  // worker, and IsLoggedIn waits on the lock they hold: the board stays as it
  // is, and the queue's settle replays the device's lifecycle here.
  if (host_.CommandsBusy()) return;
  // the controller lives on the device: no session or no device, no board
  if (!host_.IsLoggedIn() || !host_.hasDevice()) {
    ClosePointsBoard(/*deviceAlive=*/false);
    kit::SetTextOrCollapse(*pointsBoardStatus_,
                           T_("please_login_to_urnetwork", "Please login to URnetwork"));
    return;
  }
  const uint64_t device = host_.device().handle();
  if (pointsVc_ && pointsVcDevice_ == device) return;  // still the device it was opened on
  ClosePointsBoard(pointsVcDevice_ == device);
  pointsVcDevice_ = device;
  try {
    pointsVc_.emplace(host_.device().openPointsLeaderboardViewController());
  } catch (const std::exception& e) {
    g_warning("points board: could not open the controller: %s", e.what());
    pointsVc_.reset();
    pointsVcDevice_ = 0;
    kit::SetTextOrCollapse(*pointsBoardStatus_, T_("something_went_wrong", "Something went wrong."));
    return;
  }
  auto alive = alive_;
  // the SDK calls from its own thread; the state is read on the main loop
  pointsSub_.emplace(pointsVc_->addPointsLeaderboardListener([this, alive] {
    PostToMain([this, alive] {
      if (!*alive) return;
      ReadPointsBoard();
    });
  }));
  pointsVc_->start();
  // a sort picked before the controller existed is applied now
  if (pointsVc_->getSort() != pointsSort_) pointsVc_->setSort(pointsSort_);
  kit::SetTextOrCollapse(*pointsBoardStatus_, T_("loading", "Loading..."));
  ReadPointsBoard();
}

void EarningsPage::ClosePointsBoard(bool deviceAlive) {
  ReleasePointsBoard(deviceAlive);
  if (pointsRows_ != nullptr) RebuildPointsRows();
  RenderPointsHeader();
  RenderPointsFooter();
  UpdatePointsIndicator();
}

void EarningsPage::ReleasePointsBoard(bool deviceAlive) {
  pointsSub_.reset();  // unsubscribes
  if (pointsVc_) {
    // the controller must be closed on the device that opened it; a device
    // that is gone took its controllers with it
    if (deviceAlive && host_.DeviceSteady() && host_.device().handle() == pointsVcDevice_) {
      host_.device().closePointsLeaderboardViewController(*pointsVc_);
    }
    pointsVc_.reset();
  }
  pointsVcDevice_ = 0;
  pointsRowsUi_.clear();
  pointsHasLoaded_ = false;
  pointsLoading_ = false;
  pointsEnd_ = false;
  pointsError_.clear();
  pointsMe_.reset();
  pointsTotalRanked_ = 0;
  pointsFirstPosition_ = 1;
  pointsHasMoreBefore_ = false;
  pointsFirstVisible_ = 1;
  pointsSeekPending_ = false;
  pointsAnchorConn_.disconnect();
}

void EarningsPage::ReadPointsBoard() {
  if (!pointsVc_) return;
  std::vector<PointsRowUi> next;
  try {
    if (auto list = pointsVc_->getRows()) {
      next.reserve(list->size());
      for (const auto& row : *list) next.push_back(ToPointsRowUi(row));
    }
    pointsSort_ = pointsVc_->getSort();
    if (pointsSort_.empty()) pointsSort_ = urnet::PointsLeaderboardSortPoints;
    pointsLoading_ = pointsVc_->isLoading();
    pointsEnd_ = pointsVc_->isEndReached();
    pointsError_ = pointsVc_->getErrorMessage();
    pointsTotalRanked_ = pointsVc_->getTotalRanked();
    pointsFirstPosition_ = std::max<int64_t>(1, pointsVc_->firstLoadedPosition());
    pointsHasMoreBefore_ = pointsVc_->hasMoreBefore();
    if (auto me = pointsVc_->getMe()) {
      pointsMe_ = me->Row ? std::optional<PointsRowUi>(ToPointsRowUi(*me->Row)) : std::nullopt;
      if (ownFlagsAppliedAt_ >= ownFlagsEditedAt_) {
        pointsPublic_ = me->PointsLeaderboardPublic;
        emojiTag_ = pointsMe_ ? pointsMe_->emojiTag : std::string();
      }
    }
  } catch (const std::exception& e) {
    // a malformed document must never take the page down
    g_warning("points board: reading the controller failed: %s", e.what());
    return;
  }
  // the common case is the next page landing below the rows already drawn:
  // then only the new rows are appended; a sort change, a refresh, a restart
  // or a newly known own id redraws the list from the top
  const std::string ownIdNow = pointsMe_ ? pointsMe_->networkId : std::string();
  const bool sameContext = pointsRenderedSort_ == pointsSort_ && pointsRenderedOwnId_ == ownIdNow;
  const bool extends = sameContext && pointsRenderedCount_ == pointsRowsUi_.size() &&
                       next.size() > pointsRowsUi_.size() &&
                       std::equal(pointsRowsUi_.begin(), pointsRowsUi_.end(), next.begin());
  // the page before the window lands above the rows already drawn (the old
  // rows are the new list's tail): those are kept, the new ones inserted
  // above them, and the scroller moved by their height so the row in view
  // stays put
  const bool prepends = !extends && sameContext && pointsRenderedCount_ == pointsRowsUi_.size() &&
                        !pointsRowsUi_.empty() && next.size() > pointsRowsUi_.size() &&
                        std::equal(pointsRowsUi_.begin(), pointsRowsUi_.end(),
                                   next.end() - static_cast<std::ptrdiff_t>(pointsRowsUi_.size()));
  const size_t prepended = prepends ? next.size() - pointsRowsUi_.size() : 0;
  const bool rowsChanged = next != pointsRowsUi_ || !sameContext;
  const size_t renderFrom = extends ? pointsRowsUi_.size() : 0;
  if (next != pointsRowsUi_) pointsRowsUi_ = std::move(next);
  if (!pointsLoading_ && (!pointsRowsUi_.empty() || pointsEnd_ || !pointsError_.empty())) {
    pointsHasLoaded_ = true;
  }
  // a seek's window has landed once the controller is done loading: the
  // scroller goes to its first row (the rank released on)
  const bool seekLanded = pointsSeekPending_ && !pointsLoading_ && pointsError_.empty();

  // the sort chips follow the controller (a same-sort reselect is a no-op)
  const int sortIndex = pointsSort_ == urnet::PointsLeaderboardSortBlocks   ? 1
                        : pointsSort_ == urnet::PointsLeaderboardSortStreak ? 2
                                                                            : 0;
  if (pointsSortTabs_[sortIndex] != nullptr && !pointsSortTabs_[sortIndex]->get_active()) {
    pointsSortTabs_[sortIndex]->set_active(true);
  }

  if (prepends) {
    PrependPointsRows(prepended);
    AnchorPointsScroll(static_cast<double>(prepended) * kPointsRowHeight);
  } else if (rowsChanged) {
    RebuildPointsRows(renderFrom);
  }
  RenderPointsHeader();
  RenderPointsFooter();
  if (pointsBoardShowing_) ApplyLedgerMeta();
  if (seekLanded) {
    pointsSeekPending_ = false;
    ScrollPointsToFirstRow();
  }
  // the positions in view follow the new window once it is laid out
  {
    auto alive = alive_;
    Glib::signal_idle().connect_once([this, alive] {
      if (*alive) RefreshPointsPosition();
    });
  }

  // a page that does not fill the pane can never be scrolled to its end, so
  // the next one is asked for once layout has run (the controller refuses a
  // second in-flight page and a page past the end)
  if (!pointsLoading_ && !pointsEnd_ && pointsError_.empty() && !pointsRowsUi_.empty()) {
    auto alive = alive_;
    Glib::signal_idle().connect_once([this, alive] {
      if (!*alive || !pointsVc_ || pointsLoading_ || pointsEnd_ || !pointsError_.empty()) return;
      Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
      if (scroller == nullptr) return;
      auto adjustment = scroller->get_vadjustment();
      if (adjustment->get_upper() <= adjustment->get_page_size()) pointsVc_->loadMore();
    });
  }
}

// One continuous list: `fromIndex` > 0 appends the rows from that index below
// the ones already drawn (the next page); 0 redraws everything.
void EarningsPage::RebuildPointsRows(size_t fromIndex) {
  const std::string ownId = pointsMe_ ? pointsMe_->networkId : std::string();
  const std::vector<int> weights{1, 5, 2, 1, 1};
  if (fromIndex == 0 || fromIndex > pointsRowsUi_.size()) {
    fromIndex = 0;
    RemoveAllChildren(*pointsRows_);
    // the same table builder the data board uses; rank and network read as
    // text, the three figures read right
    if (!pointsRowsUi_.empty()) {
      pointsRows_->append(*kit::MakePaneTableHeader(
          weights,
          {T_("current_ranking", "Current Ranking"), T_("network", "Network"),
           T_("points", "Points"), T_("blocks", "Blocks"), T_("streak", "Streak")},
          2));
    }
  }
  pointsRenderedSort_ = pointsSort_;
  pointsRenderedOwnId_ = ownId;
  pointsRenderedCount_ = pointsRowsUi_.size();
  if (pointsRowsUi_.empty()) return;

  for (size_t i = fromIndex; i < pointsRowsUi_.size(); ++i) {
    pointsRows_->append(*MakePointsRow(pointsRowsUi_[i], ownId));
  }
}

// The page before the window: `count` rows now at the head of pointsRowsUi_
// go in above the rows already drawn, under the column-name strip.
void EarningsPage::PrependPointsRows(size_t count) {
  Gtk::Widget* header = pointsRows_ != nullptr ? pointsRows_->get_first_child() : nullptr;
  if (header == nullptr || count > pointsRowsUi_.size()) {
    RebuildPointsRows();
    return;
  }
  const std::string ownId = pointsMe_ ? pointsMe_->networkId : std::string();
  // inserted last-first, each straight under the header, so they end up in order
  for (size_t i = count; i-- > 0;) {
    pointsRows_->insert_child_after(*MakePointsRow(pointsRowsUi_[i], ownId), *header);
  }
  pointsRenderedCount_ = pointsRowsUi_.size();
}

// One ranked row of the table, keyed by its position.
Gtk::Widget* EarningsPage::MakePointsRow(const PointsRowUi& r, const std::string& ownId) {
  const std::vector<int> weights{1, 5, 2, 1, 1};
  const bool byBlocks = pointsSort_ == urnet::PointsLeaderboardSortBlocks;
  const bool byStreak = pointsSort_ == urnet::PointsLeaderboardSortStreak;
  const size_t activeColumn = byBlocks ? 3 : (byStreak ? 4 : 2);
  const Glib::ustring anonymous = T_("anonymous", "Anonymous");
  // an anonymous row reads "Anonymous" to everyone, its owner included: the
  // own row is what the network looks like to others, only highlighted (the
  // highlight keys on the network id, never the name; the own card carries
  // the name)
  const bool isOwn = !ownId.empty() && r.networkId == ownId;
  auto row = kit::MakePaneTableRow(weights, kPointsRowHeight, 2);
  // the name sits on its own line above the tag, so a long tag never
  // squeezes the name to a stub on a narrow pane
  auto identity = kit::MakePaneTableStack(row, 1);
  row.cells[0]->set_text(byBlocks ? r.rankBlocksText
                                  : (byStreak ? r.rankStreakText : r.rankPointsText));
  // the emoji tag shows either way; the name only when the network is not anonymous
  const bool anon = r.anonymous || r.displayName.empty();
  Glib::ustring name = anon ? anonymous : Glib::ustring(SanitizeExternalDisplayText(r.displayName));
  row.cells[1]->set_text(name);
  identity.bottom->set_text(r.emojiTag);
  identity.bottom->set_visible(!r.emojiTag.empty());
  row.cells[2]->set_text(r.totalPointsText);
  row.cells[3]->set_text(r.blocksText);
  row.cells[4]->set_text(r.streakText);
  // the sorted figure reads in the text voice; the other two step back
  for (size_t i = 2; i < row.cells.size(); ++i) {
    row.cells[i]->add_css_class(i == activeColumn ? "ur-value" : "dim-label");
  }
  if (anon) row.cells[1]->add_css_class("dim-label");
  // the account's own row is the point of the table: colour AND the pane's
  // fill step, because colour alone is never the only signal
  if (isOwn) {
    for (Gtk::Label* cell : row.cells) {
      cell->remove_css_class("dim-label");
      cell->add_css_class("ur-value-on");
    }
    row.root->add_css_class("ur-earn-own-row");
  }
  return row.root;
}

// The network's own name for the points board: the me row's, or the jwt's
// until me lands, filtered for display; empty only when signed out.
std::string EarningsPage::OwnPointsName() {
  if (pointsMe_ && !pointsMe_->displayName.empty()) {
    return SanitizeExternalDisplayText(pointsMe_->displayName);
  }
  if (auto jwt = host_.ParseByJwt(); jwt && !jwt->NetworkName.empty()) {
    return SanitizeExternalDisplayText(jwt->NetworkName);
  }
  return std::string();
}

void EarningsPage::RenderPointsHeader() {
  if (pointsNameLabel_ == nullptr) return;  // not built yet
  const bool hasMe = pointsMe_.has_value();

  pointsEmojiLabel_->set_text(emojiTag_);
  pointsEmojiLabel_->set_visible(!emojiTag_.empty());
  // the caller always sees their own name: the me row's, or the jwt's until
  // me lands
  pointsNameLabel_->set_text(OwnPointsName());
  const Glib::ustring editName =
      emojiTag_.empty() ? T_("add_emoji", "Add emoji") : T_("edit_emoji", "Edit emoji");
  editEmojiButton_->set_tooltip_text(editName);
  kit::SetAccessibleLabel(*editEmojiButton_, editName);
  kit::SetTextOrCollapse(*pointsGroupMeta_, Glib::ustring());
  kit::SetTextOrCollapse(
      *pointsRankedLabel_,
      pointsTotalRanked_ > 0
          ? Glib::ustring(Format(T_("ranked_networks_count", "{} ranked networks"),
                                 urnet::formatPoints(static_cast<double>(pointsTotalRanked_))))
          : Glib::ustring());

  const std::string values[3] = {hasMe ? pointsMe_->totalPointsText : std::string(),
                                 hasMe ? pointsMe_->blocksText : std::string(),
                                 hasMe ? pointsMe_->streakText : std::string()};
  const std::string ranks[3] = {hasMe ? pointsMe_->rankPointsText : std::string(),
                                hasMe ? pointsMe_->rankBlocksText : std::string(),
                                hasMe ? pointsMe_->rankStreakText : std::string()};
  const bool emphasized[3] = {pointsSort_ == urnet::PointsLeaderboardSortPoints,
                              pointsSort_ == urnet::PointsLeaderboardSortBlocks,
                              pointsSort_ == urnet::PointsLeaderboardSortStreak};
  for (int i = 0; i < 3; ++i) {
    pointsTiles_[i].value->set_text(values[i].empty() ? std::string("-") : values[i]);
    if (values[i].empty()) {
      pointsTiles_[i].value->add_css_class("ur-label-faint");
    } else {
      pointsTiles_[i].value->remove_css_class("ur-label-faint");
    }
    pointsTiles_[i].rank->set_text(ranks[i].empty() ? std::string("-") : ranks[i]);
    if (emphasized[i]) {
      pointsTiles_[i].rank->add_css_class("ur-value-on");
    } else {
      pointsTiles_[i].rank->remove_css_class("ur-value-on");
    }
  }
  if (hasMe) {
    pointsLongestLabel_->set_text(Glib::ustring(T_("longest_streak", "Longest streak")) + ": " +
                                  pointsMe_->longestStreakText);
  }
  pointsLongestLabel_->set_visible(hasMe);

  SetPointsToggle(pointsPublic_);
  pointsPublicToggle_->set_sensitive(!settingPointsPublic_);
  if (pointsPrivateHintRow_ != nullptr) pointsPrivateHintRow_->set_visible(!pointsPublic_);
}

void EarningsPage::RenderPointsFooter() {
  if (pointsFooter_ == nullptr) return;  // not built yet
  const bool showError = !pointsLoading_ && !pointsError_.empty();
  // the page spinner only once there are rows to page after; before the first
  // page the centred status line says "Loading..." on its own
  const bool paging = pointsLoading_ && !pointsRowsUi_.empty();
  pointsFooterSpinner_->set_visible(paging);
  if (paging) {
    pointsFooterSpinner_->start();
  } else {
    pointsFooterSpinner_->stop();
  }
  kit::SetTextOrCollapse(*pointsFooterLabel_, showError ? Glib::ustring(pointsError_) : Glib::ustring());
  pointsRetryButton_->set_visible(showError);
  if (!pointsVc_) return;  // the status line already says why there is no board
  if (pointsRowsUi_.empty() && !showError) {
    kit::SetTextOrCollapse(
        *pointsBoardStatus_,
        pointsHasLoaded_
            ? Glib::ustring(T_("points_leaderboard_empty", "No one is on the points leaderboard yet."))
            : Glib::ustring(T_("loading", "Loading...")));
  } else {
    kit::SetTextOrCollapse(*pointsBoardStatus_, {});
  }
}

// ---- the position indicator ---------------------------------------------------
// A slider over ranks 1..N floating at the pane's right edge while the points
// board is longer than the pane (mmm/DESIGNSTYLE.md "Long ranked lists: tab
// reset and a draggable position indicator"). The thumb sits at the first row
// in view over the total and is as long as the loaded window over the total;
// dragging it shows the rank and tier beside it and, on release, asks the
// controller for the window at that rank. The math is LeaderboardIndicator.hpp.

namespace {
constexpr double kIndicatorWidth = 32.0;  // the 24px thumb with 4px of air each side
constexpr double kIndicatorPad = 8.0;     // the track stops short of the pane's ends
constexpr double kIndicatorTrackWidth = 6.0;

void RoundedRectPath(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w,
                     double h, double r) {
  r = std::min(r, std::min(w, h) / 2.0);
  cr->begin_new_sub_path();
  cr->arc(x + w - r, y + r, r, -M_PI / 2, 0);
  cr->arc(x + w - r, y + h - r, r, 0, M_PI / 2);
  cr->arc(x + r, y + h - r, r, M_PI / 2, M_PI);
  cr->arc(x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cr->close_path();
}
}  // namespace

void EarningsPage::BuildPointsIndicator() {
  if (paneBOverlay_ == nullptr) return;
  // a slider to assistive tech; the role is construct-only, so the area comes
  // from the C constructor
  GtkWidget* raw = GTK_WIDGET(g_object_new(GTK_TYPE_DRAWING_AREA, "accessible-role",
                                           GTK_ACCESSIBLE_ROLE_SLIDER, nullptr));
  pointsIndicator_ = Gtk::manage(Glib::wrap(GTK_DRAWING_AREA(raw)));
  pointsIndicator_->set_content_width(static_cast<int>(kIndicatorWidth));
  pointsIndicator_->set_halign(Gtk::Align::END);
  pointsIndicator_->set_valign(Gtk::Align::FILL);
  pointsIndicator_->set_visible(false);
  pointsIndicator_->set_focusable(true);
  pointsIndicator_->set_cursor("grab");
  kit::SetAccessibleLabel(*pointsIndicator_,
                          T_("leaderboard_position_indicator", "Leaderboard position"));
  pointsIndicator_->set_draw_func(sigc::mem_fun(*this, &EarningsPage::DrawPointsIndicator));
  paneBOverlay_->add_overlay(*pointsIndicator_);
  paneBOverlay_->set_measure_overlay(*pointsIndicator_, false);

  auto drag = Gtk::GestureDrag::create();
  drag->set_button(GDK_BUTTON_PRIMARY);
  drag->signal_drag_begin().connect(
      [this, drag](double x, double y) { OnPointsDragBegin(x, y, drag); });
  drag->signal_drag_update().connect([this](double, double dy) { OnPointsDragUpdate(dy); });
  drag->signal_drag_end().connect([this](double, double dy) { OnPointsDragEnd(dy); });
  pointsIndicator_->add_controller(drag);

  // the keyboard moves the thumb and seeks: arrows by a window, page keys by
  // a tenth of the list, Home and End to the ends
  auto key = Gtk::EventControllerKey::create();
  key->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) -> bool { return OnPointsIndicatorKey(keyval); },
      false);
  pointsIndicator_->add_controller(key);

  // the floating label beside the thumb: the rank, the tier beneath
  pointsIndicatorLabel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  pointsIndicatorLabel_->add_css_class("ur-earn-scrub");
  pointsIndicatorLabel_->set_halign(Gtk::Align::END);
  pointsIndicatorLabel_->set_valign(Gtk::Align::START);
  pointsIndicatorLabel_->set_margin_end(static_cast<int>(kIndicatorWidth) + 8);
  pointsIndicatorLabel_->set_visible(false);
  pointsIndicatorLabel_->set_can_target(false);
  kit::MarkDecorative(*pointsIndicatorLabel_);  // the slider's value text says the same
  pointsIndicatorRank_ = Gtk::make_managed<Gtk::Label>();
  pointsIndicatorRank_->add_css_class("ur-value");
  pointsIndicatorRank_->set_halign(Gtk::Align::END);
  pointsIndicatorTier_ = Gtk::make_managed<Gtk::Label>();
  pointsIndicatorTier_->add_css_class("dim-label");
  pointsIndicatorTier_->add_css_class("caption");
  pointsIndicatorTier_->set_halign(Gtk::Align::END);
  pointsIndicatorLabel_->append(*pointsIndicatorRank_);
  pointsIndicatorLabel_->append(*pointsIndicatorTier_);
  paneBOverlay_->add_overlay(*pointsIndicatorLabel_);
  paneBOverlay_->set_measure_overlay(*pointsIndicatorLabel_, false);
}

urnw::leaderboard::Thumb EarningsPage::PointsThumb(double& trackTop, double& trackHeight) const {
  trackTop = 0;
  trackHeight = 0;
  Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
  if (scroller == nullptr || pointsIndicator_ == nullptr) return {};
  // the track spans the scroller, not the pane's header above it
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(GTK_WIDGET(scroller->gobj()), GTK_WIDGET(pointsIndicator_->gobj()),
                                 &bounds)) {
    return {};
  }
  trackTop = bounds.origin.y + kIndicatorPad;
  trackHeight = bounds.size.height - 2 * kIndicatorPad;
  auto adjustment = scroller->get_vadjustment();
  auto thumb = urnw::leaderboard::ThumbFor(pointsFirstVisible_,
                                           static_cast<int64_t>(pointsRowsUi_.size()),
                                           pointsTotalRanked_, trackHeight, adjustment->get_upper(),
                                           adjustment->get_page_size());
  if (thumb.visible && pointsDragging_) thumb.top = pointsDragTop_;
  return thumb;
}

void EarningsPage::DrawPointsIndicator(const Cairo::RefPtr<Cairo::Context>& cr, int width, int) {
  double trackTop = 0;
  double trackHeight = 0;
  const auto thumb = PointsThumb(trackTop, trackHeight);
  if (!thumb.visible) return;
  const double centre = width / 2.0;
  // the faint track
  RoundedRectPath(cr, centre - kIndicatorTrackWidth / 2, trackTop, kIndicatorTrackWidth, trackHeight,
                  kIndicatorTrackWidth / 2);
  cr->set_source_rgba(kUrTextMuted.r, kUrTextMuted.g, kUrTextMuted.b, 0.22);
  cr->fill();
  // the accent thumb, brighter in hand
  const double x = centre - urnw::leaderboard::kThumbWidth / 2;
  RoundedRectPath(cr, x, trackTop + thumb.top, urnw::leaderboard::kThumbWidth, thumb.height, 6.0);
  cr->set_source_rgba(kUrAccent.r, kUrAccent.g, kUrAccent.b, pointsDragging_ ? 1.0 : 0.85);
  cr->fill();
  // a grip: three short dark lines across the thumb's middle
  cr->set_source_rgba(0, 0, 0, 0.35);
  cr->set_line_width(1.5);
  const double gripY = trackTop + thumb.top + thumb.height / 2;
  for (int line = -1; line <= 1; ++line) {
    cr->move_to(centre - 5, gripY + line * 4);
    cr->line_to(centre + 5, gripY + line * 4);
  }
  cr->stroke();
}

// The first row in view, from the scroller; the thumb follows it (queue_draw
// coalesces to the frame clock).
void EarningsPage::RefreshPointsPosition() {
  Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
  if (scroller != nullptr && !pointsRowsUi_.empty()) {
    auto adjustment = scroller->get_vadjustment();
    pointsFirstVisible_ = urnw::leaderboard::FirstVisiblePosition(
        adjustment->get_value(), PointsRowsTop(), kPointsRowHeight, pointsFirstPosition_,
        static_cast<int64_t>(pointsRowsUi_.size()));
  } else {
    pointsFirstVisible_ = pointsFirstPosition_;
  }
  UpdatePointsIndicator();
}

void EarningsPage::UpdatePointsIndicator() {
  if (pointsIndicator_ == nullptr) return;
  const bool boardShowing = pointsBoardShowing_ && leaderboardTab_ != nullptr &&
                            leaderboardTab_->get_active() && !pointsRowsUi_.empty();
  double trackTop = 0;
  double trackHeight = 0;
  const auto thumb = boardShowing ? PointsThumb(trackTop, trackHeight) : urnw::leaderboard::Thumb{};
  const bool show = boardShowing && thumb.visible;
  if (show != pointsIndicator_->get_visible()) {
    pointsIndicator_->set_visible(show);
    // the indicator is the list's scrollbar while it shows, and the table
    // steps in from under its strip
    if (Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content)) {
      scroller->set_policy(Gtk::PolicyType::NEVER,
                           show ? Gtk::PolicyType::EXTERNAL : Gtk::PolicyType::AUTOMATIC);
    }
    if (pointsHost_ != nullptr) {
      pointsHost_->set_margin_end(show ? static_cast<int>(kIndicatorWidth) : 0);
    }
    if (!show) {
      pointsDragging_ = false;
      HidePointsDragLabel(/*fade=*/false);
    }
  }
  if (!show) return;
  const int64_t rank = pointsDragging_ ? pointsDragRank_ : pointsFirstVisible_;
  std::string valueText = "#" + urnw::leaderboard::GroupedRank(rank);
  if (auto parts = urnet::pointsLeaderboardScrollLabel(rank, pointsTotalRanked_)) {
    if (!parts->rank_text.empty()) valueText = parts->rank_text;
    const char* tierKey = urnw::leaderboard::TierLabelKey(parts->tier);
    if (std::string(tierKey) == "leaderboard_tier_top") {
      valueText += ", " + urnw::Format(T_("leaderboard_tier_top", "Top {}%"), parts->tier_percent);
    } else if (std::string(tierKey) == "leaderboard_tier_rest") {
      valueText += std::string(", ") + T_("leaderboard_tier_rest", "Everyone else");
    }
  }
  gtk_accessible_update_property(GTK_ACCESSIBLE(pointsIndicator_->gobj()),
                                 GTK_ACCESSIBLE_PROPERTY_VALUE_MIN, 1.0,
                                 GTK_ACCESSIBLE_PROPERTY_VALUE_MAX,
                                 static_cast<double>(pointsTotalRanked_),
                                 GTK_ACCESSIBLE_PROPERTY_VALUE_NOW, static_cast<double>(rank),
                                 GTK_ACCESSIBLE_PROPERTY_VALUE_TEXT, valueText.c_str(), -1);
  pointsIndicator_->queue_draw();
}

void EarningsPage::OnPointsDragBegin(double, double y, const Glib::RefPtr<Gtk::GestureDrag>& drag) {
  double trackTop = 0;
  double trackHeight = 0;
  const auto thumb = PointsThumb(trackTop, trackHeight);
  if (!thumb.visible || y < trackTop || y > trackTop + trackHeight) {
    drag->set_state(Gtk::EventSequenceState::DENIED);
    return;
  }
  drag->set_state(Gtk::EventSequenceState::CLAIMED);
  pointsIndicator_->grab_focus();
  pointsDragging_ = true;
  // a press on the thumb picks it up where it is; a press on the track
  // brings the thumb under the pointer
  const double pressed = y - trackTop;
  const bool onThumb = pressed >= thumb.top && pressed <= thumb.top + thumb.height;
  pointsDragStartTop_ = onThumb ? thumb.top
                                : std::clamp(pressed - thumb.height / 2, 0.0,
                                             std::max(0.0, trackHeight - thumb.height));
  pointsDragTop_ = pointsDragStartTop_;
  pointsIndicator_->set_cursor("grabbing");
  OnPointsDragUpdate(0);
}

void EarningsPage::OnPointsDragUpdate(double dy) {
  if (!pointsDragging_) return;
  double trackTop = 0;
  double trackHeight = 0;
  const auto thumb = PointsThumb(trackTop, trackHeight);
  if (!thumb.visible) return;
  pointsDragTop_ = std::clamp(pointsDragStartTop_ + dy, 0.0, std::max(0.0, trackHeight - thumb.height));
  pointsDragRank_ = urnw::leaderboard::RankForThumbTop(pointsDragTop_, thumb.height, trackHeight,
                                                       pointsTotalRanked_);
  ShowPointsDragLabel(pointsDragRank_);
  UpdatePointsIndicator();
}

void EarningsPage::OnPointsDragEnd(double dy) {
  if (!pointsDragging_) return;
  OnPointsDragUpdate(dy);
  pointsDragging_ = false;
  if (pointsIndicator_ != nullptr) pointsIndicator_->set_cursor("grab");
  HidePointsDragLabel(/*fade=*/true);
  SeekPoints(pointsDragRank_);
  UpdatePointsIndicator();
}

bool EarningsPage::OnPointsIndicatorKey(guint keyval) {
  using urnw::leaderboard::Step;
  std::optional<Step> step;
  switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up: step = Step::Up; break;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: step = Step::Down; break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: step = Step::PageUp; break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: step = Step::PageDown; break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: step = Step::Home; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: step = Step::End; break;
    default: break;
  }
  if (!step || pointsTotalRanked_ <= 0) return false;
  const int64_t rank = urnw::leaderboard::StepRank(
      pointsFirstVisible_, *step, static_cast<int64_t>(pointsRowsUi_.size()), pointsTotalRanked_);
  ShowPointsDragLabel(rank);
  HidePointsDragLabel(/*fade=*/true);
  SeekPoints(rank);
  return true;
}

// A rank inside the loaded window is a scroll; any other asks the controller
// for the window at that rank, and the scroller goes to its first row when it
// lands (ReadPointsBoard).
void EarningsPage::SeekPoints(int64_t rank) {
  if (pointsRowsUi_.empty() || pointsTotalRanked_ <= 0) return;
  rank = std::clamp<int64_t>(rank, 1, pointsTotalRanked_);
  const int64_t last = pointsFirstPosition_ + static_cast<int64_t>(pointsRowsUi_.size()) - 1;
  if (rank >= pointsFirstPosition_ && rank <= last) {
    if (Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content)) {
      scroller->get_vadjustment()->set_value(
          PointsRowsTop() + static_cast<double>(rank - pointsFirstPosition_) * kPointsRowHeight);
    }
    return;
  }
  if (!pointsVc_) return;
  pointsSeekPending_ = true;
  pointsVc_->seekToRank(rank);
}

void EarningsPage::ShowPointsDragLabel(int64_t rank) {
  if (pointsIndicatorLabel_ == nullptr) return;
  ++pointsLabelGen_;  // overtakes a fade in progress
  std::string rankText = "#" + urnw::leaderboard::GroupedRank(rank);
  Glib::ustring tier;
  if (auto parts = urnet::pointsLeaderboardScrollLabel(rank, pointsTotalRanked_)) {
    if (!parts->rank_text.empty()) rankText = parts->rank_text;
    const std::string tierKey = urnw::leaderboard::TierLabelKey(parts->tier);
    if (tierKey == "leaderboard_tier_top") {
      tier = urnw::Format(T_("leaderboard_tier_top", "Top {}%"), parts->tier_percent);
    } else if (tierKey == "leaderboard_tier_rest") {
      tier = T_("leaderboard_tier_rest", "Everyone else");
    }
  }
  pointsIndicatorRank_->set_text(rankText);
  kit::SetTextOrCollapse(*pointsIndicatorTier_, tier);
  pointsIndicatorLabel_->set_visible(true);  // a hidden widget measures as nothing
  // beside the thumb's middle
  double trackTop = 0;
  double trackHeight = 0;
  const auto thumb = PointsThumb(trackTop, trackHeight);
  int minimum = 0, natural = 0, minimumBaseline = 0, naturalBaseline = 0;
  pointsIndicatorLabel_->measure(Gtk::Orientation::VERTICAL, -1, minimum, natural, minimumBaseline,
                                 naturalBaseline);
  const double middle = trackTop + thumb.top + thumb.height / 2;
  pointsIndicatorLabel_->set_margin_top(
      static_cast<int>(std::max(0.0, middle - natural / 2.0)));
  pointsIndicatorLabel_->set_opacity(1.0);
  pointsIndicatorLabel_->set_visible(true);
}

// The label leaves after a beat: a short hold, then a fade.
void EarningsPage::HidePointsDragLabel(bool fade) {
  if (pointsIndicatorLabel_ == nullptr) return;
  if (!fade) {
    ++pointsLabelGen_;
    pointsIndicatorLabel_->set_visible(false);
    return;
  }
  const uint64_t gen = pointsLabelGen_;
  auto alive = alive_;
  Glib::signal_timeout().connect_once(
      [this, alive, gen] {
        if (!*alive || gen != pointsLabelGen_ || pointsIndicatorLabel_ == nullptr) return;
        const gint64 start = g_get_monotonic_time();
        pointsIndicatorLabel_->add_tick_callback(
            [this, alive, gen, start](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
              if (!*alive || gen != pointsLabelGen_) return false;
              const double t = static_cast<double>(clock->get_frame_time() - start) / 250000.0;
              if (t >= 1.0) {
                pointsIndicatorLabel_->set_visible(false);
                pointsIndicatorLabel_->set_opacity(1.0);
                return false;
              }
              pointsIndicatorLabel_->set_opacity(1.0 - t);
              return true;
            });
      },
      600);
}

// After a seek: the window's first row to the top of the pane, once the new
// rows are laid out.
void EarningsPage::ScrollPointsToFirstRow() {
  auto alive = alive_;
  Glib::signal_idle().connect_once([this, alive] {
    if (!*alive) return;
    Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
    if (scroller == nullptr || pointsRowsUi_.empty()) return;
    scroller->get_vadjustment()->set_value(PointsRowsTop());
    RefreshPointsPosition();
  });
}

// Rows went in above the view: the scroller moves by their height once the
// range has grown to hold them, so the row in view stays where it is.
void EarningsPage::AnchorPointsScroll(double prependedHeight) {
  Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
  if (scroller == nullptr || prependedHeight <= 0) return;
  auto adjustment = scroller->get_vadjustment();
  const double target = adjustment->get_value() + prependedHeight;
  pointsAnchorConn_.disconnect();
  pointsAnchorConn_ = adjustment->signal_changed().connect([this, adjustment, target] {
    pointsAnchorConn_.disconnect();
    adjustment->set_value(target);
  });
}

double EarningsPage::PointsRowsTop() const {
  if (pointsRows_ == nullptr || paneB_.content == nullptr) return 0.0;
  // the first row is the child after the column-name strip
  Gtk::Widget* header = pointsRows_->get_first_child();
  Gtk::Widget* first = header != nullptr ? header->get_next_sibling() : nullptr;
  Gtk::Widget* anchor = first != nullptr ? first : pointsRows_;
  graphene_rect_t bounds;
  if (!gtk_widget_compute_bounds(GTK_WIDGET(anchor->gobj()), GTK_WIDGET(paneB_.content->gobj()),
                                 &bounds)) {
    return 0.0;
  }
  double top = bounds.origin.y;
  if (first == nullptr && header != nullptr) top += header->get_height();
  return top;
}

// A board tab activated (the active one included): its list to the top; the
// points board also reloads from the top when its window moved away from it.
void EarningsPage::ResetBoardList(bool pointsBoard) {
  const auto reset =
      urnw::leaderboard::TabResetFor(pointsBoard, pointsVc_.has_value(), pointsFirstPosition_);
  if (reset.scrollToTop) {
    if (Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content)) {
      auto adjustment = scroller->get_vadjustment();
      adjustment->set_value(adjustment->get_lower());
    }
  }
  if (reset.reloadFromTop && pointsVc_) {
    pointsSeekPending_ = false;
    pointsVc_->reloadFromTop();
  }
  RefreshPointsPosition();
}

// URNETWORK_PREVIEW_SAMPLE: a long synthetic window so the indicator, its label
// and a deep jump can be looked at without an account. URNETWORK_PREVIEW_POINTS_FROM
// starts the window at that position (a jumped-to window, with rows above it).
void EarningsPage::ApplyPointsBoardSample() {
  int64_t from = 1;
  if (const char* env = g_getenv("URNETWORK_PREVIEW_POINTS_FROM")) {
    from = std::max<int64_t>(1, g_ascii_strtoll(env, nullptr, 10));
  }
  pointsTotalRanked_ = 12480;
  pointsFirstPosition_ = from;
  pointsHasMoreBefore_ = from > 1;
  pointsHasLoaded_ = true;
  pointsEnd_ = false;
  pointsRowsUi_.clear();
  const char* tags[] = {"🐸", "🦊🌵", "", "🍋", "🚀🌙", "🧊", "", "🐙🐙"};
  for (int64_t i = 0; i < 60; ++i) {
    PointsRowUi row;
    row.position = from + i;
    row.networkId = "sample-" + std::to_string(row.position);
    row.anonymous = (i % 5) == 3;
    row.displayName = row.anonymous ? std::string() : "sample-net-" + std::to_string(row.position);
    row.emojiTag = tags[i % 8];
    const int64_t points = std::max<int64_t>(1, 90000 - row.position * 7);
    row.totalPointsText = urnw::leaderboard::GroupedRank(points);
    row.blocksText = std::to_string(std::max<int64_t>(1, 40 - row.position / 400));
    row.streakText = std::to_string(std::max<int64_t>(0, 12 - (row.position % 13)));
    row.longestStreakText = row.streakText;
    row.rankPointsText = "#" + urnw::leaderboard::GroupedRank(row.position);
    row.rankBlocksText = row.rankPointsText;
    row.rankStreakText = row.rankPointsText;
    pointsRowsUi_.push_back(std::move(row));
  }
  if (pointsRows_ != nullptr) RebuildPointsRows();
  RenderPointsHeader();
  RenderPointsFooter();
  if (pointsBoardStatus_ != nullptr) kit::SetTextOrCollapse(*pointsBoardStatus_, {});
  auto alive = alive_;
  Glib::signal_idle().connect_once([this, alive] {
    if (*alive) RefreshPointsPosition();
  });
  // URNETWORK_PREVIEW_POINTS_DRAG=<rank>: the thumb held mid-drag at that
  // rank with its label, once the pane has laid out
  if (const char* env = g_getenv("URNETWORK_PREVIEW_POINTS_DRAG")) {
    const int64_t rank = std::clamp<int64_t>(g_ascii_strtoll(env, nullptr, 10), 1, pointsTotalRanked_);
    Glib::signal_timeout().connect_once(
        [this, alive, rank] {
          if (!*alive) return;
          double trackTop = 0;
          double trackHeight = 0;
          const auto thumb = PointsThumb(trackTop, trackHeight);
          if (!thumb.visible) return;
          pointsDragging_ = true;
          pointsDragRank_ = rank;
          pointsDragTop_ = std::clamp(static_cast<double>(rank - 1) / pointsTotalRanked_ * trackHeight,
                                      0.0, trackHeight - thumb.height);
          pointsDragStartTop_ = pointsDragTop_;
          ShowPointsDragLabel(rank);
          UpdatePointsIndicator();
        },
        400);
  }
}

// Switches the sort; the controller clears its rows and reloads.
void EarningsPage::OnPointsSortChanged(const std::string& sort) {
  if (sort == pointsSort_ || !urnet::isPointsLeaderboardSort(sort)) return;
  // reflect the chip immediately; the controller confirms on its event
  pointsSort_ = sort;
  if (pointsVc_) pointsVc_->setSort(sort);
  if (pointsRows_ != nullptr) RebuildPointsRows();
  RenderPointsHeader();
}

// Asks for the next page when the last visible row is within reach of the end.
void EarningsPage::OnPointsScrolled() {
  if (!pointsVc_ || !pointsBoardShowing_ || leaderboardTab_ == nullptr ||
      !leaderboardTab_->get_active()) {
    return;
  }
  Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
  if (scroller == nullptr) return;
  auto adjustment = scroller->get_vadjustment();
  const double remainingBelow =
      adjustment->get_upper() - (adjustment->get_value() + adjustment->get_page_size());
  // the rows end at the footer, so the rows still below the fold are the
  // remaining height less the footer, in whole rows
  const double footer = pointsFooter_ != nullptr ? pointsFooter_->get_height() : 0.0;
  const int64_t rowCount = static_cast<int64_t>(pointsRowsUi_.size());
  const int64_t hiddenRows =
      static_cast<int64_t>(std::max(0.0, remainingBelow - footer) / kPointsRowHeight);
  const int64_t lastVisible = rowCount - 1 - hiddenRows;
  RefreshPointsPosition();
  // near the window's first row with rows above it (after a seek): the page
  // before; otherwise near its last row: the page after
  const int64_t firstVisibleRow = pointsFirstVisible_ - pointsFirstPosition_;
  if (urnw::leaderboard::ShouldLoadMoreBefore(firstVisibleRow, pointsLoading_, pointsHasMoreBefore_,
                                              !pointsError_.empty())) {
    pointsVc_->loadMoreBefore();
  } else if (emoji::ShouldLoadMore(lastVisible, rowCount, pointsLoading_, pointsEnd_,
                                   !pointsError_.empty())) {
    pointsVc_->loadMore();
  }
}

// Retries after an error: the controller re-requests the same page.
void EarningsPage::OnPointsRetry() {
  if (!pointsVc_) {
    EnsurePointsBoard();
    return;
  }
  if (pointsRowsUi_.empty()) {
    ownFlagsAppliedAt_ = ++ownFlagsClock_;  // the next `me` is newer than any local edit
    pointsVc_->refresh();
  } else {
    pointsVc_->loadMore();
  }
}

void EarningsPage::SetPointsToggle(bool on) {
  if (pointsPublicToggle_ == nullptr) return;
  // THE ECHO GUARD: the handler cannot tell a user flip from the programmatic
  // render of the server's answer, so every programmatic write goes here
  applyingPointsToggle_ = true;
  pointsPublicToggle_->set_active(on);
  applyingPointsToggle_ = false;
}

void EarningsPage::OnPointsPublicToggled() {
  if (applyingPointsToggle_) return;
  const bool requested = pointsPublicToggle_->get_active();
  if (requested == pointsPublic_) return;
  if (settingPointsPublic_) {
    SetPointsToggle(pointsPublic_);  // one in flight: snap back
    return;
  }
  if (!CanCallApi()) {
    SetPointsToggle(pointsPublic_);
    RefuseNoSession();
    return;
  }
  settingPointsPublic_ = true;
  pointsPublicToggle_->set_sensitive(false);
  const uint32_t generation = BeginFlow(pointsPublicFlow_, kApiTimeoutMs, [this] {
    settingPointsPublic_ = false;
    pointsPublicToggle_->set_sensitive(true);
    SetPointsToggle(pointsPublic_);
    Notify(T_("something_went_wrong", "Something went wrong."),
           kit::Snackbar::Severity::Error);
  });

  urnet::SetPointsLeaderboardPublicArgs args;
  args.public_ = requested;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().setPointsLeaderboardPublic(
      args, [this, epoch, seen, generation, requested](
                std::optional<urnet::SetPointsLeaderboardPublicResult> result,
                std::optional<std::string> err) {
        std::string detail = err.value_or(std::string());
        if (detail.empty() && result && result->error) detail = result->error->message;
        const bool ok = !err.has_value() && result.has_value() && !result->error;
        PostToMain([this, epoch, seen, generation, ok, requested, detail] {
          if (*epoch != seen) return;
          ApplyPointsPublicResult(generation, ok, requested, detail);
        });
      });
}

void EarningsPage::ApplyPointsPublicResult(uint32_t generation, bool ok, bool requested,
                                           const std::string& serverError) {
  if (!SettleFlow(pointsPublicFlow_, generation, "points leaderboard visibility set")) return;
  settingPointsPublic_ = false;
  pointsPublicToggle_->set_sensitive(true);
  if (!ok) {
    SetPointsToggle(pointsPublic_);
    Notify(serverError.empty()
               ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
               : Glib::ustring(serverError),
           kit::Snackbar::Severity::Error);
    return;
  }
  // the local value wins until a `me` newer than this edit lands
  ownFlagsEditedAt_ = ++ownFlagsClock_;
  pointsPublic_ = requested;
  RenderPointsHeader();
  // the list shows or hides the own row; `me` is re-read too
  if (pointsVc_) {
    ownFlagsAppliedAt_ = ++ownFlagsClock_;
    pointsVc_->refresh();
  }
}

void EarningsPage::OnEditEmoji() {
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: emoji sheet suppressed — a modal is already open");
    return;
  }
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr) {
    g_warning("earnings: no window root; the emoji sheet was not opened");
    return;
  }
  auto alive = alive_;
  auto sheet = std::make_shared<EmojiTagSheet>(
      *root, [this, alive](std::string tag, std::function<void(std::string)> done) {
        if (!*alive) return;
        SaveEmojiTag(std::move(tag), std::move(done));
      });
  sheet->Open(emojiTag_);
  PresentSheet(sheet);
}

// Stores the tag (already normalized by the SDK), or an empty string to clear
// it; `done` gets the server's message on failure, on the main loop.
void EarningsPage::SaveEmojiTag(std::string tag, std::function<void(std::string)> done) {
  if (savingEmojiTag_) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    if (done) done(T_("please_login_to_urnetwork", "Please login to URnetwork"));
    return;
  }
  savingEmojiTag_ = true;
  urnet::SetEmojiTagArgs args;
  args.emoji_tag = tag;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  auto alive = alive_;
  host_.api().setEmojiTag(
      args, [this, epoch, seen, alive, done](std::optional<urnet::SetEmojiTagResult> result,
                                             std::optional<std::string> err) {
        std::string error = err.value_or(std::string());
        if (error.empty() && result && result->error) error = result->error->message;
        if (error.empty() && !result) error = "set emoji tag: no result";
        const std::string stored =
            error.empty() && result && result->emoji_tag ? *result->emoji_tag : std::string();
        if (!error.empty()) g_warning("points board: setEmojiTag failed: %s", error.c_str());
        PostToMain([this, epoch, seen, alive, done, error, stored] {
          if (!*alive) return;
          savingEmojiTag_ = false;
          if (*epoch != seen) return;  // the session moved on; the sheet is gone with it
          if (error.empty()) {
            ownFlagsEditedAt_ = ++ownFlagsClock_;
            emojiTag_ = stored;
            RenderPointsHeader();
            if (pointsVc_) {
              ownFlagsAppliedAt_ = ++ownFlagsClock_;
              pointsVc_->refresh();
            }
          }
          if (done) done(error);
        });
      });
}

// --preview-ui: the board on its real empty state, with no controller
void EarningsPage::SettlePointsBoardPreview() {
  ClosePointsBoard(/*deviceAlive=*/false);
  if (samplePinned_) {
    ApplyPointsBoardSample();
    return;
  }
  pointsHasLoaded_ = true;
  if (pointsBoardStatus_ != nullptr) {
    kit::SetTextOrCollapse(
        *pointsBoardStatus_,
        T_("points_leaderboard_empty", "No one is on the points leaderboard yet."));
  }
}

void EarningsPage::ApplyProvideState(const LiveStats& stats) {
  const auto visual = ProvideModeGlyphFor(stats.provideMode, stats.providePaused);
  provideModeDot_.set_markup(std::string("<span foreground='") + visual.colorHex + "'>" +
                             visual.glyph + "</span>");
  controlMode_ = host_.GetProvideControlMode();
  if (provideModeValue_) provideModeValue_->set_text(ProvideModeValueText(controlMode_));
  // the line under the row follows the live provide state on every push, so
  // it is applied before the gate's early return below (P008)
  liveProvideMode_ = stats.provideMode;
  providePaused_ = stats.providePaused;
  ApplyStatusLine();
  // the gate reads the same value the row shows: the provide mode the user
  // picked. Never hides every provider plot behind the disabled message,
  // whatever the device's live provide state says.
  const bool enabled = controlMode_ != "never";
  if (enabled == providingEnabled_) return;
  providingEnabled_ = enabled;
  ApplyReliability(lastReliability_, lastReliabilityState_);  // repaint under the new gate
  ApplyStatsSections();  // the provider statistics share the gate (O8)
}

// ---- the extender and provider statistics (EXTENDER.md N7, O5, O8) ------------

void EarningsPage::OnHostEvent(DrawerEvent event) {
  switch (event) {
    case DrawerEvent::DeviceLifecycle:
      // a device arriving or leaving, or the window coming back: the status
      // listener fires nothing on registration and the series controller is
      // new, so both are re-read, and so is the provider status controller
      // that opened (or closed) with the presentation
      ApplyExtenderProvideState();
      PullProviderThroughput(/*forced=*/true);
      ApplyProviderStatus();
      break;
    case DrawerEvent::Throughput:
      PullProviderThroughput(/*forced=*/false);
      break;
    case DrawerEvent::ExtenderProvideStatus:
      ApplyExtenderProvideState();
      break;
    case DrawerEvent::ProviderStatus:
      ApplyProviderStatus();
      break;
    case DrawerEvent::ProviderTransportSettings:
      // the enabled flags behind the provider bar's unused footer follow the
      // policy; re-read the distribution so the footer does not wait for a tick
      if (providerTransportBar_) {
        providerTransportBar_->SetDistribution(host_.ProviderTransportDistribution());
      }
      break;
    case DrawerEvent::BlockActions:
    case DrawerEvent::BlockStats:
    case DrawerEvent::Overrides:
    case DrawerEvent::DnsSettings:
    case DrawerEvent::TransportSettings:
    case DrawerEvent::Blocker:
    case DrawerEvent::RouteLocal:
    case DrawerEvent::Contracts:
    case DrawerEvent::Location:
    case DrawerEvent::Profile:
    case DrawerEvent::Locations:
    case DrawerEvent::Peers:
    case DrawerEvent::ProviderIdentities:
    case DrawerEvent::ProviderLocations:
    case DrawerEvent::ProviderSelection:
    case DrawerEvent::ExtenderStatus:
      // the connect page's and the window's surfaces
      break;
  }
}

void EarningsPage::SetPresentationActive(bool active) {
  if (!active) {
    // a sheet may not outlive the surface that feeds it
    // (ConnectPage::SetPresentationActive): the window only hides to the tray
    if (providerTransportSheet_) providerTransportSheet_->hide();
    return;
  }
  // every drawer event was dropped while hidden, and with no device the host
  // announces nothing on re-show. With connect commands outstanding (the
  // re-show queues the controllers' open behind them) the reads would wait on
  // the lock they hold, and the window replays this once the queue settles
  // (MainWindow::OnCommandsSettled).
  if (host_.CommandsBusy()) return;
  OnHostEvent(DrawerEvent::DeviceLifecycle);
}

// One pull per throughput tick, the shape of ConnectPage::PullThroughput: the
// provider series feeds the Local and Blocked charts, the extender series the
// extender chart, the distribution the bar, and the provider stats flag the
// gate. The charts redraw on their own timers.
void EarningsPage::PullProviderThroughput(bool forced) {
  const double window = static_cast<double>(host_.ThroughputWindowSeconds());
  auto providerPoints = std::make_shared<const urnet::ThroughputPointList>(
      host_.ProviderThroughputPoints().value_or(urnet::ThroughputPointList()));
  auto extenderPoints = std::make_shared<const urnet::ThroughputPointList>(
      host_.ExtenderThroughputPoints().value_or(urnet::ThroughputPointList()));
  if (localChart_) localChart_->SetPoints(providerPoints, window);
  if (blockedChart_) blockedChart_->SetPoints(providerPoints, window);
  if (extenderChart_) extenderChart_->SetPoints(extenderPoints, window);
  const std::optional<urnet::TransportDistribution> distribution =
      host_.ProviderTransportDistribution();
  providerDistributionKnown_ = distribution.has_value();
  if (providerTransportBar_) providerTransportBar_->SetDistribution(distribution);
  // the provider bytes of the window, which tell the idle line that a public
  // provider has carried no traffic yet (P008); none read, nothing is said
  const std::optional<int64_t> recentBytes =
      distribution ? std::optional<int64_t>(distribution->ByteCount) : std::nullopt;
  if (recentBytes != recentProviderBytes_) {
    recentProviderBytes_ = recentBytes;
    ApplyStatusLine();
  }
  // A forced re-read follows a device arriving or the window coming back; either
  // way SdkHost has just opened a new contract view controller, whose provider
  // stats stay nil until its first sample and whose first throughput tick lands
  // after its second. The device answers now; the tick reads the controller,
  // which has sampled by then.
  const bool hasStats = forced ? host_.DeviceHasProviderStats() : host_.HasProviderStats();
  if (hasStats != hasProviderStats_) {
    hasProviderStats_ = hasStats;
    ApplyStatsSections();
  }
}

// The read-only row's reading (N7) and the running state of the role (O4),
// both from the pushed status of the device that provides: the session's, or
// while disconnected the daemon's provider-only device's. This row draws no
// switch, so the setting is not read here.
void EarningsPage::ApplyExtenderProvideState() {
  const std::optional<urnet::ExtenderProvideStatus> status =
      host_.ProviderExtenderProvideStatus();
  DrawExtenderRow(extender::ProvideRowOf(status, [] { return false; }));
  const bool running = status && status->Enabled;
  if (running != extenderRunning_) {
    extenderRunning_ = running;
    ApplyStatsSections();
  }
}

void EarningsPage::DrawExtenderRow(const extender::ProvideRow& row) {
  if (!extenderRow_) return;
  // a push that changes nothing is dropped
  if (extenderRowApplied_ && row == extenderRowDrawn_) return;
  extenderRowApplied_ = true;
  extenderRowDrawn_ = row;
  extenderRow_->set_visible(row.visible);
  if (!row.visible) return;
  const std::string text = PaintExtenderProvideRow(*extenderDot_, *extenderState_, row);
  // one element for a screen reader, named the way ExtenderPanel names itself
  kit::SetAccessibleLabel(*extenderRow_, std::string(T_("extender", "Extender")) + ": " + text);
}

// O8 over its three inputs: the provide control mode's gate, the provider
// packet stats of the throughput tick, and the role's running state from the
// pushed status. Each input re-applies it when it flips.
void EarningsPage::ApplyStatsSections() {
  if (!extenderStatsHeader_.root || !providerStatsHeader_.meta) return;
  const extender::StatsSections sections =
      extender::StatsSectionsFor(providingEnabled_, hasProviderStats_, extenderRunning_);
  if (statsSectionsApplied_ && sections == statsSections_) return;
  const bool providerRowsAppear =
      sections.providerVisible && !(statsSectionsApplied_ && statsSections_.providerVisible);
  statsSectionsApplied_ = true;
  statsSections_ = sections;
  extenderStatsHeader_.root->set_visible(sections.extenderVisible);
  extenderChartRow_->set_visible(sections.extenderVisible);
  kit::SetTextOrCollapse(*providerStatsHeader_.meta,
                         sections.disabledMeta
                             ? Glib::ustring(T_("providing_disabled", "Providing is disabled"))
                             : Glib::ustring());
  localChartRow_->set_visible(sections.providerVisible);
  providerTransportRow_->set_visible(sections.providerVisible);
  blockedChartRow_->set_visible(sections.providerVisible);
  // the demand histogram and its "Why?" sit with the plots, under the same
  // gate (P008, the owner's placement rule)
  demandChartRow_->set_visible(sections.providerVisible);
  ApplyWhyVisibility();
  // the bar's legend line is a skeleton while its row shows before the first
  // distribution, and settles when the row hides
  if (!sections.providerVisible) {
    providerTransportBar_->SettleEmpty();
  } else if (providerRowsAppear && !providerDistributionKnown_) {
    providerTransportBar_->BeginLoading();
  }
}

void EarningsPage::OpenProviderTransportSheet() {
  auto* parent = dynamic_cast<Gtk::Window*>(get_root());
  if (!parent) return;
  if (!providerTransportSheet_) {
    providerTransportSheet_ =
        std::make_unique<TransportSheet>(*parent, host_, TransportSheet::Kind::Provider);
  }
  // always presentable: with no device the draft comes from the GUI's mirror
  // or the SDK default, and the edit applies at the next tunnel start
  providerTransportSheet_->Open();
}

// ---- the provider status (support part P008) ------------------------------------

// The local reason comes from the live stats and the throughput window and is
// immediate; the server's reason joins it once the controller has a status
// for this device. The desktop has no Wi-Fi-only setting, and Linux derives a
// reason only while a provider device runs: the tunnel session's, or the
// daemon's provider-only device while disconnected (SessionIdleReasonFor).
void EarningsPage::ApplyStatusLine() {
  if (statusLineRow_ == nullptr) return;
  const providerstatus::ProviderIdleReason idle =
      providerstatus::SessionIdleReasonFor(host_.ProviderRuns(), controlMode_, liveProvideMode_,
                                           providePaused_, recentProviderBytes_);
  const providerstatus::StatusLine line =
      providerstatus::StatusLineFor(idle, providerStatus_.reason, providerStatus_.reasonText);
  const bool shown = controlMode_ != "never" && line.kind != providerstatus::LineKind::Hidden;
  statusLineRow_->set_visible(shown);
  if (!shown) return;
  switch (line.kind) {
    case providerstatus::LineKind::Hidden:
      break;
    case providerstatus::LineKind::Idle:
      statusLineText_->set_text(IdleReasonText(line.idle));
      break;
    case providerstatus::LineKind::Server:
      statusLineText_->set_text(ServerReasonText(line.server));
      break;
    case providerstatus::LineKind::ServerText:
      // a code this app does not know yet: the server's English text
      statusLineText_->set_text(line.text);
      break;
  }
}

void EarningsPage::ApplyProviderStatus() {
  if (demandChart_ == nullptr) return;
  providerStatus_ = ProviderStatusPollOf(host_.ProviderStatusNow());
  providerStatusView_ = providerstatus::StatusViewFor(providerStatus_);
  switch (providerStatusView_.chart) {
    case providerstatus::ChartState::Loading:
      demandChart_->ShowMessage(T_("loading", "Loading..."));
      break;
    case providerstatus::ChartState::Unavailable:
      demandChart_->ShowMessage(
          T_("provider_status_unavailable", "Provider status isn't available right now."));
      break;
    case providerstatus::ChartState::Empty:
    case providerstatus::ChartState::Bars: {
      providerstatus::Histogram histogram =
          providerstatus::HistogramFor(providerStatus_.appearancesPerMinute);
      const Glib::ustring total = Format(
          TN_("provider_status_histogram_total", "{} time in the last hour",
              "{} times in the last hour", static_cast<unsigned long>(histogram.total)),
          histogram.total);
      const Glib::ustring emptyLine =
          histogram.empty ? Glib::ustring(T_("provider_status_histogram_empty",
                                             "Not offered to clients in the last hour"))
                          : Glib::ustring();
      demandChart_->ShowBars(std::move(histogram.fractions), total, emptyLine);
      break;
    }
  }
  RebuildWhyRows();
  ApplyWhyVisibility();
  ApplyStatusLine();
}

void EarningsPage::RebuildWhyRows() {
  if (whyRows_ == nullptr) return;
  std::vector<providerstatus::NumberRow> rows;
  if (providerStatusView_.showWhy) {
    rows = providerstatus::NumberRowsFor(providerStatus_.ranking, providerStatus_.country,
                                         ProviderStatusNumberWords());
  }
  // a poll a minute that repeats the numbers redraws nothing
  if (whyRowsApplied_ && rows == whyRowsDrawn_) return;
  whyRowsApplied_ = true;
  whyRowsDrawn_ = rows;
  RemoveAllChildren(*whyRows_);
  for (const providerstatus::NumberRow& number : rows) {
    auto row = MakePaddedRow(8);
    row.content->set_spacing(2);
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    const Glib::ustring labelText = NumberLabelText(number.label);
    auto* label = Gtk::make_managed<Gtk::Label>(labelText);
    label->add_css_class("ur-key");
    label->set_xalign(0);
    label->set_hexpand(true);
    label->set_wrap(true);
    kit::MarkDecorative(*label);  // the value carries "Label, value"
    line->append(*label);
    auto* value = Gtk::make_managed<Gtk::Label>();
    value->add_css_class("ur-value");
    value->set_xalign(1.f);
    value->set_wrap(true);
    value->set_justify(Gtk::Justification::RIGHT);
    if (number.passes) {
      value->set_text(number.value);
    } else {
      // a number that holds this device back is tinted amber (a pango
      // attribute, so it outranks the value class's own colour)
      value->set_markup("<span foreground='" + HexForMarkup(kUrAmber) + "'>" +
                        Glib::Markup::escape_text(number.value) + "</span>");
    }
    kit::SetAccessibleLabel(*value, labelText + ", " + number.value);
    line->append(*value);
    row.content->append(*line);
    row.content->append(*MakeWrappedNote(NumberHelpText(number.label), "ur-row-note"));
    whyRows_->append(*row.root);
  }
}

void EarningsPage::ApplyWhyVisibility() {
  if (whyRow_ == nullptr || whyRows_ == nullptr) return;
  const bool shown = statsSectionsApplied_ && statsSections_.providerVisible &&
                     providerStatusView_.showWhy && !whyRowsDrawn_.empty();
  whyRow_->set_visible(shown);
  whyRows_->set_visible(shown && whyExpanded_);
}

void EarningsPage::OnWhyToggled() {
  whyExpanded_ = !whyExpanded_;
  whyChevron_->set_from_icon_name(whyExpanded_ ? "go-down-symbolic" : "go-next-symbolic");
  gtk_accessible_update_state(GTK_ACCESSIBLE(whyRow_->gobj()), GTK_ACCESSIBLE_STATE_EXPANDED,
                              whyExpanded_ ? TRUE : FALSE, -1);
  ApplyWhyVisibility();
}

}  // namespace urnw
