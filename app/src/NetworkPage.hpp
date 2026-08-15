// The Network destination — the provider-locations browser (port of the
// windows NetworkPage in LocationSheets.cpp, spec docs/parity/network-dest.md).
//
// Pane A (the star column — inverse of Home) is the searchable provider list:
// a 40px search row over the SDK's own buckets (network peers pinned first,
// then Promoted/Best-available when idle or Top Matches while searching, then
// Countries / Regions / Cities / Devices), every row the one 36px species.
// Pane B (400dip, visible at >= 1000dip only) is the selected-provider detail:
// ConnectLocation fields ONLY (the SDK carries no latency and no load — no
// such rows), the honest bucket counts behind the current query, and the
// blocked-locations door.
//
// Clicking a row IS select-and-connect — deliberately no separate "highlight"
// concept; the detail pane shows the SELECTED provider, not a hover state.
//
// The three empty states Loading / Failed / genuinely-empty are distinguished
// by the feed state string, checked BEFORE the snapshot (a LOCATIONS_LOADING
// push carries null buckets, byte-identical to "loaded, zero providers").
// There is deliberately NO "no session" state: the provider list is public
// (GET /network/provider-locations answers 200 unauthenticated), served from
// the in-process Api when there is no device.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <gtkmm.h>

#include "PaneKit.hpp"
#include "SdkHost.hpp"
#include "Ui.hpp"

namespace urnw {

class NetworkPage : public Gtk::Box {
 public:
  explicit NetworkPage(SdkHost& host);

  // nav-select + auth-change loads: bumps the stale-async epoch, invalidates
  // the api fallback cache (the server/space may have changed), reconciles.
  void Load();
  // navigation edge: this destination became (un)selected
  void SetSelected(bool selected);
  // the window presentation gate (shown && !minimized — NOT activated: plain
  // alt-tab must not close the feeds). Feeds re-arm on BOTH edges.
  void SetPresentationActive(bool active);
  // the spec's pane-fold table: Pane B + its rule hidden under 1000dip.
  // Below the breakpoint nothing is unreachable — a row click still connects.
  void ApplyBreakpoint(int widthDip);

  // The destination's feed slot. The windows client binds a SECOND observer
  // pair on SdkHost (SetLocationsObserver/SetPeersObserver); this host has
  // only the single DrawerEvent handler, so the window's dispatcher forwards
  // DrawerEvent::Locations / DrawerEvent::Peers here (already marshalled onto
  // the GTK loop). The events are signal-only — both re-read the accessors.
  // TODO(sdk-wiring): SdkHost::SetLocationsObserver / SetPeersObserver — the
  // dedicated second feed slot (payload copied to the observer, state string
  // included) once the host grows it.
  void OnLocationsEvent();
  void OnPeersEvent();

  // --preview-ui + URNETWORK_PREVIEW_SAMPLE=1 (BOTH required; the window
  // checks the gate): pin synthetic buckets so every surface of this page
  // renders without a session. Later real (empty) pushes cannot blank it.
  void ApplyPreviewSample();

 private:
  // §5.2 feed-state machine. No NoSession member: a state that cannot occur
  // must not be representable.
  enum class FeedState { Loading, Loaded, Failed };

  void ReconcileFeeds();
  void EnsureApiLocations();
  FeedState CurrentFeedState() const;
  void OnSearchChanged();
  void Render();
  void RenderDetail();
  // The ONE row species of Pane A: the kit's 36px list-row button with the
  // state-glyph stack (providing / unstable / strong-privacy / selected)
  // between title and figure, and the composed accessible name.
  Gtk::Button* MakeRow(const Glib::ustring& title, const Glib::ustring& meta,
                       const Rgba& dotColor, bool selected, bool unstable,
                       bool strongPrivacy, bool providing);
  void AppendGroup(Gtk::Box& into, const Glib::ustring& title, int count);
  void AppendLocationSection(const Glib::ustring& title,
                             const std::optional<urnet::ConnectLocationList>& items,
                             const std::optional<urnet::ConnectLocation>& selected,
                             int& total);

  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);  // stale-async guard

  // pane shell
  kit::Pane paneA_;
  kit::Pane paneB_;
  Gtk::Widget* paneBRule_ = nullptr;
  kit::PaneSearchRow search_;
  Gtk::Box* listHost_ = nullptr;       // Pane A rows/groups are appended here
  Gtk::Box* listEmptyHost_ = nullptr;  // full-height centred empty-state host (§5.4)
  Gtk::Box* detailHost_ = nullptr;     // Pane B content column
  int lastWide_ = -1;                  // breakpoint no-op guard (-1 = never applied)

  // feed snapshot. The state string is stored WITH the snapshot — it is the
  // only separator of the three empty screens (dropping it was the windows
  // client's historical bug).
  std::string query_;
  std::optional<urnet::FilteredLocations> locations_;
  std::string locationsState_;
  std::optional<urnet::NetworkPeerList> peers_;

  bool navSelected_ = false;
  bool presentationActive_ = true;
  bool samplePinned_ = false;

  // ---- no-device api fallback (§7.3) ----------------------------------------
  // The provider list does not need a session. With no device this page runs
  // the LocationsViewController::FilterLocations dispatch against the
  // in-process Api and buckets with urnet::getFilteredLocationsFromResult.
  // TODO(sdk-wiring): SdkHost::EnsureLocations — this cache belongs in the
  // host so the page and the modal chooser share one copy; move it there when
  // the host grows the api path.
  std::string apiTargetQuery_;                 // trimmed query the fallback should answer
  std::optional<std::string> apiLoadedQuery_;  // query the Loaded cache answers
  std::optional<std::string> apiInFlightQuery_;
  uint64_t apiGeneration_ = 0;  // superseded answers are dropped
};

}  // namespace urnw
