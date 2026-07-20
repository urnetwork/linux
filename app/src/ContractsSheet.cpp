// SPDX-License-Identifier: MPL-2.0
#include "ContractsSheet.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// geometry (apple ContractBlock parity)
constexpr double kCircleSlot = 56;   // the fixed square each contract occupies
constexpr double kMinDiameter = 16;  // smallest visible ring
// a stream contract draws a second concentric ring this far (radially) outside the
// outer ring, so streams read as a double ring vs a single ring for direct contracts.
// Applied to the diameter doubled: a 4px radial gap is an 8px diameter delta. (apple
// streamRingGap)
constexpr double kStreamRingGap = 4;
constexpr double kBlockSpacing = 4;  // gap between stacked blocks (apple VStack spacing 4)
constexpr double kPitch = kCircleSlot + kBlockSpacing;
constexpr int kStatsGap = 10;  // circle<->stats gap inside a block

// animation durations (microseconds)
constexpr gint64 kEaseUs = 500000;    // inner disc / diameter / brightness ease
constexpr gint64 kSettleUs = 500000;  // stack fall/rise (apple settleDuration)
constexpr gint64 kSlideUs = 400000;   // leaver slide-off (apple slideDuration)
constexpr gint64 kEnterUs = 350000;   // arrival fade-in
constexpr gint64 kRowFadeUs = 250000;  // closed-row fade-out

// which way a leaving contract slides: send (green) off the leading (left) edge,
// receive (pink) off the trailing (right) edge (apple removalEdge).
enum class EjectEdge { Leading, Trailing };

// one contract, flattened for the stack widgets (they can't see the private
// ContractsSheet::EntryVM); same fields
struct StackEntry {
  std::string id;
  int64_t used = 0;
  int64_t total = 0;
  int64_t bitRate = 0;
  bool hasStream = false;  // stream contract -> double concentric outer ring
};

void SetCairoColor(const Cairo::RefPtr<Cairo::Context>& cr, const Rgba& c) {
  cr->set_source_rgba(c.r, c.g, c.b, c.a);
}

double EaseInOut(double t) {
  const double p = std::clamp(t, 0.0, 1.0);
  return p * p * (3 - 2 * p);
}
double EaseOutCubic(double t) {
  const double p = std::clamp(t, 0.0, 1.0);
  return 1 - std::pow(1 - p, 3);
}
double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// One contract circle: a fixed 56x56 area drawing the outer ring (diameter
// area-proportional to the stack max) and the inner used-fraction disc. A
// contract moving bytes brightens the ring. Diameter, fraction and brightness
// each ease to their target on the frame clock, so a rescale (new stack max), a
// usage bump and an activity change all animate smoothly. The stack just pushes
// new targets.
class ContractCircleArea : public Gtk::DrawingArea {
 public:
  ContractCircleArea(const Rgba& color, bool hasStream)
      : color_(color), hasStream_(hasStream) {
    set_content_width(static_cast<int>(kCircleSlot));
    set_content_height(static_cast<int>(kCircleSlot));
    set_valign(Gtk::Align::CENTER);
    set_draw_func(sigc::mem_fun(*this, &ContractCircleArea::OnDraw));
  }

  void SetTarget(int64_t total, int64_t used, int64_t stackMax, bool active) {
    const double dTarget =
        (0 < stackMax && 0 < total)
            ? std::clamp(kCircleSlot * std::sqrt(static_cast<double>(total) /
                                                 static_cast<double>(stackMax)),
                         kMinDiameter, kCircleSlot)
            : kMinDiameter;
    const double fTarget =
        0 < total ? std::min(1.0, static_cast<double>(used) / static_cast<double>(total)) : 0.0;
    const double bTarget = active ? 1.0 : 0.0;

    if (!seeded_) {
      seeded_ = true;
      diameter_ = diaTo_ = dTarget;
      fraction_ = fracTo_ = fTarget;
      brightness_ = brightTo_ = bTarget;
      queue_draw();
      return;
    }
    if (dTarget == diaTo_ && fTarget == fracTo_ && bTarget == brightTo_) return;
    diaFrom_ = diameter_;
    fracFrom_ = fraction_;
    brightFrom_ = brightness_;
    diaTo_ = dTarget;
    fracTo_ = fTarget;
    brightTo_ = bTarget;
    animStart_ = g_get_monotonic_time();
    animating_ = true;
    EnsureTick();
  }

 private:
  void EnsureTick() {
    if (ticking_) return;
    ticking_ = true;
    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool { return OnTick(); });
  }
  bool OnTick() {
    if (!animating_) {
      ticking_ = false;
      return false;
    }
    const double p =
        std::clamp(static_cast<double>(g_get_monotonic_time() - animStart_) / kEaseUs, 0.0, 1.0);
    const double e = EaseOutCubic(p);
    diameter_ = Lerp(diaFrom_, diaTo_, e);
    fraction_ = Lerp(fracFrom_, fracTo_, e);
    brightness_ = Lerp(brightFrom_, brightTo_, e);
    queue_draw();
    if (1.0 <= p) {
      animating_ = false;
      ticking_ = false;
      return false;
    }
    return true;
  }

  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    const double cx = width / 2.0;
    const double cy = height / 2.0;

    // outer ring: alpha 0.55 (idle) -> 1.0 (active), width 1 -> 1.5
    const double alpha = 0.55 + 0.45 * brightness_;
    const double lineWidth = 1.0 + 0.5 * brightness_;
    SetCairoColor(cr, color_.WithAlpha(alpha));
    cr->set_line_width(lineWidth);
    cr->arc(cx, cy, std::max(0.5, diameter_ / 2 - lineWidth / 2), 0, 2 * G_PI);
    cr->stroke();

    // stream contracts: a second concentric ring kStreamRingGap (radially) outside
    // the main outer ring, same color/width/brightness -- streams read as a double
    // ring vs a single ring for direct contracts. A radial gap is twice the diameter
    // delta, hence 2x. It stays outside the used-fraction inner disc (drawn below, at
    // most `diameter_` across), so it remains visible even when the contract is full.
    // (apple streamRingGap)
    if (hasStream_) {
      cr->arc(cx, cy, std::max(0.5, (diameter_ + 2 * kStreamRingGap) / 2 - lineWidth / 2), 0,
              2 * G_PI);
      cr->stroke();
    }

    // inner disc: area-proportional to the used fraction of THIS contract
    const double innerSize =
        0 < fraction_ ? std::max(4.0, diameter_ * std::sqrt(fraction_)) : 0.0;
    if (0 < innerSize) {
      cr->arc(cx, cy, innerSize / 2, 0, 2 * G_PI);
      SetCairoColor(cr, color_.WithAlpha(0.3));
      cr->fill_preserve();
      SetCairoColor(cr, color_.WithAlpha(0.6));
      cr->set_line_width(0.5);
      cr->stroke();
    }
  }

  Rgba color_;
  bool hasStream_ = false;  // stream contract -> draw the second concentric ring
  bool seeded_ = false;
  bool ticking_ = false;
  bool animating_ = false;
  gint64 animStart_ = 0;
  double diameter_ = kMinDiameter, diaFrom_ = kMinDiameter, diaTo_ = kMinDiameter;
  double fraction_ = 0, fracFrom_ = 0, fracTo_ = 0;
  double brightness_ = 0, brightFrom_ = 0, brightTo_ = 0;
};

// One contract as a fixed-height row: the circle plus its used/total counts. A
// mirrored block (send) puts the stats on the outside and the circle against the
// row center (stats|circle); an unmirrored block (receive) is circle|stats.
class ContractBlock : public Gtk::Box {
 public:
  ContractBlock(const std::string& id, const Rgba& color, bool mirrored, bool hasStream)
      : Gtk::Box(Gtk::Orientation::HORIZONTAL, kStatsGap), id_(id) {
    set_valign(Gtk::Align::START);
    circle_ = Gtk::make_managed<ContractCircleArea>(color, hasStream);

    auto* stats = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    stats->set_valign(Gtk::Align::CENTER);
    stats->set_halign(mirrored ? Gtk::Align::END : Gtk::Align::START);
    used_ = Gtk::make_managed<Gtk::Label>();
    used_->add_css_class("ur-caption-11");
    used_->set_xalign(mirrored ? 1.0 : 0.0);
    total_ = Gtk::make_managed<Gtk::Label>();
    total_->add_css_class("ur-caption-10");
    total_->add_css_class("dim-label");
    total_->set_xalign(mirrored ? 1.0 : 0.0);
    stats->append(*used_);
    stats->append(*total_);

    if (mirrored) {
      append(*stats);
      append(*circle_);
    } else {
      append(*circle_);
      append(*stats);
    }
  }

  const std::string& Id() const { return id_; }
  int64_t Total() const { return totalBytes_; }

  void SetEntry(int64_t used, int64_t total, int64_t bitRate, int64_t stackMax) {
    totalBytes_ = total;
    used_->set_text(FormatByteCountCompact(used));
    total_->set_text(Format(T_("of_total", "of {}"), FormatByteCountCompact(total)));
    circle_->SetTarget(total, used, stackMax, 0 < bitRate);
  }

 private:
  std::string id_;
  int64_t totalBytes_ = 0;
  ContractCircleArea* circle_ = nullptr;
  Gtk::Label* used_ = nullptr;
  Gtk::Label* total_ = nullptr;
};

// The animated pile for one direction: a custom container that stacks
// ContractBlock children newest-on-top and choreographs membership like the
// apple ContractStackView. A leaving contract slides off toward the eject edge
// (holding its slot open); once the last leaver has cleared, one settle drops
// the leavers, admits arrivals at the top, and the stack falls into the freed
// space. Value changes (used bytes, bit rate) apply live in any phase. Blocks
// stay keyed by contract id so identity is stable. overflow VISIBLE lets a
// leaver slide clear of its slot and off the row.
class ContractStack : public Gtk::Widget {
 public:
  ContractStack(const Rgba& color, bool mirrored, EjectEdge edge)
      : Glib::ObjectBase("UrnwContractStack"),
        Gtk::Widget(),
        color_(color),
        mirrored_(mirrored),
        edge_(edge) {
    set_overflow(Gtk::Overflow::VISIBLE);
    set_valign(Gtk::Align::START);
    set_halign(mirrored ? Gtk::Align::END : Gtk::Align::START);
  }
  ~ContractStack() override {
    for (auto& s : slots_) s.block->unparent();
    slots_.clear();
  }

  void SetStackMaxCallback(std::function<void(int64_t)> cb) { onStackMax_ = std::move(cb); }

  void SetEntries(const std::vector<StackEntry>& entries) {
    truth_ = entries;
    haveTruth_ = true;
    Sync();
  }

  bool Empty() const { return slots_.empty(); }

 protected:
  void measure_vfunc(Gtk::Orientation orientation, int, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override {
    minimum_baseline = natural_baseline = -1;
    if (orientation == Gtk::Orientation::HORIZONTAL) {
      int w = 0;
      for (const auto& s : slots_) {
        int cmin, cnat, bmin, bnat;
        s.block->measure(Gtk::Orientation::HORIZONTAL, -1, cmin, cnat, bmin, bnat);
        w = std::max(w, cnat);
      }
      minimum = natural = w;
    } else {
      double bottom = 0;
      for (const auto& s : slots_) bottom = std::max(bottom, s.curY + kCircleSlot);
      minimum = natural = static_cast<int>(std::ceil(bottom));
    }
  }

  void size_allocate_vfunc(int width, int, int) override {
    for (auto& s : slots_) {
      int cmin, cnat, bmin, bnat;
      s.block->measure(Gtk::Orientation::HORIZONTAL, -1, cmin, cnat, bmin, bnat);
      const int blockW = cnat;
      const int x = (mirrored_ ? (width - blockW) : 0) + static_cast<int>(std::lround(s.xOffset));
      Gtk::Allocation a;
      a.set_x(x);
      a.set_y(static_cast<int>(std::lround(s.curY)));
      a.set_width(blockW);
      a.set_height(static_cast<int>(kCircleSlot));
      s.block->size_allocate(a, -1);
    }
  }

 private:
  struct Slot {
    std::string id;
    ContractBlock* block = nullptr;
    double curY = 0, fromY = 0, toY = 0;
    gint64 moveStart = 0;
    bool moving = false;
    bool entering = false;
    gint64 enterStart = 0;
    bool leaving = false;
    gint64 leaveStart = 0;
    double xOffset = 0;
  };

  double Offscreen() const {
    const double distance = kCircleSlot * 4;
    return edge_ == EjectEdge::Leading ? -distance : distance;
  }
  bool AnyLeaving() const {
    for (const auto& s : slots_)
      if (s.leaving) return true;
    return false;
  }
  const StackEntry* TruthById(const std::string& id) const {
    for (const auto& e : truth_)
      if (e.id == id) return &e;
    return nullptr;
  }
  void ReportMax(int64_t m) {
    if (m == reportedMax_) return;
    reportedMax_ = m;
    if (onStackMax_) onStackMax_(m);
  }
  void EnsureTick() {
    if (ticking_) return;
    ticking_ = true;
    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool { return OnTick(); });
  }

  // rebuild slots_ from truth order, reusing blocks by id and creating arrivals;
  // returns whether anything moved/appeared/left (i.e. an animation is needed)
  bool BuildFromTruth(int64_t stackMax, bool animate) {
    const gint64 now = g_get_monotonic_time();
    bool changed = false;
    std::vector<Slot> next;
    next.reserve(truth_.size());
    for (size_t i = 0; i < truth_.size(); ++i) {
      const StackEntry& e = truth_[i];
      const double targetY = static_cast<double>(i) * kPitch;
      auto it = std::find_if(slots_.begin(), slots_.end(),
                             [&](const Slot& s) { return !s.leaving && s.id == e.id; });
      if (it != slots_.end()) {
        Slot s = std::move(*it);
        slots_.erase(it);
        s.block->SetEntry(e.used, e.total, e.bitRate, stackMax);
        s.toY = targetY;
        if (!animate) {
          s.curY = targetY;
          s.moving = false;
        } else if (s.curY != targetY) {
          s.fromY = s.curY;
          s.moveStart = now;
          s.moving = true;
          changed = true;
        }
        next.push_back(std::move(s));
      } else {
        Slot s;
        s.id = e.id;
        s.block = Gtk::make_managed<ContractBlock>(e.id, color_, mirrored_, e.hasStream);
        s.block->set_parent(*this);
        s.block->SetEntry(e.used, e.total, e.bitRate, stackMax);
        s.curY = s.toY = targetY;
        if (animate) {
          s.entering = true;
          s.enterStart = now;
          s.block->set_opacity(0.0);
        }
        next.push_back(std::move(s));
        changed = true;
      }
    }
    // any non-reused leftovers vanished without a Closed tombstone: drop them
    for (auto& s : slots_) {
      s.block->unparent();
      changed = true;
    }
    slots_ = std::move(next);
    return changed;
  }

  void Sync() {
    if (!haveTruth_) return;

    // effective stack max over the displayed contracts (truth + still-leaving),
    // so survivors rescale in the settle rather than mid-slide (apple stackMax)
    int64_t stackMax = 0;
    for (const auto& e : truth_) stackMax = std::max(stackMax, e.total);
    for (const auto& s : slots_)
      if (s.leaving) stackMax = std::max(stackMax, s.block->Total());

    // push live values to survivors -- eases even during a slide-off
    for (auto& s : slots_) {
      if (s.leaving) continue;
      if (const StackEntry* e = TruthById(s.id))
        s.block->SetEntry(e->used, e->total, e->bitRate, stackMax);
    }

    // first population seeds settled (no entrance animation); the row itself fades in
    if (!seeded_) {
      seeded_ = true;
      BuildFromTruth(stackMax, /*animate=*/false);
      ReportMax(stackMax);
      lastHeight_ = -1;
      queue_resize();
      return;
    }

    // mark new departures; hold the layout while any leaver is still sliding
    const gint64 now = g_get_monotonic_time();
    std::set<std::string> truthIds;
    for (const auto& e : truth_) truthIds.insert(e.id);
    bool newDeparture = false;
    for (auto& s : slots_) {
      if (!s.leaving && !truthIds.count(s.id)) {
        s.leaving = true;
        s.leaveStart = now;
        newDeparture = true;
      }
    }
    if (AnyLeaving()) {
      if (newDeparture) EnsureTick();
      ReportMax(stackMax);
      return;  // arrivals + resettle wait for the leavers to clear (OnTick -> Sync)
    }

    // phase 2 / fast path: admit arrivals at the top, settle to the truth order
    const bool changed = BuildFromTruth(stackMax, /*animate=*/true);
    ReportMax(stackMax);
    if (changed) {
      EnsureTick();
      queue_resize();
    }
  }

  bool OnTick() {
    const gint64 now = g_get_monotonic_time();
    bool active = false;

    // leavers slide off (phase 1)
    bool anyLeaving = false, allLeaversDone = true;
    for (auto& s : slots_) {
      if (!s.leaving) continue;
      anyLeaving = true;
      const double p = std::clamp(static_cast<double>(now - s.leaveStart) / kSlideUs, 0.0, 1.0);
      const double e = EaseInOut(p);
      s.xOffset = e * Offscreen();
      s.block->set_opacity(1.0 - e);
      if (p < 1.0) {
        active = true;
        allLeaversDone = false;
      }
    }

    // vertical settle
    for (auto& s : slots_) {
      if (!s.moving) continue;
      const double p = std::clamp(static_cast<double>(now - s.moveStart) / kSettleUs, 0.0, 1.0);
      s.curY = Lerp(s.fromY, s.toY, EaseOutCubic(p));
      if (p < 1.0)
        active = true;
      else {
        s.moving = false;
        s.curY = s.toY;
      }
    }

    // arrival fade-in
    for (auto& s : slots_) {
      if (!s.entering) continue;
      const double p = std::clamp(static_cast<double>(now - s.enterStart) / kEnterUs, 0.0, 1.0);
      s.block->set_opacity(EaseOutCubic(p));
      if (p < 1.0)
        active = true;
      else
        s.entering = false;
    }

    // phase-1 completion: drop the cleared leavers, then settle to the truth
    if (anyLeaving && allLeaversDone) {
      for (auto it = slots_.begin(); it != slots_.end();) {
        if (it->leaving) {
          it->block->unparent();
          it = slots_.erase(it);
        } else {
          ++it;
        }
      }
      Sync();       // admit arrivals + settle survivors to the new order
      active = true;
    }

    // relayout; height follows the settle so the parent shrinks/grows smoothly
    double bottom = 0;
    for (const auto& s : slots_) bottom = std::max(bottom, s.curY + kCircleSlot);
    const int h = static_cast<int>(std::ceil(bottom));
    if (h != lastHeight_) {
      lastHeight_ = h;
      queue_resize();
    } else {
      queue_allocate();
    }

    if (!active) ticking_ = false;
    return active;
  }

  Rgba color_;
  bool mirrored_;
  EjectEdge edge_;
  std::vector<Slot> slots_;
  std::vector<StackEntry> truth_;  // latest desired, newest first
  bool haveTruth_ = false;
  bool seeded_ = false;
  bool ticking_ = false;
  int64_t reportedMax_ = -1;
  int lastHeight_ = -1;
  std::function<void(int64_t)> onStackMax_;
};

}  // namespace

// One direction's column: the header (title, arrow, summed bit rate -- ordered so
// the rate lands over the circle column), the animated pile, and the "max N"
// scale anchor. Send reads title-arrow-rate and hugs the row center from the
// left; receive reads rate-arrow-title and hugs it from the right.
class ContractStackView : public Gtk::Box {
 public:
  ContractStackView(const Rgba& color, bool mirrored, const std::string& title, EjectEdge edge)
      : Gtk::Box(Gtk::Orientation::VERTICAL, 12) {
    set_valign(Gtk::Align::START);
    set_hexpand(true);
    set_halign(mirrored ? Gtk::Align::END : Gtk::Align::START);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
    header->set_halign(mirrored ? Gtk::Align::END : Gtk::Align::START);
    auto* titleLabel = Gtk::make_managed<Gtk::Label>(title);
    titleLabel->add_css_class("ur-caption-11");
    titleLabel->add_css_class("dim-label");
    auto* arrow = Gtk::make_managed<Gtk::Label>(mirrored ? "→" : "←");  // -> / <-
    arrow->add_css_class(mirrored ? "ur-fg-green" : "ur-fg-pink");
    arrow->add_css_class("ur-caption-11");
    rate_ = Gtk::make_managed<Gtk::Label>();
    rate_->add_css_class(mirrored ? "ur-fg-green" : "ur-fg-pink");
    rate_->add_css_class("ur-caption-10");
    if (mirrored) {
      header->append(*titleLabel);
      header->append(*arrow);
      header->append(*rate_);
    } else {
      header->append(*rate_);
      header->append(*arrow);
      header->append(*titleLabel);
    }
    append(*header);

    pile_ = Gtk::make_managed<ContractStack>(color, mirrored, edge);
    pile_->SetStackMaxCallback([this](int64_t m) { UpdateMax(m); });
    append(*pile_);

    max_ = Gtk::make_managed<Gtk::Label>();
    max_->add_css_class("ur-caption-10");
    max_->add_css_class("ur-label-faint");
    max_->set_xalign(mirrored ? 1.0 : 0.0);
    max_->set_halign(mirrored ? Gtk::Align::END : Gtk::Align::START);
    max_->set_opacity(0);  // holds height; shown once the stack has a max
    append(*max_);
  }

  void SetEntries(const std::vector<StackEntry>& entries, int64_t byteCount) {
    // header run total: cumulative bytes moved on this stack since the peer last
    // went idle
    rate_->set_text(0 < byteCount ? FormatByteCountCompact(byteCount) : "");
    pile_->SetEntries(entries);
  }

 private:
  void UpdateMax(int64_t m) {
    if (0 < m) {
      max_->set_text(Format(T_("contract_stack_max", "max {}"), FormatByteCountCompact(m)));
      max_->set_opacity(1);
    } else {
      max_->set_opacity(0);
    }
  }

  ContractStack* pile_ = nullptr;
  Gtk::Label* rate_ = nullptr;
  Gtk::Label* max_ = nullptr;
};

// ---- ContractsSheet ---------------------------------------------------------

ContractsSheet::ContractsSheet(Gtk::Window& parent, SdkHost& host, ContractDetailsMode mode)
    : host_(host), mode_(mode) {
  EnsureDrawerCss();
  set_title(mode_ == ContractDetailsMode::Provider
                ? T_("provider_contracts", "Provider contracts")
                : T_("client_contracts", "Client contracts"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 560);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  // AdwToastOverlay (C API) hosts the copied-to-clipboard toast
  toastOverlay_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  gtk_window_set_child(GTK_WINDOW(gobj()), GTK_WIDGET(toastOverlay_));

  // the overlay carries the scroller plus the floating "N new" chip
  overlay_ = Gtk::make_managed<Gtk::Overlay>();
  adw_toast_overlay_set_child(toastOverlay_, GTK_WIDGET(overlay_->gobj()));

  scroller_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  overlay_->set_child(*scroller_);

  listBox_.set_margin(16);
  listBox_.set_valign(Gtk::Align::START);

  // empty state
  auto* empty = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  empty->set_valign(Gtk::Align::CENTER);
  empty->set_halign(Gtk::Align::CENTER);
  auto* emptyTitle = Gtk::make_managed<Gtk::Label>(T_("no_open_contracts", "No open contracts"));
  emptyTitle->add_css_class("dim-label");
  empty->append(*emptyTitle);
  auto* emptyDetail = Gtk::make_managed<Gtk::Label>(
      mode_ == ContractDetailsMode::Provider
          ? T_("contracts_appear_providing", "Contracts appear here while providing.")
          : T_("contracts_appear_connected", "Contracts appear here while connected."));
  emptyDetail->add_css_class("dim-label");
  emptyDetail->add_css_class("caption");
  empty->append(*emptyDetail);

  stack_.add(*empty, "empty");
  stack_.add(listBox_, "list");
  stack_.set_vexpand(true);
  scroller_->set_child(stack_);

  // the floating "N new" chip (shown only while scrolled away from the top)
  chipButton_ = Gtk::make_managed<Gtk::Button>();
  chipButton_->add_css_class("ur-newchip");
  chipButton_->set_halign(Gtk::Align::CENTER);
  chipButton_->set_valign(Gtk::Align::START);
  chipButton_->set_margin_top(8);
  chipButton_->set_visible(false);
  {
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    auto* up = Gtk::make_managed<Gtk::Image>();
    up->set_from_icon_name("go-up-symbolic");
    up->set_pixel_size(10);
    content->append(*up);
    chipLabel_ = Gtk::make_managed<Gtk::Label>();
    content->append(*chipLabel_);
    chipButton_->set_child(*content);
  }
  chipButton_->signal_clicked().connect([this] {
    // report we're back at the top -- the SDK VC merges the collected rows + re-sorts
    // and fires the rows listener -- and scroll back to the top
    ReportAtTop(true);
    if (auto v = scroller_->get_vadjustment()) v->set_value(0);
  });
  overlay_->add_overlay(*chipButton_);

  // scroll gating: report the scroll position to the SDK VC, which owns the
  // ordering -- at the top it re-sorts active rows above idle ones; scrolled away
  // it freezes membership + order and new rows collect behind the chip
  if (auto v = scroller_->get_vadjustment()) {
    v->signal_value_changed().connect([this] {
      auto adj = scroller_->get_vadjustment();
      ReportAtTop(!adj || adj->get_value() <= 4.0);
    });
  }
}

ContractsSheet::~ContractsSheet() = default;

void ContractsSheet::Open() {
  isAtTop_ = true;
  host_.SetContractsAtTop(true);  // a freshly opened list is at the top
  Refresh();
  present();
  if (auto v = scroller_->get_vadjustment()) v->set_value(0);
}

std::vector<ContractsSheet::PeerRowVM> ContractsSheet::ReadRows() {
  // The SDK ContractDetailsViewController already grouped per peer (direction
  // resolved), ran the closing/eject lifecycle, throttled the change stream AND
  // produced the FINAL display order (activity sort + scrolled-away freeze) -- just
  // map its rows onto the render type, in order.
  std::vector<PeerRowVM> rows;
  auto list = host_.ContractRows();
  if (!list) return rows;
  rows.reserve(list->size());
  auto mapEntries = [](const std::optional<urnet::ContractEntryList>& src) {
    std::vector<EntryVM> out;
    if (!src) return out;
    out.reserve(src->size());
    for (const auto& e : *src)
      out.push_back({e.ContractId, e.UsedByteCount, e.TotalByteCount, e.BitRate, e.HasStream});
    return out;
  };
  for (const auto& r : *list) {
    PeerRowVM row;
    row.clientId = r.ClientId;
    row.send = mapEntries(r.SendContracts);
    row.receive = mapEntries(r.ReceiveContracts);
    row.sendByteCount = r.SendByteCount;
    row.receiveByteCount = r.ReceiveByteCount;
    rows.push_back(std::move(row));
  }
  return rows;
}

void ContractsSheet::Refresh() {
  rows_ = ReadRows();
  ApplyRows();
  UpdateChip();
}

void ContractsSheet::ApplyRows() {
  // the SDK owns membership + order (frozen while scrolled away): render rows_ as
  // handed. Reconcile the list widgets to it -- fade out rows that are gone, build
  // arrivals, push live values, and order the list to match.
  stack_.set_visible_child(rows_.empty() ? "empty" : "list");

  std::set<std::string> targetSet;
  for (const auto& r : rows_) targetSet.insert(r.clientId);

  // fade out + drop rows no longer present
  std::vector<std::string> gone;
  for (auto& [id, w] : rowWidgets_)
    if (!targetSet.count(id)) gone.push_back(id);
  for (const auto& id : gone) {
    Gtk::Widget* container = rowWidgets_[id].container;
    rowWidgets_.erase(id);
    FadeOutAndRemove(container);
  }

  // ensure a widget per row, update its values, and order the list to match
  Gtk::Widget* prev = nullptr;
  for (const auto& row : rows_) {
    auto it = rowWidgets_.find(row.clientId);
    if (it == rowWidgets_.end()) {
      RowWidgets w;
      Gtk::Widget* container = BuildRow(row, w);
      listBox_.append(*container);
      it = rowWidgets_.emplace(row.clientId, w).first;
    }
    UpdateRow(row, it->second);
    gtk_box_reorder_child_after(listBox_.gobj(), GTK_WIDGET(it->second.container->gobj()),
                                prev ? GTK_WIDGET(prev->gobj()) : nullptr);
    prev = it->second.container;
  }
}

Gtk::Widget* ContractsSheet::BuildRow(const PeerRowVM& row, RowWidgets& out) {
  // container = the row content + a trailing divider, so a reorder moves both
  auto* container = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  rowBox->set_margin_top(16);
  rowBox->set_margin_bottom(16);

  // the full client id, click to copy
  auto* clientId = Gtk::make_managed<Gtk::Label>(row.clientId);
  clientId->add_css_class("ur-mono-13");
  clientId->set_xalign(0);
  clientId->set_hexpand(true);
  clientId->set_halign(Gtk::Align::FILL);
  clientId->set_wrap(true);
  clientId->set_wrap_mode(Pango::WrapMode::CHAR);
  SetPointerCursor(*clientId);
  auto gesture = Gtk::GestureClick::create();
  const std::string cid = row.clientId;
  gesture->signal_released().connect([this, cid](int, double, double) { CopyClientId(cid); });
  clientId->add_controller(gesture);
  rowBox->append(*clientId);

  // the two stacks: four columns mirrored around the center. Each stack takes
  // half the width, send hugging the center from the left, receive from the right
  auto* viz = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
  out.send = Gtk::make_managed<ContractStackView>(kUrGreen, /*mirrored=*/true, T_("send", "Send"),
                                                  EjectEdge::Leading);
  out.receive = Gtk::make_managed<ContractStackView>(
      kUrPink, /*mirrored=*/false, T_("receive", "Receive"), EjectEdge::Trailing);
  viz->append(*out.send);
  viz->append(*out.receive);
  rowBox->append(*viz);

  container->append(*rowBox);
  container->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
  out.container = container;

  // a new peer row fades in (the per-stack choreography handles its contracts)
  if (get_visible()) AnimateEntrance(*container, 0, 0, 250);
  return container;
}

void ContractsSheet::UpdateRow(const PeerRowVM& row, RowWidgets& w) {
  auto toStack = [](const std::vector<EntryVM>& in) {
    std::vector<StackEntry> out;
    out.reserve(in.size());
    for (const auto& e : in) out.push_back({e.id, e.used, e.total, e.bitRate, e.hasStream});
    return out;
  };
  w.send->SetEntries(toStack(row.send), row.sendByteCount);
  w.receive->SetEntries(toStack(row.receive), row.receiveByteCount);
}

void ContractsSheet::UpdateChip() {
  // the SDK VC owns the "N new" count (0 at the top); the local at-top guard keeps
  // the chip hidden the instant we return to the top, before the merge lands
  const int pending = static_cast<int>(host_.ContractsPendingCount());
  const bool show = !isAtTop_ && 0 < pending;
  chipButton_->set_visible(show);
  if (show)
    chipLabel_->set_text(Format(TN_("new_items_count", "{} new", "{} new", pending), pending));
}

void ContractsSheet::ReportAtTop(bool atTop) {
  if (atTop == isAtTop_) return;
  isAtTop_ = atTop;
  // the SDK VC owns the ordering: at the top it re-sorts + merges the pending rows
  // and fires the rows listener (-> Refresh); scrolled away it freezes and collects
  host_.SetContractsAtTop(atTop);
  UpdateChip();
}

void ContractsSheet::FadeOutAndRemove(Gtk::Widget* container) {
  if (!container) return;
  auto start = std::make_shared<gint64>(-1);
  container->add_tick_callback(
      [this, container, start](const Glib::RefPtr<Gdk::FrameClock>& clock) -> bool {
        const gint64 now = clock->get_frame_time();
        if (*start < 0) *start = now;
        const double p = std::min(1.0, static_cast<double>(now - *start) / kRowFadeUs);
        container->set_opacity(1.0 - p);
        if (p < 1.0) return true;
        listBox_.remove(*container);
        return false;
      });
}

void ContractsSheet::CopyClientId(const std::string& clientId) {
  get_clipboard()->set_text(clientId);
  adw_toast_overlay_add_toast(toastOverlay_,
                              adw_toast_new(T_("client_id_copied", "Client ID copied")));
}

}  // namespace urnw
