// The payment-method toggle and the crypto invoice, wired (crypto-pay
// mmm/ZCASH-CONTRACT.md §6). The rules are CryptoPayTest.cpp's; the screens that apply
// them need GTK and the SDK, so this reads their sources: where the method row
// sits and what a coin changes, that the invoice polls only while it is on
// screen, that a URI is drawn and opened only after the SDK's check, what paid
// and a refused start lead to, the controller's sign-in and sign-out, Account's
// rows, and the build lists.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <initializer_list>
#include <string>

#include "WiringSource.hpp"

using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;
using urnw::testing::wiring::ReadRaw;

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

}  // namespace

// The row sits directly above the buy button, after the plan cards and the
// offer line, and the button follows the method.
UR_TEST(CryptoPayWiring_TheSheetsRowSitsAboveTheButton) {
  const std::string sheet = ReadCode("UpgradeSheet.cpp");
  const std::string build = FunctionBody(sheet, "void UpgradeSheet::BuildUi() {");
  UR_EXPECT_TRUE(!build.empty());
  InOrder(build, {"plans_ = Gtk::make_managed<PlanPicker>();",
                  "offerLine_ = Gtk::make_managed<OfferCard>(",
                  "methodRow_ = Gtk::make_managed<PaymentMethodRow>();",
                  "optionsBox_->append(*methodRow_);", "joinBtn_ = Gtk::make_managed<Gtk::Button>();",
                  "optionsBox_->append(*joinBtn_);", "T_(\"crypto_fine_print_yearly\"",
                  "optionsBox_->append(*finePrint_);"});
  // a coin starts its invoice for the yearly plan or the offer; the card checks out
  InOrder(build, {"if (crypto_pay::IsCoin(method_)) {",
                  "StartCrypto(crypto_pay::CoinPlan(balance_.Offer().active));", "StartCheckout();"});
  // the pick is remembered on this device
  Has(build, "host_.CryptoInvoice().SetPaymentMethod(method);");
  const std::string apply = FunctionBody(sheet, "void UpgradeSheet::ApplyMethod() {");
  InOrder(apply, {"plans_->SetMonthlyAllowed(!coin);", "plans_->SetCardTermsShown(!coin);",
                  "if (coin && !plans_->Yearly()) plans_->Select(true);",
                  "joinLabel_->set_text(JoinLabel());", "finePrint_->set_visible(coin);",
                  "offerLine_->SetCardTermsShown(!coin);",
                  "methodRow_->SetUnavailable(coin && method_ == unavailableAsset_);"});
  InOrder(FunctionBody(sheet, "Glib::ustring UpgradeSheet::JoinLabel() const {"),
          {"T_(\"crypto_pay_button\", \"Pay with {}\")", "PaymentMethodName(method_)",
           "PlanPicker::CtaLabel(plans_->Yearly())"});
  // the tabs come from the SDK's answer over the balance, and the opening
  // selection is the device's last method
  Has(FunctionBody(sheet, "bool UpgradeSheet::RefreshMethods() {"),
      "methodRow_->SetTabs(balance_.PaymentTabs(crypto_pay::CoinPlan(balance_.Offer().active)));");
  // a start in flight holds the row's picks without making it insensitive,
  // whose libadwaita filter would dim the coins' marks
  const std::string setState = FunctionBody(sheet, "void UpgradeSheet::SetState(");
  Has(setState, "methodRow_->SetEnabled(!launching);");
  UR_EXPECT_FALSE(Contains(sheet, "methodRow_->set_sensitive("));
  InOrder(FunctionBody(sheet, "void UpgradeSheet::Open(bool freeRefresh) {"),
          {"RefreshMethods();", "SelectMethod(host_.CryptoInvoice().PaymentMethod());",
           "crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())", "ShowInvoice();"});
}

// The invoice polls only while it is on screen: the view's map starts the
// controller, its unmap stops it, and nothing else does.
UR_TEST(CryptoPayWiring_TheInvoicePollsOnlyWhileShown) {
  const std::string sheet = ReadCode("UpgradeSheet.cpp");
  const std::string build = FunctionBody(sheet, "void UpgradeSheet::BuildUi() {");
  Has(build, "invoiceView_->signal_map().connect([this] { SetInvoiceViewer(!Suspended()); });");
  Has(build, "invoiceView_->signal_unmap().connect([this] { SetInvoiceViewer(false); });");
  // minimized is backgrounded: notify::suspended stops it too (GTK 4.12+)
  InOrder(build, {"#if GTK_CHECK_VERSION(4, 12, 0)", "\"notify::suspended\"",
                  "self->SetInvoiceViewer(self->invoiceView_->get_mapped() && !self->Suspended());"});
  Has(FunctionBody(sheet, "bool UpgradeSheet::Suspended() {"), "gtk_window_is_suspended(gobj())");
  const std::string viewer = FunctionBody(sheet, "void UpgradeSheet::SetInvoiceViewer(bool visible) {");
  InOrder(viewer, {"if (visible == invoiceViewer_) return;", "AddVisibleViewer();",
                   "RemoveVisibleViewer();"});
  // the destructor lets go before any member does
  InOrder(FunctionBody(sheet, "UpgradeSheet::~UpgradeSheet() {"),
          {"Unsubscribe(invoiceSubscription_);", "invoiceMapped_.disconnect();",
           "invoiceUnmapped_.disconnect();", "g_signal_handler_disconnect(gobj(), suspendedHandler_);",
           "if (invoiceViewer_)"});
  const std::string host = ReadCode("CryptoInvoiceHost.cpp");
  // start and stop: the first viewer and the last, and a controller opened
  // while one is shown
  UR_EXPECT_EQ(size_t{2}, CountOf(host, "vc_->start();"));
  UR_EXPECT_EQ(size_t{1}, CountOf(host, "vc_->stop();"));
  InOrder(FunctionBody(host, "void CryptoInvoiceHost::AddVisibleViewer() {"),
          {"if (visibleViewers_++ != 0 || !vc_) return;", "vc_->start();"});
  InOrder(FunctionBody(host, "void CryptoInvoiceHost::RemoveVisibleViewer() {"),
          {"if (--visibleViewers_ != 0 || !vc_) return;", "vc_->stop();"});
  Has(FunctionBody(host, "void CryptoInvoiceHost::SignIn() {"), "if (0 < visibleViewers_) vc_->start();");
  // no window or page starts the controller behind the sheet's back
  for (const char* file : {"MainWindow.cpp", "AccountPage.cpp", "Onboarding.cpp", "UpgradeSheet.cpp",
                           "CryptoInvoiceView.cpp"}) {
    const std::string code = ReadCode(file);
    UR_EXPECT_TRUE_MSG(file, !Contains(code, "AddVisibleViewer") ||
                                 std::string(file) == "UpgradeSheet.cpp");
  }
}

// The SDK's listener fires on its own thread: it only marshals, and a change
// posted before a close or the host's end is dropped.
UR_TEST(CryptoPayWiring_TheControllerFeedIsMarshalled) {
  const std::string host = ReadCode("CryptoInvoiceHost.cpp");
  const std::string signIn = FunctionBody(host, "void CryptoInvoiceHost::SignIn() {");
  InOrder(signIn, {"urnet::newCryptoInvoiceViewController(host_.api(), dir)",
                   "vc_->addCryptoInvoiceListener([this, epoch, issued] {",
                   "PostToMain([this, epoch, issued] {", "if (*epoch != issued) return;",
                   "Refresh();", "vc_->resume();"});
  InOrder(FunctionBody(host, "void CryptoInvoiceHost::CloseController() {"),
          {"++*epoch_;", "sub_.reset();", "vc_->close();", "vc_.reset();"});
  Has(FunctionBody(host, "CryptoInvoiceHost::~CryptoInvoiceHost() {"), "CloseController();");
  // the state is read on the GTK loop, with the SDK's URI check beside it:
  // the whole gate, with no pre-check of this app's (an empty URI fails it)
  Has(host, "view.uriChecked = urnet::checkCryptoPaymentUri(state.asset, state.uri, state.address);");
  UR_EXPECT_FALSE(Contains(host, "!state.uri.empty() &&"));
  Has(host, "urnet::cryptoWalletSuggestions(state.asset)");
  // the QR draws the SDK's qr text, and a coin with no payment URI scheme
  // (TAO) is paid by hand
  Has(host, "view.qrText = state.qr_text;");
  Has(host, "crypto_pay::IsCoin(state.asset) && urnet::cryptoPaymentUriScheme(state.asset).empty();");
  // the coin's lines, whether its memo shows and the status steps are the
  // SDK's, carried as it hands them out
  const std::string viewOf =
      FunctionBody(host, "crypto_pay::InvoiceView ViewOf(const urnet::CryptoInvoiceState& state) {");
  InOrder(viewOf, {"for (const urnet::CryptoPaymentStep& sdkStep : *state.steps) {",
                   "step.progress = sdkStep.progress;",
                   "step.confirmationsRequired = sdkStep.confirmations_required;",
                   "if (const auto lines = urnet::cryptoAssetLinesFor(state.asset)) {",
                   "view.lines.networkLine = {lines->network_line_key, lines->network_line};",
                   "view.lines.networkWarning = {lines->network_warning_key, lines->network_warning};",
                   "view.lines.noPaymentLinkNote = {lines->no_payment_link_note_key, lines->no_payment_link_note};",
                   "view.showsMemo = urnet::cryptoPaymentShowsMemo(state.asset, state.memo);"});
  // a plan, never an item, and no storefront on the desktop
  Has(FunctionBody(host, "bool CryptoInvoiceHost::StartInvoice("), "vc_->startInvoice(asset, plan, \"\", \"\");");
}

// A sign-out ends the controller and clears what it saved (the invoice with
// its status token, the method), as the SDK's sign-out would clear its
// local-state directory. The window's signed-out state only ends it: a switch
// to another network server keeps this one's saved invoice.
UR_TEST(CryptoPayWiring_SignInAndSignOut) {
  const std::string host = ReadCode("CryptoInvoiceHost.cpp");
  InOrder(FunctionBody(host, "void CryptoInvoiceHost::SignOut() {"),
          {"CloseController();", "crypto_pay::WipeStorage(dir)", "Refresh();"});
  const std::string close = FunctionBody(host, "void CryptoInvoiceHost::Close() {");
  InOrder(close, {"CloseController();", "Refresh();"});
  UR_EXPECT_FALSE(Contains(close, "WipeStorage"));
  // the directory is the GUI's own, per network server, owner-only
  Has(FunctionBody(host, "void CryptoInvoiceHost::SignIn() {"),
      "crypto_pay::EnsureStorageDir(storageDir_)");
  const std::string sdk = ReadCode("SdkHost.cpp");
  Has(FunctionBody(sdk, "std::string SdkHost::CryptoInvoiceStorageDir() const {"),
      "crypto_pay::StorageDir(storageRoot_, activeSpaceHost_)");
  Has(FunctionBody(sdk, "bool SdkHost::Initialize("), "storageRoot_ = storageDir;");
  Has(FunctionBody(sdk, "bool SdkHost::ApplyNetworkServer("), "activeSpaceHost_ = hostName;");
  // the controller closes before the Api it was opened on goes
  const std::string header = ReadCode("SdkHost.hpp");
  UR_EXPECT_TRUE(Before(header, "std::optional<urnet::Api> api_;",
                        "std::unique_ptr<CryptoInvoiceHost> cryptoInvoice_;"));
  const std::string window = ReadCode("MainWindow.cpp");
  const std::string auth = FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {");
  InOrder(auth, {"balance_.Start();", "host_.CryptoInvoice().SignIn();", "} else {",
                 "balance_.Stop();", "host_.CryptoInvoice().Close();"});
  UR_EXPECT_FALSE(Contains(auth, "CryptoInvoice().SignOut()"));
  // the real sign-out (the user's, the server's, a deletion's) clears it, on
  // the main loop, before the tail that ends the session
  InOrder(FunctionBody(sdk, "void SdkHost::Logout() {"),
          {"CancelRowConnect(\"sign-out\");", "cryptoInvoice_->SignOut();", "RunCommand("});
}

// Paid does what a confirmed card purchase does; a refused start keeps a coin
// that cannot be quoted selectable, converts a guest, and words the rest like
// a refused checkout.
UR_TEST(CryptoPayWiring_PaidAndRefusals) {
  const std::string sheet = ReadCode("UpgradeSheet.cpp");
  InOrder(FunctionBody(sheet, "void UpgradeSheet::OnCryptoPaid() {"),
          {"balance_.StartConfirmationPolling();", "SetState(State::Success);"});
  const std::string changed = FunctionBody(sheet, "void UpgradeSheet::OnInvoiceChanged() {");
  // only the sheet on screen acts, so a payment completes once
  InOrder(changed, {"if (!get_visible()) return;", "case crypto_pay::InvoiceStage::Open:",
                    "ShowInvoice();", "case crypto_pay::InvoiceStage::Failed:",
                    "OnCryptoStartFailed(invoice);", "if (state_ != State::Invoice) return;",
                    "case crypto_pay::InvoiceStage::Paid:", "OnCryptoPaid();"});
  const std::string failed = FunctionBody(sheet, "void UpgradeSheet::OnCryptoStartFailed(");
  InOrder(failed, {"crypto_pay::StartRefusal::GuestSignIn:", "RefuseForGuest();",
                   "crypto_pay::StartRefusal::Unavailable:", "unavailableAsset_ =",
                   "SetState(State::Options);", "ApplyMethod();", "crypto_pay::StartRefusal::Other:",
                   "ServerRefusalTextFor(", "kUpgradeSheetRefusalLine", "errorLabel_->set_visible(true);"});
  // Pay another way gives the invoice up and goes back to Card
  const std::string build = FunctionBody(sheet, "void UpgradeSheet::BuildUi() {");
  InOrder(build, {"invoiceView_->on_pay_another_way = [this] {", "host_.CryptoInvoice().Dismiss();",
                  "SelectMethod(crypto_pay::kMethodCard);", "SetState(State::Options);"});
  Has(build, "invoiceView_->on_refresh = [this] { host_.CryptoInvoice().Requote(); };");
  // the events name the coin's market and the coin's product (the SDK's:
  // pro_yearly_<ticker>, so USDC and SOL, which share the solana store, stay
  // apart), and no trial; Card's product is its plan's
  const std::string emit = FunctionBody(sheet, "void UpgradeSheet::EmitPurchase(");
  InOrder(emit, {"PaymentMethodEventStore(purchaseMethod_)",
                 "const std::string coinProduct = coin ? PaymentMethodEventProduct(purchaseMethod_) : std::string();",
                 "const std::string product = coinProduct.empty() ? std::string(PlanProduct(yearly)) : coinProduct;",
                 "const bool trial = yearly && !coin;", "events.PurchaseStarted(store, product,",
                 "events.PurchaseCompleted(store, product,", "events.PurchaseCancelled(store, product,",
                 "events.PurchaseFailed(store, product,"});
  UR_EXPECT_FALSE(Contains(emit, "(store, PlanProduct("));
  const std::string row = ReadCode("PaymentMethodRow.cpp");
  InOrder(FunctionBody(row, "std::string PaymentMethodEventStore(const std::string& method) {"),
          {"if (!crypto_pay::IsCoin(method)) return urnet::EventStoreStripe;",
           "return urnet::cryptoAssetMarket(method);"});
  InOrder(FunctionBody(row, "std::string PaymentMethodEventProduct(const std::string& method) {"),
          {"if (!crypto_pay::IsCoin(method)) return std::string();",
           "return urnet::cryptoPaymentEventProduct(method);"});
  // the onboarding's coin never replaces a payment going on
  InOrder(FunctionBody(sheet, "void UpgradeSheet::OpenCryptoCheckout("),
          {"crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())", "ShowInvoice();",
           "return;", "StartCrypto(plan);"});
}

// Only the SDK's qr text is drawn (a URI that passed the SDK's check, or TAO's
// address alone), and only a checked URI is opened; a failed open says nothing
// alarming and never logs the URI.
UR_TEST(CryptoPayWiring_TheUriIsCheckedBeforeItIsShownOrOpened) {
  const std::string view = ReadCode("CryptoInvoiceView.cpp");
  const std::string render = FunctionBody(view, "void CryptoInvoiceView::Render(");
  InOrder(render, {"network_->set_text(LabelText(network.key, network.english));",
                   "const std::string qrText = p.qrText;", "crypto_pay::EncodePaymentQr(qrText)",
                   "noLinkNote_->set_text(LabelText(note.key, note.english));",
                   "noLinkNote_->set_visible(qrDrawn && !note.key.empty());",
                   "openUri_ = p.showOpenWallet ? invoice.uri : std::string();",
                   "openWallet_->set_visible(p.showOpenWallet);",
                   "networkWarning_->set_text(LabelText(warning.key, warning.english));"});
  // the coin's network line is the one the SDK carried for it
  Has(render, "const crypto_pay::CoinLine& network = invoice.lines.networkLine;");
  // the steps are the SDK's, row by row, each drawn by its progress and the
  // confirming one with its own counts
  const std::string steps = FunctionBody(view, "void CryptoInvoiceView::RenderSteps(");
  InOrder(steps, {"for (size_t index = 0; index < p.steps.size() && index < stepRows_.size(); ++index) {",
                  "const crypto_pay::Step& step = p.steps[index];",
                  "if (step.status == crypto_pay::kStatusPending) {",
                  "} else if (step.status == crypto_pay::kStatusConfirming) {",
                  "step.confirmationsRequired);",
                  "const bool done = step.progress == crypto_pay::kStepDone;",
                  "const bool current = step.progress == crypto_pay::kStepCurrent;"});
  UR_EXPECT_FALSE(Contains(steps, "StepKind") || Contains(steps, "p.confirmations"));
  // Open in wallet never hands over the qr text (TAO's QR is a bare address)
  UR_EXPECT_FALSE(Contains(render, "openUri_ = qrText"));
  UR_EXPECT_FALSE(Contains(render, "openWallet_->set_visible(p.showQr);"));
  const std::string open = FunctionBody(view, "void CryptoInvoiceView::OpenInWallet() {");
  InOrder(open, {"if (openUri_.empty()) return;",
                 "g_app_info_launch_default_for_uri(openUri_.c_str(), nullptr, &error)", "g_debug("});
  UR_EXPECT_FALSE(Contains(open, "set_text"));
  UR_EXPECT_FALSE(Contains(open, "set_visible"));
  // the URI and the address never reach a log line
  for (const char* log : {"g_message(", "g_warning(", "g_debug(", "fprintf("}) {
    for (size_t at = view.find(log); at != std::string::npos; at = view.find(log, at + 1)) {
      const std::string call = view.substr(at, view.find(';', at) - at);
      UR_EXPECT_TRUE_MSG(call, !Contains(call, "uri") || Contains(call, "error->message"));
      UR_EXPECT_TRUE_MSG(call, !Contains(call, "address"));
    }
  }
  // Copy puts the value itself on the display's clipboard
  Has(FunctionBody(view, "void CryptoInvoiceView::Copy(CopyRow& row) {"),
      "row.copy->get_clipboard()->set_text(row.text);");
  // the amount is copied without its ticker
  Has(render, "SetCopyText(amount_, p.amountText, invoice.amount);");
}

// The invoice reads top to bottom as §6.2 lists it.
UR_TEST(CryptoPayWiring_TheInvoiceReadsInTheContractsOrder) {
  const std::string view = ReadCode("CryptoInvoiceView.cpp");
  const std::string build = FunctionBody(view, "CryptoInvoiceView::CryptoInvoiceView() :");
  InOrder(build, {"network_ = MakeLine(\"title-2\");",
                  "T_(\"crypto_invoice_send_exactly_to_address\"", "append(*byHandLine_);",
                  "append(*qr_);", "append(*noLinkNote_);", "append(*openWallet_);",
                  "BuildCopyRow(amount_, T_(\"crypto_invoice_send_exactly\", \"Send exactly\"));",
                  "BuildCopyRow(address_, T_(\"crypto_invoice_to_address\", \"To this address\"));",
                  "networkWarning_ = MakeLine(\"ur-amber\");", "append(*networkWarning_);",
                  "BuildCopyRow(memo_, T_(\"crypto_invoice_memo\", \"Memo, if you send by hand\"));",
                  "T_(\"crypto_invoice_quote_expired\"", "T_(\"crypto_invoice_refresh_quote\"",
                  "append(*steps_);", "append(*remaining_);", "T_(\"crypto_status_review\"",
                  "append(*wallets_);", "T_(\"crypto_invoice_zcash_exchange_note\"",
                  "append(*notice_);", "T_(\"crypto_invoice_pay_another_way\", \"Pay another way\")",
                  "T_(\"close\", \"Close\")"});
  // the QR is ink on paper, at whole pixels per module
  const std::string draw = FunctionBody(view, "void CryptoInvoiceView::DrawQr(");
  InOrder(draw, {"kPaper", "crypto_pay::QrModulePixels(side, count)", "kInk"});
}

// No AdwToggleGroup (the AppImage's libadwaita 1.5 lacks it): linked toggle
// buttons in one group, named "Pay with".
UR_TEST(CryptoPayWiring_TheRowIsLinkedToggleButtons) {
  const std::string row = ReadCode("PaymentMethodRow.cpp");
  Has(row, "toggles_->add_css_class(\"linked\");");
  Has(row, "kit::SetAccessibleLabel(*toggles_, T_(\"pay_method_label\", \"Pay with\"));");
  Has(row, "auto* toggle = Gtk::make_managed<Gtk::ToggleButton>();");
  Has(row, "toggle->set_child(content);");
  Has(row, "toggle->set_group(*group);");
  Has(row, "set_visible(crypto_pay::MethodRowShown(tabs_));");
  Has(row, "T_(\"pay_method_unavailable\", \"Temporarily unavailable\")");
  for (const char* file : {"PaymentMethodRow.cpp", "UpgradeSheet.cpp", "Onboarding.cpp"}) {
    const std::string code = ReadCode(file);
    UR_EXPECT_TRUE_MSG(file, !Contains(code, "AdwToggleGroup") && !Contains(code, "adw_toggle_group"));
  }
  const std::string picker = ReadCode("PlanPicker.cpp");
  InOrder(FunctionBody(picker, "void PlanPicker::SetMonthlyAllowed(bool allowed) {"),
          {"monthlyCard_->set_sensitive(allowed);",
           "monthlyLine_->set_visible(allowed && !monthlyLine_->get_text().empty());",
           "monthlyNote_->set_visible(!allowed);", "kit::SetAccessibleLabel(*monthlyCard_,"});
  Has(picker, "T_(\"pay_method_card_only_monthly\", \"Monthly is card only\")");
}

// No renewal or trial wording while a coin is selected (CONTRACT §6.1): the
// offer's "then …/year" is marked where it is composed, and the yearly card
// leaves it out for a coin with its trial line, keeping its height.
UR_TEST(CryptoPayWiring_ACoinShowsNoRenewalOrTrialWording) {
  const std::string offer = ReadCode("OfferCard.cpp");
  InOrder(FunctionBody(offer, "PlanCardTexts ComposePlanCardTexts("),
          {"if (offer.active) {", "T_(\"offer_then_regular_price\", \"then {}/year\")",
           "texts.yearlySecondaryRenews = true;", "} else {"});
  InOrder(FunctionBody(offer, "void OfferCard::SetCardTermsShown(bool shown) {"),
          {"timeline_->set_visible(shown);", "terms_->set_visible(shown);"});
  const std::string picker = ReadCode("PlanPicker.cpp");
  Has(picker, "texts.yearlyTrial, texts.yearlySecondaryRenews);");
  const std::string layout = FunctionBody(picker, "void Layout() {");
  InOrder(layout, {"crypto_pay::YearlyCardLines(", "savingRenews_", "!cardTerms_",
                   "row->set_opacity(0.0);", "trial_.set_opacity(cardTerms_ ? 1.0 : 0.0);",
                   "kit::SetAccessibleLabel(*this, PlanCardName("});
  // the one-time year's price stays: no surface hides the first-year price
  for (const char* file : {"PlanPicker.cpp", "Onboarding.cpp", "UpgradeSheet.cpp"}) {
    const std::string code = ReadCode(file);
    UR_EXPECT_TRUE_MSG(file, !Contains(code, "offerPrice_->set_visible("));
  }
}

// One invoice at a time (CONTRACT §6.2.1): both coin paths show a payment
// going on instead of starting another.
UR_TEST(CryptoPayWiring_OneInvoiceAtATime) {
  const std::string sheet = ReadCode("UpgradeSheet.cpp");
  InOrder(FunctionBody(sheet, "void UpgradeSheet::StartCrypto(const std::string& plan) {"),
          {"crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())", "ShowInvoice();",
           "return;", "host_.CryptoInvoice().StartInvoice(method_, plan)"});
  InOrder(FunctionBody(sheet, "void UpgradeSheet::OpenCryptoCheckout("),
          {"crypto_pay::InvoiceInProgress(host_.CryptoInvoice().State())", "ShowInvoice();",
           "StartCrypto(plan);"});
}

// The onboarding's two purchase pages carry the row, and their CTA opens the
// coin's invoice.
UR_TEST(CryptoPayWiring_TheOnboardingPagesCarryTheRow) {
  const std::string onboarding = ReadCode("Onboarding.cpp");
  InOrder(FunctionBody(onboarding, "void OnboardingWindow::BuildWelcome() {"),
          {"top->append(*plans_);", "top->append(*welcomeOffer_);",
           "welcomeMethods_ = Gtk::make_managed<PaymentMethodRow>();", "top->append(*welcomeMethods_);",
           "top->append(*start);", "top->append(*welcomeFinePrint_);"});
  InOrder(FunctionBody(onboarding, "void OnboardingWindow::BuildOffer() {"),
          {"offerMethods_ = Gtk::make_managed<PaymentMethodRow>();", "page->append(*offerMethods_);",
           "page->append(*offerCta_);", "page->append(*offerFinePrint_);"});
  const std::string start = FunctionBody(onboarding, "void OnboardingWindow::StartCheckout(");
  InOrder(start, {"PaymentMethodEventStore(method_)",
                  "checkout_->OpenCryptoCheckout(method_, crypto_pay::CoinPlan(ShowsOffer()));",
                  "checkout_->OpenCheckout(yearly);"});
  Has(FunctionBody(onboarding, "bool OnboardingWindow::ShowsOffer() const {"),
      "offerEnabled_ && balance_.Offer().active");
  InOrder(FunctionBody(onboarding, "void OnboardingWindow::ApplyMethod() {"),
          {"plans_->SetMonthlyAllowed(!coin);", "plans_->SetCardTermsShown(!coin);", "SelectPlan(true);",
           "welcomeOffer_->SetCardTermsShown(!coin);", "offerCard_->SetCardTermsShown(!coin);",
           "offerThen_->set_visible(!coin);", "offerTrial_->set_visible(!coin);",
           "offerCta_->set_label(OfferCtaLabel());"});
  // both pages pick the one method, remembered on this device
  UR_EXPECT_EQ(size_t{2}, CountOf(onboarding, "host_.CryptoInvoice().SetPaymentMethod(method);"));
}

// The balance result's coins and crypto term reach the surfaces through the
// SDK's rules.
UR_TEST(CryptoPayWiring_TheBalanceCarriesTheCoins) {
  const std::string store = ReadCode("SubscriptionBalance.cpp");
  Has(store, "cryptoAssets_ = result->crypto_assets;");
  Has(store, "cryptoUnavailableAssets_ = result->crypto_unavailable_assets;");
  Has(store, "cryptoTerm_ = result->crypto_term;");
  // the SDK's tabs over the balance's two lists: a coin it cannot quote now
  // greys out in its place instead of vanishing (and More coins with it when
  // all four are), and the SDK alone says whether there is a selector; each
  // coin with the SDK's network line
  const std::string tabs =
      FunctionBody(store, "std::vector<crypto_pay::PaymentTabView> SubscriptionBalanceStore::PaymentTabs(");
  InOrder(tabs, {"urnet::cryptoPaymentTabs(cryptoAssets_, cryptoUnavailableAssets_,",
                 "urnet::StringList{plan})", "tab.labelKey = sdkTab.label_key;",
                 "coin.iconKey = sdkCoin.icon_key;", "coin.available = sdkCoin.available;",
                 "if (const auto lines = urnet::cryptoAssetLinesFor(sdkCoin.asset)) {",
                 "coin.networkLine = {lines->network_line_key, lines->network_line};",
                 "tab.available = sdkTab.available;"});
  UR_EXPECT_FALSE(Contains(tabs, "cryptoAssets_->empty()") || Contains(tabs, "CryptoPaymentAssetList{}"));
  UR_EXPECT_FALSE(Contains(store, "urnet::paymentMethods("));
  Has(FunctionBody(store, "bool SubscriptionBalanceStore::CryptoTermNoticeDue() const {"),
      "urnet::cryptoTermNoticeDue(cryptoTerm_)");
  // a sign-in and a sign-out forget the last network's
  UR_EXPECT_EQ(size_t{2}, CountOf(store, "cryptoAssets_.reset();"));
  UR_EXPECT_EQ(size_t{2}, CountOf(store, "cryptoUnavailableAssets_.reset();"));
  UR_EXPECT_EQ(size_t{2}, CountOf(store, "cryptoTerm_.reset();"));
}

// Account: the renewal notice with Renew, "does not renew", and "Payment in
// progress", which reopens the saved invoice; the window feeds them.
UR_TEST(CryptoPayWiring_TheAccountRows) {
  const std::string page = ReadCode("AccountPage.cpp");
  const std::string build = FunctionBody(page, "void AccountPage::BuildPlanPane() {");
  InOrder(build, {"planValue_->add_controller(planTap);", "T_(\"crypto_term_renew\", \"Renew\")",
                  "on_open_upgrade();", "T_(\"crypto_does_not_renew\", \"Does not renew. Pay again when it ends.\")",
                  "content->append(*upgradeButton_);",
                  "T_(\"crypto_payment_in_progress\", \"Payment in progress\")",
                  "on_open_crypto_invoice();"});
  const std::string apply = FunctionBody(page, "void AccountPage::ApplyBalance(const AccountBalance& snapshot) {");
  InOrder(apply, {"balance_.cryptoTermNotice && !balance_.guest",
                  "T_(\"crypto_term_ended_notice\",", "Renew by {} to keep Pro.",
                  "balance_.cryptoTermRenewBy", "crypto_pay::StoreDoesNotRenew(balance_.subscriptionStoreFamily)"});
  // a crypto store never gets the Stripe portal
  Has(apply, "portalRow_->set_visible(ShowsManageSubscription(balance_.subscriptionStoreFamily))");
  const std::string window = ReadCode("MainWindow.cpp");
  Has(window, "snapshot.cryptoTermNotice = balance_.CryptoTermNoticeDue();");
  Has(window, "snapshot.cryptoTermRenewBy = LocalDateText(balance_.CryptoTermGraceEnd());");
  Has(window, "accountPage_->on_open_crypto_invoice = [this] { OpenCryptoInvoice(); };");
  Has(FunctionBody(window, "void MainWindow::OpenCryptoInvoice() {"), "upgradeSheet_->OpenInvoice();");
  const std::string invoiceRow = FunctionBody(window, "void MainWindow::ApplyCryptoInvoiceRow() {");
  InOrder(invoiceRow, {"const crypto_pay::CoinLine& network = invoice.lines.networkLine;",
                       "crypto_pay::InvoiceInProgress(invoice)", "LabelText(network.key, network.english)"});
  // Account appearing resumes the saved invoice (§6.2)
  InOrder(window, {"if (tag == \"account\" && accountPage_) {", "accountPage_->Load();",
                   "host_.CryptoInvoice().Resume();"});
}

// The new GUI sources build into the app, and the QR encoder into the tests.
UR_TEST(CryptoPayWiring_TheBuildListsTheSources) {
  const std::string build = ReadRaw("../meson.build");
  for (const char* source : {"'src/PaymentMethodRow.cpp'", "'src/CryptoInvoiceView.cpp'",
                             "'src/CryptoInvoiceHost.cpp'"}) {
    UR_EXPECT_TRUE_MSG(source, Before(build, "executable('urnetwork-gui',", source) &&
                                   Before(build, source, "executable('urnetworkd',"));
  }
  const size_t tests = build.find("urnetwork_tests = executable('urnetwork-tests',");
  UR_EXPECT_TRUE(tests != std::string::npos);
  if (tests == std::string::npos) return;
  const std::string testTarget = build.substr(tests, build.find("install : false)", tests) - tests);
  for (const char* source : {"'tests/CryptoPayTest.cpp'", "'tests/CryptoPayWiringTest.cpp'",
                             "'third_party/qrcodegen/qrcodegen.cpp'", "qrcodegen_inc"}) {
    UR_EXPECT_TRUE_MSG(source, Contains(testTarget, source));
  }
  // Open in wallet inside the Flatpak goes through the OpenURI portal
  Has(ReadRaw("../../packaging/flatpak/com.bringyour.network.yml"),
      "--talk-name=org.freedesktop.portal.OpenURI");
  // More coins' marks are installed where CoinMark reads them
  InOrder(build, {"install_data('assets/coin_tao.svg', 'assets/coin_btc.svg', 'assets/coin_eth.svg',",
                  "'assets/coin_sol.svg',",
                  "install_dir : get_option('datadir') / 'urnetwork/coins')"});
}

// More coins (mmm/BTCPAY.md §17.6): the tab shows its coins' marks, each its
// own with space between them, names the coins for a screen reader, and opens
// a list titled with its name, each coin with its mark and its network line; a
// coin or tab that cannot be paid with now greys its label without going
// insensitive; the selection's tab is the SDK's tab id; the labels are the
// SDK's keys with its English; the marks are the installed svgs, filled
// nonzero in white, never dimmed.
UR_TEST(CryptoPayWiring_MoreCoins) {
  const std::string row = ReadCode("PaymentMethodRow.cpp");
  const std::string rebuild = FunctionBody(row, "void PaymentMethodRow::Rebuild() {");
  InOrder(rebuild, {"if (crypto_pay::MethodRowShown(tabs_)) {",
                    "PaymentMethodName(crypto_pay::kMethodCard)",
                    "for (const crypto_pay::PaymentTabView& tab : tabs_) {",
                    "LabelText(tab.labelKey, tab.label)",
                    "auto* marks = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);",
                    "for (const std::string& iconKey : tab.iconKeys) {",
                    "Gtk::make_managed<CoinMark>(iconKey, kTabMarkPx);",
                    "T_(\"pay_method_more_coins_a11y\", \"More coins: {}\")",
                    "const crypto_pay::CoinLine& network = coin.networkLine;",
                    "description = LabelText(network.key, network.english);", "radio->set_child(*line);",
                    "coinChoices_.push_back(CoinChoice{coin.asset, radio, nameLabel, coin.available});",
                    "ApplyChoosable();"});
  // a tap on a tab takes the SDK's coin for it, asked with the coin chosen
  // last in that tab (kept per tab: a choice in another tab never replaces
  // it) and the remembered method
  Has(rebuild, "const std::string coin = TabCoinFor(tab, lastChosen_.In(tab.id), remembered_);");
  InOrder(FunctionBody(row, "std::string TabCoinFor(const crypto_pay::PaymentTabView& tab,"),
          {"urnet::cryptoPaymentTabCoin(TabToSdk(tab), lastChosen, remembered);",
           "return crypto_pay::IsCoin(coin) ? coin : std::string();"});
  InOrder(FunctionBody(row, "urnet::CryptoPaymentTab TabToSdk(const crypto_pay::PaymentTabView& tab) {"),
          {"sdkTab.id = tab.id;", "sdkCoin.available = coin.available;", "sdkTab.available = tab.available;"});
  // every selection counts as the coin chosen last in its tab, a programmatic
  // one too (the remembered method, the onboarding's one choice on two rows),
  // and what the row is given is the remembered method
  InOrder(FunctionBody(row, "void PaymentMethodRow::Select(const std::string& method) {"),
          {"remembered_ = method;", "selected_ = crypto_pay::MethodSelection(tabs_, method);",
           "lastChosen_.Choose(tabs_, selected_);"});
  InOrder(FunctionBody(row, "void PaymentMethodRow::Pick(const std::string& method) {"),
          {"selected_ = crypto_pay::MethodSelection(tabs_, method);", "lastChosen_.Choose(tabs_, selected_);",
           "if (on_select) on_select(selected_);"});
  // one coin chosen last anywhere was the bug: More coins fell back to TAO
  UR_EXPECT_FALSE(Contains(row, "wanted_"));
  // unavailable greys the label and takes no pointer or focus, never
  // insensitive: libadwaita's disabled filter would dim the mark with it
  InOrder(FunctionBody(row, "void SetChoosable(Gtk::Widget& choice, Gtk::Label& label, bool choosable) {"),
          {"choice.set_can_target(choosable);", "choice.set_focusable(choosable);",
           "label.add_css_class(\"dim-label\");", "GTK_ACCESSIBLE_STATE_DISABLED"});
  UR_EXPECT_FALSE(Contains(row, "set_sensitive("));
  UR_EXPECT_FALSE(Contains(row, "set_opacity("));
  // the selection's tab is the SDK's tab id
  Has(FunctionBody(row, "void PaymentMethodRow::Sync() {"),
      "const std::string tabId = PaymentTabIdFor(selected_);");
  Has(FunctionBody(row, "std::string PaymentTabIdFor(const std::string& method) {"),
      "return urnet::cryptoPaymentTabId(method);");
  // the labels by the SDK's key, its English the msgid ("Pay with USDC on Solana")
  Has(FunctionBody(row, "Glib::ustring LabelText(const std::string& key, const std::string& english) {"),
      "return Glib::ustring(T_(key.c_str(), english.c_str()));");
  InOrder(FunctionBody(row, "Glib::ustring PaymentMethodName(const std::string& method) {"),
          {"T_(\"pay_method_card\", \"Card\")",
           "LabelText(urnet::cryptoAssetLabelKey(method), urnet::cryptoAssetLabel(method))"});

  // the marks: the installed svgs, filled nonzero in white whatever the coin's
  // state, on this dark-only app
  const std::string icons = ReadCode("BrandIcons.cpp");
  Has(icons, "constexpr GdkRGBA kMarkOnDark{1.f, 1.f, 1.f, 1.f};");
  InOrder(FunctionBody(icons, "GskPath* CoinMarkPath(const std::string& iconKey) {"),
          {"if (crypto_pay::IsMarkKey(iconKey)) {",
           "std::string(UR_PKGDATADIR) + \"/coins/\" + iconKey + \".svg\"",
           "\"assets/\" + iconKey + \".svg\"", "crypto_pay::SvgPathData(",
           "gsk_path_parse(data.c_str())"});
  InOrder(FunctionBody(icons, "void CoinMark::snapshot_vfunc("),
          {"gtk_snapshot_push_fill(snap, path_, GSK_FILL_RULE_WINDING);",
           "gtk_snapshot_append_color(snap, &kMarkOnDark, &bounds);"});
  Has(ReadCode("main.cpp"), "ADW_COLOR_SCHEME_FORCE_DARK");
}
