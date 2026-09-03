// SPDX-License-Identifier: MPL-2.0
#include "ReferralPanel.hpp"

#include <algorithm>
#include <cmath>

#include <graphene.h>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "RuntimePaths.hpp"
#include "Ui.hpp"
#include "UrMotion.hpp"

namespace urnw {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kProgressBarPx = 12;  // the gold bar's height (windows Height=12)

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

double Now() { return g_get_monotonic_time() / 1000.0; }  // ms

Gtk::Label* MakeLabel(const Glib::ustring& text, const char* cssClass, bool wrap = true) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(wrap);
  label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  return label;
}

}  // namespace

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

// ---- the progress card ------------------------------------------------------

ReferralProgressBox::ReferralProgressBox() : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
  EnsureOnboardingCss();
  add_css_class("ur-onb-card");
  auto* inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  inner->set_margin(16);
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* title = MakeLabel(T_("refer_friends_header", "Refer friends"), "ur-onb-neuebit", false);
  title->set_hexpand(true);
  row->append(*title);
  count_ = MakeLabel("", "ur-onb-neuebit", false);
  row->append(*count_);
  inner->append(*row);
  bar_ = Gtk::make_managed<Gtk::DrawingArea>();
  bar_->set_content_height(kProgressBarPx);
  bar_->set_hexpand(true);
  kit::MarkDecorative(*bar_);  // the count beside it carries the figure
  bar_->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    RoundedRectPath(cr, 0, 0, w, h, 6);
    cr->set_source_rgba(kUrTextFaint.r, kUrTextFaint.g, kUrTextFaint.b, 1);
    cr->fill();
    if (0 < fraction_) {
      RoundedRectPath(cr, 0, 0, std::max(static_cast<double>(kProgressBarPx), w * fraction_), h, 6);
      cr->set_source_rgba(kReferralGold.r, kReferralGold.g, kReferralGold.b, 1);
      cr->fill();
    }
  });
  inner->append(*bar_);
  auto* legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  auto legendKey = [](const Glib::ustring& text, const Rgba& color) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    auto* dot = Gtk::make_managed<Gtk::Label>();
    dot->set_markup("<span foreground='" + HexForMarkup(color) + "'>●</span>");
    kit::MarkDecorative(*dot);
    box->append(*dot);
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->add_css_class("ur-onb-muted");
    label->add_css_class("caption");
    box->append(*label);
    return box;
  };
  legend->append(*legendKey(T_("referrals", "Referrals"), kReferralGold));
  legend->append(*legendKey(T_("available_data_key", "Available"), kUrTextFaint));
  inner->append(*legend);
  append(*inner);
  Update(0, CurrentReferralTerms());
}

void ReferralProgressBox::Update(int64_t totalReferrals, const ReferralTerms& terms) {
  const int64_t maxReferrals = std::max<int64_t>(1, terms.maxReferrals);
  const int64_t paid = terms.PaidReferrals(totalReferrals);
  fraction_ = std::clamp(paid / static_cast<double>(maxReferrals), 0.0, 1.0);
  // "n/max", the count the code has earned out of the ones that pay; once the
  // code is used up the figure says so in words instead
  const bool capped = 0 < terms.maxReferrals && terms.maxReferrals <= totalReferrals;
  count_->set_text(capped ? Glib::ustring(T_("referral_code_capped", "This code has been used up"))
                          : Glib::ustring(std::to_string(totalReferrals) + "/" +
                                          std::to_string(terms.maxReferrals)));
  kit::SetAccessibleLabel(*this, Glib::ustring(T_("referrals", "Referrals")) + ", " +
                                     count_->get_text());
  bar_->queue_draw();
}

// ---- the gold panel ---------------------------------------------------------

ReferralPanel::ReferralPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
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
    get_clipboard()->set_text(referralCode_);
    copy_->set_label(T_("copied", "Copied!"));
    Glib::signal_timeout().connect_once([this] { copy_->set_label(T_("copy", "Copy")); }, 1800);
  });
  codePill_->append(*copy_);
  column->append(*codePill_);

  share_ = Gtk::make_managed<Gtk::Button>(T_("share", "Share"));
  share_->add_css_class("ur-onb-gold-btn");
  share_->set_margin_top(10);
  share_->signal_clicked().connect([this] {
    get_clipboard()->set_text(Format(
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

ReferralPanel::~ReferralPanel() {
  if (tick_) remove_tick_callback(tick_);
}

void ReferralPanel::Update(const std::string& referralCode, int64_t totalReferrals,
                           const ReferralTerms& terms) {
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

void ReferralPanel::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
  const double w = get_width();
  const double h = get_height();
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
  auto cr = snapshot->append_cairo(&bounds);
  Draw(cr, w, h);
  Gtk::Box::snapshot_vfunc(snapshot);
}

void ReferralPanel::Draw(const Cairo::RefPtr<Cairo::Context>& cr, double w, double h) {
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

}  // namespace urnw
