// SPDX-License-Identifier: MPL-2.0
#include "BrandIcons.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>

namespace urnw {
namespace {

// android res/drawable/bittensor_logo.xml, converted (24x24 box). Drawn in
// the button's content colour rather than upstream's white-on-white.
constexpr const char* kBittensorPath =
    "M6,7 L18,7 L18,9.2 L13.3,9.2 L13.3,13.9 C13.3,15.02 13.94,15.6 15.1,15.6 L15.7,15.6 "
    "L15.7,17 L14.7,17 C12.35,17 10.9,15.82 10.9,13.9 L10.9,9.2 L6,9.2 Z";

// The Solana mark (101x88 box) with its brand gradient — the mark may not be
// recoloured.
constexpr const char* kSolanaPath =
    "M100.48,69.38L83.81,86.8C83.44,87.18 83.01,87.48 82.52,87.69C82.03,87.89 81.51,88 80.97,"
    "88H1.94C1.56,88 1.19,87.89 0.87,87.69C0.56,87.49 0.31,87.2 0.16,86.87C0.01,86.53 -0.04,"
    "86.16 0.03,85.79C0.09,85.43 0.26,85.1 0.52,84.83L17.21,67.41C17.57,67.03 18,66.73 18.49,"
    "66.52C18.98,66.32 19.5,66.21 20.03,66.21H99.06C99.44,66.21 99.81,66.32 100.13,66.52C100."
    "44,66.72 100.69,67.01 100.84,67.34C100.99,67.68 101.04,68.05 100.97,68.42C100.91,68.78 "
    "100.74,69.11 100.48,69.38ZM83.81,34.3C83.44,33.92 83.01,33.62 82.52,33.42C82.03,33.21 "
    "81.51,33.1 80.97,33.1H1.94C1.56,33.1 1.19,33.21 0.87,33.41C0.56,33.62 0.31,33.9 0.16,34."
    "24C0.01,34.58 -0.04,34.95 0.03,35.31C0.09,35.67 0.26,36.01 0.52,36.28L17.21,53.7C17.57,"
    "54.07 18,54.38 18.49,54.58C18.98,54.79 19.5,54.89 20.03,54.9H99.06C99.44,54.9 99.81,54."
    "79 100.13,54.59C100.44,54.38 100.69,54.1 100.84,53.76C100.99,53.42 101.04,53.05 100.97,"
    "52.69C100.91,52.33 100.74,51.99 100.48,51.72L83.81,34.3ZM1.94,21.79H80.97C81.51,21.79 "
    "82.03,21.68 82.52,21.48C83.01,21.27 83.44,20.97 83.81,20.59L100.48,3.17C100.74,2.9 100."
    "91,2.57 100.97,2.21C101.04,1.84 100.99,1.47 100.84,1.13C100.69,0.8 100.44,0.51 100.13,0."
    "31C99.81,0.11 99.44,0 99.06,0L20.03,0C19.5,0 18.98,0.11 18.49,0.31C18,0.52 17.57,0.82 "
    "17.21,1.2L0.52,18.62C0.27,18.89 0.1,19.22 0.03,19.58C-0.03,19.95 0.01,20.32 0.16,20.65C0"
    ".31,20.99 0.56,21.28 0.88,21.48C1.19,21.68 1.56,21.79 1.94,21.79Z";

// The auth-code barcode glyph (32x32 box).
constexpr const char* kAuthCodePath =
    "M30.48,10.67H32v10.66h-1.52Z M28.95,21.33h1.53v1.53h-1.53Z M28.95,9.14h1.53v1.53h-1.53Z "
    "M3.05,22.86h25.9v1.52H3.05Z M21.34,16.76h7.61v1.52h-7.61Z M18.29,16.76h1.52v1.52h-1.52Z "
    "M3.05,7.62h25.9v1.52H3.05Z M1.52,9.14h1.53v1.53H1.52Z M1.52,21.33h1.53v1.53H1.52Z "
    "M0,10.67h1.52v10.66H0Z M6.1,12.19h1.52v7.62H6.1Z M9.14,12.19h1.53v7.62H9.14Z "
    "M12.19,12.19h1.52v7.62h-1.52Z M16.76,12.19h1.53v7.62h-1.53Z M21.34,12.19h1.52v7.62h-1.52Z "
    "M25.9,12.19h1.53v7.62H25.9Z";

struct Spec {
  const char* path;
  double viewW;
  double viewH;
};

Spec SpecFor(BrandIcon::Kind kind) {
  switch (kind) {
    case BrandIcon::Kind::Bittensor: return {kBittensorPath, 24, 24};
    case BrandIcon::Kind::Solana: return {kSolanaPath, 101, 88};
    default: return {kAuthCodePath, 32, 32};
  }
}

}  // namespace

BrandIcon::BrandIcon(Kind kind, int sizePx) : kind_(kind), size_(sizePx) {
  set_valign(Gtk::Align::CENTER);
}

void BrandIcon::measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural,
                              int& minimum_baseline, int& natural_baseline) const {
  minimum = natural = size_;
  minimum_baseline = natural_baseline = -1;
}

void BrandIcon::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
  const Spec spec = SpecFor(kind_);
  static GskPath* paths[3] = {nullptr, nullptr, nullptr};
  GskPath*& path = paths[static_cast<int>(kind_)];
  if (!path) path = gsk_path_parse(spec.path);
  if (!path) return;

  GtkSnapshot* snap = snapshot->gobj();
  const double w = get_width();
  const double h = get_height();
  const double scale = std::min(w / spec.viewW, h / spec.viewH);
  gtk_snapshot_save(snap);
  const graphene_point_t origin{static_cast<float>((w - spec.viewW * scale) / 2.0),
                                static_cast<float>((h - spec.viewH * scale) / 2.0)};
  gtk_snapshot_translate(snap, &origin);
  gtk_snapshot_scale(snap, static_cast<float>(scale), static_cast<float>(scale));
  gtk_snapshot_push_fill(snap, path, GSK_FILL_RULE_WINDING);
  const graphene_rect_t bounds{{0, 0},
                               {static_cast<float>(spec.viewW), static_cast<float>(spec.viewH)}};
  if (kind_ == Kind::Solana) {
    const GskColorStop stops[] = {
        {0.08f, {0x99 / 255.f, 0x45 / 255.f, 0xFF / 255.f, 1.f}},
        {0.30f, {0x87 / 255.f, 0x52 / 255.f, 0xF3 / 255.f, 1.f}},
        {0.50f, {0x54 / 255.f, 0x97 / 255.f, 0xD5 / 255.f, 1.f}},
        {0.60f, {0x43 / 255.f, 0xB4 / 255.f, 0xCA / 255.f, 1.f}},
        {0.72f, {0x28 / 255.f, 0xE0 / 255.f, 0xB9 / 255.f, 1.f}},
        {0.97f, {0x19 / 255.f, 0xFB / 255.f, 0x9B / 255.f, 1.f}},
    };
    const graphene_point_t gradStart{8.53f, 90.1f};
    const graphene_point_t gradEnd{88.99f, -3.02f};
    gtk_snapshot_append_linear_gradient(snap, &bounds, &gradStart, &gradEnd, stops,
                                        G_N_ELEMENTS(stops));
  } else {
    // the button's content colour (black on the white SECONDARY pill)
    const GdkRGBA black{0.f, 0.f, 0.f, 1.f};
    gtk_snapshot_append_color(snap, &black, &bounds);
  }
  gtk_snapshot_pop(snap);
  gtk_snapshot_restore(snap);
}

// ---- NavIcon ---------------------------------------------------------------
// Every icon is drawn in a 24x24 design box and scaled to the requested size.
// Stroke-based outlines at 1.7/24 (~1.13px at 16) — the weight Segoe Fluent's
// outline set reads at, and close to Adwaita's own symbolic stroke.

NavIcon::NavIcon(Kind kind, int sizePx) : kind_(kind), size_(sizePx) {
  set_valign(Gtk::Align::CENTER);
  set_halign(Gtk::Align::CENTER);
}

void NavIcon::measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural,
                            int& minimum_baseline, int& natural_baseline) const {
  minimum = natural = size_;
  minimum_baseline = natural_baseline = -1;
}

void NavIcon::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
  const double w = get_width();
  const double h = get_height();
  if (w <= 0 || h <= 0) return;
  const graphene_rect_t bounds{{0, 0}, {static_cast<float>(w), static_cast<float>(h)}};
  cairo_t* raw = gtk_snapshot_append_cairo(snapshot->gobj(), &bounds);
  auto cr = Cairo::RefPtr<Cairo::Context>(new Cairo::Context(raw, /*has_reference=*/false));

  // inherit the nav item's CSS color, so muted -> selected white is free
  GdkRGBA color;
  gtk_widget_get_color(GTK_WIDGET(gobj()), &color);
  cr->set_source_rgba(color.red, color.green, color.blue, color.alpha);

  const double scale = std::min(w, h) / 24.0;
  cr->translate((w - 24 * scale) / 2.0, (h - 24 * scale) / 2.0);
  cr->scale(scale, scale);
  cr->set_line_width(1.7);
  cr->set_line_cap(Cairo::Context::LineCap::ROUND);
  cr->set_line_join(Cairo::Context::LineJoin::ROUND);

  switch (kind_) {
    case Kind::Home:  // Segoe E80F: a house outline
      cr->move_to(3.6, 11.2);
      cr->line_to(12, 4.2);
      cr->line_to(20.4, 11.2);
      cr->stroke();
      cr->move_to(6.2, 10.6);
      cr->line_to(6.2, 19.8);
      cr->line_to(17.8, 19.8);
      cr->line_to(17.8, 10.6);
      cr->stroke();
      break;
    case Kind::Globe:  // Segoe E774: sphere with equator + meridian
      cr->arc(12, 12, 8.4, 0, 2 * G_PI);
      cr->stroke();
      cr->move_to(3.6, 12);
      cr->line_to(20.4, 12);
      cr->stroke();
      cr->save();  // the meridian is a squashed circle
      cr->translate(12, 12);
      cr->scale(0.42, 1.0);
      cr->arc(0, 0, 8.4, 0, 2 * G_PI);
      cr->restore();
      cr->stroke();
      break;
    case Kind::Wallet: {  // Segoe E8C7: a wallet body with a clasp
      const double x = 3.4, y = 6.2, ww = 17.2, hh = 12.4, r = 2.4;
      cr->begin_new_sub_path();
      cr->arc(x + ww - r, y + r, r, -G_PI / 2, 0);
      cr->arc(x + ww - r, y + hh - r, r, 0, G_PI / 2);
      cr->arc(x + r, y + hh - r, r, G_PI / 2, G_PI);
      cr->arc(x + r, y + r, r, G_PI, 1.5 * G_PI);
      cr->close_path();
      cr->stroke();
      cr->move_to(15.2, 8.6);
      cr->line_to(15.2, 16.2);
      cr->stroke();
      cr->arc(18.0, 12.4, 1.45, 0, 2 * G_PI);  // the clasp, big enough to survive 16/24
      cr->fill();
      break;
    }
    case Kind::Person:  // Segoe E77B: head + shoulders
      cr->arc(12, 8.4, 3.7, 0, 2 * G_PI);
      cr->stroke();
      cr->move_to(5.4, 19.8);
      cr->curve_to(5.4, 15.9, 8.4, 13.6, 12, 13.6);
      cr->curve_to(15.6, 13.6, 18.6, 15.9, 18.6, 19.8);
      cr->stroke();
      break;
    case Kind::Help:  // Segoe E897: a bare question mark
      cr->move_to(8.5, 9.0);
      cr->curve_to(8.5, 6.4, 10.2, 4.9, 12.2, 4.9);
      cr->curve_to(14.4, 4.9, 15.9, 6.4, 15.9, 8.4);
      cr->curve_to(15.9, 11.2, 12.6, 11.6, 12.2, 13.9);
      cr->line_to(12.2, 15.2);
      cr->stroke();
      cr->arc(12.2, 18.4, 1.15, 0, 2 * G_PI);
      cr->fill();
      break;
    case Kind::DevTools:  // Segoe EBE8: a gear between brackets
      cr->move_to(8.0, 5.6);
      cr->line_to(4.6, 12.0);
      cr->line_to(8.0, 18.4);
      cr->stroke();
      cr->move_to(16.0, 5.6);
      cr->line_to(19.4, 12.0);
      cr->line_to(16.0, 18.4);
      cr->stroke();
      cr->arc(12, 12, 2.0, 0, 2 * G_PI);
      cr->stroke();
      break;
    case Kind::Gear: {  // Segoe E713: an 8-tooth gear
      // A LARGE ring with SHORT teeth. (A small hub with long spokes renders
      // as a sunburst at 16px — measured on the rail.)
      cr->arc(12, 12, 5.5, 0, 2 * G_PI);
      cr->stroke();
      cr->set_line_width(2.2);
      for (int i = 0; i < 8; ++i) {
        const double a = i * G_PI / 4.0;
        cr->move_to(12 + std::cos(a) * 6.4, 12 + std::sin(a) * 6.4);
        cr->line_to(12 + std::cos(a) * 8.4, 12 + std::sin(a) * 8.4);
      }
      cr->stroke();
      cr->set_line_width(1.7);
      break;
    }
  }
  cairo_destroy(raw);
}

}  // namespace urnw
