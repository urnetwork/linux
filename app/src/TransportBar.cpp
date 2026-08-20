// SPDX-License-Identifier: MPL-2.0
#include "TransportBar.hpp"

#include <algorithm>

#include <glib.h>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "TransportBarGeometry.hpp"
#include "TransportPresentation.hpp"
#include "UrMotion.hpp"

namespace urnw {
namespace {

// the bar: a thin rounded track (apple barHeight 8)
constexpr int kBarHeight = 8;
// legend / footer dots (apple 6pt)
constexpr int kDotSize = 6;
// the app's general 1s tween (motion tokens: kEpicMs), eased through the
// soft ease-in-out curve so a retargeted shape settles rather than snaps
constexpr int kTweenMs = motion::kEpicMs;

// The legend percent. The SDK's whole percents sum to exactly 100, so a used
// sliver can round to 0; label it "<1%" rather than a zero next to a visible
// segment.
std::string PercentText(const urnet::TransportShare& share) {
  if (share.Used && share.Percent == 0) return "<1%";
  return std::to_string(share.Percent) + "%";
}

// Fade a freshly built legend/footer item in over the general tween (an item
// leaving is simply removed). Gated like every other animation.
void FadeIn(Gtk::Widget& widget) {
  if (!motion::ShouldAnimate()) return;
  widget.set_opacity(0);
  motion::AnimateValue(widget, 0, kTweenMs, motion::kSoftP1, motion::kSoftP2,
                       [&widget](double eased) { widget.set_opacity(eased); });
}

bool SameShare(const urnet::TransportShare& a, const urnet::TransportShare& b) {
  return a.TransportType == b.TransportType && a.EgressByteCount == b.EgressByteCount &&
         a.IngressByteCount == b.IngressByteCount &&
         a.EgressPacketCount == b.EgressPacketCount &&
         a.IngressPacketCount == b.IngressPacketCount && a.Share == b.Share &&
         a.Boundary == b.Boundary && a.Percent == b.Percent && a.Used == b.Used &&
         a.Enabled == b.Enabled;
}

// Value equality of two distributions (the SDK structs carry no operator==):
// the dedup guard, so an idle tick that republishes the same window does not
// retrigger the tween or rebuild the rows.
bool SameDistribution(const std::optional<urnet::TransportDistribution>& a,
                      const std::optional<urnet::TransportDistribution>& b) {
  if (a.has_value() != b.has_value()) return false;
  if (!a) return true;
  if (a->Active != b->Active || a->ByteCount != b->ByteCount) return false;
  const size_t aCount = a->Shares ? a->Shares->size() : 0;
  const size_t bCount = b->Shares ? b->Shares->size() : 0;
  if (aCount != bCount) return false;
  for (size_t i = 0; i < aCount; ++i) {
    if (!SameShare((*a->Shares)[i], (*b->Shares)[i])) return false;
  }
  return true;
}

}  // namespace

// ---- WrapRow -----------------------------------------------------------------

WrapRow::WrapRow(int horizontalSpacing, int verticalSpacing)
    : horizontalSpacing_(horizontalSpacing), verticalSpacing_(verticalSpacing) {}

WrapRow::~WrapRow() {
  while (Gtk::Widget* child = get_first_child()) child->unparent();
}

void WrapRow::Append(Gtk::Widget& child) {
  child.set_parent(*this);  // appended after the last child
  queue_resize();
}

void WrapRow::Clear() {
  while (Gtk::Widget* child = get_first_child()) child->unparent();
  queue_resize();
}

Gtk::SizeRequestMode WrapRow::get_request_mode_vfunc() const {
  return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
}

void WrapRow::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum,
                            int& natural, int& minimum_baseline, int& natural_baseline) const {
  minimum_baseline = natural_baseline = -1;
  if (orientation == Gtk::Orientation::HORIZONTAL) {
    // minimum: the widest single item (a row can always hold one item, so an
    // item is never squeezed); natural: everything on one line
    int widest = 0;
    int total = 0;
    int count = 0;
    for (const Gtk::Widget* child = get_first_child(); child; child = child->get_next_sibling()) {
      if (!child->get_visible()) continue;
      int childMinimum = 0, childNatural = 0, ignoredMinimum = 0, ignoredNatural = 0;
      child->measure(Gtk::Orientation::HORIZONTAL, -1, childMinimum, childNatural,
                     ignoredMinimum, ignoredNatural);
      widest = std::max(widest, childNatural);
      total += childNatural + (0 < count ? horizontalSpacing_ : 0);
      ++count;
    }
    minimum = widest;
    natural = total;
    return;
  }
  minimum = natural = Layout(for_size, /*place=*/false);
}

void WrapRow::size_allocate_vfunc(int width, int /*height*/, int /*baseline*/) {
  Layout(width, /*place=*/true);
}

// One row walk for both measure and allocate: items flow left to right at
// their natural size and wrap when the next one would overflow `width` (a
// negative width is unconstrained: one line); each row is as tall as its
// tallest item and items share the row's bottom edge, so the labels sit on
// one common text baseline (they share a font size; bottom is the closest
// portable proxy for baseline alignment). Returns the total height.
int WrapRow::Layout(int width, bool place) const {
  struct Slot {
    Gtk::Widget* child = nullptr;
    int width = 0;
    int height = 0;
  };
  std::vector<Slot> row;
  int y = 0;
  int x = 0;
  int rowHeight = 0;
  auto flushRow = [&] {
    if (place) {
      int cx = 0;
      for (const Slot& slot : row) {
        slot.child->size_allocate(
            Gtk::Allocation(cx, y + rowHeight - slot.height, slot.width, slot.height),
            -1);
        cx += slot.width + horizontalSpacing_;
      }
    }
    row.clear();
    y += rowHeight;
    x = 0;
    rowHeight = 0;
  };
  // the children list is walked through the (const) sibling chain; the
  // pointers themselves are non-const so the place pass can allocate them
  for (Gtk::Widget* child = const_cast<WrapRow*>(this)->get_first_child(); child;
       child = child->get_next_sibling()) {
    if (!child->get_visible()) continue;
    int minimumWidth = 0, naturalWidth = 0, minimumHeight = 0, naturalHeight = 0;
    int ignoredMinimum = 0, ignoredNatural = 0;
    child->measure(Gtk::Orientation::HORIZONTAL, -1, minimumWidth, naturalWidth, ignoredMinimum,
                   ignoredNatural);
    child->measure(Gtk::Orientation::VERTICAL, naturalWidth, minimumHeight, naturalHeight,
                   ignoredMinimum, ignoredNatural);
    if (0 <= width && 0 < x && width < x + naturalWidth) {
      flushRow();
      y += verticalSpacing_;
    }
    row.push_back(Slot{child, naturalWidth, naturalHeight});
    x += naturalWidth + horizontalSpacing_;
    rowHeight = std::max(rowHeight, naturalHeight);
  }
  flushRow();
  return y;
}

// ---- TransportBar --------------------------------------------------------------

TransportBar::TransportBar() : Gtk::Box(Gtk::Orientation::VERTICAL, 6) {
  EnsureDrawerCss();
  BuildUi();
}

void TransportBar::BuildUi() {
  // title row, styled like the chart title, with a disclosure so the nested
  // tap target reads as its own control inside the card
  auto* titleRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* title = Gtk::make_managed<Gtk::Label>(T_("transports", "Transports"));
  title->add_css_class("dim-label");
  title->add_css_class("ur-caption-11");
  title->set_xalign(0);
  title->set_hexpand(true);
  titleRow->append(*title);
  auto* chevron = Gtk::make_managed<Gtk::Image>();
  chevron->set_from_icon_name("go-next-symbolic");
  chevron->set_pixel_size(11);
  chevron->add_css_class("ur-label-faint");
  chevron->set_valign(Gtk::Align::CENTER);
  kit::MarkDecorative(*chevron);
  titleRow->append(*chevron);
  append(*titleRow);

  // the bar: an empty track with the animated segments on top
  bar_.set_content_height(kBarHeight);
  bar_.set_hexpand(true);
  bar_.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    OnDrawBar(cr, w, h);
  });
  kit::MarkDecorative(bar_);
  append(bar_);

  // legend: the transports with traffic and their share (hidden while empty)
  legend_ = Gtk::make_managed<WrapRow>(12, 4);
  legend_->set_visible(false);
  append(*legend_);

  // unused footer: enabled transports that carried nothing in the window
  unused_ = Gtk::make_managed<WrapRow>(12, 4);
  unused_->set_visible(false);
  append(*unused_);

  // whole-component tap opens the editor. The bar sits inside a tappable
  // card (the client statistics card opens the contract details), so the
  // press CLAIMS the sequence: gestures on the ancestors are denied it and
  // the card's own click does not fire for a tap on the bar (the same thing
  // a Gtk::Button inside the card does).
  SetPointerCursor(*this);
  auto gesture = Gtk::GestureClick::create();
  Gtk::GestureClick* claiming = gesture.get();
  gesture->signal_pressed().connect([claiming](int, double, double) {
    claiming->set_state(Gtk::EventSequenceState::CLAIMED);
  });
  gesture->signal_released().connect([this](int, double, double) {
    if (on_activate) on_activate();
  });
  add_controller(gesture);
  kit::SetAccessibleLabel(*this, T_("transports", "Transports"));
}

void TransportBar::SetSurfaceColor(const Rgba& color) {
  surfaceColor_ = color;
  bar_.queue_draw();
}

void TransportBar::SetDistribution(
    const std::optional<urnet::TransportDistribution>& distribution) {
  // the distribution is inactive while the window is idle; only a real change
  // retargets the tween or touches the rows
  if (SameDistribution(distribution, distribution_)) return;
  distribution_ = distribution;

  std::vector<const urnet::TransportShare*> used;
  std::vector<const urnet::TransportShare*> unused;
  std::vector<double> boundaries;
  std::vector<Rgba> colors;
  const bool active = distribution && distribution->Active;
  if (distribution && distribution->Shares) {
    for (const auto& share : *distribution->Shares) {
      // stable order: the SDK's boundary is the segment's right edge
      boundaries.push_back(share.Boundary);
      colors.push_back(transport::Color(share.TransportType));
      if (share.Used) used.push_back(&share);
      if (share.Enabled && !share.Used) unused.push_back(&share);
    }
  }
  if (!colors.empty()) colors_ = std::move(colors);

  if (active) {
    // the live shape, and the shape to hold while the window is empty
    Retarget(boundaries, 1.0);
  } else {
    // hold the last shape and fade the segments out in place; before any
    // traffic has been seen the held vector is the live (all zero) one
    const std::vector<double> held = toBoundaries_.empty() ? boundaries : toBoundaries_;
    Retarget(held, 0.0);
  }

  RebuildLegend(used);
  RebuildUnused(unused);
}

void TransportBar::Retarget(const std::vector<double>& boundaries, double opacity) {
  // an unchanged target (a tick that only moved the byte counts) keeps the
  // tween in flight instead of restarting it
  if (boundaries == toBoundaries_ && opacity == toOpacity_) return;
  ++generation_;
  fromBoundaries_ = currentBoundaries_;
  fromOpacity_ = currentOpacity_;
  toBoundaries_ = boundaries;
  toOpacity_ = opacity;
  if (!motion::ShouldAnimate()) {
    // animations off: the settled state, no tween
    currentBoundaries_ = toBoundaries_;
    currentOpacity_ = toOpacity_;
    bar_.queue_draw();
    return;
  }
  // frame-clock tween on the bar's own clock (dies with the widget); a newer
  // retarget bumps the generation and this callback stops applying
  const uint64_t generation = generation_;
  motion::AnimateValue(bar_, 0, kTweenMs, motion::kSoftP1, motion::kSoftP2,
                       [this, generation](double eased) {
                         if (generation != generation_) return;
                         currentBoundaries_ =
                             transportbar::LerpBoundaries(fromBoundaries_, toBoundaries_, eased);
                         currentOpacity_ = fromOpacity_ + (toOpacity_ - fromOpacity_) * eased;
                         bar_.queue_draw();
                       });
}

void TransportBar::OnDrawBar(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
  const double w = width;
  const double h = std::min<double>(height, kBarHeight);
  const double r = h / 2.0;
  if (w <= 0 || h <= 0) return;

  // clip to the rounded track, then the empty track and the segments on top
  cr->begin_new_path();
  cr->arc(r, r, r, G_PI, 3 * G_PI / 2);
  cr->arc(w - r, r, r, 3 * G_PI / 2, 2 * G_PI);
  cr->arc(w - r, h - r, r, 0, G_PI / 2);
  cr->arc(r, h - r, r, G_PI / 2, G_PI);
  cr->close_path();
  cr->clip();

  const Rgba& track = kUrBorderBase;
  cr->set_source_rgba(track.r, track.g, track.b, track.a);
  cr->paint();

  if (currentOpacity_ <= 0 || currentBoundaries_.empty()) return;

  // every segment edge comes from the one interpolated boundary vector, so the
  // segments tile the width at every frame; hairlines in the surface color
  // between adjacent visible segments, easing in with the narrower one
  for (const auto& segment : transportbar::SegmentsFor(currentBoundaries_, w)) {
    const Rgba color =
        segment.index < colors_.size() ? colors_[segment.index]
                                       : (colors_.empty() ? kUrTextMuted : colors_.back());
    cr->set_source_rgba(color.r, color.g, color.b, color.a * currentOpacity_);
    // overshoot the right edge a hair to avoid seams from rounding
    cr->rectangle(segment.start, 0, segment.end - segment.start + 0.5, h);
    cr->fill();
    if (0 < segment.separatorWidth) {
      cr->set_source_rgba(surfaceColor_.r, surfaceColor_.g, surfaceColor_.b,
                          surfaceColor_.a * currentOpacity_);
      cr->rectangle(segment.start - segment.separatorWidth / 2, 0, segment.separatorWidth, h);
      cr->fill();
    }
  }
}

void TransportBar::RebuildLegend(const std::vector<const urnet::TransportShare*>& used) {
  std::vector<std::string> types;
  types.reserve(used.size());
  for (const auto* share : used) types.push_back(share->TransportType);
  if (types == legendTypes_) {
    // same membership: only the percents roll
    for (size_t i = 0; i < used.size() && i < legendPercents_.size(); ++i) {
      legendPercents_[i]->set_text(PercentText(*used[i]));
    }
    return;
  }
  // membership changed: rebuild the row in stable order; only the transports
  // that entered fade in (the ones that left are simply gone)
  const std::vector<std::string> previous = std::move(legendTypes_);
  legendTypes_ = types;
  legendPercents_.clear();
  legend_->Clear();
  for (const auto* share : used) {
    auto* item = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
    item->append(*transport::MakeDot(share->TransportType, kDotSize, /*hollow=*/false));
    auto* name = Gtk::make_managed<Gtk::Label>(transport::DisplayName(share->TransportType));
    name->add_css_class("ur-caption-11");
    // bottom-align the labels within the chip: the mono percent face has
    // different metrics from the caption face, so centering would skew their
    // baselines against each other
    name->set_valign(Gtk::Align::END);
    item->append(*name);
    // monospace digits so a rolling percent does not jitter its neighbours
    auto* percent = Gtk::make_managed<Gtk::Label>(PercentText(*share));
    percent->add_css_class("ur-mono-11");
    percent->add_css_class("dim-label");
    percent->set_valign(Gtk::Align::END);
    item->append(*percent);
    legendPercents_.push_back(percent);
    legend_->Append(*item);
    if (std::find(previous.begin(), previous.end(), share->TransportType) == previous.end()) {
      FadeIn(*item);
    }
  }
  legend_->set_visible(!used.empty());
}

void TransportBar::RebuildUnused(const std::vector<const urnet::TransportShare*>& unused) {
  std::vector<std::string> types;
  types.reserve(unused.size());
  for (const auto* share : unused) types.push_back(share->TransportType);
  if (types == unusedTypes_) return;
  const std::vector<std::string> previous = std::move(unusedTypes_);
  unusedTypes_ = types;
  unused_->Clear();
  if (!unused.empty()) {
    // prefixed with the word "unused", all faint
    auto* caption = Gtk::make_managed<Gtk::Label>(T_("transport_unused", "unused"));
    caption->add_css_class("ur-caption-11");
    caption->add_css_class("ur-label-faint");
    unused_->Append(*caption);
    for (const auto* share : unused) {
      auto* item = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
      item->append(*transport::MakeDot(share->TransportType, kDotSize, /*hollow=*/true));
      auto* name = Gtk::make_managed<Gtk::Label>(transport::DisplayName(share->TransportType));
      name->add_css_class("ur-caption-11");
      name->add_css_class("ur-label-faint");
      name->set_valign(Gtk::Align::END);
      item->append(*name);
      unused_->Append(*item);
      if (std::find(previous.begin(), previous.end(), share->TransportType) == previous.end()) {
        FadeIn(*item);
      }
    }
  }
  unused_->set_visible(!unused.empty());
}

}  // namespace urnw
