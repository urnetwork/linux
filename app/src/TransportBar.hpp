// The transport distribution bar (port of the apple TransportDistributionBar):
// the remote traffic of the stats window partitioned by the transport that
// carried it, as one full-width stacked bar under the transfer chart, with a
// "Transports ›" title row, a legend (dot + name + SDK percent per transport
// with traffic) and an "unused" footer (hollow dots for the enabled transports
// that carried nothing in the window). The whole component is one tap target
// that opens the transport settings editor; its click claims the sequence, so
// it wins over the tappable card around it.
//
// All the numbers -- shares, cumulative boundaries, whole percents, used,
// enabled, the stable order -- come from the SDK's TransportDistribution (the
// contract view controller); this widget only draws and animates them. The
// segments are read from ONE animated vector of boundaries (a 1s tween off the
// frame clock, restarted from the current shape on every change) so they tile
// exactly 100% mid-tween; when the window has no remote traffic the segments
// fade out over the tween while the last shape is held, leaving the faint
// empty track, and fade back in from that shape when traffic resumes.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

#include "Ui.hpp"

namespace urnw {

// A left-aligned horizontal flow that wraps its children onto the next line
// when they do not fit (the apple FlowRow layout), for the legend and footer
// rows on narrow cards. Children are managed widgets appended in order.
class WrapRow : public Gtk::Widget {
 public:
  WrapRow(int horizontalSpacing, int verticalSpacing);
  ~WrapRow() override;

  void Append(Gtk::Widget& child);
  void Clear();
  bool IsEmpty() const { return get_first_child() == nullptr; }

 protected:
  Gtk::SizeRequestMode get_request_mode_vfunc() const override;
  void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override;
  void size_allocate_vfunc(int width, int height, int baseline) override;

 private:
  // The wrapped rows for a given width: the height needed, and (when `place`
  // is set) each child's allocation. One algorithm for measure and allocate.
  int Layout(int width, bool place) const;

  int horizontalSpacing_;
  int verticalSpacing_;
};

class TransportBar : public Gtk::Box {
 public:
  TransportBar();

  // Feed the window's transport distribution (SdkHost::ClientTransportDistribution /
  // ProviderTransportDistribution), on the same throughput tick as the charts.
  // nullopt = no session: the bar holds its shape and fades to the empty track.
  // Dedups by value: an unchanged distribution retriggers nothing.
  void SetDistribution(const std::optional<urnet::TransportDistribution>& distribution);

  // The fill of the surface the bar sits on (the card / the pane), used for
  // the hairline separators between segments. Defaults to the card color.
  void SetSurfaceColor(const Rgba& color);

  // Opens the transport settings editor (the whole component is the target).
  std::function<void()> on_activate;

 private:
  void BuildUi();
  void OnDrawBar(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
  // Retarget the tween: boundaries + opacity ease from the current values to
  // the new targets over the general 1s tween (instant with animations off).
  void Retarget(const std::vector<double>& boundaries, double opacity);
  void RebuildLegend(const std::vector<const urnet::TransportShare*>& used);
  void RebuildUnused(const std::vector<const urnet::TransportShare*>& unused);

  Gtk::DrawingArea bar_;
  WrapRow* legend_ = nullptr;
  WrapRow* unused_ = nullptr;

  // the last published distribution (dedup) and, per share index in the SDK's
  // stable order, the segment color
  std::optional<urnet::TransportDistribution> distribution_;
  std::vector<Rgba> colors_;
  Rgba surfaceColor_ = kUrCardBackground;

  // the tween: from -> to over the general tween, `current` is what draws.
  // The boundaries are held while the window is empty (only the opacity
  // tweens), so the bar fades in place instead of collapsing to a corner.
  std::vector<double> fromBoundaries_;
  std::vector<double> toBoundaries_;
  std::vector<double> currentBoundaries_;
  double fromOpacity_ = 0;
  double toOpacity_ = 0;
  double currentOpacity_ = 0;
  // bumped on every retarget so an in-flight tick callback no-ops
  uint64_t generation_ = 0;

  // the legend / footer membership (transport types in stable order) that
  // the rows were last built for; percent labels update in place otherwise
  std::vector<std::string> legendTypes_;
  std::vector<Gtk::Label*> legendPercents_;
  std::vector<std::string> unusedTypes_;
};

}  // namespace urnw
