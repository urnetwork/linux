// SPDX-License-Identifier: MPL-2.0
#include "EarningsPage.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "EmojiKeyboard.hpp"
#include "EmojiTagSheet.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

// ---- the numbers the destination is built from ------------------------------
constexpr int kPaneAWidth = 360;
constexpr int kPaneCWidth = 380;
constexpr int kThreePaneDip = 1500;  // earnings | history | network
constexpr int kTwoPaneDip = 900;     // earnings | history
constexpr int kApiTimeoutMs = 20000;      // plain api calls
constexpr int kBridgeTimeoutMs = 180000;  // browser-bridge flows: minutes are legitimate
constexpr int kChainTimeoutMs = 180000;   // a claim waits for a receipt
constexpr int kValidateDebounceMs = 300;
constexpr int kSheetMinWidth = 480;
constexpr int kClaimSheetMaxHeight = 560;
constexpr int kReliabilityChartHeight = 110;
constexpr int kCopiedResetMs = 1800;
constexpr double kMultiplierHighlight = 2.0;   // a country multiplier goes lime at >= 2.0
constexpr double kMinGasTao = 0.001;           // below this the gas key cannot pay a claim
constexpr double kSuggestedGasTao = 0.005;     // the top-up the dialog suggests
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

// A status chip on a history row / claim row: "unclaimed", "Claimed", ...
Gtk::Label* MakeStatusChip(const Glib::ustring& text) {
  auto* chip = Gtk::make_managed<Gtk::Label>(text);
  chip->add_css_class("ur-earn-tag");
  chip->set_valign(Gtk::Align::CENTER);
  return chip;
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
// when one is earning — points only, it never touches the alpha.
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
    text->append(*MakeSizedLabel(
        T_("seeker_points_only", "The Seeker multiplier applies to points only."), 12,
        "ur-caption"));
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
  if (host.hasDevice()) settings = host.device().getSnChainSettings();
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
using ClaimsDone = std::function<void(std::optional<std::vector<SnClaimRow>> claims,
                                      int64_t totalClaimableRao, std::string err)>;
void FetchClaims(SdkHost& host, ClaimsDone done) {
  if (!host.hasDevice()) {
    done(std::nullopt, 0, kNoDevice);
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
        done(std::nullopt, 0, ErrorText(result ? result->error : std::nullopt, err, "no result"));
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
      done(std::move(rows), result->total_claimable_rao, std::string());
      });
    });
  });
}

using GasDone = std::function<void(std::optional<SnGasInfo> gas, std::string err)>;
void FetchGas(SdkHost& host, GasDone done) {
  if (!host.hasDevice()) {
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
  if (host.hasDevice() && !clientId.empty()) {
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
      if (detail.empty() && result && result->error) detail = result->error->message;
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
  if (!host.hasDevice()) {
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

}  // namespace sn

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
         "Claims open 48 hours after an epoch is finalized and stay open for the vault's "
         "expiry window."),
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
  RemoveAllChildren(*gasPanel_);
  auto* keyLine = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* key = MakeSizedLabel(T_("gas_key", "Gas key"), 12, "ur-caption");
  key->set_hexpand(true);
  keyLine->append(*key);
  Glib::ustring value = "-";
  if (gas_) {
    value = ShortHex(gas_->address);
    if (gas_->balanceKnown) {
      value += Glib::ustring(" · ") +
               Format(T_("tao_amount_linux", "{} TAO"), FormatTao(gas_->tao));
    }
  }
  auto* valueLabel = MakeSizedLabel(value, 13, "ur-value");
  valueLabel->set_wrap(false);
  if (gas_) valueLabel->set_tooltip_text(gas_->address);
  keyLine->append(*valueLabel);
  gasPanel_->append(*keyLine);

  if (!NeedsGas()) return;
  // the needs-gas state: the mirror address is what the user must fund
  auto* box = MakeGoldTile();
  box->append(*MakeGoldLabel(T_("add_tao_for_gas", "Add TAO for gas"), 14));
  const std::string mirror = gas_ ? gas_->mirrorSs58 : std::string();
  box->append(*MakeWrappedNote(
      Format(T_("send_tao_to_mirror", "Send about {0} TAO to {1} to cover gas."),
             FormatTao(kSuggestedGasTao), mirror.empty() ? std::string("-") : mirror),
      "ur-key"));
  if (!mirror.empty()) {
    auto* copy = Gtk::make_managed<Gtk::Button>(T_("copy", "Copy"));
    copy->add_css_class("flat");
    copy->set_halign(Gtk::Align::START);
    copy->signal_clicked().connect([this, copy, mirror] {
      get_clipboard()->set_text(mirror);
      copy->set_label(T_("copied", "Copied!"));
      copiedReset_.disconnect();
      copiedReset_ = Glib::signal_timeout().connect(
          [copy]() -> bool {
            copy->set_label(T_("copy", "Copy"));
            return false;
          },
          kCopiedResetMs);
    });
    box->append(*copy);
  }
  gasPanel_->append(*box);
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

// ---- EarningsPage ------------------------------------------------------------

EarningsPage::EarningsPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  EnsureBrandCss();
  EnsureDrawerCss();
  BuildEarningsPane();
  append(*paneA_.root);
  ruleB_ = kit::MakePaneVRule();
  append(*ruleB_);

  BuildLedgerPane();
  append(*paneB_.root);
  ruleC_ = kit::MakePaneVRule();
  append(*ruleC_);

  BuildNetworkPane();
  append(*paneC_.root);

  // every panel opens on its LOADING state and the stat values on the faint
  // dash: an unloaded blank destination would read "there is nothing"
  SetStatValue(netProvidedValue_, T_("net_provided", "Net Provided"), {}, false);
  SetStatValue(rankValue_, T_("current_ranking", "Current Ranking"), {}, false);
  RebuildWalletBlock();
  RebuildUnclaimedTile();
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
  pointsPublicFlow_.timer.disconnect();
  pointsScrollConn_.disconnect();
  ClosePointsBoard(/*deviceAlive=*/true);
  rankingFlow_.timer.disconnect();
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
  // the leaderboard is a one-shot per LOOK, never per PROCESS
  if (leaderboardTab_ != nullptr && leaderboardTab_->get_active()) {
    leaderboardRequested_ = true;
    LoadLeaderboard();
    if (pointsBoardShowing_) EnsurePointsBoard();
  } else {
    leaderboardRequested_ = false;
  }
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
  ApplyPoints(urnet::AccountPointsList{}, Fetch::Ready);
  ApplyEpochs(std::vector<AccountEpochRow>{}, Fetch::Ready);
  ApplySnWallet(std::nullopt, Fetch::Ready);
  ApplyClaims(std::vector<SnClaimRow>{}, 0, Fetch::Ready);
  ApplyGas(std::nullopt);
  ApplyHead(std::nullopt, Fetch::Ready);
  ApplyReliability(std::nullopt, Fetch::Ready);
  ApplyRanking(std::nullopt, false);
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
    // plain note; the whole sentence opens the protocol site, the outward arrow
    // after the last word (inline, so it wraps with the sentence) says the click
    // leaves the app
    {
      auto* note = Gtk::make_managed<Gtk::Label>();
      note->set_markup(
          Glib::Markup::escape_text(
              T_("wallet_not_retroactive",
                 "Connect a wallet to earn SN25α from the next epoch. Earlier epochs are not "
                 "settled retroactively.")) +
          "\xC2\xA0<span alpha=\"70%\">\xE2\x86\x97</span>");
      note->add_css_class("ur-row-note");
      note->set_xalign(0);
      note->set_wrap(true);
      note->set_ellipsize(Pango::EllipsizeMode::NONE);
      walletConnectNote_ = Gtk::make_managed<Gtk::Button>();
      walletConnectNote_->set_child(*note);
      walletConnectNote_->add_css_class("flat");
      walletConnectNote_->set_has_frame(false);
      walletConnectNote_->set_halign(Gtk::Align::FILL);
      walletConnectNote_->signal_clicked().connect([this] { OpenLink(kUrXyzUrl); });
      row.content->append(*walletConnectNote_);
    }
    connectBridgeButton_ =
        Gtk::make_managed<Gtk::Button>(T_("connect_bittensor_wallet", "Connect Bittensor wallet"));
    connectBridgeButton_->add_css_class("ur-pane-primary");
    connectBridgeButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConnectWithBridge));
    row.content->append(*connectBridgeButton_);
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

  // 1 + 2. own ranking
  {
    Gtk::Widget* header =
        kit::MakePaneGroupHeader(T_("current_ranking", "Current Ranking")).root;
    content->append(*header);
    dataRankingWidgets_.push_back(header);
  }
  {
    auto row = MakePaddedRow(12);
    auto* grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* key = Gtk::make_managed<Gtk::Label>(T_("net_provided", "Net Provided"));
    key->add_css_class("ur-key");
    key->set_xalign(0);
    key->set_hexpand(true);
    key->set_valign(Gtk::Align::END);
    kit::MarkDecorative(*key);
    grid->append(*key);
    auto* figures = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    netProvidedValue_ = MakeStrongValue(18);
    figures->append(*netProvidedValue_);
    rankValue_ = MakeStrongValue(22);
    figures->append(*rankValue_);
    grid->append(*figures);
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

// The subnet layer: the vault's view of this network's epochs and the gas
// key. Only with an attached wallet — without one there is nothing to read.
void EarningsPage::LoadWalletLayer() {
  if (!wallet_ || !CanCallApi()) return;
  ApplyClaims(std::nullopt, 0, Fetch::Loading);
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  sn::FetchClaims(host_, [this, epoch, seen](std::optional<std::vector<SnClaimRow>> claims,
                                              int64_t total, std::string err) {
    PostToMain([this, epoch, seen, claims = std::move(claims), total, err = std::move(err)] {
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
      ApplyClaims(std::move(claims), total, Fetch::Ready);
    });
  });
  sn::FetchGas(host_, [this, epoch, seen](std::optional<SnGasInfo> gas, std::string err) {
    PostToMain([this, epoch, seen, gas = std::move(gas), err = std::move(err)] {
      if (*epoch != seen) return;
      if (!gas) {
        g_message("earnings: sn gas key unavailable: %s", err.c_str());
        ApplyGas(std::nullopt);
        return;
      }
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
  }
  RebuildWalletBlock();
  RebuildUnclaimedTile();
  RebuildHistory();  // the alpha column follows the wallet
}

void EarningsPage::ApplyClaims(std::optional<std::vector<SnClaimRow>> claims,
                               int64_t totalClaimableRao, Fetch state,
                               const Glib::ustring& failure) {
  claimsState_ = state;
  claimsFailure_ = failure;
  if (state == Fetch::Ready) {
    claims_ = claims.value_or(std::vector<SnClaimRow>{});
    totalClaimableRao_ = totalClaimableRao;
  } else if (state == Fetch::Failed) {
    claims_.clear();
    totalClaimableRao_ = 0;
  }
  RebuildUnclaimedTile();
  RebuildHistory();
}

void EarningsPage::ApplyGas(std::optional<SnGasInfo> gas) {
  gas_ = std::move(gas);
  if (auto sheet = claimSheet_.lock()) sheet->SetGas(gas_);
}

void EarningsPage::ApplyHead(std::optional<SnHeadInfo> head, Fetch state) {
  headState_ = state;
  head_ = state == Fetch::Ready ? std::move(head) : std::nullopt;
  RebuildTop200();
}

void EarningsPage::ApplyReliability(std::optional<urnet::ReliabilityWindow> window,
                                    Fetch state) {
  reliabilityState_ = state;
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
    changeWalletButton_->set_sensitive(!connecting_);
  }
  const bool offerConnect = walletState_ != Fetch::Loading && (!connected || changingWallet_);
  walletConnectPanel_->set_visible(offerConnect);
  // a link label keeps its markup; only its visibility follows the wallet state
  walletConnectNote_->set_visible(!connected);
  connectBridgeButton_->set_sensitive(!connecting_);
  manualToggleButton_->set_sensitive(!connecting_);
  manualPanel_->set_visible(manualEntryOpen_);
  const std::string typed = TrimWhitespace(walletAddressBox_->get_text().raw());
  const bool checked = check_.has_value() && !typed.empty() && checkedAddress_ == typed &&
                       check_->validSyntax && !check_->banned;
  connectManualButton_->set_sensitive(checked && !connecting_);
  kit::SetTextOrCollapse(
      *connectingStatus_,
      connecting_ ? Glib::ustring(T_("opening_bittensor_wallet_in_browser",
                                     "Opening your Bittensor wallet in the browser…"))
                  : Glib::ustring());
}

void EarningsPage::RebuildTop200() {
  if (top200Panel_ == nullptr) return;
  RemoveAllChildren(*top200Panel_);
  const bool show = headState_ == Fetch::Ready && head_ && (head_->eligible || head_->bound);
  top200Card_->set_visible(show);
  if (!show) return;
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
             "Claims open 48 hours after an epoch is finalized and stay open for the vault's "
             "expiry window."),
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
                                  : Glib::ustring(earner.network_name));
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
}

// ---- attach a Bittensor wallet -----------------------------------------------

void EarningsPage::OnConnectWithBridge() {
  if (connecting_) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  StartWalletSignature(std::string());  // whichever wallet the bridge picks
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
  if (connecting_) return;
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
  StartWalletSignature(address);  // still signed: the bridge must sign with THIS wallet
}

void EarningsPage::StartWalletSignature(const std::string& expectedAddress) {
  connecting_ = true;
  RebuildWalletBlock();
  // 180s, not 20s: the bridge reports errors only when a deep link comes BACK,
  // and a closed browser tab produces nothing, ever
  const uint32_t generation = BeginFlow(connectFlow_, kBridgeTimeoutMs, [this] {
    FinishConnecting();
    Notify(T_("wallet_connect_failed", "Failed to connect the wallet."),
           kit::Snackbar::Severity::Error);
  });
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.SignBittensorConnect(
      expectedAddress,
      [this, epoch, seen, generation, expectedAddress](SdkHost::WalletSignature signature) {
        PostToMain([this, epoch, seen, generation, expectedAddress,
                    signature = std::move(signature)] {
          if (*epoch != seen) return;
          OnWalletSigned(generation, signature, expectedAddress);
        });
      });
}

void EarningsPage::OnWalletSigned(uint32_t generation, const SdkHost::WalletSignature& signature,
                                  const std::string& expectedAddress) {
  if (!SettleFlow(connectFlow_, generation, "wallet signature")) return;
  if (!signature.ok) {
    FinishConnecting();
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
  if (allowActions && !gas_) {
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
  const uint32_t generation = BeginFlow(claimFlow_, kChainTimeoutMs, [this] {
    claiming_ = false;
    if (auto open = claimSheet_.lock()) {
      open->ShowError(T_("chain_rpc_unreachable", "The chain RPC is unreachable. Try again."));
      open->OnDone();
    }
    RebuildUnclaimedTile();
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
      LoadWalletLayer();  // the chain's answer replaces the optimistic rows
    });
  };
  sn::Claim(host_, epochs, std::move(events));
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

void EarningsPage::CloseSheet() {
  if (!sheet_) return;
  sheet_.reset();
  claimSheet_.reset();
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
  ApplyEpochs(sampleEpochs, Fetch::Ready);
  ApplyReliability(window, Fetch::Ready);
  ApplyRanking(ranking, true);
  ApplyLeaderboard(sampleEarners, Fetch::Ready);
  ApplyHead(head, Fetch::Ready);
  leaderboardRequested_ = true;  // the sample IS the leaderboard answer

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
  ApplyClaims(sampleClaims, 2031000000LL + 1210000000LL, Fetch::Ready);
  SnGasInfo gas;
  gas.address = "0x9a1cSAMPLEsampleSAMPLEsampleSAMPLEsamplee07f";
  gas.mirrorSs58 = "5GhSAMPLEsampleSAMPLEsampleSAMPLEsampleSAMPL2q";
  gas.balanceKnown = true;
  // URNETWORK_PREVIEW_GAS=low: the needs-gas state (the mirror address + top-up)
  const char* gasPreview = g_getenv("URNETWORK_PREVIEW_GAS");
  gas.tao = (gasPreview && std::string(gasPreview) == "low") ? 0.0002 : 0.0021;
  ApplyGas(gas);
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

constexpr int kPointsRowHeight = 36;

EarningsPage::PointsRowUi ToPointsRowUi(const urnet::PointsLeaderboardRow& row) {
  EarningsPage::PointsRowUi out;
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
    line->append(*pointsEmojiLabel_);
    pointsNameLabel_ = Gtk::make_managed<Gtk::Label>("-");
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
    row.content->append(*line);
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
        T_("show_on_points_leaderboard", "Show on the points leaderboard"), {}, 44);
    pointsPublicToggle_ = Gtk::make_managed<Gtk::Switch>();
    pointsPublicToggle_->set_valign(Gtk::Align::CENTER);
    kit::SetAccessibleLabel(*pointsPublicToggle_,
                            T_("show_on_points_leaderboard", "Show on the points leaderboard"));
    pointsPublicToggle_->property_active().signal_changed().connect(
        sigc::mem_fun(*this, &EarningsPage::OnPointsPublicToggled));
    row.trailing->append(*pointsPublicToggle_);
    pointsGroup_->append(*row.root);
  }
  {
    auto row = MakePaddedRow(8);
    row.content->append(*MakeWrappedNote(
        T_("points_leaderboard_private_hint",
           "Only you can see this. Turn it on to appear on the leaderboard."),
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
}

void EarningsPage::EnsurePointsBoard() {
  if (previewMode_) {
    SettlePointsBoardPreview();
    return;
  }
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
  pointsSub_.reset();  // unsubscribes
  if (pointsVc_) {
    // the controller must be closed on the device that opened it; a device
    // that is gone took its controllers with it
    if (deviceAlive && host_.hasDevice() && host_.device().handle() == pointsVcDevice_) {
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
  if (pointsRows_ != nullptr) RebuildPointsRows();
  RenderPointsHeader();
  RenderPointsFooter();
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
  const bool rowsChanged = next != pointsRowsUi_ || pointsRenderedSort_ != pointsSort_;
  if (next != pointsRowsUi_) pointsRowsUi_ = std::move(next);
  if (!pointsLoading_ && (!pointsRowsUi_.empty() || pointsEnd_ || !pointsError_.empty())) {
    pointsHasLoaded_ = true;
  }

  // the sort chips follow the controller (a same-sort reselect is a no-op)
  const int sortIndex = pointsSort_ == urnet::PointsLeaderboardSortBlocks   ? 1
                        : pointsSort_ == urnet::PointsLeaderboardSortStreak ? 2
                                                                            : 0;
  if (pointsSortTabs_[sortIndex] != nullptr && !pointsSortTabs_[sortIndex]->get_active()) {
    pointsSortTabs_[sortIndex]->set_active(true);
  }

  if (rowsChanged) RebuildPointsRows();
  RenderPointsHeader();
  RenderPointsFooter();
  if (pointsBoardShowing_) ApplyLedgerMeta();

  // a page that does not fill the pane can never be scrolled to its end, so
  // the next one is asked for once layout has run (the controller refuses a
  // second in-flight page and a page past the end)
  if (!pointsLoading_ && !pointsEnd_ && !pointsRowsUi_.empty()) {
    auto alive = alive_;
    Glib::signal_idle().connect_once([this, alive] {
      if (!*alive || !pointsVc_ || pointsLoading_ || pointsEnd_) return;
      Gtk::ScrolledWindow* scroller = PaneScroller(paneB_.content);
      if (scroller == nullptr) return;
      auto adjustment = scroller->get_vadjustment();
      if (adjustment->get_upper() <= adjustment->get_page_size()) pointsVc_->loadMore();
    });
  }
}

void EarningsPage::RebuildPointsRows() {
  RemoveAllChildren(*pointsRows_);
  pointsRenderedSort_ = pointsSort_;
  if (pointsRowsUi_.empty()) return;

  // the same table builder the data board uses; rank and network read as
  // text, the three figures read right
  const std::vector<int> weights{1, 5, 2, 1, 1};
  pointsRows_->append(*kit::MakePaneTableHeader(
      weights,
      {T_("current_ranking", "Current Ranking"), T_("network", "Network"),
       T_("points", "Points"), T_("blocks", "Blocks"), T_("streak", "Streak")},
      2));
  const bool byBlocks = pointsSort_ == urnet::PointsLeaderboardSortBlocks;
  const bool byStreak = pointsSort_ == urnet::PointsLeaderboardSortStreak;
  const size_t activeColumn = byBlocks ? 3 : (byStreak ? 4 : 2);
  const std::string ownId = pointsMe_ ? pointsMe_->networkId : std::string();
  const Glib::ustring anonymous = T_("anonymous", "Anonymous");

  for (const auto& r : pointsRowsUi_) {
    const bool isOwn = !ownId.empty() && r.networkId == ownId;
    auto row = kit::MakePaneTableRow(weights, kPointsRowHeight, 2);
    row.cells[0]->set_text(byBlocks ? r.rankBlocksText
                                    : (byStreak ? r.rankStreakText : r.rankPointsText));
    // the emoji tag shows either way; the name only when the network is not anonymous
    const bool anon = r.anonymous || r.displayName.empty();
    Glib::ustring name = anon ? anonymous : Glib::ustring(r.displayName);
    if (!r.emojiTag.empty()) name = Glib::ustring(r.emojiTag) + "  " + name;
    row.cells[1]->set_text(name);
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
    pointsRows_->append(*row.root);
  }
}

void EarningsPage::RenderPointsHeader() {
  if (pointsNameLabel_ == nullptr) return;  // not built yet
  const bool hasMe = pointsMe_.has_value();

  pointsEmojiLabel_->set_text(emojiTag_);
  pointsEmojiLabel_->set_visible(!emojiTag_.empty());
  pointsNameLabel_->set_text(hasMe && !pointsMe_->displayName.empty() ? pointsMe_->displayName
                                                                      : std::string("-"));
  const Glib::ustring editName =
      emojiTag_.empty() ? T_("add_emoji", "Add emoji") : T_("edit_emoji", "Edit emoji");
  editEmojiButton_->set_tooltip_text(editName);
  kit::SetAccessibleLabel(*editEmojiButton_, editName);
  kit::SetTextOrCollapse(
      *pointsGroupMeta_,
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
  if (emoji::ShouldLoadMore(lastVisible, rowCount, pointsLoading_, pointsEnd_)) {
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
  pointsHasLoaded_ = true;
  if (pointsBoardStatus_ != nullptr) {
    kit::SetTextOrCollapse(
        *pointsBoardStatus_,
        T_("points_leaderboard_empty", "No one is on the points leaderboard yet."));
  }
}

}  // namespace urnw
