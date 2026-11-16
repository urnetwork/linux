// A refused card payment (CardPaymentReport.hpp): what the card payment
// report's answer means, where a refusal shows and what it ends, and how the
// upgrade sheet, the balance store and the payment web view carry it out.
// The sheet, the store and the web view need GTK, WebKit and the SDK, so
// their wiring is read as text.
//
// The server refuses a yearly trial whose card failed the CVC check (POST
// /subscription/verify-stripe-checkout answers status invalid with
// error.code incorrect_cvc): no Pro, the card detached. The sheet ends the
// confirmation and goes back to the options with the refusal's line and Try
// again, which starts a new payment sheet or checkout session; nothing falls
// through to another checkout path.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <filesystem>
#include <initializer_list>
#include <string>

#include "CardPaymentReport.hpp"
#include "ServerRefusalText.hpp"
#include "WiringSource.hpp"

using urnw::CardRefusalAction;
using urnw::CardRefusalStage;
using urnw::CardReportOutcome;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;

namespace {

// Each needle occurs in `text`, in this order.
bool InOrder(const std::string& text, std::initializer_list<const char*> needles) {
  size_t at = 0;
  for (const char* needle : needles) {
    const size_t found = text.find(needle, at);
    if (found == std::string::npos) {
      UR_FAIL(std::string("missing or out of order: ") + needle);
      return false;
    }
    at = found + 1;
  }
  return true;
}

bool Has(const std::string& text, const std::string& needle) {
  if (Contains(text, needle)) return true;
  UR_FAIL("missing: " + needle);
  return false;
}

// The sheet's source with its comments blanked, failing the test when it
// cannot be read.
std::string Sheet() {
  const std::string sheet = ReadCode("UpgradeSheet.cpp");
  if (sheet.empty()) UR_FAIL("could not read UpgradeSheet.cpp");
  return sheet;
}

constexpr CardRefusalStage kShownStages[] = {CardRefusalStage::Options, CardRefusalStage::Waiting,
                                             CardRefusalStage::TimedOut};
constexpr CardRefusalStage kMovedOnStages[] = {CardRefusalStage::Starting, CardRefusalStage::Paying,
                                               CardRefusalStage::Success};

const char* const kCvcEnglish =
    "Your card's security code (CVC) didn't match. Check the card details and try again.";

}  // namespace

// A credit wins; an answer that carries an error is a refusal whatever its
// status; anything else is left to the confirmation poll.
UR_TEST(CardPaymentReport_TheAnswersOutcome) {
  UR_EXPECT_TRUE(urnw::CardReportOutcomeFor(true, false) == CardReportOutcome::Credited);
  UR_EXPECT_TRUE(urnw::CardReportOutcomeFor(true, true) == CardReportOutcome::Credited);
  UR_EXPECT_TRUE(urnw::CardReportOutcomeFor(false, true) == CardReportOutcome::Refused);
  UR_EXPECT_TRUE(urnw::CardReportOutcomeFor(false, false) == CardReportOutcome::Confirming);
}

// Only the latest attempt's refusal, and only while the sheet is still on it:
// waiting, timed out (the hosted checkout's refusal can land long after the
// 2-minute confirmation gave up), or back on its options. On screen it shows
// now; a hidden sheet shows it at its next open.
UR_TEST(CardPaymentReport_WhereARefusalShows) {
  for (const CardRefusalStage stage : kShownStages) {
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(true, stage, true) == CardRefusalAction::ShowNow);
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(true, stage, false) == CardRefusalAction::ShowAtOpen);
  }
  // a new checkout or invoice starting, the payment form or an invoice on
  // screen, or Pro: the sheet went on to something else
  for (const CardRefusalStage stage : kMovedOnStages) {
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(true, stage, true) == CardRefusalAction::Ignore);
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(true, stage, false) == CardRefusalAction::Ignore);
  }
  // an older attempt's refusal is never the sheet's
  for (const CardRefusalStage stage : kShownStages) {
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(false, stage, true) == CardRefusalAction::Ignore);
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(false, stage, false) == CardRefusalAction::Ignore);
  }
  for (const CardRefusalStage stage : kMovedOnStages) {
    UR_EXPECT_TRUE(urnw::CardRefusalActionFor(false, stage, true) == CardRefusalAction::Ignore);
  }
}

// The refusal ends only the confirmation its attempt started: the wait, or a
// give-up still flagged. On the options a running poll is not the attempt's.
UR_TEST(CardPaymentReport_TheRefusalEndsOnlyItsConfirmation) {
  UR_EXPECT_TRUE(urnw::CardRefusalEndsConfirmation(CardRefusalStage::Waiting));
  UR_EXPECT_TRUE(urnw::CardRefusalEndsConfirmation(CardRefusalStage::TimedOut));
  UR_EXPECT_FALSE(urnw::CardRefusalEndsConfirmation(CardRefusalStage::Options));
  for (const CardRefusalStage stage : kMovedOnStages) {
    UR_EXPECT_FALSE(urnw::CardRefusalEndsConfirmation(stage));
  }
}

// incorrect_cvc reads as its own line alone, the store's English byte for
// byte; a refusal the app has no line for keeps the server's words under the
// sheet's own line.
UR_TEST(CardPaymentReport_TheCvcRefusalReadsItsOwnLine) {
  const urnw::ServerRefusalText cvc = urnw::ServerRefusalTextFor(
      "incorrect_cvc", "The server's English words.", urnw::kUpgradeSheetRefusalLine);
  UR_EXPECT_TRUE(std::string(cvc.line.key) == "checkout_error_incorrect_cvc");
  UR_EXPECT_TRUE(std::string(cvc.line.english) == kCvcEnglish);
  UR_EXPECT_TRUE(cvc.detail.empty());
  UR_EXPECT_TRUE(urnw::ServerRefusalDisplay("Translated.", cvc) == "Translated.");
  const urnw::ServerRefusalText other = urnw::ServerRefusalTextFor(
      "card_refused_some_new_way", "The server's English words.", urnw::kUpgradeSheetRefusalLine);
  UR_EXPECT_TRUE(std::string(other.line.key) == urnw::kUpgradeSheetRefusalLine.key);
  UR_EXPECT_TRUE(other.detail == "The server's English words.");
  // the catalog the app reads carries it with that English
  const std::string pot = urnw::testing::wiring::ReadRaw("../po/urnetwork.pot");
  Has(pot, std::string("msgctxt \"checkout_error_incorrect_cvc\"\nmsgid \"") + kCvcEnglish + "\"");
  Has(urnw::testing::wiring::ReadRaw("../po/linux-keys.txt"), "\ncheckout_error_incorrect_cvc\n");
}

// The report hands a refusal on with the attempt it was for, once the sheet is
// known to be alive; a credit still reads the balance.
UR_TEST(CardPaymentReport_TheReportHandsTheRefusalOn) {
  const std::string sheet = Sheet();
  const std::string body = FunctionBody(sheet, "void UpgradeSheet::ReportCardPayment(bool paySheet) {");
  InOrder(body, {"cardPaymentReported_ = true;", "const uint64_t attempt = attempt_;",
                 "std::weak_ptr<bool> alive = alive_;", "host_.api().reportStripePurchase(",
                 "PostToMain(", "if (!alive.lock()) return;",
                 "const bool refused = result->error.has_value();",
                 "result->error->code.value_or(std::string())",
                 "urnet::PurchaseReportStatusCredited",
                 "urnet::PurchaseReportStatusAlreadyCredited",
                 "switch (CardReportOutcomeFor(credited, refused)) {",
                 "case CardReportOutcome::Credited:", "balance_.FetchNow();",
                 "case CardReportOutcome::Refused:",
                 "OnCardPaymentRefused(attempt, code, result->error->message);",
                 "case CardReportOutcome::Confirming:"});
  // the server's English words are shown, never logged
  UR_EXPECT_FALSE(Contains(body, "message.c_str()"));
  UR_EXPECT_FALSE(Contains(body, "error->message.c_str()"));
}

// The refusal is final for its attempt: it ends the confirmation the attempt
// started, words the refusal and goes back to the options, now or at the next
// open. It never starts another checkout path or another confirmation.
UR_TEST(CardPaymentReport_TheRefusalIsFinalForItsAttempt) {
  const std::string sheet = Sheet();
  const std::string body = FunctionBody(sheet, "void UpgradeSheet::OnCardPaymentRefused(");
  InOrder(body, {"const CardRefusalStage stage = RefusalStage();",
                 "const bool latest = attempt == attempt_ && SignedInNetworkId(host_) == attemptNetwork_;",
                 "CardRefusalActionFor(latest, stage, get_visible())",
                 "if (action == CardRefusalAction::Ignore) return;", "++*epoch_;",
                 "if (CardRefusalEndsConfirmation(stage)) balance_.StopConfirmationPolling();",
                 "EmitPurchase(\"failed\",",
                 "ServerRefusalTextFor(code, message, kUpgradeSheetRefusalLine)",
                 "ServerRefusalDisplay(T_(refusal.line.key, refusal.line.english), refusal)",
                 "if (action == CardRefusalAction::ShowAtOpen) {", "refusalAtOpen_ = true;",
                 "SetState(State::Options);", "return;", "ShowCardRefusal();"});
  for (const char* never : {"RequestSession(", "RequestPaymentSheet(", "OpenPaySheet(",
                            "OpenEmbedded(", "StartCheckout(", "g_app_info_launch_default_for_uri(",
                            "StartConfirmationPolling(", "present("}) {
    UR_EXPECT_TRUE_MSG(never, !Contains(body, never));
  }
  InOrder(FunctionBody(sheet, "void UpgradeSheet::ShowCardRefusal() {"),
          {"SetState(State::Options);", "errorLabel_->set_text(refusal_);",
           "errorLabel_->set_visible(true);", "joinLabel_->set_text(JoinLabel());"});
  // the button reads Try again while the refusal shows, for the refused
  // attempt's plan with the card
  InOrder(FunctionBody(sheet, "Glib::ustring UpgradeSheet::JoinLabel() const {"),
          {"if (crypto_pay::IsCoin(method_)) {",
           "if (!refusal_.empty() && plans_->Yearly() == attemptYearly_) return T_(\"try_again\", "
           "\"Try again\");",
           "PlanPicker::CtaLabel(plans_->Yearly())"});
  // every state is placed
  const std::string stages = FunctionBody(sheet, "CardRefusalStage UpgradeSheet::RefusalStage() const {");
  InOrder(stages, {"case State::Options:", "return CardRefusalStage::Options;",
                   "case State::Launching:", "return CardRefusalStage::Starting;",
                   "case State::Checkout:", "case State::Invoice:", "return CardRefusalStage::Paying;",
                   "case State::Waiting:", "return CardRefusalStage::Waiting;",
                   "case State::TimedOut:", "return CardRefusalStage::TimedOut;",
                   "case State::Success:", "return CardRefusalStage::Success;"});
}

// Each checkout and invoice is a new attempt, which drops an older attempt's
// refusal; a refusal kept while the sheet was hidden shows at its next open,
// on the options only; picking a method or opening a coin's checkout or the
// invoice drops it with its line.
UR_TEST(CardPaymentReport_AttemptsAndTheNextOpen) {
  const std::string sheet = Sheet();
  InOrder(FunctionBody(sheet, "void UpgradeSheet::StartCheckout() {"),
          {"SetState(State::Launching);", "++attempt_;", "attemptYearly_ = plans_->Yearly();",
           "attemptNetwork_ = SignedInNetworkId(host_);", "refusal_.clear();",
           "refusalAtOpen_ = false;", "joinLabel_->set_text(JoinLabel());",
           "EmitPurchase(\"started\");"});
  InOrder(FunctionBody(sheet, "void UpgradeSheet::StartCrypto(const std::string& plan) {"),
          {"SetState(State::Launching);", "++attempt_;", "refusal_.clear();",
           "refusalAtOpen_ = false;", "EmitPurchase(\"started\");"});
  InOrder(FunctionBody(sheet, "void UpgradeSheet::Open(bool freeRefresh) {"),
          {"errorLabel_->set_visible(false);",
           "const bool showRefusal = refusalAtOpen_ && SignedInNetworkId(host_) == attemptNetwork_ &&",
           "!balance_.IsPolling() &&",
           "!crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State());",
           "refusalAtOpen_ = false;", "if (!showRefusal) refusal_.clear();",
           "plans_->Select(showRefusal ? attemptYearly_ : true);", "SetState(State::Waiting);",
           "ShowInvoice();", "SetState(State::Options);", "if (showRefusal) ShowCardRefusal();"});
  for (const char* signature :
       {"void UpgradeSheet::OpenCryptoCheckout(", "void UpgradeSheet::OpenInvoice() {"}) {
    InOrder(FunctionBody(sheet, signature),
            {"errorLabel_->set_visible(false);", "refusal_.clear();", "refusalAtOpen_ = false;"});
  }
  InOrder(FunctionBody(sheet, "void UpgradeSheet::BuildUi() {"),
          {"methodRow_->on_select = [this](const std::string& method) {",
           "errorLabel_->set_visible(false);", "refusal_.clear();", "ApplyMethod();"});
  // the events name the attempt's plan, which a refusal on reopened options
  // still names
  Has(FunctionBody(sheet, "void UpgradeSheet::EmitPurchase("),
      "const bool yearly = coin || attemptYearly_;");
}

// The store ends a refused confirmation without the give-up's timed-out flag
// and goes back to the background cadence; only the sheet's refusal ends one.
UR_TEST(CardPaymentReport_TheStoreEndsTheConfirmationWithoutAGiveUp) {
  const std::string store = ReadCode("SubscriptionBalance.cpp");
  const std::string body =
      FunctionBody(store, "void SubscriptionBalanceStore::StopConfirmationPolling() {");
  InOrder(body, {"const bool confirming = gate_.Confirming();",
                 "if (!confirming && !purchaseConfirmationTimedOut_) return;",
                 "purchaseConfirmationTimedOut_ = false;", "pollingTimer_.disconnect();",
                 "gate_.Stop();", "if (started_ && !isPro_) StartBackgroundPolling(", "false);",
                 "Emit();"});
  UR_EXPECT_FALSE(Contains(body, "purchaseConfirmationTimedOut_ = true"));
  UR_EXPECT_EQ(size_t{1}, CountOf(Sheet(), "StopConfirmationPolling("));
  for (const char* file : {"MainWindow.cpp", "Onboarding.cpp", "AccountPage.cpp"}) {
    UR_EXPECT_TRUE_MSG(file, !Contains(ReadCode(file), "StopConfirmationPolling("));
  }
}

// The payment web view (the pay sheet and the embedded checkout) gets an
// ephemeral network session of its own that accepts third-party cookies, for
// the banks' 3D Secure challenges in Stripe's frame. Nothing else in the app
// changes WebKit's cookie policy or uses the default session.
UR_TEST(CardPaymentReport_ThePaymentViewHasItsOwnCookieSession) {
  const std::string sheet = Sheet();
  const std::string body = FunctionBody(sheet, "bool UpgradeSheet::EnsureWebView() {");
  InOrder(body, {"if (webView_) return true;",
                 "WebKitNetworkSession* session = webkit_network_session_new_ephemeral();",
                 "webkit_cookie_manager_set_accept_policy(webkit_network_session_get_cookie_manager(session),",
                 "WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS",
                 "g_object_new(WEBKIT_TYPE_WEB_VIEW, \"network-session\", session, nullptr)",
                 "g_object_unref(session);", "if (!view) return false;", "webView_ = view;"});
  UR_EXPECT_FALSE(Contains(sheet, "webkit_web_view_new()"));
  size_t sources = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(UR_SRC_DIR)) {
    const std::string ext = entry.path().extension().string();
    if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".hpp")) continue;
    ++sources;
    const std::string relative = std::filesystem::relative(entry.path(), UR_SRC_DIR).string();
    const std::string code = ReadCode(relative);
    UR_EXPECT_TRUE_MSG(relative, !Contains(code, "webkit_network_session_get_default"));
    if (relative != "UpgradeSheet.cpp") {
      UR_EXPECT_TRUE_MSG(relative, !Contains(code, "webkit_cookie_manager_set_accept_policy"));
      UR_EXPECT_TRUE_MSG(relative, !Contains(code, "webkit_web_view_new"));
    }
  }
  UR_EXPECT_TRUE(sources > 100);
}
