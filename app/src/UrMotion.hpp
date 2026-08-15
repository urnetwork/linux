// The motion system — the GTK port of windows:app/src/App/UrMotion.h. One
// place for durations, easings, distances and the four primitives, so every
// animation in the app composes off these tokens instead of hand-rolling new
// ones. Numbers are normative (docs/linux_agent_help.md §8.3); do not tweak.
//
// UI THREAD ONLY. Everything here drives GTK frame-clock tick callbacks.
//
// Two rules, enforced at this choke point:
//   * ShouldAnimate() gates every non-trivial animation. Animations off in
//     GNOME (org.gnome.desktop.interface enable-animations, surfaced to GTK
//     as gtk-enable-animations) means the user wants motion GONE, not
//     reduced — a gated path renders an instant, fully correct final state
//     with zero animation-property writes.
//   * Exits run one step faster than entrances (kFastMs vs kBaseMs).
//
// GEOMETRY: GTK4 widgets have no free-floating translate/scale property, so
// geometry rides MotionBin — a one-child container that applies a
// GskTransform (translate + scale about the child's center) in its
// allocation, which moves drawing AND input together. Opacity rides the
// widget's own opacity property. One alpha channel per element — never
// compound a bin's opacity with its child's.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <gtkmm.h>

namespace urnw::motion {

// ---- durations (ms) --------------------------------------------------------
inline constexpr int kMicroMs = 90;
inline constexpr int kFastMs = 150;   // hover/press; exits
inline constexpr int kBaseMs = 250;   // the default: page crossfades, disclosure
inline constexpr int kSlowMs = 400;   // a large-surface change
inline constexpr int kHeroMs = 500;
inline constexpr int kEpicMs = 1000;
inline constexpr int kPulseMs = 1500;
inline constexpr int kStaggerMs = 40;
inline constexpr int kMaxStaggerSteps = 6;
inline constexpr int kOverlapMs = 60;

// ---- easings (cubic-bezier control points; P0=(0,0), P3=(1,1)) -------------
struct Bezier {
  double x = 0;
  double y = 0;
};
inline constexpr Bezier kStandardP1{0.10, 0.90};  // settle, don't snap
inline constexpr Bezier kStandardP2{0.20, 1.00};
inline constexpr Bezier kExitP1{0.70, 0.00};      // dismissing: leave fast
inline constexpr Bezier kExitP2{1.00, 0.50};
inline constexpr Bezier kSoftP1{0.40, 0.00};      // gentle disclosure
inline constexpr Bezier kSoftP2{0.20, 1.00};
inline constexpr Bezier kLinearP1{0.0, 0.0};      // the carousel's image crossfade
inline constexpr Bezier kLinearP2{1.0, 1.0};

// ---- springs ----------------------------------------------------------------
inline constexpr double kConnectSpringDamping = 0.75;  // RESERVED: success bloom
inline constexpr int kConnectSpringPeriodMs = 40;
inline constexpr double kRevealSpringDamping = 0.86;
inline constexpr int kRevealSpringPeriodMs = 60;

// ---- distances (dip) / scale ------------------------------------------------
inline constexpr double kDist4 = 4.0;
inline constexpr double kDist8 = 8.0;
inline constexpr double kDist12 = 12.0;
inline constexpr double kDist24 = 24.0;
inline constexpr double kHoverScale = 1.03;
inline constexpr double kPressScale = 0.97;

// ---- Hero Bloom -------------------------------------------------------------
inline constexpr int kHeroHoldMs = 240;   // the held brand beat
inline constexpr int kBrandBeatMs = 120;  // wordmark joins mid-hero-settle
inline constexpr double kHeroScaleFrom = 0.92;
static_assert(kBrandBeatMs * 2 == kHeroHoldMs, "the brand beat is half the hold");
static_assert(kHeroHoldMs == 6 * kStaggerMs, "the hold sits on the stagger grid");

// The one reduce-motion choke point. Reads GTK's gtk-enable-animations
// (GNOME's enable-animations); returns true on any read failure.
bool ShouldAnimate();

// Evaluate the cubic-bezier easing at linear progress t (0..1).
double EvalBezier(Bezier p1, Bezier p2, double t);

// ---- MotionBin --------------------------------------------------------------
// A one-child container whose child can be translated vertically and scaled
// about its own center. The transform is applied at ALLOCATION, so drawing
// and input move together. Settled pose: translate 0, scale 1.
class MotionBin : public Gtk::Widget {
 public:
  MotionBin();
  ~MotionBin() override;

  void set_child(Gtk::Widget& child);
  Gtk::Widget* child() { return child_; }

  void set_translate_y(double y);
  double translate_y() const { return translateY_; }
  void set_scale(double s);
  double scale() const { return scale_; }
  // The settled pose in one write (the SettleRing rule: one implementation).
  // Bumps the generation FIRST so every in-flight animation on this bin
  // no-ops from its next frame — a running ticker would otherwise overwrite
  // the settled pose it was just given (the settle invariant: no pose is ever
  // left stranded, and no cancelled animation may resurrect one).
  void settle() {
    ++generation_;
    set_translate_y(0);
    set_scale(1);
    set_opacity(1.0);
  }
  // Captured by RiseIn/HeroBloom appliers; a mismatch means "cancelled".
  uint64_t generation() const { return generation_; }

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void size_allocate_vfunc(int width, int height, int baseline) override;

 private:
  Gtk::Widget* child_ = nullptr;
  double translateY_ = 0;
  double scale_ = 1;
  uint64_t generation_ = 0;
};

// ---- the animator -----------------------------------------------------------
// Drive `apply(eased)` from the frame clock of `lifetime` over durationMs,
// after delayMs, easing through the bezier. The tick callback dies with the
// widget; `done` fires only when the animation ran to completion (the
// carousel's lifetime guard). Generation-based cancellation belongs to the
// caller: capture an epoch and early-out inside `apply`.
void AnimateValue(Gtk::Widget& lifetime, int delayMs, int durationMs, Bezier p1, Bezier p2,
                  std::function<void(double eased)> apply,
                  std::function<void()> done = {});

// An analytic underdamped spring from `from` to 1.0 (scale), ζ=damping,
// period=periodMs. Runs until visually settled (~650ms at 0.86/60).
void AnimateSpring(Gtk::Widget& lifetime, double from, double damping, int periodMs,
                   std::function<void(double value)> apply);

// ---- the four primitives ----------------------------------------------------
// Which way a RiseIn element travels as it settles INTO place. Up starts
// distDip BELOW its final slot and settles up; Down starts above.
enum class Rise { Up, Down };

// Hero Bloom, split at the Arm/Start seam. Arm writes the start pose
// synchronously (scale kHeroScaleFrom, opacity 0); Start plays the reveal
// spring under a kHeroMs standard fade.
void ArmHeroBloom(MotionBin& hero);
void StartHeroBloom(MotionBin& hero, double damping = kRevealSpringDamping);

// Opacity 0->1 over fadeMs riding translate ±distDip->0 over riseMs — motion
// outlives alpha. Skip-if-hidden lives HERE. distDip 0 degrades to a fade.
void RiseIn(MotionBin& bin, Rise direction, double distDip, int delayMs,
            int fadeMs = kBaseMs, int riseMs = kSlowMs);

// Staggered RiseIns: entry i plays at base + i*stagger BY LISTING POSITION —
// a hidden entry is skipped but holds its slot.
struct RippleEntry {
  MotionBin* bin = nullptr;
  Rise direction = Rise::Up;
  double distDip = kDist8;
};
void RippleGroup(const std::vector<RippleEntry>& entries, int baseDelayMs,
                 int staggerMs = kStaggerMs);

}  // namespace urnw::motion
