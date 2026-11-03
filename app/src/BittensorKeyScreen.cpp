// SPDX-License-Identifier: MPL-2.0
#include "BittensorKeyScreen.hpp"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include <urnetwork_sdk.hpp>

#include "BittensorKeyAuth.hpp"
#include "BittensorKeyText.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr int kScreenWidth = 480;
constexpr int kScreenMaxHeight = 640;
constexpr int kWordColumns = 3;
constexpr int kCopiedResetMs = 2000;

Gtk::Label* MakeLine(const Glib::ustring& text, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  return label;
}

// A notice in a tinted box: an icon (or a spinner), then its content.
struct Notice {
  Gtk::Box* root = nullptr;
  Gtk::Box* content = nullptr;
};
Notice MakeNotice(const char* iconName, const char* tint) {
  Notice notice;
  notice.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  notice.root->add_css_class("ur-key-notice");
  notice.root->add_css_class(std::string("ur-key-notice-") + tint);
  if (iconName == nullptr) {
    auto* spinner = Gtk::make_managed<Gtk::Spinner>();
    spinner->set_spinning(true);
    spinner->set_valign(Gtk::Align::START);
    notice.root->append(*spinner);
  } else {
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name(iconName);
    icon->set_valign(Gtk::Align::START);
    icon->add_css_class(std::string("ur-key-icon-") + tint);
    kit::MarkDecorative(*icon);
    notice.root->append(*icon);
  }
  notice.content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  notice.content->set_hexpand(true);
  notice.root->append(*notice.content);
  return notice;
}

Gtk::Button* MakeWideButton(const Glib::ustring& text, const char* cssClass) {
  auto* button = Gtk::make_managed<Gtk::Button>();
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->set_wrap(true);
  label->set_justify(Gtk::Justification::CENTER);
  button->set_child(*label);
  button->add_css_class(cssClass);
  button->set_hexpand(true);
  kit::SetAccessibleLabel(*button, text);
  return button;
}

Gtk::Button* MakeQuietButton(const Glib::ustring& text) {
  auto* button = Gtk::make_managed<Gtk::Button>(text);
  button->add_css_class("flat");
  button->set_halign(Gtk::Align::CENTER);
  return button;
}

// The two not-a-wallet texts, always in view on the Key screen and in what
// deleting means.
void AppendWording(Gtk::Box& column) {
  column.append(*MakeLine(
      T_("bittensor_key_wording_not_wallet",
         "URnetwork is not a wallet. It created this key so you can receive SN25α. To transfer or "
         "use your SN25α, enter this recovery phrase into a Bittensor wallet such as Talisman or "
         "Nova, or into btcli."),
      "ur-body"));
}

void AppendControlWarning(Gtk::Box& column) {
  Notice warning = MakeNotice("dialog-warning-symbolic", "warn");
  warning.content->append(*MakeLine(
      T_("bittensor_key_wording_control",
         "Anyone with this phrase controls your SN25α. URnetwork will never ask for it, and can't "
         "recover it. If you lose this device before saving it, your SN25α can't be reached."),
      "ur-body"));
  column.append(*warning.root);
}

// "0:58": minutes and seconds until `untilMillis`, never below 0:00
std::string Remaining(int64_t untilMillis, int64_t nowMillis) {
  const int64_t seconds = std::max<int64_t>(0, (untilMillis - nowMillis + 999) / 1000);
  char text[32];
  std::snprintf(text, sizeof(text), "%lld:%02lld", static_cast<long long>(seconds / 60),
                static_cast<long long>(seconds % 60));
  return text;
}

}  // namespace

BittensorKeyScreen::BittensorKeyScreen(Gtk::Window& parent, BittensorKeyHost& host)
    : host_(host) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("bittensor_key_screen_title", "Bittensor recovery phrase"));
  set_default_size(kScreenWidth, -1);
  set_hide_on_close(true);
  add_css_class("ur-sheet");

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  column->set_margin(24);
  column->set_size_request(kScreenWidth, -1);
  BuildMain(*column);
  BuildExplain(*column);
  BuildConfirm(*column);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(kScreenMaxHeight);
  scroller->set_child(*column);
  set_child(*scroller);

  // the window's close, the window manager's included, goes through the flow
  signal_close_request().connect([this]() -> bool { return OnCloseRequest(); }, false);
  auto escape = Gtk::EventControllerKey::create();
  escape->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval != GDK_KEY_Escape) return false;
        close();
        return true;
      },
      false);
  add_controller(escape);
  // no general capture API here: the words hide whenever this window isn't
  // the focused one
  property_is_active().signal_changed().connect([this] {
    if (!is_active()) {
      host_.Hide();
      return;
    }
    if (revealWhenFocused_) {
      const bool authenticated = *revealWhenFocused_;
      revealWhenFocused_.reset();
      if (authenticated) host_.Reveal(true);
    }
  });
  Apply();
}

BittensorKeyScreen::~BittensorKeyScreen() {
  *alive_ = false;
  countdown_.disconnect();
  copiedReset_.disconnect();
}

// ---- the screen ------------------------------------------------------------------

void BittensorKeyScreen::BuildMain(Gtk::Box& column) {
  mainPage_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  auto& page = *mainPage_;

  auto* title = MakeLine(T_("bittensor_key_screen_title", "Bittensor recovery phrase"),
                         "ur-step-heading");
  page.append(*title);

  // signing, a refused consent, or the Save now notice
  {
    Notice busy = MakeNotice(nullptr, "info");
    busy.content->append(*MakeLine(
        T_("bittensor_key_signing",
           "Setting up payouts: signing your network's payout consent with this key."),
        "ur-body"));
    busyBanner_ = busy.root;
    page.append(*busyBanner_);
  }
  {
    Notice retry = MakeNotice("dialog-warning-symbolic", "danger");
    retryText_ = MakeLine({}, "ur-body");
    retry.content->append(*retryText_);
    retryButton_ = MakeWideButton(T_("retry", "Retry"), "ur-pane-primary");
    retryButton_->signal_clicked().connect([this] { host_.RetrySaveNow(); });
    retry.content->append(*retryButton_);
    retry.content->append(*MakeLine(
        T_("bittensor_key_save_now_retry",
           "Payouts aren't set up yet. No earnings went to this key, and leaving discards it."),
        "ur-caption"));
    retryBanner_ = retry.root;
    page.append(*retryBanner_);
  }
  {
    Notice saveNow = MakeNotice("dialog-information-symbolic", "info");
    saveNow.content->append(*MakeLine(
        T_("bittensor_key_save_now_banner",
           "This key isn't saved on this device. Write the 12 words down, then choose below. If "
           "you leave this screen, the key is discarded and nothing is paid to it."),
        "ur-body"));
    saveNowBanner_ = saveNow.root;
    page.append(*saveNowBanner_);
  }
  failureLine_ = MakeLine({}, "ur-body");
  failureLine_->add_css_class("ur-danger-text");
  page.append(*failureLine_);

  AppendWording(page);

  // the words: hidden behind the cover until revealed
  auto* words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  wordsLabel_ = MakeLine({}, "ur-caption");
  words->append(*wordsLabel_);
  cover_ = Gtk::make_managed<Gtk::Button>();
  cover_->add_css_class("ur-key-cover");
  {
    auto* inside = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    inside->set_valign(Gtk::Align::CENTER);
    coverIcon_ = Gtk::make_managed<Gtk::Image>();
    coverIcon_->set_pixel_size(22);
    coverIcon_->add_css_class("ur-key-muted-icon");
    kit::MarkDecorative(*coverIcon_);
    inside->append(*coverIcon_);
    auto* reveal = Gtk::make_managed<Gtk::Label>(T_("bittensor_key_reveal", "Tap to reveal"));
    reveal->add_css_class("ur-body");
    inside->append(*reveal);
    cover_->set_child(*inside);
  }
  kit::SetAccessibleLabel(*cover_, T_("bittensor_key_reveal", "Tap to reveal"));
  cover_->signal_clicked().connect([this] { OnReveal(); });
  words->append(*cover_);

  revealed_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  wordsGrid_ = Gtk::make_managed<Gtk::Grid>();
  wordsGrid_->add_css_class("ur-key-words");
  wordsGrid_->set_row_spacing(8);
  wordsGrid_->set_column_spacing(8);
  wordsGrid_->set_column_homogeneous(true);
  revealed_->append(*wordsGrid_);
  countdownLabel_ = MakeLine({}, "ur-caption");
  revealed_->append(*countdownLabel_);
  {
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_homogeneous(true);
    auto* hide = Gtk::make_managed<Gtk::Button>(T_("bittensor_key_hide", "Hide"));
    hide->add_css_class("ur-key-outline");
    hide->signal_clicked().connect([this] { host_.Hide(); });
    actions->append(*hide);
    copyButton_ = Gtk::make_managed<Gtk::Button>(T_("copy", "Copy"));
    copyButton_->add_css_class("ur-key-outline");
    copyButton_->signal_clicked().connect([this] { OnCopy(); });
    actions->append(*copyButton_);
    revealed_->append(*actions);
  }
  revealed_->append(*MakeLine(T_("bittensor_key_copy_warning_2",
                                 "Other apps may be able to read the clipboard. The copy stays on "
                                 "this device and is cleared after 60 s."),
                              "ur-caption"));
  words->append(*revealed_);
  page.append(*words);

  AppendControlWarning(page);
  page.append(*MakeLine(
      T_("bittensor_key_not_seed_2",
         "Not the seedphrase from sign-up: that one is 24 words and signs you in to URnetwork. "
         "This Bittensor recovery phrase is 12 words and holds your SN25α."),
      "ur-caption"));

  // Keep beside the one destructive action
  savedGroup_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  savedGroup_->append(
      *MakeLine(T_("bittensor_key_saved_question", "Saved the recovery phrase?"), "ur-caption"));
  keepButton_ = MakeWideButton(
      T_("bittensor_key_keep_action", "I've saved it. Keep it on this device."), "ur-key-light");
  keepButton_->signal_clicked().connect([this] { host_.ConfirmKeep(); });
  savedGroup_->append(*keepButton_);
  deleteButton_ =
      MakeWideButton(T_("bittensor_key_delete_action",
                        "I have saved the key, it's OK to delete it now."),
                     "ur-key-destructive-outline");
  deleteButton_->signal_clicked().connect([this] {
    // the words hide before anything is asked, so none can be read behind it
    host_.Hide();
    deleteExplained_ = true;
    Apply();
  });
  savedGroup_->append(*deleteButton_);
  page.append(*savedGroup_);

  mainDisclaimer_ = MakeBittensorFundsDisclaimer(/*boxed=*/true);
  page.append(*mainDisclaimer_);

  closeButton_ = MakeQuietButton(T_("close", "Close"));
  closeButton_->signal_clicked().connect([this] { close(); });
  page.append(*closeButton_);

  column.append(page);
}

void BittensorKeyScreen::BuildExplain(Gtk::Box& column) {
  explainPage_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  auto& page = *explainPage_;
  explainTitle_ = MakeLine({}, "ur-step-heading");
  page.append(*explainTitle_);
  bullets_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  page.append(*bullets_);
  {
    Notice top200 = MakeNotice("starred-symbolic", "gold");
    top200.content->append(*MakeLine(
        T_("bittensor_key_confirm_top200",
           "Deleting this key also gives up claiming a Top 200 spot with one tap. You could still "
           "claim from a Bittensor wallet holding this recovery phrase."),
        "ur-body"));
    top200Box_ = top200.root;
    page.append(*top200Box_);
  }
  AppendWording(page);
  page.append(*MakeLine(
      T_("bittensor_key_wording_control",
         "Anyone with this phrase controls your SN25α. URnetwork will never ask for it, and can't "
         "recover it. If you lose this device before saving it, your SN25α can't be reached."),
      "ur-body"));
  explainDisclaimer_ = MakeBittensorFundsDisclaimer(/*boxed=*/true);
  page.append(*explainDisclaimer_);
  auto* proceed = MakeWideButton(T_("continue_txt", "Continue"), "ur-key-destructive");
  proceed->signal_clicked().connect([this, proceed] {
    proceed->set_sensitive(false);
    std::weak_ptr<bool> alive = alive_;
    // two of the words are asked next; a failure to read the key returns to
    // the screen, which says why
    host_.ConfirmDelete([this, alive, proceed] {
      const std::shared_ptr<bool> live = alive.lock();
      if (!live || !*live) return;
      proceed->set_sensitive(true);
      deleteExplained_ = false;
      Apply();
    });
  });
  page.append(*proceed);
  auto* cancel = MakeQuietButton(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] {
    deleteExplained_ = false;
    Apply();
  });
  page.append(*cancel);
  column.append(page);
}

void BittensorKeyScreen::BuildConfirm(Gtk::Box& column) {
  confirmPage_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  auto& page = *confirmPage_;
  confirmTitle_ = MakeLine({}, "ur-step-heading");
  page.append(*confirmTitle_);
  confirmBody_ = MakeLine({}, "ur-body");
  page.append(*confirmBody_);
  confirmPrompt_ = MakeLine({}, "ur-body");
  page.append(*confirmPrompt_);
  for (size_t i = 0; i < wordFields_.size(); ++i) {
    auto* field = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    wordTitles_[i] = MakeLine({}, "ur-caption");
    field->append(*wordTitles_[i]);
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->add_css_class("ur-input");
    entry->add_css_class("ur-key-word-field");
    // a field that learns nothing and offers nothing: no spell check, no
    // emoji, no completion, and the input method asked to keep no history
    entry->set_input_purpose(Gtk::InputPurpose::FREE_FORM);
    entry->set_input_hints(Gtk::InputHints::NO_SPELLCHECK | Gtk::InputHints::NO_EMOJI |
                           Gtk::InputHints::PRIVATE | Gtk::InputHints::LOWERCASE);
    entry->property_enable_emoji_completion() = false;
    entry->property_show_emoji_icon() = false;
    entry->signal_changed().connect([this] { ApplyWordCheck(); });
    entry->signal_activate().connect([this] { OnSubmit(); });
    wordFields_[i] = entry;
    field->append(*entry);
    wordWrong_[i] = MakeLine({}, "ur-caption");
    wordWrong_[i]->add_css_class("ur-danger-text");
    field->append(*wordWrong_[i]);
    page.append(*field);
  }
  rejectedLine_ =
      MakeLine(T_("bittensor_key_words_rejected",
                  "Those words don't match. Check your saved recovery phrase."),
               "ur-body");
  rejectedLine_->add_css_class("ur-danger-text");
  page.append(*rejectedLine_);
  confirmFailure_ = MakeLine({}, "ur-body");
  confirmFailure_->add_css_class("ur-danger-text");
  page.append(*confirmFailure_);
  confirmDisclaimer_ = MakeBittensorFundsDisclaimer(/*boxed=*/true);
  page.append(*confirmDisclaimer_);

  confirmButton_ = Gtk::make_managed<Gtk::Button>();
  confirmButton_->set_hexpand(true);
  {
    auto* inside = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    inside->set_halign(Gtk::Align::CENTER);
    confirmButtonIcon_ = Gtk::make_managed<Gtk::Image>();
    confirmButtonIcon_->set_from_icon_name("dialog-password-symbolic");
    kit::MarkDecorative(*confirmButtonIcon_);
    inside->append(*confirmButtonIcon_);
    confirmButtonLabel_ = Gtk::make_managed<Gtk::Label>();
    confirmButtonLabel_->set_wrap(true);
    inside->append(*confirmButtonLabel_);
    confirmSpinner_ = Gtk::make_managed<Gtk::Spinner>();
    inside->append(*confirmSpinner_);
    confirmButton_->set_child(*inside);
  }
  confirmButton_->signal_clicked().connect([this] { OnSubmit(); });
  page.append(*confirmButton_);
  confirmCancel_ = MakeQuietButton(T_("cancel", "Cancel"));
  confirmCancel_->signal_clicked().connect([this] { host_.CancelConfirmation(); });
  page.append(*confirmCancel_);
  column.append(page);
}

void BittensorKeyScreen::Apply() {
  const urnet::BittensorLocalKeyScreen& screen = host_.Screen();
  // a new confirmation starts with empty fields, and is what shows
  if (screen.confirming != confirming_) {
    confirming_ = screen.confirming;
    for (Gtk::Entry* field : wordFields_) field->set_text("");
    if (!confirming_.empty()) {
      deleteExplained_ = false;
      wordFields_[0]->grab_focus();
    }
  }
  const bool confirming = !screen.confirming.empty();
  mainPage_->set_visible(!confirming && !deleteExplained_);
  explainPage_->set_visible(!confirming && deleteExplained_);
  confirmPage_->set_visible(confirming);
  if (confirming) {
    ApplyConfirm(screen);
  } else if (deleteExplained_) {
    ApplyExplain(screen);
  } else {
    ApplyMain(screen);
  }
}

void BittensorKeyScreen::ApplyMain(const urnet::BittensorLocalKeyScreen& screen) {
  const Glib::ustring failure = BittensorKeyFailureText(screen.failure_code, screen.failure_message);
  busyBanner_->set_visible(screen.busy);
  retryBanner_->set_visible(!screen.busy && screen.offers_retry);
  retryText_->set_text(failure);
  retryText_->set_visible(!failure.empty());
  retryButton_->set_sensitive(!screen.busy);
  saveNowBanner_->set_visible(!screen.busy && !screen.offers_retry && screen.save_now);
  kit::SetTextOrCollapse(*failureLine_,
                         screen.busy || screen.offers_retry ? Glib::ustring() : failure);

  kit::SetTextOrCollapse(
      *wordsLabel_, screen.address.empty()
                        ? Glib::ustring()
                        : Glib::ustring(Format(T_("bittensor_key_words_label", "12 words · address {}"),
                                               urnet::shortSs58(screen.address))));
  cover_->set_visible(!screen.revealed);
  cover_->set_sensitive(!screen.hidden_by_capture && !authenticating_);
  // the lock says Reveal asks for the user's password first
  coverIcon_->set_from_icon_name(screen.needs_authentication ? "dialog-password-symbolic"
                                                             : "view-conceal-symbolic");
  revealed_->set_visible(screen.revealed);
  // the words are in the tree only while they show
  while (Gtk::Widget* child = wordsGrid_->get_first_child()) wordsGrid_->remove(*child);
  const std::vector<std::string>& words = host_.Words();
  if (screen.revealed) {
    for (size_t i = 0; i < words.size(); ++i) {
      auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
      cell->add_css_class("ur-key-word-cell");
      auto* number = Gtk::make_managed<Gtk::Label>(std::to_string(i + 1) + ".");
      number->add_css_class("ur-key-word-number");
      number->set_width_chars(3);
      number->set_xalign(1);
      kit::MarkDecorative(*number);
      cell->append(*number);
      auto* word = Gtk::make_managed<Gtk::Label>(words[i]);
      word->add_css_class("ur-key-word");
      word->set_xalign(0);
      word->set_selectable(false);
      cell->append(*word);
      wordsGrid_->attach(*cell, static_cast<int>(i % kWordColumns),
                         static_cast<int>(i / kWordColumns));
    }
    if (!countdown_.connected()) {
      std::weak_ptr<bool> alive = alive_;
      countdown_ = Glib::signal_timeout().connect_seconds(
          [this, alive]() -> bool {
            const std::shared_ptr<bool> live = alive.lock();
            if (!live || !*live) return false;
            ApplyCountdown();
            return host_.Screen().revealed;
          },
          1);
    }
    ApplyCountdown();
  } else {
    countdown_.disconnect();
  }

  savedGroup_->set_visible(!screen.offers_retry);
  keepButton_->set_visible(screen.offers_keep);
  keepButton_->set_sensitive(!screen.busy);
  deleteButton_->set_sensitive(!screen.busy);
  SetBittensorFundsDisclaimer(*mainDisclaimer_, screen.funds_disclaimer);
  closeButton_->set_label(screen.save_now ? T_("cancel", "Cancel") : T_("close", "Close"));
  closeButton_->set_sensitive(!screen.busy);
}

void BittensorKeyScreen::ApplyCountdown() {
  const urnet::BittensorLocalKeyScreen& screen = host_.Screen();
  if (!screen.revealed || screen.revealed_until_millis <= 0) {
    kit::SetTextOrCollapse(*countdownLabel_, {});
    return;
  }
  kit::SetTextOrCollapse(
      *countdownLabel_,
      Format(T_("bittensor_key_hides_in", "Hides again in {}"),
             Remaining(screen.revealed_until_millis, g_get_real_time() / 1000)));
}

void BittensorKeyScreen::ApplyExplain(const urnet::BittensorLocalKeyScreen& screen) {
  explainTitle_->set_text(screen.save_now
                              ? T_("bittensor_key_delete_save_now_title",
                                   "Set up payouts and delete the key?")
                              : T_("bittensor_key_delete_title_2", "Delete the key from this device?"));
  RemoveAllChildren(*bullets_);
  const std::string address = urnet::shortSs58(screen.address);
  std::vector<Glib::ustring> lines;
  if (screen.save_now) {
    lines = {
        Format(T_("bittensor_key_save_now_bullet_address",
                  "Payouts go to this address, {}, from the next epoch."),
               address),
        T_("bittensor_key_delete_bullet_no_recovery", "URnetwork can't get this key back."),
        T_("bittensor_key_save_now_bullet_phrase", "Only the recovery phrase can move your SN25α."),
        T_("bittensor_key_save_now_bullet_convert",
           "Convert to TAO needs this key, so it won't be available in this app."),
    };
  } else {
    lines = {
        T_("bittensor_key_delete_bullet_no_recovery", "URnetwork can't get this key back."),
        Format(T_("bittensor_key_delete_bullet_address", "Earnings keep going to this address, {}."),
               address),
        T_("bittensor_key_delete_bullet_phrase", "Only the recovery phrase can move them."),
        T_("bittensor_key_delete_bullet_convert", "Convert to TAO stops working in this app."),
    };
  }
  for (const Glib::ustring& line : lines) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* dot = Gtk::make_managed<Gtk::Label>("•");
    dot->set_valign(Gtk::Align::START);
    kit::MarkDecorative(*dot);
    row->append(*dot);
    auto* text = MakeLine(line, "ur-body");
    text->set_hexpand(true);
    row->append(*text);
    bullets_->append(*row);
  }
  top200Box_->set_visible(screen.warns_top200);
  SetBittensorFundsDisclaimer(*explainDisclaimer_, screen.funds_disclaimer);
}

void BittensorKeyScreen::ApplyConfirm(const urnet::BittensorLocalKeyScreen& screen) {
  const bool keeps = screen.confirming == urnet::BittensorLocalKeyConfirmKeep;
  if (keeps) {
    confirmTitle_->set_text(T_("bittensor_key_keep_title_2", "Keep the key on this device?"));
    confirmBody_->set_text(
        screen.save_now
            ? T_("bittensor_key_keep_save_now_body",
                 "You've saved the recovery phrase. The key is stored on this device for convert "
                 "and one-tap Top 200, and then payouts are set up.")
            : T_("bittensor_key_keep_body",
                 "You've saved the recovery phrase, so the backup reminder ends. The key stays on "
                 "this device for convert and one-tap Top 200."));
    kit::SetTextOrCollapse(*confirmPrompt_, T_("bittensor_key_keep_prompt",
                                               "To confirm, enter these two words from the phrase."));
  } else {
    confirmTitle_->set_text(T_("bittensor_key_enter_two_words", "Enter two of the words"));
    confirmBody_->set_text(T_("bittensor_key_enter_two_words_body",
                              "To confirm you've saved the recovery phrase, enter these two words "
                              "from it."));
    kit::SetTextOrCollapse(*confirmPrompt_, {});
  }
  const int64_t positions[2] = {screen.first_word_position, screen.second_word_position};
  for (size_t i = 0; i < wordFields_.size(); ++i) {
    const Glib::ustring title = Format(T_("bittensor_key_word_number", "Word {}"), positions[i]);
    wordTitles_[i]->set_text(title);
    wordFields_[i]->set_placeholder_text(title);
    kit::SetAccessibleLabel(*wordFields_[i], title);
    wordFields_[i]->set_sensitive(!screen.busy);
  }
  rejectedLine_->set_visible(screen.words_rejected);
  kit::SetTextOrCollapse(*confirmFailure_,
                         BittensorKeyFailureText(screen.failure_code, screen.failure_message));
  SetBittensorFundsDisclaimer(*confirmDisclaimer_, screen.funds_disclaimer);

  Glib::ustring label;
  if (keeps) {
    label = T_("bittensor_key_keep_confirm_2", "Keep it on this device");
  } else if (screen.save_now) {
    label = T_("bittensor_key_save_now_delete_confirm", "Set up payouts and delete key");
  } else {
    label = T_("bittensor_key_delete_confirm", "Delete key");
  }
  confirmButtonLabel_->set_text(label);
  kit::SetAccessibleLabel(*confirmButton_, label);
  confirmButton_->remove_css_class("ur-pane-primary");
  confirmButton_->remove_css_class("ur-key-destructive");
  confirmButton_->add_css_class(keeps ? "ur-key-primary" : "ur-key-destructive");
  // the lock says the button asks for the user's password first
  confirmButtonIcon_->set_visible(!keeps && screen.confirm_needs_authentication);
  const bool working = screen.busy || authenticating_;
  confirmSpinner_->set_visible(working);
  confirmSpinner_->set_spinning(working);
  confirmCancel_->set_sensitive(!screen.busy);
  ApplyWordCheck();
}

// Each keystroke: which typed word can't be right, and whether the button acts.
void BittensorKeyScreen::ApplyWordCheck() {
  const urnet::BittensorLocalKeyScreen& screen = host_.Screen();
  if (screen.confirming.empty()) return;
  const urnet::BittensorLocalKeyWordCheck check =
      host_.CheckWords(wordFields_[0]->get_text(), wordFields_[1]->get_text());
  const std::string states[2] = {check.first, check.second};
  const int64_t positions[2] = {screen.first_word_position, screen.second_word_position};
  for (size_t i = 0; i < wordWrong_.size(); ++i) {
    kit::SetTextOrCollapse(
        *wordWrong_[i],
        states[i] == urnet::BittensorLocalKeyWordWrong
            ? Glib::ustring(Format(T_("bittensor_key_word_wrong",
                                      "That isn't word {} of this recovery phrase."),
                                   positions[i]))
            : Glib::ustring());
  }
  confirmButton_->set_sensitive(check.ready && !screen.busy && !authenticating_);
}

// ---- actions -----------------------------------------------------------------------

void BittensorKeyScreen::OnReveal() {
  const urnet::BittensorLocalKeyScreen& screen = host_.Screen();
  if (screen.revealed || screen.hidden_by_capture || authenticating_) return;
  if (!screen.needs_authentication) {
    host_.Reveal(false);
    return;
  }
  authenticating_ = true;
  Apply();
  std::weak_ptr<bool> alive = alive_;
  AuthenticateForBittensorKey([this, alive](bool authenticated) {
    const std::shared_ptr<bool> live = alive.lock();
    if (!live || !*live) return;
    authenticating_ = false;
    if (authenticated) {
      // the dialog had the focus: the words show once this window has it back
      if (is_active()) {
        host_.Reveal(true);
      } else {
        revealWhenFocused_ = true;
      }
    }
    Apply();
  });
}

void BittensorKeyScreen::OnCopy() {
  if (!host_.CopyPhrase()) return;
  copyButton_->set_label(T_("copied", "Copied!"));
  copiedReset_.disconnect();
  std::weak_ptr<bool> alive = alive_;
  copiedReset_ = Glib::signal_timeout().connect(
      [this, alive]() -> bool {
        const std::shared_ptr<bool> live = alive.lock();
        if (live && *live) copyButton_->set_label(T_("copy", "Copy"));
        return false;
      },
      kCopiedResetMs);
}

void BittensorKeyScreen::OnSubmit() {
  const urnet::BittensorLocalKeyScreen& screen = host_.Screen();
  if (screen.confirming.empty() || screen.busy || authenticating_) return;
  const std::string first = wordFields_[0]->get_text();
  const std::string second = wordFields_[1]->get_text();
  if (!host_.CheckWords(first, second).ready) return;
  if (!screen.confirm_needs_authentication) {
    host_.SubmitWords(first, second, false);
    return;
  }
  // deleting a stored key: the user's password at the button, after the
  // words; nothing happens without it
  authenticating_ = true;
  Apply();
  std::weak_ptr<bool> alive = alive_;
  AuthenticateForBittensorKey([this, alive, first, second](bool authenticated) {
    const std::shared_ptr<bool> live = alive.lock();
    if (!live || !*live) return;
    authenticating_ = false;
    if (authenticated) host_.SubmitWords(first, second, true);
    Apply();
  });
}

bool BittensorKeyScreen::OnCloseRequest() {
  // a signing on its way is not abandoned
  if (host_.Screen().busy) return true;
  host_.CloseScreen();
  return false;
}

}  // namespace urnw
