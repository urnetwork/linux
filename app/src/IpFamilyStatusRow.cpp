// SPDX-License-Identifier: MPL-2.0
#include "IpFamilyStatusRow.hpp"

#include <string>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// the gap between the three columns, the transport bar's legend gap
constexpr int kColumnGap = 8;
// the gap between a column's label and its lines, and between its lines
constexpr int kLineGap = 2;

// the muted and faint text tiers, the classes the transport bar's legend and
// its unused footer wear; the bright tier is the bare text color
constexpr const char* kMutedClass = "dim-label";
constexpr const char* kFaintClass = "ur-label-faint";

// The column label, through the store: the same three words the developer
// and DNS screens use for the families, plus the dualstack one.
const char* ColumnLabel(ipfamily::Column column) {
  switch (column) {
    case ipfamily::Column::Dualstack: return T_("ip_family_dualstack", "Dualstack");
    case ipfamily::Column::V4: return T_("ipv4", "IPv4");
    case ipfamily::Column::V6: return T_("ipv6", "IPv6");
  }
  return "";
}

// One status line's text. The counts are plural keys: the CLDR forms come
// from the catalog, never from C++.
std::string LineText(const ipfamily::Line& line) {
  switch (line.kind) {
    case ipfamily::LineKind::Connected:
      return Format(TN_("ip_family_connected_count", "{} connected", "{} connected",
                        static_cast<unsigned long>(line.count)),
                    line.count);
    case ipfamily::LineKind::Connecting:
      return Format(TN_("ip_family_connecting_count", "{} connecting", "{} connecting",
                        static_cast<unsigned long>(line.count)),
                    line.count);
    case ipfamily::LineKind::Disconnected:
      return T_("ip_family_disconnected", "disconnected");
  }
  return "";
}

void SetTier(Gtk::Label& label, ipfamily::Tier tier) {
  label.remove_css_class(kMutedClass);
  label.remove_css_class(kFaintClass);
  switch (tier) {
    case ipfamily::Tier::Best: break;
    case ipfamily::Tier::Active: label.add_css_class(kMutedClass); break;
    case ipfamily::Tier::Unavailable: label.add_css_class(kFaintClass); break;
  }
}

}  // namespace

IpFamilyStatusRow::IpFamilyStatusRow() {
  EnsureDrawerCss();
  BuildUi();
}

IpFamilyStatusRow::ColumnWidgets IpFamilyStatusRow::MakeColumn(Gtk::Box& row,
                                                               ipfamily::Column column) {
  ColumnWidgets widgets;
  widgets.box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, kLineGap);
  widgets.box->set_hexpand(true);
  widgets.box->set_valign(Gtk::Align::START);
  widgets.label = Gtk::make_managed<Gtk::Label>(ColumnLabel(column));
  widgets.label->add_css_class("ur-ipfamily-label");
  widgets.label->set_xalign(0);
  widgets.box->append(*widgets.label);
  for (auto*& line : widgets.lines) {
    line = Gtk::make_managed<Gtk::Label>();
    line->add_css_class("ur-caption-11");
    line->add_css_class("ur-ipfamily-line");
    line->set_xalign(0);
    widgets.box->append(*line);
  }
  row.append(*widgets.box);
  return widgets;
}

void IpFamilyStatusRow::BuildUi() {
  // The reserve: a full column -- a label and two status lines -- invisible
  // under the live row, so the row's height is the tallest a column can be
  // whatever the live columns show (mmm/DESIGNSTYLE.md "Placeholders, not
  // pop-in"). It is the overlay's sizing child; the live row is measured too,
  // and is never taller.
  auto* reserve = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kColumnGap);
  reserve->set_homogeneous(true);
  reserve->set_hexpand(true);
  ColumnWidgets sizer = MakeColumn(*reserve, ipfamily::Column::Dualstack);
  sizer.lines[0]->set_text(LineText(ipfamily::Line{ipfamily::LineKind::Connected, 0}));
  sizer.lines[1]->set_text(LineText(ipfamily::Line{ipfamily::LineKind::Connecting, 0}));
  reserve->set_opacity(0);
  reserve->set_can_target(false);
  reserve->set_can_focus(false);
  kit::MarkDecorative(*reserve);
  set_child(*reserve);

  auto* live = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kColumnGap);
  live->set_homogeneous(true);
  live->set_hexpand(true);
  for (int i = 0; i < ipfamily::kColumnCount; ++i) {
    columns_[i] = MakeColumn(*live, static_cast<ipfamily::Column>(i));
  }
  add_overlay(*live);
  set_measure_overlay(*live, true);
  set_clip_overlay(*live, false);

  Render();
}

void IpFamilyStatusRow::SetGrid(const std::vector<urnet::ProviderGridPoint>& points) {
  const ipfamily::Statuses statuses = ipfamily::StatusesFor(ipfamily::ReducePoints(points));
  if (rendered_ && statuses == statuses_) return;  // a push that changes nothing touches nothing
  statuses_ = statuses;
  Render();
}

void IpFamilyStatusRow::Render() {
  rendered_ = true;
  const auto tiers = ipfamily::TiersFor(statuses_);
  for (int i = 0; i < ipfamily::kColumnCount; ++i) {
    const auto& status = statuses_[static_cast<size_t>(i)];
    const auto tier = tiers[static_cast<size_t>(i)];
    ColumnWidgets& widgets = columns_[i];
    SetTier(*widgets.label, tier);
    const std::vector<ipfamily::Line> lines = ipfamily::LinesFor(status);
    for (size_t n = 0; n < ipfamily::kMaxLines; ++n) {
      Gtk::Label* line = widgets.lines[n];
      if (n < lines.size()) {
        line->set_text(LineText(lines[n]));
        SetTier(*line, tier);
        line->set_visible(true);
      } else {
        // an absent line collapses; the reserve keeps the row's height
        line->set_text("");
        line->set_visible(false);
      }
    }
  }
  UpdateAccessibleLabel();
}

void IpFamilyStatusRow::UpdateAccessibleLabel() {
  // "Dualstack: 3 connected, 1 connecting. IPv4: disconnected. IPv6: 2
  // connected." -- the three readings are the whole component.
  std::string label;
  for (int i = 0; i < ipfamily::kColumnCount; ++i) {
    const auto& status = statuses_[static_cast<size_t>(i)];
    if (0 < i) label += ". ";
    label += ColumnLabel(status.column);
    label += ": ";
    bool first = true;
    for (const auto& line : ipfamily::LinesFor(status)) {
      if (!first) label += ", ";
      first = false;
      label += LineText(line);
    }
  }
  label += ".";
  kit::SetAccessibleLabel(*this, label);
}

}  // namespace urnw
