#include "Onboarding.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include <graphene.h>

#include "AppPrefs.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "ReferralRoyalty.hpp"
#include "RuntimePaths.hpp"
#include "Ui.hpp"
#include "UrMotion.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

constexpr int kSheetWidth = 560;
constexpr int kSheetHeight = 800;
constexpr int kRouteHeight = 76;
constexpr double kRouteConnectorSize = 72;   // the mark in the route on page 1
constexpr double kHeaderConnectorSize = 34;  // the mark beside the bubbles after it
constexpr double kTravellerSize = 40;
constexpr int kTripMs = 6500;      // the docs' trip: 1% -> 94%, fading in and out
constexpr int kFlightMs = 520;     // route slot -> header slot
constexpr double kPi = 3.14159265358979323846;

void EnsureOnboardingCss() {
  static bool done = false;
  if (done) return;
  done = true;
  auto css = Gtk::CssProvider::create();
  css->load_from_data(R"css(
.ur-onb-title { font-family: "ABC Gravity Extended"; font-size: 30px; font-weight: 600; color: #F8F8F8; }
.ur-onb-lead { font-family: "PP NeueBit"; font-size: 22px; font-weight: bold; color: #F8F8F8; }
.ur-onb-neuebit { font-family: "PP NeueBit"; font-size: 24px; font-weight: bold; color: #F8F8F8; }
.ur-onb-neuebit-small { font-family: "PP NeueBit"; font-size: 18px; font-weight: bold; }
.ur-onb-body { font-family: "PP Neue Montreal"; font-size: 15px; color: #F8F8F8; }
.ur-onb-muted { color: #989898; }
.ur-onb-faint { color: #5A5A5A; }
.ur-onb-mono { font-family: monospace; font-size: 13px; }
.ur-onb-card { background-color: #1C1C1C; border-radius: 12px; }
.ur-onb-plan { border: 2px solid #989898; border-radius: 12px; padding: 20px; background-color: #101010; color: #F8F8F8; }
.ur-onb-plan.selected { border-color: #ED8FFF; }
.ur-onb-pill-gold { background: linear-gradient(#FFE082, #FFC400); color: #101010; border-radius: 16px; padding: 4px 14px; font-family: "PP NeueBit"; font-size: 18px; font-weight: bold; border: 1px solid alpha(#FFFFFF, 0.45); }
.ur-onb-gold { color: #FFC400; }
.ur-onb-gold-light { color: #FFE082; }
.ur-onb-ref-gold-light { color: #FFD76A; }
.ur-onb-kicker { color: #F5B93C; font-size: 11px; font-weight: bold; letter-spacing: 2px; }
.ur-onb-blue-light { color: alpha(#D6E6F4, 0.85); }
.ur-onb-blue-faint { color: alpha(#D6E6F4, 0.6); }
.ur-onb-code-pill { border: 1px dashed alpha(#F5B93C, 0.55); border-radius: 100px; background-color: alpha(#000000, 0.35); padding: 6px 6px 6px 18px; }
.ur-onb-code { color: #FFD76A; font-weight: bold; font-size: 16px; letter-spacing: 1px; }
button.ur-onb-gold-btn { background: linear-gradient(#FFE38A, #F5B93C); color: #241A05; border-radius: 100px; font-weight: bold; padding: 8px 18px; }
button.ur-onb-gold-btn:hover { background: linear-gradient(#FFEBA0, #F7C24F); }
button.ur-onb-link { color: #989898; background: none; border: none; box-shadow: none; }
button.ur-onb-link:hover { color: #C8C8C8; background: none; }
button.ur-onb-skip { color: #989898; font-size: 14px; background: none; border: none; box-shadow: none; padding: 4px 8px; }
button.ur-onb-skip:hover { color: #C8C8C8; background: none; }
.ur-onb-provide-title { font-family: "PP Neue Montreal"; font-size: 15px; color: #F8F8F8; }
.ur-onb-provide-desc { font-family: "PP Neue Montreal"; font-size: 12px; color: #989898; }
)css");
  Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(), css,
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

std::string UrPersonPath(int index) {
  const std::string name = "UrPerson" + std::to_string(index) + ".png";
  return ResolveRuntimePath(std::string(UR_PKGDATADIR "/") + name, G_FILE_TEST_IS_REGULAR,
                            "assets/" + name);
}

// The connector mark as a cairo surface (the launcher icon texture, downloaded).
Cairo::RefPtr<Cairo::ImageSurface> ConnectorSurface() {
  static Cairo::RefPtr<Cairo::ImageSurface> cached;
  static bool tried = false;
  if (tried) return cached;
  tried = true;
  auto texture = BrandLogoTexture();
  if (!texture) return cached;
  const int w = texture->get_width();
  const int h = texture->get_height();
  auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, w, h);
  surface->flush();
  gdk_texture_download(texture->gobj(), surface->get_data(), surface->get_stride());
  surface->mark_dirty();
  cached = surface;
  return cached;
}

// Whole gibibytes without decimals ("30 GiB"), anything else in the compact form.
std::string FormatDailyAllowance(int64_t byteCount) {
  const int64_t gib = int64_t(1024) * 1024 * 1024;
  if (0 < byteCount && byteCount % gib == 0) return std::to_string(byteCount / gib) + " GiB";
  return FormatByteCountCompact(byteCount);
}

Gtk::Label* MakeLabel(const Glib::ustring& text, const char* cssClass, bool wrap = true) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(wrap);
  label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  return label;
}

// The referral page's bullet (android BulletPoint): a green dot on the first
// line, the text left-aligned even when it wraps.
Gtk::Box* MakeBullet(const Glib::ustring& text) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  row->set_valign(Gtk::Align::START);
  auto* dot = Gtk::make_managed<Gtk::Label>();
  dot->set_markup("<span foreground='" + HexForMarkup(kUrGreen) + "'>●</span>");
  dot->set_valign(Gtk::Align::START);
  dot->set_margin_top(6);
  row->append(*dot);
  auto* label = MakeLabel(text, "ur-onb-neuebit-small");
  label->add_css_class("ur-onb-body");
  row->append(*label);
  return row;
}

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
// Step bubbles: the current step is a white pill, done steps dim white,
// upcoming faint. Read as "Step N of 4".
class StepBubbles : public Gtk::DrawingArea {
 public:
  StepBubbles() {
    set_content_width(kOnboardingSteps * 8 + 14 + (kOnboardingSteps - 1) * 6);
    set_content_height(8);
    set_valign(Gtk::Align::CENTER);
    set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int, int) { Draw(cr); });
    SetStep(1);
  }
  void SetStep(int step) {
    step_ = step;
    kit::SetAccessibleLabel(*this, Format(T_("onboarding_step_of", "Step {0} of {1}"), step,
                                          kOnboardingSteps));
    queue_draw();
  }

 private:
  void Draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    double x = 0;
    for (int i = 1; i <= kOnboardingSteps; ++i) {
      const bool current = i == step_;
      const double w = current ? 22 : 8;
      if (current) cr->set_source_rgba(1, 1, 1, 1);
      else if (i < step_) cr->set_source_rgba(1, 1, 1, 0.55);
      else cr->set_source_rgba(kUrTextFaint.r, kUrTextFaint.g, kUrTextFaint.b, 1);
      RoundedRectPath(cr, x, 0, w, 8, 4);
      cr->fill();
      x += w + 6;
    }
  }
  int step_ = 1;
};

// ---------------------------------------------------------------------------
// The route line: "You ─ [connector] ─ Internet" with one of the pixel
// ur-people walking it, trip after trip (the ur.io docs RouteLine).
class RouteLine : public Gtk::DrawingArea {
 public:
  RouteLine() {
    set_content_height(kRouteHeight);
    set_hexpand(true);
    for (int i = 1; i <= 3; ++i) {
      const std::string path = UrPersonPath(i);
      if (path.empty()) continue;
      try {
        people_.push_back(Gdk::Pixbuf::create_from_file(path));
      } catch (const Glib::Error&) {
      }
    }
    set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) { Draw(cr, w, h); });
    if (motion::ShouldAnimate()) {
      start_ = Now();
      tick_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
        const double elapsed = Now() - start_;
        const int trip = static_cast<int>(elapsed / kTripMs);
        if (trip != tripCount_) {
          tripCount_ = trip;
          NextPerson();
        }
        progress_ = std::fmod(elapsed, kTripMs) / kTripMs;
        queue_draw();
        return true;
      });
    } else {
      progress_ = 0.85;  // parked on the last leg, in view
    }
  }
  ~RouteLine() override {
    if (tick_) remove_tick_callback(tick_);
  }

  // whether the route draws the mark itself (the host draws it while flying)
  void SetDrawConnector(bool draw) {
    drawConnector_ = draw;
    queue_draw();
  }
  // the mark's slot, in the route's own coordinates
  void SlotRect(double& x, double& y, double& size) const {
    size = kRouteConnectorSize;
    x = (get_width() - size) / 2;
    y = (get_height() - size) / 2;
  }

 private:
  void NextPerson() {
    if (people_.size() < 2) return;
    if (deck_.empty()) {
      for (size_t i = 0; i < people_.size(); ++i) {
        if (static_cast<int>(i) != person_) deck_.push_back(static_cast<int>(i));
      }
      std::shuffle(deck_.begin(), deck_.end(), rng_);
    }
    person_ = deck_.back();
    deck_.pop_back();
  }

  void DrawStop(const Cairo::RefPtr<Cairo::Context>& cr, const Glib::ustring& text, double x,
                double centerY, const Rgba& color, bool alignEnd) {
    auto layout = create_pango_layout(text);
    layout->set_font_description(Pango::FontDescription("Monospace 12"));
    int tw = 0, th = 0;
    layout->get_pixel_size(tw, th);
    const double left = alignEnd ? x - tw : x;
    // the label sits on black so the dashes stop at its edge
    cr->set_source_rgba(kUrBackground.r, kUrBackground.g, kUrBackground.b, 1);
    cr->rectangle(left - 6, centerY - th / 2.0 - 2, tw + 12, th + 4);
    cr->fill();
    cr->set_source_rgba(color.r, color.g, color.b, 0.9);
    cr->move_to(left, centerY - th / 2.0);
    layout->show_in_cairo_context(cr);
  }

  void Draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const double centerY = h / 2.0;
    // the dashed route
    cr->save();
    cr->set_source_rgba(kUrPink.r, kUrPink.g, kUrPink.b, 0.4);
    cr->set_line_width(1);
    std::vector<double> dashes{6, 5};
    cr->set_dash(dashes, 0);
    cr->move_to(6, centerY + 0.5);
    cr->line_to(w - 6, centerY + 0.5);
    cr->stroke();
    cr->restore();

    // the stops
    DrawStop(cr, T_("route_you", "You"), 0, centerY, kUrLightBlue, false);
    DrawStop(cr, T_("route_internet", "Internet"), w, centerY, kUrText, true);

    // the connector, unless the host is flying it
    double sx = 0, sy = 0, size = 0;
    SlotRect(sx, sy, size);
    cr->set_source_rgba(kUrBackground.r, kUrBackground.g, kUrBackground.b, 1);
    cr->rectangle(sx, sy, size, size);
    cr->fill();
    if (drawConnector_) {
      if (auto surface = ConnectorSurface()) {
        cr->save();
        cr->translate(sx, sy);
        cr->scale(size / surface->get_width(), size / surface->get_height());
        cr->set_source(surface, 0, 0);
        cr->paint();
        cr->restore();
      }
    }

    // the traveller, in front of the mark
    if (people_.empty()) return;
    const auto& pixbuf = people_[std::clamp<int>(person_, 0, static_cast<int>(people_.size()) - 1)];
    const bool animate = motion::ShouldAnimate();
    const double t = progress_;
    const double position = animate ? 0.01 + 0.93 * t : 0.85;
    double alpha = 1;
    if (animate) {
      if (t < 0.04) alpha = t / 0.04;
      else if (t > 0.94) alpha = 0;
      else if (t > 0.88) alpha = 1 - (t - 0.88) / 0.06;
    }
    if (alpha <= 0) return;
    const double bob = animate ? std::sin(t * 2 * kPi * 9) : 0;
    const double travel = w - kTravellerSize;
    const double px = travel * position;
    const double py = centerY - kTravellerSize / 2 - 2 * (bob + 1) / 2;
    const double scale = kTravellerSize / pixbuf->get_width();
    cr->save();
    cr->translate(px + kTravellerSize / 2, py + kTravellerSize / 2);
    cr->rotate(4 * bob * kPi / 180);
    cr->translate(-kTravellerSize / 2, -kTravellerSize / 2);
    // the docs' soft white drop-shadow, traced on the tile's own outline
    const double layers[] = {0.10, 0.07, 0.05, 0.04, 0.03, 0.02};
    for (int i = 0; i < 6; ++i) {
      const double s = 1 + 0.035 * (i + 1);
      cr->save();
      cr->translate(kTravellerSize * (1 - s) / 2, kTravellerSize * (1 - s) / 2);
      cr->scale(scale * s, scale * s);
      Gdk::Cairo::set_source_pixbuf(cr, pixbuf, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr->cobj()), CAIRO_FILTER_NEAREST);
      auto mask = cr->get_source();
      cr->set_source_rgba(1, 1, 1, layers[i] * alpha);
      cr->mask(mask);
      cr->restore();
    }
    cr->save();
    cr->scale(scale, scale);
    Gdk::Cairo::set_source_pixbuf(cr, pixbuf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr->cobj()), CAIRO_FILTER_NEAREST);
    cr->paint_with_alpha(alpha);
    cr->restore();
    cr->restore();
  }

  std::vector<Glib::RefPtr<Gdk::Pixbuf>> people_;
  std::vector<int> deck_;
  std::mt19937 rng_{std::random_device{}()};
  int person_ = 0;
  int tripCount_ = 0;
  double progress_ = 0;
  double start_ = 0;
  guint tick_ = 0;
  bool drawConnector_ = true;
};

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
// The referral king-frog panel (android ReferralGoldPanel): a gold-washed
// surface with a pulsing aura, the frog, the copy, the code pill and share.
class ReferralPanel : public Gtk::Box {
 public:
  explicit ReferralPanel(Gtk::Window& owner)
      : Gtk::Box(Gtk::Orientation::VERTICAL, 0), owner_(owner) {
    EnsureOnboardingCss();
    set_overflow(Gtk::Overflow::VISIBLE);

    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    column->set_margin(22);
    column->set_margin_top(28);
    column->set_margin_bottom(28);
    column->set_halign(Gtk::Align::FILL);

    const std::string frog = ResolveRuntimePath(UR_PKGDATADIR "/ReferralFrog.png",
                                                G_FILE_TEST_IS_REGULAR, "assets/ReferralFrog.png");
    if (!frog.empty()) {
      auto* image = Gtk::make_managed<Gtk::Image>();
      image->set(frog);
      image->set_pixel_size(108);
      image->set_halign(Gtk::Align::CENTER);
      image->set_margin_bottom(12);
      column->append(*image);
    }
    auto* kicker = Gtk::make_managed<Gtk::Label>(Glib::ustring(T_("referrals", "Referrals")).uppercase());
    kicker->add_css_class("ur-onb-kicker");
    column->append(*kicker);
    heading_.add_css_class("ur-onb-neuebit");
    heading_.set_wrap(true);
    heading_.set_justify(Gtk::Justification::CENTER);
    column->append(heading_);
    detail_.add_css_class("ur-onb-body");
    detail_.add_css_class("ur-onb-blue-light");
    detail_.set_wrap(true);
    detail_.set_justify(Gtk::Justification::CENTER);
    detail_.set_margin_bottom(12);
    column->append(detail_);

    auto* codeLabel = Gtk::make_managed<Gtk::Label>(
        Glib::ustring(T_("your_referral_code", "Your referral code")).uppercase());
    codeLabel->add_css_class("ur-onb-kicker");
    codeLabel->add_css_class("ur-onb-blue-faint");
    column->append(*codeLabel);

    codePill_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    codePill_->add_css_class("ur-onb-code-pill");
    codePill_->set_halign(Gtk::Align::CENTER);
    codePill_->set_margin_top(2);
    code_.add_css_class("ur-onb-code");
    code_.set_selectable(true);
    codePill_->append(code_);
    copy_ = Gtk::make_managed<Gtk::Button>(T_("copy", "Copy"));
    copy_->add_css_class("ur-onb-gold-btn");
    copy_->signal_clicked().connect([this] {
      owner_.get_clipboard()->set_text(referralCode_);
      copy_->set_label(T_("copied", "Copied!"));
      Glib::signal_timeout().connect_once([this] { copy_->set_label(T_("copy", "Copy")); }, 1800);
    });
    codePill_->append(*copy_);
    column->append(*codePill_);

    share_ = Gtk::make_managed<Gtk::Button>(T_("share", "Share"));
    share_->add_css_class("ur-onb-gold-btn");
    share_->set_margin_top(10);
    share_->signal_clicked().connect([this] {
      owner_.get_clipboard()->set_text(Format(
          T_("referral_share_message",
             "Join me on URnetwork! Get the app and enter referral code {} when you sign up."),
          referralCode_));
      share_->set_label(T_("copied", "Copied!"));
      Glib::signal_timeout().connect_once([this] { share_->set_label(T_("share", "Share")); }, 1800);
    });
    column->append(*share_);

    status_.add_css_class("ur-onb-body");
    status_.set_wrap(true);
    status_.set_justify(Gtk::Justification::CENTER);
    status_.set_margin_top(14);
    column->append(status_);
    append(*column);

    if (motion::ShouldAnimate()) {
      start_ = Now();
      tick_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
        const double period = crowned_ ? 3400.0 : 5000.0;
        breath_ = 0.5 + 0.5 * std::sin((Now() - start_) / period * 2 * kPi);
        queue_draw();
        return true;
      });
    }
  }
  ~ReferralPanel() override {
    if (tick_) remove_tick_callback(tick_);
  }

  void Update(const std::string& referralCode, int64_t totalReferrals, const ReferralTerms& terms) {
    referralCode_ = referralCode;
    crowned_ = 0 < totalReferrals;
    heading_.set_text(crowned_ ? T_("referral_royalty", "You're referral royalty!")
                               : T_("referral_panel_heading",
                                    "Refer a friend and you both get free data"));
    detail_.set_text(Format(T_("referral_panel_detail",
                               "Every verified referral gives each of you {} GiB/day for free, for life."),
                            terms.bonusGibPerDay));
    code_.set_text(referralCode);
    codePill_->set_visible(!referralCode.empty());
    share_->set_visible(!referralCode.empty());
    if (crowned_) {
      const int64_t paid = std::min<int64_t>(totalReferrals, terms.maxReferrals <= 0 ? totalReferrals
                                                                                       : terms.maxReferrals);
      status_.set_text(Glib::ustring("👑 ") +
                       Format(TN_("referral_crowned_congrats",
                                  "A friend has joined — you're earning +{1} GiB/day, for life.",
                                  "{0} friends have joined — you're earning +{1} GiB/day, for life.",
                                  totalReferrals),
                              totalReferrals, paid * terms.bonusGibPerDay));
      status_.remove_css_class("ur-onb-blue-faint");
      status_.add_css_class("ur-onb-ref-gold-light");
    } else {
      status_.set_text(T_("referral_panel_none",
                          "No friends yet. Share your code and watch the crown appear. 👑"));
      status_.remove_css_class("ur-onb-ref-gold-light");
      status_.add_css_class("ur-onb-blue-faint");
    }
    queue_draw();
  }

 protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override {
    const double w = get_width();
    const double h = get_height();
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
    auto cr = snapshot->append_cairo(&bounds);
    Draw(cr, w, h);
    Gtk::Box::snapshot_vfunc(snapshot);
  }

 private:
  void Draw(const Cairo::RefPtr<Cairo::Context>& cr, double w, double h) {
    const double radius = 20;
    RoundedRectPath(cr, 0, 0, w, h, radius);
    cr->set_source_rgba(kUrBackground.r, kUrBackground.g, kUrBackground.b, 1);
    cr->fill_preserve();
    cr->set_source_rgba(kReferralGold.r, kReferralGold.g, kReferralGold.b, 0.04);
    cr->fill_preserve();
    auto wash = Cairo::RadialGradient::create(w * 0.12, 0, 0, w * 0.12, 0, w * 1.3);
    wash->add_color_stop_rgba(0, kReferralGold.r, kReferralGold.g, kReferralGold.b, 0.16);
    wash->add_color_stop_rgba(1, kReferralGold.r, kReferralGold.g, kReferralGold.b, 0);
    cr->set_source(wash);
    cr->fill_preserve();
    const double auraAlpha = 0.55 + 0.35 * breath_;
    const double auraRadius = std::max(w, h) * 0.7 * (1 + 0.06 * breath_);
    auto aura = Cairo::RadialGradient::create(w / 2.0, h / 2.0, 0, w / 2.0, h / 2.0, auraRadius);
    aura->add_color_stop_rgba(0, kReferralGold.r, kReferralGold.g, kReferralGold.b, 0.22 * auraAlpha);
    aura->add_color_stop_rgba(1, kReferralGold.r, kReferralGold.g, kReferralGold.b, 0);
    cr->set_source(aura);
    cr->fill();
    RoundedRectPath(cr, 0.5, 0.5, w - 1, h - 1, radius);
    cr->set_source_rgba(kReferralGold.r, kReferralGold.g, kReferralGold.b, crowned_ ? 0.75 : 0.4);
    cr->set_line_width(1);
    cr->stroke();
  }

  Gtk::Window& owner_;
  Gtk::Label heading_;
  Gtk::Label detail_;
  Gtk::Box* codePill_ = nullptr;
  Gtk::Label code_;
  Gtk::Button* copy_ = nullptr;
  Gtk::Button* share_ = nullptr;
  Gtk::Label status_;
  std::string referralCode_;
  bool crowned_ = false;
  double breath_ = 0.5;
  double start_ = 0;
  guint tick_ = 0;
};

// ---------------------------------------------------------------------------

OnboardingWindow::OnboardingWindow(Gtk::Window& parent, SdkHost& host,
                                   SubscriptionBalanceStore& balance)
    : host_(host), balance_(balance) {
  EnsureDrawerCss();
  EnsureOnboardingCss();
  set_title(T_("welcome_to_urnetwork", "Welcome to URnetwork"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(kSheetWidth, kSheetHeight);
  set_resizable(false);
  add_css_class("ur-sheet");
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  signal_hide().connect([this] {
    balancePoll_.disconnect();
    if (on_finished) on_finished();
  });
  BuildUi();
}

OnboardingWindow::~OnboardingWindow() {
  if (flightTick_) remove_tick_callback(flightTick_);
}

void OnboardingWindow::Open() {
  step_ = 1;
  connectorInHeader_ = false;
  connectorVisible_ = false;
  if (route_) route_->SetDrawConnector(true);
  ShowStep(1);
  balance_.FetchNow();
  RefreshBalance();
  RefreshReferral();
  balancePoll_.disconnect();
  balancePoll_ = Glib::signal_timeout().connect([this] {
    RefreshBalance();
    RefreshReferral();
    return true;
  }, 2000);
  present();
}

void OnboardingWindow::OpenAt(int step) {
  Open();
  if (1 < step) {
    Glib::signal_timeout().connect_once([this, step] { ShowStep(step); }, 400);
  }
}

void OnboardingWindow::Finish() {
  hide();
}

Gtk::Widget* OnboardingWindow::WrapPage(Gtk::Widget& page) {
  page.set_margin(24);
  page.set_margin_top(8);
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_child(page);
  scroller->set_vexpand(true);
  return scroller;
}

void OnboardingWindow::BuildTopBar(Gtk::Box& column) {
  auto* bar = Gtk::make_managed<Gtk::CenterBox>();
  bar->set_margin(8);
  bar->set_margin_start(12);
  bar->set_margin_end(12);
  back_ = Gtk::make_managed<Gtk::Button>();
  back_->set_icon_name("go-previous-symbolic");
  back_->add_css_class("flat");
  back_->add_css_class("circular");
  back_->set_tooltip_text(T_("back", "Back"));
  back_->signal_clicked().connect([this] {
    if (1 < step_) ShowStep(step_ - 1);
  });
  bar->set_start_widget(*back_);

  auto* center = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  center->set_valign(Gtk::Align::CENTER);
  headerSlot_ = Gtk::make_managed<Gtk::Box>();
  headerSlot_->set_size_request(static_cast<int>(kHeaderConnectorSize),
                                static_cast<int>(kHeaderConnectorSize));
  headerSlot_->set_visible(false);
  center->append(*headerSlot_);
  bubbles_ = Gtk::make_managed<StepBubbles>();
  center->append(*bubbles_);
  bar->set_center_widget(*center);

  skip_ = Gtk::make_managed<Gtk::Button>(T_("skip", "Skip"));
  skip_->add_css_class("ur-onb-skip");
  skip_->signal_clicked().connect([this] { Finish(); });
  bar->set_end_widget(*skip_);
  column.append(*bar);
}

void OnboardingWindow::BuildUi() {
  set_child(overlay_);
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  BuildTopBar(*column);
  stack_.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
  stack_.set_transition_duration(motion::ShouldAnimate() ? motion::kBaseMs : 0);
  stack_.set_vexpand(true);
  column->append(stack_);
  overlay_.set_child(*column);


  BuildWelcome();
  BuildBandwidth();
  BuildProvide();
  BuildReferral();
}

// ---- page 1: welcome + the plan
void OnboardingWindow::BuildWelcome() {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  route_ = Gtk::make_managed<RouteLine>();
  top->append(*route_);

  auto* title = MakeLabel(T_("welcome_to_urnetwork", "Welcome to URnetwork"), "ur-onb-title");
  title->set_margin_top(20);
  top->append(*title);
  auto* tagline = MakeLabel(T_("intro_verifiable_encryption",
                               "URnetwork gives you verifiable encryption for everyday use."),
                            "ur-onb-lead");
  tagline->set_margin_top(16);
  top->append(*tagline);

  // the plan cards: annual in the gold dress, monthly plain
  yearlyCard_ = Gtk::make_managed<GoldPlanCard>();
  yearlyCard_->SetTexts(T_("plan_yearly_price", "$40/year"), T_("save_33_percent", "Save 33%"),
                        Format(T_("includes_free_trial_days", "Includes {} day free trial"),
                               kFreeTrialDays));
  yearlyCard_->set_margin_top(52);  // room for the halo and the pill, and air after the tagline
  yearlyCard_->on_select = [this] { SelectPlan(true); };
  top->append(*yearlyCard_);

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
  monthlyCard_->signal_clicked().connect([this] { SelectPlan(false); });
  top->append(*monthlyCard_);

  // only the yearly plan carries the trial: the button says what the click does
  startButton_ = Gtk::make_managed<Gtk::Button>(T_("start_free_trial", "Start free trial"));
  auto* start = startButton_;
  start->add_css_class("ur-btn-primary");
  start->add_css_class("pill");
  start->set_margin_top(20);
  start->signal_clicked().connect([this] {
    if (!checkout_) checkout_ = std::make_unique<UpgradeSheet>(*this, host_, balance_);
    checkout_->OpenCheckout(yearly_);
  });
  top->append(*start);
  page->append(*top);

  // the other ways in, as quiet links at the bottom
  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  page->append(*spacer);
  auto* links = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  links->set_halign(Gtk::Align::CENTER);
  links->set_margin_top(24);
  auto* community = Gtk::make_managed<Gtk::Button>(T_("community_edition", "Community Edition"));
  community->add_css_class("ur-onb-link");
  community->add_css_class("flat");
  community->signal_clicked().connect([this] { ShowStep(2); });
  links->append(*community);
  auto* redeem = Gtk::make_managed<Gtk::Button>(T_("redeem_balance_code", "Redeem Balance Code"));
  redeem->add_css_class("ur-onb-link");
  redeem->add_css_class("flat");
  redeem->signal_clicked().connect([this] {
    if (!redeem_) redeem_ = std::make_unique<RedeemCodeSheet>(*this, host_, balance_);
    redeem_->Open();
  });
  links->append(*redeem);
  page->append(*links);

  SelectPlan(true);
  stack_.add(*WrapPage(*page), "welcome");
}

void OnboardingWindow::SelectPlan(bool yearly) {
  yearly_ = yearly;
  if (startButton_) {
    startButton_->set_label(yearly ? T_("start_free_trial", "Start free trial")
                                   : T_("pay_with_stripe", "Join with Stripe"));
  }
  if (yearlyCard_) yearlyCard_->SetSelected(yearly);
  if (monthlyCard_) {
    if (yearly) monthlyCard_->remove_css_class("selected");
    else monthlyCard_->add_css_class("selected");
  }
  if (monthlyDot_) {
    const Rgba& color = yearly ? kUrTextMuted : kUrPink;
    monthlyDot_->set_markup("<span foreground='" + HexForMarkup(color) + "' size='" +
                            std::to_string(14 * PANGO_SCALE) + "'>" + (yearly ? "○" : "●") +
                            "</span>");
  }
}

// ---- page 2: your bandwidth
void OnboardingWindow::BuildBandwidth() {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* title = MakeLabel(T_("your_bandwidth", "Your bandwidth"), "ur-onb-title");
  title->set_margin_top(12);
  top->append(*title);
  auto* line = MakeLabel(T_("you_get_free_data_every_day", "You get free data every day."),
                         "ur-onb-lead");
  line->set_margin_top(16);
  top->append(*line);

  usage_ = Gtk::make_managed<UsageBar>();
  usage_->SetShowReferrals(false);
  usage_->set_margin_top(32);
  top->append(*usage_);

  dailyLine_ = MakeLabel("", "ur-onb-body");
  dailyLine_->set_margin_top(32);
  dailyLine_->set_visible(false);
  top->append(*dailyLine_);
  page->append(*top);

  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  page->append(*spacer);
  auto* next = Gtk::make_managed<Gtk::Button>(T_("next", "Next"));
  next->add_css_class("ur-btn-primary");
  next->add_css_class("pill");
  next->set_margin_top(24);
  next->signal_clicked().connect([this] { ShowStep(3); });
  page->append(*next);
  stack_.add(*WrapPage(*page), "bandwidth");
}

void OnboardingWindow::RefreshBalance() {
  if (!usage_) return;
  usage_->SetReferralTerms(balance_.MaxReferrals(), balance_.BonusGibPerDay());
  usage_->SetData(balance_.UsedByteCount(), balance_.PendingByteCount(),
                  balance_.AvailableByteCount(), balance_.StartBalanceByteCount(),
                  balance_.TotalReferrals());
  const int64_t daily = balance_.StartBalanceByteCount();
  if (dailyLine_) {
    // the free allowance the server grants, never a number typed into the app
    dailyLine_->set_visible(0 < daily);
    if (0 < daily) {
      dailyLine_->set_text(Format(T_("default_daily_data", "By default, you get {} every day."),
                                  FormatDailyAllowance(daily)));
    }
  }
}

// ---- page 3: contribute bandwidth
void OnboardingWindow::BuildProvide() {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* title = MakeLabel(T_("contribute_bandwidth", "Contribute bandwidth"), "ur-onb-title");
  title->set_margin_top(12);
  top->append(*title);
  auto* lead = MakeLabel(T_("provide_intro_lead", "Your choice, completely optional:"), "ur-onb-lead");
  lead->set_margin_top(16);
  top->append(*lead);
  auto* first = MakeBullet(T_("provide_intro_bullet_devices",
                              "Allow your devices to share internet with each other"));
  first->set_margin_top(16);
  top->append(*first);
  auto* second = MakeBullet(T_("provide_intro_bullet_people",
                               "Share your internet with other people ❤️"));
  second->set_margin_top(12);
  top->append(*second);

  // the provide mode picker, one line under each mode saying what it does
  auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  card->add_css_class("ur-onb-card");
  card->set_margin_top(32);
  auto* cardInner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  cardInner->set_margin(16);
  auto* heading = MakeLabel(T_("provide_mode", "Provide mode"), "ur-onb-body");
  heading->set_margin_bottom(6);
  cardInner->append(*heading);
  struct Mode {
    const char* value;
    Glib::ustring title;
    Glib::ustring description;
  };
  const Mode modes[] = {
      {"auto", T_("auto", "Auto"),
       T_("provide_mode_auto_description",
          "Provides to everyone while you're connected, otherwise only to your own devices.")},
      {"always", T_("always", "Always"),
       T_("provide_mode_always_description", "Provides to everyone whenever the app is running.")},
      {"network", T_("network", "Network"),
       T_("provide_mode_network_description", "Provides only to your own network's devices.")},
      {"never", T_("never", "Never"), ""},
  };
  const std::string current = host_.GetProvideControlMode();
  Gtk::CheckButton* group = nullptr;
  for (const Mode& mode : modes) {
    auto* check = Gtk::make_managed<Gtk::CheckButton>();
    if (group) check->set_group(*group);
    else group = check;
    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_margin_start(6);
    auto* modeTitle = MakeLabel(mode.title, "ur-onb-provide-title", false);
    labels->append(*modeTitle);
    if (!mode.description.empty()) {
      labels->append(*MakeLabel(mode.description, "ur-onb-provide-desc"));
    }
    check->set_child(*labels);
    check->set_margin_top(4);
    check->set_margin_bottom(4);
    const std::string value = mode.value;
    check->set_active(current == value);
    check->signal_toggled().connect([this, check, value] {
      if (syncingProvide_ || !check->get_active()) return;
      host_.SetProvideControlMode(value);
    });
    provideChecks_.push_back(check);
    cardInner->append(*check);
  }
  if (!std::any_of(provideChecks_.begin(), provideChecks_.end(),
                   [](Gtk::CheckButton* c) { return c->get_active(); })) {
    syncingProvide_ = true;
    provideChecks_.back()->set_active(true);  // never: providing is opt-in
    syncingProvide_ = false;
  }
  card->append(*cardInner);
  top->append(*card);
  page->append(*top);

  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  page->append(*spacer);
  auto* next = Gtk::make_managed<Gtk::Button>(T_("next", "Next"));
  next->add_css_class("ur-btn-primary");
  next->add_css_class("pill");
  next->set_margin_top(24);
  next->signal_clicked().connect([this] { ShowStep(4); });
  page->append(*next);
  stack_.add(*WrapPage(*page), "provide");
}

// ---- page 4: refer friends
void OnboardingWindow::BuildReferral() {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* title = MakeLabel(T_("refer_friends_header", "Refer friends"), "ur-onb-title");
  title->set_margin_top(12);
  top->append(*title);
  auto* lead = MakeLabel(T_("when_you_refer_a_friend", "When you refer a friend:"), "ur-onb-lead");
  lead->set_margin_top(16);
  top->append(*lead);
  auto* perks = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  perks->set_margin_top(16);
  perkYou_ = MakeBullet("");
  perkFriend_ = MakeBullet("");
  perks->append(*perkYou_);
  perks->append(*perkFriend_);
  top->append(*perks);

  // the progress box: referrals earned out of the ones that pay, in gold
  auto* progress = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  progress->add_css_class("ur-onb-card");
  progress->set_margin_top(32);
  auto* progressInner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  progressInner->set_margin(16);
  auto* progressRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* progressTitle = MakeLabel(T_("refer_friends_header", "Refer friends"), "ur-onb-neuebit", false);
  progressTitle->set_hexpand(true);
  progressRow->append(*progressTitle);
  referralProgressCount_ = MakeLabel("", "ur-onb-neuebit", false);
  progressRow->append(*referralProgressCount_);
  progressInner->append(*progressRow);
  referralBar_ = Gtk::make_managed<Gtk::DrawingArea>();
  referralBar_->set_content_height(12);
  referralBar_->set_hexpand(true);
  referralBar_->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const int64_t total = balance_.TotalReferrals();
    const int64_t maxReferrals = std::max<int64_t>(1, balance_.MaxReferrals());
    const double fraction = std::clamp(total / static_cast<double>(maxReferrals), 0.0, 1.0);
    RoundedRectPath(cr, 0, 0, w, h, 6);
    cr->set_source_rgba(kUrTextFaint.r, kUrTextFaint.g, kUrTextFaint.b, 1);
    cr->fill();
    if (0 < fraction) {
      RoundedRectPath(cr, 0, 0, std::max(12.0, w * fraction), h, 6);
      cr->set_source_rgba(kReferralGold.r, kReferralGold.g, kReferralGold.b, 1);
      cr->fill();
    }
  });
  progressInner->append(*referralBar_);
  auto* legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  auto legendKey = [](const Glib::ustring& text, const Rgba& color) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    auto* dot = Gtk::make_managed<Gtk::Label>();
    dot->set_markup("<span foreground='" + HexForMarkup(color) + "'>●</span>");
    box->append(*dot);
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->add_css_class("ur-onb-muted");
    label->add_css_class("caption");
    box->append(*label);
    return box;
  };
  legend->append(*legendKey(T_("referrals", "Referrals"), kReferralGold));
  legend->append(*legendKey(T_("available_data_key", "Available"), kUrTextFaint));
  progressInner->append(*legend);
  progress->append(*progressInner);
  top->append(*progress);

  referralPanel_ = Gtk::make_managed<ReferralPanel>(*this);
  referralPanel_->set_margin_top(24);
  top->append(*referralPanel_);
  page->append(*top);

  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  page->append(*spacer);
  auto* done = Gtk::make_managed<Gtk::Button>(T_("get_connected", "Get connected"));
  done->add_css_class("ur-btn-primary");
  done->add_css_class("pill");
  done->set_margin_top(24);
  done->signal_clicked().connect([this] { Finish(); });
  page->append(*done);
  stack_.add(*WrapPage(*page), "referral");
}

void OnboardingWindow::RefreshReferral() {
  const ReferralTerms terms{balance_.MaxReferrals(), balance_.BonusGibPerDay(),
                            balance_.ReferredBonusGibPerDay()};
  if (perkYou_) {
    auto* label = dynamic_cast<Gtk::Label*>(perkYou_->get_last_child());
    if (label) label->set_text(Format(T_("refer_friends_perks", "You get +{} GiB/day for life"),
                                      terms.bonusGibPerDay));
  }
  if (perkFriend_) {
    auto* label = dynamic_cast<Gtk::Label*>(perkFriend_->get_last_child());
    if (label) label->set_text(Format(T_("refer_friends_they_get_data",
                                         "Your friend gets +{} GiB/day for life"),
                                      terms.referredBonusGibPerDay));
  }
  if (referralProgressCount_) {
    referralProgressCount_->set_text(std::to_string(balance_.TotalReferrals()) + "/" +
                                     std::to_string(terms.maxReferrals));
  }
  if (referralBar_) referralBar_->queue_draw();
  if (referralPanel_) referralPanel_->Update(balance_.ReferralCode(), balance_.TotalReferrals(), terms);
}

// ---- the flow
void OnboardingWindow::ShowStep(int step) {
  step = std::clamp(step, 1, kOnboardingSteps);
  step_ = step;
  static const char* names[] = {"welcome", "bandwidth", "provide", "referral"};
  stack_.set_visible_child(names[step - 1]);
  bubbles_->SetStep(step);
  back_->set_visible(1 < step);
  headerSlot_->set_visible(1 < step);
  if (step == 2) RefreshBalance();
  if (step == 4) RefreshReferral();
  // the connector: large in page 1's route, small beside the bubbles after it
  // the flight measures both slots, so it waits for the layout pass that
  // places the header slot (it just became visible) before taking off
  if (step == 1 && connectorInHeader_) {
    connectorInHeader_ = false;
    Glib::signal_idle().connect_once([this] { FlyConnector(/*toHeader=*/false); });
  } else if (step > 1 && !connectorInHeader_) {
    connectorInHeader_ = true;
    Glib::signal_idle().connect_once([this] { FlyConnector(/*toHeader=*/true); });
  }
}

void OnboardingWindow::PlaceConnector(double x, double y, double size) {
  connectorX_ = x;
  connectorY_ = y;
  connectorSize_ = size;
  queue_draw();
}

// The header slot's bounds in window coordinates, or false before it is laid out.
static bool SlotBounds(Gtk::Widget& slot, Gtk::Window& window, double& x, double& y, double& size) {
  graphene_rect_t bounds;
  if (!slot.get_visible() || !gtk_widget_compute_bounds(slot.gobj(), GTK_WIDGET(window.gobj()), &bounds)) return false;
  if (bounds.size.width < 1) return false;
  x = bounds.origin.x;
  y = bounds.origin.y;
  size = bounds.size.width;
  return true;
}

void OnboardingWindow::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
  Gtk::Window::snapshot_vfunc(snapshot);
  if (!connectorVisible_) return;
  // settled in the header: follow the slot wherever the layout puts it
  if (flightTick_ == 0 && connectorInHeader_ && headerSlot_) {
    double x = 0, y = 0, size = 0;
    if (SlotBounds(*headerSlot_, *this, x, y, size)) {
      connectorX_ = x;
      connectorY_ = y;
      connectorSize_ = size;
    }
  }
  if (connectorSize_ <= 0) return;
  auto surface = ConnectorSurface();
  if (!surface) return;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(static_cast<float>(connectorX_), static_cast<float>(connectorY_),
                                              static_cast<float>(connectorSize_), static_cast<float>(connectorSize_));
  auto cr = snapshot->append_cairo(&bounds);
  cr->translate(connectorX_, connectorY_);
  cr->scale(connectorSize_ / surface->get_width(), connectorSize_ / surface->get_height());
  cr->set_source(surface, 0, 0);
  cr->paint();
}

void OnboardingWindow::FlyConnector(bool toHeader) {
  // both slots in the overlay's coordinates
  auto boundsOf = [this](Gtk::Widget& widget, double& x, double& y) {
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(widget.gobj(), GTK_WIDGET(gobj()), &bounds)) return false;
    x = bounds.origin.x;
    y = bounds.origin.y;
    return true;
  };
  double routeX = 0, routeY = 0, slotX = 0, slotY = 0, slotSize = kRouteConnectorSize;
  const bool haveRoute = route_ && boundsOf(*route_, routeX, routeY);
  if (haveRoute) route_->SlotRect(slotX, slotY, slotSize);
  const double routeSlotX = routeX + slotX;
  const double routeSlotY = routeY + slotY;
  double headerX = 0, headerY = 0;
  const bool haveHeader = headerSlot_ && boundsOf(*headerSlot_, headerX, headerY);

  if (flightTick_) {
    remove_tick_callback(flightTick_);
    flightTick_ = 0;
  }
  if (toHeader) {
    fromX_ = routeSlotX; fromY_ = routeSlotY; fromSize_ = slotSize;
    toX_ = headerX; toY_ = headerY; toSize_ = kHeaderConnectorSize;
  } else {
    fromX_ = headerX; fromY_ = headerY; fromSize_ = kHeaderConnectorSize;
    toX_ = routeSlotX; toY_ = routeSlotY; toSize_ = slotSize;
  }
  if (route_) route_->SetDrawConnector(false);
  connectorVisible_ = true;
  const bool animate = motion::ShouldAnimate() && haveRoute && haveHeader;
  if (!animate) {
    PlaceConnector(toX_, toY_, toSize_);
    if (!toHeader) {
      connectorVisible_ = false;
      if (route_) route_->SetDrawConnector(true);
      queue_draw();
    }
    return;
  }
  PlaceConnector(fromX_, fromY_, fromSize_);
  flightStart_ = Now();
  flightTick_ = add_tick_callback([this, toHeader](const Glib::RefPtr<Gdk::FrameClock>&) {
    // the header slot may still be settling into place: retarget every frame
    if (headerSlot_) {
      double x = 0, y = 0, size = 0;
      if (SlotBounds(*headerSlot_, *this, x, y, size)) {
        if (toHeader) { toX_ = x; toY_ = y; toSize_ = size; }
        else { fromX_ = x; fromY_ = y; fromSize_ = size; }
      }
    }
    const double t = std::clamp((Now() - flightStart_) / kFlightMs, 0.0, 1.0);
    const double e = motion::EvalBezier(motion::kStandardP1, motion::kStandardP2, t);
    PlaceConnector(fromX_ + (toX_ - fromX_) * e, fromY_ + (toY_ - fromY_) * e,
                   fromSize_ + (toSize_ - fromSize_) * e);
    if (t < 1) return true;
    flightTick_ = 0;
    if (!toHeader) {
      connectorVisible_ = false;
      if (route_) route_->SetDrawConnector(true);
      queue_draw();
    }
    return false;
  });
}

}  // namespace urnw
