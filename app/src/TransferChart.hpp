// Live transfer chart for one throughput route (port of the apple app's
// TransferChart). Mirrored around a center horizontal axis: egress above,
// ingress below, newest at the right edge sliding left. Byte and packet
// series draw on independent auto-scales (floors 1024 bytes / 8 packets)
// whose maxima ease toward new peaks; the rolling 5-bucket averages label the
// top right and sliding peak byte-rate labels ride the top/bottom bands.
// Redraws on a ~10fps Glib timeout only while there is activity in-window or
// an ease in flight. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

#include "Ui.hpp"

namespace urnw {

class TransferChart : public Gtk::DrawingArea {
 public:
  enum class Route { Remote, Local, Block };

  TransferChart(std::string title, Route route, Rgba byteColor = kUrGreen,
                Rgba packetColor = kUrPink);
  ~TransferChart() override;

  // Feed the latest throughput window (the point list is shared across the
  // three charts; each picks its route's sample).
  void SetPoints(std::shared_ptr<const urnet::ThroughputPointList> points, double windowSeconds);

 private:
  // A time-based cubic ease-out of a scalar from `from` to `to`.
  struct ScaleEase {
    double from = 0;
    double to = 0;
    double startTime = 0;
    double ValueAt(double now) const;
    bool Settling(double now) const;
  };
  struct Entry {
    double time = 0;
    urnet::ThroughputSample sample{};
  };

  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
  void DrawSeries(const Cairo::RefPtr<Cairo::Context>& cr,
                  const std::vector<std::pair<double, double>>& pts, double clipY0, double clipY1,
                  double width, const Rgba& color, double lineWidth, bool fillToCenter,
                  double centerY) const;
  void DrawPeakLabel(const Cairo::RefPtr<Cairo::Context>& cr, double width, int64_t value,
                     double time, double now, double y, bool pointsUp, const Rgba& color) const;
  void DrawTriangle(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double size,
                    bool pointsUp, const Rgba& color) const;
  int64_t AverageOverRecent(int64_t urnet::ThroughputSample::*field) const;
  bool Animated(double now) const;
  void EnsureTimer();
  bool OnTick();

  std::string title_;
  Route route_;
  Rgba byteColor_;
  Rgba packetColor_;

  // route samples extracted from the shared point list, oldest first, time in
  // unix seconds
  std::vector<Entry> entries_;
  double window_ = 60.0;
  ScaleEase byteScale_{1024, 1024, 0};
  ScaleEase packetScale_{8, 8, 0};

  bool timerRunning_ = false;
  sigc::connection timer_;
};

}  // namespace urnw
