// SPDX-License-Identifier: MPL-2.0
#include "LocationsSheet.hpp"

#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// A colored bullet used as a row's leading dot (same markup form the drawer's
// selected-location dot uses).
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

bool SameId(const std::optional<std::string>& a, const std::optional<std::string>& b) {
  return a && b && !a->empty() && *a == *b;
}

bool IsBestAvailableSelected(const std::optional<urnet::ConnectLocation>& selected) {
  return !selected || (selected->connect_location_id &&
                       selected->connect_location_id->best_available.value_or(false));
}

bool IsPeerSelected(const std::optional<urnet::ConnectLocation>& selected,
                    const urnet::NetworkPeer& peer) {
  if (!selected || !selected->connect_location_id) return false;
  return SameId(selected->connect_location_id->client_id, peer.ClientId);
}

bool IsLocationSelected(const std::optional<urnet::ConnectLocation>& selected,
                        const urnet::ConnectLocation& loc) {
  if (!selected || !selected->connect_location_id || !loc.connect_location_id) return false;
  const auto& a = *selected->connect_location_id;
  const auto& b = *loc.connect_location_id;
  return SameId(a.location_id, b.location_id) || SameId(a.client_id, b.client_id) ||
         SameId(a.location_group_id, b.location_group_id);
}

// A trailing symbolic icon (glyphs on the right of a row).
Gtk::Image* MakeTrailingIcon(const std::string& iconName, const char* cssClass) {
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name(iconName);
  icon->set_valign(Gtk::Align::CENTER);
  if (cssClass) icon->add_css_class(cssClass);
  return icon;
}

// A row shell: leading color dot + a two-line text column (primary + optional
// caption), hoverable and pointer-cursored. Callers append trailing glyphs and a
// click gesture.
Gtk::Box* MakeRowShell(Gtk::Label* dot, const std::string& primary, const std::string& caption) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  row->set_margin_top(2);
  row->set_margin_bottom(2);
  row->add_css_class("ur-card-tappable");
  SetPointerCursor(*row);
  row->append(*dot);

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  column->set_hexpand(true);
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
  row->append(*column);
  return row;
}

}  // namespace

std::string PeerDisplayName(const urnet::NetworkPeer& peer) {
  if (!peer.DeviceName.empty()) return peer.DeviceName;
  if (!peer.DeviceSpec.empty()) return peer.DeviceSpec;
  return peer.ClientId.value_or(std::string());
}

LocationsSheet::LocationsSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_("browse_locations", "Browse locations"));
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
  Refresh();
  present();
}

void LocationsSheet::Refresh() { RebuildSections(); }

void LocationsSheet::OnSearchChanged() {
  currentQuery_ = searchEntry_.get_text().raw();
  // The SDK debounces stale responses; it re-emits FilteredLocations (-> a
  // Locations drawer event -> Refresh) with the new results.
  host_.FilterLocations(currentQuery_);
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

Gtk::Box* LocationsSheet::MakeLocationRow(const urnet::ConnectLocation& location, bool selected) {
  const int providerCount = location.provider_count.value_or(0);
  const std::string caption =
      0 < providerCount ? Format(T_("provider_count_int", "{} providers"), providerCount)
                        : std::string();
  auto* row = MakeRowShell(MakeColorDot(LocationColor(location)),
                           location.name.value_or(std::string()), caption);

  if (!location.stable) row->append(*MakeTrailingIcon("dialog-warning-symbolic", nullptr));
  if (location.strong_privacy) {
    row->append(*MakeTrailingIcon("security-high-symbolic", "ur-value-on"));
  }
  if (selected) row->append(*MakeTrailingIcon("object-select-symbolic", nullptr));

  auto gesture = Gtk::GestureClick::create();
  const urnet::ConnectLocation locationCopy = location;
  gesture->signal_released().connect([this, locationCopy](int, double, double) {
    host_.Connect(locationCopy);
    set_visible(false);  // dismiss on connect (iOS/Android parity)
  });
  row->add_controller(gesture);
  return row;
}

Gtk::Box* LocationsSheet::MakePeerRow(const urnet::NetworkPeer& peer, bool selected) {
  const Rgba fallback{0.5, 0.5, 0.5, 1.0};
  auto* dot = MakeColorDot(ParseHexColor(urnet::getColorHex(peer.ClientId.value_or("")), fallback));
  // secondary line = the device spec, but only when a distinct name is shown too
  const std::string caption =
      (!peer.DeviceName.empty() && !peer.DeviceSpec.empty()) ? peer.DeviceSpec : std::string();
  auto* row = MakeRowShell(dot, PeerDisplayName(peer), caption);

  // the green "providing to network" glyph, always present on a peer row
  row->append(*MakeTrailingIcon("network-transmit-receive-symbolic", "ur-value-on"));
  if (selected) row->append(*MakeTrailingIcon("object-select-symbolic", nullptr));

  auto gesture = Gtk::GestureClick::create();
  const urnet::NetworkPeer peerCopy = peer;
  gesture->signal_released().connect([this, peerCopy](int, double, double) {
    urnet::ConnectLocation location;
    urnet::ConnectLocationId id;
    id.client_id = peerCopy.ClientId;
    location.connect_location_id = id;
    location.name = PeerDisplayName(peerCopy);
    host_.Connect(location);
    set_visible(false);
  });
  row->add_controller(gesture);
  return row;
}

Gtk::Box* LocationsSheet::MakeBestAvailableRow(bool selected) {
  auto* row = MakeRowShell(MakeColorDot(kUrCoral),
                           T_("best_available_provider", "Best available provider"), std::string());
  if (selected) row->append(*MakeTrailingIcon("object-select-symbolic", nullptr));

  auto gesture = Gtk::GestureClick::create();
  gesture->signal_released().connect([this](int, double, double) {
    host_.ConnectBestAvailable();
    set_visible(false);
  });
  row->add_controller(gesture);
  return row;
}

void LocationsSheet::RebuildSections() {
  RemoveAllChildren(sectionsBox_);

  const auto selected = host_.SelectedLocation();
  const bool searching = !currentQuery_.empty();

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
    if (filtered) AppendLocationSection(T_("top_matches", "Top matches"), filtered->BestMatches, selected);
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

  // no-results text: only while searching and with nothing at all to show
  // (peers are included in the check, unlike the android original)
  auto nonEmpty = [](const std::optional<urnet::ConnectLocationList>& list) {
    return list && !list->empty();
  };
  const bool anyLocation =
      filtered && (nonEmpty(filtered->BestMatches) || nonEmpty(filtered->Countries) ||
                   nonEmpty(filtered->Regions) || nonEmpty(filtered->Cities) ||
                   nonEmpty(filtered->Devices));
  if (searching && !anyLocation && peerCount == 0) {
    statusLabel_.set_text(T_("no_providers_found", "We could not find any providers."));
    statusLabel_.set_visible(true);
  } else {
    statusLabel_.set_visible(false);
  }
}

}  // namespace urnw
