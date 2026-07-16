// SPDX-License-Identifier: MPL-2.0
#include "ContractsSheet.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr double kCircleSize = 56;

// which way a replaced/closing ring ejects: the contract (green) ring slides off
// the leading edge, the companion (pink) ring off the trailing edge (apple
// ContractPairViz removalEdge: .leading / .trailing).
enum class EjectEdge { Leading, Trailing };

void SetCairoColor(const Cairo::RefPtr<Cairo::Context>& cr, const Rgba& c) {
  cr->set_source_rgba(c.r, c.g, c.b, c.a);
}

// symmetric ease (apple .easeInOut) for the ring slide-out + fade
double EaseInOut(double t) {
  const double p = std::clamp(t, 0.0, 1.0);
  return p * p * (3 - 2 * p);
}

// the bare identity ring at the color's 0.8 base alpha; used to draw an ejecting
// copy (its widget opacity carries the fade).
void DrawRing(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height, const Rgba& color) {
  const double cx = width / 2.0;
  const double cy = height / 2.0;
  SetCairoColor(cr, color.WithAlpha(0.8));
  cr->set_line_width(1);
  cr->arc(cx, cy, kCircleSize / 2 - 0.5, 0, 2 * G_PI);
  cr->stroke();
}

}  // namespace

// The settled circle occupying the slot: its identity ring (alpha driven by the
// container's fade-in) plus the area-proportional inner usage disc. The disc
// persists across a contract swap and just resizes -- it never ejects. Pure
// drawer: the container (ContractCircle) computes the eased ring alpha and disc
// fraction each frame and pushes them here.
class CurrentCircle : public Gtk::DrawingArea {
 public:
  explicit CurrentCircle(const Rgba& color) : color_(color) {
    set_content_width(static_cast<int>(kCircleSize));
    set_content_height(static_cast<int>(kCircleSize));
    set_draw_func(sigc::mem_fun(*this, &CurrentCircle::OnDraw));
  }

  void SetRingAlpha(double alpha) {
    if (alpha == ringAlpha_) return;
    ringAlpha_ = alpha;
    queue_draw();
  }
  void SetFraction(double fraction) {
    if (fraction == fraction_) return;
    fraction_ = fraction;
    queue_draw();
  }

 private:
  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    const double cx = width / 2.0;
    const double cy = height / 2.0;

    // identity ring (0.8 base alpha, faded in via ringAlpha_)
    if (0 < ringAlpha_) {
      SetCairoColor(cr, color_.WithAlpha(0.8 * ringAlpha_));
      cr->set_line_width(1);
      cr->arc(cx, cy, kCircleSize / 2 - 0.5, 0, 2 * G_PI);
      cr->stroke();
    }

    // area-proportional inner disc, with a minimum visible size
    const double innerSize = 0 < fraction_ ? std::max(6.0, kCircleSize * std::sqrt(fraction_)) : 0;
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
  double ringAlpha_ = 0;  // 0..1, scales the ring's 0.8 base alpha
  double fraction_ = 0;   // 0..1 usage, area-proportional inner disc
};

// A contract circle that animates like the apple ContractRing. The identity ring
// carries the contract-id signature: when it changes, the on-screen ring is
// EJECTED -- an independent copy slides off toward the eject edge and fades on
// its own fixed schedule, never reversing; multiple can be leaving at once, and
// the new ring only fades into the slot once the LAST ejecting copy has left.
// A closing row (its last contract gone) ejects its ring and shows nothing
// after. The inner usage disc lives in the settled circle and never ejects; it
// just resizes. All of it is driven off one frame-clock tick.
//
// It is a container (not a DrawingArea) so an ejecting ring can slide clear of
// the 56px slot and over its neighbors: children draw with overflow visible and
// are clipped only by the scroll viewport, i.e. off the side of the row.
class ContractCircle : public Gtk::Widget {
 public:
  ContractCircle(const Rgba& color, EjectEdge edge)
      // register a distinct GType so the measure/size-allocate vfunc overrides
      // are installed (required for a custom Gtk::Widget container)
      : Glib::ObjectBase("UrnwContractCircle"), Gtk::Widget(), color_(color), edge_(edge) {
    set_overflow(Gtk::Overflow::VISIBLE);
    set_halign(Gtk::Align::CENTER);
    set_valign(Gtk::Align::START);
    current_ = Gtk::make_managed<CurrentCircle>(color);
    current_->set_parent(*this);
  }

  ~ContractCircle() override {
    for (auto& e : ejections_) e.widget->unparent();
    ejections_.clear();
    if (current_) current_->unparent();
  }

  // usage disc target; eases over 0.5s (mac parity). Never ejects.
  void SetData(int64_t used, int64_t total) {
    const double target =
        0 < total ? std::min(1.0, static_cast<double>(used) / static_cast<double>(total)) : 0;
    if (target == discTo_) return;
    const gint64 now = g_get_monotonic_time();
    discFrom_ = CurrentFraction(now);
    discTo_ = target;
    discStart_ = now;
    discAnimating_ = true;
    EnsureTick();
  }

  // contract-id signature + whether a ring is wanted (false while the row is
  // closing). Mirrors apple ContractRing.onChange(contractId)/onChange(visible).
  void SetContract(const std::string& contractId, bool visible) {
    const gint64 now = g_get_monotonic_time();

    if (!haveId_) {
      // first data: occupy the slot immediately, no eject/fade
      haveId_ = true;
      currentId_ = contractId;
      present_ = visible;
      settledVisible_ = visible;
      settledAlpha_ = visible ? 1.0 : 0.0;
      current_->SetRingAlpha(settledAlpha_);
      return;
    }

    if (contractId != currentId_) {
      if (settledVisible_) EjectCurrentRing(now);
      currentId_ = contractId;
      settledVisible_ = false;
      settledAlpha_ = 0.0;
      current_->SetRingAlpha(0.0);
      // nothing leaving -> the new ring can fade in right away
      if (present_ && ejections_.empty()) FadeInCurrent(now);
    }

    if (visible != present_) {
      present_ = visible;
      if (visible) {
        if (!settledVisible_ && ejections_.empty()) FadeInCurrent(now);
      } else if (settledVisible_) {
        // the row is closing: eject the ring and show nothing after
        EjectCurrentRing(now);
        settledVisible_ = false;
        settledAlpha_ = 0.0;
        current_->SetRingAlpha(0.0);
      }
    }
  }

 protected:
  // fixed 56x56 slot regardless of the ejecting children (which overflow it),
  // so adding/removing an ejection never disturbs the row layout
  void measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural,
                     int& minimum_baseline, int& natural_baseline) const override {
    minimum = natural = static_cast<int>(kCircleSize);
    minimum_baseline = natural_baseline = -1;
  }

  void size_allocate_vfunc(int width, int height, int /*baseline*/) override {
    const int size = static_cast<int>(kCircleSize);
    const int baseX = (width - size) / 2;
    const int baseY = (height - size) / 2;
    Gtk::Allocation a;
    a.set_width(size);
    a.set_height(size);
    a.set_y(baseY);
    a.set_x(baseX);
    current_->size_allocate(a, -1);
    for (auto& e : ejections_) {
      Gtk::Allocation ea;
      ea.set_width(size);
      ea.set_height(size);
      ea.set_y(baseY);
      ea.set_x(baseX + static_cast<int>(std::lround(e.offset)));
      e.widget->size_allocate(ea, -1);
    }
  }

 private:
  static constexpr gint64 kDiscEaseUs = 500000;  // 0.5s inner-disc ease (mac parity)
  static constexpr gint64 kSlideUs = 500000;     // 0.5s ring slide-out (apple slideDuration)
  static constexpr gint64 kFadeInUs = 350000;    // 0.35s ring fade-in (apple fadeInDuration)

  // one in-flight ejection: an independent slide-out + fade that runs once
  struct Ejection {
    Gtk::DrawingArea* widget = nullptr;
    gint64 start = 0;
    double offset = 0;
  };

  double CurrentFraction(gint64 now) const {
    const double progress =
        std::clamp(static_cast<double>(now - discStart_) / kDiscEaseUs, 0.0, 1.0);
    const double eased = 1 - std::pow(1 - progress, 3);  // easeOut cubic
    return discFrom_ + (discTo_ - discFrom_) * eased;
  }

  double OffscreenOffset() const {
    const double distance = kCircleSize * 3;  // slide fully clear of the slot
    return edge_ == EjectEdge::Leading ? -distance : distance;
  }

  void FadeInCurrent(gint64 now) {
    settledVisible_ = true;
    fadeFrom_ = settledAlpha_;
    fadeStart_ = now;
    fading_ = true;
    EnsureTick();
  }

  // spawn an independent slide-out of the on-screen ring; once started it always
  // runs to completion (never reverses), even if more changes land meanwhile
  void EjectCurrentRing(gint64 now) {
    auto* ring = Gtk::make_managed<Gtk::DrawingArea>();
    ring->set_content_width(static_cast<int>(kCircleSize));
    ring->set_content_height(static_cast<int>(kCircleSize));
    const Rgba color = color_;
    ring->set_draw_func([color](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
      DrawRing(cr, w, h, color);
    });
    ring->set_parent(*this);
    ejections_.push_back({ring, now, 0.0});
    queue_allocate();
    EnsureTick();
  }

  void EnsureTick() {
    if (ticking_) return;
    ticking_ = true;
    add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool { return OnTick(); });
  }

  bool OnTick() {
    const gint64 now = g_get_monotonic_time();
    bool active = false;

    // inner usage disc ease
    if (discAnimating_) {
      current_->SetFraction(CurrentFraction(now));
      if (kDiscEaseUs <= now - discStart_) {
        discAnimating_ = false;
        current_->SetFraction(discTo_);
      } else {
        active = true;
      }
    }

    // settled identity ring fade-in
    if (fading_) {
      const double p = std::clamp(static_cast<double>(now - fadeStart_) / kFadeInUs, 0.0, 1.0);
      settledAlpha_ = fadeFrom_ + (1.0 - fadeFrom_) * EaseInOut(p);
      current_->SetRingAlpha(settledAlpha_);
      if (1.0 <= p) {
        fading_ = false;
        settledAlpha_ = 1.0;
        current_->SetRingAlpha(1.0);
      } else {
        active = true;
      }
    }

    // ejecting rings: each slides + fades on its own schedule
    bool removed = false;
    const bool hadEjections = !ejections_.empty();
    for (auto it = ejections_.begin(); it != ejections_.end();) {
      const double p = std::clamp(static_cast<double>(now - it->start) / kSlideUs, 0.0, 1.0);
      const double eased = EaseInOut(p);
      it->offset = eased * OffscreenOffset();
      it->widget->set_opacity(1.0 - eased);
      if (1.0 <= p) {
        it->widget->unparent();  // destroys the managed ring
        it = ejections_.erase(it);
        removed = true;
      } else {
        active = true;
        ++it;
      }
    }
    if (hadEjections) queue_allocate();  // reposition remaining / clear removed
    if (removed && ejections_.empty() && present_ && !settledVisible_) {
      // the last ejection left -> admit the waiting ring
      FadeInCurrent(now);
      active = true;
    }

    if (!active) ticking_ = false;
    return active;
  }

  Rgba color_;
  EjectEdge edge_;
  CurrentCircle* current_ = nullptr;
  std::vector<Ejection> ejections_;
  bool ticking_ = false;

  // inner usage disc easing
  double discFrom_ = 0;
  double discTo_ = 0;
  gint64 discStart_ = 0;
  bool discAnimating_ = false;

  // identity ring state (apple ContractRing)
  std::string currentId_;      // contract-id signature occupying the slot
  bool haveId_ = false;        // whether the slot has been seeded yet
  bool present_ = true;        // a ring is wanted in the slot (mirror `visible`)
  bool settledVisible_ = false;// the settled ring has faded in
  double settledAlpha_ = 0;    // 0..1 animated settled-ring alpha
  double fadeFrom_ = 0;        // settledAlpha_ at the start of a fade-in
  gint64 fadeStart_ = 0;
  bool fading_ = false;
};

// The two directional transfer lines between the circles: contract (green)
// pointing right on top, companion (pink) pointing left below; each labeled
// with its bit rate while active, dimmed while idle.
class TransferLines : public Gtk::DrawingArea {
 public:
  TransferLines() {
    set_content_height(static_cast<int>(kCircleSize));
    set_hexpand(true);
    set_valign(Gtk::Align::START);
    set_draw_func(sigc::mem_fun(*this, &TransferLines::OnDraw));
  }

  void SetRates(int64_t contractBitRate, int64_t companionBitRate) {
    if (contractBitRate == contractBitRate_ && companionBitRate == companionBitRate_) return;
    contractBitRate_ = contractBitRate;
    companionBitRate_ = companionBitRate;
    queue_draw();
  }

 private:
  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    auto drawLine = [&](double y, int64_t bitRate, const Rgba& color, bool pointsRight) {
      const bool active = 0 < bitRate;
      if (active) {
        const std::string label = FormatBitRate(bitRate);
        cr->select_font_face("monospace", Cairo::ToyFontFace::Slant::NORMAL,
                             Cairo::ToyFontFace::Weight::NORMAL);
        cr->set_font_size(9);
        Cairo::TextExtents te;
        cr->get_text_extents(label, te);
        SetCairoColor(cr, color);
        cr->move_to((width - te.x_advance) / 2, y - 5);
        cr->show_text(label);
      }
      SetCairoColor(cr, color.WithAlpha(active ? 0.9 : 0.25));
      cr->set_line_width(1);
      const double arrow = 6;
      cr->move_to(pointsRight ? 0 : arrow - 1, y);
      cr->line_to(pointsRight ? width - arrow + 1 : width, y);
      cr->stroke();
      if (pointsRight) {
        cr->move_to(width, y);
        cr->line_to(width - arrow, y - arrow / 2);
        cr->line_to(width - arrow, y + arrow / 2);
      } else {
        cr->move_to(0, y);
        cr->line_to(arrow, y - arrow / 2);
        cr->line_to(arrow, y + arrow / 2);
      }
      cr->close_path();
      cr->fill();
    };
    drawLine(height * 0.4, contractBitRate_, kUrGreen, true);
    drawLine(height * 0.75, companionBitRate_, kUrPink, false);
  }

  int64_t contractBitRate_ = 0;
  int64_t companionBitRate_ = 0;
};

ContractsSheet::ContractsSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_("client_contracts", "Client contracts"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 540);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  // AdwToastOverlay (C API) hosts the copied-to-clipboard toast
  toastOverlay_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  gtk_window_set_child(GTK_WINDOW(gobj()), GTK_WIDGET(toastOverlay_));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  adw_toast_overlay_set_child(toastOverlay_, GTK_WIDGET(scroller->gobj()));

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
      T_("contracts_appear_connected", "Contracts appear here while connected."));
  emptyDetail->add_css_class("dim-label");
  emptyDetail->add_css_class("caption");
  empty->append(*emptyDetail);

  stack_.add(*empty, "empty");
  stack_.add(listBox_, "list");
  stack_.set_vexpand(true);
  scroller->set_child(stack_);
}

void ContractsSheet::Open() {
  Refresh();
  present();
}

std::vector<ContractsSheet::PeerRow> ContractsSheet::ReadRows() {
  // The SDK ContractDetailsViewController already coalesced, aggregated per peer,
  // and ordered these (newest first), and carries the closing flag -- just map
  // them onto the render type in order (mirrors apple ContractDetailsStore.update).
  std::vector<PeerRow> rows;
  const auto list = host_.ClientContractRows();
  if (!list) return rows;
  rows.reserve(list->size());
  for (const auto& r : *list) {
    PeerRow row;
    row.clientId = r.ClientId;
    row.contractId = r.ContractId;
    row.companionContractId = r.CompanionContractId;
    row.contractUsed = r.ContractUsedByteCount;
    row.contractTotal = r.ContractByteCount;
    row.contractBitRate = r.ContractBitRate;
    row.companionUsed = r.CompanionContractUsedByteCount;
    row.companionTotal = r.CompanionContractByteCount;
    row.companionBitRate = r.CompanionContractBitRate;
    row.pairCount = static_cast<int>(r.PairCount);
    row.closing = r.Closing;
    rows.push_back(std::move(row));
  }
  return rows;
}

void ContractsSheet::Refresh() {
  std::vector<PeerRow> rows = ReadRows();
  // the SDK settled this already; skip when nothing changed
  if (built_ && rows == rows_) return;

  std::vector<std::string> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) ids.push_back(row.clientId);

  if (built_ && ids == rowIds_) {
    // same peers, new data: update in place so the usage discs ease to their
    // new size instead of snapping through a rebuild
    rows_ = std::move(rows);
    for (const auto& row : rows_) UpdateRowWidgets(row, rowWidgets_[row.clientId]);
    return;
  }
  built_ = true;
  rows_ = std::move(rows);
  rowIds_ = std::move(ids);
  RebuildList();
}

void ContractsSheet::RebuildList() {
  RemoveAllChildren(listBox_);
  rowWidgets_.clear();
  if (rows_.empty()) {
    stack_.set_visible_child("empty");
    return;
  }
  stack_.set_visible_child("list");
  bool firstRow = true;
  unsigned stagger = 0;
  for (const auto& row : rows_) {
    if (!firstRow) {
      listBox_.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    }
    firstRow = false;
    RowWidgets widgets;
    Gtk::Widget* rowWidget = BuildRow(row, widgets);
    listBox_.append(*rowWidget);
    rowWidgets_[row.clientId] = widgets;
    // membership changed: fade the rows in rather than snapping (capped stagger)
    if (get_visible()) {
      AnimateEntrance(*rowWidget, std::min(stagger, 6u) * 40, 0, 250);
      ++stagger;
    }
  }
}

Gtk::Widget* ContractsSheet::BuildRow(const PeerRow& row, RowWidgets& outWidgets) {
  auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  rowBox->set_margin_top(16);
  rowBox->set_margin_bottom(16);

  // header: the full client id, click to copy (+ toast)
  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* clientIdLabel = Gtk::make_managed<Gtk::Label>(row.clientId);
  clientIdLabel->add_css_class("ur-mono-13");
  clientIdLabel->set_xalign(0);
  clientIdLabel->set_hexpand(true);
  clientIdLabel->set_wrap(true);
  clientIdLabel->set_wrap_mode(Pango::WrapMode::CHAR);
  SetPointerCursor(*clientIdLabel);
  auto copyGesture = Gtk::GestureClick::create();
  const std::string clientId = row.clientId;
  copyGesture->signal_released().connect(
      [this, clientId](int, double, double) { CopyClientId(clientId); });
  clientIdLabel->add_controller(copyGesture);
  header->append(*clientIdLabel);
  outWidgets.pairCount = Gtk::make_managed<Gtk::Label>();
  outWidgets.pairCount->add_css_class("dim-label");
  outWidgets.pairCount->add_css_class("caption");
  outWidgets.pairCount->set_valign(Gtk::Align::START);
  header->append(*outWidgets.pairCount);
  rowBox->append(*header);

  // viz: contract circle | transfer lines | companion circle
  auto* viz = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
  auto makeCircleColumn = [&](const Rgba& color, EjectEdge edge, const char* label,
                              ContractCircle*& outCircle, Gtk::Label*& outUsed,
                              Gtk::Label*& outTotal) -> Gtk::Widget* {
    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    column->set_size_request(92, -1);
    outCircle = Gtk::make_managed<ContractCircle>(color, edge);
    column->append(*outCircle);
    outUsed = Gtk::make_managed<Gtk::Label>();
    outUsed->add_css_class("ur-caption-11");
    column->append(*outUsed);
    outTotal = Gtk::make_managed<Gtk::Label>();
    outTotal->add_css_class("dim-label");
    outTotal->add_css_class("caption");
    column->append(*outTotal);
    auto* nameLabel = Gtk::make_managed<Gtk::Label>(label);
    nameLabel->add_css_class("dim-label");
    nameLabel->add_css_class("caption");
    column->append(*nameLabel);
    return column;
  };
  viz->append(*makeCircleColumn(kUrGreen, EjectEdge::Leading, T_("contract", "Contract"),
                                outWidgets.contractCircle, outWidgets.contractUsed,
                                outWidgets.contractTotal));
  outWidgets.lines = Gtk::make_managed<TransferLines>();
  viz->append(*outWidgets.lines);
  viz->append(*makeCircleColumn(kUrPink, EjectEdge::Trailing, T_("companion", "Companion"),
                                outWidgets.companionCircle, outWidgets.companionUsed,
                                outWidgets.companionTotal));
  rowBox->append(*viz);

  UpdateRowWidgets(row, outWidgets);
  return rowBox;
}

void ContractsSheet::UpdateRowWidgets(const PeerRow& row, RowWidgets& widgets) {
  widgets.pairCount->set_text(Format(T_("contract_count", "{} contracts"), row.pairCount));
  widgets.pairCount->set_visible(1 < row.pairCount);
  // a changed contract-id signature swaps (ejects) the ring; a closing row ejects
  // and shows nothing after. the usage disc just eases to its new size.
  const bool visible = !row.closing;
  widgets.contractCircle->SetContract(row.contractId, visible);
  widgets.contractCircle->SetData(row.contractUsed, row.contractTotal);
  widgets.contractUsed->set_text(FormatByteCountCompact(row.contractUsed));
  widgets.contractTotal->set_text(
      Format(T_("of_total", "of {}"), FormatByteCountCompact(row.contractTotal)));
  widgets.companionCircle->SetContract(row.companionContractId, visible);
  widgets.companionCircle->SetData(row.companionUsed, row.companionTotal);
  widgets.companionUsed->set_text(FormatByteCountCompact(row.companionUsed));
  widgets.companionTotal->set_text(
      Format(T_("of_total", "of {}"), FormatByteCountCompact(row.companionTotal)));
  widgets.lines->SetRates(row.contractBitRate, row.companionBitRate);
}

void ContractsSheet::CopyClientId(const std::string& clientId) {
  get_clipboard()->set_text(clientId);
  adw_toast_overlay_add_toast(toastOverlay_,
                              adw_toast_new(T_("client_id_copied", "Client ID copied")));
}

}  // namespace urnw
