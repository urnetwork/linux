// SPDX-License-Identifier: MPL-2.0
#include "TransferChart.hpp"

#include <algorithm>
#include <cmath>

#include <glib.h>

#include "Formatters.hpp"

namespace urnw {
namespace {

// duration of the smooth transitions: the rightmost value easing to a new
// bucket, and the axis scale easing to a new maximum
constexpr double kTransitionSeconds = 0.5;
// the top-right stats average over the last N buckets (~ N seconds)
constexpr int kAverageBucketCount = 5;
// redraw cadence while animated (the mac app runs 20fps; 10fps reads the same)
constexpr unsigned kTickMs = 100;

constexpr double kStatsBand = 30;
constexpr double kPeakBand = 13;

double NowSeconds() { return static_cast<double>(g_get_real_time()) / 1e6; }

bool IsActiveSample(const urnet::ThroughputSample& s) {
  return 0 < s.EgressByteCount || 0 < s.IngressByteCount || 0 < s.EgressPacketCount ||
         0 < s.IngressPacketCount;
}

int64_t LerpValue(int64_t a, int64_t b, double t) {
  return a + static_cast<int64_t>(std::llround(static_cast<double>(b - a) * t));
}

urnet::ThroughputSample LerpSample(const urnet::ThroughputSample& a,
                                   const urnet::ThroughputSample& b, double t) {
  urnet::ThroughputSample s{};
  s.EgressByteCount = LerpValue(a.EgressByteCount, b.EgressByteCount, t);
  s.IngressByteCount = LerpValue(a.IngressByteCount, b.IngressByteCount, t);
  s.EgressPacketCount = LerpValue(a.EgressPacketCount, b.EgressPacketCount, t);
  s.IngressPacketCount = LerpValue(a.IngressPacketCount, b.IngressPacketCount, t);
  return s;
}

void SetSource(const Cairo::RefPtr<Cairo::Context>& cr, const Rgba& c) {
  cr->set_source_rgba(c.r, c.g, c.b, c.a);
}

void SelectFont(const Cairo::RefPtr<Cairo::Context>& cr, bool mono, double size) {
  cr->select_font_face(mono ? "monospace" : "sans-serif", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);
  cr->set_font_size(size);
}

// Baseline y that vertically centers `extents` on centerY.
double CenterBaseline(const Cairo::TextExtents& extents, double centerY) {
  return centerY - extents.y_bearing - extents.height / 2;
}

// Catmull-Rom smoothing through the sample points, appended to the current path.
void AppendSmoothPath(const Cairo::RefPtr<Cairo::Context>& cr,
                      const std::vector<std::pair<double, double>>& pts) {
  cr->move_to(pts[0].first, pts[0].second);
  if (pts.size() == 2) {
    cr->line_to(pts[1].first, pts[1].second);
    return;
  }
  for (size_t i = 1; i < pts.size(); ++i) {
    const auto& p0 = pts[i >= 2 ? i - 2 : 0];
    const auto& p1 = pts[i - 1];
    const auto& p2 = pts[i];
    const auto& p3 = pts[std::min(i + 1, pts.size() - 1)];
    const double c1x = p1.first + (p2.first - p0.first) / 6;
    const double c1y = p1.second + (p2.second - p0.second) / 6;
    const double c2x = p2.first - (p3.first - p1.first) / 6;
    const double c2y = p2.second - (p3.second - p1.second) / 6;
    cr->curve_to(c1x, c1y, c2x, c2y, p2.first, p2.second);
  }
}

}  // namespace

double TransferChart::ScaleEase::ValueAt(double now) const {
  const double progress = std::clamp((now - startTime) / kTransitionSeconds, 0.0, 1.0);
  const double eased = 1 - std::pow(1 - progress, 3);
  return from + (to - from) * eased;
}

bool TransferChart::ScaleEase::Settling(double now) const {
  return now - startTime < kTransitionSeconds;
}

TransferChart::TransferChart(std::string title, Route route, Rgba byteColor, Rgba packetColor)
    : title_(std::move(title)), route_(route), byteColor_(byteColor), packetColor_(packetColor) {
  set_content_height(128);
  set_hexpand(true);
  set_draw_func(sigc::mem_fun(*this, &TransferChart::OnDraw));
  // a hidden window unmaps the chart and the tick loop stops; restart on map
  signal_map().connect([this] {
    queue_draw();
    EnsureTimer();
  });
}

TransferChart::~TransferChart() { timer_.disconnect(); }

void TransferChart::SetPoints(std::shared_ptr<const urnet::ThroughputPointList> points,
                              double windowSeconds) {
  window_ = 0 < windowSeconds ? windowSeconds : 60.0;
  entries_.clear();
  if (points) {
    entries_.reserve(points->size());
    for (const auto& point : *points) {
      Entry entry;
      entry.time = static_cast<double>(point.Time) / 1000.0;  // SDK time is unix ms
      const std::optional<urnet::ThroughputSample>* sample = nullptr;
      switch (route_) {
        case Route::Remote: sample = &point.Remote; break;
        case Route::Local: sample = &point.Local; break;
        case Route::Block: sample = &point.Block; break;
      }
      if (sample && *sample) entry.sample = **sample;
      entries_.push_back(entry);
    }
  }

  // scale to the window peak so the peak curve reaches the plot edge, with the
  // auto-scale floors; ease toward a changed target rather than jumping
  int64_t peakBytes = 0;
  int64_t peakPackets = 0;
  for (const auto& e : entries_) {
    peakBytes = std::max({peakBytes, e.sample.EgressByteCount, e.sample.IngressByteCount});
    peakPackets = std::max({peakPackets, e.sample.EgressPacketCount, e.sample.IngressPacketCount});
  }
  const double targetBytes = static_cast<double>(std::max<int64_t>(peakBytes, 1024));
  const double targetPackets = static_cast<double>(std::max<int64_t>(peakPackets, 8));
  const double now = NowSeconds();
  if (targetBytes != byteScale_.to) {
    byteScale_ = ScaleEase{byteScale_.ValueAt(now), targetBytes, now};
  }
  if (targetPackets != packetScale_.to) {
    packetScale_ = ScaleEase{packetScale_.ValueAt(now), targetPackets, now};
  }

  queue_draw();
  EnsureTimer();
}

int64_t TransferChart::AverageOverRecent(int64_t urnet::ThroughputSample::*field) const {
  if (entries_.empty()) return 0;
  const size_t count = std::min<size_t>(entries_.size(), kAverageBucketCount);
  int64_t sum = 0;
  for (size_t i = entries_.size() - count; i < entries_.size(); ++i) {
    sum += entries_[i].sample.*field;
  }
  return sum / static_cast<int64_t>(count);
}

bool TransferChart::Animated(double now) const {
  // moving only when recent traffic is still scrolling through the window, the
  // newest-bucket lerp is in flight, or an axis-scale ease is settling
  double lastActivity = -1;
  for (const auto& e : entries_) {
    if (IsActiveSample(e.sample)) lastActivity = std::max(lastActivity, e.time);
  }
  const bool hasRecentActivity = 0 <= lastActivity && now - lastActivity < window_;
  const bool lerpInFlight = !entries_.empty() && now - entries_.back().time < kTransitionSeconds;
  return hasRecentActivity || lerpInFlight || byteScale_.Settling(now) ||
         packetScale_.Settling(now);
}

void TransferChart::EnsureTimer() {
  if (timerRunning_ || !get_mapped() || !Animated(NowSeconds())) return;
  timerRunning_ = true;
  timer_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &TransferChart::OnTick), kTickMs);
}

bool TransferChart::OnTick() {
  queue_draw();
  if (!get_mapped() || !Animated(NowSeconds())) {
    timerRunning_ = false;
    return false;  // settle static; SetPoints/map restarts
  }
  return true;
}

void TransferChart::OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
  const double now = NowSeconds();
  const double w = width;
  const double h = height;

  // brand dark-theme palette (the app forces dark; see Ui.hpp)
  const Rgba text = kUrText;
  const Rgba textMuted = kUrTextMuted;
  const Rgba border = kUrBorderBase;  // chart axis

  const double plotTop = kStatsBand + kPeakBand;
  const double plotBottom = h - kPeakBand;
  const double centerY = (plotTop + plotBottom) / 2;
  const double plotHalf = std::max((plotBottom - plotTop) / 2, 8.0);

  // the center zero axis always spans the full width
  auto drawAxis = [&] {
    SetSource(cr, border);
    cr->set_line_width(1);
    cr->move_to(0, centerY);
    cr->line_to(w, centerY);
    cr->stroke();
  };

  // title, top left (the mac chart overlays it inside the stats band)
  if (!title_.empty()) {
    SelectFont(cr, false, 11);
    Cairo::TextExtents te;
    cr->get_text_extents(title_, te);
    SetSource(cr, textMuted);
    cr->move_to(0, CenterBaseline(te, 7));
    cr->show_text(title_);
  }

  // top-right rolling-average rows: egress up then ingress down; the byte and
  // packet values dim when zero, the triangle lights when the direction is live
  {
    const int64_t avgEgressBytes = AverageOverRecent(&urnet::ThroughputSample::EgressByteCount);
    const int64_t avgIngressBytes = AverageOverRecent(&urnet::ThroughputSample::IngressByteCount);
    const int64_t avgEgressPackets =
        AverageOverRecent(&urnet::ThroughputSample::EgressPacketCount);
    const int64_t avgIngressPackets =
        AverageOverRecent(&urnet::ThroughputSample::IngressPacketCount);
    auto drawRow = [&](bool pointsUp, int64_t byteValue, int64_t packetValue, double rowCenterY) {
      const double triangleSize = 7;
      double x = w - triangleSize;
      DrawTriangle(cr, x, rowCenterY - triangleSize / 2, triangleSize, pointsUp,
                   (0 < byteValue || 0 < packetValue) ? text : textMuted);
      SelectFont(cr, true, 10);
      const std::string packetText = FormatPacketRate(packetValue);
      Cairo::TextExtents te;
      cr->get_text_extents(packetText, te);
      x -= 5 + te.x_advance;
      SetSource(cr, packetColor_.WithAlpha(0 < packetValue ? 1.0 : 0.4));
      cr->move_to(x, CenterBaseline(te, rowCenterY));
      cr->show_text(packetText);
      const std::string byteText = FormatByteRate(byteValue);
      cr->get_text_extents(byteText, te);
      x -= 5 + te.x_advance;
      SetSource(cr, byteColor_.WithAlpha(0 < byteValue ? 1.0 : 0.4));
      cr->move_to(x, CenterBaseline(te, rowCenterY));
      cr->show_text(byteText);
    };
    drawRow(true, avgEgressBytes, avgEgressPackets, 7);
    drawRow(false, avgIngressBytes, avgIngressPackets, 20);
  }

  if (entries_.empty() || w <= 0) {
    drawAxis();
    return;
  }

  const double scaleBytes = byteScale_.ValueAt(now);
  const double scalePackets = packetScale_.ValueAt(now);

  // ease the newest bucket's value from the previous bucket's value so a
  // changed rightmost value transitions smoothly rather than hopping
  std::vector<Entry> padded = entries_;
  {
    Entry& last = padded.back();
    const urnet::ThroughputSample prev =
        2 <= padded.size() ? padded[padded.size() - 2].sample : urnet::ThroughputSample{};
    const double progress = std::clamp((now - last.time) / kTransitionSeconds, 0.0, 1.0);
    const double eased = 1 - std::pow(1 - progress, 3);
    last.sample = LerpSample(prev, last.sample, eased);
  }

  // pad so the baseline spans the full width: zeros back to the window start
  // on the left, and a hold of the latest value out to the right edge
  const double windowStart = now - window_;
  const Entry first = padded.front();
  if (windowStart < first.time) {
    const double step =
        2 <= padded.size() ? std::max(0.2, padded[1].time - first.time) : 1.0;
    const double rampTime = first.time - step;
    if (windowStart < rampTime) {
      Entry ramp;
      ramp.time = rampTime;
      padded.insert(padded.begin(), ramp);
    }
    Entry start;
    start.time = windowStart;
    padded.insert(padded.begin(), start);
  }
  if (padded.back().time < now) {
    Entry hold = padded.back();
    hold.time = now;
    padded.push_back(hold);
  }

  auto xAt = [&](double time) { return w * (1 - (now - time) / window_); };
  auto offset = [&](int64_t value, double scale) {
    return plotHalf * std::min(1.0, static_cast<double>(value) / std::max(scale, 1.0));
  };

  std::vector<std::pair<double, double>> egressBytes, ingressBytes, egressPackets, ingressPackets;
  egressBytes.reserve(padded.size());
  ingressBytes.reserve(padded.size());
  egressPackets.reserve(padded.size());
  ingressPackets.reserve(padded.size());
  for (const auto& e : padded) {
    const double x = xAt(e.time);
    egressBytes.emplace_back(x, centerY - offset(e.sample.EgressByteCount, scaleBytes));
    ingressBytes.emplace_back(x, centerY + offset(e.sample.IngressByteCount, scaleBytes));
    egressPackets.emplace_back(x, centerY - offset(e.sample.EgressPacketCount, scalePackets));
    ingressPackets.emplace_back(x, centerY + offset(e.sample.IngressPacketCount, scalePackets));
  }

  // clip each direction to its half so smoothing never crosses the center axis
  DrawSeries(cr, egressBytes, 0, centerY, w, byteColor_, 1.5, true, centerY);
  DrawSeries(cr, ingressBytes, centerY, h, w, byteColor_, 1.5, true, centerY);
  DrawSeries(cr, egressPackets, 0, centerY, w, packetColor_, 1.0, false, centerY);
  DrawSeries(cr, ingressPackets, centerY, h, w, packetColor_, 1.0, false, centerY);

  drawAxis();

  // sliding peak byte-rate labels: peak egress bucket over the top band, peak
  // ingress under the bottom band, x tracking the peak's time (clamped inside)
  const Entry* peakEgress = nullptr;
  const Entry* peakIngress = nullptr;
  for (const auto& e : entries_) {
    if (!peakEgress || peakEgress->sample.EgressByteCount < e.sample.EgressByteCount) {
      peakEgress = &e;
    }
    if (!peakIngress || peakIngress->sample.IngressByteCount < e.sample.IngressByteCount) {
      peakIngress = &e;
    }
  }
  if (peakEgress && 0 < peakEgress->sample.EgressByteCount) {
    DrawPeakLabel(cr, w, peakEgress->sample.EgressByteCount, peakEgress->time, now,
                  kStatsBand + kPeakBand / 2, true, textMuted);
  }
  if (peakIngress && 0 < peakIngress->sample.IngressByteCount) {
    DrawPeakLabel(cr, w, peakIngress->sample.IngressByteCount, peakIngress->time, now,
                  h - kPeakBand / 2, false, textMuted);
  }
}

void TransferChart::DrawSeries(const Cairo::RefPtr<Cairo::Context>& cr,
                               const std::vector<std::pair<double, double>>& pts, double clipY0,
                               double clipY1, double width, const Rgba& color, double lineWidth,
                               bool fillToCenter, double centerY) const {
  if (pts.size() < 2) return;
  cr->save();
  cr->rectangle(0, clipY0, width, clipY1 - clipY0);
  cr->clip();
  if (fillToCenter) {
    AppendSmoothPath(cr, pts);
    cr->line_to(pts.back().first, centerY);
    cr->line_to(pts.front().first, centerY);
    cr->close_path();
    SetSource(cr, color.WithAlpha(0.07));
    cr->fill();
  }
  AppendSmoothPath(cr, pts);
  SetSource(cr, color.WithAlpha(0.9));
  cr->set_line_width(lineWidth);
  cr->set_line_cap(Cairo::Context::LineCap::ROUND);
  cr->set_line_join(Cairo::Context::LineJoin::ROUND);
  cr->stroke();
  cr->restore();
}

void TransferChart::DrawPeakLabel(const Cairo::RefPtr<Cairo::Context>& cr, double width,
                                  int64_t value, double time, double now, double y, bool pointsUp,
                                  const Rgba& color) const {
  const std::string label = FormatByteRate(value);
  SelectFont(cr, true, 9);
  Cairo::TextExtents te;
  cr->get_text_extents(label, te);
  const double triangleSize = 6;
  const double gap = 3;
  const double total = te.x_advance + gap + triangleSize;

  const double rawX = width * (1 - (now - time) / window_);
  // keep the whole label (value + direction triangle) inside the component
  const double halfWidth = total / 2 + 2;
  const double centerX = std::clamp(rawX, halfWidth, std::max(halfWidth, width - halfWidth));
  const double leftX = centerX - total / 2;

  SetSource(cr, color);
  cr->move_to(leftX, CenterBaseline(te, y));
  cr->show_text(label);
  DrawTriangle(cr, leftX + te.x_advance + gap, y - triangleSize / 2, triangleSize, pointsUp,
               color);
}

void TransferChart::DrawTriangle(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y,
                                 double size, bool pointsUp, const Rgba& color) const {
  SetSource(cr, color);
  if (pointsUp) {
    cr->move_to(x + size / 2, y);
    cr->line_to(x + size, y + size);
    cr->line_to(x, y + size);
  } else {
    cr->move_to(x, y);
    cr->line_to(x + size, y);
    cr->line_to(x + size / 2, y + size);
  }
  cr->close_path();
  cr->fill();
}

}  // namespace urnw
