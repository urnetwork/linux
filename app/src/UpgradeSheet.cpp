// SPDX-License-Identifier: MPL-2.0
#include "UpgradeSheet.hpp"

#include "CryptoInvoiceHost.hpp"
#include "CryptoInvoiceView.hpp"
#include "PaymentMethodRow.hpp"
#include "PlanPicker.hpp"

#include <cstdio>

#include <gio/gio.h>

#ifdef UR_HAVE_WEBKIT
#include <algorithm>
#include <map>

#include <webkit/webkit.h>
#endif

#include <nlohmann/json.hpp>

#include "CheckoutBridgeError.hpp"
#include "CheckoutCrashRoute.hpp"
#include "CheckoutSessionMode.hpp"
#include "ClientEvents.hpp"
#include "DisplayText.hpp"
#include "GuestConversion.hpp"
#include "I18n.hpp"
#include "PricePresentation.hpp"
#include "ServerRefusalText.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr const char* kStoreStripe = "stripe";

// Canonical Stripe checkout items (server/controller/subscription_stripe_
// controller.go StripeItemPro*).
constexpr const char* kItemProMonthly = "pro_monthly";
constexpr const char* kItemProYearly = "pro_yearly";

#ifdef UR_HAVE_WEBKIT
// The ur.io bridge page (mmm/ur.io react EmbeddedCheckout.jsx): mounts
// Stripe's Embedded Checkout for the session's client_secret — the card form
// stays in Stripe's iframe, no card data ever touches the app — and hands
// control back by navigating to the redirect_link:
//   done:  urnetwork://checkout?status=complete&session_id=cs_...
//   error: urnetwork://checkout?errorCode=<code>&errorMessage=...
// (the error's code is one of urnet::CheckoutBridgeError*, CheckoutBridgeError.hpp)
// The session is redirect_on_completion "never" (CheckoutSessionMode.hpp), so
// the done hand-back comes from Stripe's onComplete on the bridge page, in
// place. The url and the hand-back are the SDK's envelope
// (urnet::buildInlineCheckoutBridgeUrl, urnet::parseCheckoutRedirect; windows
// UpgradeSheet parity). There is no
// cancel url: Stripe's embedded flow never leaves the page, so the sheet's
// own close (X) is the only way out.
// The ur.io embedded pay page (mmm/ur.io /app/pay-sheet): mounts Stripe's
// Payment Element for the payment sheet's client secret, confirms the intent,
// and hands control back by navigating to the return url (success) or posting
// {type: "ur-pay", status} to the host — the desktop forwards that message
// through a script message handler, so both signals land in the sheet.
constexpr const char* kPaySheetPage = "https://ur.io/app/pay-sheet";
constexpr const char* kPayReturn = "urnetwork://pay/done";
constexpr const char* kPayMessageHandler = "urpay";
constexpr const char* kPayMessageBridge =
    "window.addEventListener('message', function (e) {"
    "  var d = e && e.data;"
    "  if (typeof d === 'string') { try { d = JSON.parse(d); } catch (err) { return; } }"
    "  if (!d || d.type !== 'ur-pay') return;"
    "  var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.urpay;"
    "  if (h) h.postMessage(JSON.stringify(d));"
    "});";

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
  // the invoice controller's changes, on the GTK loop
  invoiceSubscription_ = host_.CryptoInvoice().Subscribe([this] { OnInvoiceChanged(); });
}

UpgradeSheet::~UpgradeSheet() {
  // before any member goes: the feed and the view's map signals call back
  // into this sheet
  host_.CryptoInvoice().Unsubscribe(invoiceSubscription_);
  invoiceMapped_.disconnect();
  invoiceUnmapped_.disconnect();
  if (suspendedHandler_) g_signal_handler_disconnect(gobj(), suspendedHandler_);
  if (invoiceViewer_) host_.CryptoInvoice().RemoveVisibleViewer();
}

void UpgradeSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->set_margin(24);
  set_child(*root);

  // ---- product options (mac UpgradeSubscriptionSheet) ------------------------
  optionsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  // "Get Pro", the same heading onboarding, Android and Apple give this sheet
  auto* proTitle = Gtk::make_managed<Gtk::Label>(T_("get_pro", "Get Pro"));
  proTitle->add_css_class("title-1");
  proTitle->set_xalign(0);
  optionsBox_->append(*proTitle);

  // No explainer under the title: the sheet is the title and the two plan
  // options (android UpgradeScreenHeader). A blocked connect is the exception:
  // upgrading must not read as the only way back, so the sheet says when the
  // free data refreshes and offers to wait for it (Open).
  freeRefreshBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  freeRefreshBox_->set_margin_top(8);
  freeRefreshLabel_ = Gtk::make_managed<Gtk::Label>();
  freeRefreshLabel_->add_css_class("dim-label");
  freeRefreshLabel_->set_xalign(0);
  freeRefreshLabel_->set_wrap(true);
  freeRefreshBox_->append(*freeRefreshLabel_);
  // reserved data may come back before the refresh; used up does not
  freeRefreshKindLabel_ = Gtk::make_managed<Gtk::Label>();
  freeRefreshKindLabel_->add_css_class("dim-label");
  freeRefreshKindLabel_->set_xalign(0);
  freeRefreshKindLabel_->set_wrap(true);
  freeRefreshBox_->append(*freeRefreshKindLabel_);
  auto* waitForRefresh = Gtk::make_managed<Gtk::Button>(T_("wait_for_refresh", "Wait for refresh"));
  waitForRefresh->signal_clicked().connect([this] { set_visible(false); });
  freeRefreshBox_->append(*waitForRefresh);
  freeRefreshBox_->set_visible(false);
  optionsBox_->append(*freeRefreshBox_);
  signal_hide().connect([this] { freeRefreshTicker_.Stop(); });

  // the plan picker the onboarding welcome page shows: yearly in the gold
  // dress with the trial, selected by default, monthly plain below it. One
  // component, so Get Pro and onboarding cannot drift.
  plans_ = Gtk::make_managed<PlanPicker>();
  plans_->set_margin_top(36);  // room for the halo and the Best value pill
  plans_->on_select = [this](bool) {
    // a coin reads "Pay with {method}" whatever the plan (monthly is greyed
    // for it anyway)
    if (joinLabel_) joinLabel_->set_text(JoinLabel());
  };
  optionsBox_->append(*plans_);

  // the network's welcome offer while it is active (read-only: the picker's
  // yearly card already prints the first-year price; this is the deadline
  // and the terms). Issued elsewhere — the sheet never issues one.
  offerLine_ = Gtk::make_managed<OfferCard>(/*compact=*/true);
  offerLine_->set_margin_top(16);
  offerLine_->set_visible(false);
  optionsBox_->append(*offerLine_);

  // Card · USDC on Solana · Zcash · Monero · More coins, directly above the
  // button; hidden while the server lists no coin, so the sheet then looks as
  // it always did
  methodRow_ = Gtk::make_managed<PaymentMethodRow>();
  methodRow_->set_margin_top(20);
  methodRow_->on_select = [this](const std::string& method) {
    method_ = method;
    methodPicked_ = true;
    // the last choice, remembered on this device
    host_.CryptoInvoice().SetPaymentMethod(method);
    errorLabel_->set_visible(false);
    ApplyMethod();
  };
  optionsBox_->append(*methodRow_);

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
  joinBtn_->signal_clicked().connect([this] {
    if (crypto_pay::IsCoin(method_)) {
      // a coin buys the yearly plan, or the welcome offer while it is active
      StartCrypto(crypto_pay::CoinPlan(balance_.Offer().active));
    } else {
      StartCheckout();
    }
  });
  optionsBox_->append(*joinBtn_);

  // a coin's terms under its button: one payment, no renewal, no trial
  finePrint_ = Gtk::make_managed<Gtk::Label>(
      T_("crypto_fine_print_yearly",
         "A one-time payment for a year of Pro. It doesn't renew, and there's no free trial."));
  finePrint_->add_css_class("dim-label");
  finePrint_->add_css_class("caption");
  finePrint_->set_wrap(true);
  finePrint_->set_xalign(0);
  finePrint_->set_margin_top(8);
  finePrint_->set_visible(false);
  optionsBox_->append(*finePrint_);

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

  // ---- the crypto invoice (a state of this sheet, CryptoInvoiceView) ---------
  invoiceBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  invoiceBox_->set_visible(false);
  // the invoice is taller than the options: it scrolls past a small screen
  auto* invoiceScroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  invoiceScroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  invoiceScroll->set_propagate_natural_height(true);
  invoiceScroll->set_max_content_height(640);
  invoiceView_ = Gtk::make_managed<CryptoInvoiceView>();
  invoiceView_->on_refresh = [this] { host_.CryptoInvoice().Requote(); };
  invoiceView_->on_pay_another_way = [this] {
    if (!purchaseEmitted_) {
      EmitPurchase("cancelled");
      purchaseEmitted_ = true;
    }
    // with nothing received the saved invoice goes (the controller's rule)
    host_.CryptoInvoice().Dismiss();
    // back to the method row, with Card
    SelectMethod(crypto_pay::kMethodCard);
    host_.CryptoInvoice().SetPaymentMethod(crypto_pay::kMethodCard);
    SetState(State::Options);
  };
  // the invoice stays saved: Account's "Payment in progress" opens it again
  invoiceView_->on_close = [this] { set_visible(false); };
  // On screen exactly while mapped (the sheet shown in the Invoice state) and
  // not minimized: the controller polls only then (no background checks).
  invoiceMapped_ = invoiceView_->signal_map().connect([this] { SetInvoiceViewer(!Suspended()); });
  invoiceUnmapped_ = invoiceView_->signal_unmap().connect([this] { SetInvoiceViewer(false); });
#if GTK_CHECK_VERSION(4, 12, 0)
  suspendedHandler_ = g_signal_connect(
      gobj(), "notify::suspended", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto* self = static_cast<UpgradeSheet*>(data);
        self->SetInvoiceViewer(self->invoiceView_->get_mapped() && !self->Suspended());
      }),
      this);
#endif
  invoiceScroll->set_child(*invoiceView_);
  invoiceBox_->append(*invoiceScroll);
  root->append(*invoiceBox_);

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
  checkoutClose->signal_clicked().connect([this] {
    if (state_ == State::Checkout && !purchaseEmitted_) {
      EmitPurchase("cancelled");
      purchaseEmitted_ = true;
    }
    SetState(State::Options);
  });
  checkoutHeader->append(*checkoutClose);
  checkoutBox_->append(*checkoutHeader);
  webViewSlot_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  webViewSlot_->set_vexpand(true);
  checkoutBox_->append(*webViewSlot_);
  root->append(*checkoutBox_);

  // hidden mid-checkout (Escape / window close): drop the webview so a stale
  // payment page never lingers behind a hidden window
  signal_hide().connect([this] {
    if (state_ == State::Checkout) {
      if (!purchaseEmitted_) {
        EmitPurchase("cancelled");
        purchaseEmitted_ = true;
      }
      SetState(State::Options);
    }
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
  // the account the plan is on (SetState fills it in); hidden for a guest and
  // without a name
  successAccount_ = Gtk::make_managed<Gtk::Label>();
  successAccount_->add_css_class("dim-label");
  successAccount_->set_wrap(true);
  successAccount_->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  successAccount_->set_xalign(0);
  successAccount_->set_visible(false);
  successBox_->append(*successAccount_);
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
  if (state == State::Success && state_ != State::Success && !purchaseEmitted_) {
    purchaseEmitted_ = true;
    EmitPurchase("completed");
  }
  if (state == State::Success) {
    // the account the plan is on, read now: the sheet outlives a sign-in. The
    // sdk's rule gives "" for a guest or without a name.
    const std::optional<urnet::ByJwt> jwt =
        host_.IsLoggedIn() ? host_.ParseByJwt() : std::nullopt;
    const std::string account =
        jwt ? urnet::planAccountName(SanitizeExternalDisplayText(jwt->NetworkName),
                                     jwt->GuestMode || balance_.IsGuest())
            : std::string();
    if (account.empty()) {
      successAccount_->set_text({});
    } else {
      successAccount_->set_text(Format(T_("plan_account_name", "Account: {}"), account));
    }
    successAccount_->set_visible(!account.empty());
  }
  state_ = state;
  optionsBox_->set_visible(state == State::Options || state == State::Launching);
  invoiceBox_->set_visible(state == State::Invoice);
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
  // held, not insensitive: an insensitive row would dim the coins' marks
  methodRow_->SetEnabled(!launching);
}

void UpgradeSheet::ApplyPrices() {
  const PriceTierView& tier = balance_.Tier();
  const OfferView& offer = balance_.Offer();
  if (plans_) plans_->SetPrices(tier, offer);
  if (offerLine_) {
    offerLine_->set_visible(offer.active);
    if (offer.active) offerLine_->Update(offer, tier, kFreeTrialDays);
  }
}

void UpgradeSheet::EmitPurchase(const char* outcome, const std::string& errorClass) {
  // A coin's purchase names its market as the store (§6.1, the SDK's
  // urnet::cryptoAssetMarket: solana, zcash, monero, bittensor, bitcoin,
  // ethereum) and the SDK's product for the coin (pro_yearly_<ticker>: USDC
  // and SOL share the solana store), buys the yearly plan, and has no trial;
  // the card's is Stripe's, with its plan's product.
  const bool coin = crypto_pay::IsCoin(purchaseMethod_);
  const bool yearly = coin || plans_->Yearly();
  const PriceTierView& tier = balance_.Tier();
  const OfferView& offer = balance_.Offer();
  const bool offerPrice = coin ? purchasePlan_ == crypto_pay::kPlanYearlyOnboarding : offer.active;
  double price = yearly ? (offerPrice ? offer.firstYear : tier.yearly) : tier.monthly;
  // once a coin's invoice is quoted, the server's price is what it charges
  if (coin) {
    const crypto_pay::InvoiceView& invoice = host_.CryptoInvoice().State();
    if (invoice.asset == purchaseMethod_ && 0 < invoice.priceUsd) price = invoice.priceUsd;
  }
  const std::string store = coin ? PaymentMethodEventStore(purchaseMethod_) : kStoreStripe;
  const std::string coinProduct = coin ? PaymentMethodEventProduct(purchaseMethod_) : std::string();
  const std::string product = coinProduct.empty() ? std::string(PlanProduct(yearly)) : coinProduct;
  const bool trial = yearly && !coin;
  const std::string out(outcome);
  ClientEventQueue& events = host_.events();
  if (out == "started") {
    events.PurchaseStarted(store, product, PlanName(yearly), trial, price, tier.currency);
  } else if (out == "completed") {
    events.PurchaseCompleted(store, product, PlanName(yearly), trial, price, tier.currency);
  } else if (out == "cancelled") {
    events.PurchaseCancelled(store, product, PlanName(yearly), trial, price, tier.currency);
  } else {
    events.PurchaseFailed(store, product, PlanName(yearly), trial, price, tier.currency,
                          errorClass);
  }
}

void UpgradeSheet::Open(bool freeRefresh) {
  ++*epoch_;
  freeRefreshBox_->set_visible(freeRefresh);
  if (freeRefresh) {
    freeRefreshTicker_.Start([this] {
      freeRefreshLabel_->set_text(
          Format(T_("insufficient_balance_refreshes_in", "Free data refreshes in {}."),
                 FreeRefreshCountdownText()));
    });
    balance_notice::AccountBalance read;
    read.known = balance_.HasFetched();
    read.pro = balance_.IsPro();
    read.availableBytes = balance_.AvailableByteCount();
    read.openTransferBytes = balance_.PendingByteCount();
    read.fetchedAtMillis = balance_.FetchedAtMillis();
    const std::string kind =
        OutOfBalanceKindText(balance_notice::OutOfBalanceKindFor(read), balance_.PendingByteCount());
    freeRefreshKindLabel_->set_text(kind);
    freeRefreshKindLabel_->set_visible(!kind.empty());
  } else {
    freeRefreshTicker_.Stop();
  }
  errorLabel_->set_visible(false);
  cryptoStarting_ = false;
  unavailableAsset_.clear();
  methodPicked_ = false;
  ApplyPrices();
  plans_->Select(true);
  // the method this device chose last, while the server still offers it
  RefreshMethods();
  SelectMethod(host_.CryptoInvoice().PaymentMethod());
  // resuming a still-running confirmation poll re-opens onto the waiting
  // state, and a crypto payment going on onto its invoice
  if (balance_.IsPolling()) {
    SetState(State::Waiting);
  } else if (crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())) {
    ShowInvoice();
  } else {
    SetState(State::Options);
  }
  present();
}

void UpgradeSheet::OpenCheckout(bool yearly) {
  ApplyPrices();
  // the onboarding chose the card
  RefreshMethods();
  SelectMethod(crypto_pay::kMethodCard);
  if (plans_) plans_->Select(yearly);
  if (joinLabel_) joinLabel_->set_text(PlanPicker::CtaLabel(yearly));
  present();
  StartCheckout();
}

void UpgradeSheet::OpenCryptoCheckout(const std::string& asset, const std::string& plan) {
  ++*epoch_;
  freeRefreshBox_->set_visible(false);
  freeRefreshTicker_.Stop();
  errorLabel_->set_visible(false);
  cryptoStarting_ = false;
  unavailableAsset_.clear();
  methodPicked_ = true;  // the onboarding's row picked it
  ApplyPrices();
  plans_->Select(true);
  RefreshMethods();
  SelectMethod(asset);
  present();
  // a payment already going on is shown, never replaced by a second invoice
  if (crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())) {
    ShowInvoice();
    return;
  }
  SetState(State::Options);
  // the coin is still offered (else the row fell back to Card and the
  // options show it)
  if (crypto_pay::IsCoin(method_)) StartCrypto(plan);
}

void UpgradeSheet::OpenInvoice() {
  if (!crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())) {
    Open();
    return;
  }
  ++*epoch_;
  freeRefreshBox_->set_visible(false);
  freeRefreshTicker_.Stop();
  errorLabel_->set_visible(false);
  cryptoStarting_ = false;
  methodPicked_ = false;
  ApplyPrices();
  plans_->Select(true);
  RefreshMethods();
  SelectMethod(host_.CryptoInvoice().State().asset);
  ShowInvoice();
  present();
}

void UpgradeSheet::StartCheckout() {
  // no checkout without a session (windows UpgradeSheet::BeginCheckout parity)
  if (state_ == State::Launching || !host_.IsLoggedIn()) return;
  SetState(State::Launching);
  errorLabel_->set_visible(false);
  purchaseEmitted_ = false;
  paySheetSubscriptionId_.clear();
  checkoutSessionId_.clear();
  cardPaymentReported_ = false;
  purchaseMethod_ = crypto_pay::kMethodCard;
  purchasePlan_.clear();
  EmitPurchase("started");
  // A legacy guest network buys nothing. The owners gate their entries
  // (MainWindow::OpenUpgrade, OnboardingWindow::StartCheckout); this is a
  // guest the balance reported after the sheet opened. As for the server's
  // refusal, the sheet hides and its owner opens the conversion.
  if (balance_.IsGuest()) {
    RefuseForGuest();
    return;
  }
#ifdef UR_HAVE_WEBKIT
  // The inline payment sheet first, then embedded checkout — but only once a
  // webview actually exists (webkit can be present at build time and still
  // unusable at runtime).
  if (EnsureWebView()) {
    RequestPaymentSheet();
    return;
  }
#endif
  RequestSession(/*embedded=*/false);
}

void UpgradeSheet::RequestPaymentSheet() {
#ifdef UR_HAVE_WEBKIT
  urnet::StripePaymentSheetArgs args;
  args.plan = PlanName(plans_->Yearly());
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().stripePaymentSheet(
      args, [this, epoch, issued](std::optional<urnet::StripePaymentSheetResult> result,
                                  std::optional<std::string> err) {
        PostToMain([this, epoch, issued, result = std::move(result), err = std::move(err)] {
          if (*epoch != issued) return;  // sheet was reset since
          if (state_ != State::Launching) return;
          if (result && result->error &&
              PurchaseRefusalFor(result->error->code.value_or(std::string())) ==
                  PurchaseRefusal::AddSignIn) {
            RefuseForGuest();
            return;
          }
          // the intent to confirm: the SetupIntent for the yearly plan (the
          // trial defers the charge), the PaymentIntent for monthly
          std::string clientSecret;
          if (!err && result && !result->error) {
            if (result->setup_intent_client_secret && !result->setup_intent_client_secret->empty()) {
              clientSecret = *result->setup_intent_client_secret;
            } else if (result->payment_intent_client_secret &&
                       !result->payment_intent_client_secret->empty()) {
              clientSecret = *result->payment_intent_client_secret;
            }
          }
          if (clientSecret.empty() || !result->publishable_key || result->publishable_key->empty()) {
            // nothing rendered yet: the embedded checkout session saves the purchase
            if (err) std::fprintf(stderr, "[upgrade] payment sheet failed: %s\n", err->c_str());
            RequestSession(/*embedded=*/true);
            return;
          }
          // what the finished payment is reported by (ReportCardPayment)
          paySheetSubscriptionId_ = result->subscription_id.value_or(std::string());
          OpenPaySheet(clientSecret, *result->publishable_key);
        });
      });
#endif
}

void UpgradeSheet::RefuseForGuest() {
  if (!purchaseEmitted_) {
    EmitPurchase("failed", kPurchaseErrorCodeGuestSignInRequired);
    purchaseEmitted_ = true;
  }
  SetState(State::Options);
  hide();
  if (on_guest_sign_in_required) on_guest_sign_in_required();
}

void UpgradeSheet::RequestSession(bool embedded) {
  // reached from StartCheckout, directly or as a later leg's fallback: the
  // session may have ended, or the balance named a guest, since its gate
  if (!host_.IsLoggedIn()) {
    // windows UpgradeSheet::RequestSession parity
    if (!purchaseEmitted_) {
      EmitPurchase("failed", "transport");
      purchaseEmitted_ = true;
    }
    SetState(State::Options);
    errorLabel_->set_text(T_("something_went_wrong_please_try_again_later",
                             "Something went wrong. Please try again later."));
    errorLabel_->set_visible(true);
    return;
  }
  if (balance_.IsGuest()) {
    RefuseForGuest();
    return;
  }
  urnet::StripeCreateCheckoutSessionArgs args;
  args.item_id = plans_->Yearly() ? kItemProYearly : kItemProMonthly;
  const CheckoutSessionMode mode = CheckoutSessionModeFor(embedded);
  args.ui_mode = mode.uiMode;
  if (!mode.redirectOnCompletion.empty()) {
    args.redirect_on_completion = mode.redirectOnCompletion;
  }
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().createStripeCheckoutSession(
      args, [this, epoch, issued,
             embedded](std::optional<urnet::StripeCreateCheckoutSessionResult> result,
                       std::optional<std::string> err) {
        PostToMain([this, epoch, issued, embedded, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != issued) return;  // sheet was reset since
          if (result && result->error &&
              PurchaseRefusalFor(result->error->code.value_or(std::string())) ==
                  PurchaseRefusal::AddSignIn) {
            // a guest network: no other session can sell it a plan either
            RefuseForGuest();
            return;
          }
          // the session this attempt goes on with, which the finished payment
          // is reported by (ReportCardPayment); a fallback's replaces it
          checkoutSessionId_ =
              result && result->session_id ? *result->session_id : std::string();
          auto fail = [this](const std::string& message) {
            if (!purchaseEmitted_) {
              EmitPurchase("failed", "transport");
              purchaseEmitted_ = true;
            }
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
          if (result->error) {
            // a refusal reads as its code's line, or as the sheet's own line with
            // the server's words under it (ServerRefusalText.hpp)
            const ServerRefusalText refusal =
                ServerRefusalTextFor(result->error->code.value_or(std::string()),
                                     result->error->message, kUpgradeSheetRefusalLine);
            fail(ServerRefusalDisplay(T_(refusal.line.key, refusal.line.english), refusal));
            return;
          }
          if (!result->checkout_url || result->checkout_url->empty()) { fail(""); return; }

          // hosted checkout: hand off to the system browser, then report the
          // session (the sdk asks again while the browser payment is pending)
          // and poll so Pro flips the moment the server credits it
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
          ReportCardPayment(/*paySheet=*/false);
          balance_.StartConfirmationPolling();
          SetState(State::Waiting);
        });
      });
}

void UpgradeSheet::ReportCardPayment(bool paySheet) {
  // once: the pay page can both post its result and navigate to the return url
  if (cardPaymentReported_ || !host_.IsLoggedIn()) return;
  // exactly one id; a server that answered none leaves the webhook to credit it
  urnet::VerifyStripeCheckoutArgs args;
  if (paySheet) {
    if (paySheetSubscriptionId_.empty()) return;
    args.subscription_id = paySheetSubscriptionId_;
  } else {
    if (checkoutSessionId_.empty()) return;
    args.session_id = checkoutSessionId_;
  }
  cardPaymentReported_ = true;
  host_.api().reportStripePurchase(
      args, [this](std::optional<urnet::VerifyStorePurchaseResult> result,
                   std::optional<std::string> err) {
        // once, after the sdk's retries. No epoch check: the payment was made
        // whatever the sheet shows now, and the sheet lives with its window.
        PostToMain([this, result = std::move(result), err = std::move(err)] {
          if (err) std::fprintf(stderr, "[upgrade] card payment report failed: %s\n", err->c_str());
          if (!result) return;
          std::fprintf(stderr, "[upgrade] card payment report: %s\n", result->status.c_str());
          // the server holds the credit: read the balance now, which also
          // takes a timed-out wait to the success state
          if (result->status == urnet::PurchaseReportStatusCredited ||
              result->status == urnet::PurchaseReportStatusAlreadyCredited) {
            balance_.FetchNow();
          }
        });
      });
}

#ifdef UR_HAVE_WEBKIT

bool UpgradeSheet::EnsureWebView() {
  if (webView_) return true;
  GtkWidget* view = webkit_web_view_new();
  if (!view) return false;  // webkit unusable at runtime -> hosted checkout
  webView_ = view;
  // the pay page's posted {type:"ur-pay"} message: a page-side bridge forwards
  // it to a script message handler, so the sheet hears it even when the page
  // does not navigate to the return url
  if (WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(view))) {
    g_signal_connect(ucm, "script-message-received::urpay",
                     G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* value, gpointer data) {
                       if (!value || !jsc_value_is_string(value)) return;
                       char* text = jsc_value_to_string(value);
                       const std::string json = text ? text : "";
                       if (text) g_free(text);
                       static_cast<UpgradeSheet*>(data)->HandlePayMessage(json);
                     }),
                     this);
    webkit_user_content_manager_register_script_message_handler(ucm, kPayMessageHandler, nullptr);
    WebKitUserScript* bridge = webkit_user_script_new(
        kPayMessageBridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, bridge);
    webkit_user_script_unref(bridge);
  }
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
                     static_cast<UpgradeSheet*>(data)->OnWebProcessTerminated();
                   }),
                   this);

  gtk_box_append(webViewSlot_->gobj(), view);
  return true;
}

void UpgradeSheet::OpenPaySheet(const std::string& clientSecret, const std::string& publishableKey) {
  if (!EnsureWebView()) {  // torn down since the request went out
    RequestSession(/*embedded=*/true);
    return;
  }
  pageLoaded_ = false;
  webFallbackTried_ = false;
  paySheetActive_ = true;
  const std::string url = std::string(kPaySheetPage) + "?cs=" + Escape(clientSecret) +
                          "&pk=" + Escape(publishableKey) +
                          "&plan=" + Escape(PlanName(plans_->Yearly())) +
                          "&return=" + Escape(kPayReturn);
  SetState(State::Checkout);
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webView_), url.c_str());
}

void UpgradeSheet::HandlePayMessage(const std::string& json) {
  // deferred: this arrives from the web view's own signal emission
  PostToMain([this, json] {
    if (state_ != State::Checkout || !paySheetActive_) return;
    nlohmann::json message = nlohmann::json::parse(json, nullptr, false);
    if (!message.is_object()) return;
    const std::string status = message.value("status", std::string());
    if (status == "succeeded" || status == "complete" || status == "processing" || status == "ok") {
      HandleCheckoutCallback(std::string(kPayReturn));
      return;
    }
    if (status == "cancelled" || status == "canceled") {
      EmitPurchase("cancelled");
      purchaseEmitted_ = true;
      SetState(State::Options);
      return;
    }
    const std::string detail = message.value("message", std::string());
    HandleCheckoutCallback("urnetwork://pay/error?errorMessage=" + Escape(detail));
  });
}

void UpgradeSheet::OpenEmbedded(const std::string& clientSecret) {
  if (!EnsureWebView()) {  // torn down since the request went out
    RequestSession(/*embedded=*/false);
    return;
  }
  pageLoaded_ = false;
  webFallbackTried_ = false;
  paySheetActive_ = false;
  // the session is "never" (CheckoutSessionModeFor(true)): the bridge hands
  // back from Stripe's onComplete
  const std::string url = urnet::buildInlineCheckoutBridgeUrl(clientSecret);
  SetState(State::Checkout);
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webView_), url.c_str());
}

void UpgradeSheet::HandleCheckoutCallback(const std::string& uri) {
  // deferred: this arrives mid "decide-policy" emission
  PostToMain([this, uri] {
    // the checkout bridge's hand-back is the SDK's envelope; the pay page's
    // (urnetwork://pay/done, urnetwork://pay/error?errorMessage=) is this sheet's
    const bool checkoutRedirect = urnet::isCheckoutRedirect(uri);
    bool complete = false;
    std::string sessionId;
    std::string errorCode;
    std::string errorMessage;
    if (checkoutRedirect) {
      try {
        if (auto redirect = urnet::parseCheckoutRedirect(uri)) {
          complete = redirect->Complete;
          sessionId = redirect->SessionId;
          errorCode = redirect->ErrorCode;
          errorMessage = redirect->ErrorMessage;
        }
      } catch (...) {
        // a malformed hand-back: an error with no message of its own
      }
    } else {
      complete = uri.rfind(kPayReturn, 0) == 0;
      const auto params = ParseQuery(uri);
      if (const auto message = params.find("errorMessage"); message != params.end()) {
        errorMessage = message->second;
      }
    }
    if (complete) {
      // paid in the webview — a client saying "I paid" is not evidence, so the
      // report hands the server the payment's id to read back from Stripe (the
      // session the bridge names, else the one it was opened with), and the
      // confirmation poll shows the result exactly like hosted
      if (!sessionId.empty()) checkoutSessionId_ = sessionId;
      ReportCardPayment(/*paySheet=*/!checkoutRedirect);
      waitingLabel_->set_text(T_("processing_payment", "Processing payment"));
      balance_.StartConfirmationPolling();
      SetState(State::Waiting);
      return;
    }
    if (state_ != State::Checkout) return;  // stale error after close
    EmitPurchase("failed", paySheetActive_ ? "payment_sheet" : "checkout");
    purchaseEmitted_ = true;
    SetState(State::Options);
    // the page's code in this app's words when it knows it, else the page's text
    const CheckoutFailureText failure = CheckoutFailureTextFor(errorCode, errorMessage);
    errorLabel_->set_text(failure.key
                              ? Glib::ustring(g_dpgettext2(GETTEXT_PACKAGE, failure.key,
                                                           failure.english))
                              : Glib::ustring(failure.pageText));
    errorLabel_->set_visible(true);
  });
}

void UpgradeSheet::OnCheckoutLoadFailed() {
  // The checkout page could not load at all (offline webview, missing web
  // process, TLS failure). Nothing rendered, so nothing was paid — the hosted
  // flow can still save the purchase. Once per checkout attempt.
  if (pageLoaded_ || webFallbackTried_ || state_ != State::Checkout) return;
  webFallbackTried_ = true;
  const bool fromPaySheet = paySheetActive_;
  PostToMain([this, fromPaySheet] {
    if (state_ != State::Checkout) return;  // closed in the meantime
    SetState(State::Launching);             // tears the webview down
    // the pay page failing falls back to the embedded checkout page; that
    // failing falls back to the browser
    RequestSession(/*embedded=*/fromPaySheet);
  });
}

void UpgradeSheet::OnWebProcessTerminated() {
  // deferred: this arrives from the view's own signal emission, and every
  // route below tears the dead view down
  PostToMain([this] {
    if (state_ != State::Checkout) return;
    switch (RouteCheckoutCrash(pageLoaded_, webFallbackTried_)) {
      case CheckoutCrashRoute::ConfirmPayment:
        // died after the form rendered: the card may have been charged with
        // only the hand-back lost. Confirm with the server rather than
        // inviting a second purchase.
        waitingLabel_->set_text(
            T_("checkout_interrupted_confirming",
               "The checkout view closed unexpectedly. If your payment went through, your plan "
               "updates here automatically — there's no need to buy again."));
        ReportCardPayment(/*paySheet=*/paySheetActive_);
        balance_.StartConfirmationPolling();
        SetState(State::Waiting);
        return;
      case CheckoutCrashRoute::RetryCheckout:
        // died before the page rendered: nothing was paid, the next checkout
        // path can still save the purchase
        OnCheckoutLoadFailed();
        return;
      case CheckoutCrashRoute::ShowError:
        if (!purchaseEmitted_) {
          EmitPurchase("failed", paySheetActive_ ? "payment_sheet" : "checkout");
          purchaseEmitted_ = true;
        }
        SetState(State::Options);
        errorLabel_->set_text(T_("something_went_wrong_please_try_again_later",
                                 "Something went wrong. Please try again later."));
        errorLabel_->set_visible(true);
        return;
    }
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
  // The coins the server lists can change with any fetch: the row follows
  // while the options are what is on screen, keeping a method picked in this
  // open when it is still offered.
  if (state_ == State::Options && RefreshMethods()) {
    SelectMethod(methodPicked_ ? method_ : host_.CryptoInvoice().PaymentMethod());
  }
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

// ---- crypto (mmm/ZCASH-CONTRACT.md §6.1, §6.2) -------------------------------------------

bool UpgradeSheet::RefreshMethods() {
  return methodRow_->SetTabs(balance_.PaymentTabs(crypto_pay::CoinPlan(balance_.Offer().active)));
}

void UpgradeSheet::SelectMethod(const std::string& method) {
  methodRow_->Select(method);
  method_ = methodRow_->Selected();
  ApplyMethod();
}

void UpgradeSheet::ApplyMethod() {
  const bool coin = crypto_pay::IsCoin(method_);
  // Monthly is card only: a coin greys it out and buys the yearly plan, once,
  // with no trial and no renewal (no such wording while it is selected)
  plans_->SetMonthlyAllowed(!coin);
  plans_->SetCardTermsShown(!coin);
  if (coin && !plans_->Yearly()) plans_->Select(true);
  joinLabel_->set_text(JoinLabel());
  finePrint_->set_visible(coin);
  if (offerLine_) offerLine_->SetCardTermsShown(!coin);
  // a coin that could not be quoted stays selectable for a retry
  methodRow_->SetUnavailable(coin && method_ == unavailableAsset_);
}

Glib::ustring UpgradeSheet::JoinLabel() const {
  if (crypto_pay::IsCoin(method_)) {
    return Format(T_("crypto_pay_button", "Pay with {}"), PaymentMethodName(method_).raw());
  }
  // only the yearly plan carries the trial: the button says what the click does
  return PlanPicker::CtaLabel(plans_->Yearly());
}

void UpgradeSheet::StartCrypto(const std::string& plan) {
  if (state_ == State::Launching) return;
  // One invoice at a time (CONTRACT §6.2.1): a payment going on is shown,
  // never replaced by a second invoice the app would lose track of.
  if (crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())) {
    ShowInvoice();
    return;
  }
  SetState(State::Launching);
  errorLabel_->set_visible(false);
  purchaseEmitted_ = false;
  purchaseMethod_ = method_;
  purchasePlan_ = plan;
  EmitPurchase("started");
  // set before the call: the controller reports "starting" at once
  cryptoStarting_ = true;
  if (!host_.CryptoInvoice().StartInvoice(method_, plan)) {
    // no controller (no session to start one with)
    cryptoStarting_ = false;
    crypto_pay::InvoiceView failed;
    failed.phase = crypto_pay::kPhaseFailed;
    failed.asset = method_;
    OnCryptoStartFailed(failed);
  }
}

void UpgradeSheet::ShowInvoice() {
  const crypto_pay::InvoiceView& invoice = host_.CryptoInvoice().State();
  // the purchase the events describe, also for an invoice resumed from an
  // earlier run: its completion is reported once, when it is paid here
  purchaseMethod_ = invoice.asset;
  purchasePlan_ = invoice.plan;
  purchaseEmitted_ = false;
  invoiceView_->Render(invoice);
  SetState(State::Invoice);
}

void UpgradeSheet::OnInvoiceChanged() {
  // A hidden sheet waits for its next open, which reads the invoice afresh.
  // Only the sheet on screen acts on a change, so a payment is completed once.
  if (!get_visible()) return;
  const crypto_pay::InvoiceView& invoice = host_.CryptoInvoice().State();
  const crypto_pay::InvoiceStage stage = crypto_pay::StageOf(invoice);
  if (state_ == State::Launching && cryptoStarting_) {
    switch (stage) {
      case crypto_pay::InvoiceStage::Starting:
        return;  // the quote is on its way
      case crypto_pay::InvoiceStage::Open:
        cryptoStarting_ = false;
        unavailableAsset_.clear();
        ShowInvoice();
        return;
      case crypto_pay::InvoiceStage::Paid:
        // the network's open invoice for the same coin and plan, paid already
        cryptoStarting_ = false;
        OnCryptoPaid();
        return;
      case crypto_pay::InvoiceStage::Failed:
        cryptoStarting_ = false;
        OnCryptoStartFailed(invoice);
        return;
      case crypto_pay::InvoiceStage::None:
        cryptoStarting_ = false;
        SetState(State::Options);
        return;
    }
  }
  if (state_ != State::Invoice) return;
  switch (stage) {
    case crypto_pay::InvoiceStage::Paid:
      OnCryptoPaid();
      return;
    case crypto_pay::InvoiceStage::None:
      // dismissed (Pay another way) or signed out: back to the options, with Card
      SelectMethod(crypto_pay::kMethodCard);
      SetState(State::Options);
      return;
    case crypto_pay::InvoiceStage::Starting:
    case crypto_pay::InvoiceStage::Open:
    case crypto_pay::InvoiceStage::Failed:
      invoiceView_->Render(invoice);
      return;
  }
}

void UpgradeSheet::OnCryptoStartFailed(const crypto_pay::InvoiceView& invoice) {
  switch (crypto_pay::StartRefusalFor(invoice.errorCode, true)) {
    case crypto_pay::StartRefusal::GuestSignIn:
      // a legacy guest: the conversion first, like a refused card checkout
      RefuseForGuest();
      return;
    case crypto_pay::StartRefusal::Unavailable:
      if (!purchaseEmitted_) {
        EmitPurchase("failed", invoice.errorCode);
        purchaseEmitted_ = true;
      }
      // "Temporarily unavailable" under the row; the coin stays selected
      unavailableAsset_ = invoice.asset.empty() ? method_ : invoice.asset;
      SetState(State::Options);
      ApplyMethod();
      return;
    case crypto_pay::StartRefusal::Other:
    case crypto_pay::StartRefusal::None: {
      if (!purchaseEmitted_) {
        EmitPurchase("failed", invoice.errorCode.empty() ? std::string("transport")
                                                         : invoice.errorCode);
        purchaseEmitted_ = true;
      }
      SetState(State::Options);
      // rate limited, offer unavailable, start failed, or no answer: the
      // sheet's own refusal line, with the server's words under it when it
      // has no line of its own (ServerRefusalText.hpp)
      const ServerRefusalText refusal = ServerRefusalTextFor(
          invoice.errorCode, invoice.errorMessage, kUpgradeSheetRefusalLine);
      errorLabel_->set_text(
          ServerRefusalDisplay(T_(refusal.line.key, refusal.line.english), refusal));
      errorLabel_->set_visible(true);
      return;
    }
  }
}

void UpgradeSheet::OnCryptoPaid() {
  // What a confirmed card purchase does: the confirmation poll reads the
  // credited Pro (the window's Pro celebration follows the flip), and the
  // sheet shows its success, which reports the purchase completed once.
  balance_.StartConfirmationPolling();
  SetState(State::Success);
}

bool UpgradeSheet::Suspended() {
#if GTK_CHECK_VERSION(4, 12, 0)
  return gtk_window_is_suspended(gobj());
#else
  return false;
#endif
}

void UpgradeSheet::SetInvoiceViewer(bool visible) {
  if (visible == invoiceViewer_) return;
  invoiceViewer_ = visible;
  if (visible) {
    host_.CryptoInvoice().AddVisibleViewer();
  } else {
    host_.CryptoInvoice().RemoveVisibleViewer();
  }
}

}  // namespace urnw
