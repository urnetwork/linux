// SPDX-License-Identifier: MPL-2.0
#include "NetworkPage.hpp"

#include <glib.h>

#include <utility>
#include <vector>

#include "I18n.hpp"
#include "LocationsSheet.hpp"  // PeerDisplayName — shared with the chooser
#include "UrTheme.hpp"

namespace urnw {
namespace {

// ---- selection predicates ---------------------------------------------------
// One identity question, one implementation: these duplicate the file-local
// predicates in LocationsSheet.cpp byte for byte (the windows client exports
// them from SdkHost.h so page, sheet and coalescer cannot disagree — hoist
// these into a shared header when the host grows the coalescer).

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

// ---- row color (§9) ---------------------------------------------------------
// Countries key on the country code; everything else on the first non-empty of
// location/client/group id; no key at all -> muted #989898. The palette comes
// from the SDK's getColorHex (Go countryCodeColorHexes + md5-mix fallback),
// never a re-implemented table.
Rgba LocationRowColor(const urnet::ConnectLocation& loc) {
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
  if (code.empty()) return kUrTextMuted;
  return ParseHexColor(urnet::getColorHex(code), kUrTextMuted);
}

// Paint the kit list-row dot (the 7px ● species) in the row's color.
void PaintRowDot(Gtk::Label& dot, const Rgba& color) {
  dot.set_markup("<span size='" + std::to_string(7 * PANGO_SCALE) + "' foreground='" +
                 HexForMarkup(color) + "'>●</span>");
}

// Go strings.TrimSpace parity for the api search path: a box of spaces is the
// idle list, not a search for " ".
std::string TrimSpace(const std::string& text) {
  static const char* kWs = " \t\n\v\f\r";
  const auto begin = text.find_first_not_of(kWs);
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(kWs);
  return text.substr(begin, end - begin + 1);
}

// MakePaneRow returns a bordered host; content goes in its inset first child.
Gtk::Box* RowInner(Gtk::Box* host) {
  return dynamic_cast<Gtk::Box*>(host->get_first_child());
}

// ---- preview sample (§10) ---------------------------------------------------

urnet::ConnectLocation SampleLocation(const std::string& name, const char* locationType,
                                      const std::string& countryCode, int providers,
                                      bool stable, bool privacy, bool promoted,
                                      const std::string& country, const std::string& region,
                                      const std::string& city) {
  // every field set explicitly so the detail pane exercises all of them
  urnet::ConnectLocation loc;
  urnet::ConnectLocationId id;
  id.location_id = "sample-" + std::string(locationType) + "-" + name;
  id.best_available = false;
  loc.connect_location_id = id;
  loc.name = name;
  loc.provider_count = providers;
  loc.promoted = promoted;
  loc.match_distance = 0;
  loc.location_type = locationType;
  loc.city = city;
  loc.region = region;
  loc.country = country;
  loc.country_code = countryCode;
  loc.city_location_id = city.empty() ? std::string() : "sample-city-" + city;
  loc.region_location_id = region.empty() ? std::string() : "sample-region-" + region;
  loc.country_location_id = "sample-country-" + countryCode;
  loc.stable = stable;
  loc.strong_privacy = privacy;
  loc.network_peer = false;
  return loc;
}

urnet::NetworkPeer SamplePeer(const std::string& name, const std::string& spec) {
  urnet::NetworkPeer peer;
  peer.ClientId = "peer-" + name;
  peer.ProvideEnabled = true;
  peer.Principal = name;
  peer.DeviceSpec = spec;
  peer.DeviceName = name;
  return peer;
}

}  // namespace

NetworkPage::NetworkPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  EnsureBrandCss();   // the pane-shell vocabulary
  EnsureDrawerCss();  // .ur-value-on (the green glyph tint)

  // ---- Pane A: the list takes the star (inverse of Home) --------------------
  paneA_ = kit::MakePane(T_("available_providers", "Available providers"));
  paneA_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneA_.root, T_("available_providers", "Available providers"));

  // Search row, built once, ABOVE the scrolled list (windows NetworkSearchHost
  // is a fixed grid row between the header strip and the ScrollViewer).
  search_ = kit::MakePaneSearchRow(
      T_("search_providers_input_placeholder", "Search countries, states, cities..."));
  // a placeholder is NOT an accessible name — the label has its own store key
  kit::SetAccessibleLabel(*search_.box, T_("search_providers_input_label", "Search providers"));
  search_.box->signal_changed().connect(sigc::mem_fun(*this, &NetworkPage::OnSearchChanged));
  paneA_.root->insert_child_after(*search_.root, *paneA_.header);

  listHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  paneA_.content->append(*listHost_);
  // The full-height empty overlay (§5.4). With the §5.3 inline feed-state line
  // present, this can no longer fire in any reachable state — ported anyway as
  // the markup-level guarantee that an empty pane centres its message instead
  // of showing a short line at the top.
  listEmptyHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  listEmptyHost_->set_vexpand(true);
  paneA_.content->append(*listEmptyHost_);
  append(*paneA_.root);

  paneBRule_ = kit::MakePaneVRule();
  append(*paneBRule_);

  // ---- Pane B: 400dip, not Home's 380 — the longest row is a country name
  // beside a provider count -----------------------------------------------
  paneB_ = kit::MakePane(T_("selected_provider", "Selected provider"));
  paneB_.root->set_size_request(400, -1);
  paneB_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneB_.root, T_("selected_provider", "Selected provider"));
  detailHost_ = paneB_.content;
  append(*paneB_.root);

  Render();
}

void NetworkPage::Load() {
  ++*epoch_;  // drop every async completion armed for the previous load
  ++apiGeneration_;
  apiInFlightQuery_.reset();
  // The auth/server space may have changed under us: forget the Loaded marker
  // so the next arming refetches (the previous buckets stay on screen beneath
  // LOADING — same as the view controller, which never blanks).
  apiLoadedQuery_.reset();
  navSelected_ = true;
  ReconcileFeeds();
}

void NetworkPage::SetSelected(bool selected) {
  navSelected_ = selected;
  ReconcileFeeds();
}

void NetworkPage::SetPresentationActive(bool active) {
  presentationActive_ = active;
  ReconcileFeeds();
}

void NetworkPage::ApplyBreakpoint(int widthDip) {
  const bool wide = widthDip >= static_cast<int>(kit::kWideBreakpointDip);
  if (lastWide_ == static_cast<int>(wide)) return;  // no-op unless the boolean flips
  lastWide_ = static_cast<int>(wide);
  // Below the breakpoint nothing is unreachable — clicking a row still connects.
  paneBRule_->set_visible(wide);
  paneB_.root->set_visible(wide);
}

// ---- feeds ------------------------------------------------------------------

void NetworkPage::ReconcileFeeds() {
  // Feed lifetime (§7.2): selection AND presentation edges both land here.
  // History to keep: the host tears the presentation-scoped view controllers
  // down when the presentation stops and the api cache must survive that —
  // returning to the destination you left on must never find an empty pane.
  if (navSelected_ && presentationActive_) {
    if (!samplePinned_) {
      if (host_.hasDevice()) {
        // Device path: SdkHost opens the locations/peer view controllers with
        // the presentation (SubscribeDrawer); seed from the current snapshot.
        // The seed is EMPTY by construction right after arming — it settles
        // the "loading" state, it does not fetch.
        locations_ = host_.GetFilteredLocations();
        locationsState_ = host_.GetFilteredLocationState();
        peers_ = host_.ConnectedProvidePeers();
      } else {
        // No device: peers genuinely need a session (nullopt), but the
        // provider list is public — arm the api fallback. A LOCATIONS_ERROR
        // cache is deliberately not "good": this arming IS the retry.
        peers_ = host_.ConnectedProvidePeers();
        EnsureApiLocations();
      }
    }
    Render();
  } else if (navSelected_) {
    Render();
  }
}

void NetworkPage::EnsureApiLocations() {
  if (samplePinned_ || host_.hasDevice()) return;
  const std::string query = apiTargetQuery_;
  // cache rules: skip if a fetch for this exact query is in flight, or the
  // cache is LOADED for this exact query. An error cache is not good.
  if (apiInFlightQuery_ && *apiInFlightQuery_ == query) return;
  if (apiLoadedQuery_ && *apiLoadedQuery_ == query &&
      locationsState_ == urnet::LocationsLoaded && locations_) {
    return;
  }
  apiInFlightQuery_ = query;
  // publish LOADING immediately; the previous result stays underneath (the
  // view controller re-pushes its snapshot with LOADING rather than blanking)
  locationsState_ = urnet::LocationsLoading;
  Render();

  const uint64_t generation = ++apiGeneration_;
  const auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  // No app-side timer anywhere on this surface: staleness is this generation
  // counter (api path) / per-query sequence numbers (device path).
  auto done = [this, epoch, seen, generation, query](
                  std::optional<urnet::FindLocationsResult> result,
                  std::optional<std::string> err) {
    PostToMain([this, epoch, seen, generation, query, result = std::move(result),
                err = std::move(err)] {
      if (*epoch != seen) return;             // page re-loaded since
      if (generation != apiGeneration_) return;  // superseded query
      if (samplePinned_) return;              // pinned sample must not be clobbered
      apiInFlightQuery_.reset();
      if (!result || err) {
        // the last good buckets stay on screen; only the state records the
        // failure — the next arming (destination re-entry, window return,
        // next search) is the retry
        locationsState_ = urnet::LocationsError;
      } else {
        // bucket with the query the RESULT answers, not the current box text
        locations_ = urnet::getFilteredLocationsFromResult(result, query);
        locationsState_ = urnet::LocationsLoaded;
        apiLoadedQuery_ = query;
      }
      Render();
    });
  };
  if (query.empty()) {
    host_.api().getProviderLocations(done);
  } else {
    urnet::FindLocationsArgs args;
    args.query = query;
    host_.api().findProviderLocations(args, done);
  }
}

NetworkPage::FeedState NetworkPage::CurrentFeedState() const {
  // State string checked BEFORE the snapshot: a LOCATIONS_LOADING push carries
  // JSON null — an engaged FilteredLocations whose six buckets are all
  // nullopt, byte-identical to "loaded, zero providers". Only the state
  // string separates them.
  if (locationsState_ == urnet::LocationsError) return FeedState::Failed;
  if (locationsState_ == urnet::LocationsLoading) return FeedState::Loading;
  if (!locations_) return FeedState::Loading;  // no snapshot yet
  // engaged snapshot + empty/unrecognized state string => Loaded (the code,
  // not the windows comment, is what this ports)
  return FeedState::Loaded;
}

void NetworkPage::OnLocationsEvent() {
  if (samplePinned_) return;  // real pushes must not blank the pinned sample
  if (!host_.hasDevice()) return;  // the api fallback publishes for itself
  // store BOTH — the state is the only separator of the three empty screens
  locations_ = host_.GetFilteredLocations();
  locationsState_ = host_.GetFilteredLocationState();
  Render();
}

void NetworkPage::OnPeersEvent() {
  if (samplePinned_) return;
  if (!host_.hasDevice()) return;
  peers_ = host_.ConnectedProvidePeers();
  Render();
}

void NetworkPage::OnSearchChanged() {
  // Every keystroke, NO app-side timer debounce (§3).
  query_ = search_.box->get_text().raw();
  if (!samplePinned_) {
    if (host_.hasDevice()) {
      // the SDK owns the search (server-side narrowing; per-query sequence
      // numbers drop late answers); results re-emit as a Locations event
      host_.FilterLocations(query_);
    } else {
      apiTargetQuery_ = TrimSpace(query_);  // Go strings.TrimSpace parity
      EnsureApiLocations();                 // dedupes unchanged/in-flight queries
    }
  }
  // Render immediately: the idle/searching branch (best-available row vs Top
  // Matches) is app-side and must flip without waiting for a push (a no-op
  // filter change produces none).
  Render();
}

// ---- Pane A -----------------------------------------------------------------

void NetworkPage::AppendGroup(Gtk::Box& into, const Glib::ustring& title, int count) {
  // the count meta collapses when <= 0 (the idle "Promoted Locations" header
  // is deliberately count 0 — it just labels the single synthetic row)
  into.append(*kit::MakePaneGroupHeader(
                   title, count > 0 ? Glib::ustring(std::to_string(count)) : Glib::ustring())
                   .root);
}

Gtk::Button* NetworkPage::MakeRow(const Glib::ustring& title, const Glib::ustring& meta,
                                  const Rgba& dotColor, bool selected, bool unstable,
                                  bool strongPrivacy, bool providing) {
  auto row = kit::MakePaneListRowButton(36);  // Home's list rows are 36, so are these
  PaintRowDot(*row.dot, dotColor);
  row.title->set_text(title);
  kit::SetTextOrCollapse(*row.meta, meta);

  // State glyphs between name and figure, in the spec's fixed order. The
  // segoe glyphs have no licensed twin here — symbolic icons stand in
  // (providing globe / unstable warning / strong-privacy lock / selected
  // check; green tint via .ur-value-on where windows paints #87FB67).
  if (providing || unstable || strongPrivacy || selected) {
    auto* glyphs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    glyphs->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*glyphs);  // color restates what the row's name says
    auto addGlyph = [glyphs](const char* iconName, const char* cssClass) {
      auto* icon = Gtk::make_managed<Gtk::Image>();
      icon->set_from_icon_name(iconName);
      icon->set_pixel_size(14);
      icon->set_valign(Gtk::Align::CENTER);
      if (cssClass) icon->add_css_class(cssClass);
      glyphs->append(*icon);
    };
    if (providing) addGlyph("network-transmit-receive-symbolic", "ur-value-on");
    if (unstable) addGlyph("dialog-warning-symbolic", nullptr);
    if (strongPrivacy) addGlyph("security-high-symbolic", "ur-value-on");
    if (selected) addGlyph("object-select-symbolic", nullptr);
    if (auto* grid = dynamic_cast<Gtk::Box*>(row.title->get_parent())) {
      grid->insert_child_after(*glyphs, *row.title);
    }
  }

  // The button's content is a box => no automatic name; it carries the whole
  // row (title, meta, then the state words; the selected state rides the name
  // too — GTK has no separate FullDescription surface here).
  Glib::ustring name = title;
  if (!meta.empty()) name += ", " + meta;
  if (unstable) name += Glib::ustring(", ") + T_("unstable_providers_warning", "* (may be unstable)");
  if (strongPrivacy) name += Glib::ustring(", ") + T_("strong_anonymization", "Strong Anonymization");
  if (providing) name += Glib::ustring(", ") + T_("network_peers", "Network peers");
  if (selected) name += Glib::ustring(", ") + T_("selected_provider", "Selected provider");
  kit::SetAccessibleLabel(*row.root, name);
  return row.root;
}

void NetworkPage::AppendLocationSection(
    const Glib::ustring& title, const std::optional<urnet::ConnectLocationList>& items,
    const std::optional<urnet::ConnectLocation>& selected, int& total) {
  if (!items || items->empty()) return;  // self-hides on empty
  AppendGroup(*listHost_, title, static_cast<int>(items->size()));
  for (const auto& location : *items) {
    const int providerCount = static_cast<int>(location.provider_count.value_or(0));
    const Glib::ustring meta =
        providerCount > 0
            ? Glib::ustring(Format(
                  TN_("provider_count", "{} provider", "{} providers", providerCount),
                  providerCount))
            : Glib::ustring();
    auto* row = MakeRow(location.name ? Glib::ustring(*location.name) : Glib::ustring(),
                        meta, LocationRowColor(location),
                        IsLocationSelected(selected, location), !location.stable,
                        location.strong_privacy, /*providing=*/false);
    const urnet::ConnectLocation copy = location;
    row->signal_clicked().connect([this, copy] {
      // The click IS select-and-connect. TODO(sdk-wiring):
      // SdkHost::ConnectFromRow — the windows row-click coalescer (1200ms
      // settle in the session worker; re-click of the active target is a
      // no-op that cancels a newer pending intent; immediate
      // connect/disconnect supersedes). Rationale: every real connect tears
      // the provider window down and the dial staircase charges 100ms–1s of
      // shared budget per cold dial with no refund — a click burst without
      // the settle runs it minutes ahead. Immediate Connect() until the host
      // grows the coalescer.
      host_.Connect(copy);
      // The SDK persists selection when the (settled) intent fires — the
      // check glyph may move only once status pushes arrive. Deliberately no
      // optimistic local highlight.
      Render();
    });
    listHost_->append(*row);
  }
  total += static_cast<int>(items->size());
}

void NetworkPage::Render() {
  RemoveAllChildren(*listHost_);
  const auto selected = host_.SelectedLocation();
  const bool searching = !query_.empty();
  int total = 0;

  // Group 1 — network peers (only when the feed has any)
  const int peerCount = peers_ ? static_cast<int>(peers_->size()) : 0;
  if (peerCount > 0) {
    AppendGroup(*listHost_, T_("network_peers", "Network peers"), peerCount);
    for (const auto& peer : *peers_) {
      auto* row = MakeRow(PeerDisplayName(peer), peer.DeviceSpec,
                          ParseHexColor(urnet::getColorHex(peer.ClientId.value_or("")),
                                        kUrTextMuted),
                          IsPeerSelected(selected, peer), false, false, /*providing=*/true);
      const urnet::NetworkPeer copy = peer;
      row->signal_clicked().connect([this, copy] {
        urnet::ConnectLocation location;
        urnet::ConnectLocationId id;
        id.client_id = copy.ClientId;
        location.connect_location_id = id;
        location.name = PeerDisplayName(copy);
        // TODO(sdk-wiring): SdkHost::ConnectFromRow (coalesced row click)
        host_.Connect(location);
        Render();
      });
      listHost_->append(*row);
    }
    total += peerCount;
  }

  // Group 2 — idle vs searching
  if (!searching) {
    // header passed count 0 => no meta; it labels the single synthetic row
    // (every client ignores the SDK's Promoted bucket — intentional quirk)
    AppendGroup(*listHost_, T_("promoted_locations", "Promoted Locations"), 0);
    auto* row = MakeRow(T_("best_available_provider", "Best available provider"), {},
                        kUrCoral /* hardcoded, mobile parity — no SDK call */,
                        IsBestAvailableSelected(selected), false, false, false);
    row->signal_clicked().connect([this] {
      // TODO(sdk-wiring): SdkHost::ConnectBestAvailableFromRow (coalesced)
      host_.ConnectBestAvailable();
      Render();
    });
    listHost_->append(*row);
    total += 1;
  }

  // Groups: Top Matches (searching), then the SDK's own buckets in the SDK's
  // own order (regions/cities populate only while searching; no client-side
  // re-bucketing — narrowing is server-side).
  const int beforeBuckets = total;
  if (locations_) {
    if (searching) {
      AppendLocationSection(T_("top_matches", "Top Matches"), locations_->BestMatches,
                            selected, total);
    }
    AppendLocationSection(T_("countries", "Countries"), locations_->Countries, selected, total);
    AppendLocationSection(T_("regions", "Regions"), locations_->Regions, selected, total);
    AppendLocationSection(T_("cities", "Cities"), locations_->Cities, selected, total);
    AppendLocationSection(T_("devices", "Devices"), locations_->Devices, selected, total);
  }
  const int bucketRows = total - beforeBuckets;

  // §5.3 — the inline "why the buckets are empty" line. The list is never
  // structurally empty when idle (the best-available row is always there), so
  // the full-height overlay alone could not fire — this line renders where
  // the buckets would be.
  if (bucketRows <= 0 && !samplePinned_) {
    Glib::ustring line;
    switch (CurrentFeedState()) {
      case FeedState::Loading:
        line = T_("loading", "Loading...");
        break;
      case FeedState::Failed:
        // Key adv_providers_load_failed is NOT in the store — the English
        // fallback renders until the store grows it. No retry promise in the
        // copy: nothing is scheduled; the list reloads on destination
        // re-entry, window return, or the next search.
        line = T_("adv_providers_load_failed", "Could not load the provider list.");
        break;
      case FeedState::Loaded:
        line = searching ? T_("no_locations_found", "No locations found")
                         : T_("no_providers_found", "We could not find any providers.");
        break;
    }
    listHost_->append(*kit::MakePaneEmptyLine(line));
  }

  // §5.4 — the full-height overlay: only when the list ends the render with
  // zero children (unreachable with the inline line present; kept for exact
  // markup-level parity).
  RemoveAllChildren(*listEmptyHost_);
  if (!listHost_->get_first_child()) {
    listEmptyHost_->append(*kit::MakePaneEmptyLine(
        searching ? T_("no_locations_found", "No locations found")
                  : T_("connecting_status_indicator", "Connecting to providers")));
  }

  kit::SetTextOrCollapse(*paneA_.meta,
                         total > 0 ? Glib::ustring(std::to_string(total)) : Glib::ustring());
  RenderDetail();
}

// ---- Pane B -----------------------------------------------------------------

void NetworkPage::RenderDetail() {
  RemoveAllChildren(*detailHost_);
  const auto selected = host_.SelectedLocation();
  const bool best = IsBestAvailableSelected(selected);

  detailHost_->append(*kit::MakePaneGroupHeader(T_("selected_location", "Selected location")).root);

  // key/value rows with an empty value are skipped entirely
  auto addKv = [this](const Glib::ustring& key, const Glib::ustring& value) {
    if (value.empty()) return;
    detailHost_->append(*kit::MakePaneKeyValueRow(key, value).root);
  };

  if (best) {
    addKv(T_("name_label", "Name"), T_("best_available_provider", "Best available provider"));
    kit::SetTextOrCollapse(*paneB_.meta,
                           T_("best_available_provider", "Best available provider"));
  } else {
    // ConnectLocation fields ONLY — the SDK carries no latency and no load
    // anywhere; there are no such rows. Do not invent them.
    const auto& loc = *selected;
    const Glib::ustring name = loc.name ? Glib::ustring(*loc.name) : Glib::ustring();
    addKv(T_("name_label", "Name"), name);
    kit::SetTextOrCollapse(*paneB_.meta, name);
    const int providerCount = static_cast<int>(loc.provider_count.value_or(0));
    if (providerCount > 0) {
      addKv(T_("available_providers", "Available providers"),
            Format(TN_("provider_count", "{} provider", "{} providers", providerCount),
                   providerCount));
    }
    addKv(T_("country", "Country"), loc.country.value_or(std::string()));
    // deliberate plural-key reuse: the store has no singular Region/City keys
    addKv(T_("regions", "Regions"), loc.region.value_or(std::string()));
    addKv(T_("cities", "Cities"), loc.city.value_or(std::string()));
    addKv(T_("strong_anonymization", "Strong Anonymization"),
          loc.strong_privacy ? T_("yes", "Yes") : T_("no", "No"));
    addKv(T_("promoted", "Promoted"),
          loc.promoted.value_or(false) ? T_("yes", "Yes") : T_("no", "No"));
    if (!loc.stable) {
      // the shipped amber sentence, not a "Stable: No" row (the store has no
      // "Stable" label). #F5C242 = kUrAmber — deliberately neither brand
      // yellow nor danger red.
      auto* host = kit::MakePaneRow(34);
      auto* line = Gtk::make_managed<Gtk::Label>();
      line->set_markup("<span size='" + std::to_string(12 * PANGO_SCALE) + "' foreground='" +
                       HexForMarkup(kUrAmber) + "'>" +
                       Glib::Markup::escape_text(
                           T_("unstable_providers_warning", "* (may be unstable)")) +
                       "</span>");
      line->set_xalign(0);
      line->set_valign(Gtk::Align::CENTER);
      if (auto* inner = RowInner(host)) inner->append(*line);
      detailHost_->append(*host);
    }
  }

  // Reset-to-automatic: hidden when it would change nothing.
  if (!best) {
    auto* row = MakeRow(T_("best_available_provider", "Best available provider"), {},
                        kUrCoral, false, false, false, false);
    row->signal_clicked().connect([this] {
      // TODO(sdk-wiring): SdkHost::ConnectBestAvailableFromRow (coalesced)
      host_.ConnectBestAvailable();
      Render();
    });
    detailHost_->append(*row);
  }

  // The honest bucket counts behind the list — for the CURRENT query, not a
  // second copy of the list. Integer values, 0 when the feed is absent.
  detailHost_->append(
      *kit::MakePaneGroupHeader(T_("available_providers", "Available providers")).root);
  auto bucketCount = [](const std::optional<urnet::ConnectLocationList>& list) {
    return list ? static_cast<int>(list->size()) : 0;
  };
  auto addCount = [this](const Glib::ustring& key, int n) {
    detailHost_->append(*kit::MakePaneKeyValueRow(key, std::to_string(n)).root);
  };
  addCount(T_("network_peers", "Network peers"),
           peers_ ? static_cast<int>(peers_->size()) : 0);
  addCount(T_("countries", "Countries"), locations_ ? bucketCount(locations_->Countries) : 0);
  addCount(T_("regions", "Regions"), locations_ ? bucketCount(locations_->Regions) : 0);
  addCount(T_("cities", "Cities"), locations_ ? bucketCount(locations_->Cities) : 0);
  addCount(T_("devices", "Devices"), locations_ ? bucketCount(locations_->Devices) : 0);

  // The SECOND door to the blocked list (it stays on Settings too; this page
  // does not own the network API read).
  detailHost_->append(
      *kit::MakePaneGroupHeader(T_("blocked_locations_2", "Blocked locations")).root);
  auto door = kit::MakePaneTwoLineRowButton(T_("blocked_locations_2", "Blocked locations"),
                                            T_("select_country_to_block",
                                               "Select country to block"));
  door.root->signal_clicked().connect([] {
    // TODO(sheet): BlockedLocationsSheet — route through the window-level
    // sheetOpen guard to the Settings-owned blocked-locations sheet
    // (SettingsPage::ShowBlockedLocationsSheet) once it exists in this tree;
    // no such sheet is present today.
  });
  detailHost_->append(*door.root);
}

// ---- preview sample ---------------------------------------------------------

void NetworkPage::ApplyPreviewSample() {
  g_warning("NetworkPage: preview sample pinned — provider content is SYNTHETIC");

  urnet::FilteredLocations sample;
  urnet::ConnectLocationList countries;
  auto country = [&countries](const std::string& name, const std::string& code, int providers,
                              bool stable, bool privacy, bool promoted) {
    countries.push_back(SampleLocation(name, urnet::LocationTypeCountry, code, providers,
                                       stable, privacy, promoted, name, {}, {}));
  };
  country("Germany", "de", 412, true, true, true);
  country("United States", "us", 1876, true, false, false);
  country("Japan", "jp", 233, true, false, false);
  country("Netherlands", "nl", 198, true, true, false);
  country("Brazil", "br", 76, false, false, false);  // unstable
  country("Singapore", "sg", 141, true, false, false);
  country("United Kingdom", "gb", 604, true, false, false);
  country("Canada", "ca", 287, true, false, false);
  country("France", "fr", 351, true, false, false);
  country("Sweden", "se", 119, true, true, false);
  country("Australia", "au", 92, false, false, false);  // unstable
  country("India", "in", 508, true, false, false);
  sample.Countries = countries;

  urnet::ConnectLocationList regions;
  auto region = [&regions](const std::string& name, const std::string& code,
                           const std::string& country) {
    regions.push_back(SampleLocation(name, urnet::LocationTypeRegion, code, 48, true, false,
                                     false, country, name, {}));
  };
  region("California", "us", "United States");
  region("Bavaria", "de", "Germany");
  region("Ontario", "ca", "Canada");
  region("Kanto", "jp", "Japan");
  region("New South Wales", "au", "Australia");
  sample.Regions = regions;

  urnet::ConnectLocationList cities;
  auto city = [&cities](const std::string& name, const std::string& code,
                        const std::string& region, const std::string& country) {
    cities.push_back(SampleLocation(name, urnet::LocationTypeCity, code, 27, true, false,
                                    false, country, region, name));
  };
  city("Berlin", "de", "Berlin", "Germany");
  city("New York", "us", "New York", "United States");
  city("Tokyo", "jp", "Kanto", "Japan");
  city("Amsterdam", "nl", "North Holland", "Netherlands");
  city("São Paulo", "br", "São Paulo", "Brazil");
  city("Singapore", "sg", "Singapore", "Singapore");
  city("London", "gb", "England", "United Kingdom");
  city("Toronto", "ca", "Ontario", "Canada");
  city("Paris", "fr", "Île-de-France", "France");
  city("Stockholm", "se", "Stockholm", "Sweden");
  sample.Cities = cities;

  urnet::NetworkPeerList samplePeers;
  samplePeers.push_back(SamplePeer("workshop-desktop", "windows"));
  samplePeers.push_back(SamplePeer("kitchen-pi", "linux/arm64"));
  samplePeers.push_back(SamplePeer("studio-mbp", "darwin/arm64"));

  locations_ = sample;
  locationsState_ = urnet::LocationsLoaded;
  peers_ = samplePeers;
  // pinned: later real (empty) pushes cannot blank the pane — OnLocations/
  // OnPeers early-return, ReconcileFeeds skips the snapshot seed, and the
  // §5.3 inline line is suppressed
  samplePinned_ = true;
  Render();
}

}  // namespace urnw
