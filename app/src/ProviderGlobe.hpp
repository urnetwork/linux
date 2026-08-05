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
  // provider that appears and thereafter only on an explicit selection --
  // recentering on every window turnover would fight the user.
  void SetRows(std::vector<ProviderLocationRow> rows);

  // Selection, shared with the list. An empty id clears it.
  void SetSelected(const std::string& clientId);

  // Fired when a dot is clicked or the wheel steps.
  std::function<void(const std::string& clientId)> on_select;

 private:
  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
  void OnDragBegin(double startX, double startY);
  void OnDragUpdate(double offsetX, double offsetY);
  void OnDragEnd(double offsetX, double offsetY);
  bool OnScroll(double dx, double dy);
  void OnPressed(int pressCount, double x, double y);

  // Advances the wheel selection by `steps` and emits on_select.
  void StepWheel(int steps);
  // Starts the ~450ms spin to the selection (or to the first provider, once).
  void AnimateToSelection();
  bool OnTick();
  // The wheel order (plottable rows by longitude, west to east).
  const std::vector<int>& wheel() const { return wheel_; }

  std::function<Rgba(const std::string&)> colorResolver_;
  std::vector<ProviderLocationRow> rows_;
  std::vector<int> wheel_;  // indexes into rows_, ordered by longitude
  std::string selectedClientId_;
  bool centeredOnce_ = false;

  GlobeRotation rotation_;      // the live rotation
  GlobeRotation animFrom_;
  GlobeRotation animTo_;
  gint64 animStart_ = 0;
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
