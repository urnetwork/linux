// SPDX-License-Identifier: MPL-2.0
#include "ReferralPanel.hpp"

#include <algorithm>
#include <cmath>

#include <graphene.h>

#include "I18n.hpp"
#include "NetworkSpaceBootstrap.hpp"  // kUrLinkHostName
#include "PaneKit.hpp"
#include "ReferralShare.hpp"
#include "RuntimePaths.hpp"
#include "Ui.hpp"
#include "UrMotion.hpp"

namespace urnw {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kPanelBarPx = 6;      // the gold panel's bar (android ReferralProgressBar 6dp)

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
.ur-onb-offer-card { background-color: #1C1C1C; border-radius: 12px; border: 1px solid #FFC400; padding: 20px; }
.ur-onb-plan { border: 2px solid #989898; border-radius: 12px; padding: 34px 20px; background-color: #101010; color: #F8F8F8; }
.ur-onb-plan.selected { border-color: #ED8FFF; }
.ur-onb-gold-card { border-radius: 12px; }
.ur-onb-gold-card:focus-visible { outline: 2px solid alpha(@accent_color, 0.5); outline-offset: -2px; }
.ur-onb-pill-gold { background: linear-gradient(#FFE082, #FFC400); color: #101010; border-radius: 16px; padding: 4px 14px; font-family: "PP NeueBit"; font-size: 18px; font-weight: bold; border: 1px solid alpha(#FFFFFF, 0.45); }
.ur-onb-gold { color: #FFC400; }
.ur-onb-gold-light { color: #FFE082; }
.ur-onb-ref-gold-light { color: #FFD76A; }
.ur-onb-kicker { color: #F5B93C; font-size: 11px; font-weight: bold; letter-spacing: 2px; }
.ur-onb-blue-light { color: alpha(#D6E6F4, 0.85); }
.ur-onb-blue-faint { color: alpha(#D6E6F4, 0.6); }
.ur-onb-progress-count { font-size: 12px; }
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

  // a failed read with no code says so, with the read again beside it
  codeFailed_.set_text(
      T_("load_failed", "Couldn't load. Check your connection and try again."));
  codeFailed_.add_css_class("ur-onb-body");
  codeFailed_.add_css_class("ur-onb-blue-light");
  codeFailed_.set_wrap(true);
  codeFailed_.set_justify(Gtk::Justification::CENTER);
  codeFailed_.set_visible(false);
  column->append(codeFailed_);
  retry_ = Gtk::make_managed<Gtk::Button>(T_("try_again", "Try again"));
  retry_->add_css_class("ur-onb-gold-btn");
  retry_->set_halign(Gtk::Align::CENTER);
  retry_->set_visible(false);
  retry_->signal_clicked().connect([this] {
    if (on_retry) on_retry();
  });
  column->append(*retry_);

  share_ = Gtk::make_managed<Gtk::Button>(T_("share", "Share"));
  share_->add_css_class("ur-onb-gold-btn");
  share_->set_margin_top(6);  // 12 under the pill, with the column's 6
  share_->signal_clicked().connect([this] {
    get_clipboard()->set_text(ReferralShareText(
        Format(T_("referral_share_message",
                  "Join me on URnetwork! Get the app and enter referral code {} when you sign up."),
               referralCode_),
        ReferralLinkUrl(kUrLinkHostName, referralCode_)));
    share_->set_label(T_("copied", "Copied!"));
    Glib::signal_timeout().connect_once([this] { share_->set_label(T_("share", "Share")); }, 1800);
  });
  column->append(*share_);

  // android ReferralProgressBar: 16 under the share button, a 6px gold track
  // that fills per friend, 6 to "joined / cap" under it, 12 to the status line
  progressBar_ = Gtk::make_managed<Gtk::DrawingArea>();
  progressBar_->set_content_height(kPanelBarPx);
  progressBar_->set_hexpand(true);
  progressBar_->set_margin_top(10);
  kit::MarkDecorative(*progressBar_);  // the count under it carries the figure
  progressBar_->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    RoundedRectPath(cr, 0, 0, w, h, h / 2.0);
    cr->set_source_rgba(kReferralGold.r, kReferralGold.g, kReferralGold.b, 0.18);
    cr->fill();
    if (0 < progressFraction_) {
      const double filled = std::max(static_cast<double>(h), w * progressFraction_);
      RoundedRectPath(cr, 0, 0, filled, h, h / 2.0);
      auto fill = Cairo::LinearGradient::create(0, 0, filled, 0);
      fill->add_color_stop_rgba(0, kReferralGold.r, kReferralGold.g, kReferralGold.b, 1);
      fill->add_color_stop_rgba(1, kReferralGoldLight.r, kReferralGoldLight.g,
                                kReferralGoldLight.b, 1);
      cr->set_source(fill);
      cr->fill();
    }
  });
  column->append(*progressBar_);
  progressCount_.add_css_class("ur-onb-body");
  progressCount_.add_css_class("ur-onb-progress-count");
  progressCount_.add_css_class("ur-onb-blue-faint");
  progressCount_.set_justify(Gtk::Justification::CENTER);
  column->append(progressCount_);

  status_.add_css_class("ur-onb-body");
  status_.set_wrap(true);
  status_.set_justify(Gtk::Justification::CENTER);
  status_.set_margin_top(6);
  column->append(status_);
  append(*column);

  if (motion::ShouldAnimate()) {
    start_ = Now();
    mapped_ = signal_map().connect([this] { StartBreath(); });
    unmapped_ = signal_unmap().connect([this] { StopBreath(); });
  }
}

ReferralPanel::~ReferralPanel() {
  mapped_.disconnect();
  unmapped_.disconnect();
  StopBreath();
}

void ReferralPanel::StartBreath() {
  if (tick_) return;
  tick_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) {
    const double period = crowned_ ? 3400.0 : 5000.0;
    breath_ = 0.5 + 0.5 * std::sin((Now() - start_) / period * 2 * kPi);
    queue_draw();
    return true;
  });
}

void ReferralPanel::StopBreath() {
  if (!tick_) return;
  remove_tick_callback(tick_);
  tick_ = 0;
}

void ReferralPanel::Update(const std::string& referralCode, ReferralCodeView view,
                           int64_t totalReferrals, const ReferralTerms& terms) {
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
  codeFailed_.set_visible(view == ReferralCodeView::Unavailable);
  retry_->set_visible(view == ReferralCodeView::Unavailable);
  // the bar: friends joined out of the code's cap; once the cap is reached the
  // count says so in words (android ReferralProgressBar)
  const int64_t cap = std::max<int64_t>(1, terms.maxReferrals);
  const int64_t joined = std::clamp<int64_t>(totalReferrals, 0, cap);
  const bool capped = cap <= joined;
  progressFraction_ = joined / static_cast<double>(cap);
  progressCount_.set_text(capped ? Glib::ustring(T_("referral_code_capped", "This code has been used up"))
                                 : Glib::ustring(std::to_string(joined) + " / " + std::to_string(cap)));
  if (capped) {
    progressCount_.remove_css_class("ur-onb-blue-faint");
    progressCount_.add_css_class("ur-onb-ref-gold-light");
  } else {
    progressCount_.remove_css_class("ur-onb-ref-gold-light");
    progressCount_.add_css_class("ur-onb-blue-faint");
  }
  progressBar_->queue_draw();
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
