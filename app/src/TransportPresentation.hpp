// The transport vocabulary the transport bar and the transport settings
// editor share: display names, brand colors and one-line descriptions for the
// SDK's transport types / modes (the strings are identical for the selectable
// carriers: h3, h1, dns, dnspump; p2p and unknown are observable only).
//
// PRESENTATION ONLY (apple TransportSettingsStore.TransportType parity). Every
// rule -- the stable stats order, the selectable modes and their preference
// order, which carriers a policy enables, the Auto editing constraints -- is
// the SDK's (urnet::selectableTransportModes, urnet::transportSettings*), so
// it is shared and tested once for every platform; nothing here orders or
// selects anything.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include <gtkmm.h>

#include "Ui.hpp"

namespace urnw::transport {

// The display name. The DNS carriers carry their product names ("whodis",
// "whodis pump"), which are not localized; the queued bucket ("queued", the
// SDK's unknown type: admitted but not yet attributed to a physical carrier)
// is a plain word and is. An unrecognized type (a newer SDK vocabulary) shows
// its raw id.
std::string DisplayName(const std::string& transportType);

// A one line description for the settings editor rows; empty for the
// non-selectable types.
std::string Description(const std::string& transportMode);

// The brand color of the carrier for the bar segments and the legend dots:
// H3 green, H1 light blue, whodis pink, whodis pump yellow, P2P electric blue;
// the queued bucket (and anything unrecognized) is neutral muted. Coral is
// deliberately not used so the bar cannot be confused with the Blocked chart
// next to it.
Rgba Color(const std::string& transportType);

// A `size` px dot in the transport's color: filled for a transport with
// traffic (or a selectable mode row), or a hollow 1px ring at 60% for an idle
// (unused) one -- the color mapping stays legible either way. Drawn rather
// than a glyph so the size and stroke do not depend on the font. Decorative
// (hidden from the accessibility tree); the caller owns the placement.
Gtk::DrawingArea* MakeDot(const std::string& transportType, int size, bool hollow);

}  // namespace urnw::transport
