// Upgrade sheet (port of the apple UpgradeSubscriptionSheet + ProductOption
// Card + PurchaseSuccessView, over Stripe checkout instead of StoreKit):
// pick monthly or yearly UR Pro -> Api::createStripeCheckoutSession{item_id,
// ui_mode} -> pay -> the balance store's 5s confirmation poll flips Pro when
// the Stripe webhook lands. The sheet then tracks the store: waiting ->
// premium success, or the poll's 2-minute deadline -> a timed-out state (the
// purchase still lands later; the background poll and the next launch pick it
// up).
//
// Two checkout paths (PARITY.md decision #2):
//
//   * embedded (preferred): ui_mode "embedded" -> the sheet content swaps to a
//     WebKitGTK webview loading https://ur.io/checkout, which mounts Stripe's
//     Embedded Checkout and hands control back over the urnetwork:// scheme
//     (intercepted in "decide-policy"; the navigation never leaves the
//     webview). Only compiled when meson found webkitgtk-6.0 (UR_HAVE_WEBKIT).
//   * hosted (fallback): ui_mode "hosted" -> open checkout_url in the system
//     browser. Used when the app was built without webkit, when the webview
//     cannot be created at runtime, when the embedded session request fails,
//     or when the checkout page itself fails to load — a payment path must
//     never hard-fail.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include <gtkmm.h>

#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"

namespace urnw {

class UpgradeSheet : public Gtk::Window {
 public:
  UpgradeSheet(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance);

  void Open();
  // Balance store change feed (marshalled onto the GTK loop by the owner):
  // flips waiting -> success / timed-out.
  void OnBalanceChanged();

 private:
  enum class State { Options, Launching, Checkout, Waiting, Success, TimedOut };

  void BuildUi();
  Gtk::ToggleButton* MakeOptionCard(const std::string& title, const std::string& subtitle);
  void StartCheckout();
  // One createStripeCheckoutSession round trip. An embedded failure retries
  // once as hosted; sessions are only ever created in sequence, never both.
  void RequestSession(bool embedded);
  void SetState(State state);

#ifdef UR_HAVE_WEBKIT
  // Creates the webview on first use; false means webkit is unusable at
  // runtime and checkout must go hosted.
  bool EnsureWebView();
  void OpenEmbedded(const std::string& clientSecret);
  // urnetwork://checkout?... interception from the webview's "decide-policy".
  void HandleCheckoutCallback(const std::string& uri);
  // Checkout page failed before it ever rendered: retry the purchase hosted.
  void OnCheckoutLoadFailed();
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
  Gtk::Box* timedOutBox_ = nullptr;
  Gtk::ToggleButton* yearlyCard_ = nullptr;
  Gtk::ToggleButton* monthlyCard_ = nullptr;
  Gtk::Button* joinBtn_ = nullptr;
  Gtk::Spinner* joinSpinner_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
  // waiting-state headline: "complete in the browser" (hosted) vs
  // "processing payment" (embedded, already paid in the webview)
  Gtk::Label* waitingLabel_ = nullptr;

#ifdef UR_HAVE_WEBKIT
  Gtk::Box* checkoutBox_ = nullptr;   // brand header + webview slot
  Gtk::Box* webViewSlot_ = nullptr;   // owns the webview while checkout shows
  GtkWidget* webView_ = nullptr;      // WebKitWebView, created per checkout
  bool pageLoaded_ = false;           // first WEBKIT_LOAD_FINISHED arrived
  bool webFallbackTried_ = false;     // load-failed -> hosted, at most once
#endif
};

}  // namespace urnw
