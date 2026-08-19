// Shared UI helpers for the connect drawer surfaces: the brand visual system
// (the mac app's dark theme palette, centralized here), a one-time CSS
// bootstrap for the card/chip/button styles, small widget factories used by
// the drawer cards and detail sheets, and the entrance animation helper.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <gtkmm.h>

namespace urnw {

// Post a callback onto the GTK main loop from any thread (SDK callbacks fire
// on background threads; every widget touch must marshal through here).
void PostToMain(std::function<void()> fn);

// Plain sRGB color for cairo drawing (the widget styles use CSS instead —
// both pull from this one palette; keep them in sync).
struct Rgba {
  double r = 0;
  double g = 0;
  double b = 0;
  double a = 1;
  Rgba WithAlpha(double alpha) const { return Rgba{r, g, b, alpha}; }
};

// Brand palette (apple app dark theme parity — exact values, do not tweak).
// Series/semantic colors:
inline constexpr Rgba kUrGreen{0x87 / 255.0, 0xFB / 255.0, 0x67 / 255.0, 1.0};      // #87FB67
inline constexpr Rgba kUrPink{0xED / 255.0, 0x8F / 255.0, 0xFF / 255.0, 1.0};       // #ED8FFF
inline constexpr Rgba kUrCoral{0xFF / 255.0, 0x6C / 255.0, 0x58 / 255.0, 1.0};      // #FF6C58
inline constexpr Rgba kUrMutedCoral{0xC8 / 255.0, 0x60 / 255.0, 0x4F / 255.0, 1.0};  // #C8604F
inline constexpr Rgba kUrAmber{0xF5 / 255.0, 0xC2 / 255.0, 0x42 / 255.0, 1.0};  // #F5C242 (idle/none)
inline constexpr Rgba kUrElectricBlue{0x00 / 255.0, 0x39 / 255.0, 0xDE / 255.0, 1.0};  // #0039DE
// transport bar carriers (apple urLightBlue / urYellow): H1 and whodis pump
inline constexpr Rgba kUrLightBlue{0xD6 / 255.0, 0xE6 / 255.0, 0xF4 / 255.0, 1.0};  // #D6E6F4
inline constexpr Rgba kUrYellow{0xE6 / 255.0, 0xEA / 255.0, 0x23 / 255.0, 1.0};     // #E6EA23
// Surfaces and text:
inline constexpr Rgba kUrBackground{0x10 / 255.0, 0x10 / 255.0, 0x10 / 255.0, 1.0};  // #101010
inline constexpr Rgba kUrCardBackground{0x1C / 255.0, 0x1C / 255.0, 0x1C / 255.0, 1.0};  // #1C1C1C
inline constexpr Rgba kUrText{1.0, 1.0, 1.0, 1.0};
inline constexpr Rgba kUrTextMuted{0x98 / 255.0, 0x98 / 255.0, 0x98 / 255.0, 1.0};  // #989898
inline constexpr Rgba kUrTextFaint{0x5A / 255.0, 0x5A / 255.0, 0x5A / 255.0, 1.0};  // #5A5A5A
inline constexpr Rgba kUrBorderBase{1.0, 1.0, 1.0, 0.12};  // borders + chart axis
inline constexpr Rgba kUrDanger{0xF8 / 255.0, 0x52 / 255.0, 0x3B / 255.0, 1.0};  // #F8523B
inline constexpr Rgba kUrAccent{0xEF / 255.0, 0xF7 / 255.0, 0xBB / 255.0, 1.0};  // #EFF7BB

// Parse "rrggbb" / "#rrggbb" (the SDK GetColorHex format); fallback on failure.
Rgba ParseHexColor(const std::string& hex, const Rgba& fallback);
// "#rrggbb" for pango markup.
std::string HexForMarkup(const Rgba& color);
// Lower markdown links ("[text](url)", the form the store's terms lines carry
// in every locale) to pango <a href> markup, escaping everything else.
std::string MarkdownLinksToPango(const std::string& markdown);

// Install the drawer CSS (cards, chips, dots, mono values) once per display.
void EnsureDrawerCss();

// A rounded-12 tinted card (vertical box, .ur-card padding).
Gtk::Box* MakeCard(int spacing = 0);
// A capsule state chip. colorClass: "green" | "coral" | "muted".
// Highlighted renders solid (inverse text), idle renders tinted.
Gtk::Label* MakeChip(const std::string& text, const std::string& colorClass, bool highlighted);
// Muted caption label, left aligned.
Gtk::Label* MakeCaption(const std::string& text);
// Card header: muted title + trailing chevron.
Gtk::Box* MakeCardHeader(const std::string& title);

void RemoveAllChildren(Gtk::Box& box);
void SetPointerCursor(Gtk::Widget& widget);
// Pressed-state feedback for a tappable card (.ur-card-tappable): toggles the
// "pressed" CSS class from a capture-phase click gesture, since GTK4 never
// sets :active on a plain box. Pair with SetPointerCursor.
void WireCardPressFeedback(Gtk::Widget& widget);
// Pop a toast on the nearest enclosing AdwToastOverlay (MainWindow wraps its
// page stack in one; the detail sheets carry their own). No-op without one.
void ShowToast(Gtk::Widget& context, const std::string& message);
// Close (hide) the window on Escape, matching sheet behavior elsewhere.
void AddEscapeToClose(Gtk::Window& window);

// Entrance animation: fade the widget in (and slide it up by slideOffset px
// when nonzero) with a ~300ms cubic ease-out, starting after delayMs. The
// widget is hidden (opacity 0) immediately so staggered runs read as a
// sequence. Frame-clock driven; safe against widget destruction.
void AnimateEntrance(Gtk::Widget& widget, unsigned delayMs = 0, int slideOffset = 0,
                     unsigned durationMs = 300);

}  // namespace urnw
