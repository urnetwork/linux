// SPDX-License-Identifier: MPL-2.0
#include "ContractsSheet.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr double kCircleSize = 56;

// the peer end of the contract transfer path: the side that is not this
// client, with destination -> source -> contract id fallbacks. The "unknown"
// sentinel stands in for a missing id and is not translated, matching the
// android (ContractStatsViewModel) and apple (ContractDetailsStore) apps.
std::string PeerClientId(const urnet::ContractDetails& details, const std::string& ownClientId) {
  if (details.ContractTransferPath) {
    const auto& sourceId = details.ContractTransferPath->SourceId;
    const auto& destinationId = details.ContractTransferPath->DestinationId;
    if (!ownClientId.empty()) {
      if (sourceId && *sourceId == ownClientId && destinationId) return *destinationId;
      if (destinationId && *destinationId == ownClientId && sourceId) return *sourceId;
    }
    if (destinationId) return *destinationId;
    if (sourceId) return *sourceId;
  }
  if (details.ContractId) return *details.ContractId;
  return "unknown";
}

void SetCairoColor(const Cairo::RefPtr<Cairo::Context>& cr, const Rgba& c) {
  cr->set_source_rgba(c.r, c.g, c.b, c.a);
}

}  // namespace

// Outer contract ring with an area-proportional inner usage disc. Size changes
// ease over 0.5s (mac parity), frame-clock driven.
class ContractCircle : public Gtk::DrawingArea {
 public:
  explicit ContractCircle(const Rgba& color) : color_(color) {
    set_content_width(static_cast<int>(kCircleSize));
    set_content_height(static_cast<int>(kCircleSize));
    set_halign(Gtk::Align::CENTER);
    set_draw_func(sigc::mem_fun(*this, &ContractCircle::OnDraw));
  }

  void SetData(int64_t used, int64_t total) {
    const double target =
        0 < total ? std::min(1.0, static_cast<double>(used) / static_cast<double>(total)) : 0;
    if (target == toFraction_) return;
    const gint64 now = g_get_monotonic_time();
    fromFraction_ = CurrentFraction(now);
    toFraction_ = target;
    easeStart_ = now;
    if (!animating_) {
      animating_ = true;
      add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
        queue_draw();
        if (kEaseUs <= g_get_monotonic_time() - easeStart_) {
          animating_ = false;
          return false;
        }
        return true;
      });
    }
    queue_draw();
  }

 private:
  static constexpr gint64 kEaseUs = 500000;  // 0.5s, matching the chart easing

  double CurrentFraction(gint64 now) const {
    const double progress =
        std::clamp(static_cast<double>(now - easeStart_) / kEaseUs, 0.0, 1.0);
    const double eased = 1 - std::pow(1 - progress, 3);
    return fromFraction_ + (toFraction_ - fromFraction_) * eased;
  }

  void OnDraw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    SetCairoColor(cr, color_.WithAlpha(0.8));
    cr->set_line_width(1);
    cr->arc(cx, cy, kCircleSize / 2 - 0.5, 0, 2 * G_PI);
    cr->stroke();

    // area-proportional inner disc, with a minimum visible size
    const double fraction = CurrentFraction(g_get_monotonic_time());
    const double innerSize = 0 < fraction ? std::max(6.0, kCircleSize * std::sqrt(fraction)) : 0;
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
  double fromFraction_ = 0;
  double toFraction_ = 0;
  gint64 easeStart_ = 0;
  bool animating_ = false;
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
  const std::string ownClientId = host_.ClientId();
  const auto egress = host_.EgressContractDetails();
  const auto ingress = host_.IngressContractDetails();

  std::unordered_map<std::string, PeerRow> rowsByClient;
  std::vector<std::string> encounterOrder;
  auto merge = [&](const std::optional<urnet::ContractDetailsList>& list) {
    if (!list) return;
    for (const auto& details : *list) {
      const std::string clientId = PeerClientId(details, ownClientId);
      auto it = rowsByClient.find(clientId);
      if (it == rowsByClient.end()) {
        PeerRow row;
        row.clientId = clientId;
        it = rowsByClient.emplace(clientId, std::move(row)).first;
        encounterOrder.push_back(clientId);
      }
      PeerRow& row = it->second;
      row.contractUsed += details.ContractUsedByteCount;
      row.contractTotal += details.ContractByteCount;
      row.contractBitRate += details.ContractBitRate;
      row.companionUsed += details.CompanionContractUsedByteCount;
      row.companionTotal += details.CompanionContractByteCount;
      row.companionBitRate += details.CompanionContractBitRate;
      row.pairCount += 1;
    }
  };
  merge(egress);
  merge(ingress);

  // newest first: newly seen clients sort to the top, existing clients keep
  // their first-seen relative order
  for (const auto& clientId : encounterOrder) {
    if (clientOrder_.find(clientId) == clientOrder_.end()) {
      clientOrder_[clientId] = static_cast<int>(clientOrder_.size());
    }
  }
  std::vector<PeerRow> rows;
  rows.reserve(rowsByClient.size());
  for (auto& entry : rowsByClient) rows.push_back(std::move(entry.second));
  std::sort(rows.begin(), rows.end(), [this](const PeerRow& a, const PeerRow& b) {
    return clientOrder_[a.clientId] > clientOrder_[b.clientId];
  });
  return rows;
}

void ContractsSheet::Refresh() {
  std::vector<PeerRow> rows = ReadRows();
  // both contract listeners funnel here; skip when nothing changed
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
  auto makeCircleColumn = [&](const Rgba& color, const char* label, ContractCircle*& outCircle,
                              Gtk::Label*& outUsed, Gtk::Label*& outTotal) -> Gtk::Widget* {
    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    column->set_size_request(92, -1);
    outCircle = Gtk::make_managed<ContractCircle>(color);
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
  viz->append(*makeCircleColumn(kUrGreen, T_("contract", "Contract"), outWidgets.contractCircle,
                                outWidgets.contractUsed, outWidgets.contractTotal));
  outWidgets.lines = Gtk::make_managed<TransferLines>();
  viz->append(*outWidgets.lines);
  viz->append(*makeCircleColumn(kUrPink, T_("companion", "Companion"), outWidgets.companionCircle,
                                outWidgets.companionUsed, outWidgets.companionTotal));
  rowBox->append(*viz);

  UpdateRowWidgets(row, outWidgets);
  return rowBox;
}

void ContractsSheet::UpdateRowWidgets(const PeerRow& row, RowWidgets& widgets) {
  widgets.pairCount->set_text(Format(T_("contract_count", "{} contracts"), row.pairCount));
  widgets.pairCount->set_visible(1 < row.pairCount);
  widgets.contractCircle->SetData(row.contractUsed, row.contractTotal);
  widgets.contractUsed->set_text(FormatByteCountCompact(row.contractUsed));
  widgets.contractTotal->set_text(
      Format(T_("of_total", "of {}"), FormatByteCountCompact(row.contractTotal)));
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
