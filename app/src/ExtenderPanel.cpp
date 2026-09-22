// SPDX-License-Identifier: MPL-2.0
#include "ExtenderPanel.hpp"

#include <algorithm>
#include <vector>

#include <glib.h>

#include "ExtenderRingGeometry.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// The active-extender rings, at the connect canvas's default dot size so the
// rows under the transport bar read as one stack.
constexpr int kRingDiameter = 12;
constexpr int kRingGap = 4;
// An extender whose color the SDK did not fill in still rings, in this
// neutral: the ring count must match the number of live extenders whatever the
// status carried.
constexpr Rgba kRingFallback{0xF8 / 255.0, 0xF8 / 255.0, 0xF8 / 255.0, 1.0};

// The state's label, through the store. Written out per case rather than
// looked up from the pure header's key id so the three literals stay greppable
// here, the way the status row's column labels are.
const char* StateLabel(extender::GossipState state) {
  switch (state) {
    case extender::GossipState::Connected: return T_("connected", "Connected");
    case extender::GossipState::Connecting: return T_("gossip_connecting", "Connecting");
    case extender::GossipState::Disconnected: return T_("disconnected", "Disconnected");
  }
  return "";
}

Rgba ColorForDot(extender::StatusDot dot) {
  switch (dot) {
    case extender::StatusDot::Green: return kUrGreen;
    case extender::StatusDot::Yellow: return kUrAmber;
    case extender::StatusDot::Red: return kUrCoral;
  }
  return kUrCoral;
}

// One HOLLOW ring: the canvas's ring stroke, at this row's size, in the
// extender's own color. Hollow is the point -- a filled dot here would read as
// a provider, which is what the connect canvas draws.
Gtk::DrawingArea* MakeRing(const std::string& colorHex) {
  auto* ring = Gtk::make_managed<Gtk::DrawingArea>();
  ring->set_content_width(kRingDiameter);
  ring->set_content_height(kRingDiameter);
  ring->set_valign(Gtk::Align::CENTER);
  const Rgba color = ParseHexColor(colorHex, kRingFallback);
  ring->set_draw_func([color](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const double stroke = extender::kRingStroke;
    const double radius = std::min(w, h) / 2.0 - stroke / 2.0;
    if (radius <= 0) return;
    cr->set_source_rgba(color.r, color.g, color.b, color.a);
    cr->set_line_width(stroke);
    cr->arc(w / 2.0, h / 2.0, radius, 0, 2 * G_PI);
    cr->stroke();
  });
  kit::MarkDecorative(*ring);
  return ring;
}

}  // namespace

ExtenderPanel::ExtenderPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 6) {
  EnsureDrawerCss();
  BuildUi();
}

void ExtenderPanel::BuildUi() {
  // the same title treatment the transport bar wears, so the two stack as
  // one block around the status row
  auto* title = Gtk::make_managed<Gtk::Label>(T_("extenders", "Extenders"));
  title->add_css_class("dim-label");
  title->add_css_class("ur-caption-11");
  title->set_xalign(0);
  append(*title);

  auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

  // the rings wrap rather than push the figures off the card
  rings_ = Gtk::make_managed<WrapRow>(kRingGap, kRingGap);
  line->append(*rings_);

  count_ = Gtk::make_managed<Gtk::Label>();
  count_->add_css_class("ur-caption-11");
  count_->set_xalign(0);
  line->append(*count_);

  // the gossip group sits hard right
  auto* gossip = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  gossip->set_hexpand(true);
  gossip->set_halign(Gtk::Align::END);
  dot_ = Gtk::make_managed<Gtk::Label>("●");
  kit::MarkDecorative(*dot_);  // the state label beside it says the same word
  gossip->append(*dot_);
  state_ = Gtk::make_managed<Gtk::Label>();
  state_->add_css_class("ur-caption-11");
  gossip->append(*state_);
  events_ = Gtk::make_managed<Gtk::Label>();
  events_->add_css_class("dim-label");
  events_->add_css_class("ur-caption-11");
  gossip->append(*events_);
  line->append(*gossip);

  append(*line);
  Render();
}

void ExtenderPanel::SetStatus(const std::optional<urnet::ExtenderStatus>& status) {
  std::vector<extender::Entry> entries;
  if (status && status->Extenders) {
    entries.reserve(status->Extenders->size());
    for (const auto& info : *status->Extenders) {
      entries.push_back(extender::Entry{info.Ip, info.ColorHex, info.InUse});
    }
  }
  const extender::Panel panel =
      extender::PanelFor(status.has_value(), entries,
                         status ? status->ActiveCount : 0, status ? status->ReserveCount : 0,
                         status ? status->GossipState : std::string(),
                         status ? status->EventCountLastMinute : 0);
  if (rendered_ && panel == panel_) return;  // a push that changes nothing touches nothing
  panel_ = panel;
  Render();
}

void ExtenderPanel::Render() {
  rendered_ = true;
  // No status at all is NOT "zero extenders": with no session the panel is
  // absent rather than reporting a number it does not have.
  set_visible(panel_.known);
  if (!panel_.known) return;

  rings_->Clear();
  for (const auto& colorHex : panel_.ringColorHexes) rings_->Append(*MakeRing(colorHex));
  // an empty strip collapses so the count sits where the rings would start
  rings_->set_visible(!panel_.ringColorHexes.empty());

  count_->set_text(Format(T_("extenders_active_of_reserve", "{0} of {1}"), panel_.active,
                          panel_.reserve));
  dot_->set_markup("<span foreground='" + HexForMarkup(ColorForDot(panel_.dot())) + "'>●</span>");
  state_->set_text(StateLabel(panel_.gossip));
  events_->set_text(Format(TN_("gossip_events_per_minute", "{} event/min", "{} events/min",
                               static_cast<unsigned long>(panel_.eventsPerMinute)),
                           panel_.eventsPerMinute));

  // "Extenders: 2 of 9. Active extenders 2. Gossip network: Connected,
  // 4 events/min" -- the numbers are the whole reading, and the rings say
  // nothing on their own.
  std::string label = T_("extenders", "Extenders");
  label += ": ";
  label += count_->get_text().raw();
  label += ". ";
  label += T_("active_extenders", "Active extenders");
  label += " ";
  label += std::to_string(panel_.ringColorHexes.size() + static_cast<size_t>(panel_.hiddenRings));
  label += ". ";
  label += T_("gossip_network", "Gossip network");
  label += ": ";
  label += state_->get_text().raw();
  label += ", ";
  label += events_->get_text().raw();
  kit::SetAccessibleLabel(*this, label);
}

}  // namespace urnw
