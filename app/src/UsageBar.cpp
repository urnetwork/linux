// SPDX-License-Identifier: MPL-2.0
#include "UsageBar.hpp"

#include <algorithm>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr double kBarHeight = 32.0;   // mac chart frame height
constexpr double kBarRadius = 12.0;   // mac cornerRadius
constexpr double kMinFraction = 0.015;  // mac minNonZeroValue: 1.5% floor

// series colors (mac chartForegroundStyleScale: Used electric blue, Pending
// coral, Available faint)
const Rgba& UsedColor() { return kUrElectricBlue; }
const Rgba& PendingColor() { return kUrCoral; }
const Rgba& AvailableColor() { return kUrTextFaint; }

// legend dot + muted label
Gtk::Box* MakeLegendKey(const std::string& label, const Rgba& color) {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
  auto* dot = Gtk::make_managed<Gtk::Label>();
  dot->set_markup("<span foreground='" + HexForMarkup(color) + "'>●</span>");
  dot->add_css_class("ur-caption-11");
  dot->set_valign(Gtk::Align::CENTER);
  box->append(*dot);
  auto* text = Gtk::make_managed<Gtk::Label>(label);
  text->add_css_class("dim-label");
  text->add_css_class("caption");
  box->append(*text);
  return box;
}

}  // namespace

UsageBar::UsageBar() : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
  EnsureDrawerCss();

  bar_.set_content_height(static_cast<int>(kBarHeight));
  bar_.set_hexpand(true);
  bar_.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    DrawBar(cr, w, h);
  });
  append(bar_);

  // legend (android port parity; the mac SwiftUI chart auto-generates it)
  auto* legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  legend->append(*MakeLegendKey(T_("used_data_key", "Used"), UsedColor()));
  legend->append(*MakeLegendKey(T_("pending_data_key", "Pending"), PendingColor()));
  legend->append(*MakeLegendKey(T_("available_data_key", "Available"), AvailableColor()));
  append(*legend);

  // daily data balance
  auto* dailyRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* dailyLabel =
      Gtk::make_managed<Gtk::Label>(T_("daily_data_balance_label", "Daily Data Balance:"));
  dailyLabel->add_css_class("dim-label");
  dailyLabel->set_xalign(0);
  dailyLabel->set_hexpand(true);
  dailyRow->append(*dailyLabel);
  dailyBalanceValue_ = Gtk::make_managed<Gtk::Label>();
  dailyBalanceValue_->add_css_class("dim-label");
  dailyRow->append(*dailyBalanceValue_);
  append(*dailyRow);

  append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  // referrals: every referral adds 30 GiB/month to the daily balance
  auto* referralRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  referralCount_ = Gtk::make_managed<Gtk::Label>();
  referralCount_->add_css_class("dim-label");
  referralCount_->set_xalign(0);
  referralCount_->set_hexpand(true);
  referralRow->append(*referralCount_);
  referralBonus_ = Gtk::make_managed<Gtk::Label>();
  referralBonus_->add_css_class("dim-label");
  referralRow->append(*referralBonus_);
  append(*referralRow);

  SetData(0, 0, 0, 0, 0);
}

void UsageBar::SetData(int64_t usedByteCount, int64_t pendingByteCount,
                       int64_t availableByteCount, int64_t dailyBalanceByteCount,
                       int64_t totalReferrals) {
  used_ = std::max<int64_t>(0, usedByteCount);
  pending_ = std::max<int64_t>(0, pendingByteCount);
  available_ = std::max<int64_t>(0, availableByteCount);
  bar_.queue_draw();

  dailyBalanceValue_->set_text(FormatByteCountCompact(dailyBalanceByteCount));
  referralCount_->set_text(
      Format(T_("total_referral_count", "Total referrals: {}"), totalReferrals));
  referralBonus_->set_text(Format(T_("referral_bonus", "+{} GiB/Month"), totalReferrals * 30));
}

void UsageBar::DrawBar(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
  const double w = width;
  const double h = std::min<double>(height, kBarHeight);
  const double r = std::min(kBarRadius, h / 2.0);

  // clip to the rounded capsule, then fill the three segments left to right
  cr->begin_new_path();
  cr->arc(r, r, r, G_PI, 3 * G_PI / 2);
  cr->arc(w - r, r, r, 3 * G_PI / 2, 2 * G_PI);
  cr->arc(w - r, h - r, r, 0, G_PI / 2);
  cr->arc(r, h - r, r, G_PI / 2, G_PI);
  cr->close_path();
  cr->clip();

  const double total = static_cast<double>(used_ + pending_ + available_);
  if (total <= 0) {
    // empty state: a faint full-width track
    const Rgba& c = kUrTextFaint;
    cr->set_source_rgba(c.r, c.g, c.b, 0.35);
    cr->paint();
    return;
  }

  // non-zero segments get a 1.5% floor so they stay visible (mac parity)
  double fractions[3] = {used_ / total, pending_ / total, available_ / total};
  const Rgba colors[3] = {UsedColor(), PendingColor(), AvailableColor()};
  double sum = 0;
  for (double& f : fractions) {
    if (f > 0 && f < kMinFraction) f = kMinFraction;
    sum += f;
  }
  double x = 0;
  for (int i = 0; i < 3; ++i) {
    const double segment = w * (fractions[i] / sum);
    if (segment <= 0) continue;
    cr->set_source_rgba(colors[i].r, colors[i].g, colors[i].b, colors[i].a);
    // overshoot the right edge a hair to avoid seams from rounding
    cr->rectangle(x, 0, segment + 0.5, h);
    cr->fill();
    x += segment;
  }
}

}  // namespace urnw
