// The connect hero canvas — the signature visual, a faithful GTK4/cairo port
// of windows:ConnectCanvas.{h,cpp} (itself iOS Main/Connect/ConnectButton).
// Spec: docs/parity/connect-canvas.md — every metric is written in iOS's
// 256pt canvas space and scaled by side/256.
//
// The canvas is DECORATIVE and non-interactive: click/focus/name live on the
// hero button that wraps it (ConnectPage). Five states: Disconnected (the
// electric-blue core + bounded pulse burst), Connecting (GlobeConnector
// lattice + the live provider grid), Connected (five 180pt brand circles
// sliding in), Error/Processing (a faint glyph, 500ms delayed).
//
// Motion budget (normative): repeating/one-shot motion rides an internal
// frame-clock callback that runs ONLY while something animates; the external
// Tick() (the page's shared ~10fps clock) advances grid-dot transitions only.
// gtk-enable-animations off = motion GONE, everything snaps.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <random>
#include <map>
#include <string>
#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

namespace urnw {

class ConnectCanvas : public Gtk::Widget {
 public:
  enum class State { Disconnected, Connecting, Connected, Error, Processing };

  ConnectCanvas();

  void SetState(State state);
  State state() const { return state_; }
  // The live provider grid (ignored unless state == Connecting — iOS freezes
  // the grid the instant the connection lands). Empty list = bare lattice.
  void SetGrid(const std::vector<urnet::ProviderGridPoint>& points, int64_t gridWidth,
               int64_t gridHeight);
  void SetHovered(bool hovered);
  void SetFocusRingVisible(bool visible);
  void SetPresentationActive(bool active);
  // the page's shared ~10fps tick: advances grid-dot color/size transitions
  void Tick();

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  enum class PointState { InEvaluation, EvaluationFailed, NotAdded, Added, Removed };
  struct Dot {
    double x = 0, y = 0;          // grid cell coords
    PointState state = PointState::InEvaluation;
    PointState previous = PointState::InEvaluation;
    double colorProgress = 1.0;   // previous -> state blend
    double sizeProgress = 0.0;    // grow-in
  };

  // ---- animation clock ------------------------------------------------------
  void EnsureAnimClock();
  bool AnimStep(gint64 nowUs);  // returns whether anything is still animating
  static bool AnimationsEnabled();

  void StartIdlePulse();  // the bounded 3-burst pulse (Disconnected only)
  void RunBlobs(bool in);
  void ShuffleBlobs();
  void ClearPoints();

  void DrawCanvas(const Cairo::RefPtr<Cairo::Context>& cr, double width, double height);
  void AddGlobePath(const Cairo::RefPtr<Cairo::Context>& cr, double originX, double originY,
                    double side) const;

  State state_ = State::Disconnected;
  bool presenting_ = false;
  bool hovered_ = false;
  bool focusRing_ = false;

  // layer opacities (state crossfade, 500ms ease-in-out)
  double idleOpacity_ = 1.0, idleTarget_ = 1.0;
  double gridOpacity_ = 0.0, gridTarget_ = 0.0;
  double glyphOpacity_ = 0.0;
  bool glyphShown_ = false;
  gint64 glyphShownAtUs_ = 0;  // entrance: 500ms delay + 300ms fade
  gint64 fadeStartUs_ = -1;
  double idleFadeFrom_ = 1.0, gridFadeFrom_ = 0.0;

  // pulse burst: 3 cycles x 1500ms ease-out
  int pulseBurstsLeft_ = 0;
  gint64 pulseStartUs_ = -1;
  double pulseScale_ = 1.0, pulseOpacity_ = 0.0;

  // blob slide: 1000ms ease-in-out; progress 0=parked out, 1=settled in
  bool blobsIn_ = false;
  bool blobsVisible_ = false;
  double blobProgress_ = 0.0;
  gint64 blobStartUs_ = -1;
  double blobFrom_ = 0.0, blobTo_ = 0.0;
  std::vector<int> blobColorOrder_{0, 1, 2, 3, 4};
  std::vector<int> blobOffsetOrder_{0, 1, 2, 3, 4};
  std::mt19937 blobRng_{0x5EED0BE};  // constant seed: deterministic arrangements (parity)

  // hover lift: 180ms ease-out to 1.03
  double hoverScale_ = 1.0;
  gint64 hoverStartUs_ = -1;
  double hoverFrom_ = 1.0, hoverTo_ = 1.0;

  bool animClockActive_ = false;

  // the provider grid
  std::map<std::string, Dot> dots_;
  int64_t gridWidth_ = 0, gridHeight_ = 0;
  bool dotsAnimating_ = false;
};

}  // namespace urnw
