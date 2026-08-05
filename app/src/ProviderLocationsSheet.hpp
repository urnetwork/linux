// The connected providers and where they are: a fixed globe over an
// independently scrolling list, one row per provider in the current connect
// window. Opened from the home screen's "Connected to N providers" label.
//
// Port of the android ProviderLocationsScreen, with the desktop affordances the
// android version could not have (an explicit dismiss in the header, an inline
// remove button rather than a swipe, and a mouse wheel driving the globe).
//
// The SDK returns the providers sorted OLDEST-CONNECTED FIRST, and that order
// is preserved verbatim: it is what makes "the oldest provider with
// coordinates" -- the location override's target -- simply the first plottable
// row.
//
// Above the globe sits the device-location override toggle (see
// LocationOverride.hpp), which is a real, supported feature on linux.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <adwaita.h>
#include <gtkmm.h>

#include "LocationOverride.hpp"
#include "LocationOverrideGuide.hpp"
#include "ProviderGlobe.hpp"
#include "ProviderLocationRow.hpp"
#include "SdkHost.hpp"

namespace urnw {

// Maps the SDK list onto the render rows, preserving the SDK's ordering
// (oldest-connected first) verbatim. Coordinates come from the city centroid
// when the server knows it, else the region centroid; a provider with neither
// is listed but not plotted. Shared by the sheet and by MainWindow, which keeps
// the location override following the oldest provider even while the sheet is
// closed and the window is hidden to the tray.
std::vector<ProviderLocationRow> MapConnectedProviderLocations(
    const urnet::ConnectedProviderLocationList& locations);

class ProviderLocationsSheet : public Gtk::Window {
 public:
  ProviderLocationsSheet(Gtk::Window& parent, SdkHost& host,
                         LocationOverrideController* locationOverride);
  ~ProviderLocationsSheet() override;

  void Open();
  // Re-read the SDK getter and reconcile. Safe to call on every change event:
  // the rows are compared by value first, so the frequent window-turnover
  // notifications and the per-second duration clock cannot thrash the widgets.
  void Refresh();

 private:
  struct RowWidgets {
    Gtk::Box* container = nullptr;
    Gtk::DrawingArea* dot = nullptr;
    Gtk::Label* clientId = nullptr;
    Gtk::Label* place = nullptr;
    Gtk::Label* coordinates = nullptr;
    Gtk::Label* duration = nullptr;
    Gtk::Button* remove = nullptr;
  };

  std::vector<ProviderLocationRow> ReadRows();
  void BuildHeader(Gtk::Box& root);
  void BuildOverrideSection(Gtk::Box& root);
  void RebuildList();
  Gtk::Widget* BuildRow(size_t index, RowWidgets& out);
  void UpdateSelection();
  void UpdateDurations();
  void Select(const std::string& clientId);
  void CopyClientId(const std::string& clientId);
  void RemoveProvider(const std::string& clientId);
  void RefreshOverrideSection();
  bool OnDurationTick();

  SdkHost& host_;
  LocationOverrideController* locationOverride_ = nullptr;

  AdwToastOverlay* toastOverlay_ = nullptr;
  ProviderGlobe* globe_ = nullptr;
  Gtk::Box listBox_{Gtk::Orientation::VERTICAL};
  Gtk::ScrolledWindow* scroller_ = nullptr;
  Gtk::Label* statusLabel_ = nullptr;  // empty state / "unavailable until connected"

  // override section widgets
  Gtk::Box* overrideBox_ = nullptr;
  Gtk::Switch* overrideSwitch_ = nullptr;
  Gtk::Label* overrideNote_ = nullptr;
  bool updatingOverrideSwitch_ = false;

  std::vector<ProviderLocationRow> rows_;
  std::vector<RowWidgets> rowWidgets_;
  std::string selectedClientId_;
  // Providers the user removed, filtered out of every read until the SDK stops
  // reporting them -- otherwise the row would flicker back for the round trip.
  std::set<std::string> pendingRemovals_;
  bool deviceAvailable_ = false;

  sigc::connection durationTick_;
  std::unique_ptr<LocationOverrideGuide> guide_;
};

}  // namespace urnw
