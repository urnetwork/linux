// The provider globe: a dark sphere with white land, a graticule, and one dot
// per plottable provider colored by its country. The selected provider gets a
// ring, and selecting spins the globe to center that provider.
//
// A Cairo port of the android ProviderGlobe.kt, which is itself a port of the
// d3 orthographic globe on ur.io /ip. All the projection math lives in
// GlobeGeometry (pure, unit tested); this file is only drawing and gestures.
//
// The land outlines are decoded from assets/world-110m.json on a worker thread
// the first time a globe is built and cached process-wide: the sphere,
// graticule and dots render immediately, land appears when it is ready, and a
// missing or corrupt asset simply leaves the land out.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "GlobeGeometry.hpp"
#include "ProviderLocationRow.hpp"
#include "Ui.hpp"

namespace urnw {

// The rotation the globe is animating toward, identified by the provider it
// belongs to. Comparing the coordinates as well as the id is what makes a
// provider whose position changes under the selection recenter the globe.
struct GlobeCenterTarget {
  std::string clientId;
  double lat = 0;
  double lon = 0;

  // spelled out rather than `= default`: this app builds as C++17, where
  // defaulted comparison operators do not exist yet
  bool operator==(const GlobeCenterTarget& other) const {
    return clientId == other.clientId && lat == other.lat && lon == other.lon;
  }
  bool operator!=(const GlobeCenterTarget& other) const { return !(*this == other); }
};

class ProviderGlobe : public Gtk::DrawingArea {
 public:
  ProviderGlobe();
  ~ProviderGlobe() override;

  // The country color for a provider dot; the sheet wires this to the SDK
  // palette. Called with the lowercased ISO-2 code, never with an empty string
  // (providers with no country use the web globe's neutral blue instead).
  void SetColorResolver(std::function<Rgba(const std::string& countryCode)> resolver);

  // Replaces the rendered set. Rows without coordinates are ignored here (they
  // still appear in the list). The globe centers itself once on the first
  // provider that appears and thereafter follows the selection -- recentering
  // on every window turnover would fight the user.
  void SetRows(std::vector<ProviderLocationRow> rows);

  // Selection, shared with the list. An empty id clears it.
  void SetSelected(const std::string& clientId);

  // Fired when a dot is clicked.
  std::function<void(const std::string& clientId)> on_select;
  // Fired when a drag or scroll crosses the wheel's hysteresis threshold,
  // positive east. Where that lands is the SDK view controller's business (the
  // centroid-relative order, clamped at the ends), not the widget's.
  std::function<void(int steps)> on_step;

 private:
  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
  void OnDragBegin(double startX, double startY);
  void OnDragUpdate(double offsetX, double offsetY);
  void OnDragEnd(double offsetX, double offsetY);
  bool OnScroll(double dx, double dy);
  void OnPressed(int pressCount, double x, double y);

  // Retargets the recenter spring at the selected provider (or, until the globe
  // has been placed once, at the first plottable one) when that provider or its
  // position has actually changed. A no-op otherwise, so an unchanged window
  // never restarts the spin.
  void RecenterIfTargetChanged();
  bool OnTick();
  // Whether any row can be plotted: with none there is nothing to traverse, so
  // the drag rotates the globe freely instead of driving the wheel.
  bool HasPlottable() const;

  std::function<Rgba(const std::string&)> colorResolver_;
  std::vector<ProviderLocationRow> rows_;
  std::string selectedClientId_;
  // The provider the globe is centered on. Empty until the globe has been
  // placed once.
  GlobeCenterTarget centerTarget_;
  bool centeredOnce_ = false;

  GlobeRotation rotation_;  // the live rotation
  // recenter spring: the target rotation, the current angular velocity per axis
  // (degrees per second, carried ACROSS retargets so overlapping recenters stay
  // continuous), and the frame it was last advanced at
  GlobeRotation animTo_;
  GlobeRotation animVelocity_;
  gint64 animLastUs_ = 0;
  bool animating_ = false;
  bool ticking_ = false;

  // wheel gesture state, reset per drag gesture
  double lastDragOffsetX_ = 0;
  double lastDragOffsetY_ = 0;
  double wheelTravel_ = 0;
  // horizontal scroll accumulates against the same hysteresis threshold
  double scrollTravel_ = 0;

  // cleared on destruction, so the async topology callback can tell that the
  // widget is gone before it touches it
  std::shared_ptr<bool> alive_;
};

}  // namespace urnw
