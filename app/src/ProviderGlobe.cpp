// SPDX-License-Identifier: MPL-2.0
#include "ProviderGlobe.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include <glib.h>

#include "RuntimePaths.hpp"
#include "WorldTopology.hpp"

namespace urnw {
namespace {

// Visual constants, matching the /ip globe on ur.io (see PROVIDERLOCATIONS.md
// "ur.io /ip globe"). All lengths are in the 600-unit virtual space.
constexpr Rgba kSphere{0x10 / 255.0, 0x10 / 255.0, 0x10 / 255.0, 1.0};    // #101010
constexpr Rgba kLand{0xF8 / 255.0, 0xF8 / 255.0, 0xF8 / 255.0, 1.0};      // #F8F8F8
constexpr Rgba kGraticule{0xCC / 255.0, 0xCC / 255.0, 0xCC / 255.0, 0.376};
// the web's fallback for a provider whose country is unknown
constexpr Rgba kUnknownCountry{0x00 / 255.0, 0x99 / 255.0, 0xFF / 255.0, 1.0};  // #0099FF

constexpr double kLandStrokeWidth = 0.3;
constexpr double kGraticuleStrokeWidth = 0.5;
constexpr double kDotRadius = 7.0;
// the selected provider keeps its solid dot; the ring is an outline sitting
// kSelectedRingGap outside the dot's edge (radii are stroke centerlines)
constexpr double kSelectedRingGap = 4.0;
constexpr double kSelectedRingStroke = 1.5;
constexpr double kSelectedRingRadius = kDotRadius + kSelectedRingGap + kSelectedRingStroke / 2.0;
// The sphere is sized to fit its box with room for a selected dot's ring at the
// limb, so the globe never paints outside the component. (The web zooms past
// its frame and crops; here the globe sits fully inside instead.)
constexpr double kGlobeScale =
    globe::kCenter - kDotRadius - kSelectedRingGap - kSelectedRingStroke;

// Recentering is a primary interaction here (every wheel step recenters), not
// the web's occasional pointer-leave animation, so it is snappier than the
// web's 1000ms -- a slow ease makes rapid stepping feel like it lags the input.
constexpr gint64 kRecenterUs = 450000;
constexpr double kClickSlop = 28.0;  // virtual px
// how far the pointer travels to advance one provider, as a fraction of the
// component's width
constexpr double kWheelStepWidthFraction = 0.18;
// one mouse-wheel notch is one provider; GTK reports notches as +/-1 deltas, so
// scale them to a full threshold
constexpr double kScrollNotchTravel = 1.0;

constexpr int kDefaultContentHeight = 260;

void SetCairoColor(const Cairo::RefPtr<Cairo::Context>& cr, const Rgba& c) {
  cr->set_source_rgba(c.r, c.g, c.b, c.a);
}

double EaseInOutCubic(double t) {
  const double p = std::clamp(t, 0.0, 1.0);
  return p < 0.5 ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3) / 2;
}

// ---- world topology, decoded once per process off the main loop -------------

struct TopologyCache {
  std::mutex mutex;
  std::shared_ptr<const WorldTopology> topology;  // null until decoded
  bool started = false;
  std::vector<std::function<void()>> waiters;
};

TopologyCache& Cache() {
  static TopologyCache cache;
  return cache;
}

std::string ReadFileIfPresent(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Where the installed asset lives. meson passes the configured package data
// dir; the shared ladder (RuntimePaths.hpp) prefixes $APPDIR inside an
// AppImage and falls back to the build tree. A miss only costs the land layer.
#ifndef UR_PKGDATADIR
#define UR_PKGDATADIR "/usr/share/urnetwork"
#endif

std::string FindWorldTopologyAsset() {
  return ResolveRuntimePath(UR_PKGDATADIR "/world-110m.json", G_FILE_TEST_IS_REGULAR,
                            "assets/world-110m.json");
}

// Starts the one-shot background decode if it has not run, and registers
// `onLoaded` to fire on the GTK main loop when it completes (immediately, on
// the next idle, if it is already done). Never blocks the caller: ~100 KB of
// TopoJSON to parse and stitch would drop frames on the main loop.
void EnsureWorldTopology(std::function<void()> onLoaded) {
  TopologyCache& cache = Cache();
  bool alreadyLoaded = false;
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.topology) {
      alreadyLoaded = true;
    } else {
      cache.waiters.push_back(std::move(onLoaded));
      start = !cache.started;
      cache.started = true;
    }
  }
  if (alreadyLoaded) {
    PostToMain(std::move(onLoaded));
    return;
  }
  if (!start) return;

  std::thread([] {
    auto decoded = std::make_shared<WorldTopology>();
    const std::string path = FindWorldTopologyAsset();
    bool ok = false;
    if (!path.empty()) {
      const std::string text = ReadFileIfPresent(path);
      ok = !text.empty() && WorldTopology::Decode(text, *decoded);
    }
    if (!ok) {
      g_warning("world topology asset unavailable (%s); the globe will show no land",
                path.empty() ? "not found" : path.c_str());
      decoded->countries.clear();
    }
    std::vector<std::function<void()>> waiters;
    {
      TopologyCache& cache = Cache();
      std::lock_guard<std::mutex> lock(cache.mutex);
      // published even when empty, so the waiters stop waiting
      cache.topology = decoded;
      waiters.swap(cache.waiters);
    }
    PostToMain([waiters = std::move(waiters)] {
      for (const auto& waiter : waiters) waiter();
    });
  }).detach();
}

std::shared_ptr<const WorldTopology> LoadedTopology() {
  TopologyCache& cache = Cache();
  std::lock_guard<std::mutex> lock(cache.mutex);
  return cache.topology;
}

}  // namespace

ProviderGlobe::ProviderGlobe() {
  set_content_height(kDefaultContentHeight);
  set_hexpand(true);
  set_draw_func(sigc::mem_fun(*this, &ProviderGlobe::OnDraw));

  // Two interaction modes. With providers on the globe a horizontal drag is a
  // scroll wheel locked to the provider order -- free rotation would fight the
  // centering animation. With none, there is nothing to traverse, so the globe
  // rotates freely.
  auto drag = Gtk::GestureDrag::create();
  drag->signal_drag_begin().connect(sigc::mem_fun(*this, &ProviderGlobe::OnDragBegin));
  drag->signal_drag_update().connect(sigc::mem_fun(*this, &ProviderGlobe::OnDragUpdate));
  drag->signal_drag_end().connect(sigc::mem_fun(*this, &ProviderGlobe::OnDragEnd));
  add_controller(drag);

  // A mouse has no horizontal axis, so a plain wheel steps the provider wheel
  // too -- the desktop equivalent of the touch swipe.
  auto scroll = Gtk::EventControllerScroll::create();
  scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
  scroll->signal_scroll().connect(sigc::mem_fun(*this, &ProviderGlobe::OnScroll), false);
  add_controller(scroll);

  auto click = Gtk::GestureClick::create();
  click->signal_pressed().connect(sigc::mem_fun(*this, &ProviderGlobe::OnPressed));
  add_controller(click);

  // land arrives asynchronously; redraw when it does
  auto alive = std::make_shared<bool>(true);
  alive_ = alive;
  EnsureWorldTopology([this, alive] {
    if (!*alive) return;  // the widget went away while the decode ran
    queue_draw();
  });
}

ProviderGlobe::~ProviderGlobe() {
  if (alive_) *alive_ = false;
}

void ProviderGlobe::SetColorResolver(std::function<Rgba(const std::string&)> resolver) {
  colorResolver_ = std::move(resolver);
  queue_draw();
}

void ProviderGlobe::SetRows(std::vector<ProviderLocationRow> rows) {
  rows_ = std::move(rows);
  wheel_ = WheelOrderByLongitude(rows_);
  // a selection whose provider left the window is dropped by the sheet; keep
  // the globe consistent if it happens here first
  if (!selectedClientId_.empty()) {
    const bool stillPresent =
        std::any_of(rows_.begin(), rows_.end(),
                    [this](const ProviderLocationRow& r) { return r.clientId == selectedClientId_; });
    if (!stillPresent) selectedClientId_.clear();
  }
  // center once on the first provider that appears, then never again except on
  // an explicit selection
  if (!centeredOnce_ && !wheel_.empty()) {
    AnimateToSelection();
  }
  queue_draw();
}

void ProviderGlobe::SetSelected(const std::string& clientId) {
  if (selectedClientId_ == clientId) return;
  selectedClientId_ = clientId;
  AnimateToSelection();
  queue_draw();
}

void ProviderGlobe::AnimateToSelection() {
  // the selected provider, else (only until the first centering) the oldest one
  const ProviderLocationRow* target = nullptr;
  for (const auto& row : rows_) {
    if (row.plottable() && row.clientId == selectedClientId_) {
      target = &row;
      break;
    }
  }
  if (target == nullptr && !centeredOnce_ && !wheel_.empty()) {
    target = &rows_[static_cast<size_t>(wheel_.front())];
  }
  if (target == nullptr) return;
  centeredOnce_ = true;

  const GlobeRotation to = globe::RotationCentering(static_cast<float>(target->lon),
                                                    static_cast<float>(target->lat));
  // resolve the target to its nearest equivalent angle so the globe spins the
  // short way around
  animFrom_ = rotation_;
  animTo_ = globe::LerpRotation(rotation_, to, 1.0f);
  animStart_ = g_get_monotonic_time();
  animating_ = true;
  if (!ticking_) {
    ticking_ = true;
    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool { return OnTick(); });
  }
}

bool ProviderGlobe::OnTick() {
  if (!animating_) {
    ticking_ = false;
    return false;
  }
  const double p = std::clamp(
      static_cast<double>(g_get_monotonic_time() - animStart_) / kRecenterUs, 0.0, 1.0);
  rotation_ = globe::LerpRotation(animFrom_, animTo_, static_cast<float>(EaseInOutCubic(p)));
  queue_draw();
  if (1.0 <= p) {
    animating_ = false;
    ticking_ = false;
    return false;
  }
  return true;
}

void ProviderGlobe::StepWheel(int steps) {
  if (steps == 0 || wheel_.empty()) return;
  int current = -1;
  for (size_t i = 0; i < wheel_.size(); ++i) {
    if (rows_[static_cast<size_t>(wheel_[i])].clientId == selectedClientId_) {
      current = static_cast<int>(i);
      break;
    }
  }
  // nothing selected yet: the first step lands on the westernmost provider
  const int next = current < 0 ? 0
                               : globe::WrapIndex(current, steps, static_cast<int>(wheel_.size()));
  if (next < 0) return;
  const std::string& clientId = rows_[static_cast<size_t>(wheel_[static_cast<size_t>(next)])].clientId;
  if (on_select) on_select(clientId);
  SetSelected(clientId);
}

void ProviderGlobe::OnDragBegin(double, double) {
  lastDragOffsetX_ = 0;
  lastDragOffsetY_ = 0;
  wheelTravel_ = 0;
}

void ProviderGlobe::OnDragUpdate(double offsetX, double offsetY) {
  const double dx = offsetX - lastDragOffsetX_;
  const double dy = offsetY - lastDragOffsetY_;
  lastDragOffsetX_ = offsetX;
  lastDragOffsetY_ = offsetY;

  if (wheel_.empty()) {
    // free-form rotation: nothing to traverse
    const float unit = globe::UnitFor(static_cast<float>(get_width()),
                                      static_cast<float>(get_height()));
    if (unit <= 0) return;
    const float k = globe::DragDegreesPerVirtualPx(static_cast<float>(kGlobeScale));
    animating_ = false;  // the user takes over from any running recenter
    rotation_.lambda += static_cast<float>(dx) / unit * k;
    rotation_.phi = std::clamp(rotation_.phi - static_cast<float>(dy) / unit * k, -90.0f, 90.0f);
    queue_draw();
    return;
  }

  wheelTravel_ += dx;
  const WheelStep step = globe::ResolveWheelStep(
      static_cast<float>(wheelTravel_), static_cast<float>(get_width() * kWheelStepWidthFraction));
  if (step.steps != 0) {
    wheelTravel_ = step.remainingTravel;
    StepWheel(step.steps);
  }
}

void ProviderGlobe::OnDragEnd(double, double) { wheelTravel_ = 0; }

bool ProviderGlobe::OnScroll(double dx, double dy) {
  if (wheel_.empty()) return false;
  // a horizontal wheel where there is one, the vertical wheel otherwise
  const double delta = dx != 0 ? dx : dy;
  if (delta == 0) return false;
  // scrolling down/right advances, matching the swipe-left direction
  scrollTravel_ -= delta * kScrollNotchTravel;
  const WheelStep step =
      globe::ResolveWheelStep(static_cast<float>(scrollTravel_), kScrollNotchTravel);
  if (step.steps != 0) {
    scrollTravel_ = step.remainingTravel;
    StepWheel(step.steps);
  }
  return true;
}

void ProviderGlobe::OnPressed(int, double x, double y) {
  const float width = static_cast<float>(get_width());
  const float height = static_cast<float>(get_height());
  const GlobePoint clickVirtual = globe::ToVirtual(static_cast<float>(x), static_cast<float>(y),
                                                   width, height);
  std::vector<GlobePoint> points;
  std::vector<int> pointRows;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (!rows_[i].plottable()) continue;
    const GlobeProjection projection =
        globe::Project(static_cast<float>(rows_[i].lon), static_cast<float>(rows_[i].lat),
                       rotation_.lambda, rotation_.phi, static_cast<float>(kGlobeScale));
    if (!projection.visible) continue;
    points.push_back(projection.point);
    pointRows.push_back(static_cast<int>(i));
  }
  const int hit = globe::NearestWithin(clickVirtual.x, clickVirtual.y, points, kClickSlop);
  if (hit < 0) return;
  const std::string& clientId = rows_[static_cast<size_t>(pointRows[static_cast<size_t>(hit)])].clientId;
  if (on_select) on_select(clientId);
  SetSelected(clientId);
}

void ProviderGlobe::OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  // fit center: the globe is scaled to the smaller dimension and centered in
  // both, so a box shorter than it is wide neither crops nor offsets it
  const float unit = globe::UnitFor(w, h);
  if (unit <= 0) return;
  const float scale = static_cast<float>(kGlobeScale);

  // kGlobeScale already keeps every draw inside the box; this is the backstop
  // so nothing can paint over the widgets above and below
  cr->rectangle(0, 0, width, height);
  cr->clip();

  auto toCanvas = [w, h](const GlobePoint& point) { return globe::ToCanvas(point, w, h); };

  // the sphere
  SetCairoColor(cr, kSphere);
  cr->arc(w / 2.0, h / 2.0, scale * unit, 0, 2 * G_PI);
  cr->fill();

  // land: filled countries with a hairline border, clamped at the horizon
  if (auto topology = LoadedTopology()) {
    for (const auto& country : topology->countries) {
      for (const auto& ring : country.rings) {
        bool anyVisible = false;
        bool started = false;
        for (size_t i = 0; i + 1 < ring.size(); i += 2) {
          const float lon = ring[i];
          const float lat = ring[i + 1];
          if (0.0f <= globe::CosAngleToCenter(lon, lat, rotation_.lambda, rotation_.phi)) {
            anyVisible = true;
          }
          const GlobePoint mapped = toCanvas(
              globe::ProjectClamped(lon, lat, rotation_.lambda, rotation_.phi, scale));
          if (started) {
            cr->line_to(mapped.x, mapped.y);
          } else {
            cr->move_to(mapped.x, mapped.y);
            started = true;
          }
        }
        if (!started) continue;
        if (!anyVisible) {
          cr->begin_new_path();  // entirely on the back hemisphere
          continue;
        }
        cr->close_path();
        SetCairoColor(cr, kLand);
        cr->fill_preserve();
        SetCairoColor(cr, kSphere);
        cr->set_line_width(kLandStrokeWidth * unit);
        cr->stroke();
      }
    }
  }

  // graticule over the land, as on the web; broken wherever it crosses the
  // horizon so the back half is not drawn as a chord across the sphere
  SetCairoColor(cr, kGraticule);
  cr->set_line_width(kGraticuleStrokeWidth * unit);
  for (const auto& line : globe::Graticule()) {
    bool started = false;
    for (size_t i = 0; i + 1 < line.size(); i += 2) {
      const GlobeProjection projection =
          globe::Project(line[i], line[i + 1], rotation_.lambda, rotation_.phi, scale);
      if (!projection.visible) {
        started = false;
        continue;
      }
      const GlobePoint mapped = toCanvas(projection.point);
      if (started) {
        cr->line_to(mapped.x, mapped.y);
      } else {
        cr->move_to(mapped.x, mapped.y);
        started = true;
      }
    }
    cr->stroke();
  }

  // provider dots
  for (const auto& row : rows_) {
    if (!row.plottable()) continue;
    const GlobeProjection projection =
        globe::Project(static_cast<float>(row.lon), static_cast<float>(row.lat),
                       rotation_.lambda, rotation_.phi, scale);
    if (!projection.visible) continue;
    const GlobePoint center = toCanvas(projection.point);
    const Rgba color = (row.countryCode.empty() || !colorResolver_)
                           ? kUnknownCountry
                           : colorResolver_(row.countryCode);
    SetCairoColor(cr, color);
    cr->arc(center.x, center.y, kDotRadius * unit, 0, 2 * G_PI);
    cr->fill();
    if (row.clientId == selectedClientId_) {
      cr->arc(center.x, center.y, kSelectedRingRadius * unit, 0, 2 * G_PI);
      cr->set_line_width(kSelectedRingStroke * unit);
      cr->stroke();
    }
  }
}

}  // namespace urnw
