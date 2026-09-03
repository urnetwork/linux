// SPDX-License-Identifier: MPL-2.0
#include "UpgradeSheet.hpp"

#include "PlanPicker.hpp"

#include <gio/gio.h>

#ifdef UR_HAVE_WEBKIT
#include <algorithm>
#include <map>

#include <webkit/webkit.h>
#endif

#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// Canonical Stripe checkout items (server/controller/subscription_stripe_
// controller.go StripeItemPro*).
constexpr const char* kItemProMonthly = "pro_monthly";
constexpr const char* kItemProYearly = "pro_yearly";
constexpr const char* kUiModeHosted = "hosted";
constexpr const char* kUiModeEmbedded = "embedded";

#ifdef UR_HAVE_WEBKIT
// The ur.io bridge page (mmm/ur.io react EmbeddedCheckout.jsx): mounts
// Stripe's Embedded Checkout for the session's client_secret — the card form
// stays in Stripe's iframe, no card data ever touches the app — and hands
// control back by navigating to the redirect_link:
//   done:  urnetwork://checkout?status=complete&session_id=cs_...
//   error: urnetwork://checkout?errorCode=-1&errorMessage=...
// There is no cancel url: Stripe's embedded flow never leaves the page, so
// the sheet's own close (X) is the only way out.
constexpr const char* kCheckoutPage = "https://ur.io/checkout";
constexpr const char* kCheckoutRedirect = "urnetwork://checkout";

std::string Escape(const std::string& s) {
  char* e = g_uri_escape_string(s.c_str(), nullptr, FALSE);
  std::string out = e ? e : s;
  if (e) g_free(e);
  return out;
}

std::map<std::string, std::string> ParseQuery(const std::string& url) {
  std::map<std::string, std::string> out;
  const size_t q = url.find('?');
  if (q == std::string::npos) return out;
  size_t i = q + 1;
  while (i < url.size()) {
    const auto amp = url.find('&', i);
    const std::string pair =
        url.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) {
      const std::string key = pair.substr(0, eq);
      std::string value = pair.substr(eq + 1);
      // the page assembles the query with URLSearchParams (form-urlencoded):
      // '+' is a space, which g_uri_unescape_string leaves alone
      std::replace(value.begin(), value.end(), '+', ' ');
      char* dec = g_uri_unescape_string(value.c_str(), nullptr);
      out[key] = dec ? std::string(dec) : value;
      if (dec) g_free(dec);
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}
#endif  // UR_HAVE_WEBKIT

}  // namespace

UpgradeSheet::UpgradeSheet(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance)
    : host_(host), balance_(balance) {
  EnsureDrawerCss();
  // the product name is never translated (the store marks it so)
  set_title("UR Pro");
  set_transient_for(parent);
  set_modal(true);
  set_default_size(440, -1);
  set_resizable(false);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  BuildUi();
}

void UpgradeSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->set_margin(24);
  set_child(*root);

  // ---- product options (mac UpgradeSubscriptionSheet) ------------------------
  optionsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* becomeA = Gtk::make_managed<Gtk::Label>(T_("become_a", "Become a"));
  becomeA->add_css_class("title-3");
  becomeA->set_xalign(0);
  optionsBox_->append(*becomeA);
  auto* supporter =
      Gtk::make_managed<Gtk::Label>(T_("urnetwork_supporter", "URnetwork Supporter"));
  supporter->add_css_class("title-1");
  supporter->set_xalign(0);
  optionsBox_->append(*supporter);

  auto* supportUs = Gtk::make_managed<Gtk::Label>(
      T_("support_us",
         "Support us in building a new kind of network that gives instead of takes."));
  supportUs->add_css_class("dim-label");
  supportUs->set_wrap(true);
  supportUs->set_xalign(0);
  supportUs->set_margin_top(16);
  optionsBox_->append(*supportUs);
  auto* unlock = Gtk::make_managed<Gtk::Label>(
      T_("unlock_speed",
         "You’ll unlock even faster speeds, and first dibs on new features like robust "
         "anti-censorship measures and data control."));
  unlock->add_css_class("dim-label");
  unlock->set_wrap(true);
  unlock->set_xalign(0);
  unlock->set_margin_top(8);
  optionsBox_->append(*unlock);

  // the plan picker the onboarding welcome page shows: yearly in the gold
  // dress with the trial, selected by default, monthly plain below it. One
  // component, so Get Pro and onboarding cannot drift.
  plans_ = Gtk::make_managed<PlanPicker>();
  plans_->set_margin_top(36);  // room for the halo and the Best value pill
  plans_->on_select = [this](bool yearly) {
    if (joinLabel_) joinLabel_->set_text(PlanPicker::CtaLabel(yearly));
  };
  optionsBox_->append(*plans_);

  joinBtn_ = Gtk::make_managed<Gtk::Button>();
  auto* joinContent = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  joinContent->set_halign(Gtk::Align::CENTER);
  joinSpinner_ = Gtk::make_managed<Gtk::Spinner>();
  joinSpinner_->set_visible(false);
  joinContent->append(*joinSpinner_);
  // only the yearly plan carries the trial: the button says what the click does
  joinLabel_ = Gtk::make_managed<Gtk::Label>(PlanPicker::CtaLabel(plans_->Yearly()));
  joinContent->append(*joinLabel_);
  joinBtn_->set_child(*joinContent);
  joinBtn_->add_css_class("suggested-action");
  joinBtn_->add_css_class("pill");
  joinBtn_->set_margin_top(20);
  joinBtn_->signal_clicked().connect([this] { StartCheckout(); });
  optionsBox_->append(*joinBtn_);

  errorLabel_ = Gtk::make_managed<Gtk::Label>();
  errorLabel_->add_css_class("ur-error-text");
  errorLabel_->add_css_class("caption");
  errorLabel_->set_wrap(true);
  errorLabel_->set_xalign(0);
  errorLabel_->set_visible(false);
  errorLabel_->set_margin_top(8);
  optionsBox_->append(*errorLabel_);

  auto* terms = Gtk::make_managed<Gtk::Label>();
  terms->set_markup(MarkdownLinksToPango(
      T_("by_subscribing_you_agree_to_urnetwork_s_terms",
         "By subscribing, you agree to URnetwork's [Terms and Services](https://ur.io/terms) "
         "and [Privacy Policy](https://ur.io/privacy)")));
  terms->add_css_class("dim-label");
  terms->add_css_class("caption");
  terms->set_wrap(true);
  terms->set_xalign(0);
  terms->set_margin_top(16);
  optionsBox_->append(*terms);

  root->append(*optionsBox_);

#ifdef UR_HAVE_WEBKIT
  // ---- embedded checkout (webview swaps in over the options) -----------------
  checkoutBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  checkoutBox_->set_visible(false);
  auto* checkoutHeader = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  // the product name is never translated (the store marks it so)
  auto* checkoutTitle = Gtk::make_managed<Gtk::Label>("UR Pro");
  checkoutTitle->add_css_class("title-3");
  checkoutTitle->set_xalign(0);
  checkoutTitle->set_hexpand(true);
  checkoutHeader->append(*checkoutTitle);
  auto* checkoutClose = Gtk::make_managed<Gtk::Button>();
  checkoutClose->set_icon_name("window-close-symbolic");
  checkoutClose->add_css_class("flat");
  checkoutClose->add_css_class("circular");
  checkoutClose->set_tooltip_text(T_("close", "Close"));
  // back to the products; the abandoned embedded session just expires
  checkoutClose->signal_clicked().connect([this] { SetState(State::Options); });
  checkoutHeader->append(*checkoutClose);
  checkoutBox_->append(*checkoutHeader);
  webViewSlot_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  webViewSlot_->set_vexpand(true);
  checkoutBox_->append(*webViewSlot_);
  root->append(*checkoutBox_);

  // hidden mid-checkout (Escape / window close): drop the webview so a stale
  // payment page never lingers behind a hidden window
  signal_hide().connect([this] {
    if (state_ == State::Checkout) SetState(State::Options);
  });
#endif

  // ---- waiting for the webhook ------------------------------------------------
  waitingBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  waitingBox_->set_visible(false);
  auto* waitSpinner = Gtk::make_managed<Gtk::Spinner>();
  waitSpinner->set_halign(Gtk::Align::CENTER);
  waitSpinner->set_size_request(32, 32);
  waitSpinner->start();
  waitingBox_->append(*waitSpinner);
  waitingLabel_ = Gtk::make_managed<Gtk::Label>(
      T_("checkout_opened_in_browser",
         "Complete your purchase in the browser. Your plan updates here automatically once "
         "payment is confirmed."));
  waitingLabel_->add_css_class("title-3");
  waitingLabel_->set_wrap(true);
  waitingLabel_->set_justify(Gtk::Justification::CENTER);
  waitingBox_->append(*waitingLabel_);
  auto* processing = Gtk::make_managed<Gtk::Label>(
      T_("processing_subscription_balance", "Processing subscription balance..."));
  processing->add_css_class("dim-label");
  processing->add_css_class("caption");
  processing->set_justify(Gtk::Justification::CENTER);
  waitingBox_->append(*processing);
  auto* waitClose = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  waitClose->set_halign(Gtk::Align::CENTER);
  waitClose->set_margin_top(8);
  // the poll keeps running in the store; Pro flips in the plan card regardless
  waitClose->signal_clicked().connect([this] { set_visible(false); });
  waitingBox_->append(*waitClose);
  root->append(*waitingBox_);

  // ---- success (mac PurchaseSuccessView) --------------------------------------
  successBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  successBox_->set_visible(false);
  auto* premium = Gtk::make_managed<Gtk::Label>(T_("you_re_premium", "You're premium."));
  premium->add_css_class("title-1");
  premium->set_xalign(0);
  successBox_->append(*premium);
  auto* thanks = Gtk::make_managed<Gtk::Label>(
      T_("thanks_for_building_the_new_internet_with_us",
         "Thanks for building the new internet with us"));
  thanks->add_css_class("dim-label");
  thanks->set_wrap(true);
  thanks->set_xalign(0);
  successBox_->append(*thanks);
  auto* successClose = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  successClose->add_css_class("suggested-action");
  successClose->add_css_class("pill");
  successClose->set_margin_top(24);
  successClose->signal_clicked().connect([this] { set_visible(false); });
  successBox_->append(*successClose);
  root->append(*successBox_);

  // ---- confirmation deadline passed --------------------------------------------
  timedOutBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  timedOutBox_->set_visible(false);
  auto* clock = Gtk::make_managed<Gtk::Image>();
  clock->set_from_icon_name("alarm-symbolic");
  clock->set_pixel_size(40);
  clock->add_css_class("dim-label");
  timedOutBox_->append(*clock);
  auto* waiting = Gtk::make_managed<Gtk::Label>(T_("waiting_for_approval", "Waiting for approval"));
  waiting->add_css_class("title-3");
  waiting->set_justify(Gtk::Justification::CENTER);
  timedOutBox_->append(*waiting);
  auto* timedOutBody = Gtk::make_managed<Gtk::Label>(
      T_("purchase_confirmation_timeout",
         "We couldn't confirm your purchase yet. If you completed checkout, your plan will "
         "update automatically in a few minutes — there's no need to buy again."));
  timedOutBody->add_css_class("dim-label");
  timedOutBody->set_wrap(true);
  timedOutBody->set_justify(Gtk::Justification::CENTER);
  timedOutBox_->append(*timedOutBody);
  auto* gotIt = Gtk::make_managed<Gtk::Button>(T_("got_it", "Got it"));
  gotIt->add_css_class("suggested-action");
  gotIt->set_halign(Gtk::Align::CENTER);
  gotIt->set_margin_top(8);
  gotIt->signal_clicked().connect([this] {
    balance_.ClearPurchaseConfirmationTimeout();
    set_visible(false);
  });
  timedOutBox_->append(*gotIt);
  root->append(*timedOutBox_);
}

void UpgradeSheet::SetState(State state) {
#ifdef UR_HAVE_WEBKIT
  if (state != State::Checkout) TeardownWebView();
  checkoutBox_->set_visible(state == State::Checkout);
#endif
  state_ = state;
  optionsBox_->set_visible(state == State::Options || state == State::Launching);
  waitingBox_->set_visible(state == State::Waiting);
  successBox_->set_visible(state == State::Success);
  timedOutBox_->set_visible(state == State::TimedOut);

  const bool launching = state == State::Launching;
  joinSpinner_->set_visible(launching);
  if (launching) {
    joinSpinner_->start();
  } else {
    joinSpinner_->stop();
  }
  joinBtn_->set_sensitive(!launching);
  plans_->set_sensitive(!launching);
}

void UpgradeSheet::Open() {
  ++*epoch_;
  errorLabel_->set_visible(false);
  plans_->Select(true);
  joinLabel_->set_text(PlanPicker::CtaLabel(true));
  // resuming a still-running confirmation poll re-opens onto the waiting state
  SetState(balance_.IsPolling() ? State::Waiting : State::Options);
  present();
}

void UpgradeSheet::OpenCheckout(bool yearly) {
  if (plans_) plans_->Select(yearly);
  if (joinLabel_) joinLabel_->set_text(PlanPicker::CtaLabel(yearly));
  present();
  StartCheckout();
}

void UpgradeSheet::StartCheckout() {
  if (state_ == State::Launching) return;
  SetState(State::Launching);
  errorLabel_->set_visible(false);
#ifdef UR_HAVE_WEBKIT
  // Embedded first — but only once a webview actually exists (webkit can be
  // present at build time and still unusable at runtime).
  if (EnsureWebView()) {
    RequestSession(/*embedded=*/true);
    return;
  }
#endif
  RequestSession(/*embedded=*/false);
}

void UpgradeSheet::RequestSession(bool embedded) {
  urnet::StripeCreateCheckoutSessionArgs args;
  args.item_id = plans_->Yearly() ? kItemProYearly : kItemProMonthly;
  args.ui_mode = embedded ? kUiModeEmbedded : kUiModeHosted;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().createStripeCheckoutSession(
      args, [this, epoch, issued,
             embedded](std::optional<urnet::StripeCreateCheckoutSessionResult> result,
                       std::optional<std::string> err) {
        PostToMain([this, epoch, issued, embedded, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != issued) return;  // sheet was reset since
          auto fail = [this](const std::string& message) {
            SetState(State::Options);
            errorLabel_->set_text(
                message.empty()
                    ? T_("something_went_wrong_please_try_again_later",
                         "Something went wrong. Please try again later.")
                    : message);
            errorLabel_->set_visible(true);
          };
          if (embedded) {
#ifdef UR_HAVE_WEBKIT
            // Any embedded failure — transport, server error, or a session
            // without a client_secret — retries once as hosted. Nothing was
            // shown yet, so no payment can be lost by switching.
            if (!err && result && !result->error && result->client_secret &&
                !result->client_secret->empty()) {
              OpenEmbedded(*result->client_secret);
            } else {
              RequestSession(/*embedded=*/false);
            }
#else
            fail("");  // unreachable: embedded is never requested without webkit
#endif
            return;
          }
          if (err) { fail(*err); return; }
          if (!result) { fail(""); return; }
          if (result->error) { fail(result->error->message); return; }
          if (!result->checkout_url || result->checkout_url->empty()) { fail(""); return; }

          // hosted checkout: hand off to the system browser, then poll so Pro
          // flips the moment the webhook lands
          GError* openErr = nullptr;
          if (!g_app_info_launch_default_for_uri(result->checkout_url->c_str(), nullptr,
                                                 &openErr)) {
            const std::string message = openErr ? openErr->message : "";
            if (openErr) g_error_free(openErr);
            fail(message);
            return;
          }
          waitingLabel_->set_text(
              T_("checkout_opened_in_browser",
                 "Complete your purchase in the browser. Your plan updates here automatically "
                 "once payment is confirmed."));
          balance_.StartConfirmationPolling();
          SetState(State::Waiting);
        });
      });
}

#ifdef UR_HAVE_WEBKIT

bool UpgradeSheet::EnsureWebView() {
  if (webView_) return true;
  GtkWidget* view = webkit_web_view_new();
  if (!view) return false;  // webkit unusable at runtime -> hosted checkout
  webView_ = view;
  // brand background while the page loads (avoids a white flash; the checkout
  // page itself is dark, matching the sheet)
  GdkRGBA bg;
  bg.red = static_cast<float>(kUrBackground.r);
  bg.green = static_cast<float>(kUrBackground.g);
  bg.blue = static_cast<float>(kUrBackground.b);
  bg.alpha = 1.0f;
  webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(view), &bg);
  gtk_widget_set_vexpand(view, TRUE);
  gtk_widget_set_size_request(view, -1, 560);

  // Signal handlers never touch the view synchronously: the members they call
  // defer through PostToMain, because tearing the webview down from inside its
  // own signal emission would free the emitter under itself.
  g_signal_connect(
      view, "decide-policy",
      G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision,
                     WebKitPolicyDecisionType type, gpointer data) -> gboolean {
        auto* self = static_cast<UpgradeSheet*>(data);
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
            type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
          return FALSE;  // responses etc: default handling
        }
        auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction* action =
            webkit_navigation_policy_decision_get_navigation_action(nav);
        WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
        const char* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
        if (!uri) return FALSE;
        if (g_str_has_prefix(uri, "urnetwork://")) {
          // the checkout page handing control back — never a real navigation
          webkit_policy_decision_ignore(decision);
          self->HandleCheckoutCallback(uri);
          return TRUE;
        }
        if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
          // target=_blank links (Stripe's terms/privacy): the system browser
          webkit_policy_decision_ignore(decision);
          g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
          return TRUE;
        }
        return FALSE;
      }),
      this);
  g_signal_connect(view, "load-changed",
                   G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer data) {
                     if (event == WEBKIT_LOAD_FINISHED) {
                       static_cast<UpgradeSheet*>(data)->pageLoaded_ = true;
                     }
                   }),
                   this);
  g_signal_connect(
      view, "load-failed",
      G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, char* failingUri, GError* error,
                     gpointer data) -> gboolean {
        // our own decide-policy ignore() lands here too; those are not failures
        if (failingUri && g_str_has_prefix(failingUri, "urnetwork://")) return TRUE;
        if (g_error_matches(error, WEBKIT_POLICY_ERROR,
                            WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE) ||
            g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) {
          return TRUE;
        }
        static_cast<UpgradeSheet*>(data)->OnCheckoutLoadFailed();
        return TRUE;  // the sheet handles it; no webkit error page
      }),
      this);
  g_signal_connect(view, "web-process-terminated",
                   G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason,
                                  gpointer data) {
                     // a dead web process cannot take a payment: back to the
                     // products with an inline error instead of a blank box
                     auto* self = static_cast<UpgradeSheet*>(data);
                     PostToMain([self] {
                       if (self->state_ != State::Checkout) return;
                       self->SetState(State::Options);
                       self->errorLabel_->set_text(
                           T_("something_went_wrong_please_try_again_later",
                              "Something went wrong. Please try again later."));
                       self->errorLabel_->set_visible(true);
                     });
                   }),
                   this);

  gtk_box_append(webViewSlot_->gobj(), view);
  return true;
}

void UpgradeSheet::OpenEmbedded(const std::string& clientSecret) {
  if (!EnsureWebView()) {  // torn down since the request went out
    RequestSession(/*embedded=*/false);
    return;
  }
  pageLoaded_ = false;
  webFallbackTried_ = false;
  const std::string url = std::string(kCheckoutPage) +
                          "?client_secret=" + Escape(clientSecret) +
                          "&redirect_link=" + Escape(kCheckoutRedirect);
  SetState(State::Checkout);
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webView_), url.c_str());
}

void UpgradeSheet::HandleCheckoutCallback(const std::string& uri) {
  // deferred: this arrives mid "decide-policy" emission
  PostToMain([this, uri] {
    const auto params = ParseQuery(uri);
    const auto status = params.find("status");
    if (status != params.end() && status->second == "complete") {
      // paid in the webview — the server only believes the Stripe webhook, so
      // bridge the gap with the confirmation poll exactly like hosted
      waitingLabel_->set_text(T_("processing_payment", "Processing payment"));
      balance_.StartConfirmationPolling();
      SetState(State::Waiting);
      return;
    }
    if (state_ != State::Checkout) return;  // stale error after close
    const auto message = params.find("errorMessage");
    SetState(State::Options);
    errorLabel_->set_text(message != params.end() && !message->second.empty()
                              ? message->second
                              : T_("something_went_wrong_please_try_again_later",
                                   "Something went wrong. Please try again later."));
    errorLabel_->set_visible(true);
  });
}

void UpgradeSheet::OnCheckoutLoadFailed() {
  // The checkout page could not load at all (offline webview, missing web
  // process, TLS failure). Nothing rendered, so nothing was paid — the hosted
  // flow can still save the purchase. Once per checkout attempt.
  if (pageLoaded_ || webFallbackTried_ || state_ != State::Checkout) return;
  webFallbackTried_ = true;
  PostToMain([this] {
    if (state_ != State::Checkout) return;  // closed in the meantime
    SetState(State::Launching);             // tears the webview down
    RequestSession(/*embedded=*/false);
  });
}

void UpgradeSheet::TeardownWebView() {
  if (!webView_) return;
  GtkWidget* view = webView_;
  webView_ = nullptr;
  pageLoaded_ = false;
  webFallbackTried_ = false;
  // deferred: teardown is reached from inside the view's own signal handlers,
  // and unparenting destroys the view. The slot outlives the sheet's states.
  GtkBox* slot = webViewSlot_->gobj();
  PostToMain([slot, view] { gtk_box_remove(slot, view); });
}

#endif  // UR_HAVE_WEBKIT

void UpgradeSheet::OnBalanceChanged() {
  // TimedOut is NOT terminal: a Pro-confirming snapshot can land after the
  // confirmation deadline gave up (the store falls back to the background
  // poll, and every window re-show fetches) — the purchase did go through,
  // so recover straight to the success state.
  if (state_ == State::TimedOut && balance_.IsPro()) {
    SetState(State::Success);
    return;
  }
  if (state_ != State::Waiting) return;
  if (balance_.IsPro()) {
    SetState(State::Success);
  } else if (balance_.PurchaseConfirmationTimedOut()) {
    SetState(State::TimedOut);
  }
}

}  // namespace urnw
