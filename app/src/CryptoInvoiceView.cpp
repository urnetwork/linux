// SPDX-License-Identifier: MPL-2.0
#include "CryptoInvoiceView.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <gio/gio.h>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "PaymentMethodRow.hpp"  // PaymentMethodName
#include "Ui.hpp"

namespace urnw {
namespace {

// The code is drawn ink on paper whatever the theme: a scanner reads dark on
// light (ExtenderShareSheet.cpp's rule).
constexpr Rgba kPaper{1.0, 1.0, 1.0, 1.0};
constexpr Rgba kInk{0.0, 0.0, 0.0, 1.0};
// how long a Copy button reads "Copied"
constexpr unsigned kCopiedMillis = 1500;

Gtk::Label* MakeLine(const char* cssClass = nullptr) {
  auto* label = Gtk::make_managed<Gtk::Label>();
  label->set_xalign(0);
  label->set_wrap(true);
  label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  if (cssClass) label->add_css_class(cssClass);
  return label;
}

}  // namespace

CryptoInvoiceView::CryptoInvoiceView() : Gtk::Box(Gtk::Orientation::VERTICAL, 12) {
  EnsureDrawerCss();

  // 1. the network to send on, named before anything is asked for
  network_ = MakeLine("title-2");
  append(*network_);
  // a coin with no payment link (TAO): nothing fills the amount in for the
  // payer, so the invoice says to send exactly this amount to this address
  byHandLine_ = MakeLine();
  byHandLine_->set_text(T_("crypto_invoice_send_exactly_to_address",
                           "Send exactly this amount to this address."));
  byHandLine_->set_visible(false);
  append(*byHandLine_);

  // 2. the SDK's qr text as a QR (a checked payment URI, or TAO's address
  // alone, with the note saying so), and Open in wallet
  qr_ = Gtk::make_managed<Gtk::DrawingArea>();
  qr_->set_content_width(crypto_pay::kQrMinSide);
  qr_->set_content_height(crypto_pay::kQrMinSide);
  qr_->set_halign(Gtk::Align::CENTER);
  qr_->set_margin_top(4);
  qr_->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    DrawQr(cr, width, height);
  });
  // the amount and the address say the same thing to a screen reader
  kit::MarkDecorative(*qr_);
  qr_->set_visible(false);
  append(*qr_);
  noLinkNote_ = MakeLine("dim-label");
  noLinkNote_->add_css_class("caption");
  noLinkNote_->set_justify(Gtk::Justification::CENTER);
  noLinkNote_->set_xalign(0.5f);
  noLinkNote_->set_visible(false);
  append(*noLinkNote_);
  openWallet_ = Gtk::make_managed<Gtk::Button>(T_("crypto_invoice_open_wallet", "Open in wallet"));
  openWallet_->add_css_class("suggested-action");
  openWallet_->add_css_class("pill");
  openWallet_->set_halign(Gtk::Align::CENTER);
  openWallet_->signal_clicked().connect([this] { OpenInWallet(); });
  openWallet_->set_visible(false);
  append(*openWallet_);

  // 3. what to send, where to (with the coin's network warning: a transfer on
  // another network is not credited), and the memo of a coin on Solana
  BuildCopyRow(amount_, T_("crypto_invoice_send_exactly", "Send exactly"));
  BuildCopyRow(address_, T_("crypto_invoice_to_address", "To this address"));
  networkWarning_ = MakeLine("ur-amber");
  networkWarning_->add_css_class("caption");
  networkWarning_->set_visible(false);
  append(*networkWarning_);
  BuildCopyRow(memo_, T_("crypto_invoice_memo", "Memo, if you send by hand"));

  // 4. the quote's time, or its end, with Refresh quote
  countdownRow_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  countdown_ = MakeLine();
  countdown_->set_hexpand(true);
  countdown_->set_valign(Gtk::Align::CENTER);
  countdownRow_->append(*countdown_);
  expired_ = MakeLine();
  expired_->set_hexpand(true);
  expired_->set_valign(Gtk::Align::CENTER);
  expired_->set_text(T_("crypto_invoice_quote_expired",
                        "This quote expired. Refresh it for a new amount."));
  countdownRow_->append(*expired_);
  refresh_ = Gtk::make_managed<Gtk::Button>(T_("crypto_invoice_refresh_quote", "Refresh quote"));
  refresh_->set_valign(Gtk::Align::CENTER);
  refresh_->signal_clicked().connect([this] {
    if (on_refresh) on_refresh();
  });
  countdownRow_->append(*refresh_);
  countdownRow_->set_visible(false);
  append(*countdownRow_);

  // 5. the status steps, what is left to send, or the review copy
  steps_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  for (StepRow& row : stepRows_) {
    row.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row.dot = Gtk::make_managed<Gtk::Label>();
    row.dot->set_valign(Gtk::Align::START);
    kit::MarkDecorative(*row.dot);  // the text carries the step
    row.root->append(*row.dot);
    row.text = MakeLine();
    row.text->set_hexpand(true);
    row.root->append(*row.text);
    steps_->append(*row.root);
  }
  steps_->set_visible(false);
  append(*steps_);
  remaining_ = MakeLine();
  remaining_->set_visible(false);
  append(*remaining_);
  review_ = MakeLine();
  review_->set_text(T_("crypto_status_review",
                       "We received a payment that needs a manual check. Contact support and "
                       "we'll sort it out."));
  review_->set_visible(false);
  append(*review_);

  // 6. the wallets that work, and Zcash's exchange note
  wallets_ = MakeLine("dim-label");
  wallets_->add_css_class("caption");
  wallets_->set_visible(false);
  append(*wallets_);
  zcashNote_ = MakeLine("dim-label");
  zcashNote_->add_css_class("caption");
  zcashNote_->set_text(T_("crypto_invoice_zcash_exchange_note",
                          "Exchanges such as Coinbase and Robinhood can't send to this shielded "
                          "address. Send from a shielded wallet."));
  zcashNote_->set_visible(false);
  append(*zcashNote_);
  notice_ = MakeLine("ur-error-text");
  notice_->add_css_class("caption");
  notice_->set_visible(false);
  append(*notice_);

  // 7. Pay another way and Close
  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_margin_top(8);
  payAnotherWay_ =
      Gtk::make_managed<Gtk::Button>(T_("crypto_invoice_pay_another_way", "Pay another way"));
  payAnotherWay_->add_css_class("flat");
  payAnotherWay_->set_halign(Gtk::Align::START);
  payAnotherWay_->set_hexpand(true);
  payAnotherWay_->signal_clicked().connect([this] {
    if (on_pay_another_way) on_pay_another_way();
  });
  actions->append(*payAnotherWay_);
  close_ = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  // right-aligned whether or not Pay another way shows beside it
  close_->set_halign(Gtk::Align::END);
  close_->set_hexpand(true);
  close_->signal_clicked().connect([this] {
    if (on_close) on_close();
  });
  actions->append(*close_);
  append(*actions);
}

CryptoInvoiceView::~CryptoInvoiceView() {
  // a pending "Copied" -> "Copy" must not outlive its button
  for (CopyRow* row : {&amount_, &address_, &memo_}) row->reset.disconnect();
}

void CryptoInvoiceView::BuildCopyRow(CopyRow& row, const Glib::ustring& caption) {
  row.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  auto* title = MakeLine("dim-label");
  title->add_css_class("caption");
  title->set_text(caption);
  row.root->append(*title);
  auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  row.value = MakeLine("ur-mono-13");
  row.value->set_hexpand(true);
  row.value->set_valign(Gtk::Align::CENTER);
  row.value->set_selectable(true);
  // A long address or memo breaks anywhere rather than overflowing the sheet,
  // and never with a hyphen: Pango adds one where it breaks a word, and a
  // hyphen read into an address is a wrong address.
  row.value->set_wrap_mode(Pango::WrapMode::CHAR);
  Pango::AttrList attributes;
  auto noHyphens = Pango::Attribute::create_attr_insert_hyphens(false);
  attributes.insert(noHyphens);
  row.value->set_attributes(attributes);
  line->append(*row.value);
  row.copy = Gtk::make_managed<Gtk::Button>(T_("crypto_invoice_copy", "Copy"));
  row.copy->set_valign(Gtk::Align::CENTER);
  // three Copy buttons: each says what it copies to a screen reader
  gtk_accessible_update_property(GTK_ACCESSIBLE(row.copy->gobj()),
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, caption.c_str(), -1);
  CopyRow* target = &row;
  row.copy->signal_clicked().connect([this, target] { Copy(*target); });
  line->append(*row.copy);
  row.root->append(*line);
  row.root->set_visible(false);
  append(*row.root);
}

void CryptoInvoiceView::SetCopyText(CopyRow& row, const std::string& shown,
                                    const std::string& copied) {
  // the countdown re-renders every second: leave an unchanged value (and its
  // selection) alone
  if (row.value->get_text().raw() != shown) row.value->set_text(shown);
  row.text = copied;
}

Glib::ustring CryptoInvoiceView::IdleCopyLabel(const CopyRow& row) const {
  // a coin paid by hand carries two values over to the wallet: each button
  // names its own
  if (copySaysWhat_ && &row == &amount_) return T_("crypto_invoice_copy_amount", "Copy amount");
  if (copySaysWhat_ && &row == &address_) return T_("crypto_invoice_copy_address", "Copy address");
  return T_("crypto_invoice_copy", "Copy");
}

void CryptoInvoiceView::Copy(CopyRow& row) {
  if (row.text.empty()) return;
  row.copy->get_clipboard()->set_text(row.text);
  row.copy->set_label(T_("crypto_invoice_copied", "Copied"));
  row.reset.disconnect();
  CopyRow* target = &row;
  row.reset = Glib::signal_timeout().connect(
      [this, target] {
        target->copy->set_label(IdleCopyLabel(*target));
        return false;
      },
      kCopiedMillis);
}

void CryptoInvoiceView::Render(const crypto_pay::InvoiceView& invoice) {
  const crypto_pay::InvoicePresentation p = crypto_pay::PresentInvoice(invoice);

  const crypto_pay::Text network = crypto_pay::NetworkLine(invoice.asset);
  network_->set_text(network.key ? Glib::ustring(T_(network.key, network.english))
                                 : Glib::ustring());
  network_->set_visible(network.key != nullptr);
  byHandLine_->set_visible(p.showByHandLine);

  // Only the SDK's qr text is drawn (p.qrText: a URI the SDK's check accepted,
  // or TAO's address alone), and only a checked URI is opened
  // (p.showOpenWallet). The code is encoded once per text, and the canvas
  // sized from its modules so every module is a whole number of pixels.
  const std::string qrText = p.qrText;
  if (qrText != qrText_) {
    qrText_ = qrText;
    qrModules_ = crypto_pay::EncodePaymentQr(qrText);
    const int side = crypto_pay::QrCanvasSide(qrModules_.count);
    qr_->set_content_width(side);
    qr_->set_content_height(side);
    qr_->queue_draw();
  }
  const bool qrDrawn = p.showQr && 0 < qrModules_.count;
  qr_->set_visible(qrDrawn);
  const crypto_pay::Text note = p.noPaymentLinkNote;
  noLinkNote_->set_text(note.key ? Glib::ustring(T_(note.key, note.english)) : Glib::ustring());
  noLinkNote_->set_visible(qrDrawn && note.key != nullptr);
  openUri_ = p.showOpenWallet ? invoice.uri : std::string();
  openWallet_->set_visible(p.showOpenWallet);

  if (p.noPaymentLink != copySaysWhat_) {
    copySaysWhat_ = p.noPaymentLink;
    // a button showing "Copied" takes its label back when its moment ends
    for (CopyRow* row : {&amount_, &address_, &memo_}) {
      if (!row->reset.connected()) row->copy->set_label(IdleCopyLabel(*row));
    }
  }
  // the amount is copied without its ticker: a wallet's amount field takes a
  // number
  SetCopyText(amount_, p.amountText, invoice.amount);
  amount_.root->set_visible(p.showAmount);
  SetCopyText(address_, invoice.address, invoice.address);
  address_.root->set_visible(p.showAddress);
  const crypto_pay::Text warning = p.networkWarning;
  networkWarning_->set_text(warning.key ? Glib::ustring(T_(warning.key, warning.english))
                                        : Glib::ustring());
  networkWarning_->set_visible(p.showNetworkWarning);
  SetCopyText(memo_, invoice.memo, invoice.memo);
  memo_.root->set_visible(p.showMemo);

  countdownRow_->set_visible(p.showCountdown || p.expired || p.showRefresh);
  countdown_->set_visible(p.showCountdown);
  if (p.showCountdown) {
    countdown_->set_text(
        Format(T_("crypto_invoice_expires_in", "Quote expires in {}"), p.countdownText));
  }
  expired_->set_visible(p.expired);
  refresh_->set_visible(p.showRefresh);
  refresh_->set_sensitive(p.refreshEnabled);

  RenderSteps(p);
  remaining_->set_visible(p.showRemaining);
  if (p.showRemaining) {
    remaining_->set_text(
        Format(T_("crypto_invoice_remaining", "{} more to send"), p.remainingText));
  }
  review_->set_visible(p.review);

  wallets_->set_visible(p.showWallets);
  if (p.showWallets) {
    wallets_->set_text(
        Format(T_("crypto_invoice_wallets", "Wallets that work: {}"), p.walletsText));
  }
  zcashNote_->set_visible(p.zcashNote);

  switch (p.notice) {
    case crypto_pay::InvoiceNotice::None:
      notice_->set_visible(false);
      break;
    case crypto_pay::InvoiceNotice::Unavailable:
      notice_->set_text(Format(T_("crypto_invoice_unavailable",
                                  "Paying with {} is temporarily unavailable. Try again later, or "
                                  "pay another way."),
                               PaymentMethodName(invoice.asset).raw()));
      notice_->set_visible(true);
      break;
    case crypto_pay::InvoiceNotice::Failed:
      notice_->set_text(T_("something_went_wrong_please_try_again_later",
                           "Something went wrong. Please try again later."));
      notice_->set_visible(true);
      break;
  }
  payAnotherWay_->set_visible(p.showPayAnotherWay);
}

void CryptoInvoiceView::RenderSteps(const crypto_pay::InvoicePresentation& p) {
  for (StepRow& row : stepRows_) row.root->set_visible(false);
  steps_->set_visible(!p.steps.empty());
  for (const crypto_pay::Step& step : p.steps) {
    StepRow& row = stepRows_[static_cast<size_t>(step.kind)];
    Glib::ustring text;
    switch (step.kind) {
      case crypto_pay::StepKind::Waiting:
        text = T_("crypto_status_waiting", "Waiting for your payment");
        break;
      case crypto_pay::StepKind::Seen:
        text = T_("crypto_status_seen", "Payment seen");
        break;
      case crypto_pay::StepKind::Confirming:
        // a resumed invoice learns its count from the first status poll; an
        // upcoming step with no count yet waits for it
        if (p.confirmationsRequired <= 0 && step.state == crypto_pay::StepState::Upcoming) continue;
        text = Format(T_("crypto_status_confirming", "Confirming: {0} of {1}"), p.confirmations,
                      p.confirmationsRequired);
        break;
      case crypto_pay::StepKind::Paid:
        text = T_("crypto_status_paid", "Paid. Thank you!");
        break;
    }
    const Rgba* color = &kUrTextFaint;
    const char* mark = "○";
    switch (step.state) {
      case crypto_pay::StepState::Done:
        color = &kUrGreen;
        mark = "●";
        break;
      case crypto_pay::StepState::Current:
        color = &kUrPink;
        mark = "●";
        break;
      case crypto_pay::StepState::Upcoming:
        break;
    }
    row.dot->set_markup("<span foreground='" + HexForMarkup(*color) + "'>" + mark + "</span>");
    if (row.text->get_text() != text) row.text->set_text(text);
    if (step.state == crypto_pay::StepState::Current) {
      row.text->add_css_class("heading");
    } else {
      row.text->remove_css_class("heading");
    }
    if (step.state == crypto_pay::StepState::Upcoming) {
      row.text->add_css_class("dim-label");
    } else {
      row.text->remove_css_class("dim-label");
    }
    row.root->set_visible(true);
  }
}

void CryptoInvoiceView::DrawQr(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
  const int side = std::min(width, height);
  if (side <= 0) return;
  cr->set_source_rgba(kPaper.r, kPaper.g, kPaper.b, kPaper.a);
  cr->rectangle(0, 0, side, side);
  cr->fill();
  const int count = qrModules_.count;
  // whole pixels per module, or the edges antialias and the code stops
  // scanning (crypto_pay::QrModulePixels)
  const int unit = crypto_pay::QrModulePixels(side, count);
  if (unit < 1) return;  // no room: the address and amount carry the invoice
  const int drawn = unit * (count + 2 * crypto_pay::kQrQuietModules);
  const double origin = std::floor((side - drawn) / 2.0);
  cr->set_source_rgba(kInk.r, kInk.g, kInk.b, kInk.a);
  for (int y = 0; y < count; ++y) {
    for (int x = 0; x < count; ++x) {
      if (!qrModules_.Dark(x, y)) continue;
      cr->rectangle(origin + (x + crypto_pay::kQrQuietModules) * unit,
                    origin + (y + crypto_pay::kQrQuietModules) * unit, unit, unit);
    }
  }
  cr->fill();
}

void CryptoInvoiceView::OpenInWallet() {
  // only a URI that passed the SDK's check is ever handed on (Render)
  if (openUri_.empty()) return;
  GError* error = nullptr;
  // the desktop's handler for the coin's scheme (monero:, zcash:, solana:);
  // inside the Flatpak GLib asks the OpenURI portal, which the manifest grants
  if (!g_app_info_launch_default_for_uri(openUri_.c_str(), nullptr, &error)) {
    // No wallet app takes it: the QR stays on screen for a phone, and nothing
    // alarming is said. Never the URI itself in a log: it names the address.
    g_debug("crypto invoice: no app opened the payment uri: %s",
            error ? error->message : "no detail");
    if (error) g_error_free(error);
  }
}

}  // namespace urnw
