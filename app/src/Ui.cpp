// SPDX-License-Identifier: MPL-2.0
#include "Ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include <adwaita.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

namespace urnw {
namespace {

// The brand visual system (dark, matching the mac app theme; the app forces
// ADW_COLOR_SCHEME_FORCE_DARK at startup). All hex values mirror the palette
// constants in Ui.hpp — keep them in sync. The alpha() color function comes
// from the GTK CSS parser; adw_init runs before any window builds.
constexpr const char* kDrawerCss = R"(
/* surfaces + text */
window.background { background-color: #101010; color: #f8f8f8; }
.dim-label { color: #989898; opacity: 1; }
.ur-label-faint { color: #5a5a5a; }
/* amber for runtime constraint warnings (kUrAmber); symbolic icons follow it */
.ur-amber { color: #f5c242; }
/* cards: radius 12, padding 16, tinted #1c1c1c; tappable cards lighten on hover */
.ur-card { background-color: #1c1c1c; border: 1px solid alpha(#ffffff, .12);
  border-radius: 12px; padding: 16px; }
.ur-card-tappable { transition: background-color 150ms ease, border-color 150ms ease; }
.ur-card-tappable:hover { background-color: #242424; border-color: alpha(#ffffff, .22); }
/* GTK4 never sets :active on a plain box, so pressed feedback is the .pressed
   class WireCardPressFeedback toggles from the click gesture (windows
   UrCardPressedBrush #2A2A2A) */
.ur-card-tappable.pressed, .ur-card-tappable:active {
  background-color: #2a2a2a; border-color: alpha(#ffffff, .22); }
.ur-banner { background-color: #1c1c1c; border-radius: 12px; padding: 12px; }
/* dns recommendation pill: a small left-aligned coral-tinted capsule atop the
   Custom DNS card, nudging when the applied dns settings differ from the
   recommended ones (apple ConnectStatsSections.DnsRecommendationPill). font +
   text color inherit to the dot + message labels; the dot overrides its color
   with the country hex via inline markup. */
.ur-dns-pill { font-size: 11px; font-weight: 500; color: #ffffff; padding: 5px 10px;
  border-radius: 999px; background-color: alpha(#ff6c58, .15); }
/* capsule state chips */
.ur-chip { font-size: 10px; font-weight: 600; padding: 3px 7px; border-radius: 999px; }
.ur-chip-green { color: #87fb67; background-color: alpha(#87fb67, .14); }
.ur-chip-green-hi { color: #101010; background-color: #87fb67; }
.ur-chip-gold { color: #FFC400; background-color: alpha(#FFC400, .14); }
.ur-chip-gold-hi { color: #101010; background-image: linear-gradient(#FFE082, #FFC400); }
.ur-chip-coral { color: #ff6c58; background-color: alpha(#ff6c58, .14); }
.ur-chip-coral-hi { color: #ffffff; background-color: #ff6c58; }
.ur-chip-muted { color: #989898; background-color: alpha(#989898, .16); }
.ur-chip-muted-hi { color: #101010; background-color: #989898; }
/* status dots + values */
.ur-dot-on { color: #87fb67; }
.ur-dot-off { color: alpha(#5a5a5a, .4); }
.ur-value-on { color: #87fb67; }
/* monospace values (ids, IPs, URLs, identity key hashes) */
.ur-mono-15 { font-family: monospace; font-size: 15px; font-weight: 500; }
.ur-mono-13 { font-family: monospace; font-size: 13px; font-weight: 500; }
.ur-mono-12 { font-family: monospace; font-size: 12px; }
.ur-mono-11 { font-family: monospace; font-size: 11px; }
.ur-caption-11 { font-size: 11px; }
.ur-caption-10 { font-size: 10px; }
/* contract-details direction tints (send green / receive pink): arrow + rate */
.ur-fg-green { color: #87fb67; }
.ur-fg-pink { color: #ed8fff; }
/* the "N new" contract-details chip: accent capsule with dark text */
button.ur-newchip { background-color: #eff7bb; color: #101010; border-radius: 999px;
  padding: 6px 12px; min-height: 0; }
button.ur-newchip:hover { background-color: #f5fbd1; }
button.ur-newchip:active { background-color: #d9e3a3; }
/* accent: pale yellow primary actions with dark text (mac UrButton parity) */
button.suggested-action { background-color: #eff7bb; color: #101010; }
button.suggested-action:hover { background-color: #f5fbd1; }
button.suggested-action:active { background-color: #d9e3a3; }
button.suggested-action:disabled { background-color: alpha(#eff7bb, .2); color: alpha(#101010, .4); }
/* danger */
button.destructive-action { background-color: alpha(#f8523b, .15); color: #f8523b; }
button.destructive-action:hover { background-color: alpha(#f8523b, .25); }
button.destructive-action.flat { background: none; }
button.destructive-action.flat:hover { background-color: alpha(#f8523b, .12); }
/* switches: brand blue when on */
switch:checked { background-color: #638bfc; }
/* usage-bar legend dots take a per-series color via inline markup; the plan
   card and upgrade sheet reuse the card system above. */
/* upgrade product option cards: outlined, accent border when selected */
button.ur-option { background: none; border: 2px solid #5a5a5a; border-radius: 8px; padding: 14px 16px; }
button.ur-option:hover { background-color: alpha(#ffffff, .04); }
button.ur-option:checked { border-color: #eff7bb; background: none; }
/* verification code entry: large centered glyphs (the mac OTP boxes) */
entry.ur-otp { font-family: monospace; font-size: 24px; letter-spacing: 12px; }
/* inline form validation text */
.ur-error-text { color: #ff6c58; }
)";

int HexNibble(char c) {
  if ('0' <= c && c <= '9') return c - '0';
  if ('a' <= c && c <= 'f') return 10 + (c - 'a');
  if ('A' <= c && c <= 'F') return 10 + (c - 'A');
  return -1;
}

}  // namespace

void PostToMain(std::function<void()> fn) {
  // g_idle_add is documented thread-safe; the closure runs once on the GTK main loop.
  auto* heap = new std::function<void()>(std::move(fn));
  g_idle_add(
      +[](gpointer data) -> gboolean {
        auto* f = static_cast<std::function<void()>*>(data);
        (*f)();
        delete f;
        return G_SOURCE_REMOVE;
      },
      heap);
}

Rgba ParseHexColor(const std::string& hex, const Rgba& fallback) {
  std::string digits;
  digits.reserve(hex.size());
  for (char c : hex) {
    if (HexNibble(c) >= 0) digits.push_back(c);
  }
  if (digits.size() != 6) return fallback;
  auto channel = [&](size_t i) {
    return (HexNibble(digits[i]) * 16 + HexNibble(digits[i + 1])) / 255.0;
  };
  return Rgba{channel(0), channel(2), channel(4), 1.0};
}

std::string HexForMarkup(const Rgba& color) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", static_cast<int>(color.r * 255 + 0.5),
                static_cast<int>(color.g * 255 + 0.5), static_cast<int>(color.b * 255 + 0.5));
  return buf;
}

std::string MarkdownLinksToPango(const std::string& markdown) {
  auto escape = [](const std::string& text) {
    char* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string out = escaped ? escaped : "";
    g_free(escaped);
    return out;
  };
  std::string out;
  size_t pos = 0;
  while (pos < markdown.size()) {
    const size_t open = markdown.find('[', pos);
    if (open == std::string::npos) break;
    const size_t closeText = markdown.find(']', open);
    if (closeText == std::string::npos || closeText + 1 >= markdown.size() ||
        markdown[closeText + 1] != '(') {
      break;
    }
    const size_t closeUrl = markdown.find(')', closeText + 2);
    if (closeUrl == std::string::npos) break;
    out += escape(markdown.substr(pos, open - pos));
    const std::string text = markdown.substr(open + 1, closeText - open - 1);
    const std::string url = markdown.substr(closeText + 2, closeUrl - closeText - 2);
    out += "<a href=\"" + escape(url) + "\">" + escape(text) + "</a>";
    pos = closeUrl + 1;
  }
  out += escape(markdown.substr(pos));
  return out;
}

void EnsureDrawerCss() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  auto provider = Gtk::CssProvider::create();
  provider->load_from_data(kDrawerCss);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider->gobj()),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

Gtk::Box* MakeCard(int spacing) {
  auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, spacing);
  card->add_css_class("ur-card");
  return card;
}

Gtk::Label* MakeChip(const std::string& text, const std::string& colorClass, bool highlighted) {
  auto* chip = Gtk::make_managed<Gtk::Label>(text);
  chip->add_css_class("ur-chip");
  chip->add_css_class("ur-chip-" + colorClass + (highlighted ? "-hi" : ""));
  chip->set_valign(Gtk::Align::CENTER);
  return chip;
}

Gtk::Label* MakeCaption(const std::string& text) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class("dim-label");
  label->add_css_class("caption");
  label->set_xalign(0);
  return label;
}

Gtk::Box* MakeCardHeader(const std::string& title) {
  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* label = Gtk::make_managed<Gtk::Label>(title);
  label->add_css_class("dim-label");
  label->set_xalign(0);
  label->set_hexpand(true);
  header->append(*label);
  auto* chevron = Gtk::make_managed<Gtk::Image>();
  chevron->set_from_icon_name("go-next-symbolic");
  chevron->add_css_class("dim-label");
  header->append(*chevron);
  header->set_margin_bottom(8);
  return header;
}

void RemoveAllChildren(Gtk::Box& box) {
  while (Gtk::Widget* child = box.get_first_child()) {
    box.remove(*child);
  }
}

void SetPointerCursor(Gtk::Widget& widget) {
  gtk_widget_set_cursor_from_name(widget.gobj(), "pointer");
}

void WireCardPressFeedback(Gtk::Widget& widget) {
  // GTK4 sets :hover on any widget under the pointer but :active only on real
  // buttons, so a tappable card needs its own pressed feedback. A capture-
  // phase gesture that CLAIMS nothing: the card's own click gesture still
  // fires. unset_state covers release-outside and cancelled sequences.
  auto gesture = Gtk::GestureClick::create();
  gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  gesture->signal_pressed().connect(
      [&widget](int, double, double) { widget.add_css_class("pressed"); });
  gesture->signal_released().connect(
      [&widget](int, double, double) { widget.remove_css_class("pressed"); });
  gesture->signal_cancel().connect(
      [&widget](Gdk::EventSequence*) { widget.remove_css_class("pressed"); });
  widget.add_controller(gesture);
}

void ShowToast(Gtk::Widget& context, const std::string& message) {
  for (GtkWidget* widget = GTK_WIDGET(context.gobj()); widget;
       widget = gtk_widget_get_parent(widget)) {
    if (ADW_IS_TOAST_OVERLAY(widget)) {
      adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(widget), adw_toast_new(message.c_str()));
      return;
    }
  }
}

void AddEscapeToClose(Gtk::Window& window) {
  auto key = Gtk::EventControllerKey::create();
  key->signal_key_pressed().connect(
      [&window](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval == GDK_KEY_Escape) {
          window.set_visible(false);
          return true;
        }
        return false;
      },
      false);
  window.add_controller(key);
}

void AnimateEntrance(Gtk::Widget& widget, unsigned delayMs, int slideOffset,
                     unsigned durationMs) {
  widget.set_opacity(0);
  const int baseMargin = widget.get_margin_top();
  if (0 < slideOffset) widget.set_margin_top(baseMargin + slideOffset);

  Gtk::Widget* target = &widget;
  auto start = [target, baseMargin, slideOffset, durationMs] {
    // frame-clock driven cubic ease-out; the tick callback dies with the widget
    auto startTime = std::make_shared<gint64>(-1);
    target->add_tick_callback(
        [target, baseMargin, slideOffset, durationMs,
         startTime](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
          const gint64 now = clock->get_frame_time();  // µs
          if (*startTime < 0) *startTime = now;
          const double progress =
              std::min(1.0, static_cast<double>(now - *startTime) / (durationMs * 1000.0));
          const double eased = 1 - std::pow(1 - progress, 3);
          target->set_opacity(eased);
          if (0 < slideOffset) {
            target->set_margin_top(
                baseMargin + static_cast<int>(slideOffset * (1 - eased) + 0.5));
          }
          return progress < 1.0;
        });
  };
  if (delayMs == 0) {
    start();
  } else {
    // track_obj drops the timeout if the widget is destroyed before it fires
    Glib::signal_timeout().connect_once(sigc::track_obj(start, widget), delayMs);
  }
}

}  // namespace urnw

