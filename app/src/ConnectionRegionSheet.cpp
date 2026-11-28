// SPDX-License-Identifier: MPL-2.0
#include "ConnectionRegionSheet.hpp"

#include <libintl.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "I18n.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

constexpr int kSheetWidth = 400;
constexpr const char* kIsoCodesJson = "/usr/share/iso-codes/json/iso_3166-1.json";
constexpr const char* kIsoCodesDomain = "iso_3166-1";

// alpha-2 (lower case) -> the English name iso-codes translates.
const std::map<std::string, std::string>& IsoCountryNames() {
  static std::map<std::string, std::string> names;
  static std::once_flag once;
  std::call_once(once, [] {
    std::ifstream in(kIsoCodesJson);
    if (!in) return;
    try {
      const nlohmann::json doc = nlohmann::json::parse(in);
      for (const auto& entry : doc.at("3166-1")) {
        std::string code = region::Lowercased(entry.value("alpha_2", std::string()));
        // the short common name where there is one (Iran, not "Iran, Islamic
        // Republic of"); iso-codes translates both
        std::string name = entry.value("common_name", entry.value("name", std::string()));
        if (!code.empty() && !name.empty()) names.emplace(std::move(code), std::move(name));
      }
    } catch (const std::exception& e) {
      g_warning("connection region: %s unreadable: %s", kIsoCodesJson, e.what());
    }
    bind_textdomain_codeset(kIsoCodesDomain, "UTF-8");
  });
  return names;
}

std::string Joined(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) {
    if (!out.empty()) out += "\n";
    out += line;
  }
  return out;
}

}  // namespace

std::string CountryDisplayName(const std::string& countryCode) {
  const auto& names = IsoCountryNames();
  const auto it = names.find(region::Lowercased(countryCode));
  if (it == names.end()) return std::string();
  return dgettext(kIsoCodesDomain, it->second.c_str());
}

std::string LocationDisplayName(const urnet::ConnectLocation& location) {
  return region::LocationDisplayName(
      location.location_type && *location.location_type == urnet::LocationTypeCountry,
      location.country_code.value_or(std::string()), location.name.value_or(std::string()),
      CountryDisplayName);
}

std::vector<region::Option> ConnectionRegionOptions() {
  std::vector<region::Option> options = region::Options(
      SdkHost::IngressCountryCodes(),
      [](const std::string& code) {
        const std::string name = CountryDisplayName(code);
        if (!name.empty()) return name;
        std::string upper = code;  // no iso-codes: the code itself
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return upper;
      },
      T_("connection_region_undisclosed", "Other / Undisclosed"));
  return options;
}

std::string ConnectionRegionName(SdkHost& host) {
  SdkHostRegionStore store(host);
  region::Picker picker(store, ConnectionRegionOptions());
  return picker.Selected().name;
}

ConnectionRegionSheet::ConnectionRegionSheet(Gtk::Window& parent, SdkHost& host)
    : store_(host), picker_(store_, ConnectionRegionOptions()) {
  EnsureBrandCss();
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("where_are_you_connecting_from", "Where are you connecting from?"));
  set_default_size(kSheetWidth, -1);
  set_hide_on_close(true);
  add_css_class("ur-sheet");
  AddEscapeToClose(*this);

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  auto* heading = Gtk::make_managed<Gtk::Label>(
      T_("where_are_you_connecting_from", "Where are you connecting from?"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  heading->set_wrap(true);
  box->append(*heading);
  box->append(*MakeCaption(T_("connection_region_description",
                              "Only affects how the app reaches URnetwork, not your traffic.")));

  auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  Gtk::CheckButton* group = nullptr;
  for (size_t i = 0; i < picker_.options().size(); ++i) {
    auto* row = Gtk::make_managed<Gtk::CheckButton>(picker_.options()[i].name);
    if (group) row->set_group(*group); else group = row;
    row->signal_toggled().connect([this, row, i] {
      if (syncing_ || !row->get_active()) return;
      const std::string stored = picker_.Select(i);
      Sync();
      if (on_changed) on_changed(stored);
    });
    rows_.push_back(row);
    list->append(*row);
  }
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(320);
  scroller->set_child(*list);
  box->append(*scroller);

  resolvers_ = Gtk::make_managed<Gtk::Label>();
  resolvers_->add_css_class("ur-caption");
  resolvers_->set_xalign(0);
  resolvers_->set_wrap(true);
  resolvers_->set_selectable(true);
  box->append(*resolvers_);

  set_child(*box);
}

void ConnectionRegionSheet::Open() {
  Sync();
  present();
}

void ConnectionRegionSheet::Sync() {
  syncing_ = true;
  const size_t selected = picker_.SelectedIndex();
  if (selected < rows_.size()) rows_[selected]->set_active(true);
  syncing_ = false;
  ShowResolvers(picker_.options().empty() ? std::string() : picker_.options()[selected].code);
}

void ConnectionRegionSheet::ShowResolvers(const std::string& countryCode) {
  region::Resolvers resolvers;
  if (!countryCode.empty()) {
    resolvers.encrypted = SdkHost::IngressDohUrls(countryCode);
    resolvers.fallback = SdkHost::IngressPlainDnsServers(countryCode);
  }
  if (resolvers.Default()) {
    resolvers_->set_text(T_("connection_region_default_resolvers", "Default resolvers"));
    return;
  }
  std::string text;
  if (!resolvers.encrypted.empty()) {
    text += std::string(T_("connection_region_encrypted_resolvers", "Encrypted resolvers")) + "\n" +
            Joined(resolvers.encrypted);
  }
  if (!resolvers.fallback.empty()) {
    if (!text.empty()) text += "\n\n";
    text += std::string(T_("connection_region_fallback_resolvers",
                           "Unencrypted fallback (URnetwork names only)")) +
            "\n" + Joined(resolvers.fallback);
  }
  resolvers_->set_text(text);
}

}  // namespace urnw
