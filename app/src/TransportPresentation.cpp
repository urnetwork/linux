// SPDX-License-Identifier: MPL-2.0
#include "TransportPresentation.hpp"

#include <algorithm>

#include <glib.h>

#include <urnetwork_sdk.hpp>

#include "I18n.hpp"
#include "PaneKit.hpp"

namespace urnw::transport {

std::string DisplayName(const std::string& transportType) {
  // product names, not localized (the brief: whodis / whodis pump are names)
  if (transportType == urnet::TransportTypeH3) return "H3";
  if (transportType == urnet::TransportTypeH1) return "H1";
  if (transportType == urnet::TransportTypeDns) return "whodis";
  if (transportType == urnet::TransportTypeDnsPump) return "whodis pump";
  if (transportType == urnet::TransportTypeP2p) return "P2P";
  if (transportType == urnet::TransportTypeUnknown) return T_("transport_queued", "queued");
  return transportType;
}

std::string Description(const std::string& transportMode) {
  if (transportMode == urnet::TransportModeH3) {
    return T_("transport_h3_description", "Direct over QUIC. Fastest where it is not filtered.");
  }
  if (transportMode == urnet::TransportModeH1) {
    return T_("transport_h1_description", "Direct over TLS. Works on most networks.");
  }
  if (transportMode == urnet::TransportModeDns) {
    return T_("transport_dns_description",
              "Disguised as DNS traffic. For networks that filter direct connections.");
  }
  if (transportMode == urnet::TransportModeDnsPump) {
    return T_("transport_dnspump_description",
              "Disguised as DNS traffic with a constant reply pump. Lowest bandwidth, highest "
              "availability.");
  }
  return {};
}

Rgba Color(const std::string& transportType) {
  if (transportType == urnet::TransportTypeH3) return kUrGreen;
  if (transportType == urnet::TransportTypeH1) return kUrLightBlue;
  if (transportType == urnet::TransportTypeDns) return kUrPink;
  if (transportType == urnet::TransportTypeDnsPump) return kUrYellow;
  if (transportType == urnet::TransportTypeP2p) return kUrElectricBlue;
  return kUrTextMuted;  // queued, and anything this app does not know
}

Gtk::DrawingArea* MakeDot(const std::string& transportType, int size, bool hollow) {
  const Rgba color = Color(transportType);
  auto* dot = Gtk::make_managed<Gtk::DrawingArea>();
  dot->set_content_width(size);
  dot->set_content_height(size);
  dot->set_valign(Gtk::Align::CENTER);
  dot->set_draw_func([color, hollow](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const double cx = w / 2.0;
    const double cy = h / 2.0;
    const double r = std::min(w, h) / 2.0;
    if (hollow) {
      cr->arc(cx, cy, std::max(0.5, r - 0.5), 0, 2 * G_PI);
      cr->set_source_rgba(color.r, color.g, color.b, color.a * 0.6);
      cr->set_line_width(1);
      cr->stroke();
    } else {
      cr->arc(cx, cy, r, 0, 2 * G_PI);
      cr->set_source_rgba(color.r, color.g, color.b, color.a);
      cr->fill();
    }
  });
  kit::MarkDecorative(*dot);
  return dot;
}

}  // namespace urnw::transport
