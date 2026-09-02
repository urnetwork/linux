// SPDX-License-Identifier: MPL-2.0
#include "UrTheme.hpp"

#include <fontconfig/fontconfig.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <string>

#include "RuntimePaths.hpp"

namespace urnw {
namespace {

// The brand palette + component styles (windows:App.xaml Ur* resources /
// UrColors.h; one dark theme, no light theme). Values are normative — see
// docs/linux_agent_help.md §8.1. Keep literal hex here in sync with Ui.hpp's
// Rgba constants (the cairo-drawing side of the same palette).
constexpr const char* kBrandCss = R"(
/* ---- surfaces --------------------------------------------------------- */
window.background { background-color: #101010; color: #F8F8F8; }
headerbar {
  background-color: #101010; background-image: none; color: #F8F8F8;
  min-height: 48px; box-shadow: none; border: none; padding: 0 8px;
}
headerbar windowhandle > box { padding: 0; }
.ur-sheet { background-color: #151515; }

/* ---- the wordmark + brand type ---------------------------------------- */
.ur-wordmark { font-family: "PP NeueBit"; font-size: 24px; font-weight: bold; color: #F8F8F8; }
/* display face: page titles / step headings (windows UrHeadingFontFamily) */
.ur-heading { font-family: "ABC Gravity Extended"; color: #F8F8F8; }
.ur-step-heading { font-family: "ABC Gravity Extended"; font-size: 20px; font-weight: 600; color: #F8F8F8; }
.ur-heading-condensed { font-family: "ABC Gravity Extra Condensed"; color: #F8F8F8; }
/* body face for everything (windows UrBodyFontFamily) */
.ur-body { font-family: "PP Neue Montreal"; font-size: 14px; color: #F8F8F8; }
.ur-body-large { font-family: "PP Neue Montreal"; font-size: 18px; color: #F8F8F8; }
.ur-caption { font-family: "PP Neue Montreal"; font-size: 12px; color: #989898; }
.ur-label-faint { color: #5A5A5A; }
.ur-error-text { font-family: "PP Neue Montreal"; font-size: 12px; color: #FF6C58; }
.ur-danger-text { color: #F8523B; }

/* ---- URButton (windows UrButtonBaseStyle / android URButton.kt) --------
   PRIMARY   container BlueMedium #638BFC, content white
   SECONDARY container white,             content black
   radius 12, min height 48, full width, label PP NeueBit Bold 24.
   Hover/press = a translucent layer of the CONTENT colour over the container
   (8% / 12%), approximated with mix(); disabled = whole-button opacity .38. */
button.ur-btn {
  border-radius: 12px; min-height: 48px; padding: 8px 16px;
  font-family: "PP NeueBit"; font-size: 24px; font-weight: bold;
  border: none; box-shadow: none; background-image: none;
  transition: background-color 150ms ease;
}
button.ur-btn label { font-family: "PP NeueBit"; font-size: 24px; font-weight: bold; }
button.ur-btn-primary { background-color: #638BFC; color: #ffffff; }
button.ur-btn-primary:hover { background-color: mix(#638BFC, #ffffff, 0.08); }
button.ur-btn-primary:active { background-color: mix(#638BFC, #ffffff, 0.12); }
button.ur-btn-primary:disabled { background-color: #638BFC; color: #ffffff; opacity: 0.38; }
button.ur-btn-secondary { background-color: #ffffff; color: #000000; }
button.ur-btn-secondary:hover { background-color: mix(#ffffff, #000000, 0.08); }
button.ur-btn-secondary:active { background-color: mix(#ffffff, #000000, 0.12); }
button.ur-btn-secondary:disabled { background-color: #ffffff; color: #000000; opacity: 0.38; }
button.ur-btn-secondary image { color: #000000; }

/* ---- URTextInput (android URTextInput.kt / windows TextControl* keys) --
   Not a filled box: a transparent field over a 1px underline that is
   TextFaint at rest and BlueMedium while focused; text renders #D3D3D3. */
.ur-input-label { font-family: "PP Neue Montreal"; font-size: 12px; color: #989898; margin-bottom: 8px; }
entry.ur-input, entry.ur-input:focus {
  background: none; background-color: transparent;
  border: none; border-bottom: 1px solid #5A5A5A; border-radius: 0;
  box-shadow: none; outline: none;
  padding: 0 0 8px 0; min-height: 0;
  font-family: "PP Neue Montreal"; font-size: 16px; color: #D3D3D3;
  caret-color: #D3D3D3;
}
entry.ur-input:focus-within { border-bottom: 1px solid #638BFC; }
entry.ur-input:disabled { border-bottom-color: transparent; }
entry.ur-input text { background: none; }
entry.ur-input text placeholder { color: #5A5A5A; }
/* the multi-line variant (the seedphrase box) wears the same underline */
textview.ur-input-multi {
  background: none; background-color: transparent;
  border-bottom: 1px solid #5A5A5A;
  font-family: monospace; font-size: 14px; color: #D3D3D3;
  caret-color: #D3D3D3; padding: 4px 0 8px 0;
}
textview.ur-input-multi:focus-within { border-bottom: 1px solid #638BFC; }
textview.ur-input-multi:disabled { border-bottom-color: transparent; }
textview.ur-input-multi > text { background: none; color: #D3D3D3; }

/* ---- UrCard (windows UrCardStyle): tinted, hairline edge, radius 12 ---- */
.ur-card-bordered {
  background-color: #1C1C1C;
  border: 1px solid alpha(#ffffff, 0.12);
  border-radius: 12px; padding: 16px;
}

/* the quiet text affordance (Change Network API): a real button rendering
   as muted 12px text — focusable, unlike a bare label */
button.ur-quiet-link {
  background: none; border: none; box-shadow: none; padding: 0; min-height: 0;
  font-family: "PP Neue Montreal"; font-size: 12px; color: #989898;
}
button.ur-quiet-link:hover { color: #F8F8F8; }

/* Real hyperlinks (markdown spans parsed into <a href>, GtkLinkButton) carry
   the brand ACTION BLUE — and no visited state. Left to the platform they
   render Adwaita blue and then turn PURPLE once followed, which is a colour
   this product does not own. Windows' Hyperlink runs are BlueMedium always. */
:link, :visited { color: #638BFC; }
:link:hover, :visited:hover { color: #8AA9FF; }
button.link, button.link label { color: #638BFC; }
button.link:hover label { color: #8AA9FF; }

/* the carousel's headline rides over the globe in pure white (the only
   pure-white text in the app; body text is off-white) */
.ur-carousel-headline { color: #ffffff; font-weight: 600; }

/* ================= THE PANE SHELL (windows App.xaml R3/R4) =================
   A destination is 2-3 full-bleed vertical panes separated by 1px rules,
   never gaps. Every pane opens with a 40px header strip and scrolls
   independently; content is 28px group headers over uniform fixed-height
   rows with bottom hairlines and a 12px inset. No radius, no margin, no
   shadow inside a pane — a row's separation is its hairline and its hover
   fill. Fills are the existing #101010/#151515/#1C1C1C ramp. */
.ur-pane { background-color: #101010; }
.ur-pane-header {
  background-color: #151515;
  border-bottom: 1px solid alpha(#ffffff, .12);
  min-height: 40px; padding: 0 12px;
}
.ur-pane-title {
  font-family: "PP Neue Montreal"; font-size: 12px; font-weight: 600;
  letter-spacing: 0.7px; color: #F8F8F8;
}
.ur-pane-meta { font-family: "PP Neue Montreal"; font-size: 11px; color: #989898; }
.ur-group-header {
  background-color: #151515;
  border-top: 1px solid alpha(#ffffff, .12);
  border-bottom: 1px solid alpha(#ffffff, .12);
  min-height: 28px; padding: 0 12px;
}
.ur-group-title {
  font-family: "PP Neue Montreal"; font-size: 11px;
  letter-spacing: 1px; color: #989898;
}
/* the two halves of a key/value row; the row label of a list pane */
.ur-key { font-family: "PP Neue Montreal"; font-size: 13px; color: #989898; }
.ur-value { font-family: "PP Neue Montreal"; font-size: 13px; color: #F8F8F8; }
.ur-row-title { font-family: "PP Neue Montreal"; font-size: 13px; color: #F8F8F8; }
.ur-row-note { font-family: "PP Neue Montreal"; font-size: 11px; color: #989898; }
/* TONE ON TOP OF A ROW CLASS. This provider outranks the one Ui.cpp installs
   (APPLICATION + 1), so a bare `.dim-label` added beside `.ur-value` LOSES and
   the label renders full white — every muted meta/value the kit builds was
   coming out #F8F8F8. Two-class selectors settle it inside one provider,
   independent of declaration order. */
.ur-value.dim-label,
.ur-row-title.dim-label { color: #989898; }
.ur-value.ur-label-faint,
.ur-row-title.ur-label-faint { color: #5A5A5A; }
/* ...and a STATE beats a tone. A lime figure (money that arrived, the payout
   wallet's total) or a danger figure carries a third class, so it outranks
   the muted default it is layered on. */
.ur-value.ur-value-on, .ur-value.dim-label.ur-value-on,
.ur-row-title.ur-value-on, .ur-row-title.dim-label.ur-value-on { color: #87FB67; }
.ur-value.ur-danger-text, .ur-value.dim-label.ur-danger-text,
.ur-row-title.ur-danger-text, .ur-row-title.dim-label.ur-danger-text { color: #F8523B; }
/* the signed-in user's own row in the earnings leaderboard: card fill against
   the pane, never an accent — the rank is the emphasis, not the row */
.ur-earn-own-row { background-color: #1C1C1C; }
/* the status chip on a history row (unclaimed / claimed / expired) */
.ur-earn-tag {
  background-color: alpha(#F8F8F8, .04); border-radius: 6px; padding: 2px 6px;
  font-family: "PP NeueBit"; font-size: 16px; font-weight: bold; color: #989898;
}
/* the two gold tiles on the earnings page (unclaimed SN25α, Top 200): the
   referral gold (Ui.hpp kReferralGold) on a faint fill, never the Pro gold */
.ur-earn-gold-tile {
  background-color: alpha(#F5B93C, .10); border: 1px solid alpha(#F5B93C, .45);
  border-radius: 10px; padding: 12px;
}
.ur-earn-gold-text { color: #FFD76A; }
button.ur-earn-gold-button {
  background-color: #F5B93C; color: #241A05; border-radius: 8px; font-weight: 600;
  padding: 6px 14px; min-height: 32px;
}
button.ur-earn-gold-button:hover { background-color: #FFD76A; }
button.ur-earn-gold-button:active { background-color: #E0A52E; }
button.ur-earn-gold-button:disabled { opacity: 0.38; }
/* the wallet address in the bit face */
.ur-earn-address { font-family: "PP NeueBit"; font-size: 18px; font-weight: bold; color: #F8F8F8; }
.ur-col-header {
  font-family: "PP Neue Montreal"; font-size: 11px;
  letter-spacing: 0.7px; color: #5A5A5A;
}
/* a row that opens something: same metrics as a static row, hover = a fill
   step (never an outline), pressed one step further (windows
   UrPaneRowButtonStyle) */
button.ur-pane-row {
  background: none; background-image: none; border: none; box-shadow: none;
  border-bottom: 1px solid alpha(#ffffff, .12); border-radius: 0;
  padding: 0 12px; min-height: 40px;
  font-family: "PP Neue Montreal"; font-size: 13px; color: #F8F8F8;
  transition: background-color 150ms ease;
}
button.ur-pane-row:hover { background-color: #1C1C1C; }
button.ur-pane-row:active { background-color: #2A2A2A; }
button.ur-pane-row:disabled { opacity: 0.38; }
button.ur-pane-row.selected { background-color: #1C1C1C; }
/* the icon-only command riding in a pane/group header: 28x24, no chrome
   until hovered; every instance must carry an accessible name */
button.ur-pane-action {
  background: none; border: none; box-shadow: none; border-radius: 4px;
  min-height: 24px; min-width: 28px; padding: 0;
}
button.ur-pane-action:hover { background-color: #1C1C1C; }
/* THE PANE'S PRIMARY ACTION (windows UrPaneActionPrimaryStyle): blue fill,
   #101010 content (contrast 5.9:1 at 14sp — white fails AA there), radius 4,
   height 40, 12px inset all round, body face SemiBold. The outlined twin is
   the connected state's Disconnect: quiet = you are fine. */
button.ur-pane-primary {
  background-color: #638BFC; color: #101010;
  border: none; border-radius: 4px; box-shadow: none; background-image: none;
  min-height: 40px; margin: 12px; padding: 0 12px;
  font-family: "PP Neue Montreal"; font-size: 14px; font-weight: 600;
  transition: background-color 150ms ease;
}
button.ur-pane-primary:hover { background-color: mix(#638BFC, #101010, 0.08); }
button.ur-pane-primary:active { background-color: mix(#638BFC, #101010, 0.12); }
button.ur-pane-primary:disabled { opacity: 0.38; }
button.ur-pane-secondary {
  background-color: transparent; color: #F8F8F8;
  border: 1px solid alpha(#ffffff, .22); border-radius: 4px; box-shadow: none;
  min-height: 40px; margin: 12px; padding: 0 12px;
  font-family: "PP Neue Montreal"; font-size: 14px; font-weight: 600;
  transition: background-color 150ms ease;
}
button.ur-pane-secondary:hover { background-color: alpha(#ffffff, .04); }
button.ur-pane-secondary:active { background-color: alpha(#ffffff, .08); }
/* the vertical rule BETWEEN two panes; the horizontal one between rows */
.ur-vrule { background-color: alpha(#ffffff, .12); }
/* the search field at the top of a list pane: squared off, transparent, on
   the pane row metrics (windows UrPaneSearchStyle) */
entry.ur-pane-search, entry.ur-pane-search:focus {
  background: none; background-color: transparent; border: none;
  border-radius: 0; box-shadow: none; outline: none; padding: 0; min-height: 0;
  font-family: "PP Neue Montreal"; font-size: 13px; color: #F8F8F8;
  caret-color: #D3D3D3;
}

/* ---- the persistent status strip (windows UrStatusStrip*) ------------- */
.ur-status-strip {
  background-color: #151515;
  border-top: 1px solid alpha(#ffffff, .12);
  padding: 7px 16px;
}
.ur-status-caption { font-family: "PP Neue Montreal"; font-size: 11px; color: #5A5A5A; }
.ur-status-value { font-family: "PP Neue Montreal"; font-size: 12px; color: #989898; }

/* ---- the stat tile (windows UrStatLabel/UrStatValue) ------------------- */
.ur-stat-label { font-family: "PP Neue Montreal"; font-size: 12px; color: #989898; }
.ur-stat-value {
  font-family: "ABC Gravity Extra Condensed"; font-size: 26px; color: #F8F8F8;
}

/* ---- the left navigation rail (windows NavigationView skin) ------------
   #151515 pane with a hairline edge against the page; items 44px; the
   selected destination wears a card-fill pill and a 3px #EFF7BB accent bar
   (painted by the item's leading marker box, not by CSS alone). */
.ur-nav { background-color: #151515; border-right: 1px solid alpha(#ffffff, .12); }
button.ur-nav-item {
  background: none; background-image: none; border: none; box-shadow: none;
  border-radius: 8px; min-height: 44px; padding: 0 10px; margin: 1px 8px;
  font-family: "PP Neue Montreal"; font-size: 14px; color: #989898;
  transition: background-color 150ms ease, color 150ms ease;
}
button.ur-nav-item:hover { background-color: #1C1C1C; color: #F8F8F8; }
button.ur-nav-item:active { background-color: #242424; }
button.ur-nav-item.selected { background-color: #242424; color: #F8F8F8; }
button.ur-nav-item.selected:hover { background-color: #2A2A2A; }
button.ur-nav-item image { color: inherit; }
.ur-nav-accent { background-color: #EFF7BB; border-radius: 2px; }

/* ---- the window-level mode notice + snackbar surfaces ------------------ */
.ur-mode-notice {
  background-color: #1C1C1C; border-bottom: 1px solid alpha(#ffffff, .12);
  padding: 8px 16px; font-family: "PP Neue Montreal"; font-size: 12px; color: #989898;
}
.ur-snackbar {
  background-color: #1C1C1C; border: 1px solid alpha(#ffffff, .12);
  border-radius: 8px; padding: 10px 16px;
  font-family: "PP Neue Montreal"; font-size: 13px; color: #F8F8F8;
}
.ur-snackbar-error { border-color: alpha(#FF6C58, .5); }
.ur-snackbar-success { border-color: alpha(#87FB67, .4); }
/* the referral gold toast (Ui.hpp kReferralGold) */
.ur-snackbar-gold { border-color: alpha(#F5B93C, .6); }
)";

void AddFontFile(const std::string& dir, const char* file) {
  const std::string path = dir + "/" + file;
  if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
    g_warning("brand font missing: %s (text will render in a fallback face)", path.c_str());
    return;
  }
  if (!FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8*>(path.c_str()))) {
    g_warning("fontconfig refused brand font: %s", path.c_str());
  }
}

}  // namespace

void LoadBrandFonts() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
#ifdef UR_PKGDATADIR
  const std::string installed = std::string(UR_PKGDATADIR) + "/fonts";
#else
  const std::string installed = "/usr/share/urnetwork/fonts";
#endif
  std::string dir = ResolveRuntimePath(installed, G_FILE_TEST_IS_DIR, "assets/fonts");
  if (dir.empty()) {
    g_warning("brand font directory not found (looked under %s) -- brand faces unavailable",
              installed.c_str());
    return;
  }
  AddFontFile(dir, "abcgravity_extended.otf");
  AddFontFile(dir, "abcgravity_extra_condensed.otf");
  AddFontFile(dir, "pp_neue_bit_bold.ttf");
  AddFontFile(dir, "pp_neue_montreal_regular.ttf");

  // Self-verify: a wrong family name (or a registration the font map cannot
  // see) fails SILENTLY to the fallback face — the windows failure mode this
  // message exists to catch in a log instead of a screenshot diff.
  PangoFontMap* map = pango_cairo_font_map_get_default();
  PangoContext* ctx = pango_font_map_create_context(map);
  PangoFontDescription* want = pango_font_description_from_string("PP NeueBit Bold 24");
  PangoFont* font = pango_font_map_load_font(map, ctx, want);
  char* got = font ? pango_font_description_to_string(pango_font_describe(font)) : nullptr;
  g_message("brand fonts: dir=%s, PP NeueBit resolves to '%s'", dir.c_str(),
            got ? got : "(nothing)");
  g_free(got);
  pango_font_description_free(want);
  if (font) g_object_unref(font);
  g_object_unref(ctx);
}

namespace {

// the hicolor tree the packaging installs (meson: datadir/icons/hicolor/...)
std::string BrandIconDir() {
#ifdef UR_PKGDATADIR
  // UR_PKGDATADIR is <prefix>/share/urnetwork; the icons live beside it
  const std::string installed = std::string(UR_PKGDATADIR) + "/../icons";
#else
  const std::string installed = "/usr/share/icons";
#endif
  return ResolveRuntimePath(installed, G_FILE_TEST_IS_DIR, "packaging/icons");
}

}  // namespace

void RegisterBrandIcons() {
  static bool registered = false;
  if (registered) return;
  const std::string dir = BrandIconDir();
  if (dir.empty()) {
    g_warning("brand icons: hicolor dir not found -- the app icon will be blank");
    return;
  }
  registered = true;
  if (auto* display = gdk_display_get_default()) {
    GtkIconTheme* theme = gtk_icon_theme_get_for_display(display);
    gtk_icon_theme_add_search_path(theme, dir.c_str());
  }
}

Glib::RefPtr<Gdk::Texture> BrandLogoTexture() {
  static Glib::RefPtr<Gdk::Texture> cached;
  static bool tried = false;
  if (tried) return cached;
  tried = true;
  const std::string dir = BrandIconDir();
  if (dir.empty()) return cached;
  for (const char* size : {"256x256", "48x48"}) {
    const std::string path =
        dir + "/hicolor/" + size + "/apps/" + kAppIconName + ".png";
    if (!g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) continue;
    try {
      cached = Gdk::Texture::create_from_filename(path);
      return cached;
    } catch (const Glib::Error& e) {
      g_warning("brand logo: %s failed to load: %s", path.c_str(), e.what());
    }
  }
  g_warning("brand logo: no %s.png under %s", kAppIconName, dir.c_str());
  return cached;
}

void EnsureBrandCss() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kBrandCss);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  g_object_unref(provider);
}

}  // namespace urnw
