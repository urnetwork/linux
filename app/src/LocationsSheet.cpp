// SPDX-License-Identifier: MPL-2.0
#include "LocationsSheet.hpp"

#include "DisplayText.hpp"
#include "I18n.hpp"
#include "LocationRowName.hpp"
#include "LocationFeed.hpp"
#include "LocationSelection.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// A colored bullet used as a row's leading dot.
Gtk::Label* MakeColorDot(const Rgba& color) {
  auto* dot = Gtk::make_managed<Gtk::Label>();
  dot->set_valign(Gtk::Align::CENTER);
  dot->set_markup("<span foreground='" + HexForMarkup(color) + "'>●</span>");
  return dot;
}

// The dot color for a location row: countries key on the country code, everything
// else on its location/client/group id (mobile parity: solid colors, no flags).
Rgba LocationColor(const urnet::ConnectLocation& loc) {
  const Rgba fallback{0.5, 0.5, 0.5, 1.0};
  std::string code;
  if (loc.location_type && *loc.location_type == urnet::LocationTypeCountry &&
      loc.country_code && !loc.country_code->empty()) {
    code = *loc.country_code;
  } else if (loc.connect_location_id) {
    const auto& id = *loc.connect_location_id;
    if (id.location_id && !id.location_id->empty()) {
      code = *id.location_id;
    } else if (id.client_id && !id.client_id->empty()) {
      code = *id.client_id;
    } else if (id.location_group_id && !id.location_group_id->empty()) {
      code = *id.location_group_id;
    }
  }
  if (code.empty()) return fallback;
  return ParseHexColor(urnet::getColorHex(code), fallback);
}

// A trailing symbolic icon (glyphs on the right of a row). Decorative: the
// row's name says the state it shows.
Gtk::Image* MakeTrailingIcon(const std::string& iconName, const char* cssClass) {
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name(iconName);
  icon->set_valign(Gtk::Align::CENTER);
  if (cssClass) icon->add_css_class(cssClass);
  kit::MarkDecorative(*icon);
  return icon;
}

// A row shell: a button holding a leading color dot and a two-line text
// column (primary + optional caption), hoverable and pointer-cursored. A
// button, so a row is in the tab order, Enter or Space picks it and a screen
// reader hears a button. Its content is a box, which GTK names from nothing,
// so the caller names the button; the dot, the text and the trailing glyphs
// it says are hidden, so nothing is read twice. Callers append trailing
// glyphs to `content` and connect the click.
struct RowShell {
  Gtk::Button* button = nullptr;
  Gtk::Box* content = nullptr;
};

RowShell MakeRowShell(Gtk::Label* dot, const std::string& primary, const std::string& caption) {
  RowShell shell;
  shell.button = Gtk::make_managed<Gtk::Button>();
  shell.button->add_css_class("ur-card-tappable");
  shell.button->set_margin_top(2);
  shell.button->set_margin_bottom(2);
  SetPointerCursor(*shell.button);
  shell.content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  shell.button->set_child(*shell.content);
  kit::MarkDecorative(*dot);
  shell.content->append(*dot);

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  column->set_hexpand(true);
  kit::MarkDecorative(*column);
  auto* name = Gtk::make_managed<Gtk::Label>(primary);
  name->set_xalign(0);
  name->set_ellipsize(Pango::EllipsizeMode::END);
  column->append(*name);
  if (!caption.empty()) {
    auto* captionLabel = Gtk::make_managed<Gtk::Label>(caption);
    captionLabel->add_css_class("dim-label");
    captionLabel->add_css_class("caption");
    captionLabel->set_xalign(0);
    column->append(*captionLabel);
  }
  shell.content->append(*column);
  return shell;
}

// The words a row's name reads for its glyphs: the Network page's.
LocationRowWords RowWords() {
  return LocationRowWords{
      T_("unstable_providers_warning", "* (may be unstable)"),
      T_("strong_anonymization", "Strong Anonymization"),
      T_("network_peers", "Network peers"),
      T_("selected_provider", "Selected provider"),
  };
}

}  // namespace

LocationsSheet::LocationsSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_("browse_locations", "Browse Locations"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 600);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  root->set_margin(16);
  set_child(*root);

  // fixed search box above the scrolling sections (mobile parity)
  searchEntry_.set_placeholder_text(
      T_("search_providers_input_placeholder", "Search countries, states, cities..."));
  searchEntry_.signal_search_changed().connect(
      sigc::mem_fun(*this, &LocationsSheet::OnSearchChanged));
  root->append(searchEntry_);

  statusLabel_.add_css_class("dim-label");
  statusLabel_.set_xalign(0);
  statusLabel_.set_wrap(true);
  statusLabel_.set_visible(false);
  root->append(statusLabel_);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_vexpand(true);
  scroller->set_child(sectionsBox_);
  root->append(*scroller);
}

void LocationsSheet::Open() {
  // The SDK filter is shared with the Network destination and outlives the
  // sheet: a search left in it there would fill this sheet with that search's
  // buckets under this box. Reapplying the box's query also opens the feeds
  // and retries a failed load.
  host_.FilterLocations(currentQuery_);
  Refresh();
  present();
}

void LocationsSheet::Refresh() { RebuildSections(); }

void LocationsSheet::OnSearchChanged() {
  currentQuery_ = searchEntry_.get_text().raw();
  // The SDK debounces stale responses; it re-emits FilteredLocations (-> a
  // Locations drawer event -> Refresh) with the new results.
  host_.FilterLocations(currentQuery_);
  // The first push can wait behind a connect; the sections follow the box now.
  Refresh();
}

void LocationsSheet::AppendLocationSection(
    const std::string& title, const std::optional<urnet::ConnectLocationList>& items,
    const std::optional<urnet::ConnectLocation>& selected) {
  if (!items || items->empty()) return;
  sectionsBox_.append(*MakeCaption(title));
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  for (const auto& loc : *items) {
    box->append(*MakeLocationRow(loc, IsLocationSelected(selected, loc)));
  }
  sectionsBox_.append(*box);
}

Gtk::Button* LocationsSheet::MakeLocationRow(const urnet::ConnectLocation& location,
                                             bool selected) {
  const int providerCount = location.provider_count.value_or(0);
  const std::string caption =
      0 < providerCount ? Format(TN_("provider_count", "{} provider", "{} providers",
                                     static_cast<unsigned long>(providerCount)),
                                 providerCount)
                        : std::string();
  const std::string name = SanitizeExternalDisplayText(location.name.value_or(std::string()));
  auto row = MakeRowShell(MakeColorDot(LocationColor(location)), name, caption);

  if (!location.stable) row.content->append(*MakeTrailingIcon("dialog-warning-symbolic", nullptr));
  if (location.strong_privacy) {
    row.content->append(*MakeTrailingIcon("security-high-symbolic", "ur-value-on"));
  }
  if (selected) row.content->append(*MakeTrailingIcon("object-select-symbolic", nullptr));
  LocationRowStates states;
  states.unstable = !location.stable;
  states.strongPrivacy = location.strong_privacy;
  states.selected = selected;
  kit::SetAccessibleLabel(*row.button, LocationRowName(name, caption, states, RowWords()));

  const urnet::ConnectLocation locationCopy = location;
  row.button->signal_clicked().connect([this, locationCopy] {
    host_.ConnectFromRow(locationCopy);
    set_visible(false);  // dismiss on connect (iOS/Android parity)
  });
  return row.button;
}

Gtk::Button* LocationsSheet::MakePeerRow(const urnet::NetworkPeer& peer, bool selected) {
  const Rgba fallback{0.5, 0.5, 0.5, 1.0};
  auto* dot = MakeColorDot(ParseHexColor(urnet::getColorHex(peer.ClientId.value_or("")), fallback));
  // secondary line = the device spec, but only when a distinct name is shown too
  const std::string caption = (!peer.DeviceName.empty() && !peer.DeviceSpec.empty())
                                  ? SanitizeExternalDisplayText(peer.DeviceSpec)
                                  : std::string();
  const std::string name = PeerDisplayName(peer);
  auto row = MakeRowShell(dot, name, caption);

  // the green "providing to network" glyph, always present on a peer row
  row.content->append(*MakeTrailingIcon("network-transmit-receive-symbolic", "ur-value-on"));
  if (selected) row.content->append(*MakeTrailingIcon("object-select-symbolic", nullptr));
  LocationRowStates states;
  states.providing = true;
  states.selected = selected;
  kit::SetAccessibleLabel(*row.button, LocationRowName(name, caption, states, RowWords()));

  const urnet::NetworkPeer peerCopy = peer;
  row.button->signal_clicked().connect([this, peerCopy] {
    // one of the user's own devices, reached as a network peer (PeerLocation.hpp)
    host_.ConnectFromRow(PeerConnectLocation<urnet::ConnectLocation>(peerCopy));
    set_visible(false);
  });
  return row.button;
}

Gtk::Button* LocationsSheet::MakeBestAvailableRow(bool selected) {
  const std::string name = T_("best_available_provider", "Best available provider");
  auto row = MakeRowShell(MakeColorDot(kUrCoral), name, std::string());
  if (selected) row.content->append(*MakeTrailingIcon("object-select-symbolic", nullptr));
  LocationRowStates states;
  states.selected = selected;
  kit::SetAccessibleLabel(*row.button, LocationRowName(name, {}, states, RowWords()));

  row.button->signal_clicked().connect([this] {
    host_.ConnectFromRow(std::nullopt);
    set_visible(false);
  });
  return row.button;
}

void LocationsSheet::RebuildSections() {
  RemoveAllChildren(sectionsBox_);

  const auto selected = host_.SelectedLocation();
  const bool searching = !TrimLocationQuery(currentQuery_).empty();

  // 1. network peers (pinned first; self-hides when there are none)
  const auto peers = host_.ConnectedProvidePeers();
  const int peerCount = peers ? static_cast<int>(peers->size()) : 0;
  if (0 < peerCount) {
    sectionsBox_.append(*MakeCaption(T_("network_peers", "Network peers")));
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    for (const auto& peer : *peers) {
      box->append(*MakePeerRow(peer, IsPeerSelected(selected, peer)));
    }
    sectionsBox_.append(*box);
  }

  const auto filtered = host_.GetFilteredLocations();

  // 2. searching -> best search matches; idle -> the single best-available row
  //    (both apps ignore the SDK Promoted list; the header is just a label)
  if (searching) {
    if (filtered) AppendLocationSection(T_("top_matches", "Top Matches"), filtered->BestMatches, selected);
  } else {
    sectionsBox_.append(*MakeCaption(T_("promoted_locations", "Promoted Locations")));
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    box->append(*MakeBestAvailableRow(IsBestAvailableSelected(selected)));
    sectionsBox_.append(*box);
  }

  // 3. countries / regions / cities / devices (regions+cities only while searching)
  if (filtered) {
    AppendLocationSection(T_("countries", "Countries"), filtered->Countries, selected);
    AppendLocationSection(T_("regions", "Regions"), filtered->Regions, selected);
    AppendLocationSection(T_("cities", "Cities"), filtered->Cities, selected);
    AppendLocationSection(T_("devices", "Devices"), filtered->Devices, selected);
  }

  // The feed's state, when the sections do not answer the query. BestMatches
  // only renders while searching, so it only counts then.
  auto nonEmpty = [](const std::optional<urnet::ConnectLocationList>& list) {
    return list && !list->empty();
  };
  const bool anyLocation =
      filtered && ((searching && nonEmpty(filtered->BestMatches)) ||
                   nonEmpty(filtered->Countries) || nonEmpty(filtered->Regions) ||
                   nonEmpty(filtered->Cities) || nonEmpty(filtered->Devices));
  LocationFeedNotice notice = LocationFeedNotice::None;
  if (host_.hasDevice()) {
    notice = ChooserFeedNotice(host_.GetFilteredLocationState(), filtered.has_value(),
                               anyLocation || 0 < peerCount, searching);
  } else if (searching && !anyLocation && peerCount == 0) {
    // no feed without a device: only the search can come up empty
    notice = LocationFeedNotice::NoResults;
  }
  std::string text;
  switch (notice) {
    case LocationFeedNotice::None:
      break;
    case LocationFeedNotice::Loading:
      text = T_("loading", "Loading...");
      break;
    case LocationFeedNotice::Failed:
      text = T_("adv_providers_load_failed", "Could not load the provider list.");
      break;
    case LocationFeedNotice::NoResults:
      text = T_("no_providers_found", "We could not find any providers.");
      break;
  }
  statusLabel_.set_text(text);
  statusLabel_.set_visible(!text.empty());
}

}  // namespace urnw
