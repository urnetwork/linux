// What the upgrade sheet and Manage subscription say when the server refuses
// to start a checkout, refuses a finished card payment, or refuses to open
// the billing portal (UpgradeSheet.cpp RequestSession and ReportCardPayment,
// AccountPage.cpp OpenCustomerPortal; ur.io src/lib/paymentFailure.js
// parity).
//
// The server refuses with a stable error.code beside its English message
// (server PurchaseErrorCode* for POST /stripe/create-checkout-session and the
// card payment report, POST /subscription/verify-stripe-checkout;
// SubscriptionErrorCode* for POST /stripe/customer-portal). The rule is
// ur.io's:
//   - a code with a line of its own reads as that line alone:
//     already_subscribed, plan_unavailable and checkout_unavailable from the
//     checkout session, incorrect_cvc from the card payment report (a yearly
//     trial whose card failed the CVC check: no Pro, the card detached),
//     no_customer and store_unavailable from the billing portal;
//   - invalid_request and start_failed read as the screen's own line alone (a
//     request this app got wrong, or a start that did not happen and can be
//     tried again);
//   - any other code, or none (a server from before the codes), reads as the
//     screen's own line with the server's words on the line under it.
// guest_sign_in_required never gets here: the upgrade sheet opens the add
// sign-in flow first (GuestConversion.hpp PurchaseRefusalFor), and the billing
// portal does not send it. Nor does a request that got no answer (the SDK's
// error string), which is not a refusal and reads as it always did. Codes
// compare exactly, as the server spells them.
//
// Dependency-free so tests/ServerRefusalTextTest.cpp runs it without GTK or
// the SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw {

// A line a screen shows: a store key and its English (the gettext msgid).
struct RefusalLine {
  const char* key;
  const char* english;
};

// The upgrade sheet's own line for a checkout that did not start.
inline constexpr RefusalLine kUpgradeSheetRefusalLine{
    "something_went_wrong_please_try_again_later",
    "Something went wrong. Please try again later."};

// Manage subscription's own line for a billing portal that did not open.
inline constexpr RefusalLine kManageSubscriptionRefusalLine{"something_went_wrong",
                                                            "Something went wrong."};

// The words for a refusal: the line, and the server's words to show under it
// ("" when the line stands alone).
struct ServerRefusalText {
  RefusalLine line;
  std::string detail;
};

// `screenLine` is the refusing screen's own line, for the codes without a line
// of their own.
inline ServerRefusalText ServerRefusalTextFor(std::string_view code,
                                              const std::string& serverMessage,
                                              const RefusalLine& screenLine) {
  if (code == "already_subscribed") {
    return {{"site_payment_error_already_subscribed",
             "You already have Pro, so nothing was charged. Manage your subscription from your "
             "account."},
            std::string()};
  }
  if (code == "plan_unavailable") {
    return {{"site_payment_error_plan_unavailable",
             "This plan is not available right now. Try again later."},
            std::string()};
  }
  if (code == "checkout_unavailable") {
    return {{"checkout_error_unavailable",
             "Checkout isn't available right now. Please try again later."},
            std::string()};
  }
  if (code == "incorrect_cvc") {
    return {{"checkout_error_incorrect_cvc",
             "Your card's security code (CVC) didn't match. Check the card details and try "
             "again."},
            std::string()};
  }
  if (code == "no_customer") {
    return {{"site_subscription_error_no_customer",
             "This network has no Stripe billing details."},
            std::string()};
  }
  if (code == "store_unavailable") {
    return {{"site_subscription_error_store_unavailable",
             "Stripe could not be reached just now. Try again in a moment."},
            std::string()};
  }
  if (code == "invalid_request" || code == "start_failed") return {screenLine, std::string()};
  return {screenLine, serverMessage};
}

// What the screen shows: `translatedLine` (gettext's answer for text.line)
// with the server's words, when there are any, on the line under it.
inline std::string ServerRefusalDisplay(const std::string& translatedLine,
                                        const ServerRefusalText& text) {
  if (text.detail.empty()) return translatedLine;
  return translatedLine + "\n" + text.detail;
}

}  // namespace urnw
