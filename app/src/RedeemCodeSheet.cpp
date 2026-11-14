// SPDX-License-Identifier: MPL-2.0
#include "RedeemCodeSheet.hpp"

#include "BalanceCodeRedeem.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

#include <string_view>

namespace urnw {

// BalanceCodeRedeem.hpp reads a gift code's kind by its text
static_assert(std::string_view(urnet::BalanceCodeKindProGift) == kBalanceCodeKindProGift);

namespace {
// first 3 ... last 3 of a redeemed code's secret (mac TransferBalanceCodesView)
std::string MaskSecret(const std::string& secret) {
  constexpr size_t keep = 3;
  if (secret.size() <= keep * 2) return std::string(secret.size(), '.');
  return secret.substr(0, keep) + "..." + secret.substr(secret.size() - keep);
}

// the YYYY-MM-DD prefix of an ISO timestamp (the Windows redeemed-codes list)
std::string IsoDate(const std::string& isoTime) {
  return isoTime.size() >= 10 ? isoTime.substr(0, 10) : isoTime;
}

// An RFC 3339 time as the locale's date in this machine's zone ("10/20/2026"),
// as the crypto year's renewal notice dates its grace (MainWindow.cpp
// LocalDateText); "" when it does not read.
std::string LocalDateText(const std::string& rfc3339) {
  if (rfc3339.empty()) return std::string();
  const Glib::DateTime when = Glib::DateTime::create_from_iso8601(rfc3339);
  if (!when) return std::string();
  return LocalDate(when.to_unix());
}

// The gift confirmation (BalanceCodeRedeem.hpp ShowsQueuedGiftLine): the day a
// year queued after the network's paid Pro period starts, or "A year of Pro is
// yours." `result` is a credited redeem's answer, or nothing for a gift found
// in the redeemed list, which does not say whether its year was queued.
std::string GiftRedeemedText(const std::optional<urnet::RedeemBalanceCodeResult>& result) {
  const bool queued = result && result->pro && result->pro->queued;
  const std::string date =
      queued && result->pro->start_time ? LocalDateText(*result->pro->start_time) : std::string();
  if (ShowsQueuedGiftLine(queued, !date.empty())) {
    return Format(T_("gift_pro_redeemed_queued",
                     "Your gift year of Pro starts on {}, after your current Pro period."),
                  date);
  }
  return T_("gift_pro_redeemed", "A year of Pro is yours.");
}
}  // namespace

RedeemCodeSheet::RedeemCodeSheet(Gtk::Window& parent, SdkHost& host,
                                 SubscriptionBalanceStore& balance)
    : host_(host), balance_(balance) {
  EnsureDrawerCss();
  set_title(T_("redeem_code", "Redeem Code"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(420, -1);
  set_resizable(false);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  BuildUi();
}

void RedeemCodeSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->set_margin(24);
  set_child(*root);

  // ---- entry state -----------------------------------------------------------
  entryBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);

  auto* codeCaption = MakeCaption(T_("balance_code", "Balance code"));
  entryBox_->append(*codeCaption);

  codeEntry_ = Gtk::make_managed<Gtk::Entry>();
  codeEntry_->set_placeholder_text(T_("enter_balance_code", "Enter balance code"));
  // room for a code as a gift card prints it, in groups of four, which
  // urnet::isBalanceCodeFormatValid reads as the server does (no separators,
  // any case): capped at the code's own length, a grouped code was cut short
  codeEntry_->set_max_length(static_cast<int>(2 * urnet::BalanceCodeLength));
  codeEntry_->add_css_class("ur-mono-13");
  codeEntry_->signal_changed().connect([this] {
    errorLabel_->set_visible(false);
    codeEntry_->remove_css_class("error");
    redeemBtn_->set_sensitive(!redeeming_ && urnet::isBalanceCodeFormatValid(codeEntry_->get_text()));
  });
  codeEntry_->signal_activate().connect([this] {
    if (redeemBtn_->get_sensitive()) Redeem();
  });
  entryBox_->append(*codeEntry_);

  errorLabel_ = Gtk::make_managed<Gtk::Label>(T_("invalid_balance_code", "Invalid balance code"));
  errorLabel_->add_css_class("ur-error-text");
  errorLabel_->add_css_class("caption");
  errorLabel_->set_xalign(0);
  errorLabel_->set_visible(false);
  entryBox_->append(*errorLabel_);

  // Someone opening this sheet with no code needs to know where to get one —
  // plain text, no link (mac keeps it that way for store-policy reasons; the
  // same copy reads fine here).
  auto* hint = Gtk::make_managed<Gtk::Label>(
      T_("balance_code_where_to_buy",
         "Don't have a code? Data codes can be purchased at ur.io and emailed to you."));
  hint->add_css_class("dim-label");
  hint->add_css_class("caption");
  hint->set_wrap(true);
  hint->set_xalign(0);
  hint->set_margin_top(4);
  entryBox_->append(*hint);

  auto* actionRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actionRow->set_margin_top(16);
  actionRow->set_halign(Gtk::Align::END);
  spinner_ = Gtk::make_managed<Gtk::Spinner>();
  spinner_->set_visible(false);
  actionRow->append(*spinner_);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { set_visible(false); });
  actionRow->append(*cancel);
  redeemBtn_ = Gtk::make_managed<Gtk::Button>(T_("redeem", "Redeem"));
  redeemBtn_->add_css_class("suggested-action");
  redeemBtn_->set_sensitive(false);
  redeemBtn_->signal_clicked().connect([this] { Redeem(); });
  actionRow->append(*redeemBtn_);
  entryBox_->append(*actionRow);

  root->append(*entryBox_);

  // ---- success state ---------------------------------------------------------
  successBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  successBox_->set_visible(false);
  auto* check = Gtk::make_managed<Gtk::Image>();
  check->set_from_icon_name("emblem-ok-symbolic");
  check->set_pixel_size(40);
  check->add_css_class("ur-value-on");
  successBox_->append(*check);
  auto* redeemed =
      Gtk::make_managed<Gtk::Label>(T_("balance_code_redeemed", "Balance code redeemed."));
  redeemed->add_css_class("title-3");
  redeemed->set_wrap(true);
  redeemed->set_justify(Gtk::Justification::CENTER);
  successBox_->append(*redeemed);
  // what the code added (Windows RedeemCodeSheet parity): its data, or a gift
  // code's year of Pro. Both are on the network when the redeem answers, so
  // nothing here waits on a plan.
  successAmount_ = Gtk::make_managed<Gtk::Label>("");
  successAmount_->add_css_class("title-2");
  successAmount_->set_justify(Gtk::Justification::CENTER);
  successBox_->append(*successAmount_);
  auto* closeBtn = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  closeBtn->add_css_class("suggested-action");
  closeBtn->set_halign(Gtk::Align::CENTER);
  closeBtn->set_margin_top(8);
  closeBtn->signal_clicked().connect([this] { set_visible(false); });
  successBox_->append(*closeBtn);
  root->append(*successBox_);

  // ---- redeemed codes (mac Balance Codes screen / Windows Account panel) ----
  // The history stays below both states; RefreshCodes fills it on open and
  // again after a successful redeem.
  auto* separator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  separator->set_margin_top(16);
  separator->set_margin_bottom(12);
  root->append(*separator);
  root->append(*MakeCaption(T_("balance_codes_title", "Balance Codes")));

  codesGrid_ = Gtk::make_managed<Gtk::Grid>();
  codesGrid_->set_column_spacing(16);
  codesGrid_->set_row_spacing(4);
  // small histories take their natural height; long ones cap and scroll (the
  // sheet is fixed-size, so the list must not outgrow the screen)
  codesScroller_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  codesScroller_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  codesScroller_->set_propagate_natural_height(true);
  codesScroller_->set_max_content_height(240);
  codesScroller_->set_margin_top(8);
  codesScroller_->set_child(*codesGrid_);
  codesScroller_->set_visible(false);  // the first fetch decides list vs empty
  root->append(*codesScroller_);

  codesEmpty_ =
      Gtk::make_managed<Gtk::Label>(T_("no_balance_codes_found", "No balance codes found"));
  codesEmpty_->add_css_class("dim-label");
  codesEmpty_->add_css_class("caption");
  codesEmpty_->set_xalign(0);
  codesEmpty_->set_margin_top(8);
  codesEmpty_->set_visible(false);
  root->append(*codesEmpty_);

  // a fetch FAILURE is not an empty history: it gets its own state so the
  // user is never told "No balance codes found" over a transport error
  codesError_ = Gtk::make_managed<Gtk::Label>(
      T_("balance_codes_load_error",
         "Couldn't load your balance codes. Check your connection and try again."));
  codesError_->add_css_class("ur-error-text");
  codesError_->add_css_class("caption");
  codesError_->set_wrap(true);
  codesError_->set_xalign(0);
  codesError_->set_margin_top(8);
  codesError_->set_visible(false);
  root->append(*codesError_);
}

// The redeemed-codes history: one row per code — masked secret, +data,
// redeemed date, expiry — the Windows Account panel columns and the mac
// Balance Codes screen rows. A failed fetch renders the error state; only a
// SUCCESSFUL fetch with no codes renders the empty state.
void RedeemCodeSheet::RefreshCodes() {
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().getNetworkRedeemedBalanceCodes(
      [this, epoch, issued](std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> result,
                            std::optional<std::string> err) {
        const bool failed = err.has_value() || !result || result->error.has_value();
        urnet::RedeemedBalanceCodeList codes;
        if (!failed && result->balance_codes) codes = *result->balance_codes;
        PostToMain([this, epoch, issued, failed, codes = std::move(codes)] {
          if (*epoch != issued) return;  // sheet was reset since
          while (Gtk::Widget* child = codesGrid_->get_first_child()) {
            codesGrid_->remove(*child);
          }
          codesScroller_->set_visible(!codes.empty());
          codesEmpty_->set_visible(!failed && codes.empty());
          codesError_->set_visible(failed);
          if (codes.empty()) return;

          auto cell = [](const std::string& text, bool header) {
            auto* label = Gtk::make_managed<Gtk::Label>(text);
            label->add_css_class("caption");
            if (header) label->add_css_class("dim-label");
            label->set_xalign(0);
            return label;
          };
          codesGrid_->attach(*cell(T_("code", "Code"), true), 0, 0);
          codesGrid_->attach(*cell(T_("data", "Data"), true), 1, 0);
          codesGrid_->attach(*cell(T_("redeemed", "Redeemed"), true), 2, 0);
          codesGrid_->attach(*cell(T_("expires", "Expires"), true), 3, 0);
          int row = 1;
          for (const auto& code : codes) {
            auto* secret = cell(MaskSecret(code.secret), false);
            secret->add_css_class("ur-mono-12");
            secret->set_hexpand(true);
            codesGrid_->attach(*secret, 0, row);
            // a gift code's year of Pro adds no data: never "+0 B"
            const std::string amount =
                IsProGiftCode(code.kind)
                    ? std::string(T_("balance_code_pro_year", "A year of Pro"))
                    : "+" + FormatByteCountCompact(code.balance_byte_count);
            codesGrid_->attach(*cell(amount, false), 1, row);
            codesGrid_->attach(
                *cell(code.redeem_time ? IsoDate(*code.redeem_time) : std::string(), false), 2,
                row);
            codesGrid_->attach(
                *cell(code.end_time ? IsoDate(*code.end_time) : std::string(), false), 3, row);
            ++row;
          }
        });
      });
}

void RedeemCodeSheet::Open() {
  ++*epoch_;
  SetRedeeming(false);
  codeEntry_->set_text("");
  codeEntry_->remove_css_class("error");
  errorLabel_->set_visible(false);
  entryBox_->set_visible(true);
  successBox_->set_visible(false);
  RefreshCodes();
  present();
  codeEntry_->grab_focus();
}

void RedeemCodeSheet::SetRedeeming(bool redeeming) {
  redeeming_ = redeeming;
  spinner_->set_visible(redeeming);
  if (redeeming) {
    spinner_->start();
  } else {
    spinner_->stop();
  }
  codeEntry_->set_sensitive(!redeeming);
  redeemBtn_->set_sensitive(!redeeming && urnet::isBalanceCodeFormatValid(codeEntry_->get_text()));
}

void RedeemCodeSheet::Redeem() {
  if (redeeming_) return;
  const std::string code = TrimWhitespace(codeEntry_->get_text());
  // the SDK's cheap pre-submit gate (the exact code length)
  if (!urnet::isBalanceCodeFormatValid(code)) return;
  SetRedeeming(true);
  errorLabel_->set_visible(false);

  urnet::RedeemBalanceCodeArgs args;
  args.secret = code;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().redeemBalanceCode(
      args, [this, epoch, issued, code](std::optional<urnet::RedeemBalanceCodeResult> result,
                                        std::optional<std::string> err) {
        // a transport failure has no result to classify: the outcome is unknown
        if (err) result.reset();
        PostToMain([this, epoch, issued, code, result = std::move(result)] {
          if (*epoch != issued) return;  // sheet was reset since
          const std::string outcome = urnet::classifyBalanceCodeRedeem(result, std::nullopt, code);
          if (!BalanceCodeRedeemNeedsCodeList(outcome)) {
            ShowRedeemOutcome(BalanceCodeRedeemNoticeFor(outcome), result);
            return;
          }
          // Not credited by this call. The server's refusal is the same for an
          // unknown data code and one this network already redeemed, and a
          // lost response may have committed: ask the network's redeemed-code
          // list before saying anything. A gift code found there confirms as
          // the gift.
          host_.api().getNetworkRedeemedBalanceCodes(
              [this, epoch, issued, code, result](
                  std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> list,
                  std::optional<std::string> listErr) {
                std::optional<urnet::RedeemedBalanceCodeList> codes;
                if (!listErr && list && !list->error) {
                  codes = list->balance_codes.value_or(urnet::RedeemedBalanceCodeList{});
                }
                PostToMain([this, epoch, issued, code, result, codes = std::move(codes)] {
                  if (*epoch != issued) return;  // sheet was reset since
                  // the code in the list, read as the server reads a typed
                  // code, and a refusal saying the code is redeemed (by any
                  // network)
                  const std::optional<std::string> listedKind =
                      codes ? RedeemedCodeKind(code, *codes) : std::nullopt;
                  const bool refusalSaysRedeemed =
                      result && result->error && RefusalSaysAlreadyRedeemed(result->error->message);
                  ShowRedeemOutcome(
                      BalanceCodeRedeemNoticeAfterList(urnet::classifyBalanceCodeRedeem(result, codes, code),
                                                       listedKind, refusalSaysRedeemed),
                      result);
                });
              });
        });
      });
}

void RedeemCodeSheet::ShowRedeemOutcome(BalanceCodeRedeemNotice notice,
                                        const std::optional<urnet::RedeemBalanceCodeResult>& result) {
  SetRedeeming(false);
  switch (notice) {
    // a gift code this network already holds (its answer was lost or refused
    // after it committed) confirms as the gift, as a credited one does
    case BalanceCodeRedeemNotice::GiftRedeemed:
    case BalanceCodeRedeemNotice::Redeemed:
      // success: confirm what the code added and read the balance once, so a
      // data code's data lands in the usage bar and a gift code's year of Pro
      // in the plan (a free plan flips to Pro on that read); the fresh code
      // joins the history list right away. The redeem is committed when the
      // server answers, so never the Pro confirmation poll: there is no
      // payment to wait for, and it spins the plan ring for 2 minutes.
      if (notice == BalanceCodeRedeemNotice::GiftRedeemed || (result && result->pro)) {
        // a gift code: its year of Pro as a sentence under the title, never
        // the 0-byte transfer balance that comes with it. A gift found in the
        // redeemed list has no answer to read a queued year from.
        successAmount_->set_text(
            GiftRedeemedText(notice == BalanceCodeRedeemNotice::Redeemed ? result : std::nullopt));
        successAmount_->remove_css_class("title-2");
        successAmount_->add_css_class("title-4");
        successAmount_->set_wrap(true);
        successAmount_->set_visible(true);
      } else {
        // a figure (the sheet is reused: undo a gift's sentence style)
        successAmount_->remove_css_class("title-4");
        successAmount_->add_css_class("title-2");
        successAmount_->set_wrap(false);
        successAmount_->set_text(
            "+" + FormatByteCountCompact(result && result->transfer_balance
                                             ? result->transfer_balance->balance_byte_count
                                             : 0));
        successAmount_->set_visible(result && result->transfer_balance &&
                                    0 < result->transfer_balance->balance_byte_count);
      }
      entryBox_->set_visible(false);
      successBox_->set_visible(true);
      balance_.FetchNow();
      RefreshCodes();
      if (on_redeemed) on_redeemed();
      return;
    case BalanceCodeRedeemNotice::AlreadyRedeemed:
      // this network has the data code: the data is on the balance (a retry
      // after a lost-but-credited response lands here); or the server says the
      // code is redeemed and it is not this network's. Not an error on the
      // entry.
      errorLabel_->set_text(
          T_("balance_code_already_redeemed_message",
             "This balance code has already been redeemed. If you redeemed it earlier, the data "
             "is already on your balance."));
      errorLabel_->set_visible(true);
      balance_.FetchNow();
      RefreshCodes();
      if (on_redeemed) on_redeemed();
      return;
    case BalanceCodeRedeemNotice::Invalid:
      // the server rejected the code: surface its reason when it gives one,
      // falling back to the generic invalid-code copy
      errorLabel_->set_text(result && result->error && !result->error->message.empty()
                                ? Glib::ustring(result->error->message)
                                : T_("invalid_balance_code", "Invalid balance code"));
      errorLabel_->set_visible(true);
      codeEntry_->add_css_class("error");
      return;
    case BalanceCodeRedeemNotice::Unknown:
      // No answer (or an empty one): the server may have COMMITTED the
      // redemption before the response was lost — never call the code
      // invalid (and don't mark the entry as wrong), point at the balance.
      errorLabel_->set_text(
          T_("balance_code_transport_error",
             "We couldn't reach the server — check your connection. If you were charged, "
             "the code may already be applied; check your balance before trying again."));
      errorLabel_->set_visible(true);
      return;
  }
}

}  // namespace urnw
