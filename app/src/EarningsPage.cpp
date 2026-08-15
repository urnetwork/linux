// SPDX-License-Identifier: MPL-2.0
#include "EarningsPage.hpp"

#include <glib.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

// ---- the numbers the destination is built from ------------------------------
constexpr int kPaneAWidth = 360;
constexpr int kPaneCWidth = 380;
constexpr int kThreePaneDip = 1500;  // wallets | ledger | points
constexpr int kTwoPaneDip = 900;     // wallets | ledger
constexpr int kApiTimeoutMs = 20000;      // plain api calls (and the sheet watchdog)
constexpr int kBridgeTimeoutMs = 180000;  // browser-bridge flows: minutes are legitimate
constexpr int kValidateDebounceMs = 300;
constexpr size_t kMinValidatableAddress = 32;  // shortest supported: solana base58
constexpr int kWalletDiscSize = 44;            // the header comment says 48; 44 is the code
constexpr double kMultiplierHighlight = 2.0;   // a country multiplier goes lime at >= 2.0
constexpr int kWalletSheetMaxHeight = 520;
constexpr int kPayoutSheetMaxHeight = 560;
constexpr int kSheetMinWidth = 460;
constexpr int kReliabilityChartHeight = 110;

// Two classes the shared pane vocabulary does not carry yet, both spending
// values that already exist in the palette: the own-leaderboard-row fill step
// (#1C1C1C, the SECOND channel beside the lime text — identity is never colour
// alone) and the DEFAULT tag on the wallet sheet (off-white at alpha 0x0A).

// ---- presentation helpers (windows WalletSheets.cpp — ported exactly) --------

Glib::ustring ChainDisplayName(const std::string& blockchain) {
  if (blockchain == urnet::SOL) return T_("solana", "Solana");
  if (blockchain == urnet::TAO) return T_("bittensor", "Bittensor");
  if (blockchain == urnet::MATIC) return T_("polygon", "Polygon");
  return blockchain;  // an unknown chain shows its raw id rather than nothing
}

// "***" + the last six characters. The full address lives on the detail sheet;
// a row never carries it.
std::string MaskAddress(const std::string& address) {
  if (address.empty()) return {};
  const size_t take = std::min<size_t>(6, address.size());
  return "***" + address.substr(address.size() - take);
}

std::string FormatUsdcAmount(double amount) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.2f", amount);
  return buffer;
}

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

// Deliberately ISO, not localized month names: the server's own stamp, trimmed.
std::string ShortDate(const std::string& timestamp) {
  if (timestamp.size() >= 10 && timestamp[4] == '-' && timestamp[7] == '-') {
    return timestamp.substr(0, 10);
  }
  return timestamp;
}

std::string ExplorerTxUrl(const std::string& chain, const std::string& hash) {
  if (hash.empty()) return {};
  if (chain == urnet::SOL) return "https://solscan.io/tx/" + hash;
  return "https://polygonscan.com/tx/" + hash;  // everything else is polygonscan
}

// The SDK promises no order, so the ledger sorts on this: completion time when
// there is one, else the creation time.
std::string PaymentTime(const urnet::AccountPayment& payment) {
  const std::string complete = payment.complete_time.value_or(std::string());
  if (!complete.empty()) return complete;
  return payment.create_time.value_or(std::string());
}

std::string FormatMiB(double mibCount) {
  return FormatByteCountCompact(static_cast<int64_t>(mibCount * 1024.0 * 1024.0));
}

// ---- small typography factories --------------------------------------------

// A right-aligned body-face figure at an explicit size + SemiBold (the pane's
// headline stats: pending payout 22, own rank 22, net provided 18).
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

// A muted label at an explicit size that WRAPS (the note blocks on this
// surface set TextWrapping=Wrap + TextTrimming=None, unlike the kit's trimmed
// row note).
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

// A 12sp muted caption over a condensed-22 value — the one stat cell shape the
// points breakdown and the reliability stats both spend.
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

// A pane row whose height is its CONTENT (windows MinHeight=0 + Padding 12,N):
// the note blocks and the two stat rows the spec pads by hand.
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

// Wrap a built table row in a row BUTTON so the row is keyboard reachable and
// has hover/press: the button's own 12px inset and bottom hairline replace the
// row's, or the inset would be applied twice and the hairline drawn twice.
Gtk::Button* WrapRowInButton(Gtk::Widget* rowRoot, int height) {
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->add_css_class("ur-pane-row");
  button->set_size_request(-1, height);
  if (auto* host = dynamic_cast<Gtk::Box*>(rowRoot)) {
    if (auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child())) {
      inner->set_margin_start(0);
      inner->set_margin_end(0);
    }
    if (auto* rule = host->get_last_child()) rule->set_visible(false);
    host->set_size_request(-1, -1);  // the button pins the height now
  }
  button->set_child(*rowRoot);
  return button;
}

// The chain disc: a `size`x`size` ellipse under a vertical brand gradient with
// the raw ticker centred on it in the bit face. Decorative — the row/sheet
// already says the chain in words.
Gtk::Widget* MakeWalletDisc(const std::string& blockchain, int size) {
  Rgba top = ParseHexColor("8A46FF", kUrTextMuted);  // MATIC + default
  Rgba bottom = ParseHexColor("6E38CC", kUrTextMuted);
  if (blockchain == urnet::SOL) {
    top = ParseHexColor("9945FF", kUrTextMuted);
    bottom = ParseHexColor("14F195", kUrTextMuted);
  } else if (blockchain == urnet::TAO) {
    top = ParseHexColor("1C1C1C", kUrCardBackground);
    bottom = ParseHexColor("3A3A3A", kUrTextFaint);
  }
  auto* area = Gtk::make_managed<Gtk::DrawingArea>();
  area->set_content_width(size);
  area->set_content_height(size);
  area->set_draw_func([top, bottom](const Cairo::RefPtr<Cairo::Context>& cr, int width,
                                    int height) {
    auto gradient = Cairo::LinearGradient::create(width / 2.0, 0, width / 2.0, height);
    gradient->add_color_stop_rgba(0.0, top.r, top.g, top.b, top.a);
    gradient->add_color_stop_rgba(1.0, bottom.r, bottom.g, bottom.b, bottom.a);
    cr->arc(width / 2.0, height / 2.0, std::min(width, height) / 2.0, 0.0, 2.0 * G_PI);
    cr->set_source(gradient);
    cr->fill();
  });
  auto* ticker = Gtk::make_managed<Gtk::Label>();
  // pure white here, deliberately — the disc is a brand mark, not body text
  ticker->set_markup("<span font_family='PP NeueBit' weight='bold' size='" +
                     std::to_string(static_cast<int>(size * 0.42) * PANGO_SCALE) +
                     "' foreground='#ffffff'>" + Glib::Markup::escape_text(blockchain) +
                     "</span>");
  ticker->set_halign(Gtk::Align::CENTER);
  ticker->set_valign(Gtk::Align::CENTER);
  auto* overlay = Gtk::make_managed<Gtk::Overlay>();
  overlay->set_child(*area);
  overlay->add_overlay(*ticker);
  overlay->set_valign(Gtk::Align::START);
  kit::MarkDecorative(*overlay);
  return overlay;
}

// The "DEFAULT" chip. Only the payout wallet carries one, and only on the
// detail sheet — the row marks the default with its lime value instead.
Gtk::Widget* MakePayoutWalletTag() {
  auto* tag = Gtk::make_managed<Gtk::Label>(T_("default_txt", "DEFAULT"));
  tag->add_css_class("ur-earn-tag");
  tag->set_valign(Gtk::Align::START);
  return tag;
}

// ---- points -----------------------------------------------------------------

struct PointsBreakdown {
  double net = 0;
  double payout = 0;
  double referral = 0;
  double multiplier = 0;
  double reliability = 0;
};

// The five buckets, by the server's own event ids. NOTE the SDK naming trap:
// nanoPointsToPoints divides by 1e6, NOT 1e9 despite the name — always the SDK
// helper, never a hand-rolled divisor.
PointsBreakdown AggregatePoints(const urnet::AccountPointsList& points,
                                const std::string* paymentId) {
  PointsBreakdown out;
  if (paymentId != nullptr && paymentId->empty()) return out;  // no id -> zeros
  for (const auto& point : points) {
    if (paymentId != nullptr &&
        point.account_payment_id.value_or(std::string()) != *paymentId) {
      continue;
    }
    const double value = urnet::nanoPointsToPoints(point.point_value);
    out.net += value;
    if (point.event == "payout") {
      out.payout += value;
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

// Shared by the pane C card and the payout detail sheet, so the two can never
// disagree about what a points figure means.
Gtk::Widget* BuildPointsBreakdown(const PointsBreakdown& points, bool seekerHolder) {
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* heading = MakeSizedLabel(T_("points_breakdown", "Points breakdown"), 15, "ur-body");
  column->append(*heading);

  auto* cells = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
  cells->set_homogeneous(true);
  cells->set_margin_top(12);
  cells->append(*MakeStatCell(T_("payout", "Payout"), FormatPointsValue(points.payout)));
  cells->append(*MakeStatCell(T_("referral", "Referral"), FormatPointsValue(points.referral)));
  cells->append(
      *MakeStatCell(T_("reliability", "Reliability"), FormatPointsValue(points.reliability)));
  column->append(*cells);

  auto* rule = kit::MakeDivider();
  rule->set_margin_top(8);
  rule->set_margin_bottom(8);
  column->append(*rule);

  if (seekerHolder) {
    // GREEN, not gold — gold is reserved product-wide for Pro.
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    text->set_hexpand(true);
    auto* verified =
        MakeSizedLabel(T_("seeker_token_verified", "Seeker Token Verified!"), 14, "ur-value");
    verified->add_css_class("ur-value-on");
    text->append(*verified);
    text->append(*MakeSizedLabel(T_("you_re_earning_2x_points", "You're earning 2x points"),
                                 12, "ur-caption"));
    row->append(*text);
    auto* bonus = MakeCondensedValue(
        Format(T_("plus_amount", "+{}"), FormatPointsValue(points.multiplier)), 22, 1.f);
    bonus->set_valign(Gtk::Align::CENTER);
    row->append(*bonus);
    column->append(*row);
    auto* rule2 = kit::MakeDivider();
    rule2->set_margin_top(8);
    rule2->set_margin_bottom(8);
    column->append(*rule2);
  }

  auto* total = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  total->set_halign(Gtk::Align::END);
  total->append(*MakeCondensedValue(FormatPointsValue(points.net), 38, 1.f));
  auto* caption =
      Gtk::make_managed<Gtk::Label>(T_("net_points_earned", "net points earned"));
  caption->add_css_class("ur-caption");
  caption->set_xalign(1.f);
  total->append(*caption);
  column->append(*total);
  return column;
}

// ---- the reliability chart ---------------------------------------------------
// Three polylines on one canvas with INDEPENDENT scales: weights and the mean
// normalize against max(mean, max(weights)); clients against max(clients). The
// series are captured BY VALUE — capturing the shapes strongly is what leaked
// one chart per load on windows.
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
        const double y = h - norm * h;  // y is inverted, clamped to [0,1]*height
        if (index == 0) {
          cr->move_to(x, y);
        } else {
          cr->line_to(x, y);
        }
      }
      cr->stroke();
      cr->restore();
    };

    // z-order: the flat mean under the two live series, weights on top
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
  kit::MarkDecorative(*dot);  // the colour restates the word beside it
  entry->append(*dot);
  auto* text = Gtk::make_managed<Gtk::Label>(label);
  text->add_css_class("ur-caption");
  entry->append(*text);
  return entry;
}

// ---- WalletDetailSheet -------------------------------------------------------
// Opened by a wallet row. It OPENS AND READS with no session — the preview
// harness needs that — but its two buttons are the two API writes, so they are
// disabled without one. Every failure renders ON THE SHEET: a snackbar behind
// a modal is unreadable (a shipped bug).
class WalletDetailSheet : public Gtk::Window {
 public:
  WalletDetailSheet(Gtk::Window& parent, SdkHost& host, urnet::AccountWallet wallet,
                    bool isPayoutWallet, const urnet::AccountPaymentsList& payments,
                    bool allowActions);
  ~WalletDetailSheet() override;

  std::function<void()> on_changed;                     // page: LoadWallet()
  std::function<void(const Glib::ustring&)> on_success;  // page snackbar

 private:
  bool CanAct();
  void SetBusy(bool busy);
  bool SettleRequest(uint32_t generation);
  void ShowError(const Glib::ustring& message);
  void OnMakeDefault();
  void OnRemove();

  SdkHost& host_;
  urnet::AccountWallet wallet_;
  bool allowActions_ = false;
  bool busy_ = false;
  bool removeArmed_ = false;
  uint32_t requestGeneration_ = 0;
  sigc::connection watchdog_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  Gtk::Button* makeDefaultButton_ = nullptr;
  Gtk::Button* removeButton_ = nullptr;
  Gtk::Label* confirmText_ = nullptr;
  Gtk::Label* errorText_ = nullptr;
};

WalletDetailSheet::WalletDetailSheet(Gtk::Window& parent, SdkHost& host,
                                     urnet::AccountWallet wallet, bool isPayoutWallet,
                                     const urnet::AccountPaymentsList& payments,
                                     bool allowActions)
    : host_(host), wallet_(std::move(wallet)), allowActions_(allowActions) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("wallet", "Wallet"));
  set_default_size(kSheetMinWidth, -1);
  add_css_class("ur-sheet");

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  column->set_margin(24);
  column->set_size_request(kSheetMinWidth, -1);

  // 1. identity header: the chain disc, the chain name over the masked
  //    address, and the DEFAULT chip (only when this IS the payout wallet)
  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  header->append(*MakeWalletDisc(wallet_.blockchain, kWalletDiscSize));
  auto* names = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  names->set_valign(Gtk::Align::CENTER);
  names->set_hexpand(true);
  names->append(*MakeSizedLabel(ChainDisplayName(wallet_.blockchain), 15, "ur-value"));
  auto* masked = Gtk::make_managed<Gtk::Label>();
  masked->set_markup("<span font_family='PP NeueBit' weight='bold' size='" +
                     std::to_string(18 * PANGO_SCALE) + "'>" +
                     Glib::Markup::escape_text(MaskAddress(wallet_.wallet_address)) +
                     "</span>");
  masked->set_xalign(0);
  names->append(*masked);
  header->append(*names);
  if (isPayoutWallet) header->append(*MakePayoutWalletTag());
  column->append(*header);

  // 2. the full address — this is what the user came to copy, so it selects
  column->append(
      *MakeSizedLabel(T_("site_app_wallet_address", "Wallet address"), 12, "ur-caption"));
  auto* full = MakeSizedLabel(wallet_.wallet_address, 13, "ur-value");
  full->set_selectable(true);
  full->set_wrap_mode(Pango::WrapMode::CHAR);
  column->append(*full);

  column->append(*kit::MakeDivider());

  // 4. actions — the Bittensor rule: the server refuses TAO as a payout
  //    wallet, so the affordance is REPLACED BY THE REASON, never greyed out
  if (wallet_.blockchain == urnet::TAO) {
    column->append(*MakeSizedLabel(
        T_("bittensor_wallet_future_use",
           "Bittensor wallets are stored for future use and can't receive payouts yet."),
        12, "ur-caption"));
  } else if (!isPayoutWallet) {
    makeDefaultButton_ =
        Gtk::make_managed<Gtk::Button>(T_("make_default", "Make default"));
    makeDefaultButton_->set_sensitive(allowActions_);
    makeDefaultButton_->signal_clicked().connect([this] { OnMakeDefault(); });
    column->append(*makeDefaultButton_);
  }
  removeButton_ = Gtk::make_managed<Gtk::Button>(T_("remove_wallet", "Remove wallet"));
  removeButton_->add_css_class("destructive-action");
  removeButton_->set_sensitive(allowActions_);
  removeButton_->signal_clicked().connect([this] { OnRemove(); });
  column->append(*removeButton_);

  confirmText_ = MakeSizedLabel(
      T_("are_you_sure_you_want_to_remove_this_wallet",
         "Are you sure you want to remove this wallet?"),
      12, "ur-danger-text");
  confirmText_->set_visible(false);
  column->append(*confirmText_);
  errorText_ = MakeSizedLabel({}, 12, "ur-danger-text");
  errorText_->set_visible(false);
  column->append(*errorText_);

  column->append(*kit::MakeDivider());

  // 5. this wallet's payouts
  column->append(*MakeSizedLabel(T_("earnings", "Earnings"), 15, "ur-value"));
  if (payments.empty()) {
    column->append(*MakeSizedLabel(
        T_("no_payouts_found", "No payouts found for this wallet"), 12, "ur-caption"));
  } else {
    for (const auto& payment : payments) {
      auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
      row->set_margin_top(6);
      auto* when = MakeSizedLabel(ShortDate(PaymentTime(payment)), 13, "ur-caption");
      when->set_hexpand(true);
      row->append(*when);
      if (payment.completed.value_or(false)) {
        row->append(*MakeSizedLabel(
            Format(T_("plus_amount_usdc", "+{} USDC"),
                   FormatUsdcAmount(payment.token_amount.value_or(0.0))),
            13, "ur-value"));
      } else {
        row->append(*MakeSizedLabel(T_("pending_payout", "Pending payout"), 13, "ur-caption"));
      }
      column->append(*row);
    }
  }

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(kWalletSheetMaxHeight);
  scroller->set_child(*column);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->append(*scroller);
  auto* close = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  close->set_halign(Gtk::Align::END);
  close->set_margin(16);
  close->signal_clicked().connect([this] { set_visible(false); });
  root->append(*close);
  set_child(*root);
}

WalletDetailSheet::~WalletDetailSheet() {
  ++*epoch_;  // a callback outliving the sheet finds a moved epoch and stops
  watchdog_.disconnect();
}

// Checked at press time too, though the buttons are already disabled —
// defense in depth, out loud.
bool WalletDetailSheet::CanAct() {
  if (allowActions_) return true;
  g_warning("earnings: wallet action refused — no session");
  ShowError(T_("please_login_to_urnetwork", "Please login to URnetwork"));
  return false;
}

void WalletDetailSheet::SetBusy(bool busy) {
  busy_ = busy;
  // never re-enable what has no session
  const bool enabled = !busy_ && allowActions_;
  if (makeDefaultButton_) makeDefaultButton_->set_sensitive(enabled);
  if (removeButton_) removeButton_->set_sensitive(enabled);
  watchdog_.disconnect();
  if (!busy_) return;
  ++requestGeneration_;
  const uint32_t generation = requestGeneration_;
  // The known SDK trap this exists for: setPayoutWallet DROPS the call
  // silently (the callback never fires) when wallet_id is not a UUID.
  watchdog_ = Glib::signal_timeout().connect(
      [this, generation]() -> bool {
        if (busy_ && requestGeneration_ == generation) {
          // the give-up is FINAL: a late success must not hide the sheet and
          // reload after the user was already told it failed
          ++requestGeneration_;
          g_warning("earnings: wallet request timed out with no callback after %d ms",
                    kApiTimeoutMs);
          SetBusy(false);
          ShowError(T_("something_went_wrong", "Something went wrong."));
        }
        return false;
      },
      kApiTimeoutMs);
}

bool WalletDetailSheet::SettleRequest(uint32_t generation) {
  if (generation != requestGeneration_) {
    g_message("earnings: dropping a superseded wallet-sheet result");
    return false;
  }
  SetBusy(false);
  return true;
}

void WalletDetailSheet::ShowError(const Glib::ustring& message) {
  if (!errorText_) return;
  kit::SetTextOrCollapse(*errorText_, message);
}

void WalletDetailSheet::OnMakeDefault() {
  // a second press during flight must NOT paint an error over a live request
  if (busy_) return;
  if (!CanAct()) return;
  const std::string walletId = wallet_.wallet_id.value_or(std::string());
  if (walletId.empty()) {
    ShowError(T_("error_setting_default_wallet", "Error setting default wallet"));
    return;
  }
  ShowError({});
  SetBusy(true);
  const uint32_t generation = requestGeneration_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  urnet::SetPayoutWalletArgs args;
  args.wallet_id = walletId;
  host_.api().setPayoutWallet(
      args, [this, epoch, seen, generation](std::optional<urnet::SetPayoutWalletResult> result,
                                            std::optional<std::string> err) {
        PostToMain([this, epoch, seen, generation, ok = result.has_value() && !err.has_value(),
                    detail = err.value_or(std::string())] {
          if (*epoch != seen) return;  // the sheet is gone
          if (!SettleRequest(generation)) return;
          if (ok) {
            if (on_success) on_success(T_("payout_wallet_updated", "Payout wallet updated"));
            if (on_changed) on_changed();
            set_visible(false);
            return;
          }
          ShowError(detail.empty()
                        ? Glib::ustring(T_("error_setting_default_wallet",
                                           "Error setting default wallet"))
                        : Glib::ustring(Format(
                              T_("error_setting_default_wallet_with_reason",
                                 "Error setting default wallet: {}"),
                              detail)));
        });
      });
}

// Two presses: the first ARMS (confirm line + the button relabels), the second
// commits. Removal reports itself by the sheet closing and the card vanishing
// — the store has no "wallet removed" sentence and inventing English is banned.
void WalletDetailSheet::OnRemove() {
  if (busy_) return;
  if (!removeArmed_) {
    removeArmed_ = true;
    confirmText_->set_visible(true);
    removeButton_->set_label(T_("remove", "Remove"));
    return;
  }
  if (!CanAct()) return;
  const std::string walletId = wallet_.wallet_id.value_or(std::string());
  if (walletId.empty()) {
    ShowError(T_("something_went_wrong", "Something went wrong."));
    return;
  }
  ShowError({});
  SetBusy(true);
  const uint32_t generation = requestGeneration_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  urnet::RemoveWalletArgs args;
  args.wallet_id = walletId;
  host_.api().removeWallet(
      args, [this, epoch, seen, generation](std::optional<urnet::RemoveWalletResult> result,
                                            std::optional<std::string> err) {
        std::string detail = err.value_or(std::string());
        if (detail.empty() && result && result->error) detail = result->error->message;
        const bool ok = result && result->success && !err.has_value();
        PostToMain([this, epoch, seen, generation, ok, detail] {
          if (*epoch != seen) return;
          if (!SettleRequest(generation)) return;
          if (ok) {
            if (on_changed) on_changed();
            set_visible(false);
            return;
          }
          ShowError(detail.empty()
                        ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                        : Glib::ustring(detail));
        });
      });
}

// ---- PayoutDetailSheet -------------------------------------------------------
// Read-only: it makes no requests, so it has no failure states of its own.
class PayoutDetailSheet : public Gtk::Window {
 public:
  PayoutDetailSheet(Gtk::Window& parent, const urnet::AccountPayment& payment,
                    const PointsBreakdown& breakdown, bool seekerHolder);
};

PayoutDetailSheet::PayoutDetailSheet(Gtk::Window& parent, const urnet::AccountPayment& payment,
                                     const PointsBreakdown& breakdown, bool seekerHolder) {
  set_transient_for(parent);
  set_modal(true);
  const bool completed = payment.completed.value_or(false);
  set_title(completed ? Glib::ustring(Format(T_("date_payout", "{} Payout"),
                                             ShortDate(PaymentTime(payment))))
                      : Glib::ustring(T_("pending_payout", "Pending payout")));
  set_default_size(kSheetMinWidth, -1);
  add_css_class("ur-sheet");

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  column->set_margin(24);
  column->set_size_request(kSheetMinWidth, -1);

  // 1. the per-payment points card
  auto* card = MakeCard(0);
  card->append(*BuildPointsBreakdown(breakdown, seekerHolder));
  column->append(*card);

  if (completed) {
    column->append(*MakeSizedLabel(T_("amount", "Amount"), 12, "ur-caption"));
    const std::string tokenType =
        payment.token_type.empty() ? std::string(T_("usdc", "USDC")) : payment.token_type;
    column->append(*MakeSizedLabel(
        FormatUsdcAmount(payment.token_amount.value_or(0.0)) + " " + tokenType, 14, "ur-value"));

    column->append(*MakeSizedLabel(T_("wallet_address", "Wallet Address"), 12, "ur-caption"));
    auto* address = MakeSizedLabel(payment.wallet_address, 13, "ur-value");
    address->set_selectable(true);
    address->set_wrap_mode(Pango::WrapMode::CHAR);
    column->append(*address);

    column->append(*MakeSizedLabel(T_("transaction", "Transaction"), 12, "ur-caption"));
    const std::string hash = payment.tx_hash.value_or(std::string());
    if (hash.empty()) {
      column->append(*MakeSizedLabel(T_("none", "None"), 13, "ur-caption"));
    } else {
      // The hash ITSELF is the link (there is no store string for a separate
      // "view on explorer" row). A server hash is unvalidated, so an
      // unparseable URI must not make the whole sheet unopenable: it degrades
      // to the hash as plain text.
      const std::string url =
          ExplorerTxUrl(payment.blockchain.value_or(std::string()), hash);
      auto* line = MakeSizedLabel({}, 13, "ur-value");
      line->set_wrap_mode(Pango::WrapMode::CHAR);
      if (!url.empty() && g_uri_is_valid(url.c_str(), G_URI_FLAGS_NONE, nullptr)) {
        line->set_markup("<a href=\"" + Glib::Markup::escape_text(url) + "\">" +
                         Glib::Markup::escape_text(hash) + "</a>");
      } else {
        g_warning("earnings: unparseable explorer uri for tx %s", hash.c_str());
        line->set_text(hash);
      }
      column->append(*line);
    }
  } else {
    // decimal MB through the two-decimal usdc formatter — iOS parity, kept
    column->append(*MakeSizedLabel(
        Format(T_("pending_mb_provided", "Pending: {} MB provided"),
               FormatUsdcAmount(static_cast<double>(payment.payout_byte_count) / 1000000.0)),
        13, "ur-caption"));
  }

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(kPayoutSheetMaxHeight);
  scroller->set_child(*column);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->append(*scroller);
  auto* close = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  close->set_halign(Gtk::Align::END);
  close->set_margin(16);
  close->signal_clicked().connect([this] { set_visible(false); });
  root->append(*close);
  set_child(*root);
}

}  // namespace

EarningsPage::EarningsPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  EnsureBrandCss();     // the pane-shell vocabulary
  EnsureDrawerCss();    // .ur-card / .ur-value-on / .ur-caption / input styles
  BuildWalletsPane();
  append(*paneA_.root);
  ruleB_ = kit::MakePaneVRule();
  append(*ruleB_);

  BuildLedgerPane();
  append(*paneB_.root);
  ruleC_ = kit::MakePaneVRule();
  append(*ruleC_);

  BuildPointsPane();
  append(*paneC_.root);

  // ApplyStrings parity: every panel opens on its LOADING state and the five
  // stat values on the faint dash. An unloaded blank destination would read
  // "there is nothing", not "nothing asked yet".
  SetStatValue(pendingValue_, T_("pending_payout", "Pending payout"), {}, false);
  SetStatValue(unpaidValue_, T_("unpaid_data_provided", "Unpaid data provided"), {}, false);
  SetStatValue(referralsValue_, T_("total_referrals", "Total referrals"), {}, false);
  SetStatValue(netProvidedValue_, T_("net_provided", "Net Provided"), {}, false);
  SetStatValue(rankValue_, T_("current_ranking", "Current Ranking"), {}, false);
  ApplySeekerState();
}

EarningsPage::~EarningsPage() {
  ++*epoch_;       // orphan every in-flight completion
  *alive_ = false;  // ... and every marshaled cleanup
  walletDebounce_.disconnect();
  seekerFlow_.timer.disconnect();
  connectFlow_.timer.disconnect();
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
    return false;  // superseded or timed out: the caller must do NOTHING
  }
  flow.timer.disconnect();
  return true;
}

// ---- gating + messaging ------------------------------------------------------

bool EarningsPage::CanCallApi() {
  // TODO(sdk-wiring): SdkHost::apiReady() — this host exposes no has-value
  // check for its in-process Api (api_ and localState_ are derived together in
  // Initialize()), so the session read stands in for both. The preview gate is
  // first for a reason: preview-mode actions once reached production
  // authenticated.
  return !previewMode_ && host_.IsLoggedIn();
}

void EarningsPage::RefuseNoSession() {
  // NEVER silently ignore an affordance — the one exception is address
  // validation while typing, which declines in ValidateWalletAddress().
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
  // Both bars are folded away (spec FLAG: the leaderboard bar lives in pane C,
  // hidden below 1500dip; pane A goes below 900). Hand it to the shell rather
  // than render an error on a hidden bar.
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

double EarningsPage::TotalPaidToWallet(const std::string& walletId) const {
  if (walletId.empty()) return 0.0;
  double total = 0.0;
  for (const auto& payment : payments_) {
    if (!payment.completed.value_or(false)) continue;
    if (payment.wallet_id.value_or(std::string()) != walletId) continue;
    total += payment.token_amount.value_or(0.0);
  }
  return total;
}

// ---- lifecycle ---------------------------------------------------------------

void EarningsPage::Load() {
  ++*epoch_;  // drop every completion armed for the previous session
  if (samplePinned_) return;  // the pinned sample must not be clobbered
  LoadWallet();
  // The leaderboard is a one-shot per LOOK, never per PROCESS.
  //
  // It used to be per process ("fires the first time the Leaderboard tab is
  // looked at, and never again"), and that is what made the observed bug
  // permanent: every pane-A read is re-issued here on every navigation and on
  // every auth change (MainWindow's on_navigate + ApplyAuthState), so a bad
  // early attempt heals itself — while pane B kept whatever the FIRST attempt
  // settled on, including a no-session settle written before the user had even
  // signed in. Re-arming here gives the board the same self-healing: if the
  // Leaderboard tab is the one showing, its fetch is re-issued now; otherwise
  // the next look at the tab re-issues it.
  if (leaderboardTab_ != nullptr && leaderboardTab_->get_active()) {
    leaderboardRequested_ = true;
    LoadLeaderboard();
  } else {
    leaderboardRequested_ = false;
  }
}

void EarningsPage::ApplyBreakpoint(int widthDip) {
  const int lanes = widthDip >= kThreePaneDip ? 3 : (widthDip >= kTwoPaneDip ? 2 : 1);
  if (lanes_ == lanes) return;
  lanes_ = lanes;
  // The LEDGER survives to the smallest width — a payouts table is what the
  // user opens this destination to read.
  paneA_.root->set_visible(lanes >= 2);
  ruleB_->set_visible(lanes >= 2);
  paneC_.root->set_visible(lanes >= 3);
  ruleC_->set_visible(lanes >= 3);
}

void EarningsPage::SetBalanceState(bool isPro, bool guest) {
  isPro_ = isPro;
  isGuest_ = guest;
  // hidden for Pro AND for guests: an account comes first
  if (upgradeButton_ != nullptr) upgradeButton_->set_visible(!isPro_ && !isGuest_);
}

void EarningsPage::SetPreviewMode(bool on) { previewMode_ = on; }

void EarningsPage::ShowPreviewState() { SettleAllEmpty(); }

void EarningsPage::ShowPreviewSnackbar() {
  // the PERSISTENT severity (support previews the timing-out one)
  Notify(T_("wallet_connect_failed", "Failed to connect the wallet."),
         kit::Snackbar::Severity::Error);
}

void EarningsPage::SettleAllEmpty() {
  // Every panel lands on its real empty state, through the same Apply*
  // functions the server answers use.
  ApplyWallets(urnet::AccountWalletsList{}, Fetch::Ready);
  ApplyTransferStats(false, 0);
  ApplyWalletBalance(false, 0);
  ApplyReferrals(false, 0);
  ApplyPoints(urnet::AccountPointsList{}, Fetch::Ready);
  ApplyReliability(std::nullopt, Fetch::Ready);
  ApplyPayments(urnet::AccountPaymentsList{}, Fetch::Ready);
  ApplyRanking(std::nullopt, false);
  // ...with ONE exception, and it is the bug this file was carrying. The
  // leaderboard is not this network's data: "no session" is not an answer about
  // it, so it must never be settled Ready+empty here — that renders as the
  // authoritative "No networks on the leaderboard yet." The preview harness is
  // the only caller that legitimately wants the REAL empty state (it exists to
  // review exactly that); a live no-session lands on NoSession instead, which
  // also re-arms the fetch so the next look asks the server for real.
  if (previewMode_) {
    ApplyLeaderboard(urnet::LeaderboardEarnersList{}, Fetch::Ready);
  } else {
    ApplyLeaderboard(std::nullopt, Fetch::NoSession);
  }
}

// ---- PANE A: wallets (360) ---------------------------------------------------

void EarningsPage::BuildWalletsPane() {
  paneA_ = kit::MakePane(T_("payout_wallets", "Payout Wallets"));
  paneA_.root->set_size_request(kPaneAWidth, -1);
  paneA_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneA_.root, T_("payout_wallets", "Payout Wallets"));
  Gtk::Box* content = paneA_.content;

  // 1. pending payout — the pane's headline figure
  {
    auto row = MakePaddedRow(12);
    auto* grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* key = Gtk::make_managed<Gtk::Label>(T_("pending_payout", "Pending payout"));
    key->add_css_class("ur-key");
    key->set_xalign(0);
    key->set_hexpand(true);
    key->set_valign(Gtk::Align::END);
    kit::MarkDecorative(*key);  // the key names the value; one fact, one node
    grid->append(*key);
    pendingValue_ = MakeStrongValue(22);
    grid->append(*pendingValue_);
    row.content->append(*grid);
    content->append(*row.root);
  }

  // 2 + 3. unpaid data and referrals — the kit's 34px key/value species
  {
    auto unpaid = kit::MakePaneKeyValueRow(T_("unpaid_data_provided", "Unpaid data provided"));
    unpaidValue_ = unpaid.value;
    content->append(*unpaid.root);
    auto referrals = kit::MakePaneKeyValueRow(T_("total_referrals", "Total referrals"));
    referralsValue_ = referrals.value;
    content->append(*referrals.root);
  }

  // 4. the payout-threshold note. The threshold AMOUNT is never named — the
  //    server does not report it, and inventing a figure would be a promise.
  {
    auto row = MakePaddedRow(8);
    row.content->append(*MakeWrappedNote(
        T_("payouts_amount_threshold",
           "Payouts occur every Sunday at 00:00 UTC, and require meeting a minimum USDC "
           "threshold."),
        "ur-row-note"));
    content->append(*row.root);
  }

  // 5. the pane's primary action. Visibility is the window's balance relay,
  //    not this page's: SetBalanceState.
  upgradeButton_ =
      Gtk::make_managed<Gtk::Button>(T_("upgrade_with_stripe", "Upgrade with Stripe"));
  upgradeButton_->add_css_class("ur-pane-primary");
  // the markup default is visible (a free account is the default state); the
  // window's balance relay narrows it to !isPro && !guest
  upgradeButton_->set_visible(true);
  upgradeButton_->signal_clicked().connect([this] {
    if (on_open_upgrade) {
      // the window forks it: guests into the create-account (guest upgrade)
      // flow, everyone else into the existing UpgradeSheet
      on_open_upgrade();
      return;
    }
    g_warning("earnings: upgrade route unbound; the button opened nothing");
  });
  content->append(*upgradeButton_);

  // 6. the wallets group + its fetch status
  {
    auto group = kit::MakePaneGroupHeader(T_("payout_wallets", "Payout Wallets"),
                                          T_("loading", "Loading..."));
    walletsStatus_ = group.meta;
    content->append(*group.root);
  }

  // 7. one row per wallet (rebuilt whole; per-wallet totals derive from
  //    payments, so a stale total under "Something went wrong" is a bug)
  walletCardsPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  content->append(*walletCardsPanel_);

  // 8. the empty state — Ready + zero wallets ONLY. A failure clears the cards
  //    AND hides this: the status line carries the failure.
  {
    auto row = MakePaddedRow(12);
    row.content->set_spacing(6);
    row.content->append(*MakeWrappedNote(
        T_("to_start_earning_connect_your_solana_wallet_to",
           "To start earning, connect your Solana wallet to URnetwork."),
        "ur-key"));
    row.content->append(*MakeWrappedNote(
        T_("these_wallets_are_not_affiliated_or_controlled",
           "These wallets are not affiliated or controlled by URnetwork. We will send "
           "earnings into the connected wallet."),
        "ur-row-note"));
    walletsEmptyPanel_ = row.root;
    walletsEmptyPanel_->set_visible(false);
    content->append(*row.root);
  }

  // 9 + 10. connect a wallet
  content->append(*kit::MakePaneGroupHeader(T_("connect_a_wallet", "Connect a wallet")).root);
  {
    auto row = MakePaddedRow(12);
    row.content->set_spacing(10);
    // two store sentences joined with one space: what is supported, and the
    // TAO caveat (a bittensor wallet can be connected and can never pay out)
    row.content->append(*MakeWrappedNote(
        Glib::ustring(T_("connect_external_wallet_supported_chains",
                         "USDC addresses on Solana and Polygon are currently supported.")) +
            " " +
            T_("bittensor_wallet_future_use",
               "Bittensor wallets are stored for future use and can't receive payouts yet."),
        "ur-row-note"));

    walletAddressBox_ = Gtk::make_managed<Gtk::Entry>();
    walletAddressBox_->add_css_class("ur-input");
    walletAddressBox_->set_placeholder_text(T_("enter_wallet_address", "Enter wallet address"));
    // a placeholder is NOT an accessible name
    kit::SetAccessibleLabel(*walletAddressBox_,
                            T_("enter_wallet_address", "Enter wallet address"));
    walletAddressBox_->signal_changed().connect(
        sigc::mem_fun(*this, &EarningsPage::OnWalletAddressChanged));
    row.content->append(*walletAddressBox_);

    walletChainText_ = MakeWrappedNote({}, "ur-row-note");
    walletChainText_->set_visible(false);
    row.content->append(*walletChainText_);

    connectWalletButton_ = Gtk::make_managed<Gtk::Button>(T_("connect", "Connect"));
    connectWalletButton_->set_sensitive(false);  // nothing is validated yet
    connectWalletButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnConnectWallet));
    row.content->append(*connectWalletButton_);
    content->append(*row.root);
  }

  // 11. the wallet pane's own snackbar surface
  walletInfo_.root().set_margin_top(8);
  walletInfo_.root().set_margin_bottom(8);
  content->append(walletInfo_.root());
}

// ---- PANE B: the ledger (star column) ---------------------------------------

void EarningsPage::BuildLedgerPane() {
  paneB_ = kit::MakePane(T_("payouts", "Payouts"));
  paneB_.root->set_hexpand(true);
  // the landmark name is STATICALLY "Payouts", even while the Leaderboard tab
  // is showing (windows a11y nuance, kept)
  kit::SetAccessibleLabel(*paneB_.root, T_("payouts", "Payouts"));

  // the header strip carries a 2-item segmented switch instead of a title
  paneB_.title->set_visible(false);
  auto* tabs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  tabs->add_css_class("linked");
  tabs->set_valign(Gtk::Align::CENTER);
  tabs->set_hexpand(true);
  tabs->set_halign(Gtk::Align::START);
  payoutsTab_ = Gtk::make_managed<Gtk::ToggleButton>(T_("payouts", "Payouts"));
  leaderboardTab_ = Gtk::make_managed<Gtk::ToggleButton>(T_("leaderboard", "Leaderboard"));
  leaderboardTab_->set_group(*payoutsTab_);
  payoutsTab_->set_active(true);  // Payouts is the default selection
  for (Gtk::ToggleButton* tab : {payoutsTab_, leaderboardTab_}) {
    tabs->append(*tab);
    // toggled fires for the deactivated button too; apply once, on activation
    tab->signal_toggled().connect([this, tab] {
      if (tab->get_active()) OnLedgerTabChanged();
    });
  }
  paneB_.header->prepend(*tabs);

  // two stacked full-height hosts, exactly one visible
  payoutsHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  payoutsHost_->set_vexpand(true);
  payoutsPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  payoutsHost_->append(*payoutsPanel_);
  payoutsStatus_ = kit::MakePaneEmptyLine(T_("loading", "Loading..."));
  payoutsHost_->append(*payoutsStatus_);
  paneB_.content->append(*payoutsHost_);

  leaderboardHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  leaderboardHost_->set_vexpand(true);
  leaderboardHost_->set_visible(false);
  leaderboardRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  leaderboardHost_->append(*leaderboardRows_);
  leaderboardStatus_ = kit::MakePaneEmptyLine(T_("loading", "Loading..."));
  leaderboardHost_->append(*leaderboardStatus_);
  paneB_.content->append(*leaderboardHost_);
}

// ---- PANE C: network earnings (380) ------------------------------------------

void EarningsPage::BuildPointsPane() {
  // deliberately NOT "Account points" — that is the first group's own header
  paneC_ = kit::MakePane(T_("network_earnings", "Network earnings"));
  paneC_.root->set_size_request(kPaneCWidth, -1);
  paneC_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneC_.root, T_("network_earnings", "Network earnings"));
  Gtk::Box* content = paneC_.content;

  // 1 + 2. own ranking
  content->append(*kit::MakePaneGroupHeader(T_("current_ranking", "Current Ranking")).root);
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
    netProvidedValue_ = MakeStrongValue(18);  // 18, not 22 — the rank is the 22
    figures->append(*netProvidedValue_);
    rankValue_ = MakeStrongValue(22);
    figures->append(*rankValue_);
    grid->append(*figures);
    row.content->append(*grid);
    content->append(*row.root);
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
  }

  // 5. the leaderboard pane's snackbar surface
  leaderboardInfo_.root().set_margin_top(8);
  leaderboardInfo_.root().set_margin_bottom(8);
  content->append(leaderboardInfo_.root());

  // 6 + 7. account points
  {
    auto group = kit::MakePaneGroupHeader(T_("account_points", "Account points"),
                                          T_("loading", "Loading..."));
    accountPointsStatus_ = group.meta;
    content->append(*group.root);
  }
  {
    auto row = MakePaddedRow(10);
    accountPointsCard_ = row.root;
    accountPointsPanel_ = row.content;
    accountPointsCard_->set_visible(false);  // collapsed until Ready
    content->append(*row.root);
  }

  // 8 + 9. earning multipliers: the seeker block
  content->append(
      *kit::MakePaneGroupHeader(T_("earning_multipliers", "Earning multipliers")).root);
  {
    auto row = MakePaddedRow(10);
    row.content->set_spacing(10);
    seekerStatus_ = MakeWrappedNote({}, "ur-key");
    row.content->append(*seekerStatus_);
    verifySeekerButton_ = Gtk::make_managed<Gtk::Button>(
        T_("verify_seeker_token_btn", "Verify Seeker Pre-Order Token"));
    verifySeekerButton_->signal_clicked().connect(
        sigc::mem_fun(*this, &EarningsPage::OnVerifySeeker));
    row.content->append(*verifySeekerButton_);
    content->append(*row.root);
  }

  // 10 + 11. network reliability
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

// EIGHT independent requests, never chained: each settles its own panel, so
// one 500 cannot blank the rest. Every callback lands on an SDK thread and is
// marshaled with PostToMain, then dropped if the epoch moved.
void EarningsPage::LoadWallet() {
  if (!CanCallApi()) {
    // No session: settle every panel on its real empty state rather than leave
    // a permanent "Loading...", which is indistinguishable from a hang.
    g_message("earnings: no session; settling the wallet panels on their empty state");
    SettleAllEmpty();
    return;
  }

  // the own-row highlight on the leaderboard reads from this
  if (auto jwt = host_.ParseByJwt()) {
    ownNetworkId_ = jwt->NetworkId.value_or(std::string());
  }

  kit::SetTextOrCollapse(*walletsStatus_, T_("loading", "Loading..."));
  kit::SetTextOrCollapse(*accountPointsStatus_, T_("loading", "Loading..."));
  kit::SetTextOrCollapse(*reliabilityStatus_, T_("loading", "Loading..."));
  kit::SetTextOrCollapse(*payoutsStatus_, T_("loading", "Loading..."));

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;

  // 1. the wallets themselves
  host_.api().getAccountWallets(
      [this, epoch, seen](std::optional<urnet::GetAccountWalletsResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result) {
            g_warning("earnings: getAccountWallets failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplyWallets(std::nullopt, Fetch::Failed);
            return;
          }
          ApplyWallets(result->wallets, Fetch::Ready);
        });
      });

  // 2. which of them is the payout wallet
  host_.api().getPayoutWallet(
      [this, epoch, seen](std::optional<urnet::GetPayoutWalletIdResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result) {
            // a miss is NOT an error: it is logged and nothing changes
            g_message("earnings: getPayoutWallet miss: %s", err ? err->c_str() : "(no result)");
            return;
          }
          ApplyPayoutWalletId(result->wallet_id.value_or(std::string()));
        });
      });

  // 3. unpaid data provided
  host_.api().getTransferStats(
      [this, epoch, seen](std::optional<urnet::TransferStatsResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result) {
            g_warning("earnings: getTransferStats failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplyTransferStats(false, 0);
            return;
          }
          ApplyTransferStats(true, result->unpaid_bytes_provided);
        });
      });

  // 4. the pending payout figure
  host_.api().walletBalance(
      [this, epoch, seen](std::optional<urnet::WalletBalanceResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result || !result->wallet_info) {
            g_warning("earnings: walletBalance failed: %s", err ? err->c_str() : "(no result)");
            ApplyWalletBalance(false, 0);
            return;
          }
          ApplyWalletBalance(true, result->wallet_info->balance_usdc_nano_cents);
        });
      });

  // 5. total referrals
  host_.api().getNetworkReferralCode(
      [this, epoch, seen](std::optional<urnet::GetNetworkReferralCodeResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result || result->error) {
            g_warning("earnings: getNetworkReferralCode failed: %s",
                      err ? err->c_str() : "(server error)");
            ApplyReferrals(false, 0);
            return;
          }
          ApplyReferrals(true, result->total_referrals);
        });
      });

  // 6. account points (pane C card + the per-payment breakdowns)
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

  // 7. the reliability window
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

  // 8. the payouts ledger (which the per-wallet totals also derive from)
  host_.api().getAccountPayments(
      [this, epoch, seen](std::optional<urnet::GetNetworkAccountPaymentsResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result), err = std::move(err)] {
          if (*epoch != seen) return;
          if (err || !result || result->error) {
            g_warning("earnings: getAccountPayments failed: %s",
                      err ? err->c_str() : "(server error)");
            ApplyPayments(std::nullopt, Fetch::Failed);
            return;
          }
          ApplyPayments(result->account_payments, Fetch::Ready);
        });
      });
}

void EarningsPage::LoadLeaderboard() {
  if (!CanCallApi()) {
    // NOT "empty" — nothing was asked. ApplyLeaderboard re-arms the one-shot on
    // this state, so the look that lands once a session exists asks for real.
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
          // `!result->earners` is part of the FAILURE predicate, matching the
          // Windows source of truth (WalletPage.cpp:1595 `result && result->
          // earners && !err`). A LeaderboardResult with no earners list is a
          // fault, not an answer: Go marshals a nil *LeaderboardResult as the
          // document `null` (cgo/cstrings.go cJson) and parseJson turns `null`
          // into a default-constructed struct, so "the SDK produced nothing"
          // arrives here as an ENGAGED optional whose fields are all unset. It
          // used to fall through to Ready and render as "no networks".
          if (err || !result || result->error || !result->earners) {
            g_warning("earnings: getLeaderboard failed: %s",
                      err ? err->c_str()
                          : (result && result->error ? result->error->message.c_str()
                                                     : "(no earners in the result)"));
            ApplyLeaderboard(std::nullopt, Fetch::Failed);
            return;
          }
          // The success line the investigation needed and did not have: with it,
          // a genuinely empty board and a discarded one are never confusable.
          g_message("earnings: getLeaderboard ok: %zu earner(s)", result->earners->size());
          ApplyLeaderboard(result->earners, Fetch::Ready);
        });
      });
}

// ---- appliers ----------------------------------------------------------------

void EarningsPage::ApplyWallets(std::optional<urnet::AccountWalletsList> wallets, Fetch state) {
  walletsState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*walletsStatus_, T_("loading", "Loading..."));
    return;
  }
  if (state == Fetch::Failed) {
    // the failure lives in the status line; the cards clear and the empty
    // panel collapses so "no wallets" is never shown as the answer
    kit::SetTextOrCollapse(*walletsStatus_,
                           T_("something_went_wrong", "Something went wrong."));
    wallets_.clear();
    seekerHolder_ = false;
    RemoveAllChildren(*walletCardsPanel_);
    walletsEmptyPanel_->set_visible(false);
    kit::SetTextOrCollapse(*paneA_.meta, {});
    ApplySeekerState();
    return;
  }
  wallets_ = wallets.value_or(urnet::AccountWalletsList{});
  seekerHolder_ = false;
  for (const auto& wallet : wallets_) {
    if (wallet.has_seeker_token) seekerHolder_ = true;
  }
  kit::SetTextOrCollapse(*walletsStatus_, {});
  walletsEmptyPanel_->set_visible(wallets_.empty());
  kit::SetTextOrCollapse(*paneA_.meta, wallets_.empty()
                                           ? Glib::ustring()
                                           : Glib::ustring(std::to_string(wallets_.size())));
  RebuildWalletCards();
  ApplySeekerState();
}

void EarningsPage::ApplyPayoutWalletId(const std::string& walletId) {
  // An EMPTY id is ignored: the server may answer nil transiently, and
  // dropping the marker would make the default wallet look unset.
  if (walletId.empty()) return;
  payoutWalletId_ = walletId;
  RebuildWalletCards();
}

void EarningsPage::ApplyTransferStats(bool ok, int64_t unpaidBytes) {
  SetStatValue(unpaidValue_, T_("unpaid_data_provided", "Unpaid data provided"),
               ok ? Glib::ustring(FormatByteCountCompact(unpaidBytes)) : Glib::ustring(), ok);
}

void EarningsPage::ApplyWalletBalance(bool ok, int64_t balanceNanoCents) {
  SetStatValue(pendingValue_, T_("pending_payout", "Pending payout"),
               ok ? Glib::ustring(Format(T_("amount_usdc", "{} USDC"),
                                         FormatUsdcAmount(
                                             urnet::nanoCentsToUsd(balanceNanoCents))))
                  : Glib::ustring(),
               ok);
}

void EarningsPage::ApplyReferrals(bool ok, int64_t totalReferrals) {
  SetStatValue(referralsValue_, T_("total_referrals", "Total referrals"),
               ok ? Glib::ustring(std::to_string(totalReferrals)) : Glib::ustring(), ok);
}

void EarningsPage::ApplyPoints(std::optional<urnet::AccountPointsList> points, Fetch state) {
  pointsState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*accountPointsStatus_, T_("loading", "Loading..."));
    accountPointsCard_->set_visible(false);
    return;
  }
  if (state == Fetch::Failed) {
    points_.clear();
    kit::SetTextOrCollapse(*accountPointsStatus_,
                           T_("something_went_wrong", "Something went wrong."));
    accountPointsCard_->set_visible(false);
    RemoveAllChildren(*accountPointsPanel_);
    return;
  }
  points_ = points.value_or(urnet::AccountPointsList{});
  kit::SetTextOrCollapse(*accountPointsStatus_, {});
  accountPointsCard_->set_visible(true);
  RebuildPointsCard();
  RebuildWalletCards();  // the seeker state can change with the points
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
    // Ready with no window is NOT a failure and must not read like one
    kit::SetTextOrCollapse(*reliabilityStatus_,
                           T_("site_app_no_reliability", "No reliability data yet."));
    reliabilityCard_->set_visible(false);
    return;
  }
  kit::SetTextOrCollapse(*reliabilityStatus_, {});
  reliabilityCard_->set_visible(true);
  RebuildReliabilityCard();
}

void EarningsPage::ApplyPayments(std::optional<urnet::AccountPaymentsList> payments,
                                 Fetch state) {
  paymentsState_ = state;
  if (state == Fetch::Loading) {
    kit::SetTextOrCollapse(*payoutsStatus_, T_("loading", "Loading..."));
    return;
  }
  if (state == Fetch::Failed) {
    payments_.clear();
    kit::SetTextOrCollapse(*payoutsStatus_,
                           T_("something_went_wrong", "Something went wrong."));
    RemoveAllChildren(*payoutsPanel_);
  } else {
    payments_ = payments.value_or(urnet::AccountPaymentsList{});
    // the SDK promises no order: newest first, by completion time when there
    // is one
    std::stable_sort(payments_.begin(), payments_.end(),
                     [](const urnet::AccountPayment& a, const urnet::AccountPayment& b) {
                       return PaymentTime(a) > PaymentTime(b);
                     });
    kit::SetTextOrCollapse(*payoutsStatus_, {});
    if (payments_.empty()) {
      RemoveAllChildren(*payoutsPanel_);
      // deliberately a CARD here while the leaderboard's empty state is a bare
      // centred line — the two tables in one pane use different vocabulary
      // (an inconsistency kept for parity)
      auto* empty = kit::MakeEmptyStateCard("", T_("site_app_no_payouts", "No payouts yet"));
      empty->set_margin(16);
      payoutsPanel_->append(*empty);
    } else {
      RebuildPayouts();
    }
  }
  // EVERY path, failed and empty included: the per-wallet totals derive from
  // payments, and stale totals under "Something went wrong" shipped as a bug
  RebuildWalletCards();
  ApplyLedgerMeta();
}

void EarningsPage::ApplyRanking(std::optional<urnet::NetworkRanking> ranking, bool ok) {
  if (!ok || !ranking) {
    // the list's own status line carries the failure: two error messages for
    // one screen is noise
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
    // The pane says what is true — nobody asked yet — and the one-shot is
    // re-armed so the first look with a session issues the real fetch. This is
    // the branch that must NEVER borrow the empty-board string.
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
    // a failure is RETRYABLE: the next look at the tab (or the next navigation
    // to the destination) asks again rather than freezing the pane on the error
    leaderboardRequested_ = false;
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
    // ONE centred line in the full-height pane — deliberately NOT a card
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

void EarningsPage::RebuildWalletCards() {
  RemoveAllChildren(*walletCardsPanel_);
  for (const auto& wallet : wallets_) {
    const std::string walletId = wallet.wallet_id.value_or(std::string());
    const bool isPayout = !walletId.empty() && walletId == payoutWalletId_;
    // masked address over the chain product name, lifetime USDC on the right
    auto row = kit::MakePaneTwoLineRowButton(MaskAddress(wallet.wallet_address),
                                             ChainDisplayName(wallet.blockchain));
    row.value->set_text(Format(T_("amount_usdc", "{} USDC"),
                               FormatUsdcAmount(TotalPaidToWallet(walletId))));
    if (isPayout) {
      // the lime value is the row's ONLY visual default-marker: no disc and no
      // DEFAULT chip out here (both live on the detail sheet)
      row.value->remove_css_class("dim-label");
      row.value->add_css_class("ur-value-on");
    }
    Glib::ustring name = Format(T_("wallet_provider", "{} Wallet"),
                                ChainDisplayName(wallet.blockchain).raw());
    name += ", " + MaskAddress(wallet.wallet_address);
    if (isPayout) name += Glib::ustring(", ") + T_("default_wallet", "Default");
    kit::SetAccessibleLabel(*row.root, name);
    const urnet::AccountWallet copy = wallet;
    row.root->signal_clicked().connect([this, copy] { ShowWalletDetail(copy); });
    walletCardsPanel_->append(*row.root);
  }
}

void EarningsPage::RebuildPayouts() {
  RemoveAllChildren(*payoutsPanel_);
  const std::vector<int> weights{2, 2, 3, 3};
  payoutsPanel_->append(*kit::MakePaneTableHeader(
      weights,
      {T_("payout", "Payout"), T_("amount", "Amount"), T_("site_app_wallet", "Wallet"),
       T_("transaction", "Transaction")},
      1));
  for (const auto& payment : payments_) {
    auto row = kit::MakePaneTableRow(weights, 36, 1);
    const std::string when = ShortDate(PaymentTime(payment));
    row.cells[0]->set_text(when);
    if (payment.completed.value_or(false)) {
      // lime = money that ARRIVED; the only lime on the row
      row.cells[1]->set_text(Format(T_("plus_amount_usdc", "+{} USDC"),
                                    FormatUsdcAmount(payment.token_amount.value_or(0.0))));
      row.cells[1]->remove_css_class("dim-label");
      row.cells[1]->add_css_class("ur-value-on");
    } else {
      row.cells[1]->set_text(T_("pending_payout", "Pending payout"));
    }
    row.cells[2]->set_text(MaskAddress(payment.wallet_address));
    const std::string hash = payment.tx_hash.value_or(std::string());
    row.cells[3]->set_text(hash.empty() ? Glib::ustring(T_("none", "None"))
                                        : Glib::ustring(MaskAddress(hash)));
    kit::MarkDecorative(*row.cells[0]);  // the row's own name says the date
    auto* button = WrapRowInButton(row.root, 36);
    // EVERY row, pending included: the date is the only thing distinguishing
    // one pending row from another
    kit::SetAccessibleLabel(*button, Format(T_("date_payout", "{} Payout"), when));
    const urnet::AccountPayment copy = payment;
    button->signal_clicked().connect([this, copy] { ShowPayoutDetail(copy); });
    payoutsPanel_->append(*button);
  }
  ApplyLedgerMeta();
}

void EarningsPage::RebuildLeaderboard() {
  RemoveAllChildren(*leaderboardRows_);
  const std::vector<int> weights{1, 5, 2};
  // textColumns = 2: rank and network name read left as text, net-provided
  // reads right as a figure
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
    // the name is NEVER rendered for a non-public network; profanity is
    // flagged by the server and hidden by the client. The OWN row is never
    // masked.
    const bool masked = !isOwn && (!earner.is_public || earner.contains_profanity);
    row.cells[0]->set_text("#" + std::to_string(rank));
    row.cells[1]->set_text(masked ? Glib::ustring(T_("private_network", "Private Network"))
                                  : Glib::ustring(earner.network_name));
    row.cells[2]->set_text(FormatMiB(earner.net_mib_count));
    if (masked) {
      for (Gtk::Label* cell : row.cells) cell->add_css_class("dim-label");
    }
    if (isOwn) {
      // colour PLUS a fill step, never colour alone
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

void EarningsPage::RebuildPointsCard() {
  RemoveAllChildren(*accountPointsPanel_);
  accountPointsPanel_->append(
      *BuildPointsBreakdown(AggregatePoints(points_, nullptr), seekerHolder_));
}

void EarningsPage::RebuildReliabilityCard() {
  RemoveAllChildren(*reliabilityPanel_);
  if (!reliability_) return;
  const urnet::ReliabilityWindow& window = *reliability_;

  // 1. the two headline figures
  auto* stats = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 24);
  stats->set_homogeneous(true);
  char meanText[32];
  std::snprintf(meanText, sizeof(meanText), "%.2f", window.mean_reliability_weight);
  stats->append(*MakeStatCell(T_("average_reliability", "Average reliability"), meanText));
  stats->append(*MakeStatCell(T_("total_clients", "Total Clients"),
                              std::to_string(window.max_total_client_count)));
  reliabilityPanel_->append(*stats);

  // 2. the chart — only when a series can actually be drawn
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

  // 3. country multipliers — exactly 1.0 means "no multiplier" and is dropped
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

void EarningsPage::ApplySeekerState() {
  if (seekerStatus_ == nullptr || verifySeekerButton_ == nullptr) return;
  if (seekerHolder_) {
    seekerStatus_->set_text(
        Glib::ustring(T_("seeker_token_verified", "Seeker Token Verified!")) + " " +
        T_("you_re_earning_2x_points", "You're earning 2x points"));
    verifySeekerButton_->set_visible(false);
    return;
  }
  // "waiting" must be VISIBLE — a silently greyed button is indistinguishable
  // from a broken one
  seekerStatus_->set_text(
      verifyingSeeker_
          ? T_("opening_wallet_in_browser", "Opening your wallet in the browser…")
          : T_("connect_seeker_wallet",
               "Connect a wallet with the Saga Genesis or Seeker Pre-Order Token"));
  verifySeekerButton_->set_visible(true);
  verifySeekerButton_->set_sensitive(!verifyingSeeker_);
}

void EarningsPage::ApplyLedgerMeta() {
  // the row count OF WHICHEVER TABLE IS SHOWING
  const int count = (leaderboardTab_ != nullptr && leaderboardTab_->get_active())
                        ? leaderboardCount_
                        : static_cast<int>(payments_.size());
  kit::SetTextOrCollapse(
      *paneB_.meta, count > 0 ? Glib::ustring(std::to_string(count)) : Glib::ustring());
}

void EarningsPage::OnLedgerTabChanged() {
  // No echo guard is needed here and none exists: the default selection is set
  // BEFORE the handlers are connected, and nothing else writes the tabs.
  const bool payouts = !leaderboardTab_->get_active();
  payoutsHost_->set_visible(payouts);
  leaderboardHost_->set_visible(!payouts);
  ApplyLedgerMeta();
  // the FIRST time the Leaderboard tab is looked at, and never again
  if (!payouts && !leaderboardRequested_) {
    leaderboardRequested_ = true;
    LoadLeaderboard();
  }
}

// ---- connect a wallet --------------------------------------------------------

void EarningsPage::OnWalletAddressChanged() {
  // every keystroke: forget the verdict, drop the in-flight answers, disarm
  walletValidSol_ = false;
  walletValidMatic_ = false;
  walletValidTao_ = false;
  walletChain_.clear();
  ++walletValidateGeneration_;
  connectWalletButton_->set_sensitive(false);
  kit::SetTextOrCollapse(*walletChainText_, {});
  walletDebounce_.disconnect();
  walletDebounce_ = Glib::signal_timeout().connect(
      [this]() -> bool {
        ValidateWalletAddress();
        return false;  // non-repeating
      },
      kValidateDebounceMs);
}

void EarningsPage::ValidateWalletAddress() {
  const std::string address = walletAddressBox_->get_text().raw();
  // The ONE affordance allowed to decline SILENTLY: the user did not ask for
  // anything, so a too-short address or no session says nothing at all.
  if (address.size() < kMinValidatableAddress || !CanCallApi()) return;

  const uint64_t generation = ++walletValidateGeneration_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // three calls, one per chain, in the precedence order they are resolved in
  for (const char* chain : {urnet::SOL, urnet::MATIC, urnet::TAO}) {
    const std::string chainId = chain;
    urnet::WalletValidateAddressArgs args;
    args.address = address;
    args.chain = chainId;
    host_.api().walletValidateAddress(
        args, [this, epoch, seen, generation, chainId](
                  std::optional<urnet::WalletValidateAddressResult> result,
                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, generation, chainId, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (err || !result) {
              // a transport error is logged and treated as invalid
              g_warning("earnings: walletValidateAddress(%s) failed: %s", chainId.c_str(),
                        err ? err->c_str() : "(no result)");
              ApplyWalletValidation(chainId, generation, false);
              return;
            }
            ApplyWalletValidation(chainId, generation, result->valid.value_or(false));
          });
        });
  }
}

void EarningsPage::ApplyWalletValidation(const std::string& chain, uint64_t generation,
                                         bool valid) {
  if (generation != walletValidateGeneration_) return;  // the box moved on
  if (chain == urnet::SOL) {
    walletValidSol_ = valid;
  } else if (chain == urnet::MATIC) {
    walletValidMatic_ = valid;
  } else if (chain == urnet::TAO) {
    walletValidTao_ = valid;
  }
  // precedence SOL > MATIC > TAO: the first chain that accepted wins
  if (walletValidSol_) {
    walletChain_ = urnet::SOL;
  } else if (walletValidMatic_) {
    walletChain_ = urnet::MATIC;
  } else if (walletValidTao_) {
    walletChain_ = urnet::TAO;
  } else {
    walletChain_.clear();
  }
  connectWalletButton_->set_sensitive(!walletChain_.empty() && !connectingWallet_);
  if (walletChain_ == urnet::TAO) {
    // TAO can be connected and can NEVER pay out: say so at the point of entry
    kit::SetTextOrCollapse(
        *walletChainText_,
        T_("bittensor_wallet_future_use",
           "Bittensor wallets are stored for future use and can't receive payouts yet."));
  } else if (!walletChain_.empty()) {
    kit::SetTextOrCollapse(*walletChainText_,
                           Format(T_("wallet_provider_lower", "{} wallet"),
                                  ChainDisplayName(walletChain_).raw()));
  } else {
    kit::SetTextOrCollapse(*walletChainText_, {});
  }
}

void EarningsPage::OnConnectWallet() {
  const std::string address = walletAddressBox_->get_text().raw();
  if (address.empty() || walletChain_.empty() || connectingWallet_) return;
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }
  connectingWallet_ = true;
  connectWalletButton_->set_sensitive(false);
  const uint32_t generation = BeginFlow(connectFlow_, kApiTimeoutMs, [this] {
    connectingWallet_ = false;
    connectWalletButton_->set_sensitive(!walletChain_.empty());
    Notify(T_("wallet_connect_failed", "Failed to connect the wallet."),
           kit::Snackbar::Severity::Error);
  });

  urnet::CreateAccountWalletArgs args;
  args.blockchain = walletChain_;
  args.wallet_address = address;
  args.default_token_type = "USDC";
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().createAccountWallet(
      args, [this, epoch, seen, generation](
                std::optional<urnet::CreateAccountWalletResult> result,
                std::optional<std::string> err) {
        // success = a result carrying a NON-EMPTY wallet id
        const bool ok = !err.has_value() && result.has_value() && result->wallet_id &&
                        !result->wallet_id->empty();
        PostToMain([this, epoch, seen, generation, ok, detail = err.value_or(std::string())] {
          if (*epoch != seen) return;
          ApplyWalletConnectResult(generation, ok, detail);
        });
      });
}

void EarningsPage::ApplyWalletConnectResult(uint32_t generation, bool ok,
                                            const std::string& serverError) {
  if (!SettleFlow(connectFlow_, generation, "wallet connect")) return;
  connectingWallet_ = false;
  if (ok) {
    Notify(T_("wallet_connected", "Wallet connected."), kit::Snackbar::Severity::Success);
    walletAddressBox_->set_text("");  // resets the verdict through TextChanged
    LoadWallet();
    return;
  }
  // the raw server error VERBATIM when there is one (unlocalizable, and often
  // the only diagnostic); Error severity persists until dismissed
  Notify(serverError.empty()
             ? Glib::ustring(T_("wallet_connect_failed", "Failed to connect the wallet."))
             : Glib::ustring(serverError),
         kit::Snackbar::Severity::Error);
  connectWalletButton_->set_sensitive(!walletChain_.empty());
}

// ---- the seeker browser-bridge flow -----------------------------------------

void EarningsPage::OnVerifySeeker() {
  if (sheet_ || (sheet_open && sheet_open()) || verifyingSeeker_) return;
  // CanCallApi BEFORE the picker: the flow ends in an API write and opens a
  // browser on the way
  if (!CanCallApi()) {
    RefuseNoSession();
    return;
  }

  auto picker = std::make_shared<Gtk::Window>();
  picker->set_title(T_("confirm_seeker_token", "Confirm Seeker Token"));
  picker->set_default_size(400, -1);
  picker->set_resizable(false);
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  column->set_margin(24);
  column->append(*MakeWrappedNote(
      T_("connect_seeker_wallet",
         "Connect a wallet with the Saga Genesis or Seeker Pre-Order Token"),
      "ur-key"));
  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  auto* solflare = Gtk::make_managed<Gtk::Button>(T_("solflare", "Solflare"));
  auto* phantom = Gtk::make_managed<Gtk::Button>(T_("phantom", "Phantom"));
  phantom->add_css_class("suggested-action");  // the default button
  // hide first, then act: the dismissal cleanup is marshaled, so nothing is
  // destroyed inside its own signal
  cancel->signal_clicked().connect([picker] { picker->set_visible(false); });
  solflare->signal_clicked().connect([this, picker] {
    picker->set_visible(false);
    StartSeekerVerification(WalletConnect::Provider::Solflare);
  });
  phantom->signal_clicked().connect([this, picker] {
    picker->set_visible(false);
    StartSeekerVerification(WalletConnect::Provider::Phantom);
  });
  actions->append(*cancel);
  actions->append(*solflare);
  actions->append(*phantom);
  column->append(*actions);
  picker->set_child(*column);
  PresentSheet(picker);
}

void EarningsPage::StartSeekerVerification(WalletConnect::Provider provider) {
  verifyingSeeker_ = true;
  ApplySeekerState();
  // 180s, not 20s: WalletConnect reports errors only when a deep link comes
  // BACK, and a closed browser tab produces nothing, ever — this watchdog is
  // what un-bricks the button.
  const uint32_t generation = BeginFlow(seekerFlow_, kBridgeTimeoutMs, [this] {
    verifyingSeeker_ = false;
    ApplySeekerState();
    Notify(T_("error_claiming_multiplier", "Sorry, there was an error claiming multiplier."),
           kit::Snackbar::Severity::Error);
  });

  // replay-proof challenge (android parity)
  const std::string message =
      "Verify Seeker Token Holder - " + std::to_string(g_get_real_time() / 1000);
  (void)provider;
  (void)message;
  // TODO(sdk-wiring): SdkHost::SignWithSolanaWallet(provider, message, cb(ok,
  // address, signature, error)) — the ur.io/wallet-connect browser bridge plus
  // the urnetwork:// deep link. This host owns WalletConnect PRIVATELY and
  // exposes only SignInWithSolana, which AUTHENTICATES with the signature
  // instead of handing it back, so the challenge cannot be signed and
  // Api::verifySeekerHolder cannot be called. Nothing is faked and no request
  // is issued: the flow reports the real failure it is in, through the same
  // path a bridge error would take.
  g_warning("earnings: seeker verification unavailable — no wallet-signing host surface");
  ApplySeekerResult(generation, false, std::string());
}

void EarningsPage::ApplySeekerResult(uint32_t generation, bool ok,
                                     const std::string& serverError) {
  if (!SettleFlow(seekerFlow_, generation, "verification")) return;
  verifyingSeeker_ = false;
  if (ok) {
    Notify(T_("successfully_claimed_multiplier", "Successfully claimed multiplier!"),
           kit::Snackbar::Severity::Success);
    // has_seeker_token now reads true: the holder state and the 2x row appear
    LoadWallet();
    return;
  }
  Notify(serverError.empty()
             ? Glib::ustring(T_("error_claiming_multiplier",
                                "Sorry, there was an error claiming multiplier."))
             : Glib::ustring(Format(T_("error_claiming_multiplier_with_reason",
                                       "Sorry, there was an error claiming multiplier: {}"),
                                    serverError)),
         kit::Snackbar::Severity::Error);
  ApplySeekerState();
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
  if (requested == rankingPublic_) return;  // a no-op flip
  if (settingRankingPublic_) {
    SetRankingToggle(rankingPublic_);  // a set is already in flight
    return;
  }
  if (!CanCallApi()) {
    // this switch used to fire a real API write from preview builds
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
    SetRankingToggle(rankingPublic_);  // snap back to what the server holds
    Notify(serverError.empty()
               ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
               : Glib::ustring(serverError),
           kit::Snackbar::Severity::Error);
    return;
  }
  rankingPublic_ = requested;
  // the board ITSELF changes: our row masks or unmasks
  LoadLeaderboard();
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
      if (!*alive) return;  // the page is gone; nothing to clear
      CloseSheet();
    });
  });
  sheet->present();
}

void EarningsPage::CloseSheet() {
  if (!sheet_) return;
  sheet_.reset();
  if (on_sheet_open_changed) on_sheet_open_changed(false);
}

void EarningsPage::ShowWalletDetail(const urnet::AccountWallet& wallet) {
  if (sheet_ || (sheet_open && sheet_open())) {
    // a click that opens nothing stays a mystery: it is logged, never silent
    g_message("earnings: wallet detail suppressed — a modal is already open");
    return;
  }
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr) {
    g_warning("earnings: no window root; the wallet detail sheet was not opened");
    return;
  }
  const std::string walletId = wallet.wallet_id.value_or(std::string());
  urnet::AccountPaymentsList mine;
  for (const auto& payment : payments_) {
    if (payment.wallet_id.value_or(std::string()) == walletId) mine.push_back(payment);
  }
  // the sheet READS with no session (preview needs that); its two buttons are
  // the two API writes and are disabled without one
  auto sheet = std::make_shared<WalletDetailSheet>(
      *root, host_, wallet, !walletId.empty() && walletId == payoutWalletId_, mine,
      CanCallApi());
  sheet->on_changed = [this] { LoadWallet(); };  // RefreshAfterWalletChange
  sheet->on_success = [this](const Glib::ustring& message) {
    Notify(message, kit::Snackbar::Severity::Success);
  };
  PresentSheet(sheet);
}

void EarningsPage::ShowPayoutDetail(const urnet::AccountPayment& payment) {
  if (sheet_ || (sheet_open && sheet_open())) {
    g_message("earnings: payout detail suppressed — a modal is already open");
    return;
  }
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr) {
    g_warning("earnings: no window root; the payout detail sheet was not opened");
    return;
  }
  // the per-payment breakdown is computed HERE, from points already loaded
  const std::string paymentId = payment.payment_id.value_or(std::string());
  const PointsBreakdown breakdown = AggregatePoints(points_, &paymentId);
  PresentSheet(std::make_shared<PayoutDetailSheet>(*root, payment, breakdown, seekerHolder_));
}

// ---- preview sample ----------------------------------------------------------
// URNETWORK_PREVIEW_SAMPLE=1 on top of --preview-ui (BOTH gates; the window
// checks them). Obviously-synthetic rows flow through the SAME Apply*
// functions the server's answers do, so the preview exercises the real code
// path. The rows are INTERACTIVE, which is exactly why CanCallApi() gates the
// ACTIONS and not the loads.
void EarningsPage::ApplyPreviewSample() {
  g_warning("EarningsPage: preview sample pinned — wallet content is SYNTHETIC");
  samplePinned_ = true;

  auto wallet = [](const char* chain, const std::string& id, const std::string& address,
                   bool seeker) {
    urnet::AccountWallet out;
    out.wallet_id = id;
    out.blockchain = chain;
    out.wallet_address = address;
    out.default_token_type = "USDC";
    out.active = true;
    out.has_seeker_token = seeker;
    return out;
  };
  // deliberately readable nonsense, with a DISTINCT last six per chain so the
  // masking is visibly per-wallet and not one repeated string
  const std::string solAddress = "SAMPLEsampleSAMPLEsampleSAMPLESOL001";
  const std::string maticAddress = "0xSAMPLEsampleSAMPLEsampleSAMPLEMAT002";
  const std::string taoAddress = "SAMPLEsampleSAMPLEsampleSAMPLETAO003";
  urnet::AccountWalletsList sampleWallets;
  sampleWallets.push_back(wallet(urnet::SOL, "sample-wallet-sol", solAddress, true));
  sampleWallets.push_back(wallet(urnet::MATIC, "sample-wallet-matic", maticAddress, false));
  sampleWallets.push_back(wallet(urnet::TAO, "sample-wallet-tao", taoAddress, false));

  auto payment = [](const std::string& id, const std::string& walletId, const char* chain,
                    const std::string& address, double amount, bool completed,
                    const std::string& when, const std::string& hash) {
    urnet::AccountPayment out;
    out.payment_id = id;
    out.wallet_id = walletId;
    out.blockchain = chain;
    out.token_type = "USDC";
    out.token_amount = amount;
    out.wallet_address = address;
    out.completed = completed;
    out.create_time = when;
    if (completed) out.complete_time = when;
    if (!hash.empty()) out.tx_hash = hash;
    out.payout_byte_count = 3421000000;
    return out;
  };
  urnet::AccountPaymentsList samplePayments;
  samplePayments.push_back(payment("sample-payment-1", "sample-wallet-sol", urnet::SOL,
                                   solAddress, 0.0, false, "2026-08-09T00:00:00Z", ""));
  samplePayments.push_back(payment("sample-payment-2", "sample-wallet-sol", urnet::SOL,
                                   solAddress, 12.48, true, "2026-08-02T00:00:00Z",
                                   "SAMPLEtxSAMPLEtxSAMPLEtxSOLh01"));
  samplePayments.push_back(payment("sample-payment-3", "sample-wallet-matic", urnet::MATIC,
                                   maticAddress, 7.15, true, "2026-07-26T00:00:00Z",
                                   "0xSAMPLEtxSAMPLEtxSAMPLEtxMATh02"));

  auto point = [](const char* event, int64_t points, const std::string& paymentId) {
    urnet::AccountPoint out;
    out.event = event;
    out.point_value = points * 1000000;  // nano points: the helper divides by 1e6
    out.account_payment_id = paymentId;
    return out;
  };
  urnet::AccountPointsList samplePoints;
  samplePoints.push_back(point("payout", 1240, "sample-payment-2"));
  samplePoints.push_back(point("payout_linked_account", 310, "sample-payment-2"));
  samplePoints.push_back(point("payout_multiplier", 1240, "sample-payment-2"));
  samplePoints.push_back(point("payout_reliability", 96, "sample-payment-2"));
  samplePoints.push_back(point("payout", 705, "sample-payment-3"));
  samplePoints.push_back(point("payout_reliability", 48, "sample-payment-3"));

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
  sampleEarners.push_back(earner("sample-network-2", "sample-hidden", 3145728.f, false,
                                 false));  // must render "Private Network"
  sampleEarners.push_back(earner("sample-network-3", "sample-flagged", 2097152.f, true,
                                 true));  // must render "Private Network" too
  sampleEarners.push_back(earner("sample-network-own", "sample-your-network", 786432.f, true,
                                 false));  // the own row: lime + a fill step
  sampleEarners.push_back(earner("sample-network-4", "sample-omega", 524288.f, true, false));

  urnet::NetworkRanking ranking;
  ranking.leaderboard_rank = 42;
  ranking.net_mib_count = 786432.f;  // 768 GiB
  ranking.leaderboard_public = true;

  ApplyWallets(sampleWallets, Fetch::Ready);
  ApplyPayoutWalletId("sample-wallet-sol");
  ApplyTransferStats(true, 41231686042);  // ~38.4 GiB
  ApplyWalletBalance(true, 1948000000);
  ApplyReferrals(true, 7);
  ApplyPoints(samplePoints, Fetch::Ready);
  ApplyReliability(window, Fetch::Ready);
  ApplyPayments(samplePayments, Fetch::Ready);
  ApplyRanking(ranking, true);
  ApplyLeaderboard(sampleEarners, Fetch::Ready);
  leaderboardRequested_ = true;  // the sample IS the leaderboard answer
}

}  // namespace urnw
