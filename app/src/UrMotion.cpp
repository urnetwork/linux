// SPDX-License-Identifier: MPL-2.0
#include "UrMotion.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace urnw::motion {

bool ShouldAnimate() {
  if (auto settings = Gtk::Settings::get_default()) {
    return settings->property_gtk_enable_animations().get_value();
  }
  return true;
}

// Solve the bezier's x(u)=t for u (Newton, bisection fallback), return y(u) —
// the same curve CSS cubic-bezier() defines.
double EvalBezier(Bezier p1, Bezier p2, double t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  auto sample = [](double a1, double a2, double u) {
    // cubic bezier with P0=0, P3=1: ((1-3a2+3a1)u + (3a2-6a1))u + 3a1) * u
    return ((1.0 - 3.0 * a2 + 3.0 * a1) * u + (3.0 * a2 - 6.0 * a1)) * u * u + 3.0 * a1 * u;
  };
  auto slope = [](double a1, double a2, double u) {
    return 3.0 * (1.0 - 3.0 * a2 + 3.0 * a1) * u * u + 2.0 * (3.0 * a2 - 6.0 * a1) * u +
           3.0 * a1;
  };
  double u = t;
  for (int i = 0; i < 8; ++i) {
    const double x = sample(p1.x, p2.x, u) - t;
    const double d = slope(p1.x, p2.x, u);
    if (std::abs(x) < 1e-6) break;
    if (std::abs(d) < 1e-6) break;
    u -= x / d;
    u = std::clamp(u, 0.0, 1.0);
  }
  // bisection fallback for a curve too flat for Newton
  if (std::abs(sample(p1.x, p2.x, u) - t) > 1e-4) {
    double lo = 0, hi = 1;
    for (int i = 0; i < 24; ++i) {
      u = 0.5 * (lo + hi);
      if (sample(p1.x, p2.x, u) < t) {
        lo = u;
      } else {
        hi = u;
      }
    }
  }
  return sample(p1.y, p2.y, u);
}

// ---- MotionBin --------------------------------------------------------------

MotionBin::MotionBin() {
  set_overflow(Gtk::Overflow::VISIBLE);
}

MotionBin::~MotionBin() {
  if (child_) child_->unparent();
}

void MotionBin::set_child(Gtk::Widget& child) {
  if (child_) child_->unparent();
  child_ = &child;
  child.set_parent(*this);
}

void MotionBin::set_translate_y(double y) {
  if (translateY_ == y) return;
  translateY_ = y;
  queue_allocate();
}

void MotionBin::set_scale(double s) {
  if (scale_ == s) return;
  scale_ = s;
  queue_allocate();
}

void MotionBin::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum,
                              int& natural, int& minimum_baseline,
                              int& natural_baseline) const {
  minimum = natural = 0;
  minimum_baseline = natural_baseline = -1;
  if (!child_) return;
  child_->measure(orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
}

void MotionBin::size_allocate_vfunc(int width, int height, int baseline) {
  if (!child_) return;
  GskTransform* t = nullptr;  // identity
  if (translateY_ != 0) {
    const graphene_point_t p{0.0f, static_cast<float>(translateY_)};
    t = gsk_transform_translate(t, &p);
  }
  if (scale_ != 1) {
    // scale about the child's center: T(c) * S * T(-c)
    const graphene_point_t c{width * 0.5f, height * 0.5f};
    const graphene_point_t nc{-width * 0.5f, -height * 0.5f};
    t = gsk_transform_translate(t, &c);
    t = gsk_transform_scale(t, static_cast<float>(scale_), static_cast<float>(scale_));
    t = gsk_transform_translate(t, &nc);
  }
  // gtk_widget_allocate takes ownership of the transform (transfer full);
  // NULL is the identity.
  gtk_widget_allocate(child_->gobj(), width, height, baseline, t);
}

// ---- the animator -----------------------------------------------------------

namespace {

void RunTicker(Gtk::Widget& lifetime, int durationMs, Bezier p1, Bezier p2,
               std::function<void(double)> apply, std::function<void()> done) {
  auto startTime = std::make_shared<gint64>(-1);
  lifetime.add_tick_callback(
      [durationMs, p1, p2, apply = std::move(apply), done = std::move(done),
       startTime](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
        const gint64 now = clock->get_frame_time();  // µs
        if (*startTime < 0) *startTime = now;
        const double t =
            std::min(1.0, static_cast<double>(now - *startTime) / (durationMs * 1000.0));
        apply(EvalBezier(p1, p2, t));
        if (t < 1.0) return true;
        if (done) done();
        return false;
      });
}

}  // namespace

void AnimateValue(Gtk::Widget& lifetime, int delayMs, int durationMs, Bezier p1, Bezier p2,
                  std::function<void(double)> apply, std::function<void()> done) {
  if (delayMs <= 0) {
    RunTicker(lifetime, durationMs, p1, p2, std::move(apply), std::move(done));
    return;
  }
  // track_obj drops the timeout if the widget is destroyed before it fires
  Glib::signal_timeout().connect_once(
      sigc::track_obj(
          [&lifetime, durationMs, p1, p2, apply = std::move(apply), done = std::move(done)] {
            RunTicker(lifetime, durationMs, p1, p2, apply, done);
          },
          lifetime),
      static_cast<unsigned>(delayMs));
}

void AnimateSpring(Gtk::Widget& lifetime, double from, double damping, int periodMs,
                   std::function<void(double)> apply) {
  // Underdamped spring toward 1.0: x(t) = 1 + (x0-1) e^(-ζω0 t)(cos ωd t + (ζω0/ωd) sin ωd t)
  const double w0 = 2.0 * G_PI / (periodMs / 1000.0);
  const double zeta = damping;
  const double wd = w0 * std::sqrt(std::max(1e-6, 1.0 - zeta * zeta));
  const double x0 = from - 1.0;
  auto startTime = std::make_shared<gint64>(-1);
  lifetime.add_tick_callback(
      [w0, zeta, wd, x0, apply = std::move(apply),
       startTime](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
        const gint64 now = clock->get_frame_time();
        if (*startTime < 0) *startTime = now;
        const double t = static_cast<double>(now - *startTime) / 1e6;  // seconds
        const double envelope = std::exp(-zeta * w0 * t);
        const double x =
            1.0 + x0 * envelope * (std::cos(wd * t) + (zeta * w0 / wd) * std::sin(wd * t));
        // settled: envelope under a third of a pixel at hero scale, and past
        // the visually-settled point (~650ms at 0.86/60)
        if (envelope * std::abs(x0) < 0.0005 && t > 0.3) {
          apply(1.0);
          return false;
        }
        apply(x);
        return true;
      });
}

// ---- the four primitives ----------------------------------------------------

void ArmHeroBloom(MotionBin& hero) {
  if (!ShouldAnimate()) return;
  hero.set_scale(kHeroScaleFrom);
  hero.set_opacity(0.0);
}

void StartHeroBloom(MotionBin& hero, double damping) {
  (void)damping;
  if (!ShouldAnimate()) {
    hero.settle();
    return;
  }
  // The spec's sanctioned no-spring-API port (§8.6): the 0.86/60 reveal
  // spring reads as ~95% travel by ~300ms, settled ~650ms, with sub-visible
  // overshoot — a 650ms cubic-bezier(0.16, 1.0, 0.30, 1.0) matches that
  // envelope. (A literal e^(-ζω₀t) with ω₀ = 2π/60ms decays ~10x faster than
  // the Composition spring's observed settle, so the bezier is not merely
  // acceptable here — it is the closer match.)
  const uint64_t gen = hero.generation();
  AnimateValue(hero, 0, 650, Bezier{0.16, 1.0}, Bezier{0.30, 1.0},
               [&hero, gen](double eased) {
                 if (hero.generation() != gen) return;
                 hero.set_scale(kHeroScaleFrom + (1.0 - kHeroScaleFrom) * eased);
               });
  AnimateValue(hero, 0, kHeroMs, kStandardP1, kStandardP2, [&hero, gen](double eased) {
    if (hero.generation() != gen) return;
    hero.set_opacity(eased);
  });
}

void RiseIn(MotionBin& bin, Rise direction, double distDip, int delayMs, int fadeMs,
            int riseMs) {
  // Skip-if-hidden, at the choke point: never force-show, never pose a hidden
  // element — its owner controls its visibility.
  if (!bin.get_visible()) return;
  if (!ShouldAnimate()) return;  // current pose IS the settled pose

  const uint64_t gen = bin.generation();
  if (distDip != 0.0) {
    const double fromY = (direction == Rise::Up) ? distDip : -distDip;
    bin.set_translate_y(fromY);  // pre-delay frames correct
    AnimateValue(bin, delayMs, riseMs, kStandardP1, kStandardP2,
                 [&bin, fromY, gen](double eased) {
                   if (bin.generation() != gen) return;
                   bin.set_translate_y(fromY * (1.0 - eased));
                 });
  }
  bin.set_opacity(0.0);
  AnimateValue(bin, delayMs, fadeMs, kStandardP1, kStandardP2, [&bin, gen](double eased) {
    if (bin.generation() != gen) return;
    bin.set_opacity(eased);
  });
}

void RippleGroup(const std::vector<RippleEntry>& entries, int baseDelayMs, int staggerMs) {
  int slot = 0;
  for (const auto& entry : entries) {
    // the slot advances for EVERY listed entry, present or not: delays are a
    // property of the composition, not of what happens to be visible
    if (entry.bin) {
      RiseIn(*entry.bin, entry.direction, entry.distDip, baseDelayMs + slot * staggerMs);
    }
    ++slot;
  }
}

}  // namespace urnw::motion
