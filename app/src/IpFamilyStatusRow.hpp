// The ip family status row (connect/IPV6.md D2): under the transport bar in
// the client statistics card, one row of three equal, top-aligned columns --
// Dualstack, IPv4, IPv6 -- each its label in the pixel display face over its
// status: "n connected" then "m connecting", each line only when its count is
// not zero, or "disconnected" alone. No section title: the labels are the
// header.
//
//   Dualstack      IPv4           IPv6
//   3 connected    disconnected   2 connected
//   1 connecting
//
// The whole column takes its tier: the best family with a connected provider
// is the text color, the other live columns are the muted color and an empty
// column is the faint color (IpFamilyStatus.hpp decides which is which). The
// row always reserves a label and two status lines, so it never reflows as
// lines come and go; the reserve is an invisible template column under the
// live columns.
//
// DECORATIVE: the columns carry no interaction; a tap on the row is a tap on
// the card. The whole component names itself to accessibility with its three
// readings.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

#include "IpFamilyStatus.hpp"

namespace urnw {

class IpFamilyStatusRow : public Gtk::Overlay {
 public:
  IpFamilyStatusRow();

  // Feed the live provider grid (LiveStats::gridPoints), on the same push the
  // hero canvas rides. An empty list is a normal reading (no session, nothing
  // added yet) and renders as three "disconnected" columns. Dedups by value: a
  // push that changes no count touches nothing.
  void SetGrid(const std::vector<urnet::ProviderGridPoint>& points);

 private:
  struct ColumnWidgets {
    Gtk::Box* box = nullptr;
    Gtk::Label* label = nullptr;
    Gtk::Label* lines[ipfamily::kMaxLines] = {nullptr, nullptr};
  };

  void BuildUi();
  ColumnWidgets MakeColumn(Gtk::Box& row, ipfamily::Column column);
  void Render();
  void UpdateAccessibleLabel();

  ColumnWidgets columns_[ipfamily::kColumnCount];
  ipfamily::Statuses statuses_ = ipfamily::StatusesFor({});
  bool rendered_ = false;
};

}  // namespace urnw
