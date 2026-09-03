// SPDX-License-Identifier: MPL-2.0
#include "EmojiTagSheet.hpp"

#include <urnetwork_sdk.hpp>

#include "EmojiKeyboard.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {

namespace {

// the keyboard's grid: keys per row, and one key's footprint
constexpr int kEmojiKeysPerRow = 8;
constexpr int kEmojiKeySize = 40;

// a label at an explicit absolute size (the draft, the keys)
void SetLabelSize(Gtk::Label& label, int sizePx) {
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(sizePx * PANGO_SCALE);
  attrs.insert(size);
  label.set_attributes(attrs);
}

}  // namespace

EmojiTagSheet::EmojiTagSheet(Gtk::Window& parent, Saver saver) : saver_(std::move(saver)) {
  EnsureDrawerCss();
  set_title(T_("emoji_tag", "Emoji tag"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(440, -1);
  set_resizable(false);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  BuildUi();
}

EmojiTagSheet::~EmojiTagSheet() { *alive_ = false; }

void EmojiTagSheet::Open(const std::string& currentTag) {
  currentTag_ = currentTag;
  saving_ = false;
  // a network with no tag starts from the SDK's suggestion; one with a tag
  // starts from the tag, split into the emoji the backspace removes one by one
  draft_ = emoji::SplitEmoji(currentTag_.empty() ? urnet::suggestEmojiTag(0) : currentTag_);
  clearButton_->set_visible(!currentTag_.empty());
  ApplyDraft();
  present();
}

void EmojiTagSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  root->set_margin(24);
  set_child(*root);

  auto* hint = Gtk::make_managed<Gtk::Label>(
      Format(T_("emoji_tag_hint", "Pick 1 to {} emoji to show next to your network."),
             static_cast<int64_t>(urnet::EmojiTagMaxCount)));
  hint->add_css_class("dim-label");
  hint->add_css_class("caption");
  hint->set_wrap(true);
  hint->set_xalign(0);
  root->append(*hint);

  // the draft: a read-only line, edited only through the keys
  auto* draftRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* field = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  field->add_css_class("ur-input");
  field->set_hexpand(true);
  field->set_size_request(-1, 52);
  draftLabel_ = Gtk::make_managed<Gtk::Label>();
  draftLabel_->set_xalign(0);
  draftLabel_->set_margin_start(12);
  draftLabel_->set_margin_end(12);
  draftLabel_->set_valign(Gtk::Align::CENTER);
  draftLabel_->set_ellipsize(Pango::EllipsizeMode::END);
  SetLabelSize(*draftLabel_, 28);
  field->append(*draftLabel_);
  draftRow->append(*field);

  backspaceButton_ = Gtk::make_managed<Gtk::Button>();
  backspaceButton_->set_icon_name("edit-clear-symbolic");
  backspaceButton_->set_valign(Gtk::Align::CENTER);
  backspaceButton_->set_tooltip_text(T_("emoji_tag_delete_last", "Delete last emoji"));
  kit::SetAccessibleLabel(*backspaceButton_, T_("emoji_tag_delete_last", "Delete last emoji"));
  backspaceButton_->signal_clicked().connect([this] { DropLast(); });
  draftRow->append(*backspaceButton_);

  shuffleButton_ = Gtk::make_managed<Gtk::Button>();
  shuffleButton_->set_icon_name("media-playlist-shuffle-symbolic");
  shuffleButton_->set_valign(Gtk::Align::CENTER);
  shuffleButton_->set_tooltip_text(T_("emoji_tag_shuffle", "Suggest another"));
  kit::SetAccessibleLabel(*shuffleButton_, T_("emoji_tag_shuffle", "Suggest another"));
  shuffleButton_->signal_clicked().connect([this] { Shuffle(); });
  draftRow->append(*shuffleButton_);
  root->append(*draftRow);

  supportLabel_ = Gtk::make_managed<Gtk::Label>();
  supportLabel_->add_css_class("caption");
  supportLabel_->add_css_class("dim-label");
  supportLabel_->set_xalign(0);
  supportLabel_->set_wrap(true);
  root->append(*supportLabel_);

  BuildKeyboard(*root);

  // Clear (only when a tag is stored), Cancel, Save
  auto* actionRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actionRow->set_margin_top(8);
  actionRow->set_halign(Gtk::Align::END);
  spinner_ = Gtk::make_managed<Gtk::Spinner>();
  spinner_->set_visible(false);
  actionRow->append(*spinner_);
  clearButton_ = Gtk::make_managed<Gtk::Button>(T_("clear", "Clear"));
  clearButton_->signal_clicked().connect([this] { Submit(std::string()); });
  actionRow->append(*clearButton_);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { set_visible(false); });
  actionRow->append(*cancel);
  saveButton_ = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
  saveButton_->add_css_class("suggested-action");
  saveButton_->set_sensitive(false);
  saveButton_->signal_clicked().connect([this] {
    if (!normalized_.empty()) Submit(normalized_);
  });
  actionRow->append(*saveButton_);
  root->append(*actionRow);
}

// The keyboard: a strip of group glyphs, and the selected group's keys in a
// flow of kEmojiKeysPerRow. One group at a time keeps the window light; the
// strip is the only navigation and it is entirely emoji, so it needs no words.
void EmojiTagSheet::BuildKeyboard(Gtk::Box& into) {
  groupStrip_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  groupStrip_->add_css_class("linked");
  groupStrip_->set_halign(Gtk::Align::START);
  const auto& groups = emoji::Groups();
  Gtk::ToggleButton* first = nullptr;
  for (size_t i = 0; i < groups.size(); ++i) {
    auto* tab = Gtk::make_managed<Gtk::ToggleButton>(emoji::EncodeUtf8(groups[i].icon));
    tab->set_size_request(kEmojiKeySize, 36);
    if (first == nullptr) {
      first = tab;
    } else {
      tab->set_group(*first);
    }
    tab->signal_toggled().connect([this, tab, i] {
      if (tab->get_active()) ShowGroup(i);
    });
    groupButtons_.push_back(tab);
    groupStrip_->append(*tab);
  }
  into.append(*groupStrip_);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_size_request(-1, 232);
  keys_ = Gtk::make_managed<Gtk::FlowBox>();
  keys_->set_selection_mode(Gtk::SelectionMode::NONE);
  keys_->set_homogeneous(true);
  keys_->set_min_children_per_line(kEmojiKeysPerRow);
  keys_->set_max_children_per_line(kEmojiKeysPerRow);
  keys_->set_column_spacing(2);
  keys_->set_row_spacing(2);
  keys_->set_valign(Gtk::Align::START);
  scroller->set_child(*keys_);
  into.append(*scroller);

  if (first != nullptr) first->set_active(true);  // ShowGroup(0) through the toggle
}

void EmojiTagSheet::ShowGroup(size_t index) {
  const auto& groups = emoji::Groups();
  if (index >= groups.size() || keys_ == nullptr) return;
  group_ = index;
  // FlowBox children are FlowBoxChild wrappers around the buttons
  while (Gtk::Widget* child = keys_->get_first_child()) keys_->remove(*child);
  keyButtons_.clear();
  for (char32_t cp : groups[index].emoji) {
    const std::string utf8 = emoji::EncodeUtf8(cp);
    auto* key = Gtk::make_managed<Gtk::Button>();
    auto* glyph = Gtk::make_managed<Gtk::Label>(utf8);
    SetLabelSize(*glyph, 20);
    key->set_child(*glyph);
    key->add_css_class("flat");
    key->set_size_request(kEmojiKeySize, kEmojiKeySize);
    key->set_sensitive(!full_ && !saving_);
    key->signal_clicked().connect([this, utf8] { Append(utf8); });
    keyButtons_.push_back(key);
    keys_->append(*key);
  }
}

void EmojiTagSheet::Append(const std::string& emoji) {
  if (full_ || saving_) return;
  draft_.push_back(emoji);
  ApplyDraft();
}

void EmojiTagSheet::DropLast() {
  if (saving_ || draft_.empty()) return;
  draft_.pop_back();
  ApplyDraft();
}

void EmojiTagSheet::Shuffle() {
  if (saving_) return;
  draft_ = emoji::SplitEmoji(urnet::suggestEmojiTag(0));
  ApplyDraft();
}

void EmojiTagSheet::ApplyDraft() {
  const std::string draft = emoji::Join(draft_);
  const auto verdict = urnet::validateEmojiTag(draft);
  const bool ok = verdict && verdict->ok;
  const int64_t count = verdict ? verdict->count : 0;
  normalized_ = ok ? verdict->normalized : std::string();
  const emoji::TagError error = emoji::ErrorFor(ok, verdict ? verdict->reason : std::string());
  const bool showsError = emoji::ShowsError(draft, error);
  full_ = ok && count >= urnet::EmojiTagMaxCount;

  draftLabel_->set_text(draft);

  Glib::ustring support;
  if (showsError) {
    switch (error) {
      case emoji::TagError::Empty:
        support = T_("emoji_tag_error_empty", "Add at least one emoji.");
        break;
      case emoji::TagError::TooMany:
        support = Format(T_("emoji_tag_error_too_many", "Use at most {} emoji."),
                         static_cast<int64_t>(urnet::EmojiTagMaxCount));
        break;
      default:
        support = T_("emoji_tag_error_not_emoji", "Only emoji are allowed.");
        break;
    }
  } else {
    // the counter is the store's untranslatable "{count} / {max}" key; the
    // English fallback renders the same until the po files carry the key
    support = Format(T_("emoji_tag_counter", "{} / {}"), count, urnet::EmojiTagMaxCount);
  }
  supportLabel_->set_text(support);
  if (showsError) {
    supportLabel_->remove_css_class("dim-label");
    supportLabel_->add_css_class("ur-error-text");
  } else {
    supportLabel_->remove_css_class("ur-error-text");
    supportLabel_->add_css_class("dim-label");
  }

  backspaceButton_->set_sensitive(!draft_.empty() && !saving_);
  shuffleButton_->set_sensitive(!saving_);
  for (Gtk::Button* key : keyButtons_) key->set_sensitive(!full_ && !saving_);
  saveButton_->set_sensitive(emoji::CanSave(ok, normalized_, currentTag_, saving_));
  clearButton_->set_sensitive(!saving_);
  spinner_->set_visible(saving_);
  if (saving_) {
    spinner_->start();
  } else {
    spinner_->stop();
  }
}

void EmojiTagSheet::Submit(const std::string& tag) {
  if (saving_ || !saver_) return;
  saving_ = true;
  ApplyDraft();
  auto alive = alive_;
  saver_(tag, [this, alive](std::string error) {
    if (!*alive) return;
    ApplyResult(error);
  });
}

void EmojiTagSheet::ApplyResult(const std::string& error) {
  saving_ = false;
  if (error.empty()) {
    set_visible(false);
    return;
  }
  ApplyDraft();
  supportLabel_->set_text(error);
  supportLabel_->remove_css_class("dim-label");
  supportLabel_->add_css_class("ur-error-text");
}

}  // namespace urnw
