// SPDX-License-Identifier: MPL-2.0
#include "TransportSheet.hpp"

#include <algorithm>

#include "I18n.hpp"
#include "TransportPresentation.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// the mode-row / switch-row color dot (apple 10pt)
constexpr int kRowDotSize = 10;

bool Contains(const urnet::StringList& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

Gtk::Label* MakeFooter(const std::string& text) {
  auto* footer = Gtk::make_managed<Gtk::Label>(text);
  footer->add_css_class("dim-label");
  footer->add_css_class("caption");
  footer->set_wrap(true);
  footer->set_xalign(0);
  return footer;
}

}  // namespace

TransportSheet::TransportSheet(Gtk::Window& parent, SdkHost& host, Kind kind)
    : host_(host), kind_(kind) {
  EnsureDrawerCss();
  set_title(kind_ == Kind::Client ? T_("transports", "Transports")
                                  : T_("provider_transports", "Provider transports"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 540);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  // the selectable modes in the SDK's default preference order: the order
  // every transport list shows (never a hardcoded list in the app)
  if (auto modes = urnet::selectableTransportModes()) modes_ = std::move(*modes);
  BuildUi();
}

std::optional<urnet::TransportSettings> TransportSheet::DefaultSettings() const {
  return kind_ == Kind::Client ? urnet::defaultTransportSettings()
                               : urnet::defaultProviderTransportSettings();
}

std::optional<urnet::TransportSettings> TransportSheet::CurrentSettings() const {
  // the device's policy (the daemon's truth over the rpc, or the pending /
  // last known one offline), else the GUI's persisted mirror, else the SDK
  // default -- the editor always has something to draft from
  auto settings = kind_ == Kind::Client ? host_.GetTransportSettings()
                                        : host_.GetProviderTransportSettings();
  if (!settings) settings = DefaultSettings();
  return settings;
}

void TransportSheet::Open() {
  original_ = CurrentSettings();
  draft_ = original_;
  SyncFromDraft();
  present();
}

bool TransportSheet::IsAuto() const {
  // a single selectable mode is selected; anything else (auto, or an
  // unrecognized mode, which the sdk normalizes to auto) is Auto
  return !draft_ || !Contains(modes_, draft_->mode);
}

void TransportSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  set_child(*root);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_vexpand(true);
  root->append(*scroller);

  auto* form = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  form->set_margin(16);
  scroller->set_child(*form);

  // transport mode: Auto, then one row per selectable mode in the SDK order
  auto* modeSection = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  modeSection->append(*MakeCaption(T_("transport", "Transport")));
  auto* modeCard = MakeCard(4);
  modeCard->append(*MakeModeRow(urnet::TransportModeAuto));
  for (const auto& mode : modes_) modeCard->append(*MakeModeRow(mode));
  modeSection->append(*modeCard);
  modeSection->append(*MakeFooter(
      kind_ == Kind::Client
          ? T_("transport_client_footer",
               "The transport this device uses to reach providers. Auto tries the enabled "
               "transports in preference order and keeps every healthy transport of the same "
               "tier connected in parallel.")
          : T_("transport_provider_footer",
               "The transport this device uses while providing for others. Auto tries the "
               "enabled transports in preference order and keeps every healthy transport of "
               "the same tier connected in parallel.")));
  form->append(*modeSection);

  // enabled under Auto: a switch per selectable mode in the SDK order (only
  // while Auto is selected)
  autoSection_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  autoSection_->append(*MakeCaption(T_("enabled_under_auto", "Enabled under Auto")));
  auto* autoCard = MakeCard(10);
  for (const auto& mode : modes_) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    row->append(*transport::MakeDot(mode, kRowDotSize, /*hollow=*/false));
    auto* label = Gtk::make_managed<Gtk::Label>(transport::DisplayName(mode));
    label->set_xalign(0);
    label->set_hexpand(true);
    row->append(*label);
    AutoRow autoRow;
    autoRow.mode = mode;
    autoRow.toggle = Gtk::make_managed<Gtk::Switch>();
    autoRow.toggle->set_valign(Gtk::Align::CENTER);
    Gtk::Switch* toggle = autoRow.toggle;
    const std::string modeCopy = mode;
    toggle->property_active().signal_changed().connect([this, toggle, modeCopy] {
      if (updating_) return;
      // on/off for one carrier under auto, applied through the sdk so the
      // default-priority and last-carrier rules are the sdk's: a refused edit
      // (disabling the last enabled carrier) hands back an equal policy and
      // the switch flips back in the sync
      draft_ = urnet::transportSettingsWithAutoModeEnabled(draft_, modeCopy, toggle->get_active());
      SyncFromDraft();
    });
    row->append(*autoRow.toggle);
    autoCard->append(*row);
    autoRows_.push_back(autoRow);
  }
  autoSection_->append(*autoCard);
  autoSection_->append(*MakeFooter(
      T_("enabled_under_auto_footer",
         "Listed in preference order: H3 and H1 first, then whodis, then whodis pump. The "
         "order is fixed. At least one transport stays enabled.")));
  form->append(*autoSection_);

  // restore the SDK default policy (only while the draft differs from it)
  restoreSection_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  auto* restoreBtn =
      Gtk::make_managed<Gtk::Button>(T_("restore_default_transports", "Restore default transports"));
  restoreBtn->add_css_class("pill");
  restoreBtn->signal_clicked().connect([this] {
    draft_ = DefaultSettings();
    SyncFromDraft();
  });
  restoreSection_->append(*restoreBtn);
  form->append(*restoreSection_);

  // apply bar
  auto* actionBar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  actionBar->set_margin(12);
  updateBtn_ = Gtk::make_managed<Gtk::Button>(T_("update", "Update"));
  updateBtn_->add_css_class("suggested-action");
  updateBtn_->add_css_class("pill");
  updateBtn_->set_sensitive(false);  // Open() syncs the draft and re-decides
  updateBtn_->signal_clicked().connect([this] {
    if (draft_) {
      // applies over the device rpc (the daemon persists it) and mirrors into
      // the GUI local state; the applied policy comes back through the change
      // listener
      if (kind_ == Kind::Client) {
        host_.SetTransportSettings(*draft_);
      } else {
        host_.SetProviderTransportSettings(*draft_);
      }
    }
    set_visible(false);
  });
  actionBar->append(*updateBtn_);
  root->append(*actionBar);
}

// A selectable row for one transport mode: Auto (with its own subtitle) or a
// selectable carrier with its color dot, name and one-line description; a
// checkmark on the selected row.
Gtk::Widget* TransportSheet::MakeModeRow(const std::string& mode) {
  const bool isAuto = mode == urnet::TransportModeAuto;
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->add_css_class("flat");
  button->set_halign(Gtk::Align::FILL);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  if (!isAuto) {
    auto* dot = transport::MakeDot(mode, kRowDotSize, /*hollow=*/false);
    dot->set_valign(Gtk::Align::START);
    dot->set_margin_top(5);
    content->append(*dot);
  }
  auto* textColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  textColumn->set_hexpand(true);
  auto* name = Gtk::make_managed<Gtk::Label>(isAuto ? std::string(T_("window_type_auto", "Auto"))
                                                    : transport::DisplayName(mode));
  name->set_xalign(0);
  textColumn->append(*name);
  const std::string detail =
      isAuto ? std::string(T_("transport_auto_description",
                              "Recommended. Uses the enabled transports below."))
             : transport::Description(mode);
  if (!detail.empty()) {
    auto* detailLabel = Gtk::make_managed<Gtk::Label>(detail);
    detailLabel->add_css_class("dim-label");
    detailLabel->add_css_class("caption");
    detailLabel->set_xalign(0);
    detailLabel->set_wrap(true);
    textColumn->append(*detailLabel);
  }
  content->append(*textColumn);

  ModeRow row;
  row.mode = mode;
  row.check = Gtk::make_managed<Gtk::Image>();
  row.check->set_from_icon_name("object-select-symbolic");
  row.check->add_css_class("ur-fg-green");
  row.check->set_valign(Gtk::Align::CENTER);
  row.check->set_opacity(0);
  content->append(*row.check);
  button->set_child(*content);
  modeRows_.push_back(row);

  const std::string modeCopy = mode;
  button->signal_clicked().connect([this, modeCopy] {
    // selecting a mode sets the policy mode; the Auto policy is retained by
    // the sdk, so switching back to Auto restores the same enabled set
    draft_ = urnet::transportSettingsWithMode(draft_, modeCopy);
    SyncFromDraft();
  });
  return button;
}

void TransportSheet::SyncFromDraft() {
  updating_ = true;
  const std::string selectedMode =
      IsAuto() ? std::string(urnet::TransportModeAuto) : draft_->mode;
  for (auto& row : modeRows_) row.check->set_opacity(row.mode == selectedMode ? 1 : 0);

  // the carriers enabled under auto (retained while a single mode is
  // selected), read through the sdk
  urnet::StringList autoModes;
  if (auto modes = urnet::transportSettingsAutoModes(draft_)) autoModes = std::move(*modes);
  for (auto& row : autoRows_) {
    const bool on = Contains(autoModes, row.mode);
    if (row.toggle->get_active() != on) row.toggle->set_active(on);
    // the last enabled carrier can't be turned off (the sdk refuses the edit:
    // an empty auto policy would resolve to the full default), so show it
    // disabled
    row.toggle->set_sensitive(!(on && autoModes.size() == 1));
  }
  autoSection_->set_visible(IsAuto());
  restoreSection_->set_visible(!urnet::transportSettingsEqual(draft_, DefaultSettings()));
  updating_ = false;
  RefreshDirty();
}

void TransportSheet::RefreshDirty() {
  // Update applies the draft only when it differs from the loaded policy
  // (the sdk's normalized comparison)
  updateBtn_->set_sensitive(!urnet::transportSettingsEqual(draft_, original_));
}

}  // namespace urnw
