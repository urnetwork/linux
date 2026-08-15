// SPDX-License-Identifier: MPL-2.0
#include "SettingsPage.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AppPrefs.hpp"
#include "I18n.hpp"
#include "KillSwitchCopy.hpp"
#include "PaneKit.hpp"
#include "PostQuantumIdentity.hpp"  // ProviderIdentitiesSheet (reused as-is)
#include "SplitRulesSheet.hpp"      // reused as-is (the split-rule editor)
#include "Ui.hpp"
#include "UrTheme.hpp"

// The build's compile-time stamp — meson passes it to both binaries
// (-DUR_APP_VERSION); the fallback keeps this TU self-contained the way
// SdkHost.cpp's does. "0.0.0" outside a release build is the CORRECT answer,
// not a bug: release tags, the update banner and the OS metadata all speak
// this exact string.
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

namespace urnw {
namespace {

// ---- key numbers (spec §11) -------------------------------------------------
constexpr int kRowTall = 44;      // the two-line row species this page is made of
constexpr int kRowSingle = 40;    // the protocol-link row
constexpr int kProsePadY = 10;    // supporting prose: padding 12,10
constexpr int kStatePadY = 8;     // a state line under a control: padding 12,8
constexpr int kSheetDeviceName = 360;
constexpr int kSheetBlocked = 420;
constexpr int kCountryScrollMax = 240;
// The two canonical destinations behind the Stay-in-touch sentences. They are
// duplicated inside the localized markdown, so keep the pair in step.
constexpr const char* kDiscordUrl = "https://discord.com/invite/RUNZXMwPRK";
constexpr const char* kDepinHubUrl = "https://depinhub.io/projects/urnetwork";
constexpr const char* kProtocolUrl = "https://ur.xyz";
// The local preference the auto-update toggle owns (windows UpdateChecker:
// key "check_updates_automatically" in app_prefs.json, default true).
constexpr const char* kAutoCheckKey = "check_updates_automatically";

// ---- tone -------------------------------------------------------------------
// A line's colour is written as a pango attribute, not as a swapped CSS class.
// WHY (keep this): the pane vocabulary's own classes (.ur-value/.ur-key/
// .ur-row-title) are declared AFTER .ur-label-faint/.ur-danger-text in the same
// provider, so a tone class added on top of a row class silently loses the
// cascade. A markup foreground beats the CSS colour for the run it covers,
// which is exactly what a state change needs — and it is the idiom the rest of
// this tree already uses for dots and the unstable-provider line.
void SetToned(Gtk::Label& line, const Rgba& color, const Glib::ustring& text) {
  line.set_markup("<span foreground='" + HexForMarkup(color) + "'>" +
                  Glib::Markup::escape_text(text) + "</span>");
}

// ---- FieldState -------------------------------------------------------------
// One writer for a value line's TEXT and its COLOUR: muted when the value is
// real, faint for the three "nothing here yet" states, danger for Failed.
void ApplyFieldState(Gtk::Label& line, SettingsFieldState state,
                     const Glib::ustring& loadedText = {}) {
  Glib::ustring text;
  Rgba tone = kUrTextMuted;
  switch (state) {
    case SettingsFieldState::Loaded:
      text = loadedText;
      break;
    case SettingsFieldState::Loading:
      text = T_("loading", "Loading...");
      tone = kUrTextFaint;
      break;
    case SettingsFieldState::Empty:
      text = T_("none", "None");
      tone = kUrTextFaint;
      break;
    case SettingsFieldState::NoSession:
      text = T_("please_login_to_urnetwork", "Please login to URnetwork");
      tone = kUrTextFaint;
      break;
    case SettingsFieldState::NoDevice:
      // signed in, service not up — never "please login"
      text = T_("site_app_device_attaching", "Attaching device controls…");
      tone = kUrTextFaint;
      break;
    case SettingsFieldState::Failed:
      text = T_("something_went_wrong", "Something went wrong.");
      tone = kUrDanger;
      break;
  }
  SetToned(line, tone, text);
}

// ---- pane-mode row species the kit does not carry ---------------------------
// `Supporting()` in pane mode: 12px muted prose that is ALLOWED to be taller
// than a row (it is the one such element), but still on the pane's grid — the
// 12px inset and the bottom hairline come from kit::MakePaneRow so it cannot
// drift from the rows above and below it.
struct ProseRow {
  Gtk::Widget* root = nullptr;
  Gtk::Label* line = nullptr;
};

ProseRow MakeProseRow(const Glib::ustring& text, int padY) {
  ProseRow out;
  auto* host = kit::MakePaneRow(0);  // height 0: the prose sets the height
  out.line = Gtk::make_managed<Gtk::Label>(text);
  out.line->add_css_class("ur-caption");  // 12px muted, the supporting voice
  out.line->set_xalign(0);
  out.line->set_wrap(true);
  out.line->set_hexpand(true);
  out.line->set_margin_top(padY);
  out.line->set_margin_bottom(padY);
  if (auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child())) {
    inner->append(*out.line);
  }
  out.root = host;
  return out;
}

// ToggleRow: the tall row with a platform switch right-aligned. The switch is
// otherwise NAMELESS (its on/off content is empty), so it takes the row label
// as its accessible name — a defect this project has shipped before.
Gtk::Switch* AddToggleRow(Gtk::Box& host, const Glib::ustring& label,
                          const Glib::ustring& note) {
  auto row = kit::MakePaneTwoLineRow(label, note, kRowTall);
  auto* toggle = Gtk::make_managed<Gtk::Switch>();
  toggle->set_valign(Gtk::Align::CENTER);
  kit::SetAccessibleLabel(*toggle, label);
  row.trailing->append(*toggle);
  host.append(*row.root);
  return toggle;
}

// ValueRow: the same row with a right-aligned, trimmed, muted readout. The
// value is never touched for accessibility — its text IS its name.
Gtk::Label* AddValueRow(Gtk::Box& host, const Glib::ustring& label,
                        const Glib::ustring& note = {}) {
  auto row = kit::MakePaneTwoLineRow(label, note, kRowTall);
  auto* value = Gtk::make_managed<Gtk::Label>();
  value->add_css_class("ur-value");  // family + size; the tone is a markup run
  value->set_xalign(1.f);
  value->set_valign(Gtk::Align::CENTER);
  value->set_ellipsize(Pango::EllipsizeMode::END);
  value->set_max_width_chars(30);  // windows MaxWidth 260
  row.trailing->append(*value);
  host.append(*row.root);
  return value;
}

// ButtonRow: the same row with an action verb on the right. GTK carries no
// separate FullDescription channel, so the button's name is "verb. row label"
// — the verb first, the way the windows pair announces.
Gtk::Button* AddButtonRow(Gtk::Box& host, const Glib::ustring& label,
                          const Glib::ustring& note, const Glib::ustring& action) {
  auto row = kit::MakePaneTwoLineRow(label, note, kRowTall);
  auto* button = Gtk::make_managed<Gtk::Button>(action);
  button->set_valign(Gtk::Align::CENTER);
  kit::SetAccessibleLabel(*button, action + ". " + label);
  row.trailing->append(*button);
  host.append(*row.root);
  return button;
}

// Go strings.TrimSpace parity — a box of spaces is not a name and not a query.
std::string TrimSpace(const std::string& text) {
  static const char* kWs = " \t\n\v\f\r";
  const auto begin = text.find_first_not_of(kWs);
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(kWs);
  return text.substr(begin, end - begin + 1);
}

std::string Lower(const std::string& text) {
  std::string out = text;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

// =============================================================================
// §6.1 DeviceNameSheet — edit this device's name
// =============================================================================
// The sheet names the client THIS JWT was issued to: device_id is left unset
// deliberately, so the server resolves "this device" and a stale cached id can
// never rename someone else's client.
class SettingsDeviceNameSheet : public Gtk::Window {
 public:
  SettingsDeviceNameSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("edit_device_name", "Edit device name"));
    set_default_size(kSheetDeviceName, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("ur-sheet");  // sheets sit ABOVE the page: #151515
    AddEscapeToClose(*this);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(24);

    auto* heading =
        Gtk::make_managed<Gtk::Label>(T_("edit_device_name", "Edit device name"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    box->append(*heading);

    auto* fieldLabel = Gtk::make_managed<Gtk::Label>(T_("device_name", "Device name"));
    fieldLabel->add_css_class("ur-input-label");
    fieldLabel->set_xalign(0);
    box->append(*fieldLabel);

    entry_ = Gtk::make_managed<Gtk::Entry>();
    entry_->add_css_class("ur-input");
    kit::SetAccessibleLabel(*entry_, T_("device_name", "Device name"));
    entry_->signal_activate().connect([this] { Submit(); });
    box->append(*entry_);

    error_ = Gtk::make_managed<Gtk::Label>();
    error_->add_css_class("ur-caption");
    error_->set_xalign(0);
    error_->set_wrap(true);
    error_->set_visible(false);
    box->append(*error_);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_halign(Gtk::Align::END);
    actions->set_margin_top(8);
    auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
    cancel->signal_clicked().connect([this] { set_visible(false); });
    actions->append(*cancel);
    save_ = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
    save_->add_css_class("suggested-action");  // the dialog-primary role
    save_->signal_clicked().connect([this] { Submit(); });
    actions->append(*save_);
    box->append(*actions);

    set_child(*box);
    // Enter in the field submits; Save is the DEFAULT action of this sheet
    // (the one sheet on this destination where Enter should commit).
    save_->set_receives_default(true);
    set_default_widget(*save_);
  }

  // The page updates its cached name + row from here (one writer per surface).
  std::function<void(const std::string& name)> on_saved;

  void Open(const std::string& currentName) {
    ++*epoch_;  // orphan any save still in flight from a previous presentation
    saving_ = false;
    error_->set_visible(false);
    entry_->set_text(currentName);  // display-only prefill

    const bool session = host_.IsLoggedIn();
    entry_->set_sensitive(session);
    save_->set_sensitive(session);
    if (!session) {
      // NOT a disabled field with no explanation: the sheet says which of the
      // six states it is in.
      ApplyFieldState(*error_, SettingsFieldState::NoSession);
      error_->set_visible(true);
    }
    present();
  }

 private:
  void ShowError(const Glib::ustring& message) {
    SetToned(*error_, kUrDanger, message);
    error_->set_visible(true);
  }

  void Submit() {
    if (saving_ || !host_.IsLoggedIn()) return;
    const std::string name = TrimSpace(entry_->get_text().raw());
    if (name.empty()) return;  // an empty name is a SILENT no-op, not an error

    saving_ = true;
    save_->set_sensitive(false);
    error_->set_visible(false);

    urnet::DeviceSetNameArgs args{};
    args.device_name = name;  // device_id deliberately unset: "this device"

    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.api().deviceSetName(
        std::optional<urnet::DeviceSetNameArgs>(args),
        [this, epoch, seen, name](std::optional<urnet::DeviceSetNameResult> result,
                                  std::optional<std::string> err) {
          PostToMain([this, epoch, seen, name, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;  // a newer Open() owns the sheet
            saving_ = false;
            save_->set_sensitive(true);  // on EVERY path

            // Server error first (it says WHAT was wrong), then the transport
            // error, then the store's generic line.
            std::string message;
            if (result && result->error) message = result->error->message;
            if (message.empty() && err) message = *err;
            const bool ok = !err.has_value() && result.has_value() && !result->error;
            if (!ok) {
              g_warning("settings: deviceSetName failed: %s",
                        message.empty() ? "(no error text)" : message.c_str());
              ShowError(message.empty()
                            ? Glib::ustring(T_("error_updating_device_name",
                                               "There was an error updating the device "
                                               "name."))
                            : Glib::ustring(message));
              return;
            }
            if (on_saved) on_saved(name);
            set_visible(false);
          });
        });
  }

  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  Gtk::Entry* entry_ = nullptr;
  Gtk::Label* error_ = nullptr;
  Gtk::Button* save_ = nullptr;
  bool saving_ = false;
};

// =============================================================================
// §6.2 BlockedLocationsSheet — the network's blocked countries
// =============================================================================
// BOTH lists carry their own FieldState. Without them a 401 arrived as an
// empty list and rendered as the reassuring "No blocked locations" — the exact
// failure mode rule 2 of the contract exists to prevent.
class SettingsBlockedLocationsSheet : public Gtk::Window {
 public:
  SettingsBlockedLocationsSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
    set_transient_for(parent);
    set_modal(true);
    set_title(T_("blocked_locations", "Blocked Locations"));
    set_default_size(kSheetBlocked, 560);
    set_hide_on_close(true);
    add_css_class("ur-sheet");
    AddEscapeToClose(*this);

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* heading =
        Gtk::make_managed<Gtk::Label>(T_("blocked_locations", "Blocked Locations"));
    heading->add_css_class("ur-step-heading");
    heading->set_xalign(0);
    box->append(*heading);

    blockedPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    box->append(*blockedPanel_);
    blockedEmpty_ = Gtk::make_managed<Gtk::Label>();
    blockedEmpty_->add_css_class("ur-caption");
    blockedEmpty_->set_xalign(0);
    blockedEmpty_->set_wrap(true);
    SetToned(*blockedEmpty_, kUrTextFaint,
             T_("no_blocked_locations", "No blocked locations"));
    box->append(*blockedEmpty_);

    box->append(*kit::MakeDivider());  // card mode: dialogs DO draw dividers

    auto* pickerLabel = Gtk::make_managed<Gtk::Label>(
        T_("select_country_to_block", "Select country to block"));
    pickerLabel->add_css_class("ur-caption");
    pickerLabel->set_xalign(0);
    box->append(*pickerLabel);

    search_ = Gtk::make_managed<Gtk::SearchEntry>();
    search_->set_placeholder_text(T_("search_placeholder", "Search for all locations"));
    // a placeholder is NOT an accessible name
    kit::SetAccessibleLabel(*search_,
                            T_("select_country_to_block", "Select country to block"));
    search_->signal_search_changed().connect([this] { RenderCountries(); });
    box->append(*search_);

    auto* countryScroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    countryScroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    countryScroll->set_max_content_height(kCountryScrollMax);
    countryScroll->set_propagate_natural_height(true);
    countryPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    countryScroll->set_child(*countryPanel_);
    box->append(*countryScroll);

    error_ = Gtk::make_managed<Gtk::Label>();
    error_->add_css_class("ur-caption");
    error_->set_xalign(0);
    error_->set_wrap(true);
    error_->set_visible(false);
    box->append(*error_);

    scroller->set_child(*box);
    set_child(*scroller);
  }

  void Open() {
    ++*epoch_;  // every in-flight list/mutation from a previous presentation dies
    loadingCountries_ = false;
    error_->set_visible(false);
    present();
    LoadBlocked();
    LoadCountries();
  }

 private:
  struct Country {
    std::string id;
    std::string name;
  };

  // The blocked list gates on IsLoggedIn — NOT on "the api exists" (that is
  // true from SDK init, long before a login).
  void LoadBlocked() {
    if (!host_.IsLoggedIn()) {
      blockedState_ = SettingsFieldState::NoSession;
      blocked_.clear();
      RenderBlocked();
      RenderCountries();
      return;
    }
    blockedState_ = SettingsFieldState::Loading;
    RenderBlocked();

    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.api().getNetworkBlockedLocations(
        [this, epoch, seen](std::optional<urnet::GetNetworkBlockedLocationsResult> result,
                            std::optional<std::string> err) {
          PostToMain([this, epoch, seen, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            // the result struct carries no error field: failure IS transport
            if (!result || err) {
              g_warning("settings: getNetworkBlockedLocations failed: %s",
                        err ? err->c_str() : "(no result)");
              blockedState_ = SettingsFieldState::Failed;
              blocked_.clear();
            } else {
              blocked_.clear();
              if (result->blocked_locations) {
                blocked_ = *result->blocked_locations;
              }
              std::sort(blocked_.begin(), blocked_.end(),
                        [](const urnet::BlockedLocation& a,
                           const urnet::BlockedLocation& b) {
                          return a.location_name < b.location_name;
                        });
              blockedState_ = blocked_.empty() ? SettingsFieldState::Empty
                                               : SettingsFieldState::Loaded;
            }
            RenderBlocked();
            RenderCountries();  // an already-blocked country leaves the picker
          });
        });
  }

  void LoadCountries() {
    // getProviderLocations answers unauthenticated, but a signed-out dev
    // switch must not talk to production — same gate as the blocked list.
    if (!host_.IsLoggedIn()) {
      countriesState_ = SettingsFieldState::NoSession;
      countries_.clear();
      RenderCountries();
      return;
    }
    if (loadingCountries_) return;  // re-entry guard
    loadingCountries_ = true;
    countriesState_ = SettingsFieldState::Loading;
    RenderCountries();

    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.api().getProviderLocations(
        [this, epoch, seen](std::optional<urnet::FindLocationsResult> result,
                            std::optional<std::string> err) {
          PostToMain([this, epoch, seen, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            loadingCountries_ = false;
            if (!result || err) {
              g_warning("settings: getProviderLocations failed: %s",
                        err ? err->c_str() : "(no result)");
              countriesState_ = SettingsFieldState::Failed;
              countries_.clear();
              RenderCountries();
              return;
            }
            countries_.clear();
            if (result->locations) {
              for (const auto& location : *result->locations) {
                if (location.location_type != urnet::LocationTypeCountry) continue;
                std::string id;
                if (location.location_id && !location.location_id->empty()) {
                  id = *location.location_id;
                } else if (location.country_location_id &&
                           !location.country_location_id->empty()) {
                  id = *location.country_location_id;
                }
                if (id.empty() || location.name.empty()) continue;
                countries_.push_back(Country{id, location.name});
              }
            }
            std::sort(countries_.begin(), countries_.end(),
                      [](const Country& a, const Country& b) { return a.name < b.name; });
            countriesState_ = countries_.empty() ? SettingsFieldState::Empty
                                                 : SettingsFieldState::Loaded;
            RenderCountries();
          });
        });
  }

  void RenderBlocked() {
    RemoveAllChildren(*blockedPanel_);
    if (blockedState_ != SettingsFieldState::Loaded || blocked_.empty()) {
      // The genuine-empty case gets the SPECIFIC line; every other state gets
      // its own words, so a 401 never reads as "nothing is blocked".
      if (blockedState_ == SettingsFieldState::Empty ||
          (blockedState_ == SettingsFieldState::Loaded && blocked_.empty())) {
        // the SPECIFIC line beats the generic "None"
        SetToned(*blockedEmpty_, kUrTextFaint,
                 T_("no_blocked_locations", "No blocked locations"));
      } else {
        ApplyFieldState(*blockedEmpty_, blockedState_);
      }
      blockedEmpty_->set_visible(true);
      return;
    }
    blockedEmpty_->set_visible(false);
    for (const auto& location : blocked_) {
      auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      auto* name = Gtk::make_managed<Gtk::Label>(location.location_name);
      name->add_css_class("ur-body");
      name->set_xalign(0);
      name->set_hexpand(true);
      name->set_ellipsize(Pango::EllipsizeMode::END);
      row->append(*name);
      auto* remove = Gtk::make_managed<Gtk::Button>(T_("remove", "Remove"));
      remove->add_css_class("flat");
      remove->add_css_class("ur-danger-text");
      remove->set_valign(Gtk::Align::CENTER);
      kit::SetAccessibleLabel(*remove, Glib::ustring(T_("remove", "Remove")) + ". " +
                                           location.location_name);
      const std::string id =
          location.location_id ? *location.location_id : std::string();
      // an entry the server gave no id for cannot be unblocked — say so by
      // disabling, never by a click that silently does nothing
      remove->set_sensitive(!id.empty());
      remove->signal_clicked().connect([this, id] { Unblock(id); });
      row->append(*remove);
      blockedPanel_->append(*row);
    }
  }

  void RenderCountries() {
    RemoveAllChildren(*countryPanel_);
    const std::string filter = Lower(TrimSpace(search_->get_text().raw()));
    int shown = 0;
    for (const auto& country : countries_) {
      if (!filter.empty() && Lower(country.name).find(filter) == std::string::npos) {
        continue;
      }
      bool alreadyBlocked = false;
      for (const auto& location : blocked_) {
        if (location.location_id && *location.location_id == country.id) {
          alreadyBlocked = true;
          break;
        }
      }
      if (alreadyBlocked) continue;
      auto* button = Gtk::make_managed<Gtk::Button>();
      button->add_css_class("flat");
      auto* label = Gtk::make_managed<Gtk::Label>(country.name);
      label->set_xalign(0);
      label->set_hexpand(true);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      button->set_child(*label);
      kit::SetAccessibleLabel(*button, country.name);
      const std::string id = country.id;
      button->signal_clicked().connect([this, id] { Block(id); });
      countryPanel_->append(*button);
      ++shown;
    }
    if (shown > 0) return;

    // Zero rows has FOUR distinguishable causes; the line says WHICH.
    auto* line = Gtk::make_managed<Gtk::Label>();
    line->add_css_class("ur-caption");
    line->set_xalign(0);
    line->set_wrap(true);
    if (countriesState_ == SettingsFieldState::Loaded && !countries_.empty()) {
      // the list loaded and has rows — the SEARCH matched nothing
      SetToned(*line, kUrTextFaint, T_("no_locations_found", "No locations found"));
    } else {
      ApplyFieldState(*line, countriesState_);
    }
    countryPanel_->append(*line);
  }

  void ShowError(const Glib::ustring& message) {
    error_->set_text(message);
    error_->set_visible(true);
  }

  void Block(const std::string& locationId) {
    if (!host_.IsLoggedIn() || locationId.empty()) return;
    error_->set_visible(false);
    urnet::NetworkBlockLocationArgs args{};
    args.location_id = locationId;
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.api().networkBlockLocation(
        std::optional<urnet::NetworkBlockLocationArgs>(args),
        [this, epoch, seen](std::optional<urnet::NetworkBlockLocationResult> result,
                            std::optional<std::string> err) {
          PostToMain([this, epoch, seen, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!result || err || result->error) {
              g_warning("settings: networkBlockLocation failed: %s",
                        err ? err->c_str()
                            : (result && result->error ? result->error->message.c_str()
                                                       : "(no result)"));
              ShowError(T_("blocked_location_could_not_be_added_please_try",
                           "Blocked location could not be added. Please try again "
                           "later."));
            }
            // Re-FETCH either way: the server owns the row's name and type; a
            // locally built row would be a guess.
            LoadBlocked();
          });
        });
  }

  void Unblock(const std::string& locationId) {
    if (!host_.IsLoggedIn() || locationId.empty()) return;
    error_->set_visible(false);
    urnet::NetworkUnblockLocationArgs args{};
    args.location_id = locationId;
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.api().networkUnblockLocation(
        std::optional<urnet::NetworkUnblockLocationArgs>(args),
        [this, epoch, seen](std::optional<urnet::NetworkUnblockLocationResult> result,
                            std::optional<std::string> err) {
          PostToMain([this, epoch, seen, result = std::move(result),
                      err = std::move(err)] {
            if (*epoch != seen) return;
            if (!result || err || result->error) {
              g_warning("settings: networkUnblockLocation failed: %s",
                        err ? err->c_str()
                            : (result && result->error ? result->error->message.c_str()
                                                       : "(no result)"));
              ShowError(T_("blocked_location_could_not_be_removed_please_try",
                           "Blocked location could not be removed. Please try again "
                           "later."));
            }
            LoadBlocked();
          });
        });
  }

  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Box* blockedPanel_ = nullptr;
  Gtk::Label* blockedEmpty_ = nullptr;
  Gtk::SearchEntry* search_ = nullptr;
  Gtk::Box* countryPanel_ = nullptr;
  Gtk::Label* error_ = nullptr;

  std::vector<urnet::BlockedLocation> blocked_;
  std::vector<Country> countries_;
  SettingsFieldState blockedState_ = SettingsFieldState::Loading;
  SettingsFieldState countriesState_ = SettingsFieldState::Loading;
  bool loadingCountries_ = false;
};

// =============================================================================
// SettingsPage
// =============================================================================

SettingsPage::SettingsPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  EnsureBrandCss();   // the pane-shell vocabulary
  EnsureDrawerCss();  // .ur-caption / .ur-label-faint / the card styles
  set_hexpand(true);
  set_vexpand(true);

  // THREE EQUAL COLUMNS. A size group pins the three pane roots to one
  // request; hexpand then splits the remainder evenly, so the panes stay equal
  // whatever their content measures. (At the 2062dip reference window each is
  // ~660dip — the "constrained settings column" measure the row metrics were
  // chosen for.)
  paneSizes_ = Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL);

  paneA_ = kit::MakePane(T_("general", "General"));
  paneA_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneA_.root, T_("general", "General"));
  BuildGeneralSection(*paneA_.content);
  BuildConnectionsSection(*paneA_.content);
  paneSizes_->add_widget(*paneA_.root);
  append(*paneA_.root);

  ruleB_ = kit::MakePaneVRule();
  append(*ruleB_);

  paneB_ = kit::MakePane(T_("device", "Device"));
  paneB_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneB_.root, T_("device", "Device"));
  BuildDeviceSection(*paneB_.content);
  BuildIdentitySection(*paneB_.content);
  BuildAdvancedSection(*paneB_.content);
  paneSizes_->add_widget(*paneB_.root);
  append(*paneB_.root);

  ruleC_ = kit::MakePaneVRule();
  append(*ruleC_);

  // key `about` does not exist in the store — the English renders until it does
  paneC_ = kit::MakePane(T_("about", "About"));
  paneC_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneC_.root, T_("about", "About"));
  BuildVersionSection(*paneC_.content);
  BuildStayInTouchSection(*paneC_.content);
  paneSizes_->add_widget(*paneC_.root);
  append(*paneC_.root);

  // Build-time state: no round trips, no session required. The advanced-mode
  // toggle seeds from the STANDING value (valid at any time, restored from
  // disk before any view exists) rather than waiting for a change that is
  // never coming.
  ApplyLocalDeviceState();
}

SettingsPage::~SettingsPage() {
  // orphan every in-flight completion: each checks the epoch before touching
  // the page or its sheets
  ++*epoch_;
}

// ---- load pipeline (§9) -----------------------------------------------------

void SettingsPage::Load() {
  ++*epoch_;  // drop every completion armed for the previous session

  // 1. Local state first: no round trips, correct with no session at all.
  ApplyLocalDeviceState();

  // 2. No session: EVERY server-backed field lands on NoSession and we return.
  //    Nothing may sit on a dash or an unresolving spinner. This branch is the
  //    Settings-owned half of windows ResetForSignOut — the account-subject
  //    fields it also clears live on the ACCOUNT destination.
  if (!host_.IsLoggedIn()) {
    deviceName_.clear();
    preferencesLoaded_ = false;
    ApplyFieldState(*deviceNameRow_.value, SettingsFieldState::NoSession);
    ApplyFieldState(*deviceSpecValue_, SettingsFieldState::NoSession);
    applyingPreference_ = true;  // echo guard: this write is not a user edit
    productUpdates_->set_active(false);
    applyingPreference_ = false;
    productUpdates_->set_sensitive(false);
    ApplyFieldState(*productUpdatesState_, SettingsFieldState::NoSession);
    productUpdatesStateRow_->set_visible(true);
    return;
  }

  // 3. Signed in: the two server reads this destination owns.
  LoadDeviceInfo();
  LoadPreferences();
}

void SettingsPage::ApplyLocalDeviceState() {
  // client id: "" with no device. Everything device-scoped keys off it.
  clientId_ = host_.ClientId();

  // Kill switch: readable with no session and no tunnel — the preference lives
  // in the app's LocalState. kill switch == !routeLocal; the inversion lives
  // in SdkHost, never here.
  applyingKillSwitch_ = true;
  killSwitch_->set_active(host_.CurrentKillSwitch());
  applyingKillSwitch_ = false;
  ApplyKillSwitchState();
  // ...and ask the daemon what is REALLY installed. The preference is local
  // and instant; the enforcement leg is a control-socket round trip, so it
  // lands through the completion below rather than blocking this build path.
  {
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.RefreshKillSwitchStatus([this, epoch, seen](KillSwitchStatus) {
      if (*epoch != seen) return;  // a newer presentation owns the page
      ApplyKillSwitchState();
    });
  }

  // Advanced mode: the standing value, replayed. The toggle only WRITES;
  // this is the read side (and MainWindow's handler calls SetAdvancedMode).
  applyingAdvancedMode_ = true;
  advancedMode_->set_active(host_.CurrentAdvancedMode());
  applyingAdvancedMode_ = false;
}

void SettingsPage::LoadPreferences() {
  ApplyFieldState(*productUpdatesState_, SettingsFieldState::Loading);
  productUpdatesStateRow_->set_visible(true);

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().accountPreferencesGet(
      [this, epoch, seen](std::optional<urnet::AccountPreferencesGetResult> result,
                          std::optional<std::string> err) {
        PostToMain([this, epoch, seen, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;  // a newer Load() owns the page
          if (!result || err) {
            // The toggle STAYS DISABLED: we do not know what the server holds,
            // and an enabled switch over an unknown value invites a write that
            // silently disagrees with the account.
            g_warning("settings: accountPreferencesGet failed: %s",
                      err ? err->c_str() : "(no result)");
            ApplyFieldState(*productUpdatesState_, SettingsFieldState::Failed);
            productUpdatesStateRow_->set_visible(true);
            return;
          }
          const bool allow =
              result->product_updates.has_value() && *result->product_updates;
          applyingPreference_ = true;  // echo guard around the programmatic write
          productUpdates_->set_active(allow);
          applyingPreference_ = false;
          preferencesLoaded_ = true;
          productUpdates_->set_sensitive(true);
          // The toggle itself is the state now — the line has nothing to add.
          productUpdatesState_->set_text("");
          productUpdatesStateRow_->set_visible(false);
        });
      });
}

void SettingsPage::LoadDeviceInfo() {
  if (clientId_.empty()) {
    // Signed in but the service is not up yet: "Attaching device controls…",
    // never "please login".
    const auto state = host_.IsLoggedIn() ? SettingsFieldState::NoDevice
                                          : SettingsFieldState::NoSession;
    ApplyFieldState(*deviceNameRow_.value, state);
    ApplyFieldState(*deviceSpecValue_, state);
    return;
  }
  ApplyFieldState(*deviceNameRow_.value, SettingsFieldState::Loading);
  ApplyFieldState(*deviceSpecValue_, SettingsFieldState::Loading);

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  const std::string clientId = clientId_;
  host_.api().getNetworkClients(
      [this, epoch, seen, clientId](std::optional<urnet::NetworkClientsResult> result,
                                    std::optional<std::string> err) {
        PostToMain([this, epoch, seen, clientId, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          const urnet::NetworkClientInfo* self = nullptr;
          if (result && !err && result->clients) {
            for (const auto& info : *result->clients) {
              if (info.client_id && *info.client_id == clientId) {
                self = &info;
                break;
              }
            }
          }
          if (!self) {
            // Two failures, one render: transport failure, and "the call
            // worked but this client is not in its OWN network's list" — the
            // second is not empty, it means something is wrong.
            g_warning("settings: getNetworkClients failed or this client is absent: %s",
                      err ? err->c_str() : "(client not in its own network list)");
            ApplyFieldState(*deviceNameRow_.value, SettingsFieldState::Failed);
            ApplyFieldState(*deviceSpecValue_, SettingsFieldState::Failed);
            return;
          }
          // device_name falls back to description (the client may never have
          // been named); an empty result is Empty ("None"), not blank.
          std::string name = self->device_name;
          if (name.empty()) name = self->description;
          deviceName_ = name;
          ApplyFieldState(*deviceNameRow_.value,
                          name.empty() ? SettingsFieldState::Empty
                                       : SettingsFieldState::Loaded,
                          name);
          ApplyFieldState(*deviceSpecValue_,
                          self->device_spec.empty() ? SettingsFieldState::Empty
                                                    : SettingsFieldState::Loaded,
                          self->device_spec);
        });
      });
}

// ---- D5 advanced mode (§8) --------------------------------------------------

void SettingsPage::SetAdvancedMode(bool on) {
  // The APPLY path: MainWindow's handler replays the standing value here — the
  // same path a disk-restored value takes. No-op when nothing changes, and the
  // write is guarded so it cannot echo back out through SdkHost as a user edit
  // (assigning `active` raises the property-changed signal).
  if (!advancedMode_ || advancedMode_->get_active() == on) return;
  applyingAdvancedMode_ = true;
  advancedMode_->set_active(on);
  applyingAdvancedMode_ = false;
}

void SettingsPage::OnAdvancedModeToggled() {
  if (applyingAdvancedMode_) return;  // re-entrancy / echo guard
  // THE ONE WRITER, AND IT ONLY WRITES. SetAdvancedMode persists FIRST and
  // publishes SECOND; the publish comes back through the host's handler into
  // SetAdvancedMode(bool) above, so toggle-now and on-at-launch cannot render
  // differently.
  host_.SetAdvancedMode(advancedMode_->get_active());
}

// ---- breakpoint (§1.1) ------------------------------------------------------

void SettingsPage::ApplyBreakpoint(int widthDip) {
  const int panes = widthDip >= 1400 ? 3 : (widthDip >= 900 ? 2 : 1);
  if (lastFold_ == panes) return;  // no-op unless the fold actually changes
  lastFold_ = panes;
  // A folded pane hides WITH its rule — a rule with nothing on its far side
  // reads as a broken layout.
  paneC_.root->set_visible(panes >= 3);
  ruleC_->set_visible(panes >= 3);
  paneB_.root->set_visible(panes >= 2);
  ruleB_->set_visible(panes >= 2);
}

// ---- Pane A: General (§3.1) -------------------------------------------------

void SettingsPage::BuildGeneralSection(Gtk::Box& host) {
  // NO group header: the pane title already says "General"; a 28px strip
  // repeating it reads as a stutter.

  // Row 1 — the ACCOUNT preference. Disabled until the current value is read:
  // a switch you can flip before anyone knows what it holds is a lie.
  productUpdates_ =
      AddToggleRow(host, T_("send_product_updates", "Send me product updates"), {});
  productUpdates_->set_sensitive(false);
  productUpdates_->property_active().signal_changed().connect(
      [this] { OnProductUpdatesToggled(); });

  auto state = MakeProseRow({}, kStatePadY);
  productUpdatesState_ = state.line;
  productUpdatesStateRow_ = state.root;
  ApplyFieldState(*productUpdatesState_, SettingsFieldState::NoSession);
  host.append(*state.root);

  // Row 2 — a LOCAL preference: no session gate, no FieldState, no echo guard
  // (this toggle is the only writer; nothing ever writes its state back).
  autoCheckUpdates_ = AddToggleRow(
      host, T_("upd_auto_check", "Check for updates automatically"),
      T_("upd_auto_check_note",
         "Look for new releases shortly after launch and every six hours. Nothing is "
         "ever installed without a click."));
  autoCheckUpdates_->set_active(prefs::Get<bool>(kAutoCheckKey, true));
  autoCheckUpdates_->property_active().signal_changed().connect([this] {
    // Persisted immediately (whole-file read-modify-write, so it cannot
    // clobber advanced_mode beside it).
    prefs::Set(kAutoCheckKey, autoCheckUpdates_->get_active());
    // TODO(sdk-wiring): urnw::UpdateChecker::SetAutoCheckEnabled — turning the
    // preference ON must fire a check immediately ("answer now, not in six
    // hours"); the checker itself (30s launch delay, 6h cadence) does not
    // exist in this tree yet, so the preference is recorded and nothing is
    // scheduled.
  });
}

// ---- Pane A: Connections (§3.2) --------------------------------------------

void SettingsPage::BuildConnectionsSection(Gtk::Box& host) {
  host.append(*kit::MakePaneGroupHeader(T_("site_app_connections", "Connections")).root);

  // Row 1 — the kill switch. The shipped store key site_app_kill_switch_note
  // is DELIBERATELY not used: it is wrong twice (this is not browser-only, and
  // "when disconnected" describes a bug as a feature — the switch guards only
  // UNEXPECTED loss).
  killSwitch_ = AddToggleRow(
      host, T_("kill_switch", "Kill switch"),
      T_("adv_kill_switch_note",
         "If the tunnel drops unexpectedly, block this device's traffic instead of "
         "letting it out unprotected."));
  killSwitch_->property_active().signal_changed().connect(
      [this] { OnKillSwitchToggled(); });

  // Row 1b — WHAT IS ACTUALLY IN FORCE, as opposed to what the switch asks
  // for. The two are different facts: the daemon can refuse to install the
  // nftables floor, can be absent altogether, or can deliberately hold off
  // while nothing is connected. Collapsing them into the switch position is
  // exactly the defect this pane used to ship — a toggle reading "on" over a
  // machine with no ruleset installed anywhere.
  auto killState = MakeProseRow({}, kStatePadY);
  killSwitchState_ = killState.line;
  killSwitchStateRow_ = killState.root;
  killSwitchStateRow_->set_visible(false);  // nothing to say while it is off
  host.append(*killState.root);

  // Rows 2-3 — the two honesty disclosures. They are prose, not notes: a
  // trimmed 11px line could not carry either sentence.
  host.append(*MakeProseRow(
                   T_("adv_kill_switch_deliberate",
                      "Pressing Disconnect always restores your internet straight away. "
                      "The kill switch only applies to drops you did not ask for - a "
                      "tunnel that stops carrying traffic, a network change, or the "
                      "URnetwork service stopping."),
                   kProsePadY)
                   .root);
  host.append(*MakeProseRow(
                   T_("adv_kill_switch_dns_window",
                      "While it is on and nothing is connecting, nothing leaves this "
                      "device - name lookups included. During a connection attempt, name "
                      "lookups from any app on this device can leave in the clear so "
                      "URnetwork can reach its servers; everything else stays blocked."),
                   kProsePadY)
                   .root);

  // Row 4 — blocked locations (the Network destination is a second door).
  auto blockedRow =
      kit::MakePaneTwoLineRowButton(T_("blocked_locations_2", "Blocked locations"), {},
                                    kRowTall);
  blockedRow.root->signal_clicked().connect([this] { ShowBlockedLocationsSheet(); });
  host.append(*blockedRow.root);

  // Row 5 — app split rules.
  auto* manage =
      AddButtonRow(host, T_("app_split_rules", "App split rules"),
                   T_("apps_listed_bypass_vpn", "Apps listed here bypass the VPN."),
                   T_("manage_apps", "Manage apps"));
  manage->signal_clicked().connect([this] { ShowAppSplitRulesSheet(); });

  // Row 6 — the VPN service row, wrapped in its own host so VISIBILITY moves
  // the WHOLE row: ButtonRow hands back only the button, and hiding the button
  // alone leaves a caption pointing at nothing.
  serviceRowHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  serviceUninstall_ =
      AddButtonRow(*serviceRowHost_, T_("svc_service_label", "VPN service"),
                   // reworded for the systemd unit (the windows string says
                   // "the Windows service")
                   T_("svc_uninstall_note",
                      "Remove the systemd service URnetwork uses to carry traffic."),
                   T_("svc_uninstall_action", "Uninstall"));
  serviceUninstall_->signal_clicked().connect([this] { ConfirmUninstallService(); });
  // TODO(sdk-wiring): ServiceSetup::Classify / AwaitState — the systemd twin of
  // the windows service classifier {Running, Stopped, VersionMismatch,
  // NotInstalled, ConsoleMode, Unknown} + busy. It is the ONE writer of this
  // row's visibility (visible iff the unit is registered in any form; HIDDEN,
  // not disabled, otherwise — an affordance for removing what is not there is
  // noise) and of the button's sensitivity while an elevated verb runs. With
  // no classifier the row renders unconditionally rather than guessing.
  host.append(*serviceRowHost_);
}

// ---- Pane B: Device (§4.1) --------------------------------------------------

void SettingsPage::BuildDeviceSection(Gtk::Box& host) {
  // NO group header — the pane title already says "Device".
  deviceNameRow_ =
      kit::MakePaneTwoLineRowButton(T_("device_name_label", "Name"), {}, kRowTall);
  deviceNameRow_.root->signal_clicked().connect([this] { ShowDeviceNameSheet(); });
  host.append(*deviceNameRow_.root);

  // Server-assigned, read-only: a spec is reported, never chosen.
  deviceSpecValue_ = AddValueRow(host, T_("device_spec_label", "Spec"));

  // Both start at NoSession — the state the app OPENS in.
  ApplyFieldState(*deviceNameRow_.value, SettingsFieldState::NoSession);
  ApplyFieldState(*deviceSpecValue_, SettingsFieldState::NoSession);
}

// ---- Pane B: Post Quantum Identity (§4.2) -----------------------------------

void SettingsPage::BuildIdentitySection(Gtk::Box& host) {
  host.append(
      *kit::MakePaneGroupHeader(T_("post_quantum_identity", "Post Quantum Identity")).root);

  auto row = kit::MakePaneTwoLineRowButton(T_("provider_identities", "Provider Identities"),
                                           {}, kRowTall);
  row.root->signal_clicked().connect([this] { ShowIdentitySheet(); });
  host.append(*row.root);

  host.append(
      *MakeProseRow(
           T_("post_quantum_identity_explanation",
              "Your identity key is stored locally on this device. If any peer's key "
              "appears different than their locally stored key, it means the network "
              "operator cannot be trusted."),
           kProsePadY)
           .root);
}

// ---- Pane B: Advanced (§4.3) ------------------------------------------------

void SettingsPage::BuildAdvancedSection(Gtk::Box& host) {
  // key `advanced` is not in the store — the English renders until it is
  host.append(*kit::MakePaneGroupHeader(T_("advanced", "Advanced")).root);

  // FIRST in the group: the D5 drop point.
  advancedMode_ = AddToggleRow(
      host, T_("adv_advanced_mode", "Advanced mode"),
      T_("adv_advanced_mode_note",
         "Show raw values, identifiers, the connection inspector and the reliability "
         "tuning surface across the app."));
  advancedMode_->set_active(host_.CurrentAdvancedMode());
  advancedMode_->property_active().signal_changed().connect(
      [this] { OnAdvancedModeToggled(); });

  auto* save = AddButtonRow(host, T_("save_logs", "Save logs"),
                            T_("export_logs", "Export Logs"), T_("save", "Save"));
  save->signal_clicked().connect([this] { SaveLogsToFile(); });

  // There is deliberately NO "share/upload logs" row here: log upload happens
  // ONLY inside the feedback flow (Support destination), keyed by the SERVER's
  // feedback id — a client-minted id correlates with nothing.
}

// ---- Pane C: Version (§5.1) -------------------------------------------------

void SettingsPage::BuildVersionSection(Gtk::Box& host) {
  // No header: the pane title already says "About".
  auto* sdkValue = AddValueRow(host, T_("version_info", "Version and build info"));
  std::string sdkVersion;
  try {
    sdkVersion = urnet::version();
  } catch (const std::exception& e) {
    g_warning("settings: urnet::version threw: %s", e.what());
  }
  if (sdkVersion.empty()) {
    // TODO(sdk-wiring): SdkHost::appVersion — the constant this app reports to
    // the SDK/server, which windows falls back to when urnet::version() is
    // empty (it IS empty in the current SDK build). No such accessor exists on
    // this host, so the build stamp stands in rather than a blank or a dash.
    sdkVersion = UR_APP_VERSION;
  }
  ApplyFieldState(*sdkValue, SettingsFieldState::Loaded, sdkVersion);

  // key `app_version` is not in the store. The value is a compile-time
  // constant — no round trip, and "0.0.0" outside a release build is the
  // correct answer, not a bug.
  auto* appValue = AddValueRow(host, T_("app_version", "App version"));
  ApplyFieldState(*appValue, SettingsFieldState::Loaded, UR_APP_VERSION);
}

// ---- Pane C: Stay in touch (§5.2) -------------------------------------------

void SettingsPage::BuildStayInTouchSection(Gtk::Box& host) {
  host.append(*kit::MakePaneGroupHeader(T_("stay_in_touch", "Stay in touch")).root);

  // The store's own markdown sentence, rendered with the link INLINE and
  // clickable (never a label beside a button): the sentence IS the affordance.
  // `canonical` is what the row must still open if a translation loses the
  // [text](url) span — a community line that stops being clickable is a dead
  // row, so the whole sentence becomes the link instead.
  auto link = [&host](const char* markdown, const char* canonical) {
    auto row = MakeProseRow({}, kProsePadY);
    row.line->remove_css_class("ur-caption");
    row.line->add_css_class("ur-row-title");  // 13px, the row voice
    const std::string pango = MarkdownLinksToPango(markdown);
    if (pango.find("<a ") != std::string::npos) {
      row.line->set_markup(pango);
    } else {
      row.line->set_markup(Glib::ustring("<a href=\"") + canonical + "\">" +
                           Glib::Markup::escape_text(markdown) + "</a>");
    }
    host.append(*row.root);
  };
  link(T_("join_the_community_on_discord_https_discord_com",
          "Join the community on [Discord](https://discord.com/invite/RUNZXMwPRK)"),
       kDiscordUrl);
  link(T_("verified_project_on_depin_hub_https_depinhub_io",
          "Verified project on [DePIN Hub](https://depinhub.io/projects/urnetwork)"),
       kDepinHubUrl);

  // The protocol link rides a fixed 40px row (the single-line pane species).
  auto* protocolRow = kit::MakePaneRow(kRowSingle);
  auto* protocol = Gtk::make_managed<Gtk::Label>();
  protocol->add_css_class("ur-row-title");
  protocol->set_xalign(0);
  protocol->set_halign(Gtk::Align::START);
  protocol->set_valign(Gtk::Align::CENTER);
  protocol->set_markup(
      Glib::ustring("<a href=\"") + kProtocolUrl + "\">" +
      Glib::Markup::escape_text(T_("uses_ur_protocol", "Uses the UR Protocol")) + "</a>");
  if (auto* inner = dynamic_cast<Gtk::Box*>(protocolRow->get_first_child())) {
    inner->append(*protocol);
  }
  host.append(*protocolRow);
}

// ---- handlers ---------------------------------------------------------------

void SettingsPage::OnProductUpdatesToggled() {
  // echo guard + not-yet-loaded guard: a load writes this switch, and a switch
  // nobody has read the value for must not push one.
  if (applyingPreference_ || !preferencesLoaded_) return;

  const bool allow = productUpdates_->get_active();
  productUpdates_->set_sensitive(false);  // in-flight: no second write

  urnet::AccountPreferencesSetArgs args{};
  args.product_updates = allow;

  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().accountPreferencesUpdate(
      std::optional<urnet::AccountPreferencesSetArgs>(args),
      [this, epoch, seen, allow](std::optional<urnet::AccountPreferencesSetResult> result,
                                 std::optional<std::string> err) {
        PostToMain([this, epoch, seen, allow, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;
          productUpdates_->set_sensitive(true);  // on EVERY path
          // AccountPreferencesSetResult carries no error field: success is a
          // result AND no transport error. Never report success blindly.
          if (result.has_value() && !err.has_value()) return;
          g_warning("settings: accountPreferencesUpdate failed: %s",
                    err ? err->c_str() : "(no result)");
          // Snap the switch back to what the server still holds.
          applyingPreference_ = true;
          productUpdates_->set_active(!allow);
          applyingPreference_ = false;
          Snack(T_("couldnt_update_preferences",
                   "Couldn't update your preferences. Please try again."),
                true);
        });
      });
}

// ONE writer for the state line under the switch. It never touches the switch
// itself — the switch is the REQUEST, this line is what is in force, and the
// whole point of the pair is that they can legitimately disagree.
void SettingsPage::ApplyKillSwitchState() {
  if (killSwitchState_ == nullptr || killSwitchStateRow_ == nullptr) return;
  const KillSwitchCopy copy = KillSwitchStateLine(host_.CurrentKillSwitchStatus());
  killSwitchStateRow_->set_visible(!copy.line.empty());
  if (copy.line.empty()) return;
  SetToned(*killSwitchState_, copy.attention ? kUrDanger : kUrTextMuted, copy.line);
}

void SettingsPage::OnKillSwitchToggled() {
  if (applyingKillSwitch_) return;  // echo guard

  const bool wanted = killSwitch_->get_active();
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // THREE legs now, not one. SdkHost writes LocalState and the device
  // synchronously and then drives urnetworkd's nftables ruleset — the leg that
  // makes this a kill switch rather than a preference — on a worker, because
  // it is a control-socket round trip and this handler runs on the GTK loop.
  host_.SetKillSwitch(wanted, [this, epoch, seen, wanted](KillSwitchStatus status) {
    if (*epoch != seen) return;  // a newer presentation owns the page
    // READ BACK rather than trust the write, on every leg. The soft legs are
    // already reflected in CurrentKillSwitch(); `status` carries what the
    // daemon says it really installed.
    const bool actual = host_.CurrentKillSwitch();
    if (actual != wanted) {
      // Even the local preference did not take. THAT is the case where the
      // switch must snap back — it is showing a request that does not exist.
      g_warning("settings: kill switch preference did not take (wanted %d, actual %d)",
                static_cast<int>(wanted), static_cast<int>(actual));
      applyingKillSwitch_ = true;
      killSwitch_->set_active(actual);
      applyingKillSwitch_ = false;
    }
    ApplyKillSwitchState();
    // A FAILED enforcement leg is made VISIBLE, not reverted. Flipping the
    // switch back would hide the failure and would also be a lie in the other
    // direction: the preference IS recorded and will be re-attempted at the
    // next connection. The state line above says what is in force; the snack
    // makes sure a user who is looking elsewhere still finds out.
    const KillSwitchCopy copy = KillSwitchStateLine(status);
    if (copy.attention) Snack(copy.line, true);
  });
  // Say something immediately — the round trip can take a moment and a control
  // that appears to do nothing is how the old wiring read.
  ApplyKillSwitchState();
}

void SettingsPage::SaveLogsToFile() {
  // TODO(sdk-wiring): urnw::LogFilePath — the app's own log file (windows
  // LogFilePath). The SDK exposes only the DIRECTORY (urnet::getLogDir), so
  // the newest regular file in it stands in; a real path accessor removes this
  // guess.
  std::string logDir;
  try {
    logDir = urnet::getLogDir();
  } catch (const std::exception& e) {
    g_warning("settings: urnet::getLogDir threw: %s", e.what());
  }
  std::string source;
  if (!logDir.empty()) {
    GDir* dir = g_dir_open(logDir.c_str(), 0, nullptr);
    if (dir != nullptr) {
      gint64 newest = -1;
      while (const char* name = g_dir_read_name(dir)) {
        const std::string candidate = logDir + "/" + name;
        if (!g_file_test(candidate.c_str(), G_FILE_TEST_IS_REGULAR)) continue;
        GStatBuf info{};
        if (g_stat(candidate.c_str(), &info) != 0) continue;
        if (static_cast<gint64>(info.st_mtime) > newest) {
          newest = static_cast<gint64>(info.st_mtime);
          source = candidate;
        }
      }
      g_dir_close(dir);
    }
  }
  if (source.empty()) {
    // Warning severity: it persists (the user asked for a file and there is
    // none — that is a diagnostic, not a status).
    Snack(T_("no_log_files_found", "No log file found"), true);
    return;
  }

  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("settings: save logs dropped (page has no window yet)");
    return;
  }
  // The suggested name is the log file's STEM plus the .log extension — the
  // user saves the file they were shown, not a renamed copy.
  gchar* base = g_path_get_basename(source.c_str());
  std::string suggested = base ? base : "urnetwork.log";
  g_free(base);
  const auto dot = suggested.find_last_of('.');
  if (dot != std::string::npos) suggested = suggested.substr(0, dot);
  suggested += ".log";

  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, T_("export_logs", "Export Logs"));
  gtk_file_dialog_set_initial_name(dialog, suggested.c_str());
  // The completion carries the epoch (a shared_ptr, so it outlives the page)
  // and is dropped when a newer Load() or the destructor has bumped it — the
  // same stale-async guard every SDK completion on this page uses.
  struct SavePayload {
    SettingsPage* page;
    std::shared_ptr<uint64_t> epoch;
    uint64_t seen;
    std::string source;
  };
  auto* payload = new SavePayload{this, epoch_, *epoch_, source};
  gtk_file_dialog_save(
      dialog, GTK_WINDOW(root->gobj()), nullptr,
      +[](GObject* dialogSource, GAsyncResult* result, gpointer data) {
        std::unique_ptr<SavePayload> owned(static_cast<SavePayload*>(data));
        GFile* target =
            gtk_file_dialog_save_finish(GTK_FILE_DIALOG(dialogSource), result, nullptr);
        if (target == nullptr) return;  // dismissed: a cancel is never a failure
        if (*owned->epoch != owned->seen) {  // the page moved on (or is gone)
          g_object_unref(target);
          return;
        }
        GFile* from = g_file_new_for_path(owned->source.c_str());
        GError* error = nullptr;
        const bool ok = g_file_copy(from, target, G_FILE_COPY_OVERWRITE, nullptr, nullptr,
                                    nullptr, &error) != FALSE;
        if (!ok) {
          g_warning("settings: save logs failed: %s",
                    error ? error->message : "(no error text)");
          owned->page->Snack(T_("something_went_wrong", "Something went wrong."), true);
        } else {
          owned->page->Snack(T_("save_logs", "Save logs"), false);
        }
        g_clear_error(&error);
        g_object_unref(from);
        g_object_unref(target);
      },
      payload);
  g_object_unref(dialog);
}

void SettingsPage::ConfirmUninstallService() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) return;
  if (confirmDialog_ && confirmDialog_->get_visible()) return;  // one sheet at a time

  confirmDialog_ = std::make_unique<Gtk::Window>();
  confirmDialog_->set_transient_for(*root);
  confirmDialog_->set_modal(true);
  confirmDialog_->set_title(T_("svc_service_label", "VPN service"));
  confirmDialog_->set_resizable(false);
  confirmDialog_->add_css_class("ur-sheet");
  AddEscapeToClose(*confirmDialog_);

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(320, -1);

  auto* heading =
      Gtk::make_managed<Gtk::Label>(T_("svc_service_label", "VPN service"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  box->append(*heading);

  // The body says the two costs up front, so the elevation prompt is expected
  // (reworded from the windows string for systemd/polkit).
  auto* body = Gtk::make_managed<Gtk::Label>(
      T_("svc_uninstall_confirm",
         "This stops the URnetwork service and removes its systemd unit. The app can't "
         "connect until it is set up again. You will be asked for administrator "
         "permission."));
  body->add_css_class("ur-body");
  body->set_xalign(0);
  body->set_wrap(true);
  box->append(*body);

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { confirmDialog_->set_visible(false); });
  actions->append(*cancel);
  auto* uninstall =
      Gtk::make_managed<Gtk::Button>(T_("svc_uninstall_action", "Uninstall"));
  uninstall->add_css_class("destructive-action");
  uninstall->signal_clicked().connect([this] {
    confirmDialog_->set_visible(false);
    // TODO(sdk-wiring): ServiceSetup::RunElevatedVerb("uninstall", 30000) —
    // the Linux analogue is a privileged unit removal (pkexec urnetworkd
    // uninstall -> systemctl stop/disable + unit file removal), followed by
    // AwaitState(NotInstalled, 5000) and a fresh Classify() to republish the
    // snapshot. A DECLINED elevation must stay SILENT (the user changed their
    // mind); every other outcome is a failure. Nothing can launch the verb in
    // this tree yet, which is exactly the spec's `!launched` branch — log it
    // and say so rather than pretend the service went away.
    g_warning("settings: service uninstall unavailable (no elevated verb wired)");
    Snack(T_("something_went_wrong", "Something went wrong."), true);
  });
  actions->append(*uninstall);
  box->append(*actions);

  confirmDialog_->set_child(*box);
  // DEFAULT IS CANCEL: Enter must never remove the service.
  cancel->set_receives_default(true);
  confirmDialog_->set_default_widget(*cancel);
  confirmDialog_->present();
}

// ---- feed slots -------------------------------------------------------------

void SettingsPage::OnRouteLocalEvent() {
  // The kill switch changed elsewhere — the connect surface owns a second
  // switch, and SdkHost also fires this after every enforcement-leg round
  // trip. Re-read BOTH halves under the echo guard: the switch shows the
  // request that IS, the line under it shows what is in force.
  if (killSwitch_ == nullptr) return;
  applyingKillSwitch_ = true;
  killSwitch_->set_active(host_.CurrentKillSwitch());
  applyingKillSwitch_ = false;
  ApplyKillSwitchState();
}

void SettingsPage::OnProviderIdentitiesEvent() {
  if (identitiesSheet_ && identitiesSheet_->get_visible()) identitiesSheet_->Refresh();
}

// ---- sheets -----------------------------------------------------------------

Gtk::Window* SettingsPage::RootWindow() {
  return dynamic_cast<Gtk::Window*>(get_root());
}

void SettingsPage::ShowDeviceNameSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) return;
  if (!deviceNameSheet_) {
    deviceNameSheet_ = std::make_unique<SettingsDeviceNameSheet>(*root, host_);
    deviceNameSheet_->on_saved = [this](const std::string& name) {
      // One writer per surface: the page owns the cached name and the row.
      deviceName_ = name;
      ApplyFieldState(*deviceNameRow_.value, SettingsFieldState::Loaded, name);
      Snack(T_("device_name_updated", "Device name updated"), false);
    };
  }
  deviceNameSheet_->Open(deviceName_);
}

void SettingsPage::ShowBlockedLocationsSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) return;
  if (!blockedSheet_) {
    blockedSheet_ = std::make_unique<SettingsBlockedLocationsSheet>(*root, host_);
  }
  blockedSheet_->Open();
}

void SettingsPage::ShowAppSplitRulesSheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) return;
  // REUSED AS-IS (docs/parity/linux-reuse.md §2.13): the host-based split-rule
  // editor already in this tree.
  // TODO(sdk-wiring): SdkHost::CurrentAppRules / SetAppRule(imagePath,
  // includeInTunnel) / RemoveAppRule + urnw::EnumerateInstalledApps — the
  // windows sheet is PER-APP (a BlockActionOverride whose AppIds carry an
  // executable path, with include-precedence over exclusions) and its Linux
  // enumeration is a .desktop scan resolving Exec to the binary the
  // cgroup/fwmark split-tunnel driver matches on. Neither the host accessors
  // nor the enumeration exist here, so the row opens the host-rule sheet
  // rather than fabricating an app list.
  if (!splitRulesSheet_) {
    splitRulesSheet_ = std::make_unique<SplitRulesSheet>(*root, host_);
  }
  splitRulesSheet_->Open();
}

void SettingsPage::ShowIdentitySheet() {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) return;
  // REUSED AS-IS (linux-reuse §2.9): the provider-identities list, identicons
  // and share sheet are already the canonical SDK rasters.
  if (!identitiesSheet_) {
    identitiesSheet_ = std::make_unique<ProviderIdentitiesSheet>(*root, host_);
  }
  identitiesSheet_->Open();
}

// ---- snackbar ---------------------------------------------------------------

void SettingsPage::Snack(const Glib::ustring& message, bool error) {
  if (on_snackbar) {
    on_snackbar(message, error);
  } else {
    g_warning("settings: snackbar unbound; dropping message: %s", message.c_str());
  }
}

}  // namespace urnw
