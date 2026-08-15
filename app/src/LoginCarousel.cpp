// SPDX-License-Identifier: MPL-2.0
#include "LoginCarousel.hpp"

#include <gtk/gtk.h>

#include <algorithm>

#include "I18n.hpp"
#include "RuntimePaths.hpp"

namespace urnw {
namespace {

// The ur globe silhouette (Assets.xcassets/Icons/ur.symbols.globe.svg) in its
// own 32x32 box — the same path string windows:LoginCarousel.cpp carries.
// Filling it with the image is how the image gets clipped to the globe.
constexpr const char* kGlobePath =
    "M30 8C28.8955 8 28 7.10453 28 6C28 4.89547 27.1045 4 26 4C24.8955 4 24 3.10453 24 2C24 "
    "0.895469 23.1045 0 22 0H10C8.89547 0 8 0.895469 8 2C8 3.10453 7.10453 4 6 4C4.89547 4 4 "
    "4.89547 4 6C4 7.10453 3.10453 8 2 8C0.895469 8 0 8.89547 0 10V22C0 23.1045 0.895469 24 2 "
    "24C3.10453 24 4 24.8955 4 26C4 27.1045 4.89547 28 6 28C7.10453 28 8 28.8955 8 30C8 31.1045 "
    "8.89547 32 10 32H22C23.1045 32 24 31.1045 24 30C24 28.8955 24.8955 28 26 28C27.1045 28 28 "
    "27.1045 28 26C28 24.8955 28.8955 24 30 24C31.1045 24 32 23.1045 32 22V10C32 8.89547 31.1045 "
    "8 30 8Z";

GskPath* GlobePath() {
  static GskPath* path = gsk_path_parse(kGlobePath);
  return path;
}

// The globe never grows past this; the type is derived from what the globe
// actually ends up at, so the headline stays inside the mask at every size.
constexpr double kGlobeMaxSide = 400;
constexpr double kHeadlineWidthRatio = 0.86;
constexpr double kHeadlineSizeDivisor = 9.0;
constexpr double kHeadlineMinSize = 12;
constexpr double kHeadlineMaxSize = 26;

// iOS timings, kept in the same units so the platforms can be diffed.
constexpr int kSlideIntervalMs = 5000;
constexpr int kCrossfadeMs = 700;
constexpr int kTextOutMs = 500;
constexpr int kTextInMs = 500;
constexpr int kBottomDelayMs = 400;

const char* kSlideImages[3] = {"LoginCarousel1.jpg", "LoginCarousel2.jpg",
                               "LoginCarousel3.jpg"};

void SetLabelFont(Gtk::Label& label, const char* family, double sizePx) {
  Pango::AttrList attrs;
  auto famAttr = Pango::Attribute::create_attr_family(family);
  attrs.insert(famAttr);
  auto weightAttr = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
  attrs.insert(weightAttr);
  auto sizeAttr =
      Pango::Attribute::create_attr_size_absolute(static_cast<int>(sizePx * PANGO_SCALE));
  attrs.insert(sizeAttr);
  label.set_attributes(attrs);
}

}  // namespace

// ---- GlobeImage -------------------------------------------------------------

GlobeImage::GlobeImage() { set_overflow(Gtk::Overflow::HIDDEN); }

GlobeImage::~GlobeImage() = default;

void GlobeImage::size_allocate_vfunc(int width, int height, int baseline) {
  Gtk::Widget::size_allocate_vfunc(width, height, baseline);
  if (on_resize) on_resize(width, height);
}

void GlobeImage::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
  if (!texture_) return;
  GskPath* globe = GlobePath();
  if (!globe) return;
  const double w = get_width();
  const double h = get_height();
  const double side = std::min(std::min(w, h), kGlobeMaxSide);
  if (side <= 0) return;

  GtkSnapshot* snap = snapshot->gobj();
  gtk_snapshot_save(snap);
  // center the globe box and scale the 32x32 path space to the fitted side
  const float scale = static_cast<float>(side / 32.0);
  const graphene_point_t origin{static_cast<float>((w - side) / 2.0),
                                static_cast<float>((h - side) / 2.0)};
  gtk_snapshot_translate(snap, &origin);
  gtk_snapshot_scale(snap, scale, scale);
  gtk_snapshot_push_fill(snap, globe, GSK_FILL_RULE_WINDING);
  // aspect-fill the texture over the 32x32 box (UniformToFill)
  const double tw = texture_->get_width();
  const double th = texture_->get_height();
  double drawW = 32, drawH = 32, dx = 0, dy = 0;
  if (tw > 0 && th > 0) {
    const double cover = std::max(32.0 / tw, 32.0 / th);
    drawW = tw * cover;
    drawH = th * cover;
    dx = (32.0 - drawW) / 2.0;
    dy = (32.0 - drawH) / 2.0;
  }
  const graphene_rect_t textureRect{
      {static_cast<float>(dx), static_cast<float>(dy)},
      {static_cast<float>(drawW), static_cast<float>(drawH)}};
  gtk_snapshot_append_texture(snap, texture_->gobj(), &textureRect);
  gtk_snapshot_pop(snap);
  gtk_snapshot_restore(snap);
}

// ---- LoginCarousel ----------------------------------------------------------

LoginCarousel::LoginCarousel() {
  set_overflow(Gtk::Overflow::HIDDEN);  // the headline may never spill the slot
  Build();
  ShowSlide(0);
}

const char* LoginCarousel::HeadlineFor(size_t index) const {
  switch (index % 3) {
    case 0: return T_("see_world_content", "See all the\nworld's content");
    case 1: return T_("stay_private", "Stay\ncompletely\nprivate and\nanonymous");
    default: return T_("build_right", "Build the\ninternet the\nright way");
  }
}

Glib::RefPtr<Gdk::Texture> LoginCarousel::TextureFor(size_t index) {
  auto& slot = textures_[index % 3];
  if (slot) return slot;
#ifdef UR_PKGDATADIR
  const std::string installed = std::string(UR_PKGDATADIR) + "/carousel";
#else
  const std::string installed = "/usr/share/urnetwork/carousel";
#endif
  const std::string dir =
      ResolveRuntimePath(installed, G_FILE_TEST_IS_DIR, "assets/carousel");
  if (dir.empty()) return {};
  try {
    slot = Gdk::Texture::create_from_filename(dir + "/" + kSlideImages[index % 3]);
  } catch (const Glib::Error& e) {
    // A carousel that silently renders no image is indistinguishable from one
    // that is working — say which it is, once, on the app's only channel.
    g_warning("carousel: image '%s' failed to load: %s", kSlideImages[index % 3],
              e.what());
  }
  return slot;
}

void LoginCarousel::Build() {
  // two stacked globe-clipped images: `current` at full opacity, `next` at 0
  currentImage_ = Gtk::make_managed<GlobeImage>();
  nextImage_ = Gtk::make_managed<GlobeImage>();
  nextImage_->set_opacity(0);
  currentImage_->set_hexpand(true);
  currentImage_->set_vexpand(true);
  // The metrics resync rides the image's allocation (it fills the slot, so
  // its size IS the slot size), deferred to idle: relayout must not be driven
  // from inside an allocation pass.
  currentImage_->on_resize = [this](int, int) {
    if (metricsQueued_) return;
    metricsQueued_ = true;
    Glib::signal_idle().connect_once(sigc::track_obj(
        [this] {
          metricsQueued_ = false;
          ApplyMetrics();
        },
        *this));
  };
  set_child(*currentImage_);
  nextImage_->set_hexpand(true);
  nextImage_->set_vexpand(true);
  add_overlay(*nextImage_);

  // the headline sits OVER the globe; pure white and the display faces —
  // these are the only headlines on the signed-out screen
  textBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  textBox_->set_halign(Gtk::Align::CENTER);
  textBox_->set_valign(Gtk::Align::CENTER);

  headline_ = Gtk::make_managed<Gtk::Label>();
  headline_->set_justify(Gtk::Justification::CENTER);
  headline_->set_wrap(true);
  headline_->add_css_class("ur-carousel-headline");
  headlineShift_ = Gtk::make_managed<motion::MotionBin>();
  headlineShift_->set_child(*headline_);
  textBox_->append(*headlineShift_);

  bottomLine_ = Gtk::make_managed<Gtk::Label>();
  bottomLine_->set_justify(Gtk::Justification::CENTER);
  bottomLine_->set_wrap(true);
  bottomLine_->add_css_class("ur-carousel-headline");
  bottomShift_ = Gtk::make_managed<motion::MotionBin>();
  bottomShift_->set_child(*bottomLine_);
  textBox_->append(*bottomShift_);

  add_overlay(*textBox_);
  ApplyLabelFonts(kHeadlineMaxSize);
}

void LoginCarousel::ApplyLabelFonts(double fontPx) {
  SetLabelFont(*headline_, "ABC Gravity Extended", fontPx);
  SetLabelFont(*bottomLine_, "ABC Gravity Extra Condensed", fontPx);
}

void LoginCarousel::ApplyStrings() {
  bottomLine_->set_text(T_("with_urnetwork", "with URnetwork"));
  ShowSlide(index_);
}

// Fit the globe to the slot, and the type to the globe: the slot is elastic
// (the sign-in affordances get their height first), so nothing here can be a
// constant.
void LoginCarousel::ApplyMetrics() {
  const double slot = std::min(get_width(), get_height());
  if (slot <= 0) return;
  const double side = std::min(slot, kGlobeMaxSide);
  if (side == side_) return;
  side_ = side;
  const double font =
      std::clamp(side / kHeadlineSizeDivisor, kHeadlineMinSize, kHeadlineMaxSize);
  ApplyLabelFonts(font);
  const int maxW = static_cast<int>(side * kHeadlineWidthRatio);
  headline_->set_size_request(-1, -1);
  headline_->set_max_width_chars(1);  // let the pixel width below govern
  headline_->set_size_request(maxW, -1);
  bottomLine_->set_max_width_chars(1);
  bottomLine_->set_size_request(maxW, -1);
}

void LoginCarousel::StopAnimations() { ++animEpoch_; }

void LoginCarousel::ShowSlide(size_t index) {
  index_ = index % 3;
  headline_->set_text(HeadlineFor(index_));
  currentImage_->set_texture(TextureFor(index_));
  currentImage_->set_opacity(1);
  nextImage_->set_opacity(0);
  headlineShift_->set_translate_y(0);
  bottomShift_->set_translate_y(0);
  headline_->set_opacity(1);
  bottomLine_->set_opacity(1);
}

void LoginCarousel::HostReparented() {
  // boards first: a running animation would overwrite the clean pose below
  StopAnimations();
  ShowSlide(index_);
  side_ = 0;  // re-derive the metrics for the new slot
  ApplyMetrics();
}

void LoginCarousel::SetActive(bool active) {
  if (active_ == active) return;
  active_ = active;
  if (active) {
    timer_ = Glib::signal_timeout().connect(
        sigc::track_obj(
            [this]() -> bool {
              Advance();
              return true;
            },
            *this),
        kSlideIntervalMs);
  } else {
    timer_.disconnect();
    // land on a clean frame rather than freezing mid-transition
    StopAnimations();
    ShowSlide(index_);
  }
}

void LoginCarousel::Advance() {
  if (!active_) return;
  const size_t next = (index_ + 1) % 3;
  // reduced motion: the carousel still advances (it is a slideshow, not
  // decoration) but lands each slide rather than animating through it
  if (!motion::ShouldAnimate()) {
    ShowSlide(next);
    return;
  }
  AnimateTextOut();
  CrossfadeTo(next);
}

void LoginCarousel::AnimateTextOut() {
  const uint64_t epoch = ++animEpoch_;
  auto guard = [this, epoch](auto&& fn) {
    return [this, epoch, fn](double v) {
      if (animEpoch_ != epoch) return;
      fn(v);
    };
  };
  motion::AnimateValue(*this, 0, kTextOutMs, motion::kLinearP1, motion::kLinearP2,
                       guard([this](double t) {
                         headlineShift_->set_translate_y(-100.0 * t);
                         headline_->set_opacity(1.0 - t);
                       }));
  motion::AnimateValue(*this, 100, kTextOutMs, motion::kLinearP1, motion::kLinearP2,
                       guard([this](double t) {
                         bottomShift_->set_translate_y(-70.0 * t);
                         bottomLine_->set_opacity(1.0 - t);
                       }));
}

void LoginCarousel::CrossfadeTo(size_t index) {
  const uint64_t epoch = animEpoch_;  // shares the text-out epoch: one phase
  nextImage_->set_texture(TextureFor(index));
  motion::AnimateValue(
      *this, 0, kCrossfadeMs, motion::kLinearP1, motion::kLinearP2,
      [this, epoch](double t) {
        if (animEpoch_ != epoch) return;
        currentImage_->set_opacity(1.0 - t);
        nextImage_->set_opacity(t);
      },
      [this, epoch, index] {
        // completion fires only for a fade that ran under its own epoch —
        // a stop (SetActive(false), a reparent) suppresses it
        if (animEpoch_ != epoch || !active_) return;
        index_ = index % 3;
        currentImage_->set_texture(TextureFor(index_));
        currentImage_->set_opacity(1);
        nextImage_->set_opacity(0);
        headline_->set_text(HeadlineFor(index_));
        AnimateTextIn();
      });
}

void LoginCarousel::AnimateTextIn() {
  const uint64_t epoch = ++animEpoch_;
  // reset before animating: the From values are where the text starts
  headlineShift_->set_translate_y(100);
  headline_->set_opacity(0);
  bottomShift_->set_translate_y(40);
  bottomLine_->set_opacity(0);
  motion::AnimateValue(*this, 0, kTextInMs, motion::kLinearP1, motion::kLinearP2,
                       [this, epoch](double t) {
                         if (animEpoch_ != epoch) return;
                         headlineShift_->set_translate_y(100.0 * (1.0 - t));
                         headline_->set_opacity(t);
                       });
  motion::AnimateValue(*this, kBottomDelayMs, kTextInMs, motion::kLinearP1,
                       motion::kLinearP2, [this, epoch](double t) {
                         if (animEpoch_ != epoch) return;
                         bottomShift_->set_translate_y(40.0 * (1.0 - t));
                         bottomLine_->set_opacity(t);
                       });
}

}  // namespace urnw
