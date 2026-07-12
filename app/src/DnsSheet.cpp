// SPDX-License-Identifier: MPL-2.0
#include "DnsSheet.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

std::string Lowercased(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Uppercased(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

bool ListsEqual(const std::optional<urnet::StringList>& a,
                const std::optional<urnet::StringList>& b) {
  static const urnet::StringList kEmpty;
  const urnet::StringList& av = a ? *a : kEmpty;
  const urnet::StringList& bv = b ? *b : kEmpty;
  return av == bv;
}

}  // namespace

void DnsSheet::Normalize(urnet::DnsResolverSettings& settings) {
  // engage every list so the editor can mutate in place
  auto engage = [](std::optional<urnet::StringList>& list) {
    if (!list) list = urnet::StringList();
  };
  engage(settings.RemoteDohUrlsIpv4);
  engage(settings.RemoteDohUrlsIpv6);
  engage(settings.LocalDohUrlsIpv4);
  engage(settings.LocalDohUrlsIpv6);
  engage(settings.RemoteDnsIpv4);
  engage(settings.RemoteDnsIpv6);
  engage(settings.LocalDnsIpv4);
  engage(settings.LocalDnsIpv6);
}

bool DnsSheet::SettingsEqual(const urnet::DnsResolverSettings& a,
                             const urnet::DnsResolverSettings& b) {
  return a.EnableRemoteDoh == b.EnableRemoteDoh && a.EnableLocalDoh == b.EnableLocalDoh &&
         a.EnableRemoteDns == b.EnableRemoteDns && a.EnableLocalDns == b.EnableLocalDns &&
         a.EnableFallback == b.EnableFallback &&
         ListsEqual(a.RemoteDohUrlsIpv4, b.RemoteDohUrlsIpv4) &&
         ListsEqual(a.RemoteDohUrlsIpv6, b.RemoteDohUrlsIpv6) &&
         ListsEqual(a.LocalDohUrlsIpv4, b.LocalDohUrlsIpv4) &&
         ListsEqual(a.LocalDohUrlsIpv6, b.LocalDohUrlsIpv6) &&
         ListsEqual(a.RemoteDnsIpv4, b.RemoteDnsIpv4) &&
         ListsEqual(a.RemoteDnsIpv6, b.RemoteDnsIpv6) &&
         ListsEqual(a.LocalDnsIpv4, b.LocalDnsIpv4) && ListsEqual(a.LocalDnsIpv6, b.LocalDnsIpv6);
}

std::string DnsSheet::CountryDotMarkup(const std::string& countryCode) const {
  // SDK color hexes are 6 digits without '#'; fall back to a muted dot
  const Rgba fallback{0.5, 0.5, 0.5, 1.0};
  const Rgba color =
      countryCode.empty() ? fallback : ParseHexColor(urnet::getColorHex(countryCode), fallback);
  return "<span foreground='" + HexForMarkup(color) + "'>●</span>";
}

DnsSheet::DnsSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_("custom_dns", "Custom DNS"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 540);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  // keep every draft list engaged so the field handlers can never dereference
  // an empty optional, even before the first Open
  Normalize(draft_);
  Normalize(original_);
  BuildUi();
}

bool DnsSheet::Open() {
  auto settings = host_.GetDnsResolverSettings();
  if (!settings) return false;  // the card shows "DNS settings unavailable"
  Normalize(*settings);
  draft_ = *settings;
  original_ = *settings;

  countryCode_.clear();
  countryName_.clear();
  if (auto location = host_.SelectedLocation()) {
    if (location->country_code) countryCode_ = Lowercased(*location->country_code);
    if (location->country) countryName_ = *location->country;
  }
  recommendation_.reset();
  if (!countryCode_.empty()) {
    recommendation_ = urnet::getRecommendedDnsResolverSettings(countryCode_);
    if (recommendation_) Normalize(*recommendation_);
  }
  defaults_ = urnet::getDefaultDnsResolverSettings();
  if (defaults_) Normalize(*defaults_);

  BuildSuggestions();
  SyncFromDraft();
  present();
  return true;
}

void DnsSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  set_child(*root);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_vexpand(true);
  root->append(*scroller);

  auto* form = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  form->set_margin(16);
  scroller->set_child(*form);

  // recommendation / most-secure-default panel (content rebuilt per state)
  panelBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  panelBox_->add_css_class("ur-banner");
  form->append(*panelBox_);

  // resolvers
  form->append(*MakeCaption(T_("resolvers", "Resolvers")));
  auto* resolvers = MakeCard(10);
  AddSwitchRow(*resolvers, T_("dns_over_https", "DNS over HTTPS"),
               T_("remote_lowercase", "remote"), remoteDohSwitch_,
               &urnet::DnsResolverSettings::EnableRemoteDoh);
  AddSwitchRow(*resolvers, T_("dns_over_https", "DNS over HTTPS"), T_("local_lowercase", "local"),
               localDohSwitch_, &urnet::DnsResolverSettings::EnableLocalDoh);
  AddSwitchRow(*resolvers, T_("unencrypted_dns", "Unencrypted DNS"),
               T_("remote_lowercase", "remote"), remoteDnsSwitch_,
               &urnet::DnsResolverSettings::EnableRemoteDns);
  AddSwitchRow(*resolvers, T_("unencrypted_dns", "Unencrypted DNS"),
               T_("local_lowercase", "local"), localDnsSwitch_,
               &urnet::DnsResolverSettings::EnableLocalDns);
  form->append(*resolvers);

  // local dns fallback
  auto* fallbackCard = MakeCard(10);
  AddSwitchRow(*fallbackCard, T_("local_dns_fallback", "Local DNS fallback"), "", fallbackSwitch_,
               &urnet::DnsResolverSettings::EnableFallback);
  form->append(*fallbackCard);
  auto* fallbackFooter = Gtk::make_managed<Gtk::Label>(
      T_("local_dns_fallback_description",
         "Races a local resolver while the tunnel starts. When off, DNS only resolves through "
         "the tunnel."));
  fallbackFooter->add_css_class("dim-label");
  fallbackFooter->add_css_class("caption");
  fallbackFooter->set_wrap(true);
  fallbackFooter->set_xalign(0);
  form->append(*fallbackFooter);

  // well-known regional server suggestions (rows rebuilt per open)
  suggestionsSection_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  suggestionsSection_->append(
      *MakeCaption(T_("suggested_remote_dns_servers", "Suggested remote DNS servers")));
  suggestionsBox_ = MakeCard(10);
  suggestionsSection_->append(*suggestionsBox_);
  auto* suggestionsFooter = Gtk::make_managed<Gtk::Label>(
      T_("suggested_remote_dns_servers_description",
         "Suggestions for the connected country are marked with its color. Turning one on adds "
         "it to the remote DNS servers."));
  suggestionsFooter->add_css_class("dim-label");
  suggestionsFooter->add_css_class("caption");
  suggestionsFooter->set_wrap(true);
  suggestionsFooter->set_xalign(0);
  suggestionsSection_->append(*suggestionsFooter);
  form->append(*suggestionsSection_);

  // editable server lists. "https://" is a URL scheme, not prose: it stays a
  // literal, the way the android DNS screen does it.
  listFields_.reserve(8);
  AddListSection(*form, T_("remote_doh_urls", "Remote DoH URLs"), "https://", true,
                 &urnet::DnsResolverSettings::RemoteDohUrlsIpv4,
                 &urnet::DnsResolverSettings::RemoteDohUrlsIpv6);
  AddListSection(*form, T_("local_doh_urls", "Local DoH URLs"), "https://", true,
                 &urnet::DnsResolverSettings::LocalDohUrlsIpv4,
                 &urnet::DnsResolverSettings::LocalDohUrlsIpv6);
  AddListSection(*form, T_("remote_dns_servers", "Remote DNS servers"),
                 T_("ip_address", "IP address"), false,
                 &urnet::DnsResolverSettings::RemoteDnsIpv4,
                 &urnet::DnsResolverSettings::RemoteDnsIpv6);
  AddListSection(*form, T_("local_dns_servers", "Local DNS servers"),
                 T_("ip_address", "IP address"), false,
                 &urnet::DnsResolverSettings::LocalDnsIpv4,
                 &urnet::DnsResolverSettings::LocalDnsIpv6);

  // apply bar
  auto* actionBar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  actionBar->set_margin(12);
  updateBtn_ = Gtk::make_managed<Gtk::Button>(T_("update", "Update"));
  updateBtn_->add_css_class("suggested-action");
  updateBtn_->add_css_class("pill");
  updateBtn_->signal_clicked().connect([this] {
    host_.SetDnsResolverSettings(draft_);
    set_visible(false);
  });
  actionBar->append(*updateBtn_);
  root->append(*actionBar);
}

Gtk::Box* DnsSheet::AddSwitchRow(Gtk::Box& parent, const std::string& title,
                                 const std::string& detail, Gtk::Switch*& outSwitch,
                                 bool urnet::DnsResolverSettings::*flag) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* label = Gtk::make_managed<Gtk::Label>(title);
  label->set_xalign(0);
  row->append(*label);
  if (!detail.empty()) {
    auto* detailLabel = Gtk::make_managed<Gtk::Label>(detail);
    detailLabel->add_css_class("dim-label");
    detailLabel->add_css_class("caption");
    row->append(*detailLabel);
  }
  auto* spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_hexpand(true);
  row->append(*spacer);
  outSwitch = Gtk::make_managed<Gtk::Switch>();
  outSwitch->set_valign(Gtk::Align::CENTER);
  Gtk::Switch* toggle = outSwitch;
  toggle->property_active().signal_changed().connect([this, toggle, flag] {
    if (updating_) return;
    draft_.*flag = toggle->get_active();
    // enabling/disabling remote dns can flip the suggestion switches' backing
    // state indirectly; keep the panel + dirty state in sync
    RebuildPanel();
    RefreshDirty();
  });
  row->append(*outSwitch);
  parent.append(*row);
  return row;
}

void DnsSheet::AddListSection(Gtk::Box& parent, const std::string& title,
                              const std::string& placeholder, bool doh,
                              std::optional<urnet::StringList> urnet::DnsResolverSettings::*v4,
                              std::optional<urnet::StringList> urnet::DnsResolverSettings::*v6) {
  parent.append(*MakeCaption(title));
  auto* card = MakeCard(8);
  parent.append(*card);

  auto addSublist = [&](const char* caption,
                        std::optional<urnet::StringList> urnet::DnsResolverSettings::*field) {
    auto* captionLabel = Gtk::make_managed<Gtk::Label>(caption);
    captionLabel->add_css_class("dim-label");
    captionLabel->add_css_class("caption");
    captionLabel->set_xalign(0);
    card->append(*captionLabel);

    ListField lf;
    lf.field = field;
    lf.doh = doh;
    lf.rowsBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    card->append(*lf.rowsBox);

    auto* addRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    lf.entry = Gtk::make_managed<Gtk::Entry>();
    lf.entry->set_placeholder_text(placeholder);
    lf.entry->set_hexpand(true);
    addRow->append(*lf.entry);
    lf.addBtn = Gtk::make_managed<Gtk::Button>();
    lf.addBtn->set_icon_name("list-add-symbolic");
    lf.addBtn->add_css_class("flat");
    addRow->append(*lf.addBtn);
    card->append(*addRow);

    const size_t index = listFields_.size();
    listFields_.push_back(lf);
    // the add button enables only for a valid, non-duplicate trimmed value
    lf.entry->signal_changed().connect([this, index] {
      const ListField& field_ = listFields_[index];
      const std::string value = TrimWhitespace(std::string(field_.entry->get_text()));
      const auto& values = *(draft_.*(field_.field));
      const bool valid = field_.doh ? IsValidDohUrl(value) : IsIpAddressValue(value);
      field_.addBtn->set_sensitive(valid &&
                                   std::find(values.begin(), values.end(), value) == values.end());
    });
    lf.entry->signal_activate().connect([this, index] { AddListValue(index); });
    lf.addBtn->signal_clicked().connect([this, index] { AddListValue(index); });
    lf.addBtn->set_sensitive(false);
  };
  addSublist(T_("ipv4", "IPv4"), v4);
  addSublist(T_("ipv6", "IPv6"), v6);
}

void DnsSheet::AddListValue(size_t index) {
  const ListField& lf = listFields_[index];
  const std::string value = TrimWhitespace(std::string(lf.entry->get_text()));
  auto& values = *(draft_.*(lf.field));
  const bool valid = lf.doh ? IsValidDohUrl(value) : IsIpAddressValue(value);
  if (!valid || std::find(values.begin(), values.end(), value) != values.end()) return;
  values.push_back(value);
  lf.entry->set_text("");
  RebuildListRows(index);
  // adding a remote dns server can match a regional suggestion
  for (auto& row : suggestionRows_) SyncSuggestionRow(row);
  RebuildPanel();
  RefreshDirty();
}

void DnsSheet::RebuildListRows(size_t index) {
  const ListField& lf = listFields_[index];
  RemoveAllChildren(*lf.rowsBox);
  const auto& values = *(draft_.*(lf.field));
  if (values.empty()) return;
  for (const auto& value : values) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* valueLabel = Gtk::make_managed<Gtk::Label>(value);
    valueLabel->add_css_class("ur-mono-12");
    valueLabel->set_xalign(0);
    valueLabel->set_hexpand(true);
    valueLabel->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    row->append(*valueLabel);
    auto* removeBtn = Gtk::make_managed<Gtk::Button>();
    removeBtn->set_icon_name("window-close-symbolic");
    removeBtn->add_css_class("flat");
    const std::string valueCopy = value;
    removeBtn->signal_clicked().connect([this, index, valueCopy] {
      auto& list = *(draft_.*(listFields_[index].field));
      list.erase(std::remove(list.begin(), list.end(), valueCopy), list.end());
      RebuildListRows(index);
      for (auto& row_ : suggestionRows_) SyncSuggestionRow(row_);
      RebuildPanel();
      RefreshDirty();
    });
    row->append(*removeBtn);
    lf.rowsBox->append(*row);
  }
}

void DnsSheet::BuildSuggestions() {
  suggestionRows_.clear();
  RemoveAllChildren(*suggestionsBox_);

  auto servers = urnet::getRegionalDnsServers();
  if (!servers || servers->empty()) {
    suggestionsSection_->set_visible(false);
    return;
  }
  suggestionsSection_->set_visible(true);

  // connected country first, then country code, then name
  std::sort(servers->begin(), servers->end(),
            [this](const urnet::RegionalDnsServer& a, const urnet::RegionalDnsServer& b) {
              const bool aMatch = Lowercased(a.CountryCode) == countryCode_;
              const bool bMatch = Lowercased(b.CountryCode) == countryCode_;
              if (aMatch != bMatch) return aMatch;
              if (a.CountryCode != b.CountryCode) return a.CountryCode < b.CountryCode;
              return a.Name < b.Name;
            });

  for (const auto& server : *servers) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);

    if (Lowercased(server.CountryCode) == countryCode_) {
      // suggested for the connected country: mark with its color
      auto* dot = Gtk::make_managed<Gtk::Label>();
      dot->set_markup(CountryDotMarkup(Lowercased(server.CountryCode)));
      row->append(*dot);
    }

    auto* textColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    textColumn->set_hexpand(true);
    auto* nameRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* nameLabel = Gtk::make_managed<Gtk::Label>(server.Name);
    nameLabel->set_xalign(0);
    nameRow->append(*nameLabel);
    auto* codeLabel = Gtk::make_managed<Gtk::Label>(Uppercased(server.CountryCode));
    codeLabel->add_css_class("dim-label");
    codeLabel->add_css_class("caption");
    nameRow->append(*codeLabel);
    textColumn->append(*nameRow);
    auto* ipLabel = Gtk::make_managed<Gtk::Label>(server.Ipv4);
    ipLabel->add_css_class("ur-mono-12");
    ipLabel->add_css_class("dim-label");
    ipLabel->set_xalign(0);
    textColumn->append(*ipLabel);
    row->append(*textColumn);

    SuggestionRow suggestion;
    suggestion.ipv4 = server.Ipv4;
    suggestion.toggle = Gtk::make_managed<Gtk::Switch>();
    suggestion.toggle->set_valign(Gtk::Align::CENTER);
    Gtk::Switch* toggle = suggestion.toggle;
    const std::string ipv4 = server.Ipv4;
    toggle->property_active().signal_changed().connect([this, toggle, ipv4] {
      if (updating_) return;
      auto& remoteDns = *draft_.RemoteDnsIpv4;
      if (toggle->get_active()) {
        // on adds the server to the remote dns list and enables remote dns
        if (std::find(remoteDns.begin(), remoteDns.end(), ipv4) == remoteDns.end()) {
          remoteDns.push_back(ipv4);
        }
        draft_.EnableRemoteDns = true;
        updating_ = true;
        remoteDnsSwitch_->set_active(true);
        updating_ = false;
      } else {
        remoteDns.erase(std::remove(remoteDns.begin(), remoteDns.end(), ipv4), remoteDns.end());
      }
      RebuildRemoteDnsRows();
      RebuildPanel();
      RefreshDirty();
    });
    row->append(*suggestion.toggle);

    suggestionsBox_->append(*row);
    suggestionRows_.push_back(suggestion);
  }
}

void DnsSheet::SyncSuggestionRow(SuggestionRow& row) {
  const auto& remoteDns = *draft_.RemoteDnsIpv4;
  const bool on = std::find(remoteDns.begin(), remoteDns.end(), row.ipv4) != remoteDns.end();
  if (row.toggle->get_active() != on) {
    updating_ = true;
    row.toggle->set_active(on);
    updating_ = false;
  }
}

void DnsSheet::RebuildRemoteDnsRows() {
  for (size_t i = 0; i < listFields_.size(); ++i) {
    if (listFields_[i].field == &urnet::DnsResolverSettings::RemoteDnsIpv4) {
      RebuildListRows(i);
      break;
    }
  }
}

void DnsSheet::RebuildPanel() {
  RemoveAllChildren(*panelBox_);

  auto addStatusRow = [&](const std::string& text, bool showCountryColor) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto* dot = Gtk::make_managed<Gtk::Label>();
    if (showCountryColor) {
      dot->set_markup(CountryDotMarkup(countryCode_));
    } else {
      dot->set_markup("<span foreground='#87fb67'>✔</span>");
    }
    row->append(*dot);
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_xalign(0);
    label->set_wrap(true);
    row->append(*label);
    panelBox_->append(*row);
  };
  auto addApplyPanel = [&](const std::string& text, const std::string& buttonText,
                           bool showCountryColor, const urnet::DnsResolverSettings& target) {
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->add_css_class("dim-label");
    label->set_wrap(true);
    label->set_xalign(0);
    panelBox_->append(*label);
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    if (showCountryColor) {
      auto* dot = Gtk::make_managed<Gtk::Label>();
      dot->set_markup(CountryDotMarkup(countryCode_));
      row->append(*dot);
    }
    auto* button = Gtk::make_managed<Gtk::Button>(buttonText);
    button->add_css_class("suggested-action");
    button->add_css_class("pill");
    const urnet::DnsResolverSettings targetCopy = target;
    button->signal_clicked().connect([this, targetCopy] {
      draft_ = targetCopy;
      SyncFromDraft();
    });
    row->append(*button);
    panelBox_->append(*row);
  };

  if (recommendation_) {
    if (SettingsEqual(draft_, *recommendation_)) {
      addStatusRow(T_("dns_using_recommended", "Using recommended regional settings"), true);
    } else {
      const std::string country =
          !countryName_.empty()
              ? countryName_
              : (!countryCode_.empty() ? Uppercased(countryCode_)
                                       : std::string(T_("this_region", "this region")));
      addApplyPanel(Format(T_("dns_recommendation_message",
                              "The strongest security rules are known not to work in {}. There "
                              "are less secure recommended DNS settings that work better."),
                           country),
                    T_("use_recommended_settings", "Use recommended settings"), true,
                    *recommendation_);
    }
    panelBox_->set_visible(true);
  } else if (defaults_) {
    if (SettingsEqual(draft_, *defaults_)) {
      addStatusRow(T_("dns_using_secure", "Using most secure default settings"), false);
    } else {
      addApplyPanel(
          T_("dns_restore_secure_message",
             "Restore the most secure settings: encrypted DNS over HTTPS through the tunnel."),
          T_("restore_most_secure_settings", "Restore to most secure settings"), false, *defaults_);
    }
    panelBox_->set_visible(true);
  } else {
    panelBox_->set_visible(false);
  }
}

void DnsSheet::SyncFromDraft() {
  updating_ = true;
  remoteDohSwitch_->set_active(draft_.EnableRemoteDoh);
  localDohSwitch_->set_active(draft_.EnableLocalDoh);
  remoteDnsSwitch_->set_active(draft_.EnableRemoteDns);
  localDnsSwitch_->set_active(draft_.EnableLocalDns);
  fallbackSwitch_->set_active(draft_.EnableFallback);
  updating_ = false;
  for (size_t i = 0; i < listFields_.size(); ++i) RebuildListRows(i);
  for (auto& row : suggestionRows_) SyncSuggestionRow(row);
  RebuildPanel();
  RefreshDirty();
}

void DnsSheet::RefreshDirty() {
  // Update applies the draft only when it differs from the loaded settings
  updateBtn_->set_sensitive(!SettingsEqual(draft_, original_));
}

}  // namespace urnw
