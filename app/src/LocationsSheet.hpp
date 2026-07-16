// The location/provider chooser (port of the apple ProviderListSheet and the
// android BrowseLocations screen): a search box over the SDK-bucketed location
// sections. The SDK's LocationsViewController does all grouping and search; this
// sheet only renders the six lists it returns, with the connected network peers
// (PeerViewController — connected AND provide-enabled only) pinned as the first
// section. Tapping any row connects to it and hides the sheet.
//
// Section order mirrors mobile: network peers, then "best matches" (while
// searching) or a single "best available" row (idle), then countries, regions,
// cities, devices. Selection is reflected with a trailing check; peers also
// carry a green "providing" glyph. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

// A network peer's display name: DeviceName, else DeviceSpec, else the client id.
// Shared with the connect drawer's selected-location label.
std::string PeerDisplayName(const urnet::NetworkPeer& peer);

class LocationsSheet : public Gtk::Window {
 public:
  LocationsSheet(Gtk::Window& parent, SdkHost& host);

  void Open();
  // Re-read peers + filtered locations + the selected location and rebuild the
  // sections. Cheap enough to run on every locations/peers change event.
  void Refresh();

 private:
  void RebuildSections();
  void AppendLocationSection(const std::string& title,
                             const std::optional<urnet::ConnectLocationList>& items,
                             const std::optional<urnet::ConnectLocation>& selected);
  Gtk::Box* MakeLocationRow(const urnet::ConnectLocation& location, bool selected);
  Gtk::Box* MakePeerRow(const urnet::NetworkPeer& peer, bool selected);
  Gtk::Box* MakeBestAvailableRow(bool selected);
  void OnSearchChanged();

  SdkHost& host_;
  Gtk::SearchEntry searchEntry_;
  Gtk::Label statusLabel_;  // loading / no-results
  Gtk::Box sectionsBox_{Gtk::Orientation::VERTICAL, 12};
  std::string currentQuery_;
};

}  // namespace urnw
