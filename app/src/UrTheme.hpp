// The URnetwork brand theme for the GTK GUI — the Linux twin of the Windows
// app's App.xaml resources + UrColors.h (which mirror android ui/theme and
// elements:src/index.css). Two responsibilities:
//
//   * LoadBrandFonts(): register the four licensed brand faces with
//     fontconfig at runtime, resolved through the RuntimePaths ladder so the
//     same binary finds them installed, inside an AppImage, or straight out
//     of the build tree. MUST run before the first widget is created — the
//     Pango font map snapshots fontconfig's config when it is first used.
//     The faces are referenced by their INTERNAL family names ("PP NeueBit"
//     has no space in NeueBit; a wrong name fails silently to the fallback
//     face — verified with fc-scan against the shipped files).
//   * EnsureBrandCss(): install the one app-wide Gtk::CssProvider carrying
//     the Windows style vocabulary — URButton PRIMARY/SECONDARY, the
//     underlined URTextInput, the card + sheet surfaces, the type ramp and
//     the wordmark/heading classes. Class names are `.ur-*`, matching the
//     Ur*Style resource names on Windows so the two style tables can be
//     diffed side by side.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gdkmm/texture.h>
#include <glibmm/refptr.h>

namespace urnw {

// Register the brand fonts with the default fontconfig config. Idempotent.
// Call from main() BEFORE constructing any window. Missing files are logged
// and skipped — the UI then renders in the fallback face (readable, wrong),
// exactly the Windows failure mode.
void LoadBrandFonts();

// Install the brand CSS provider once per display. Safe to call repeatedly.
void EnsureBrandCss();

// Register the app's hicolor icon directory with the default icon theme so
// the icon NAME "urnetwork" resolves (window icon, tray, .desktop matching)
// even when the app runs from a build tree or a relocated AppImage. Without
// this, set_from_icon_name("urnetwork") silently renders a BLANK image —
// which is exactly how the title-bar logo came out empty. Idempotent.
void RegisterBrandIcons();

// The brand logo as a texture, resolved through the RuntimePaths ladder
// ($APPDIR, then the install prefix, then the build tree). Null only if the
// asset is genuinely missing — callers should fall back to the icon name.
Glib::RefPtr<Gdk::Texture> BrandLogoTexture();

}  // namespace urnw
