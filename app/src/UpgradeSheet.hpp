// Upgrade sheet (port of the apple UpgradeSubscriptionSheet + ProductOption
// Card + PurchaseSuccessView, over Stripe checkout instead of StoreKit):
// pick monthly or yearly UR Pro -> Api::createStripeCheckoutSession{item_id,
// ui_mode} -> pay -> the balance store's 5s confirmation poll flips Pro when
// the Stripe webhook lands. The sheet then tracks the store: waiting ->
// premium success, or the poll's 2-minute deadline -> a timed-out state (the
// purchase still lands later; the background poll and the next launch pick it
// up).
//
// Three checkout paths, tried in order (mmm/onboarding/PLAN.md: inline
// Stripe, never an external browser while a web view exists):
//
//   * payment sheet (preferred): Api::stripePaymentSheet{plan} -> the ur.io
//     embedded pay page https://ur.io/app/pay-sheet?cs=&pk=&plan=&return=
//     in the sheet's web view. The page mounts Stripe's Payment Element,
//     confirms the SetupIntent (yearly: the trial, the welcome offer's coupon
//     applied server-side) or the PaymentIntent (monthly), and hands control
//     back by navigating to the return url urnetwork://pay/done or posting
//     {type: "ur-pay", status}. A pay-sheet failure before anything rendered
//     falls through to the next path.
//   * embedded (fallback): ui_mode "embedded" -> the sheet content swaps to a
//     WebKitGTK webview loading https://ur.io/checkout, which mounts Stripe's
//     Embedded Checkout and hands control back over the urnetwork:// scheme
//     (intercepted in "decide-policy"; the navigation never leaves the
//     webview). Only compiled when meson found webkitgtk-6.0 (UR_HAVE_WEBKIT).
//   * hosted (fallback): ui_mode "hosted" -> open checkout_url in the system
//     browser. Used when the app was built without webkit, when the webview
//     cannot be created at runtime, when the embedded session request fails,
//     or when the checkout page itself fails to load — a payment path must
//     never hard-fail.
//
// Crypto (mmm/ZCASH-CONTRACT.md §6): the method row (PaymentMethodRow) sits
// directly above the buy button, after the plan cards, and lists Card and the
// coins the server enabled. A coin greys monthly out, the button reads "Pay
// with {method}" over the coin's fine print, and the button starts the
// signed-in network's invoice (CryptoInvoiceHost) instead of a checkout; the
// sheet then shows the native invoice as its Invoice state
// (CryptoInvoiceView). The invoice polls only while it is on screen. Paid
// does what a confirmed card purchase does: the confirmation poll flips Pro
// and the sheet shows its success. A payment already going on is what the
// sheet opens onto (Open, OpenInvoice), never replaced by a second invoice.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <gtkmm.h>

#include "CryptoPay.hpp"
#include "DataInfoSheet.hpp"
#include "OfferCard.hpp"
#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"

namespace urnw {

class CryptoInvoiceView;
class PaymentMethodRow;
class PlanPicker;

class UpgradeSheet : public Gtk::Window {
 public:
  UpgradeSheet(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance);
  ~UpgradeSheet() override;

  // `freeRefresh`: a start connect refused for the balance opened the sheet
  // (data_info::UpgradeShowsFreeRefresh), so it says when the free data
  // refreshes and offers Wait for refresh.
  void Open(bool freeRefresh = false);
  // selects the plan and goes straight to the Stripe checkout (the onboarding's Start free trial)
  void OpenCheckout(bool yearly);
  // The onboarding's coin: selects it and the yearly plan and starts the
  // invoice for `plan` (crypto_pay::CoinPlan), or shows the payment already
  // going on.
  void OpenCryptoCheckout(const std::string& asset, const std::string& plan);
  // Account's "Payment in progress": the saved invoice, or the options when
  // there is none any more.
  void OpenInvoice();
  // The balance store's tier/offer changed: reprint the plan cards and the
  // read-only offer line (shown while the offer state is active).
  void ApplyPrices();
  // Balance store change feed (marshalled onto the GTK loop by the owner):
  // flips waiting -> success / timed-out.
  void OnBalanceChanged();

  // The server refused the checkout because the network is a legacy guest
  // (guest_sign_in_required): the sheet has hidden itself, and the owner opens
  // the conversion (MainWindow::OpenUpgrade; onboarding: its owner).
  std::function<void()> on_guest_sign_in_required;

 private:
  enum class State { Options, Launching, Checkout, Waiting, Success, TimedOut, Invoice };

  void BuildUi();
  void StartCheckout();
  // ---- crypto ----
  // The row's segments from the balance; true when they changed.
  bool RefreshMethods();
  // Selects `method` (Card when the row does not list it) and follows it.
  void SelectMethod(const std::string& method);
  // What the selected method changes: monthly, the trial line, the button,
  // the fine print, the offer's card terms, "Temporarily unavailable".
  void ApplyMethod();
  Glib::ustring JoinLabel() const;
  // POST /pay/crypto/intent through the invoice controller.
  void StartCrypto(const std::string& plan);
  void ShowInvoice();
  // The invoice controller's change feed (CryptoInvoiceHost::Subscribe).
  void OnInvoiceChanged();
  void OnCryptoStartFailed(const crypto_pay::InvoiceView& invoice);
  void OnCryptoPaid();
  // The invoice is on screen (its view mapped, the sheet not minimized): the
  // controller runs.
  void SetInvoiceViewer(bool visible);
  // The sheet is minimized (GTK 4.12+ reports it; false before): the app is
  // in the background, and the invoice stops polling until it is back.
  bool Suspended();
  // The pay-sheet path: one stripePaymentSheet round trip, then the ur.io pay
  // page in the web view. Any failure before the page rendered continues
  // with the embedded checkout session.
  void RequestPaymentSheet();
  void EmitPurchase(const char* outcome, const std::string& errorClass = "");
  // One createStripeCheckoutSession round trip. An embedded failure retries
  // once as hosted; sessions are only ever created in sequence, never both.
  void RequestSession(bool embedded);
  void SetState(State state);
  // The refusal for a guest network (PurchaseRefusalFor): no fallback, no
  // error line; the sheet hides and on_guest_sign_in_required runs.
  void RefuseForGuest();

#ifdef UR_HAVE_WEBKIT
  // Creates the webview on first use; false means webkit is unusable at
  // runtime and checkout must go hosted.
  bool EnsureWebView();
  void OpenEmbedded(const std::string& clientSecret);
  void OpenPaySheet(const std::string& clientSecret, const std::string& publishableKey);
  // urnetwork://checkout?... and urnetwork://pay/... interception from the
  // webview's "decide-policy", and the pay page's posted {type:"ur-pay"} message.
  void HandleCheckoutCallback(const std::string& uri);
  void HandlePayMessage(const std::string& json);
  // Checkout page failed before it ever rendered: retry the purchase hosted.
  void OnCheckoutLoadFailed();
  // The web process died (RouteCheckoutCrash): after the page loaded, confirm
  // the payment (waiting + confirmation poll); before, the load-failure rescue.
  void OnWebProcessTerminated();
  void TeardownWebView();
#endif

  SdkHost& host_;
  SubscriptionBalanceStore& balance_;
  State state_ = State::Options;
  // invalidates the in-flight checkout-session callback after a reset
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Box* optionsBox_ = nullptr;
  Gtk::Box* waitingBox_ = nullptr;
  Gtk::Box* successBox_ = nullptr;
  // the account the plan is on, under the thanks; read when the success shows
  Gtk::Label* successAccount_ = nullptr;
  Gtk::Box* timedOutBox_ = nullptr;
  PlanPicker* plans_ = nullptr;  // the onboarding plan picker, shared
  Gtk::Button* joinBtn_ = nullptr;
  Gtk::Label* joinLabel_ = nullptr;  // follows the selection: trial vs subscribe
  Gtk::Spinner* joinSpinner_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
  OfferCard* offerLine_ = nullptr;  // the active welcome offer, read-only
  bool purchaseEmitted_ = false;    // purchase.completed once per checkout
  bool paySheetActive_ = false;     // the web view shows the pay page (not Checkout)
  Gtk::Box* freeRefreshBox_ = nullptr;  // shown when a blocked connect opened the sheet
  Gtk::Label* freeRefreshLabel_ = nullptr;
  Gtk::Label* freeRefreshKindLabel_ = nullptr;  // reserved or used up, under it
  FreeRefreshTicker freeRefreshTicker_;
  // waiting-state headline: "complete in the browser" (hosted) vs
  // "processing payment" (embedded, already paid in the webview)
  Gtk::Label* waitingLabel_ = nullptr;

  // ---- crypto ----
  PaymentMethodRow* methodRow_ = nullptr;
  Gtk::Label* finePrint_ = nullptr;  // a coin's fine print under the button
  Gtk::Box* invoiceBox_ = nullptr;   // the Invoice state
  CryptoInvoiceView* invoiceView_ = nullptr;
  std::string method_ = crypto_pay::kMethodCard;  // what the button follows
  bool methodPicked_ = false;  // the user picked a method in this open
  // the purchase the events describe: its method and, for a coin, its plan
  std::string purchaseMethod_ = crypto_pay::kMethodCard;
  std::string purchasePlan_;
  bool cryptoStarting_ = false;    // this sheet's StartInvoice waits for its quote
  std::string unavailableAsset_;   // the coin whose start was asset_unavailable
  int invoiceSubscription_ = 0;    // CryptoInvoiceHost::Subscribe
  bool invoiceViewer_ = false;     // counted as a visible viewer of the invoice
  sigc::connection invoiceMapped_;
  sigc::connection invoiceUnmapped_;
  gulong suspendedHandler_ = 0;  // notify::suspended on this window

#ifdef UR_HAVE_WEBKIT
  Gtk::Box* checkoutBox_ = nullptr;   // brand header + webview slot
  Gtk::Box* webViewSlot_ = nullptr;   // owns the webview while checkout shows
  GtkWidget* webView_ = nullptr;      // WebKitWebView, created per checkout
  bool pageLoaded_ = false;           // first WEBKIT_LOAD_FINISHED arrived
  bool webFallbackTried_ = false;     // load-failed -> hosted, at most once
#endif
};

}  // namespace urnw
