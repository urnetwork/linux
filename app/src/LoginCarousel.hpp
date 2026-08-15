// The login screen's carousel — the GTK port of windows:LoginCarousel.{h,cpp}
// (itself iOS Authenticate/LoginInitial/LoginCarousel.swift).
//
// Three brand images crossfading inside the ur globe silhouette, with the
// headline that belongs to each sliding up and out as the next slides in, and
// the "with URnetwork" line trailing it. Five seconds a slide.
//
// It is not decoration: the login screen has no heading of its own — the
// carousel supplies the headline. Timings are iOS's, kept in the same units
// so the platforms can be diffed: slide 5000ms, image crossfade 700ms linear,
// text out/in 500ms, bottom-line delay 400ms.
//
// The timer runs ONLY while the window is on screen and the flow is on the
// initial step (SetActive) — a tray app spends most of its life hidden, and
// an animation nobody can see is pure wakeups.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <gtkmm.h>

#include "UrMotion.hpp"

namespace urnw {

// A brand image clipped to the ur globe silhouette (the same kGlobePath every
// platform masks with), drawn aspect-fill. Crossfades ride widget opacity.
class GlobeImage : public Gtk::Widget {
 public:
  GlobeImage();
  ~GlobeImage() override;
  void set_texture(const Glib::RefPtr<Gdk::Texture>& texture) {
    texture_ = texture;
    queue_draw();
  }
  // Fired from size_allocate. A plain Gtk::Widget subclass has NO layout
  // manager, so its size_allocate vfunc genuinely runs — unlike the Overlay
  // above it, whose allocation is dispatched to GtkOverlayLayout and whose
  // vfunc would be dead code. The carousel hangs its metrics resync here.
  std::function<void(int width, int height)> on_resize;

 protected:
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;
  void size_allocate_vfunc(int width, int height, int baseline) override;

 private:
  Glib::RefPtr<Gdk::Texture> texture_;
};

class LoginCarousel : public Gtk::Overlay {
 public:
  LoginCarousel();

  // (Re)paint the current slide's headline from the localization store.
  void ApplyStrings();
  // Start/stop the slide timer. Off while the window is hidden or the login
  // flow has moved past the initial step.
  void SetActive(bool active);
  // The host has been reparented (narrow <-> wide breakpoint): stop the
  // boards, land a clean whole-slide pose, re-derive the metrics.
  void HostReparented();

 private:
  void Build();
  void ApplyMetrics();          // fit the globe to the slot, the type to the globe
  void ShowSlide(size_t index); // paint text + image for a slide, no animation
  void Advance();               // timer tick: animate out, swap, animate in
  void AnimateTextOut();
  void AnimateTextIn();
  void CrossfadeTo(size_t index);
  void StopAnimations();        // invalidate every in-flight animation
  Glib::RefPtr<Gdk::Texture> TextureFor(size_t index);
  const char* HeadlineFor(size_t index) const;
  void ApplyLabelFonts(double fontPx);

  GlobeImage* currentImage_ = nullptr;
  GlobeImage* nextImage_ = nullptr;
  Gtk::Label* headline_ = nullptr;
  Gtk::Label* bottomLine_ = nullptr;
  motion::MotionBin* headlineShift_ = nullptr;
  motion::MotionBin* bottomShift_ = nullptr;
  Gtk::Box* textBox_ = nullptr;

  std::array<Glib::RefPtr<Gdk::Texture>, 3> textures_;
  sigc::connection timer_;
  // Every animation captures the epoch it started under and early-outs when
  // it has moved on — the GTK analogue of Storyboard::Stop() releasing its
  // property, so a stopped board can never overwrite ShowSlide's clean pose.
  uint64_t animEpoch_ = 0;
  size_t index_ = 0;
  bool active_ = false;
  bool metricsQueued_ = false;  // one idle-time ApplyMetrics per resize burst
  double side_ = 0;  // the globe's current fitted side (metrics)
};

}  // namespace urnw
