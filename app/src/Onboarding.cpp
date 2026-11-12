#include "Onboarding.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include <graphene.h>

#include "AppPrefs.hpp"
#include "ClientEvents.hpp"
#include "CryptoInvoiceHost.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "ReferralPanel.hpp"
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
                                          count_));
    queue_draw();
  }
  // four pages for the offer holdout, five with the offer page
  void SetCount(int count) {
    count_ = std::clamp(count, 1, kOnboardingSteps);
    set_content_width(count_ * 8 + 14 + (count_ - 1) * 6);
    SetStep(step_);
  }

 private:
  void Draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    double x = 0;
    for (int i = 1; i <= count_; ++i) {
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
  int count_ = kOnboardingSteps;
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
      mapped_ = signal_map().connect([this] { StartWalk(); });
      unmapped_ = signal_unmap().connect([this] { StopWalk(); });
    } else {
      progress_ = 0.85;  // parked on the last leg, in view
    }
  }
  ~RouteLine() override {
    mapped_.disconnect();
    unmapped_.disconnect();
    StopWalk();
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
  // The walk ticks only while the route is on screen, from map to unmap (the
  // step left, the window hidden). A tick left running kept its window's
  // frame clock busy for the life of the process.
  void StartWalk() {
    if (tick_) return;
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
  }
  void StopWalk() {
    if (!tick_) return;
    remove_tick_callback(tick_);
    tick_ = 0;
  }

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
  sigc::connection mapped_;
  sigc::connection unmapped_;
  bool drawConnector_ = true;
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
  step_ = 0;  // no page yet: the first ShowStep completes nothing
  standalone_ = false;
  offerIssued_ = false;
  introOfferShown_ = false;
  // the in-app offer experiment: the holdout gets the four-page flow with the
  // regular picker and never the issue call
  offerEnabled_ = OfferPageEnabled(balance_.ExperimentVariant(kOfferExperimentSurface));
  bubbles_->SetCount(OnboardingStepCount(offerEnabled_));
  if (referralDone_) {
    referralDone_->set_label(offerEnabled_ ? T_("next", "Next") : T_("get_connected", "Get connected"));
  }
  connectorInHeader_ = false;
  connectorVisible_ = false;
  if (route_) route_->SetDrawConnector(true);
  ApplyPrices();
  // the method this device chose last, while the server still offers it
  methodPicked_ = false;
  ApplyMethods(/*remembered=*/true);
  ShowStep(1);
  balance_.FetchNow();
  RefreshBalance();
  RefreshReferral();
  balancePoll_.disconnect();
  balancePoll_ = Glib::signal_timeout().connect([this] {
    RefreshBalance();
    RefreshReferral();
    ApplyPrices();
    // the coins arrive with the balance, and the offer decides their plan
    ApplyMethods(/*remembered=*/false);
    // the experiment assignment can land after the first paint (the balance
    // store fetches in the background): take the holdout out of the offer
    if (offerEnabled_ && !OfferPageEnabled(balance_.ExperimentVariant(kOfferExperimentSurface))) {
      offerEnabled_ = false;
      bubbles_->SetCount(OnboardingStepCount(false));
      if (referralDone_) referralDone_->set_label(T_("get_connected", "Get connected"));
      if (welcomeOffer_) welcomeOffer_->set_visible(false);
      skip_->set_visible(OnboardingShowsSkip(step_, false));
    }
    if (offerEnabled_ && !offerIssued_) IssueOffer();
    return true;
  }, 2000);
  if (offerEnabled_) IssueOffer();
  present();
}

void OnboardingWindow::OpenAt(int step) {
  Open();
  if (1 < step) {
    Glib::signal_timeout().connect_once([this, step] { ShowStep(step); }, 400);
  }
}

void OnboardingWindow::OpenOffer() {
  step_ = 0;
  standalone_ = true;
  offerEnabled_ = true;
  offerIssued_ = true;  // the link's offer already exists: never re-issued
  introOfferShown_ = true;
  bubbles_->SetCount(OnboardingStepCount(true));
  connectorInHeader_ = true;
  connectorVisible_ = false;
  ApplyPrices();
  methodPicked_ = false;
  ApplyMethods(/*remembered=*/true);
  ShowStep(kOnboardingStepOffer);
  balancePoll_.disconnect();
  balancePoll_ = Glib::signal_timeout().connect([this] {
    ApplyPrices();
    ApplyMethods(/*remembered=*/false);
    return true;
  }, 2000);
  present();
}

// Skip is not a way around the offer page: from any earlier page it lands
// there once; only the page's own link finishes the flow (the holdout has no
// offer page, so Skip finishes at once).
void OnboardingWindow::Skip() {
  host_.events().OnboardingStepSkipped(OnboardingStepName(step_), step_, StepElapsedMs());
  const int target = OnboardingSkipTarget(step_, offerEnabled_);
  if (target == 0) {
    Finish();
  } else {
    ShowStep(target);
  }
}

void OnboardingWindow::Finish() {
  hide();
}

int64_t OnboardingWindow::StepElapsedMs() const {
  return stepShownAt_ <= 0 ? 0 : static_cast<int64_t>(Now() - stepShownAt_);
}

void OnboardingWindow::ApplyPrices() {
  const PriceTierView& tier = balance_.Tier();
  const OfferView& offer = balance_.Offer();
  const bool showOffer = offerEnabled_ && offer.active;
  // the picker: the offer card while the offer is active, the tier otherwise
  OfferView pickerOffer = offer;
  pickerOffer.active = showOffer;
  if (plans_) plans_->SetPrices(tier, pickerOffer);
  if (welcomeOffer_) {
    welcomeOffer_->set_visible(showOffer);
    if (showOffer) welcomeOffer_->Update(offer, tier, kFreeTrialDays);
  }
  // the final page always prints the offer's numbers (a page the holdout never sees)
  if (offerEyebrow_) {
    offerEyebrow_->set_text(
        Format(T_("offer_percent_off_first_year", "{}% off your first year"), offer.percentOff));
  }
  if (offerHeadline_) {
    offerHeadline_->set_text(
        Format(TN_("offer_months_free_headline", "{} month of Pro, free", "{} months of Pro, free",
                   static_cast<unsigned long>(offer.monthsFree)),
               offer.monthsFree));
  }
  if (offerPrice_) {
    offerPrice_->set_text(Format(T_("offer_first_year_price", "{} for your first year"),
                                 FormatMoney(offer.firstYear, offer.currency)));
  }
  if (offerThen_) {
    offerThen_->set_text(Format(T_("offer_then_regular_price", "then {}/year"),
                                FormatMoney(offer.regularYear, offer.currency)));
  }
  if (offerTrial_) {
    offerTrial_->set_text(
        Format(T_("includes_free_trial_days", "Includes {} day free trial"), kFreeTrialDays));
  }
  if (offerCard_) offerCard_->Update(offer, tier, kFreeTrialDays);
  if (offerCta_) offerCta_->set_label(OfferCtaLabel());
  // the intro surface's shown event, once the offer is actually on the page
  if (showOffer && step_ == kOnboardingStepWelcome && !introOfferShown_) {
    introOfferShown_ = true;
    host_.events().OfferScreenShown("intro_step", balance_.ExperimentId(kOfferExperimentSurface),
                                    balance_.ExperimentVariant(kOfferExperimentSurface), tier.name,
                                    offer.firstYear, offer.currency, OfferExpiresInSeconds(offer));
  }
}

void OnboardingWindow::IssueOffer() {
  if (offerIssued_ || !host_.IsLoggedIn()) return;
  offerIssued_ = true;
  urnet::OnboardingOfferIssueArgs args;
  args.surface = "intro_step";
  host_.api().onboardingOfferIssue(
      args, [this](std::optional<urnet::OnboardingOfferIssueResult> result,
                   std::optional<std::string> err) {
        PostToMain([this, result = std::move(result), err = std::move(err)] {
          if (err) {
            std::fprintf(stderr, "[onboarding] offer issue failed: %s\n", err->c_str());
            return;
          }
          if (!result || !result->offer) return;
          balance_.SetOffer(*result->offer);
          ApplyPrices();
        });
      });
}

void OnboardingWindow::StartCheckout(bool yearly, const char* surface) {
  const bool coin = crypto_pay::IsCoin(method_);
  if (offerEnabled_ && balance_.OfferActive() && (yearly || coin)) {
    // the method's store: stripe for the card, the coin's market for a coin
    host_.events().OfferCtaTapped(PlanName(true), PaymentMethodEventStore(method_));
  }
  (void)surface;
  // No purchase for a legacy guest network (MainWindow::OpenUpgrade; windows
  // gates this tap in MainWindow::ShowUpgradeCheckout and ShowCryptoCheckout):
  // the flow steps aside for the conversion its owner opens, which goes on to
  // the upgrade once a sign-in is added (GuestUpgradeContinuation)
  if (balance_.IsGuest()) {
    hide();
    if (on_guest_sign_in_required) on_guest_sign_in_required();
    return;
  }
  if (!checkout_) {
    checkout_ = std::make_unique<UpgradeSheet>(*this, host_, balance_);
    // the server refused the checkout for a guest network: the flow steps
    // aside for the conversion its owner opens
    checkout_->on_guest_sign_in_required = [this] {
      hide();
      if (on_guest_sign_in_required) on_guest_sign_in_required();
    };
  }
  if (coin) {
    // the invoice, not the checkout: the yearly plan, or the welcome offer
    // while these pages show it
    checkout_->OpenCryptoCheckout(method_, crypto_pay::CoinPlan(ShowsOffer()));
    return;
  }
  checkout_->OpenCheckout(yearly);
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
  skip_->signal_clicked().connect([this] { Skip(); });
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
  BuildOffer();
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
                               "Encryption for everyday use."),
                            "ur-onb-lead");
  tagline->set_margin_top(16);
  top->append(*tagline);

  // the plan cards (the shared picker: annual in the gold dress, monthly plain)
  plans_ = Gtk::make_managed<PlanPicker>();
  plans_->set_margin_top(52);  // room for the halo and the pill, and air after the tagline
  plans_->on_select = [this](bool yearly) {
    if (yearly && offerEnabled_ && balance_.OfferActive()) {
      host_.events().OfferCardTapped(PlanName(true));
    }
    SelectPlan(yearly);
  };
  top->append(*plans_);

  // the welcome offer under the picker while the network's offer is active:
  // the static deadline, the trial timeline and the terms (OfferCard)
  welcomeOffer_ = Gtk::make_managed<OfferCard>();
  welcomeOffer_->set_margin_top(20);
  welcomeOffer_->set_visible(false);
  top->append(*welcomeOffer_);

  // Card and the SDK's coin tabs above the button (hidden with no coin listed)
  welcomeMethods_ = Gtk::make_managed<PaymentMethodRow>();
  welcomeMethods_->set_margin_top(20);
  welcomeMethods_->on_select = [this](const std::string& method) {
    methodPicked_ = true;
    host_.CryptoInvoice().SetPaymentMethod(method);  // remembered on this device
    SelectMethod(method);
  };
  top->append(*welcomeMethods_);

  startButton_ = Gtk::make_managed<Gtk::Button>(PlanPicker::CtaLabel(true));
  auto* start = startButton_;
  start->add_css_class("ur-btn-primary");
  start->add_css_class("pill");
  start->set_margin_top(20);
  start->signal_clicked().connect([this] { StartCheckout(yearly_, "intro_step"); });
  top->append(*start);
  welcomeFinePrint_ = MakeLabel(
      T_("crypto_fine_print_yearly",
         "A one-time payment for a year of Pro. It doesn't renew, and there's no free trial."),
      "ur-onb-body");
  welcomeFinePrint_->add_css_class("ur-onb-muted");
  welcomeFinePrint_->set_margin_top(8);
  welcomeFinePrint_->set_visible(false);
  top->append(*welcomeFinePrint_);
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
  if (plans_ && plans_->Yearly() != yearly) plans_->Select(yearly);
  // only the yearly plan carries the trial, and a coin pays: the button says
  // what the click does
  if (startButton_) startButton_->set_label(StartLabel());
}

// ---- the payment method (mmm/ZCASH-CONTRACT.md §6.1, §6.3) ------------------------------

bool OnboardingWindow::ShowsOffer() const { return offerEnabled_ && balance_.Offer().active; }

void OnboardingWindow::ApplyMethods(bool remembered) {
  const std::vector<crypto_pay::PaymentTabView> tabs =
      balance_.PaymentTabs(crypto_pay::CoinPlan(ShowsOffer()));
  bool changed = false;
  for (PaymentMethodRow* row : {welcomeMethods_, offerMethods_}) {
    if (row && row->SetTabs(tabs)) changed = true;
  }
  if (!changed && !remembered) return;
  // the method picked in this open while it is still offered, else the
  // device's last one, else Card
  SelectMethod(methodPicked_ ? method_ : host_.CryptoInvoice().PaymentMethod());
}

void OnboardingWindow::SelectMethod(const std::string& method) {
  // one choice, shown on both pages
  for (PaymentMethodRow* row : {welcomeMethods_, offerMethods_}) {
    if (row) row->Select(method);
  }
  method_ = welcomeMethods_ ? welcomeMethods_->Selected() : std::string(crypto_pay::kMethodCard);
  ApplyMethod();
}

void OnboardingWindow::ApplyMethod() {
  const bool coin = crypto_pay::IsCoin(method_);
  if (plans_) {
    // Monthly is card only: a coin greys it out and buys the yearly plan,
    // once, with no trial and no renewal (no such wording while it is chosen)
    plans_->SetMonthlyAllowed(!coin);
    plans_->SetCardTermsShown(!coin);
  }
  if (coin && !yearly_) {
    SelectPlan(true);
  } else if (startButton_) {
    startButton_->set_label(StartLabel());
  }
  // The card's trial timeline and terms, and the offer page's renewal and
  // trial lines, do not describe a coin's payment (CONTRACT §6.1): its
  // first-year price stays, as the price the coin pays for the year. The
  // page's CTA sits under a spacer, so the card shrinking moves nothing else.
  if (welcomeOffer_) welcomeOffer_->SetCardTermsShown(!coin);
  if (offerCard_) offerCard_->SetCardTermsShown(!coin);
  if (offerThen_) offerThen_->set_visible(!coin);
  if (offerTrial_) offerTrial_->set_visible(!coin);
  if (offerCta_) offerCta_->set_label(OfferCtaLabel());
  if (welcomeFinePrint_) welcomeFinePrint_->set_visible(coin);
  if (offerFinePrint_) offerFinePrint_->set_visible(coin);
}

Glib::ustring OnboardingWindow::StartLabel() const {
  if (crypto_pay::IsCoin(method_)) {
    return Format(T_("crypto_pay_button", "Pay with {}"), PaymentMethodName(method_).raw());
  }
  return PlanPicker::CtaLabel(yearly_);
}

Glib::ustring OnboardingWindow::OfferCtaLabel() const {
  if (crypto_pay::IsCoin(method_)) {
    return Format(T_("crypto_pay_button", "Pay with {}"), PaymentMethodName(method_).raw());
  }
  const OfferView& offer = balance_.Offer();
  return Format(
      TN_("offer_cta_start_trial_months_free", "Start free trial with {} month free",
          "Start free trial with {} months free", static_cast<unsigned long>(offer.monthsFree)),
      offer.monthsFree);
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
  usage_->SetData(UsageBarDataFrom(balance_));
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

  // the gold panel is the SHARED referral piece (the Account "Refer and earn"
  // page shows the same one), so the onboarding step and the page cannot
  // drift; it carries the only referral progress bar on the step
  referralPanel_ = Gtk::make_managed<ReferralPanel>();
  referralPanel_->set_margin_top(32);
  referralPanel_->on_retry = [this] { balance_.RetryReferral(); };
  top->append(*referralPanel_);
  page->append(*top);

  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  page->append(*spacer);
  auto* done = Gtk::make_managed<Gtk::Button>(T_("get_connected", "Get connected"));
  referralDone_ = done;
  done->add_css_class("ur-btn-primary");
  done->add_css_class("pill");
  done->set_margin_top(24);
  // the offer page follows (everyone reaches it); the holdout finishes here
  done->signal_clicked().connect([this] {
    const int next = OnboardingReferralNext(offerEnabled_);
    if (next == 0) {
      host_.events().OnboardingStepCompleted(OnboardingStepName(step_), step_, StepElapsedMs());
      Finish();
    } else {
      ShowStep(next);
    }
  });
  page->append(*done);
  stack_.add(*WrapPage(*page), "referral");
}

// ---- page 5: the welcome offer (mmm/onboarding/PLAN.md "in-app offer screen")
// The same offer the plan page showed, restated: headline, the billed amount
// as the most prominent number, the static deadline, the trial timeline, one
// primary CTA, and the always-visible "Continue with the free plan". No
// countdown, and no second offer when the user declines.
void OnboardingWindow::BuildOffer() {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  offerEyebrow_ = MakeLabel("", "ur-onb-kicker");
  offerEyebrow_->add_css_class("ur-onb-gold");
  offerEyebrow_->set_margin_top(12);
  top->append(*offerEyebrow_);
  offerHeadline_ = MakeLabel("", "ur-onb-title");
  offerHeadline_->set_margin_top(8);
  top->append(*offerHeadline_);

  // the offer card: the price lines in the gold dress, then the supporting lines
  auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  card->add_css_class("ur-onb-offer-card");
  card->set_margin_top(28);
  offerPrice_ = MakeLabel("", "ur-onb-neuebit");
  card->append(*offerPrice_);
  offerThen_ = MakeLabel("", "ur-onb-body");
  offerThen_->add_css_class("ur-onb-muted");
  card->append(*offerThen_);
  offerTrial_ = MakeLabel("", "ur-onb-body");
  offerTrial_->add_css_class("ur-onb-gold-light");
  card->append(*offerTrial_);
  offerCard_ = Gtk::make_managed<OfferCard>();
  offerCard_->set_margin_top(12);
  card->append(*offerCard_);
  top->append(*card);
  page->append(*top);

  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  page->append(*spacer);

  // the same method row as the plan page, above this page's CTA
  offerMethods_ = Gtk::make_managed<PaymentMethodRow>();
  offerMethods_->set_margin_top(24);
  offerMethods_->on_select = [this](const std::string& method) {
    methodPicked_ = true;
    host_.CryptoInvoice().SetPaymentMethod(method);  // remembered on this device
    SelectMethod(method);
  };
  page->append(*offerMethods_);

  offerCta_ = Gtk::make_managed<Gtk::Button>();
  offerCta_->add_css_class("ur-btn-primary");
  offerCta_->add_css_class("pill");
  offerCta_->set_margin_top(24);
  offerCta_->signal_clicked().connect([this] { StartCheckout(true, "final_screen"); });
  page->append(*offerCta_);
  offerFinePrint_ = MakeLabel(
      T_("crypto_fine_print_yearly",
         "A one-time payment for a year of Pro. It doesn't renew, and there's no free trial."),
      "ur-onb-body");
  offerFinePrint_->add_css_class("ur-onb-muted");
  offerFinePrint_->set_margin_top(8);
  offerFinePrint_->set_visible(false);
  page->append(*offerFinePrint_);

  auto* keepFree = Gtk::make_managed<Gtk::Button>(
      T_("continue_with_free_plan", "Continue with the free plan"));
  keepFree->add_css_class("ur-onb-link");
  keepFree->add_css_class("flat");
  keepFree->set_halign(Gtk::Align::CENTER);
  keepFree->set_margin_top(8);
  // the decline: finishes exactly as Skip used to, and is never re-offered
  keepFree->signal_clicked().connect([this] {
    host_.events().OfferDeclined("continue_free", StepElapsedMs());
    host_.events().OnboardingStepCompleted(OnboardingStepName(step_), step_, StepElapsedMs());
    Finish();
  });
  page->append(*keepFree);
  stack_.add(*WrapPage(*page), "offer");
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
  if (referralPanel_) {
    referralPanel_->Update(balance_.ReferralCode(), balance_.ReferralView(),
                           balance_.TotalReferrals(), terms);
  }
}

// ---- the flow
void OnboardingWindow::ShowStep(int step) {
  step = std::clamp(step, 1, OnboardingStepCount(offerEnabled_));
  // the step events: the page we leave completed (or was skipped: Skip emits
  // its own event first, so the completion here is the forward path only),
  // the page we land on shown
  if (0 < step_ && step_ != step && step_ < step) {
    host_.events().OnboardingStepCompleted(OnboardingStepName(step_), step_, StepElapsedMs());
  }
  step_ = step;
  stepShownAt_ = Now();
  host_.events().OnboardingStepShown(OnboardingStepName(step), step, 0);
  stack_.set_visible_child(OnboardingStepName(step));
  bubbles_->SetStep(step);
  back_->set_visible(1 < step && !standalone_);
  headerSlot_->set_visible(1 < step);
  skip_->set_visible(OnboardingShowsSkip(step, offerEnabled_));
  if (step == kOnboardingStepBandwidth) RefreshBalance();
  if (step == kOnboardingStepReferral) RefreshReferral();
  if (step == kOnboardingStepOffer) {
    ApplyPrices();
    const OfferView& offer = balance_.Offer();
    host_.events().OfferScreenShown("final_screen", balance_.ExperimentId(kOfferExperimentSurface),
                                    balance_.ExperimentVariant(kOfferExperimentSurface),
                                    balance_.Tier().name, offer.firstYear, offer.currency,
                                    OfferExpiresInSeconds(offer));
  }
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
