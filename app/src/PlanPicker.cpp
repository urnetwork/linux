// SPDX-License-Identifier: MPL-2.0
#include "PlanPicker.hpp"

#include <algorithm>
#include <cmath>

#include <graphene.h>

#include "I18n.hpp"
#include "ReferralPanel.hpp"  // EnsureOnboardingCss
#include "Ui.hpp"
#include "UrMotion.hpp"

namespace urnw {
namespace {

constexpr double kPi = 3.14159265358979323846;

void RoundedRectPath(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w,
                     double h, double r) {
  r = std::min(r, std::min(w, h) / 2);
  cr->begin_new_sub_path();
  cr->arc(x + w - r, y + r, r, -kPi / 2, 0);
  cr->arc(x + w - r, y + h - r, r, 0, kPi / 2);
  cr->arc(x + r, y + h - r, r, kPi / 2, kPi);
  cr->arc(x + r, y + r, r, kPi, 3 * kPi / 2);
  cr->close_path();
}

// A point at parameter t (0..1) along the perimeter of a rounded rect,
// starting at the top-left corner's end and going clockwise.
void PointOnRoundedRect(double w, double h, double r, double t, double& px, double& py) {
  r = std::min(r, std::min(w, h) / 2);
  const double straightW = w - 2 * r;
  const double straightH = h - 2 * r;
  const double arc = kPi * r / 2;
  const double total = 2 * straightW + 2 * straightH + 4 * arc;
  double d = std::fmod(t, 1.0) * total;
  if (d < 0) d += total;
  // top edge
  if (d < straightW) { px = r + d; py = 0; return; }
  d -= straightW;
  if (d < arc) { const double a = -kPi / 2 + d / r; px = w - r + r * std::cos(a); py = r + r * std::sin(a); return; }
  d -= arc;
  if (d < straightH) { px = w; py = r + d; return; }
  d -= straightH;
  if (d < arc) { const double a = d / r; px = w - r + r * std::cos(a); py = h - r + r * std::sin(a); return; }
  d -= arc;
  if (d < straightW) { px = w - r - d; py = h; return; }
  d -= straightW;
  if (d < arc) { const double a = kPi / 2 + d / r; px = r + r * std::cos(a); py = h - r + r * std::sin(a); return; }
  d -= arc;
  if (d < straightH) { px = 0; py = h - r - d; return; }
  d -= straightH;
  const double a = kPi + d / r;
  px = r + r * std::cos(a);
  py = r + r * std::sin(a);
}

double Now() { return g_get_monotonic_time() / 1000.0; }  // ms

}  // namespace

// ---------------------------------------------------------------------------
// The recommended plan card in the Pro-gold dress: a breathing halo, a black
// ground with a gold wash, and a gold border with a light running around it.
class GoldPlanCard : public Gtk::Overlay {
 public:
  GoldPlanCard() {
    EnsureOnboardingCss();
    set_overflow(Gtk::Overflow::VISIBLE);
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    content->set_margin(22);
    content->set_margin_top(24);
    content->set_margin_bottom(24);
    dot_.set_valign(Gtk::Align::CENTER);
    content->append(dot_);
    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    column->set_hexpand(true);
    price_.add_css_class("ur-onb-neuebit");
    price_.set_xalign(0);
    column->append(price_);
    saving_.add_css_class("ur-onb-body");
    saving_.add_css_class("ur-onb-muted");
    saving_.set_xalign(0);
    column->append(saving_);
    trial_.add_css_class("ur-onb-body");
    trial_.add_css_class("ur-onb-gold-light");
    trial_.set_xalign(0);
    column->append(trial_);
    content->append(*column);
    set_child(*content);

    pill_.set_text(T_("best_value", "Best value"));
    pill_.add_css_class("ur-onb-pill-gold");
    pill_.set_halign(Gtk::Align::END);
    pill_.set_valign(Gtk::Align::START);
    pill_.set_margin_end(12);
    pill_.set_margin_top(-14);
    add_overlay(pill_);
    set_clip_overlay(pill_, false);

    click_ = Gtk::GestureClick::create();
    click_->signal_released().connect([this](int, double, double) {
      if (on_select) on_select();
    });
    add_controller(click_);
    SetPointerCursor(*this);

    if (motion::ShouldAnimate()) {
      start_ = Now();
      tick_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
        const double elapsed = Now() - start_;
        sweep_ = std::fmod(elapsed, 3600.0) / 3600.0;
        breath_ = 0.5 + 0.5 * std::sin(elapsed / 2200.0 * kPi);
        queue_draw();
        return true;
      });
    }
    SetSelected(true);
  }
  ~GoldPlanCard() override {
    if (tick_) remove_tick_callback(tick_);
  }

  void SetTexts(const Glib::ustring& price, const Glib::ustring& saving, const Glib::ustring& trial) {
    price_.set_text(price);
    saving_.set_text(saving);
    saving_.set_visible(!saving.empty());
    trial_.set_text(trial);
  }
  void SetSelected(bool selected) {
    selected_ = selected;
    dot_.set_markup("<span foreground='" + HexForMarkup(kProGold) + "' size='" +
                    std::to_string(14 * PANGO_SCALE) + "'>" + (selected ? "●" : "○") + "</span>");
    queue_draw();
  }
  std::function<void()> on_select;

 protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override {
    const double w = get_width();
    const double h = get_height();
    const double spill = 28;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(static_cast<float>(-spill), static_cast<float>(-spill),
                                                static_cast<float>(w + 2 * spill), static_cast<float>(h + 2 * spill));
    auto cr = snapshot->append_cairo(&bounds);
    Draw(cr, w, h);
    Gtk::Overlay::snapshot_vfunc(snapshot);
  }

 private:
  void Draw(const Cairo::RefPtr<Cairo::Context>& cr, double w, double h) {
    const double radius = 12;
    // the breathing halo: rings that follow the card's shape and fade out
    const double spill = 28;
    const int rings = 44;
    const double ringWidth = spill / rings;
    const double peak = 0.20 + 0.14 * breath_;
    for (int ring = 0; ring < rings; ++ring) {
      const double t = ring / (rings - 1.0);
      const double distance = ringWidth * (ring + 0.5);
      const double fade = (1 - t) * (1 - t);
      cr->set_source_rgba(kProGold.r, kProGold.g, kProGold.b, peak * fade);
      cr->set_line_width(ringWidth + 0.5);
      RoundedRectPath(cr, -distance, -distance, w + 2 * distance, h + 2 * distance, radius + distance);
      cr->stroke();
    }
    // opaque ground, then the gold wash brighter at the top left
    RoundedRectPath(cr, 0, 0, w, h, radius);
    cr->set_source_rgba(kUrBackground.r, kUrBackground.g, kUrBackground.b, 1);
    cr->fill_preserve();
    cr->set_source_rgba(kProGold.r, kProGold.g, kProGold.b, 0.08);
    cr->fill_preserve();
    auto wash = Cairo::RadialGradient::create(w * 0.1, 0, 0, w * 0.1, 0, w * 0.9);
    wash->add_color_stop_rgba(0, kProGold.r, kProGold.g, kProGold.b, 0.18);
    wash->add_color_stop_rgba(1, kProGold.r, kProGold.g, kProGold.b, 0);
    cr->set_source(wash);
    cr->fill();
    // the border, and the light running around it
    const double inset = 1;
    RoundedRectPath(cr, inset, inset, w - 2 * inset, h - 2 * inset, radius);
    cr->set_source_rgba(kProGold.r, kProGold.g, kProGold.b, selected_ ? 1 : 0.7);
    cr->set_line_width(2);
    cr->stroke_preserve();
    double lx = 0, ly = 0;
    PointOnRoundedRect(w - 2 * inset, h - 2 * inset, radius, sweep_, lx, ly);
    lx += inset;
    ly += inset;
    auto light = Cairo::RadialGradient::create(lx, ly, 0, lx, ly, 70);
    light->add_color_stop_rgba(0, 1, 1, 1, 1);
    light->add_color_stop_rgba(0.35, kProGoldLight.r, kProGoldLight.g, kProGoldLight.b, 0.9);
    light->add_color_stop_rgba(1, kProGold.r, kProGold.g, kProGold.b, 0);
    cr->set_source(light);
    cr->set_line_width(2.5);
    cr->stroke();
  }

  Gtk::Label dot_;
  Gtk::Label price_;
  Gtk::Label saving_;
  Gtk::Label trial_;
  Gtk::Label pill_;
  Glib::RefPtr<Gtk::GestureClick> click_;
  bool selected_ = true;
  double sweep_ = 0.25;
  double breath_ = 0.5;
  double start_ = 0;
  guint tick_ = 0;
};


// ---------------------------------------------------------------------------

PlanPicker::PlanPicker() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
  EnsureOnboardingCss();
  set_overflow(Gtk::Overflow::VISIBLE);  // the gold card's halo spills past the box

  // the plan cards: annual in the gold dress, monthly plain. The prices are
  // the Stripe prices as product literals in the store ($40 a year is a
  // third off twelve months at $5); the trial line is the yearly plan's only.
  yearlyCard_ = Gtk::make_managed<GoldPlanCard>();
  yearlyCard_->SetTexts(T_("plan_yearly_price", "$40/year"), T_("save_33_percent", "Save 33%"),
                        Format(T_("includes_free_trial_days", "Includes {} day free trial"),
                               kFreeTrialDays));
  yearlyCard_->on_select = [this] {
    Select(true);
    if (on_select) on_select(true);
  };
  append(*yearlyCard_);

  monthlyCard_ = Gtk::make_managed<Gtk::Button>();
  monthlyCard_->add_css_class("ur-onb-plan");
  monthlyCard_->set_margin_top(16);
  auto* monthlyRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  monthlyDot_ = Gtk::make_managed<Gtk::Label>();
  monthlyRow->append(*monthlyDot_);
  auto* monthlyText = Gtk::make_managed<Gtk::Label>(T_("plan_monthly_price", "$5/month"));
  monthlyText->add_css_class("ur-onb-neuebit");
  monthlyText->set_xalign(0);
  monthlyRow->append(*monthlyText);
  monthlyCard_->set_child(*monthlyRow);
  monthlyCard_->signal_clicked().connect([this] {
    Select(false);
    if (on_select) on_select(false);
  });
  append(*monthlyCard_);

  Paint();
}

void PlanPicker::Select(bool yearly) {
  yearly_ = yearly;
  Paint();
}

std::string PlanPicker::CtaLabel(bool yearly) {
  return yearly ? T_("start_free_trial", "Start free trial") : T_("subscribe", "Subscribe");
}

void PlanPicker::Paint() {
  if (yearlyCard_) yearlyCard_->SetSelected(yearly_);
  if (monthlyCard_) {
    if (yearly_) monthlyCard_->remove_css_class("selected");
    else monthlyCard_->add_css_class("selected");
  }
  if (monthlyDot_) {
    const Rgba& color = yearly_ ? kUrTextMuted : kUrPink;
    monthlyDot_->set_markup("<span foreground='" + HexForMarkup(color) + "' size='" +
                            std::to_string(14 * PANGO_SCALE) + "'>" + (yearly_ ? "○" : "●") +
                            "</span>");
  }
}

}  // namespace urnw
