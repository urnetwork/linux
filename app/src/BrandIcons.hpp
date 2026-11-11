// Inline vector brand marks for the sign-in buttons — the same path data the
// Windows markup carries (MainWindow.xaml), drawn with GskPath. The Solana
// mark keeps its brand gradient; Bittensor and the auth-code barcode draw in
// the button's content colour (black on the white SECONDARY pill). Also the
// crypto payment row's coin marks (CoinMark, GskPath from the installed svgs),
// the navigation rail's icons (NavIcon) and the Material Design Icons glyphs
// (GlyphIcon, GlyphDot), both drawn with Cairo.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include <gtkmm.h>

#include "MdiGlyphs.hpp"

typedef struct _GskPath GskPath;

namespace urnw {

// A coin's mark in the crypto payment row (mmm/BTCPAY.md §17.6): the installed
// coins/<icon key>.svg the SDK's icon key names, its one path filled nonzero
// with GSK_FILL_RULE_WINDING, at full contrast (white on this dark app) and
// never dimmed, whatever the coin's state or the row's: Solana's and
// Bittensor's brand rules allow their marks only in black or white. Decorative
// (hidden from the accessibility tree): the label beside it names the coin. A
// key without a file draws nothing (Drawn() false).
class CoinMark : public Gtk::Widget {
 public:
  CoinMark(const std::string& iconKey, int sizePx);
  bool Drawn() const;

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  GskPath* path_ = nullptr;  // the per-key cache's, never freed
  int size_;
};

class BrandIcon : public Gtk::Widget {
 public:
  // Key / Apple / Google joined for the login stack's tiles and pills
  // (LOGIN_STACK_SPEC): the key and the Apple mark draw in the content
  // colour; the Google "G" keeps its four brand colours like the Solana mark.
  enum class Kind { Bittensor, Solana, AuthCode, Key, Apple, Google };
  static constexpr int kKindCount = 6;

  explicit BrandIcon(Kind kind, int sizePx = 18);

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  Kind kind_;
  int size_;
};

// The navigation icon set — drawn, not themed.
//
// The Windows rail uses Segoe Fluent Icons (Home E80F, Globe E774, Wallet
// E8C7, Contact E77B, Help E897, DeveloperTools EBE8, Settings E713). Two of
// those shapes (globe, wallet) do not exist in Adwaita at all, and a missing
// themed icon renders as a BLANK — the same silent failure that left the
// title-bar logo empty outside an installed tree. Drawing them keeps the set
// consistent in weight, identical on every host, and correct inside the
// AppImage regardless of which icon theme got bundled.
//
// Icons are stroked in the widget's CSS color, so they inherit the nav item's
// muted/selected/hover states for free.
class NavIcon : public Gtk::Widget {
 public:
  enum class Kind { Home, Globe, Wallet, Person, Help, DevTools, Gear };

  explicit NavIcon(Kind kind, int sizePx = 16);

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  Kind kind_;
  int size_;
};

// ---- the Material Design Icons glyphs (MdiGlyphs.hpp) ----------------------
// Drawn, not themed, for NavIcon's reason: each glyph is its MDI path data,
// read by GlyphPath.hpp and filled with Cairo, identical on every host and
// never blank where an icon theme lacks a name. Both widgets are decorative
// (hidden from the accessibility tree): the row or button they sit in names
// what they show.

// Fills `glyph` in the size x size box at (x, y), in the source colour the
// caller set. Draws nothing for a glyph whose path does not read
// (tests/GlyphPathTest.cpp reads every one).
void FillGlyph(const Cairo::RefPtr<Cairo::Context>& cr, MdiGlyph glyph, double x, double y,
               double size);

// One glyph in the widget's CSS colour: the Account pane's row icons take the
// muted row tint (.ur-row-icon) and dim with a disabled row; a command's glyph
// takes its button's colour.
class GlyphIcon : public Gtk::Widget {
 public:
  explicit GlyphIcon(MdiGlyph glyph, int sizePx = 16);
  void SetGlyph(MdiGlyph glyph);

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  MdiGlyph glyph_;
  int size_;
};

// A filled circle in one colour with a glyph drawn white inside it at half the
// circle's size: the Sessions rows' leading visual, the session's country
// colour around its device logo (REVOKE-UI-FINAL.md §3).
class GlyphDot : public Gtk::Widget {
 public:
  GlyphDot(MdiGlyph glyph, int diameterPx);
  void SetGlyph(MdiGlyph glyph);
  void SetColor(const GdkRGBA& color);

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;

 private:
  MdiGlyph glyph_;
  int diameter_;
  GdkRGBA color_{0.f, 0.6f, 1.f, 1.f};
};

}  // namespace urnw
