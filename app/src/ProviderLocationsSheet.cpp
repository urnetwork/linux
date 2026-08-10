// SPDX-License-Identifier: MPL-2.0
#include "ProviderLocationsSheet.hpp"

#include <algorithm>
#include <cmath>

#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// Row dot geometry, in px. The box is sized for the ring so the column width
// never changes with selection, and the ring sits kDotRingGap outside the solid
// dot's edge (radii are stroke centerlines).
constexpr double kDotDiameter = 20.0;
constexpr double kDotRingGap = 4.0;
constexpr double kDotRingStroke = 1.5;
constexpr int kDotBox = static_cast<int>(kDotDiameter + 2 * (kDotRingGap + kDotRingStroke));
// the row's horizontal padding, mirrored as the gap between the dot and the
// detail column so the dot reads as centered in its column
constexpr int kRowPadding = 16;
// the web globe's fallback for a provider whose country is unknown
constexpr Rgba kUnknownCountry{0x00 / 255.0, 0x99 / 255.0, 0xFF / 255.0, 1.0};

// The country color for a provider, from the SDK's shared palette (the same one
// the locations chooser uses). getColorHex wants the lowercased ISO-2 code.
Rgba ProviderColor(const std::string& countryCode) {
  if (countryCode.empty()) return kUnknownCountry;
  std::string lower = countryCode;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ParseHexColor(urnet::getColorHex(lower), kUnknownCountry);
}

// One provider's dot: a fixed-size box with the solid country-color dot at the
// top, plus the selection ring.
class ProviderDotArea : public Gtk::DrawingArea {
 public:
  ProviderDotArea() {
    set_content_width(kDotBox);
    set_content_height(kDotBox);
    set_valign(Gtk::Align::START);
    set_draw_func(sigc::mem_fun(*this, &ProviderDotArea::OnDraw));
  }

  void SetColor(const Rgba& color) {
    color_ = color;
    queue_draw();
  }
  void SetSelected(bool selected) {
    if (selected_ == selected) return;
    selected_ = selected;
    queue_draw();
  }

 private:
  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    cr->set_source_rgba(color_.r, color_.g, color_.b, color_.a);
    cr->arc(cx, cy, kDotDiameter / 2.0, 0, 2 * G_PI);
    cr->fill();
    if (selected_) {
      cr->arc(cx, cy, kDotDiameter / 2.0 + kDotRingGap + kDotRingStroke / 2.0, 0, 2 * G_PI);
      cr->set_line_width(kDotRingStroke);
      cr->stroke();
    }
  }

  Rgba color_{0.5, 0.5, 0.5, 1.0};
  bool selected_ = false;
};

std::string FormatConnectedDuration(int64_t connectedSinceMillis, int64_t nowMillis) {
  if (connectedSinceMillis <= 0) return std::string();
  const int64_t elapsedSeconds = std::max<int64_t>(0, (nowMillis - connectedSinceMillis) / 1000);
  const int64_t hours = elapsedSeconds / 3600;
  const int64_t minutes = (elapsedSeconds % 3600) / 60;
  const int64_t seconds = elapsedSeconds % 60;
  if (0 < hours) {
    return Format(T_("provider_connected_duration_hours", "{0}h {1}m"), hours, minutes);
  }
  if (0 < minutes) {
    return Format(T_("provider_connected_duration_minutes", "{}m"), minutes);
  }
  return Format(T_("provider_connected_duration_seconds", "{}s"), seconds);
}

int64_t NowMillis() { return g_get_real_time() / 1000; }

}  // namespace

std::vector<ProviderLocationRow> MapConnectedProviderLocations(
    const urnet::ConnectedProviderLocationList& locations) {
  std::vector<ProviderLocationRow> rows;
  rows.reserve(locations.size());
  for (const auto& location : locations) {
    ProviderLocationRow row;
    row.clientId = location.ClientId.value_or(std::string());
    row.country = location.Country;
    row.countryCode = location.CountryCode;
    row.region = location.Region;
    row.city = location.City;
    row.hasLocation = location.HasLocation;
    // plot the city centroid when the server knows it, else the region centroid
    if (location.HasCityCoordinates) {
      row.hasCoordinates = true;
      row.lat = location.CityLat;
      row.lon = location.CityLon;
    } else if (location.HasRegionCoordinates) {
      row.hasCoordinates = true;
      row.lat = location.RegionLat;
      row.lon = location.RegionLon;
    }
    row.connectedSinceMillis = location.ConnectedSinceMillis;
    rows.push_back(std::move(row));
  }
  return rows;
}

ProviderLocationsSheet::ProviderLocationsSheet(Gtk::Window& parent, SdkHost& host,
                                               LocationOverrideController* locationOverride)
    : host_(host), locationOverride_(locationOverride) {
  EnsureDrawerCss();
  set_title(T_("provider_locations_title", "Provider Locations"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(460, 720);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  toastOverlay_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  gtk_window_set_child(GTK_WINDOW(gobj()), GTK_WIDGET(toastOverlay_));

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  adw_toast_overlay_set_child(toastOverlay_, GTK_WIDGET(root->gobj()));

  BuildHeader(*root);
  if (locationOverride_ != nullptr) BuildOverrideSection(*root);

  // the globe is fixed; only the list below it scrolls
  globe_ = Gtk::make_managed<ProviderGlobe>();
  globe_->SetColorResolver(&ProviderColor);
  globe_->on_select = [this](const std::string& clientId) { Select(clientId); };
  globe_->set_margin_top(8);
  globe_->set_margin_bottom(8);
  root->append(*globe_);

  statusLabel_ = Gtk::make_managed<Gtk::Label>();
  statusLabel_->add_css_class("dim-label");
  statusLabel_->set_wrap(true);
  statusLabel_->set_justify(Gtk::Justification::CENTER);
  statusLabel_->set_margin(24);
  statusLabel_->set_visible(false);
  root->append(*statusLabel_);

  scroller_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller_->set_vexpand(true);
  listBox_.set_valign(Gtk::Align::START);
  scroller_->set_child(listBox_);
  root->append(*scroller_);

  if (locationOverride_ != nullptr) {
    locationOverride_->on_state_changed = [this] { RefreshOverrideSection(); };
  }
}

ProviderLocationsSheet::~ProviderLocationsSheet() {
  durationTick_.disconnect();
  if (locationOverride_ != nullptr) locationOverride_->on_state_changed = nullptr;
}

void ProviderLocationsSheet::BuildHeader(Gtk::Box& root) {
  // An explicit dismiss at the top left plus a small title, rather than relying
  // on the window decoration: this is a detail view over the home screen, and
  // the same control has to work when the window manager draws no titlebar.
  auto* header = Gtk::make_managed<Gtk::HeaderBar>();
  header->set_show_title_buttons(false);

  auto* back = Gtk::make_managed<Gtk::Button>();
  back->set_image_from_icon_name("go-previous-symbolic");
  back->add_css_class("flat");
  back->set_tooltip_text(T_("close", "Close"));
  back->signal_clicked().connect([this] { set_visible(false); });
  header->pack_start(*back);

  auto* title = Gtk::make_managed<Gtk::Label>(T_("provider_locations_title", "Provider Locations"));
  title->add_css_class("ur-caption-11");
  title->add_css_class("dim-label");
  header->set_title_widget(*title);

  root.append(*header);
}

void ProviderLocationsSheet::BuildOverrideSection(Gtk::Box& root) {
  overrideBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  overrideBox_->set_margin_start(kRowPadding);
  overrideBox_->set_margin_end(kRowPadding);
  overrideBox_->set_margin_top(12);

  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* label = Gtk::make_managed<Gtk::Label>(
      T_("sync_device_location_with_provider", "Sync device location with provider"));
  label->set_xalign(0);
  label->set_hexpand(true);
  label->set_wrap(true);
  row->append(*label);

  // info button -> what it does, what it covers, and a way into the guide
  auto* info = Gtk::make_managed<Gtk::MenuButton>();
  info->set_icon_name("help-about-symbolic");
  info->add_css_class("flat");
  {
    auto* popover = Gtk::make_managed<Gtk::Popover>();
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    content->set_margin(12);
    content->set_size_request(320, -1);
    for (const char* text : {
             T_("mock_location_guide_intro",
                "When enabled, apps on this device see the location of the provider you have "
                "been connected to the longest, instead of your real location."),
             T_("location_override_coverage_geoclue",
                "Apps that ask the system for your location through GeoClue — including GNOME "
                "Settings and Maps, Firefox, and sandboxed Flatpak and Snap apps — report the "
                "provider's location instead of yours."),
             T_("location_override_coverage_gaps",
                "It does not cover everything. Chrome and KDE Plasma never ask GeoClue, and "
                "any app that works out your location from your IP address on its own is "
                "unaffected. Firefox falls back to its own lookup if the system does not "
                "answer quickly."),
         }) {
      auto* line = Gtk::make_managed<Gtk::Label>(text);
      line->set_xalign(0);
      line->set_wrap(true);
      content->append(*line);
    }
    auto* guideBtn = Gtk::make_managed<Gtk::Button>(
        T_("mock_location_guide_title", "Sync device location"));
    guideBtn->add_css_class("flat");
    guideBtn->signal_clicked().connect([this, popover] {
      popover->popdown();
      if (!guide_) guide_ = std::make_unique<LocationOverrideGuide>(*this, locationOverride_);
      guide_->Open();
    });
    content->append(*guideBtn);
    popover->set_child(*content);
    info->set_popover(*popover);
  }
  row->append(*info);

  overrideSwitch_ = Gtk::make_managed<Gtk::Switch>();
  overrideSwitch_->set_valign(Gtk::Align::CENTER);
  overrideSwitch_->property_active().signal_changed().connect([this] {
    if (updatingOverrideSwitch_ || locationOverride_ == nullptr) return;
    const bool enabled = overrideSwitch_->get_active();
    locationOverride_->SetEnabled(enabled);
    // Turning it on with the machine already set up just works -- only an
    // incomplete setup opens the guide. This reads setupComplete(), not
    // `status`, which is Disabled while the toggle is off no matter how the
    // machine is configured.
    if (enabled && !locationOverride_->State().setupComplete()) {
      if (!guide_) guide_ = std::make_unique<LocationOverrideGuide>(*this, locationOverride_);
      guide_->Open();
    }
    RefreshOverrideSection();
  });
  row->append(*overrideSwitch_);
  overrideBox_->append(*row);

  // the note is always present: "use most stable provider" at rest, replaced by
  // the more specific status while the feature works through setup / waiting /
  // syncing / cleanup
  overrideNote_ = Gtk::make_managed<Gtk::Label>();
  overrideNote_->set_xalign(0);
  overrideNote_->set_wrap(true);
  overrideNote_->add_css_class("dim-label");
  overrideNote_->add_css_class("ur-caption-11");
  overrideBox_->append(*overrideNote_);

  root.append(*overrideBox_);
}

void ProviderLocationsSheet::Open() {
  // the setup signals are files on disk with no change notification, so
  // re-probe every time the sheet appears (android's ON_RESUME discipline)
  if (locationOverride_ != nullptr) locationOverride_->RefreshSignals();
  Refresh();
  if (!durationTick_.connected()) {
    // one timer for the whole list: the durations tick locally against the
    // absolute connected-since stamps rather than costing an SDK event each
    durationTick_ = Glib::signal_timeout().connect(
        sigc::mem_fun(*this, &ProviderLocationsSheet::OnDurationTick), 1000);
  }
  present();
  if (auto v = scroller_->get_vadjustment()) v->set_value(0);
}

bool ProviderLocationsSheet::OnDurationTick() {
  if (!get_visible()) {
    durationTick_.disconnect();
    return false;
  }
  UpdateDurations();
  return true;
}

std::vector<ProviderLocationRow> ProviderLocationsSheet::ReadRows() {
  auto list = host_.ConnectedProviderLocations();
  deviceAvailable_ = list.has_value();
  if (!list) return std::vector<ProviderLocationRow>();
  std::vector<ProviderLocationRow> rows = MapConnectedProviderLocations(*list);
  if (!pendingRemovals_.empty()) {
    // A provider the user just removed stays hidden until the SDK agrees, so
    // the row cannot flicker back during the round trip. Entries the SDK has
    // already dropped leave the set, so it cannot grow unbounded.
    std::set<std::string> stillPresent;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [this, &stillPresent](const ProviderLocationRow& row) {
                                if (pendingRemovals_.count(row.clientId) == 0) return false;
                                stillPresent.insert(row.clientId);
                                return true;
                              }),
               rows.end());
    pendingRemovals_.swap(stillPresent);
  }
  return rows;
}

void ProviderLocationsSheet::Refresh() {
  std::vector<ProviderLocationRow> rows = ReadRows();
  // Snapshot the verified-e2e identity set alongside the locations. The badge
  // depends on it, but it changes independently of the location rows (a
  // session verifying does not change a row's value), so it must be compared
  // separately or a newly sealed provider would never gain its badge until
  // some unrelated location change forced a rebuild.
  std::vector<IdentityRow> identityRows = ReadProviderIdentityRows(host_);
  const bool identitiesChanged = !SameIdentityRows(identityRows, identityRows_);
  // Dedupe by value. The SDK re-emits on every window turnover and returns
  // fresh objects each time, so without this the list would be torn down and
  // rebuilt constantly -- the same discipline the connect grid uses.
  const bool changed = rows != rows_ || identitiesChanged;
  if (identitiesChanged) {
    identityRows_ = std::move(identityRows);
    identityByClientId_.clear();
    for (const IdentityRow& identity : identityRows_) {
      identityByClientId_[identity.clientId] = &identity;
    }
  }
  if (changed) {
    rows_ = std::move(rows);
    // a selection whose provider left the window is dropped
    if (!selectedClientId_.empty() &&
        std::none_of(rows_.begin(), rows_.end(), [this](const ProviderLocationRow& row) {
          return row.clientId == selectedClientId_;
        })) {
      selectedClientId_.clear();
    }
    RebuildList();
    globe_->SetRows(rows_);
    globe_->SetSelected(selectedClientId_);
  }

  // The empty list means two very different things, and saying "no providers"
  // for both would be a lie: with the tunnel down there is no window to report
  // on at all, which gets the app's existing gray unavailable treatment.
  if (!deviceAvailable_) {
    statusLabel_->set_text(T_("provider_locations_unavailable",
                             "Provider details are unavailable until connected"));
    statusLabel_->set_visible(true);
  } else if (rows_.empty()) {
    statusLabel_->set_text(T_("provider_locations_empty", "No providers connected"));
    statusLabel_->set_visible(true);
  } else {
    statusLabel_->set_visible(false);
  }

  UpdateDurations();
  RefreshOverrideSection();
}

void ProviderLocationsSheet::RebuildList() {
  RemoveAllChildren(listBox_);
  rowWidgets_.assign(rows_.size(), RowWidgets{});
  for (size_t i = 0; i < rows_.size(); ++i) {
    listBox_.append(*BuildRow(i, rowWidgets_[i]));
  }
  UpdateSelection();
}

Gtk::Widget* ProviderLocationsSheet::BuildRow(size_t index, RowWidgets& out) {
  const ProviderLocationRow& row = rows_[index];

  auto* container = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kRowPadding);
  rowBox->set_margin_start(kRowPadding);
  rowBox->set_margin_end(kRowPadding);
  rowBox->set_margin_top(12);
  rowBox->set_margin_bottom(12);
  rowBox->add_css_class("ur-card-tappable");
  SetPointerCursor(*rowBox);

  auto* dot = Gtk::make_managed<ProviderDotArea>();
  dot->SetColor(ProviderColor(row.countryCode));
  out.dot = dot;
  rowBox->append(*dot);

  // four stacked labels, all left aligned flush to the dot column
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  column->set_hexpand(true);

  // the client id and, when the provider has a verified e2e session, its
  // identity identicon as a trailing badge. hexpand + ellipsize on the label
  // keeps a long id truncating instead of shoving the badge out; the badge is
  // created only on a join hit, so absence is the "not e2e" state (no
  // placeholder square). 6px gap mirrors android's badge spacer.
  auto* idRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  out.clientId = Gtk::make_managed<Gtk::Label>(row.clientId);
  out.clientId->add_css_class("ur-mono-11");
  out.clientId->set_xalign(0);
  out.clientId->set_hexpand(true);
  out.clientId->set_ellipsize(Pango::EllipsizeMode::END);
  out.clientId->set_tooltip_text(T_("copy_to_clipboard", "Copy to Clipboard"));
  SetPointerCursor(*out.clientId);
  {
    auto gesture = Gtk::GestureClick::create();
    const std::string clientId = row.clientId;
    gesture->signal_released().connect(
        [this, clientId](int, double, double) { CopyClientId(clientId); });
    out.clientId->add_controller(gesture);
  }
  idRow->append(*out.clientId);
  if (auto it = identityByClientId_.find(row.clientId); it != identityByClientId_.end()) {
    const IdentityRow* identity = it->second;
    auto* badge = Gtk::make_managed<IdenticonWidget>(kBadgeIdenticonSize);
    badge->SetPixbuf(cache_.Get(identity->key, identity->hash, kBadgeIdenticonSize));
    badge->set_valign(Gtk::Align::CENTER);
    badge->set_tooltip_text(T_("post_quantum_encryption", "Post Quantum Encryption"));
    out.pqBadge = badge;
    idRow->append(*badge);
  }
  column->append(*idRow);

  out.place = Gtk::make_managed<Gtk::Label>();
  out.place->set_xalign(0);
  out.place->set_ellipsize(Pango::EllipsizeMode::END);
  const std::string place = PlaceLabel(row);
  out.place->set_text(place.empty() ? T_("provider_location_unknown", "Location unknown") : place);
  column->append(*out.place);

  out.coordinates = Gtk::make_managed<Gtk::Label>(CoordinatesLabel(row));
  out.coordinates->set_xalign(0);
  out.coordinates->add_css_class("dim-label");
  out.coordinates->add_css_class("ur-caption-11");
  column->append(*out.coordinates);

  out.duration = Gtk::make_managed<Gtk::Label>();
  out.duration->set_xalign(0);
  out.duration->add_css_class("dim-label");
  out.duration->add_css_class("ur-caption-11");
  column->append(*out.duration);

  rowBox->append(*column);

  // Removal is an inline destructive button: there is no swipe convention on
  // the desktop, and a row that vanishes under a drag would be a surprise.
  out.remove = Gtk::make_managed<Gtk::Button>();
  out.remove->set_image_from_icon_name("window-close-symbolic");
  out.remove->add_css_class("flat");
  out.remove->add_css_class("destructive-action");
  out.remove->set_valign(Gtk::Align::START);
  out.remove->set_tooltip_text(T_("remove", "Remove"));
  {
    const std::string clientId = row.clientId;
    Gtk::Button* button = out.remove;
    out.remove->signal_clicked().connect([this, clientId, button] {
      button->set_sensitive(false);  // no second press while the removal is in flight
      RemoveProvider(clientId);
    });
  }
  rowBox->append(*out.remove);

  // selecting from the list drives the globe, and vice versa
  {
    auto gesture = Gtk::GestureClick::create();
    const std::string clientId = row.clientId;
    gesture->signal_released().connect(
        [this, clientId](int, double, double) { Select(clientId); });
    rowBox->add_controller(gesture);
  }

  container->append(*rowBox);
  container->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
  out.container = container;
  return container;
}

void ProviderLocationsSheet::UpdateSelection() {
  for (size_t i = 0; i < rows_.size() && i < rowWidgets_.size(); ++i) {
    const bool selected = rows_[i].clientId == selectedClientId_;
    if (auto* dot = dynamic_cast<ProviderDotArea*>(rowWidgets_[i].dot)) dot->SetSelected(selected);
    if (rowWidgets_[i].clientId != nullptr) {
      rowWidgets_[i].clientId->remove_css_class("ur-label-faint");
      if (!selected) rowWidgets_[i].clientId->add_css_class("ur-label-faint");
    }
  }
}

void ProviderLocationsSheet::UpdateDurations() {
  const int64_t now = NowMillis();
  for (size_t i = 0; i < rows_.size() && i < rowWidgets_.size(); ++i) {
    if (rowWidgets_[i].duration == nullptr) continue;
    rowWidgets_[i].duration->set_text(
        FormatConnectedDuration(rows_[i].connectedSinceMillis, now));
  }
}

void ProviderLocationsSheet::Select(const std::string& clientId) {
  if (selectedClientId_ == clientId) return;
  selectedClientId_ = clientId;
  UpdateSelection();
  globe_->SetSelected(clientId);
}

void ProviderLocationsSheet::CopyClientId(const std::string& clientId) {
  if (clientId.empty()) return;
  get_clipboard()->set_text(clientId);
  adw_toast_overlay_add_toast(toastOverlay_,
                              adw_toast_new(T_("client_id_copied", "Client ID copied")));
}

void ProviderLocationsSheet::RemoveProvider(const std::string& clientId) {
  if (clientId.empty()) return;
  // Optimistic: trim locally first so the row goes away immediately, and keep
  // it suppressed until the SDK stops reporting the provider.
  pendingRemovals_.insert(clientId);
  rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                             [&clientId](const ProviderLocationRow& row) {
                               return row.clientId == clientId;
                             }),
              rows_.end());
  if (selectedClientId_ == clientId) selectedClientId_.clear();
  RebuildList();
  globe_->SetRows(rows_);
  globe_->SetSelected(selectedClientId_);
  host_.RemoveConnectedProvider(clientId);
  RefreshOverrideSection();
}

void ProviderLocationsSheet::RefreshOverrideSection() {
  if (locationOverride_ == nullptr || overrideSwitch_ == nullptr) return;
  const LocationOverrideState state = locationOverride_->State();

  updatingOverrideSwitch_ = true;
  overrideSwitch_->set_active(state.enabled);
  updatingOverrideSwitch_ = false;

  std::string note;
  switch (state.status) {
    case LocationOverrideStatus::NeedsGeoClue:
    case LocationOverrideStatus::NeedsStaticSource:
    case LocationOverrideStatus::NeedsPrivilege:
      note = T_("mock_location_needs_setup", "Setup required");
      break;
    case LocationOverrideStatus::Eligible:
      note = T_("mock_location_waiting_for_provider", "Waiting for a provider location");
      break;
    case LocationOverrideStatus::Active:
      note = state.target.label.empty()
                 ? std::string(T_("use_most_stable_provider", "Use most stable provider"))
                 : Format(T_("mock_location_active", "Syncing with {}"), state.target.label);
      break;
    case LocationOverrideStatus::Orphaned:
      note = T_("location_override_cleanup_required",
                "URnetwork could not remove the system location file, so this machine may "
                "still report a provider's location. Start the URnetwork system service and "
                "turn this off, or remove /etc/geolocation as an administrator.");
      break;
    case LocationOverrideStatus::Disabled:
    case LocationOverrideStatus::ErrorTransient:
    default:
      note = T_("use_most_stable_provider", "Use most stable provider");
      break;
  }
  overrideNote_->set_text(note);
}

}  // namespace urnw
