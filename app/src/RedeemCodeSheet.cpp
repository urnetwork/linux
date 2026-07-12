// SPDX-License-Identifier: MPL-2.0
#include "RedeemCodeSheet.hpp"

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {
// balance codes are always 26 characters (mac gates the Redeem button on this)
constexpr int kBalanceCodeLength = 26;

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
  codeEntry_->set_max_length(kBalanceCodeLength);
  codeEntry_->add_css_class("ur-mono-13");
  codeEntry_->signal_changed().connect([this] {
    errorLabel_->set_visible(false);
    codeEntry_->remove_css_class("error");
    redeemBtn_->set_sensitive(!redeeming_ &&
                              static_cast<int>(TrimWhitespace(codeEntry_->get_text()).size()) ==
                                  kBalanceCodeLength);
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
  auto* processing = Gtk::make_managed<Gtk::Label>(
      T_("processing_subscription_balance", "Processing subscription balance..."));
  processing->add_css_class("dim-label");
  processing->add_css_class("caption");
  processing->set_justify(Gtk::Justification::CENTER);
  successBox_->append(*processing);
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
}

// The redeemed-codes history: one row per code — masked secret, +data,
// redeemed date, expiry — the Windows Account panel columns and the mac
// Balance Codes screen rows. Errors leave the list empty (the empty state).
void RedeemCodeSheet::RefreshCodes() {
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().getNetworkRedeemedBalanceCodes(
      [this, epoch, issued](std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> result,
                            std::optional<std::string> err) {
        urnet::RedeemedBalanceCodeList codes;
        if (!err && result && result->balance_codes) codes = *result->balance_codes;
        PostToMain([this, epoch, issued, codes = std::move(codes)] {
          if (*epoch != issued) return;  // sheet was reset since
          while (Gtk::Widget* child = codesGrid_->get_first_child()) {
            codesGrid_->remove(*child);
          }
          codesScroller_->set_visible(!codes.empty());
          codesEmpty_->set_visible(codes.empty());
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
            codesGrid_->attach(
                *cell("+" + FormatByteCountCompact(code.balance_byte_count), false), 1, row);
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
  redeemBtn_->set_sensitive(!redeeming &&
                            static_cast<int>(TrimWhitespace(codeEntry_->get_text()).size()) ==
                                kBalanceCodeLength);
}

void RedeemCodeSheet::Redeem() {
  if (redeeming_) return;
  const std::string code = TrimWhitespace(codeEntry_->get_text());
  if (static_cast<int>(code.size()) != kBalanceCodeLength) return;
  SetRedeeming(true);
  errorLabel_->set_visible(false);

  urnet::RedeemBalanceCodeArgs args;
  args.secret = code;
  auto epoch = epoch_;
  const uint64_t issued = *epoch;
  host_.api().redeemBalanceCode(
      args, [this, epoch, issued](std::optional<urnet::RedeemBalanceCodeResult> result,
                                  std::optional<std::string> err) {
        PostToMain([this, epoch, issued, result = std::move(result), err = std::move(err)] {
          if (*epoch != issued) return;  // sheet was reset since
          SetRedeeming(false);
          if (err || !result || result->error) {
            errorLabel_->set_visible(true);
            codeEntry_->add_css_class("error");
            return;
          }
          // success: show the confirmation and re-poll the balance so the new
          // transfer balance lands in the usage bar (mac startPolling()); the
          // fresh code joins the history list right away (mac onSuccess)
          entryBox_->set_visible(false);
          successBox_->set_visible(true);
          balance_.StartConfirmationPolling();
          RefreshCodes();
        });
      });
}

}  // namespace urnw
