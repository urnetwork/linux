// The extender panel (connect/EXTENDER.md K4): in the connect drawer's client
// statistics card, directly under the ip family status row. One line:
//
//   Extenders
//   ◯ ◯   2 of 9              ● Connected   4 events/min
//
// left to right -- one HOLLOW ring per active extender in the SDK's color for
// its ip (active = carrying at least one live connection right now), the count
// of active extenders over every usable directory entry, then the gossip
// network's status dot (green connected, yellow connecting, red disconnected)
// with its state label and the number of records and revocations applied from
// the feed or the mesh in the trailing 60 seconds.
//
// DECORATIVE. K4 is explicit that tapping does nothing and there is no details
// panel, so the whole component is inert and a tap on it is a tap on the card
// underneath, exactly like the status row above it. The rings are hidden from
// the accessibility tree and the panel names itself with its numbers, since a
// row of identical circles says nothing to a screen reader.
//
// The reading -- which extenders ring, which color the dot is, what the two
// figures are -- is the pure function in ExtenderStatusPresentation.hpp; this
// widget only draws it, and drops a push that changes nothing.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

#include "ExtenderStatusPresentation.hpp"
#include "TransportBar.hpp"

namespace urnw {

class ExtenderPanel : public Gtk::Box {
 public:
  ExtenderPanel();

  // Feed the device's ExtenderStatus (SdkHost::GetExtenderStatus), on the
  // DrawerEvent::ExtenderStatus tick. nullopt is "no session": the panel
  // hides, which is a different statement from "zero extenders".
  void SetStatus(const std::optional<urnet::ExtenderStatus>& status);

 private:
  void BuildUi();
  void Render();

  WrapRow* rings_ = nullptr;
  Gtk::Label* count_ = nullptr;
  Gtk::Label* dot_ = nullptr;
  Gtk::Label* state_ = nullptr;
  Gtk::Label* events_ = nullptr;

  extender::Panel panel_;
  bool rendered_ = false;  // forces the first paint even against a default panel
};

}  // namespace urnw
