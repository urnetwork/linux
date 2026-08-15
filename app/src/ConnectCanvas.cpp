// SPDX-License-Identifier: MPL-2.0
#include "ConnectCanvas.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>

namespace urnw {
namespace {

// ---- 256-space constants (docs/parity/connect-canvas.md) --------------------
constexpr double kIosCanvas = 256.0;
constexpr double kMinSide = 168.0;
constexpr double kMaxSide = 288.0;
constexpr double kSidePad = 4.0;
constexpr double kBandPad = 12.0;

constexpr double kPulseD = 56.0, kCoreRingD = 52.0, kCoreGapD = 50.0, kCoreD = 48.0;
constexpr double kPulseScaleTo = 1.5, kPulseOpacityFrom = 0.5;
constexpr int kPulseMs = 1500, kIdlePulseBursts = 3;
constexpr int kStateFadeMs = 500;
constexpr int kBlobMs = 1000;
constexpr double kBlobD = 180.0;
constexpr int kGlyphDelayMs = 500, kGlyphFadeMs = 300;
constexpr double kGlyphSize = 32.0;
constexpr int kHoverMs = 180;
constexpr double kHoverScale = 1.03;
constexpr double kPointStep = 0.15;
constexpr size_t kMaxPoints = 1024;

// ground fixed: the hero sits on an opaque .ur-pane (#101010) by construction;
// the true even-odd clip removes the windows overlay's ground dependency
struct Rgb {
  double r, g, b;
};
constexpr Rgb kGround{0x10 / 255.0, 0x10 / 255.0, 0x10 / 255.0};
constexpr Rgb kElectric{0x00 / 255.0, 0x39 / 255.0, 0xDE / 255.0};
constexpr Rgb kFaint{0x5A / 255.0, 0x5A / 255.0, 0x5A / 255.0};
constexpr Rgb kOffWhite{0xF8 / 255.0, 0xF8 / 255.0, 0xF8 / 255.0};

// Lift(ground, amt): per-channel add — #1C1C1C resting, #242424 hovered
Rgb Lift(Rgb c, int amt) {
  auto up = [amt](double v) { return std::min(1.0, v + amt / 255.0); };
  return {up(c.r), up(c.g), up(c.b)};
}

struct Argb {
  double a, r, g, b;
};
constexpr Argb kDotColors[] = {
    {1.0, 0xEF / 255.0, 0xF7 / 255.0, 0xBB / 255.0},  // InEvaluation  kAccent
    {1.0, 0xFF / 255.0, 0x6C / 255.0, 0x58 / 255.0},  // EvaluationFailed  coral
    {1.0, 0xFF / 255.0, 0x6C / 255.0, 0x58 / 255.0},  // NotAdded  coral
    {1.0, 0x87 / 255.0, 0xFB / 255.0, 0x67 / 255.0},  // Added  green
    {0.0, 0x10 / 255.0, 0x10 / 255.0, 0x10 / 255.0},  // Removed  urBlack @ 0
};

// the five blob colors (index-stable; the shuffle permutes ORDER)
constexpr Rgb kBlobColors[] = {
    {0xFF / 255.0, 0x6C / 255.0, 0x58 / 255.0},  // coral
    {0x87 / 255.0, 0xFB / 255.0, 0x67 / 255.0},  // green
    {0xDA / 255.0, 0xE2 / 255.0, 0xF9 / 255.0},  // light blue (the windows blend)
    {0xEF / 255.0, 0xF7 / 255.0, 0xBB / 255.0},  // accent
    {0xED / 255.0, 0x8F / 255.0, 0xFF / 255.0},  // pink
};
// offsets in canvas-widths (x side)
constexpr double kBlobEntry[][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {1, 0}};
constexpr double kBlobFinal[][2] = {{-1 / 3.5, -1.0 / 4}, {1.0 / 4, -1.0 / 3},
                                    {-1.0 / 3, 1.0 / 4},  {1.0 / 5, 1 / 2.5},
                                    {1.0 / 4, 0}};

double EaseOutCubic(double t) { return 1 - std::pow(1 - t, 3); }
double EaseInOutCubic(double t) {
  return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

}  // namespace

ConnectCanvas::ConnectCanvas() {
  set_overflow(Gtk::Overflow::HIDDEN);
  // decorative: everything interactive lives on the hero button around it
  gtk_accessible_update_state(GTK_ACCESSIBLE(gobj()), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
  ShuffleBlobs();
}

bool ConnectCanvas::AnimationsEnabled() {
  if (auto settings = Gtk::Settings::get_default()) {
    return settings->property_gtk_enable_animations().get_value();
  }
  return true;
}

// width is the driven dimension; the canvas writes its own height
void ConnectCanvas::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum,
                                  int& natural, int& minimum_baseline,
                                  int& natural_baseline) const {
  minimum_baseline = natural_baseline = -1;
  if (orientation == Gtk::Orientation::HORIZONTAL) {
    minimum = static_cast<int>(kMinSide + 2 * kSidePad);
    natural = static_cast<int>(kMaxSide + 2 * kSidePad);
  } else {
    const double w = for_size > 0 ? for_size : kMaxSide + 2 * kSidePad;
    const double side = std::clamp(w - 2 * kSidePad, kMinSide, kMaxSide);
    minimum = natural = static_cast<int>(side + 2 * kBandPad);
  }
}

// ---- state machine ----------------------------------------------------------

void ConnectCanvas::SetState(State state) {
  if (state_ == state) return;
  const bool leavingLive = (state_ == State::Connecting || state_ == State::Connected);
  const bool enteringLive = (state == State::Connecting || state == State::Connected);
  if (leavingLive && !enteringLive) ClearPoints();
  state_ = state;

  // fade targets (500ms ease-in-out crossfade)
  idleFadeFrom_ = idleOpacity_;
  gridFadeFrom_ = gridOpacity_;
  idleTarget_ = (state == State::Disconnected) ? 1.0 : 0.0;
  gridTarget_ = (state == State::Connecting || state == State::Connected) ? 1.0 : 0.0;
  fadeStartUs_ = -1;  // stamped on the first anim frame

  // blobs
  RunBlobs(state == State::Connected);

  // glyph (500ms delay + 300ms fade); leaving a blocked state hides at once
  const bool glyphNow = (state == State::Error || state == State::Processing);
  if (glyphNow && !glyphShown_) {
    glyphShown_ = true;
    glyphShownAtUs_ = 0;  // stamped on the first anim frame
    glyphOpacity_ = 0.0;
  } else if (!glyphNow) {
    glyphShown_ = false;
    glyphOpacity_ = 0.0;
  }

  // pulse
  pulseBurstsLeft_ = 0;
  pulseOpacity_ = 0.0;
  pulseScale_ = 1.0;
  if (state == State::Disconnected) StartIdlePulse();

  if (!AnimationsEnabled() || !presenting_) {
    // snap: motion GONE, not reduced
    idleOpacity_ = idleTarget_;
    gridOpacity_ = gridTarget_;
    glyphOpacity_ = glyphShown_ ? 1.0 : 0.0;
    blobProgress_ = blobsIn_ ? 1.0 : 0.0;
    blobsVisible_ = blobsIn_;
    queue_draw();
    return;
  }
  EnsureAnimClock();
}

void ConnectCanvas::SetGrid(const std::vector<urnet::ProviderGridPoint>& points,
                            int64_t gridWidth, int64_t gridHeight) {
  // iOS freezes the grid the instant the connection lands
  if (state_ != State::Connecting) return;
  gridWidth_ = gridWidth;
  gridHeight_ = gridHeight;

  // the diff: key by ClientId else "x,y"
  std::map<std::string, const urnet::ProviderGridPoint*> incoming;
  for (const auto& p : points) {
    if (incoming.size() >= kMaxPoints) break;  // malformed-push guard
    std::string key = (p.ClientId && !p.ClientId->empty())
                          ? *p.ClientId
                          : std::to_string(p.X) + "," + std::to_string(p.Y);
    incoming[std::move(key)] = &p;
  }
  auto parseState = [](const std::string& s) {
    if (s == "InEvaluation") return PointState::InEvaluation;
    if (s == "EvaluationFailed") return PointState::EvaluationFailed;
    if (s == "NotAdded") return PointState::NotAdded;
    if (s == "Added") return PointState::Added;
    if (s == "Removed") return PointState::Removed;
    return PointState::InEvaluation;  // unknown must not render as accepted
  };
  for (const auto& [key, p] : incoming) {
    auto it = dots_.find(key);
    if (it == dots_.end()) {
      Dot dot;
      dot.x = p->X;
      dot.y = p->Y;
      dot.state = dot.previous = parseState(p->State);
      dot.colorProgress = 1.0;
      dot.sizeProgress = 0.0;  // grow-in
      dots_.emplace(key, dot);
    } else {
      it->second.x = p->X;
      it->second.y = p->Y;
      const PointState next = parseState(p->State);
      if (next != it->second.state) {
        it->second.previous = it->second.state;
        it->second.state = next;
        it->second.colorProgress = 0.0;
      }
    }
  }
  // missing keys fade out through Removed rather than vanishing between frames
  for (auto& [key, dot] : dots_) {
    if (incoming.find(key) == incoming.end() && dot.state != PointState::Removed) {
      dot.previous = dot.state;
      dot.state = PointState::Removed;
      dot.colorProgress = 0.0;
    }
  }
  dotsAnimating_ = false;
  for (const auto& [key, dot] : dots_) {
    if (dot.colorProgress < 1.0 || dot.sizeProgress < 1.0) {
      dotsAnimating_ = true;
      break;
    }
  }
  queue_draw();
}

void ConnectCanvas::Tick() {
  if (!dotsAnimating_ || !presenting_) return;
  bool still = false;
  for (auto it = dots_.begin(); it != dots_.end();) {
    Dot& dot = it->second;
    dot.colorProgress = std::min(1.0, dot.colorProgress + kPointStep);
    dot.sizeProgress = std::min(1.0, dot.sizeProgress + kPointStep);
    if (dot.state == PointState::Removed && dot.colorProgress >= 1.0 &&
        dot.sizeProgress >= 1.0) {
      it = dots_.erase(it);
      continue;
    }
    if (dot.colorProgress < 1.0 || dot.sizeProgress < 1.0) still = true;
    ++it;
  }
  dotsAnimating_ = still;
  queue_draw();
}

void ConnectCanvas::ClearPoints() {
  dots_.clear();
  dotsAnimating_ = false;
}

void ConnectCanvas::SetHovered(bool hovered) {
  if (hovered_ == hovered) return;
  hovered_ = hovered;
  hoverFrom_ = hoverScale_;
  hoverTo_ = hovered ? kHoverScale : 1.0;
  hoverStartUs_ = -1;
  // pointer ARRIVAL re-arms the bounded pulse burst (Disconnected only)
  if (hovered && state_ == State::Disconnected) {
    StartIdlePulse();
  } else if (!hovered) {
    pulseBurstsLeft_ = 0;
    pulseOpacity_ = 0.0;
  }
  if (!AnimationsEnabled() || !presenting_) {
    hoverScale_ = hoverTo_;
    queue_draw();
    return;
  }
  EnsureAnimClock();
}

void ConnectCanvas::SetFocusRingVisible(bool visible) {
  if (focusRing_ == visible) return;
  focusRing_ = visible;
  queue_draw();
}

void ConnectCanvas::SetPresentationActive(bool active) {
  if (presenting_ == active) return;
  presenting_ = active;
  if (!active) {
    pulseBurstsLeft_ = 0;
    pulseOpacity_ = 0.0;
    return;
  }
  // re-shown: settle (never replay the blob entrance), then re-arm the pulse
  idleOpacity_ = idleTarget_;
  gridOpacity_ = gridTarget_;
  blobProgress_ = blobsIn_ ? 1.0 : 0.0;
  blobsVisible_ = blobsIn_;
  glyphOpacity_ = glyphShown_ ? 1.0 : 0.0;
  if (state_ == State::Disconnected) StartIdlePulse();
  EnsureAnimClock();
  queue_draw();
}

void ConnectCanvas::StartIdlePulse() {
  if (!presenting_ || !AnimationsEnabled() || state_ != State::Disconnected) {
    pulseOpacity_ = 0.0;
    return;
  }
  pulseBurstsLeft_ = kIdlePulseBursts;
  pulseStartUs_ = -1;
  EnsureAnimClock();
}

void ConnectCanvas::ShuffleBlobs() {
  std::shuffle(blobColorOrder_.begin(), blobColorOrder_.end(), blobRng_);
  std::shuffle(blobOffsetOrder_.begin(), blobOffsetOrder_.end(), blobRng_);
}

void ConnectCanvas::RunBlobs(bool in) {
  if (blobsIn_ == in) return;
  blobsIn_ = in;
  if (!AnimationsEnabled() || !presenting_) {
    blobProgress_ = in ? 1.0 : 0.0;
    blobsVisible_ = in;
    queue_draw();
    return;
  }
  if (in) ShuffleBlobs();  // reshuffle on every out->in
  blobsVisible_ = true;
  blobFrom_ = blobProgress_;
  blobTo_ = in ? 1.0 : 0.0;
  blobStartUs_ = -1;
  EnsureAnimClock();
}

// ---- the animation clock ----------------------------------------------------

void ConnectCanvas::EnsureAnimClock() {
  if (animClockActive_) return;
  animClockActive_ = true;
  add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
    const bool active = AnimStep(clock->get_frame_time());
    queue_draw();
    if (!active) animClockActive_ = false;
    return active;
  });
}

bool ConnectCanvas::AnimStep(gint64 nowUs) {
  bool active = false;
  auto progress = [nowUs](gint64& startUs, int durationMs) {
    if (startUs < 0) startUs = nowUs;
    return std::min(1.0, static_cast<double>(nowUs - startUs) / (durationMs * 1000.0));
  };

  // state crossfade
  if (idleOpacity_ != idleTarget_ || gridOpacity_ != gridTarget_) {
    const double t = EaseInOutCubic(progress(fadeStartUs_, kStateFadeMs));
    idleOpacity_ = idleFadeFrom_ + (idleTarget_ - idleFadeFrom_) * t;
    gridOpacity_ = gridFadeFrom_ + (gridTarget_ - gridFadeFrom_) * t;
    if (t < 1.0) active = true;
  }

  // pulse: repeated bounded bursts, opacity = 1.5 - scale throughout
  if (pulseBurstsLeft_ > 0) {
    const double t = EaseOutCubic(progress(pulseStartUs_, kPulseMs));
    pulseScale_ = 1.0 + (kPulseScaleTo - 1.0) * t;
    pulseOpacity_ = kPulseOpacityFrom * (1.0 - t);
    if (t >= 1.0) {
      --pulseBurstsLeft_;
      pulseStartUs_ = -1;
      if (pulseBurstsLeft_ == 0) {
        pulseScale_ = 1.0;
        pulseOpacity_ = 0.0;
      }
    }
    if (pulseBurstsLeft_ > 0) active = true;
  }

  // blob slide
  if (blobProgress_ != blobTo_) {
    const double t = EaseInOutCubic(progress(blobStartUs_, kBlobMs));
    blobProgress_ = blobFrom_ + (blobTo_ - blobFrom_) * t;
    if (t < 1.0) {
      active = true;
    } else if (!blobsIn_) {
      blobsVisible_ = false;  // hide when parked (guarded: state may have flipped)
    }
  }

  // hover lift
  if (hoverScale_ != hoverTo_) {
    const double t = EaseOutCubic(progress(hoverStartUs_, kHoverMs));
    hoverScale_ = hoverFrom_ + (hoverTo_ - hoverFrom_) * t;
    if (t < 1.0) active = true;
  }

  // glyph entrance: 500ms delay then 300ms fade
  if (glyphShown_ && glyphOpacity_ < 1.0) {
    if (glyphShownAtUs_ == 0) glyphShownAtUs_ = nowUs;
    const gint64 sinceMs = (nowUs - glyphShownAtUs_) / 1000;
    if (sinceMs >= kGlyphDelayMs) {
      glyphOpacity_ = EaseInOutCubic(
          std::min(1.0, static_cast<double>(sinceMs - kGlyphDelayMs) / kGlyphFadeMs));
    }
    if (glyphOpacity_ < 1.0) active = true;
  }

  return active;
}

// ---- drawing ----------------------------------------------------------------

// the ur-globe silhouette in its 32x32 box (identical to the login carousel)
void ConnectCanvas::AddGlobePath(const Cairo::RefPtr<Cairo::Context>& cr, double originX,
                                 double originY, double side) const {
  const double u = side / 32.0;
  auto P = [&](double x, double y) { return std::pair(originX + x * u, originY + y * u); };
  auto M = [&](double x, double y) { auto [px, py] = P(x, y); cr->move_to(px, py); };
  auto C = [&](double x1, double y1, double x2, double y2, double x, double y) {
    auto [ax, ay] = P(x1, y1);
    auto [bx, by] = P(x2, y2);
    auto [cx2, cy2] = P(x, y);
    cr->curve_to(ax, ay, bx, by, cx2, cy2);
  };
  auto L = [&](double x, double y) { auto [px, py] = P(x, y); cr->line_to(px, py); };
  M(30, 8);
  C(28.8955, 8, 28, 7.10453, 28, 6);
  C(28, 4.89547, 27.1045, 4, 26, 4);
  C(24.8955, 4, 24, 3.10453, 24, 2);
  C(24, 0.895469, 23.1045, 0, 22, 0);
  L(10, 0);
  C(8.89547, 0, 8, 0.895469, 8, 2);
  C(8, 3.10453, 7.10453, 4, 6, 4);
  C(4.89547, 4, 4, 4.89547, 4, 6);
  C(4, 7.10453, 3.10453, 8, 2, 8);
  C(0.895469, 8, 0, 8.89547, 0, 10);
  L(0, 22);
  C(0, 23.1045, 0.895469, 24, 2, 24);
  C(3.10453, 24, 4, 24.8955, 4, 26);
  C(4, 27.1045, 4.89547, 28, 6, 28);
  C(7.10453, 28, 8, 28.8955, 8, 30);
  C(8, 31.1045, 8.89547, 32, 10, 32);
  L(22, 32);
  C(23.1045, 32, 24, 31.1045, 24, 30);
  C(24, 28.8955, 24.8955, 28, 26, 28);
  C(27.1045, 28, 28, 27.1045, 28, 26);
  C(28, 24.8955, 28.8955, 24, 30, 24);
  C(31.1045, 24, 32, 23.1045, 32, 22);
  L(32, 10);
  C(32, 8.89547, 31.1045, 8, 30, 8);
  cr->close_path();
}

void ConnectCanvas::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
  const double w = get_width();
  const double h = get_height();
  if (w <= 0 || h <= 0) return;
  const graphene_rect_t bounds{{0, 0}, {static_cast<float>(w), static_cast<float>(h)}};
  // gtk_snapshot_append_cairo hands back a borrowed cairo_t; wrap WITHOUT
  // taking ownership (the snapshot destroys it when the node is finished).
  cairo_t* raw = gtk_snapshot_append_cairo(snapshot->gobj(), &bounds);
  auto cr = Cairo::RefPtr<Cairo::Context>(new Cairo::Context(raw, /*has_reference=*/false));
  DrawCanvas(cr, w, h);
  cairo_destroy(raw);
}

void ConnectCanvas::DrawCanvas(const Cairo::RefPtr<Cairo::Context>& cr, double width,
                               double height) {
  const double side = std::clamp(width - 2 * kSidePad, kMinSide, kMaxSide);
  const double s = side / kIosCanvas;
  const double ox = (width - side) / 2.0;
  const double oy = (height - side) / 2.0;
  const double cx = ox + side / 2.0, cy = oy + side / 2.0;

  const Rgb base = Lift(kGround, hovered_ ? 0x14 : 0x0C);
  const Rgb restingBase = Lift(kGround, 0x0C);  // coreGap keeps the resting base

  // keyboard focus ring: OUTSIDE the clip, the silhouette at side+8
  if (focusRing_) {
    cr->save();
    const double ringSide = side + 8;
    AddGlobePath(cr, cx - ringSide / 2.0, cy - ringSide / 2.0, ringSide);
    cr->set_source_rgb(kOffWhite.r, kOffWhite.g, kOffWhite.b);
    cr->set_line_width(2.0);  // unscaled
    cr->stroke();
    cr->restore();
  }

  cr->save();
  // hover lift: whole-globe center scale
  if (hoverScale_ != 1.0) {
    cr->translate(cx, cy);
    cr->scale(hoverScale_, hoverScale_);
    cr->translate(-cx, -cy);
  }
  // the true even-odd clip to the globe (visually identical to the windows
  // square-minus-globe overlay, minus the ground-color dependency)
  AddGlobePath(cr, ox, oy, side);
  cr->clip();

  // 1) globeFill — the lit globe base
  cr->set_source_rgb(base.r, base.g, base.b);
  cr->paint();

  // 2) gridLayer (connector lattice + live dots)
  if (gridOpacity_ > 0.01) {
    cr->push_group();
    // 2a wash: globe path filled white @4% — under the clip a plain paint is
    // identical
    cr->set_source_rgba(1, 1, 1, 0.04);
    cr->paint();
    // 2b equator: band top 126·s, height 2·s, page color
    cr->set_source_rgb(kGround.r, kGround.g, kGround.b);
    cr->rectangle(ox, oy + 126 * s, side, 2 * s);
    cr->fill();
    // 2c meridian: centered ellipse rx 51.5·s ry 127·s, stroke 2·s
    cr->save();
    cr->translate(cx, cy);
    cr->scale(51.5 * s, 127 * s);
    cr->arc(0, 0, 1.0, 0, 2 * G_PI);
    cr->restore();
    cr->set_line_width(2 * s);
    cr->stroke();
    // 2d the live provider dots
    const int64_t cols = std::max<int64_t>(std::max(gridWidth_, gridHeight_), 0);
    if (cols > 0 && !dots_.empty()) {
      const double cell = side / static_cast<double>(cols);
      for (const auto& [key, dot] : dots_) {
        const Argb from = kDotColors[static_cast<int>(dot.previous)];
        const Argb to = kDotColors[static_cast<int>(dot.state)];
        const double t = dot.colorProgress;
        const double a = from.a + (to.a - from.a) * t;
        if (a <= 0.001 && dot.colorProgress >= 1.0) continue;
        const double scale = EaseInOutCubic(dot.sizeProgress);
        if (scale <= 0.001) continue;
        cr->set_source_rgba(from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                            from.b + (to.b - from.b) * t, a);
        cr->arc(ox + dot.x * cell + cell / 2.0, oy + dot.y * cell + cell / 2.0,
                (cell / 2.0) * scale, 0, 2 * G_PI);
        cr->fill();
      }
    }
    cr->pop_group_to_source();
    cr->paint_with_alpha(gridOpacity_);
  }

  // 3) blobLayer — five opaque 180pt circles above the grid
  if (blobsVisible_) {
    for (int i = 0; i < 5; ++i) {
      const double* entry = kBlobEntry[blobOffsetOrder_[i]];
      const double* fin = kBlobFinal[blobOffsetOrder_[i]];
      const double t = blobProgress_;
      const double bx = cx + (entry[0] + (fin[0] - entry[0]) * t) * side;
      const double by = cy + (entry[1] + (fin[1] - entry[1]) * t) * side;
      const Rgb& color = kBlobColors[blobColorOrder_[i]];
      cr->set_source_rgb(color.r, color.g, color.b);
      cr->arc(bx, by, (kBlobD * s) / 2.0, 0, 2 * G_PI);
      cr->fill();
    }
  }

  // 4) idleLayer — pulse, coreRing, coreGap, core
  if (idleOpacity_ > 0.01) {
    cr->push_group();
    if (pulseOpacity_ > 0.001) {
      cr->set_source_rgba(kElectric.r, kElectric.g, kElectric.b, pulseOpacity_);
      cr->arc(cx, cy, (kPulseD * s / 2.0) * pulseScale_, 0, 2 * G_PI);
      cr->fill();
    }
    cr->set_source_rgb(kElectric.r, kElectric.g, kElectric.b);
    cr->set_line_width(4 * s);
    cr->arc(cx, cy, kCoreRingD * s / 2.0, 0, 2 * G_PI);
    cr->stroke();
    cr->set_source_rgb(restingBase.r, restingBase.g, restingBase.b);
    cr->set_line_width(2 * s);
    cr->arc(cx, cy, kCoreGapD * s / 2.0, 0, 2 * G_PI);
    cr->stroke();
    cr->set_source_rgb(kElectric.r, kElectric.g, kElectric.b);
    cr->arc(cx, cy, kCoreD * s / 2.0, 0, 2 * G_PI);
    cr->fill();
    cr->pop_group_to_source();
    cr->paint_with_alpha(idleOpacity_);
  }

  // 5) glyph — error/processing icon, faint, centered, 32·s
  if (glyphOpacity_ > 0.01) {
    cr->set_source_rgba(kFaint.r, kFaint.g, kFaint.b, glyphOpacity_);
    const double g = kGlyphSize * s;
    cr->set_line_width(2.5 * s);
    if (state_ == State::Error) {
      // warning triangle + bang
      cr->move_to(cx, cy - g / 2.0);
      cr->line_to(cx + g / 2.0, cy + g / 2.0);
      cr->line_to(cx - g / 2.0, cy + g / 2.0);
      cr->close_path();
      cr->stroke();
      cr->move_to(cx, cy - g / 8.0);
      cr->line_to(cx, cy + g / 6.0);
      cr->stroke();
      cr->arc(cx, cy + g / 3.2, 1.4 * s, 0, 2 * G_PI);
      cr->fill();
    } else if (state_ == State::Processing) {
      // clock: circle + hands
      cr->arc(cx, cy, g / 2.0, 0, 2 * G_PI);
      cr->stroke();
      cr->move_to(cx, cy);
      cr->line_to(cx, cy - g / 3.2);
      cr->stroke();
      cr->move_to(cx, cy);
      cr->line_to(cx + g / 4.0, cy + g / 8.0);
      cr->stroke();
    }
  }

  cr->restore();
}

}  // namespace urnw
