// SPDX-License-Identifier: MPL-2.0
#include "PlanPicker.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <graphene.h>

#include "CryptoPay.hpp"  // YearlyCardLines
#include "I18n.hpp"
#include "OfferCard.hpp"
#include "PaneKit.hpp"
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

// A plan card's accessible name: its visible lines in order, the empty ones
// left out. A card's content is a box, which GTK names from nothing.
Glib::ustring PlanCardName(std::initializer_list<Glib::ustring> lines) {
  Glib::ustring name;
  for (const Glib::ustring& line : lines) {
    if (line.empty()) continue;
    if (!name.empty()) name += ", ";
    name += line;
  }
  return name;
}

// The two cards are a radio pair to assistive tech: the selected one is
// checked.
void SetPlanCardChecked(Gtk::Widget& card, bool checked) {
  gtk_accessible_update_state(GTK_ACCESSIBLE(card.gobj()), GTK_ACCESSIBLE_STATE_CHECKED,
                              checked ? GTK_ACCESSIBLE_TRISTATE_TRUE
                                      : GTK_ACCESSIBLE_TRISTATE_FALSE,
                              -1);
}

}  // namespace

// ---------------------------------------------------------------------------
// The recommended plan card in the Pro-gold dress: a breathing halo, a black
// ground with a gold wash, and a gold border with a light running around it.
class GoldPlanCard : public Gtk::Overlay {
 public:
  GoldPlanCard() {
    // A radio to assistive tech, as the monthly card is. The role can only be
    // set before the card's accessible state is first touched.
    g_object_set(gobj(), "accessible-role", GTK_ACCESSIBLE_ROLE_RADIO, nullptr);
    EnsureOnboardingCss();
    add_css_class("ur-onb-gold-card");
    set_overflow(Gtk::Overflow::VISIBLE);
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    content->set_margin(22);
    // taller than wide: the plan lines get room to breathe (android
    // PlanOptionContainer, vertical 36); the text sits level with the dot
    content->set_margin_top(36);
    content->set_margin_bottom(36);
    dot_.set_valign(Gtk::Align::CENTER);
    content->append(dot_);
    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    column->set_hexpand(true);
    column->set_valign(Gtk::Align::CENTER);
    price_.add_css_class("ur-onb-neuebit");
    price_.set_xalign(0);
    column->append(price_);
    saving_.add_css_class("ur-onb-body");
    saving_.add_css_class("ur-onb-muted");
    saving_.set_xalign(0);
    column->append(saving_);
    // the per-month equivalent: a sub-line under the billed amount, only when
    // the presentation rules show one (PricePresentation.hpp)
    equivalent_.add_css_class("ur-onb-body");
    equivalent_.add_css_class("ur-onb-muted");
    equivalent_.set_xalign(0);
    equivalent_.set_visible(false);
    column->append(equivalent_);
    trial_.add_css_class("ur-onb-body");
    trial_.add_css_class("ur-onb-gold-light");
    trial_.set_xalign(0);
    column->append(trial_);
    content->append(*column);
    set_child(*content);
    kit::MarkDecorative(*content);  // the card's name reads the lines

    pill_.set_text(T_("best_value", "Best value"));
    pill_.add_css_class("ur-onb-pill-gold");
    pill_.set_halign(Gtk::Align::END);
    pill_.set_valign(Gtk::Align::START);
    pill_.set_margin_end(12);
    pill_.set_margin_top(-14);
    add_overlay(pill_);
    set_clip_overlay(pill_, false);
    kit::MarkDecorative(pill_);

    click_ = Gtk::GestureClick::create();
    click_->signal_released().connect([this](int, double, double) {
      if (on_select) on_select();
    });
    add_controller(click_);
    SetPointerCursor(*this);
    // In the tab order, and Enter or Space picks the plan, as on the monthly
    // card's button.
    set_focusable(true);
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
          switch (keyval) {
            case GDK_KEY_Return:
            case GDK_KEY_KP_Enter:
            case GDK_KEY_ISO_Enter:
            case GDK_KEY_space:
            case GDK_KEY_KP_Space:
              if (on_select) on_select();
              return true;
            default:
              return false;
          }
        },
        false);
    add_controller(keys);

    if (motion::ShouldAnimate()) {
      start_ = Now();
      mapped_ = signal_map().connect([this] { StartTick(); });
      unmapped_ = signal_unmap().connect([this] { StopTick(); });
    }
    SetSelected(true);
  }
  ~GoldPlanCard() override {
    mapped_.disconnect();
    unmapped_.disconnect();
    StopTick();
  }

  // `savingRenews`: the line under the price is the offer's renewal price
  // ("then $40/year"), not "Save N%".
  void SetTexts(const Glib::ustring& price, const Glib::ustring& saving,
                const Glib::ustring& equivalent, const Glib::ustring& trial, bool savingRenews) {
    price_.set_text(price);
    savingText_ = saving;
    savingRenews_ = savingRenews;
    equivalentText_ = equivalent;
    trial_.set_text(trial);
    Layout();
  }
  // Only a card subscription starts with the free trial and renews (CONTRACT
  // §6.1): for a coin the trial line and the offer's renewal line go.
  void SetCardTermsShown(bool shown) {
    cardTerms_ = shown;
    Layout();
  }
  void SetSelected(bool selected) {
    selected_ = selected;
    SetPlanCardChecked(*this, selected);
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
  // The halo and the running light tick only while the card is on screen,
  // from map to unmap (the sheet or the page closed, the window hidden). A
  // tick left running kept its window's frame clock busy for the life of the
  // process.
  void StartTick() {
    if (tick_) return;
    tick_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
      const double elapsed = Now() - start_;
      sweep_ = std::fmod(elapsed, 3600.0) / 3600.0;
      breath_ = 0.5 + 0.5 * std::sin(elapsed / 2200.0 * kPi);
      queue_draw();
      return true;
    });
  }
  void StopTick() {
    if (!tick_) return;
    remove_tick_callback(tick_);
    tick_ = 0;
  }

  // The lines under the price for the selected method
  // (crypto_pay::YearlyCardLines), filled into the card's rows from the top.
  // A row left with nothing to say keeps its card text, transparent, as the
  // trial line does: the card keeps its height and the two cards do not jump
  // as the method changes. The card's name (its content is decorative) reads
  // only what shows.
  void Layout() {
    saving_.set_visible(!savingText_.empty());
    equivalent_.set_visible(!equivalentText_.empty());
    const std::vector<std::string> lines = crypto_pay::YearlyCardLines(
        savingText_.raw(), savingRenews_, equivalentText_.raw(), !cardTerms_);
    size_t next = 0;
    for (const auto& [row, cardText] :
         {std::pair<Gtk::Label*, const Glib::ustring*>{&saving_, &savingText_},
          std::pair<Gtk::Label*, const Glib::ustring*>{&equivalent_, &equivalentText_}}) {
      if (!row->get_visible()) continue;
      if (next < lines.size()) {
        row->set_text(lines[next++]);
        row->set_opacity(1.0);
      } else {
        row->set_text(*cardText);
        row->set_opacity(0.0);
      }
    }
    trial_.set_opacity(cardTerms_ ? 1.0 : 0.0);
    kit::SetAccessibleLabel(*this, PlanCardName({price_.get_text(),
                                                 0 < lines.size() ? lines[0] : std::string(),
                                                 1 < lines.size() ? lines[1] : std::string(),
                                                 cardTerms_ ? trial_.get_text() : Glib::ustring(),
                                                 pill_.get_text()}));
  }

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
      cr->set_line_width(ringWidth + 0.5);
      RoundedRectPath(cr, -distance, -distance, w + 2 * distance, h + 2 * distance, radius + distance);
      cr->set_source_rgba(kProGold.r, kProGold.g, kProGold.b, peak * fade);
      if (selected_) {
        // the selection pink laid over the gold at the same alpha: the glow
        // reads as gold and purple mixed, so the selection language survives
        // the gold dress instead of only the dot changing
        cr->stroke_preserve();
        cr->set_source_rgba(kUrPink.r, kUrPink.g, kUrPink.b, peak * fade);
      }
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
    if (selected_) {
      // a slight purple tint over the gold ground while selected
      RoundedRectPath(cr, 0, 0, w, h, radius);
      cr->set_source_rgba(kUrPink.r, kUrPink.g, kUrPink.b, 0.10);
      cr->fill();
    }
    // the border, and the light running around it
    const double inset = 1;
    RoundedRectPath(cr, inset, inset, w - 2 * inset, h - 2 * inset, radius);
    // selected: an even gold-purple mix so the border carries the selection
    // colour too; unselected: the gold dress alone, dimmed
    const Rgba border = selected_ ? Rgba{(kProGold.r + kUrPink.r) / 2, (kProGold.g + kUrPink.g) / 2,
                                         (kProGold.b + kUrPink.b) / 2, 1.0}
                                  : kProGold;
    cr->set_source_rgba(border.r, border.g, border.b, selected_ ? 1 : 0.7);
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
  Gtk::Label equivalent_;
  Gtk::Label trial_;
  Gtk::Label pill_;
  Glib::RefPtr<Gtk::GestureClick> click_;
  bool selected_ = true;
  // what the card says for the card method (Layout chooses what shows)
  Glib::ustring savingText_;
  Glib::ustring equivalentText_;
  bool savingRenews_ = false;
  bool cardTerms_ = true;  // a card selection: trial and renewal lines show
  double sweep_ = 0.25;
  double breath_ = 0.5;
  double start_ = 0;
  guint tick_ = 0;
  sigc::connection mapped_;
  sigc::connection unmapped_;
};


// ---------------------------------------------------------------------------

PlanPicker::PlanPicker() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
  EnsureOnboardingCss();
  set_overflow(Gtk::Overflow::VISIBLE);  // the gold card's halo spills past the box

  // the plan cards: annual in the gold dress, monthly plain. The prices come
  // from the server's price tier (SetPrices, fed by the balance store); the
  // picker starts on the standard tier so it never prints nothing.
  yearlyCard_ = Gtk::make_managed<GoldPlanCard>();
  yearlyCard_->on_select = [this] {
    Select(true);
    if (on_select) on_select(true);
  };
  append(*yearlyCard_);

  // a radio to assistive tech, as the yearly card is; the role is
  // construct-only, so the button comes from the C constructor
  monthlyCard_ = Gtk::manage(Glib::wrap(GTK_BUTTON(
      g_object_new(GTK_TYPE_BUTTON, "accessible-role", GTK_ACCESSIBLE_ROLE_RADIO, nullptr))));
  monthlyCard_->add_css_class("ur-onb-plan");
  monthlyCard_->set_margin_top(16);
  auto* monthlyRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  monthlyDot_ = Gtk::make_managed<Gtk::Label>();
  monthlyDot_->set_valign(Gtk::Align::CENTER);
  monthlyRow->append(*monthlyDot_);
  // The monthly card has two lines. Size it like the yearly card (an invisible
  // copy of that card's lines, never read aloud) so both cards are the same
  // height at any text scale, and center the visible lines in that space so
  // they sit level with the dot (android SubscriptionOptions).
  auto* monthlyReserve = Gtk::make_managed<Gtk::Overlay>();
  monthlyReserve->set_hexpand(true);
  auto* reserved = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  reserved->set_opacity(0);
  reserved->set_can_target(false);
  reserved->set_can_focus(false);
  gtk_accessible_update_state(GTK_ACCESSIBLE(reserved->gobj()), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
  const char* reservedClasses[3][2] = {{"ur-onb-neuebit", nullptr},
                                       {"ur-onb-body", "ur-onb-muted"},
                                       {"ur-onb-body", "ur-onb-gold-light"}};
  for (int i = 0; i < 3; ++i) {
    auto* line = Gtk::make_managed<Gtk::Label>("");
    for (const char* css : reservedClasses[i]) {
      if (css) line->add_css_class(css);
    }
    line->set_xalign(0);
    reserved->append(*line);
    reservedLines_.push_back(line);
  }
  monthlyReserve->set_child(*reserved);
  auto* monthlyColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  monthlyColumn->set_halign(Gtk::Align::START);
  monthlyColumn->set_valign(Gtk::Align::CENTER);
  monthlyText_ = Gtk::make_managed<Gtk::Label>("");
  monthlyText_->add_css_class("ur-onb-neuebit");
  monthlyText_->set_xalign(0);
  monthlyColumn->append(*monthlyText_);
  monthlyLine_ = Gtk::make_managed<Gtk::Label>("");
  monthlyLine_->add_css_class("ur-onb-body");
  monthlyLine_->add_css_class("ur-onb-muted");
  monthlyLine_->set_xalign(0);
  monthlyColumn->append(*monthlyLine_);
  // while a coin is the payment method: monthly is card only
  monthlyNote_ = Gtk::make_managed<Gtk::Label>(
      T_("pay_method_card_only_monthly", "Monthly is card only"));
  monthlyNote_->add_css_class("ur-onb-body");
  monthlyNote_->add_css_class("ur-onb-muted");
  monthlyNote_->set_xalign(0);
  monthlyNote_->set_visible(false);
  monthlyColumn->append(*monthlyNote_);
  monthlyReserve->add_overlay(*monthlyColumn);
  monthlyRow->append(*monthlyReserve);
  monthlyCard_->set_child(*monthlyRow);
  kit::MarkDecorative(*monthlyRow);  // the card's name reads the lines
  monthlyCard_->signal_clicked().connect([this] {
    Select(false);
    if (on_select) on_select(false);
  });
  append(*monthlyCard_);

  SetPrices(PriceTierView{}, OfferView{});
  Paint();
}

void PlanPicker::SetPrices(const PriceTierView& tier, const OfferView& offer) {
  SetTexts(ComposePlanCardTexts(tier, offer, kFreeTrialDays));
}

void PlanPicker::SetTexts(const PlanCardTexts& texts) {
  yearlyCard_->SetTexts(texts.yearlyPrice, texts.yearlySecondary, texts.yearlyEquivalent,
                        texts.yearlyTrial, texts.yearlySecondaryRenews);
  // the invisible copy that sizes the monthly card like the yearly one
  if (reservedLines_.size() == 3) {
    reservedLines_[0]->set_text(texts.yearlyPrice);
    reservedLines_[1]->set_text(texts.yearlySecondary.empty() ? texts.yearlyEquivalent
                                                              : texts.yearlySecondary);
    reservedLines_[2]->set_text(texts.yearlyTrial);
  }
  monthlyText_->set_text(texts.monthlyPrice);
  monthlyLine_->set_text(texts.monthlyLine);
  monthlyLine_->set_visible(monthlyAllowed_ && !texts.monthlyLine.empty());
  kit::SetAccessibleLabel(*monthlyCard_, PlanCardName({texts.monthlyPrice, texts.monthlyLine}));
  // greyed for a coin: the name says why, in place of its card terms
  // (SetMonthlyAllowed)
  if (!monthlyAllowed_) {
    kit::SetAccessibleLabel(*monthlyCard_,
                            PlanCardName({texts.monthlyPrice, monthlyNote_->get_text()}));
  }
}

void PlanPicker::SetMonthlyAllowed(bool allowed) {
  monthlyAllowed_ = allowed;
  // insensitive: greyed, out of the tab order, and no click selects it
  monthlyCard_->set_sensitive(allowed);
  // For a coin the card says why it is greyed, in place of its card terms:
  // "Billed monthly · cancel anytime" is renewal wording (CONTRACT §6.1). The
  // card's height is the reserved copy's, so the swap does not move it.
  monthlyLine_->set_visible(allowed && !monthlyLine_->get_text().empty());
  monthlyNote_->set_visible(!allowed);
  kit::SetAccessibleLabel(*monthlyCard_,
                          PlanCardName({monthlyText_->get_text(),
                                        allowed ? monthlyLine_->get_text() : monthlyNote_->get_text()}));
}

void PlanPicker::SetCardTermsShown(bool shown) { yearlyCard_->SetCardTermsShown(shown); }

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
    SetPlanCardChecked(*monthlyCard_, !yearly_);
  }
  if (monthlyDot_) {
    const Rgba& color = yearly_ ? kUrTextMuted : kUrPink;
    monthlyDot_->set_markup("<span foreground='" + HexForMarkup(color) + "' size='" +
                            std::to_string(14 * PANGO_SCALE) + "'>" + (yearly_ ? "○" : "●") +
                            "</span>");
  }
}

}  // namespace urnw
