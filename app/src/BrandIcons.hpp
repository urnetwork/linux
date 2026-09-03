// Inline vector brand marks for the sign-in buttons — the same path data the
// Windows markup carries (MainWindow.xaml), drawn with GskPath. The Solana
// mark keeps its brand gradient; Bittensor and the auth-code barcode draw in
// the button's content colour (black on the white SECONDARY pill).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtkmm.h>

namespace urnw {

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

}  // namespace urnw
