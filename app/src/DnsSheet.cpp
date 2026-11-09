// SPDX-License-Identifier: MPL-2.0
#include "DnsSheet.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>

#include "DnsPresetsPresentation.hpp"
#include "FastDnsCopy.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// How far a pick's lines sit in from its radio: under the label beside it.
constexpr int kPickInset = 28;
// How often the blocked-lookups count is read while it shows (I3): it
// changes with no callback.
constexpr unsigned kBlockedCountPollSeconds = 2;

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

// A presentation text through the catalog; an empty key is nothing to show.
Glib::ustring Tr(const dns_presets::Text& text) {
  if (text.key.empty()) return Glib::ustring();
  return g_dpgettext2(GETTEXT_PACKAGE, std::string(text.key).c_str(),
                      std::string(text.english).c_str());
}

// The words of a key id the sdk returned, with the value its text takes (the
// provider's name or the country's); "" for a key this build does not know.
std::string KeyLine(const std::string& key, const std::string& value) {
  const dns_presets::Text text = dns_presets::KeyText(key);
  if (text.key.empty()) return std::string();
  return Format(Tr(text).c_str(), value);
}

// A wrapping line in the dim caption style: the line under a pick, a note.
Gtk::Label* MakeNote(const Glib::ustring& text) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class("dim-label");
  label->add_css_class("caption");
  label->set_wrap(true);
  label->set_xalign(0);
  return label;
}

void OpenLink(const std::string& url) {
  GError* err = nullptr;
  if (!g_app_info_launch_default_for_uri(url.c_str(), nullptr, &err)) {
    g_warning("dns: could not open %s: %s", url.c_str(), err ? err->message : "?");
    if (err) g_error_free(err);
  }
}

// A wrapping caption line of pango markup whose links open in the browser.
Gtk::Label* MakeMarkupLine(const Glib::ustring& markup) {
  auto* label = Gtk::make_managed<Gtk::Label>();
  label->set_markup(markup);
  label->add_css_class("caption");
  label->set_wrap(true);
  label->set_xalign(0);
  label->signal_activate_link().connect(
      [](const Glib::ustring& uri) -> bool {
        OpenLink(uri.raw());
        return true;  // handled; GTK must not launch a second time
      },
      false);
  return label;
}

// A link to `url` named `text`, as pango markup.
Glib::ustring LinkMarkup(const std::string& url, const Glib::ustring& text) {
  return "<a href=\"" + Glib::Markup::escape_text(url) + "\">" + Glib::Markup::escape_text(text) +
         "</a>";
}

// `text` as pango markup, in the danger tone when it asks for attention.
Glib::ustring ToneMarkup(const Glib::ustring& text, bool attention) {
  const Glib::ustring escaped = Glib::Markup::escape_text(text);
  if (!attention) return escaped;
  return "<span foreground='" + HexForMarkup(kUrDanger) + "'>" + escaped + "</span>";
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
  return a.PresetId == b.PresetId && a.PresetPersonalId == b.PresetPersonalId &&
         a.EnableRemoteDoh == b.EnableRemoteDoh && a.EnableLocalDoh == b.EnableLocalDoh &&
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

std::string DnsSheet::CountryName(const std::string& countryCode) const {
  if (countryCode == countryCode_ && !countryName_.empty()) return countryName_;
  const std::string name = CountryDisplayName(countryCode);
  return name.empty() ? Uppercased(countryCode) : name;
}

DnsSheet::DnsSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_("custom_dns", "Custom DNS"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(520, 680);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  // keep every draft list engaged so the field handlers can never dereference
  // an empty optional, even before the first Open
  Normalize(draft_);
  Normalize(original_);
  Normalize(customSeed_);
  BuildUi();
  // the blocked count is read only while the sheet shows
  signal_hide().connect([this] { StopBlockedCountPoll(); });
}

DnsSheet::~DnsSheet() { blockedCountPoll_.disconnect(); }

bool DnsSheet::Open() {
  auto settings = host_.GetDnsResolverSettings();
  if (!settings) return false;  // the card shows "DNS settings unavailable"
  RefreshConnectionRegion();
  Seed(*settings, Edits::Drop);
  present();
  return true;
}

void DnsSheet::RefreshConnectionRegion() {
  connectionRegionBtn_->set_label(ConnectionRegionName(host_) + " ▾");
}

void DnsSheet::OpenConnectionRegion() {
  if (!connectionRegionSheet_) {
    connectionRegionSheet_ = std::make_unique<ConnectionRegionSheet>(*this, host_);
    connectionRegionSheet_->on_changed = [this](const std::string&) { RefreshConnectionRegion(); };
  }
  connectionRegionSheet_->Open();
}

void DnsSheet::RefreshStatus() {
  status_ = host_.GetDnsResolverStatus();
  RenderStatus();
}

void DnsSheet::RefreshSettings() {
  // an edit in progress keeps the sheet as it is: the next save's
  // compare-and-set re-seeds it if the settings moved
  if (pendingCustom_ || CustomEdited() || PersonalEdited()) return;
  auto settings = host_.GetDnsResolverSettings();
  if (!settings) return;
  Normalize(*settings);
  if (SettingsEqual(*settings, original_)) return;
  Seed(*settings, Edits::Drop);
}

void DnsSheet::Seed(urnet::DnsResolverSettings settings, Edits edits) {
  Normalize(settings);
  const bool keepCustom = edits == Edits::Keep && CustomEdited();
  const urnet::DnsResolverSettings editedDraft = draft_;
  original_ = settings;
  // Custom drafts from the lists the selection resolves with, its selection
  // cleared and every other field kept (the sdk's helper), so its Update saves
  // custom settings whatever was selected
  if (auto custom = urnet::dnsResolverSettingsAsCustom(settings)) {
    customSeed_ = *custom;
  } else {
    // the helper could not run: the lists read back are the ones in force
    customSeed_ = settings;
    customSeed_.PresetId.clear();
    customSeed_.PresetPersonalId.clear();
  }
  Normalize(customSeed_);
  if (keepCustom) {
    // an edit survives Fast DNS on connect saved beside it, with its new value
    draft_ = editedDraft;
    draft_.EnableFallback = customSeed_.EnableFallback;
  } else {
    draft_ = customSeed_;
  }
  if (edits == Edits::Drop) {
    pendingCustom_ = false;
    pendingPersonal_.clear();
    personalInput_ = settings.PresetPersonalId;
  }

  countryCode_.clear();
  countryName_.clear();
  // the country of the connection, not of a choice a Disconnect left behind
  if (auto location = host_.ConnectedLocation()) {
    if (location->country_code) countryCode_ = Lowercased(*location->country_code);
    if (location->country) countryName_ = *location->country;
  }
  summary_ = urnet::describeDnsResolverSettings(original_, countryCode_);
  picks_ = urnet::getDnsPicks(countryCode_).value_or(urnet::DnsPickList());
  picksError_->set_visible(false);

  BuildSuggestions();
  SyncFromDraft();
  RebuildPicks();
  RefreshStatus();
}

void DnsSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  set_child(*root);

  scroller_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller_->set_vexpand(true);
  root->append(*scroller_);

  auto* form = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  form->set_margin(16);
  scroller_->set_child(*form);

  // Connection region (ConnectionRegion.hpp): how the app reaches URnetwork,
  // the ingress dns. Its own stored value, saved at once from the picker and
  // never by a save here; the controls under "Traffic DNS" are the tunnel's.
  form->append(*MakeCaption(T_("connection_region", "Connection region")));
  auto* regionCard = MakeCard(10);
  connectionRegionBtn_ = Gtk::make_managed<Gtk::Button>();
  connectionRegionBtn_->set_halign(Gtk::Align::FILL);
  connectionRegionBtn_->signal_clicked().connect([this] { OpenConnectionRegion(); });
  regionCard->append(*connectionRegionBtn_);
  auto* regionFooter = Gtk::make_managed<Gtk::Label>(T_(
      "connection_region_description", "Only affects how the app reaches URnetwork, not your traffic."));
  regionFooter->add_css_class("dim-label");
  regionFooter->add_css_class("caption");
  regionFooter->set_wrap(true);
  regionFooter->set_xalign(0);
  regionCard->append(*regionFooter);
  form->append(*regionCard);

  auto* trafficHeading = Gtk::make_managed<Gtk::Label>(T_("traffic_dns", "Traffic DNS"));
  trafficHeading->add_css_class("heading");
  trafficHeading->set_xalign(0);
  trafficHeading->set_margin_top(8);
  form->append(*trafficHeading);

  // A selection a release dropped (A7, O8): URnetwork's dns applies in its
  // place, and this names the provider until a pick replaces it or Got it
  // saves URnetwork's.
  droppedBanner_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  droppedBanner_->add_css_class("ur-banner");
  droppedText_ = Gtk::make_managed<Gtk::Label>();
  droppedText_->set_wrap(true);
  droppedText_->set_xalign(0);
  droppedBanner_->append(*droppedText_);
  {
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto* choose = Gtk::make_managed<Gtk::Button>(
        T_("dns_preset_dropped_choose", "Choose another preset"));
    choose->add_css_class("flat");
    choose->set_halign(Gtk::Align::START);
    // the picks below are the other presets
    choose->signal_clicked().connect([this] {
      if (firstPresetRadio_) firstPresetRadio_->grab_focus();
    });
    actions->append(*choose);
    auto* custom = Gtk::make_managed<Gtk::Button>(
        T_("dns_preset_dropped_custom", "Enter it yourself under Custom"));
    custom->add_css_class("flat");
    custom->set_halign(Gtk::Align::START);
    custom->signal_clicked().connect([this] {
      pendingCustom_ = true;
      pendingPersonal_.clear();
      RebuildPicks();
      if (!listFields_.empty()) listFields_.front().entry->grab_focus();
    });
    actions->append(*custom);
    auto* gotIt = Gtk::make_managed<Gtk::Button>(T_("got_it", "Got it"));
    gotIt->add_css_class("suggested-action");
    gotIt->add_css_class("pill");
    gotIt->set_halign(Gtk::Align::START);
    // dismissing the notice saves URnetwork's dns, the one it names in force
    gotIt->signal_clicked().connect([this] { SelectPick(urnet::DnsPresetIdDefault); });
    actions->append(*gotIt);
    droppedBanner_->append(*actions);
  }
  droppedBanner_->set_visible(false);
  form->append(*droppedBanner_);

  // a pick the sdk no longer offers (the picks were listed before a change)
  picksError_ = Gtk::make_managed<Gtk::Label>();
  picksError_->set_wrap(true);
  picksError_->set_xalign(0);
  picksError_->set_visible(false);
  form->append(*picksError_);

  // The picks in the sdk's order, their rows rebuilt with each seed: those
  // through Custom, Custom's editor under its pick, then the picks after it.
  picksBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  form->append(*picksBox_);
  customBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  customBox_->set_margin_start(kPickInset);
  customBox_->set_visible(false);
  form->append(*customBox_);
  tailPicksBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  form->append(*tailPicksBox_);

  // Custom: a filtering server mixed with URnetwork's does not filter, since
  // the fastest answer wins (F1 item 7)
  customBox_->append(*MakeNote(T_(
      "dns_custom_mix_note",
      "Mixing a filtering server with the default servers doesn't filter: the fastest answer "
      "wins.")));

  // resolvers
  customBox_->append(*MakeCaption(T_("resolvers", "Resolvers")));
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
  customBox_->append(*resolvers);

  // well-known regional server suggestions (rows rebuilt per seed)
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
  customBox_->append(*suggestionsSection_);

  // editable server lists. "https://" is a URL scheme, not prose: it stays a
  // literal, the way the android DNS screen does it.
  listFields_.reserve(8);
  AddListSection(*customBox_, T_("remote_doh_urls", "Remote DoH URLs"), "https://", true,
                 &urnet::DnsResolverSettings::RemoteDohUrlsIpv4,
                 &urnet::DnsResolverSettings::RemoteDohUrlsIpv6);
  AddListSection(*customBox_, T_("local_doh_urls", "Local DoH URLs"), "https://", true,
                 &urnet::DnsResolverSettings::LocalDohUrlsIpv4,
                 &urnet::DnsResolverSettings::LocalDohUrlsIpv6);
  AddListSection(*customBox_, T_("remote_dns_servers", "Remote DNS servers"),
                 T_("ip_address", "IP address"), false,
                 &urnet::DnsResolverSettings::RemoteDnsIpv4,
                 &urnet::DnsResolverSettings::RemoteDnsIpv6);
  AddListSection(*customBox_, T_("local_dns_servers", "Local DNS servers"),
                 T_("ip_address", "IP address"), false,
                 &urnet::DnsResolverSettings::LocalDnsIpv4,
                 &urnet::DnsResolverSettings::LocalDnsIpv6);

  // Fast DNS on connect: the opt-in host-network resolver race while the
  // tunnel's dns starts, for every selection. It saves at once, through the
  // sdk helper that keeps the selection (SaveFallback).
  auto* fallbackCard = MakeCard(10);
  {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* label = Gtk::make_managed<Gtk::Label>(T_(kFastDnsLabel.key, kFastDnsLabel.english));
    label->set_xalign(0);
    label->set_hexpand(true);
    row->append(*label);
    fallbackSwitch_ = Gtk::make_managed<Gtk::Switch>();
    fallbackSwitch_->set_valign(Gtk::Align::CENTER);
    fallbackSwitch_->property_active().signal_changed().connect([this] {
      if (updating_) return;
      SaveFallback(fallbackSwitch_->get_active());
    });
    row->append(*fallbackSwitch_);
    fallbackCard->append(*row);
  }
  form->append(*fallbackCard);
  auto* fallbackFooter = Gtk::make_managed<Gtk::Label>(
      T_(kFastDnsDescription.key, kFastDnsDescription.english));
  fallbackFooter->add_css_class("dim-label");
  fallbackFooter->add_css_class("caption");
  fallbackFooter->set_wrap(true);
  fallbackFooter->set_xalign(0);
  form->append(*fallbackFooter);
  // with a preset, lookups while connecting are URnetwork's, not the
  // provider's (C7)
  fastDnsPresetNote_ = MakeNote(Glib::ustring());
  fastDnsPresetNote_->set_visible(false);
  form->append(*fastDnsPresetNote_);

  // a change applies to new lookups at once, and apps may keep earlier answers
  // for about a minute (F1 item 8); ur.io explains the presets
  form->append(*MakeNote(T_(
      "dns_preset_apply_note",
      "Changes apply to new lookups right away. Apps may keep earlier answers for about a minute.")));
  form->append(*MakeMarkupLine(LinkMarkup(
      dns_presets::kHelpUrl, Glib::ustring(T_("learn_more", "Learn more")) + "\xC2\xA0\xE2\x86\x97")));

  // Custom's Update, shown while its editor is
  actionBar_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  actionBar_->set_margin(12);
  updateBtn_ = Gtk::make_managed<Gtk::Button>(T_("update", "Update"));
  updateBtn_->add_css_class("suggested-action");
  updateBtn_->add_css_class("pill");
  // the draft started from the settings as custom (Seed), so the save keeps
  // every field the editor does not show
  updateBtn_->signal_clicked().connect([this] { Write(draft_, AfterWrite::Close, Edits::Drop); });
  actionBar_->append(*updateBtn_);
  actionBar_->set_visible(false);
  root->append(*actionBar_);
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

void DnsSheet::SyncFromDraft() {
  updating_ = true;
  remoteDohSwitch_->set_active(draft_.EnableRemoteDoh);
  localDohSwitch_->set_active(draft_.EnableLocalDoh);
  remoteDnsSwitch_->set_active(draft_.EnableRemoteDns);
  localDnsSwitch_->set_active(draft_.EnableLocalDns);
  // Fast DNS on connect saves apart from the draft, so it shows what is saved
  fallbackSwitch_->set_active(original_.EnableFallback);
  updating_ = false;
  for (size_t i = 0; i < listFields_.size(); ++i) RebuildListRows(i);
  for (auto& row : suggestionRows_) SyncSuggestionRow(row);
  RefreshDirty();
}

void DnsSheet::RefreshDirty() {
  // Update applies the draft only when it differs from the loaded settings: a
  // selection that is not Custom always does, since the draft's is cleared
  updateBtn_->set_sensitive(!SettingsEqual(draft_, original_));
}

bool DnsSheet::CustomEdited() const { return !SettingsEqual(draft_, customSeed_); }

bool DnsSheet::PersonalEdited() const {
  return !pendingPersonal_.empty() || personalInput_ != original_.PresetPersonalId;
}

// ---- the picks -----------------------------------------------------------------

bool DnsSheet::IsSelection(const urnet::DnsPick& pick) const {
  return summary_ && pick.Kind == summary_->Kind && pick.PresetId == summary_->PresetId;
}

void DnsSheet::RebuildPicks() {
  RemoveAllChildren(*picksBox_);
  RemoveAllChildren(*tailPicksBox_);
  firstPresetRadio_ = nullptr;
  personalEntry_ = nullptr;
  personalErrorLine_ = nullptr;
  statusBox_ = nullptr;
  statusLine_ = nullptr;
  statusDetail_ = nullptr;
  verificationLine_ = nullptr;
  statusActions_ = nullptr;
  useDefaultBtn_ = nullptr;
  changeLocationBtn_ = nullptr;
  blockedCountLine_ = nullptr;
  blockedFloorNote_ = nullptr;

  // A regional selection of another country than the exit's (picked before
  // the exit moved) keeps its row: the sdk's regional pick for that country.
  urnet::DnsPickList picks = picks_;
  const bool listed = std::any_of(picks.begin(), picks.end(),
                                  [this](const urnet::DnsPick& pick) { return IsSelection(pick); });
  if (!listed && summary_ && summary_->Kind == urnet::DnsPickKindRegional) {
    for (const auto& pick : urnet::getDnsPicks(summary_->CountryCode).value_or(urnet::DnsPickList())) {
      if (pick.Kind == urnet::DnsPickKindRegional) picks.push_back(pick);
    }
  }

  // the radio shows a pick tapped that needs more first, else the selection
  const auto active = [this](const urnet::DnsPick& pick) {
    if (pendingCustom_) return pick.Kind == urnet::DnsPickKindCustom;
    if (!pendingPersonal_.empty()) {
      return pick.Kind == urnet::DnsPickKindPreset && pick.PresetId == pendingPersonal_;
    }
    return IsSelection(pick);
  };
  syncing_ = true;
  Gtk::CheckButton* group = nullptr;
  Gtk::Box* target = picksBox_;
  std::string groupKey;
  for (const auto& pick : picks) {
    if (pick.GroupKey != groupKey) {
      // the picks that are no preset's stand apart from the last group
      if (pick.GroupKey.empty() && !groupKey.empty()) {
        target->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
      }
      groupKey = pick.GroupKey;
      // a group's heading over its first preset (O9)
      const Glib::ustring heading = Tr(dns_presets::KeyText(groupKey));
      if (!heading.empty()) target->append(*MakeCaption(heading.raw()));
    }
    target->append(*BuildPickRow(pick, group, active(pick)));
    // the picks after Custom go under its editor
    if (pick.Kind == urnet::DnsPickKindCustom) target = tailPicksBox_;
  }
  syncing_ = false;

  // Custom's editor shows under its pick while it is tapped or the selection
  const bool customOpen =
      pendingCustom_ ||
      (pendingPersonal_.empty() && summary_ && summary_->Kind == urnet::DnsPickKindCustom);
  customBox_->set_visible(customOpen);
  actionBar_->set_visible(customOpen);

  // A dropped selection's notice (A7) names its provider; an id this release
  // never knew (a newer catalog's, after a downgrade) has no name to give.
  const bool dropped = summary_ && summary_->Dropped;
  droppedBanner_->set_visible(dropped);
  if (dropped) {
    droppedText_->set_text(
        summary_->DroppedProviderName.empty()
            ? std::string(Tr(dns_presets::ErrorText(dns_presets::kErrorUnknown)))
            : Format(T_("dns_preset_dropped",
                        "{} is no longer available. You're using URnetwork DNS, so lookups "
                        "aren't filtered."),
                     summary_->DroppedProviderName));
  }

  // with a preset, lookups while connecting are URnetwork's (C7)
  const bool preset = summary_ && summary_->Kind == urnet::DnsPickKindPreset;
  fastDnsPresetNote_->set_text(
      preset ? Format(T_("dns_preset_fast_dns_note",
                         "While connecting, lookups use URnetwork DNS, so {} doesn't filter them."),
                      summary_->ProviderName)
             : std::string());
  fastDnsPresetNote_->set_visible(preset);
}

Gtk::Widget* DnsSheet::BuildPickRow(const urnet::DnsPick& pick, Gtk::CheckButton*& group,
                                    bool active) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  // a preset by its provider's name, never a logo (O10); the other picks by
  // their label, the regional pick's with its country
  const std::string country =
      pick.CountryCode.empty() ? std::string() : CountryName(pick.CountryCode);
  const std::string provider = pick.Preset ? pick.Preset->ProviderName : std::string();
  const std::string title = pick.LabelKey.empty() ? provider : KeyLine(pick.LabelKey, country);
  auto* radio = Gtk::make_managed<Gtk::CheckButton>(title);
  if (group) {
    radio->set_group(*group);
  } else {
    group = radio;
  }
  if (active) radio->set_active(true);
  radio->signal_toggled().connect([this, radio, pick] {
    if (syncing_ || !radio->get_active()) return;
    Defer([this, pick] { OnPick(pick); });
  });
  row->append(*radio);
  if (pick.Kind == urnet::DnsPickKindPreset && !firstPresetRadio_) firstPresetRadio_ = radio;

  auto* details = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  details->set_margin_start(kPickInset);
  row->append(*details);
  // what it blocks, or the line under a pick that is no preset
  const std::string copy = KeyLine(pick.CopyKey, pick.Preset ? provider : country);
  if (!copy.empty()) details->append(*MakeNote(copy));
  if (!pick.Preset) return row;
  const urnet::DnsPreset& preset = *pick.Preset;
  // where its operator answers to, and its privacy policy (O10, F1 item 2)
  Glib::ustring meta = Glib::Markup::escape_text(
      Format(T_("dns_preset_jurisdiction", "Jurisdiction: {}"), preset.Jurisdiction));
  if (!preset.PrivacyUrl.empty()) {
    meta += " \xC2\xB7 " +
            LinkMarkup(preset.PrivacyUrl, T_("dns_preset_privacy_link", "Privacy policy"));
  }
  details->append(*MakeMarkupLine(meta));

  // the selection's lines, with nothing tapped that needs more
  const bool selected = IsSelection(pick) && !pendingCustom_ && pendingPersonal_.empty();
  // a personal preset tapped or selected takes its profile id here (D3)
  if (preset.PersonalIdRequired && active) BuildPersonalPanel(*details, preset);
  if (selected || (preset.PersonalIdRequired && active)) {
    // what the provider receives, where it is picked and before a profile id
    // is saved (H2)
    details->append(*MakeNote(Format(
        T_("dns_preset_disclosure",
           "Your DNS lookups go to {} through the tunnel, so it never sees your IP address. Its "
           "privacy policy applies."),
        provider)));
    Glib::ustring links;
    if (!preset.PrivacyUrl.empty()) {
      links += LinkMarkup(preset.PrivacyUrl, T_("dns_preset_privacy_link", "Privacy policy"));
    }
    if (!preset.HelpUrl.empty()) {
      if (!links.empty()) links += " \xC2\xB7 ";
      links += LinkMarkup(preset.HelpUrl, T_("dns_preset_help_link", "Help"));
    }
    if (!links.empty()) details->append(*MakeMarkupLine(links));
  }
  if (selected) BuildStatusPanel(*details);
  return row;
}

void DnsSheet::BuildPersonalPanel(Gtk::Box& details, const urnet::DnsPreset& preset) {
  const std::string provider = preset.ProviderName;
  const std::string presetId = preset.PresetId;
  auto* caption = MakeCaption(T_("dns_personal_id_label", "Profile ID or DNS address"));
  caption->set_margin_top(4);
  details.append(*caption);
  personalEntry_ = Gtk::make_managed<Gtk::Entry>();
  // the provider's own example (D3)
  personalEntry_->set_placeholder_text(preset.PersonalIdExample);
  personalEntry_->set_text(personalInput_);
  personalEntry_->signal_changed().connect([this] {
    personalInput_ = std::string(personalEntry_->get_text());
    if (personalErrorLine_) personalErrorLine_->set_visible(false);
  });
  personalEntry_->signal_activate().connect([this, presetId] {
    Defer([this, presetId] { SavePersonalId(presetId); });
  });
  details.append(*personalEntry_);
  personalErrorLine_ = Gtk::make_managed<Gtk::Label>();
  personalErrorLine_->add_css_class("caption");
  personalErrorLine_->set_wrap(true);
  personalErrorLine_->set_xalign(0);
  personalErrorLine_->set_visible(false);
  details.append(*personalErrorLine_);
  // where the provider shows the id, with its dashboard (D3)
  Glib::ustring where = Glib::Markup::escape_text(KeyLine(preset.PersonalIdWhereKey, provider));
  if (!preset.PersonalIdHelpUrl.empty()) {
    if (!where.empty()) where += " ";
    where += LinkMarkup(preset.PersonalIdHelpUrl,
                        Format(T_("dns_personal_id_dashboard_link", "Open your {} dashboard"),
                               provider));
  }
  if (!where.empty()) details.append(*MakeMarkupLine(where));
  // the free plan's limit (D3), why no IP address is linked (D2), and the
  // block response to keep (C10)
  const std::string quota = KeyLine(preset.QuotaNoteKey, provider);
  if (!quota.empty()) details.append(*MakeNote(quota));
  details.append(*MakeNote(T_("dns_personal_id_linked_ip_note",
                              "No need to link an IP address in your dashboard: through URnetwork, "
                              "your lookups come from an address that isn't yours. Your ID is "
                              "enough.")));
  details.append(*MakeNote(Format(
      T_("dns_personal_id_block_response",
         "In {}, keep the block response at zero IP (0.0.0.0, the default) or NXDOMAIN. REFUSED "
         "and other responses look like an outage."),
      provider)));
  auto* save = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
  save->add_css_class("suggested-action");
  save->add_css_class("pill");
  save->set_halign(Gtk::Align::START);
  save->signal_clicked().connect([this, presetId] {
    Defer([this, presetId] { SavePersonalId(presetId); });
  });
  details.append(*save);
}

void DnsSheet::BuildStatusPanel(Gtk::Box& details) {
  statusBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  statusBox_->set_margin_top(4);
  statusLine_ = Gtk::make_managed<Gtk::Label>();
  statusLine_->set_wrap(true);
  statusLine_->set_xalign(0);
  statusBox_->append(*statusLine_);
  statusDetail_ = MakeNote(Glib::ustring());
  statusBox_->append(*statusDetail_);
  verificationLine_ = MakeMarkupLine(Glib::ustring());
  statusBox_->append(*verificationLine_);
  statusActions_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  useDefaultBtn_ = Gtk::make_managed<Gtk::Button>(T_("dns_status_use_default", "Use URnetwork DNS"));
  useDefaultBtn_->add_css_class("pill");
  // one tap back to URnetwork's dns (C8)
  useDefaultBtn_->signal_clicked().connect([this] {
    Defer([this] { SelectPick(urnet::DnsPresetIdDefault); });
  });
  statusActions_->append(*useDefaultBtn_);
  changeLocationBtn_ =
      Gtk::make_managed<Gtk::Button>(T_("dns_status_change_location", "Change location"));
  changeLocationBtn_->add_css_class("pill");
  changeLocationBtn_->signal_clicked().connect([this] {
    // the chooser is the window's: this modal sheet steps aside for it
    set_visible(false);
    if (on_change_location) on_change_location();
  });
  statusActions_->append(*changeLocationBtn_);
  statusBox_->append(*statusActions_);
  blockedCountLine_ = Gtk::make_managed<Gtk::Label>();
  blockedCountLine_->set_wrap(true);
  blockedCountLine_->set_xalign(0);
  statusBox_->append(*blockedCountLine_);
  blockedFloorNote_ = MakeNote(T_("dns_blocked_count_floor_note",
                                  "Some blocks aren't counted, so the real number may be higher."));
  statusBox_->append(*blockedFloorNote_);
  details.append(*statusBox_);
}

void DnsSheet::OnPick(const urnet::DnsPick& pick) {
  if (IsSelection(pick)) {
    // back to what is saved, from a pick tapped that needed more
    pendingCustom_ = false;
    pendingPersonal_.clear();
    personalInput_ = original_.PresetPersonalId;
    RebuildPicks();
    return;
  }
  if (pick.Kind == urnet::DnsPickKindCustom) {
    // Custom saves from its editor's Update
    pendingCustom_ = true;
    pendingPersonal_.clear();
    RebuildPicks();
    return;
  }
  if (pick.Preset && pick.Preset->PersonalIdRequired) {
    // a personal preset saves with the profile id typed under it
    pendingCustom_ = false;
    pendingPersonal_ = pick.PresetId;
    personalInput_.clear();
    RebuildPicks();
    return;
  }
  SelectPick(pick.PresetId);
}

void DnsSheet::SelectPick(const std::string& presetId) {
  const auto next = urnet::dnsResolverSettingsWithPreset(original_, presetId, std::string());
  if (!next) {
    // the sdk no longer offers it: the picks were listed before a change
    picksError_->set_markup(ToneMarkup(Tr(dns_presets::ErrorText(dns_presets::kErrorUnknown)), true));
    picksError_->set_visible(true);
    pendingCustom_ = false;
    pendingPersonal_.clear();
    RebuildPicks();
    return;
  }
  Write(*next, AfterWrite::Stay, Edits::Drop);
}

void DnsSheet::SavePersonalId(const std::string& presetId) {
  const std::string input = personalInput_;
  // the sdk reads what was typed or pasted: a bare id, the dashboard's DoH url
  // or the Private DNS hostname (D3, D4)
  const std::string errorId = urnet::validateDnsPresetPersonalId(presetId, input);
  if (!errorId.empty()) {
    ShowPersonalError(errorId, urnet::getDnsPreset(presetId));
    return;
  }
  const auto next = urnet::dnsResolverSettingsWithPreset(
      original_, presetId, urnet::normalizeDnsPresetPersonalId(presetId, input));
  if (!next) {
    ShowPersonalError(dns_presets::kErrorUnknown, urnet::getDnsPreset(presetId));
    return;
  }
  Write(*next, AfterWrite::Stay, Edits::Drop);
}

void DnsSheet::ShowPersonalError(const std::string& errorId,
                                 const std::optional<urnet::DnsPreset>& preset) {
  if (!personalErrorLine_) return;
  std::string text = Tr(dns_presets::ErrorText(errorId));
  // an id in the wrong form also says what the provider's ids look like (D4)
  if (errorId == dns_presets::kErrorPersonalIdInvalid && preset) {
    const dns_presets::Text format = dns_presets::KeyText(preset->PersonalIdFormatKey);
    if (!format.key.empty()) {
      text += " " + Format(Tr(format).c_str(), preset->ProviderName, preset->PersonalIdExample);
    }
  }
  personalErrorLine_->set_markup(ToneMarkup(text, true));
  personalErrorLine_->set_visible(true);
}

void DnsSheet::SaveFallback(bool enableFallback) {
  const auto next = urnet::dnsResolverSettingsWithFallback(original_, enableFallback);
  if (!next) {
    // nothing was saved: the switch shows what is
    updating_ = true;
    fallbackSwitch_->set_active(original_.EnableFallback);
    updating_ = false;
    return;
  }
  Write(*next, AfterWrite::Stay, Edits::Keep);
}

void DnsSheet::Write(urnet::DnsResolverSettings next, AfterWrite after, Edits edits) {
  // Compare-and-set (apple's editor safety): the save is written only while
  // the resolver still reads what the sheet was seeded from. Moved since
  // (another surface, the daemon's sync), the sheet re-seeds from what it
  // reads now instead of overwriting it; unknown now, the sheet closes and the
  // card says the settings are unavailable.
  const urnet::DnsResolverSettings shown = original_;
  std::optional<urnet::DnsResolverSettings> current;
  const bool written = host_.SetDnsResolverSettingsIf(
      [shown](const std::optional<urnet::DnsResolverSettings>& now) {
        if (!now) return false;
        urnet::DnsResolverSettings normalized = *now;
        Normalize(normalized);
        return SettingsEqual(normalized, shown);
      },
      next, &current);
  if (!current || (written && after == AfterWrite::Close)) {
    set_visible(false);
    return;
  }
  // written, the sheet shows what it saved; refused, what the resolver reads
  // now, and an edit made beside it goes too
  Seed(*current, written ? edits : Edits::Drop);
}

void DnsSheet::Defer(std::function<void()> action) {
  // a one-shot source removes itself by returning false
  Glib::signal_idle().connect(sigc::track_obj(
      [action]() -> bool {
        action();
        return false;
      },
      *this));
}

// ---- the status under the selected preset -----------------------------------

void DnsSheet::RenderStatus() {
  statusSummary_ = urnet::describeDnsResolverStatus(original_, status_);
  if (!statusBox_) {
    StopBlockedCountPoll();
    return;
  }
  const urnet::DnsStatusSummary summary = statusSummary_.value_or(urnet::DnsStatusSummary{});
  const std::string& provider = summary.ProviderName;
  // the provider and its state (C8); one failing asks for attention
  const std::string status = KeyLine(summary.StatusKey, provider);
  statusLine_->set_markup(ToneMarkup(status, summary.ShowUseDefault));
  statusLine_->set_visible(!status.empty());
  const std::string detail = KeyLine(summary.StatusDetailKey, provider);
  statusDetail_->set_text(detail);
  statusDetail_->set_visible(!detail.empty());
  // what verification found out (C6), with the dashboard beside a profile it
  // cannot confirm
  Glib::ustring verification =
      Glib::Markup::escape_text(KeyLine(summary.VerificationKey, provider));
  if (!verification.empty() && !summary.VerificationUrl.empty()) {
    const std::string link = KeyLine(summary.VerificationLinkKey, provider);
    if (!link.empty()) verification += " " + LinkMarkup(summary.VerificationUrl, link);
  }
  verificationLine_->set_markup(verification);
  verificationLine_->set_visible(!verification.empty());
  useDefaultBtn_->set_visible(summary.ShowUseDefault);
  changeLocationBtn_->set_visible(summary.ShowChangeLocation);
  statusActions_->set_visible(summary.ShowUseDefault || summary.ShowChangeLocation);
  // this connection's blocked lookups (I3), only with a device counting them
  const bool counted = summary.ShowBlockedCount && status_.has_value();
  blockedCountLine_->set_visible(counted);
  blockedFloorNote_->set_visible(counted);
  if (!counted) {
    StopBlockedCountPoll();
    return;
  }
  blockedCount_ = summary.BlockedCount;
  RenderBlockedCount();
  StartBlockedCountPoll();
}

void DnsSheet::RenderBlockedCount() {
  if (!blockedCountLine_) return;
  const std::string provider = statusSummary_ ? statusSummary_->ProviderName : std::string();
  blockedCountLine_->set_text(Format(TN_("dns_blocked_count",
                                         "{0} lookup blocked by {1} this connection",
                                         "{0} lookups blocked by {1} this connection",
                                         static_cast<unsigned long>(blockedCount_)),
                                     blockedCount_, provider));
}

void DnsSheet::StartBlockedCountPoll() {
  if (blockedCountPoll_.connected()) return;
  blockedCountPoll_ = Glib::signal_timeout().connect_seconds(
      sigc::track_obj(
          [this]() -> bool {
            // ends by returning false (never disconnect a slot from inside
            // itself) once the sheet or its count is gone
            if (!get_visible() || !blockedCountLine_) return false;
            blockedCount_ = host_.GetDnsBlockedCount();
            RenderBlockedCount();
            return true;
          },
          *this),
      kBlockedCountPollSeconds);
}

void DnsSheet::StopBlockedCountPoll() { blockedCountPoll_.disconnect(); }

}  // namespace urnw
