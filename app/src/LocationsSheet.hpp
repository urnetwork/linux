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
// carry a green "providing" glyph. Every row is a button, named as the
// Network page names its rows (LocationRowName.hpp), so the chooser works
// from the keyboard and with a screen reader. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include <gtkmm.h>

#include "PeerLocation.hpp"  // PeerDisplayName, PeerConnectLocation
#include "SdkHost.hpp"

namespace urnw {

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
  // `caption` replaces the provider count line when it is not empty.
  Gtk::Button* MakeLocationRow(const urnet::ConnectLocation& location, bool selected,
                               const std::string& caption = {});
  Gtk::Button* MakePeerRow(const urnet::NetworkPeer& peer, bool selected);
  Gtk::Button* MakeBestAvailableRow(bool selected);
  void OnSearchChanged();

  SdkHost& host_;
  Gtk::SearchEntry searchEntry_;
  Gtk::Label statusLabel_;  // loading / no-results
  Gtk::Box sectionsBox_{Gtk::Orientation::VERTICAL, 12};
  std::string currentQuery_;
};

}  // namespace urnw
