// SPDX-License-Identifier: MPL-2.0
#include "PaneKit.hpp"

#include <gtk/gtk.h>

#include "Ui.hpp"

namespace urnw::kit {
namespace {

Gtk::Label* MakeStyledLabel(const Glib::ustring& text, const char* cssClass,
                            float xalign = 0.f, bool trim = true) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(xalign);
  if (trim) label->set_ellipsize(Pango::EllipsizeMode::END);
  label->set_valign(Gtk::Align::CENTER);
  return label;
}

// a colored ● in markup — the dot species every list/status row shares
Gtk::Label* MakeDot(int sizePx) {
  auto* dot = Gtk::make_managed<Gtk::Label>();
  dot->set_valign(Gtk::Align::CENTER);
  dot->set_markup("<span size='" + std::to_string(sizePx * PANGO_SCALE) +
                  "' foreground='#5A5A5A'>●</span>");
  MarkDecorative(*dot);
  return dot;
}

void PaintDot(Gtk::Label& dot, int sizePx, const std::string& colorHex) {
  dot.set_markup("<span size='" + std::to_string(sizePx * PANGO_SCALE) + "' foreground='" +
                 colorHex + "'>●</span>");
}

}  // namespace

void SetTextOrCollapse(Gtk::Label& line, const Glib::ustring& text) {
  line.set_text(text);
  line.set_visible(!text.empty());
}

void MarkDecorative(Gtk::Widget& widget) {
  gtk_accessible_update_state(GTK_ACCESSIBLE(widget.gobj()), GTK_ACCESSIBLE_STATE_HIDDEN,
                              TRUE, -1);
}

void SetAccessibleLabel(Gtk::Widget& widget, const Glib::ustring& label) {
  gtk_accessible_update_property(GTK_ACCESSIBLE(widget.gobj()),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
}

Gtk::Widget* MakeDivider() {
  auto* rule = Gtk::make_managed<Gtk::Box>();
  rule->add_css_class("ur-vrule");
  rule->set_size_request(-1, 1);
  rule->set_hexpand(true);
  return rule;
}

Gtk::Widget* MakePaneVRule() {
  auto* rule = Gtk::make_managed<Gtk::Box>();
  rule->add_css_class("ur-vrule");
  rule->set_size_request(1, -1);
  rule->set_vexpand(true);
  return rule;
}

Gtk::Widget* MakeSectionHeader(const char* iconName, const Glib::ustring& text) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name(iconName);
  icon->set_pixel_size(16);
  icon->add_css_class("dim-label");
  MarkDecorative(*icon);
  row->append(*icon);
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class("ur-body");
  label->set_xalign(0);
  Pango::AttrList attrs;
  auto weight = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
  attrs.insert(weight);
  auto size = Pango::Attribute::create_attr_size_absolute(16 * PANGO_SCALE);
  attrs.insert(size);
  label->set_attributes(attrs);
  row->append(*label);
  return row;
}

Gtk::Widget* MakeEmptyState(const char* iconName, const Glib::ustring& text) {
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  column->set_halign(Gtk::Align::CENTER);
  column->set_margin(24);
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name(iconName);
  icon->set_pixel_size(28);
  icon->add_css_class("ur-label-faint");
  icon->set_halign(Gtk::Align::CENTER);
  MarkDecorative(*icon);
  column->append(*icon);
  auto* line = Gtk::make_managed<Gtk::Label>(text);
  line->add_css_class("ur-caption");
  line->set_wrap(true);
  line->set_justify(Gtk::Justification::CENTER);
  line->set_max_width_chars(40);
  line->set_halign(Gtk::Align::CENTER);
  column->append(*line);
  return column;
}

Gtk::Widget* MakeEmptyStateCard(const char* iconName, const Glib::ustring& text) {
  auto* card = Gtk::make_managed<Gtk::Box>();
  card->add_css_class("ur-card");
  card->append(*MakeEmptyState(iconName, text));
  return card;
}

Gtk::Label* MakePaneEmptyLine(const Glib::ustring& text) {
  auto* line = Gtk::make_managed<Gtk::Label>(text);
  line->add_css_class("ur-caption");
  line->add_css_class("ur-label-faint");
  line->set_wrap(true);
  line->set_justify(Gtk::Justification::CENTER);
  line->set_max_width_chars(40);
  line->set_halign(Gtk::Align::CENTER);
  line->set_valign(Gtk::Align::CENTER);
  line->set_vexpand(true);
  line->set_margin(16);
  return line;
}

Gtk::Widget* MakePageHeader(const Glib::ustring& title, const Glib::ustring& description) {
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  column->set_valign(Gtk::Align::START);
  auto* titleBlock = Gtk::make_managed<Gtk::Label>(title);
  titleBlock->add_css_class("ur-heading");
  titleBlock->set_xalign(0);
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(28 * PANGO_SCALE);
  attrs.insert(size);
  titleBlock->set_attributes(attrs);
  column->append(*titleBlock);
  if (!description.empty()) {
    auto* desc = Gtk::make_managed<Gtk::Label>(description);
    desc->add_css_class("ur-caption");
    desc->set_xalign(0);
    desc->set_wrap(true);
    desc->set_max_width_chars(64);  // ~60ch reading measure
    column->append(*desc);
  }
  return column;
}

MetricCard MakeMetricCard(const Glib::ustring& label, const Glib::ustring& value) {
  MetricCard card;
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  box->add_css_class("ur-card");
  card.label = MakeStyledLabel(label, "ur-stat-label");
  box->append(*card.label);
  card.value = MakeStyledLabel(value, "ur-stat-value");
  box->append(*card.value);
  card.root = box;
  return card;
}

SettingsCard MakeSettingsCard(const char* iconName, const Glib::ustring& title,
                              const Glib::ustring& description) {
  SettingsCard card;
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  row->add_css_class("ur-card");
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name(iconName);
  icon->set_pixel_size(20);
  icon->add_css_class("dim-label");
  icon->set_valign(Gtk::Align::CENTER);
  MarkDecorative(*icon);
  row->append(*icon);
  auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  text->set_valign(Gtk::Align::CENTER);
  text->set_hexpand(true);
  card.title = Gtk::make_managed<Gtk::Label>(title);
  card.title->add_css_class("ur-body");
  card.title->set_xalign(0);
  card.title->set_wrap(true);
  text->append(*card.title);
  card.description = Gtk::make_managed<Gtk::Label>();
  card.description->add_css_class("ur-caption");
  card.description->set_xalign(0);
  card.description->set_wrap(true);
  SetTextOrCollapse(*card.description, description);
  text->append(*card.description);
  row->append(*text);
  card.trailing = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  card.trailing->set_halign(Gtk::Align::END);
  card.trailing->set_valign(Gtk::Align::CENTER);
  row->append(*card.trailing);
  card.root = row;
  return card;
}

CopyField MakeCopyField(const Glib::ustring& label, const Glib::ustring& value,
                        bool masked) {
  CopyField field;
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  auto* caption = MakeStyledLabel(label, "ur-caption");
  column->append(*caption);

  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  field.value = Gtk::make_managed<Gtk::Label>(masked ? Glib::ustring("••••••••") : value);
  field.value->add_css_class("ur-body");
  field.value->set_xalign(0);
  field.value->set_ellipsize(Pango::EllipsizeMode::END);
  field.value->set_hexpand(true);
  field.value->set_valign(Gtk::Align::CENTER);
  field.value->set_selectable(!masked);
  row->append(*field.value);

  field.copy = Gtk::make_managed<Gtk::Button>();
  field.copy->set_icon_name("edit-copy-symbolic");
  field.copy->add_css_class("flat");
  field.copy->set_valign(Gtk::Align::CENTER);
  SetAccessibleLabel(*field.copy, "Copy " + label);
  // the full value is copied, never the mask
  field.copy->signal_clicked().connect([copy = field.copy, value] {
    copy->get_clipboard()->set_text(value);
  });
  row->append(*field.copy);
  column->append(*row);
  field.root = column;
  return field;
}

PlanUsageCard MakePlanUsageCard(const Glib::ustring& planLabel,
                                const Glib::ustring& planValue) {
  PlanUsageCard card;
  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  column->add_css_class("ur-card");
  column->append(*MakeStyledLabel(planLabel, "ur-caption"));
  card.planValue = Gtk::make_managed<Gtk::Label>(planValue);
  card.planValue->add_css_class("ur-stat-value");
  card.planValue->set_xalign(0);
  Pango::AttrList attrs;
  auto size = Pango::Attribute::create_attr_size_absolute(22 * PANGO_SCALE);
  attrs.insert(size);
  card.planValue->set_attributes(attrs);
  column->append(*card.planValue);
  card.usageBarHost = Gtk::make_managed<Gtk::Box>();
  card.usageBarHost->set_size_request(-1, 32);
  column->append(*card.usageBarHost);
  card.legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
  column->append(*card.legend);
  card.root = column;
  return card;
}

// ---- the status strip -------------------------------------------------------

StatusField MakeStatusField(const Glib::ustring& label, bool withDot,
                            const Glib::ustring& accessibleName) {
  StatusField field;
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  row->set_valign(Gtk::Align::CENTER);
  if (withDot) {
    field.dot = MakeDot(8);
    row->append(*field.dot);
  }
  if (!label.empty()) {
    field.caption = MakeStyledLabel(label, "ur-status-caption");
    // the caption is the field's NAME, folded into the value's announcement
    MarkDecorative(*field.caption);
    row->append(*field.caption);
  }
  field.value = MakeStyledLabel({}, "ur-status-value");
  field.name = accessibleName.empty() ? label : accessibleName;
  if (!field.name.empty()) SetAccessibleLabel(*field.value, field.name);
  row->append(*field.value);
  field.root = row;
  return field;
}

void SetStatusFieldValue(StatusField& field, const Glib::ustring& value) {
  if (!field.value) return;
  field.value->set_text(value);
  SetAccessibleLabel(*field.value,
                     field.name.empty() ? value : field.name + ", " + value);
}

void SetStatusFieldDot(StatusField& field, const std::string& colorHex) {
  if (field.dot) PaintDot(*field.dot, 8, colorHex);
}

Gtk::Widget* MakeStatusSeparator() {
  auto* rule = Gtk::make_managed<Gtk::Box>();
  rule->add_css_class("ur-vrule");
  rule->set_size_request(1, 14);
  rule->set_valign(Gtk::Align::CENTER);
  rule->set_margin_start(14);
  rule->set_margin_end(14);
  return rule;
}

// ---- the pane shell ---------------------------------------------------------

Pane MakePane(const Glib::ustring& title, const Glib::ustring& meta) {
  Pane pane;
  pane.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  pane.root->add_css_class("ur-pane");
  pane.root->set_vexpand(true);

  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  header->add_css_class("ur-pane-header");
  pane.title = MakeStyledLabel(title, "ur-pane-title");
  pane.title->set_hexpand(true);
  header->append(*pane.title);
  pane.meta = MakeStyledLabel({}, "ur-pane-meta", 1.f);
  SetTextOrCollapse(*pane.meta, meta);
  header->append(*pane.meta);
  pane.header = header;
  pane.root->append(*header);

  pane.content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_child(*pane.content);
  scroller->set_vexpand(true);
  pane.root->append(*scroller);
  return pane;
}

PaneGroupHeader MakePaneGroupHeader(const Glib::ustring& title, const Glib::ustring& meta) {
  PaneGroupHeader out;
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  row->add_css_class("ur-group-header");
  out.title = MakeStyledLabel(title, "ur-group-title");
  out.title->set_hexpand(true);
  // a group header is a heading, not a list item
  gtk_accessible_update_property(GTK_ACCESSIBLE(out.title->gobj()),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 (title + " (section)").c_str(), -1);
  row->append(*out.title);
  out.meta = MakeStyledLabel({}, "ur-pane-meta", 1.f);
  SetTextOrCollapse(*out.meta, meta);
  row->append(*out.meta);
  out.trailing = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  out.trailing->set_valign(Gtk::Align::CENTER);
  row->append(*out.trailing);
  out.root = row;
  return out;
}

namespace {
// the shared construction of the three-element list row (dot | title | meta),
// so the static and selectable forms cannot drift
struct ListRowParts {
  Gtk::Box* grid = nullptr;
  Gtk::Box* marker = nullptr;
  Gtk::Label* dot = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* meta = nullptr;
};
ListRowParts BuildListRowParts() {
  ListRowParts out;
  out.grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  out.marker = Gtk::make_managed<Gtk::Box>();
  out.marker->add_css_class("ur-nav-accent");
  out.marker->set_size_request(2, -1);
  out.marker->set_opacity(0);
  MarkDecorative(*out.marker);
  out.grid->append(*out.marker);
  out.dot = MakeDot(7);
  out.grid->append(*out.dot);
  out.title = MakeStyledLabel({}, "ur-row-title");
  out.title->set_hexpand(true);
  out.grid->append(*out.title);
  out.meta = MakeStyledLabel({}, "ur-value", 1.f);
  out.meta->add_css_class("dim-label");
  out.grid->append(*out.meta);
  return out;
}

// a bordered fixed-height host carrying the pane inset + bottom hairline
Gtk::Box* MakeRowHost(int height) {
  auto* host = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  host->set_size_request(-1, height);
  // A FIXED height, not a minimum: GTK4 propagates a child's expand flag up
  // through its ancestors unless a parent sets its own explicitly, so the
  // inner row's vexpand (which centers content in the row) would otherwise
  // make every row in a list stretch to fill the pane — measured as tall
  // gaps around the Network search row.
  host->set_vexpand(false);
  host->set_valign(Gtk::Align::START);
  auto* inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  inner->set_margin_start(12);
  inner->set_margin_end(12);
  inner->set_vexpand(true);
  host->append(*inner);
  auto* rule = Gtk::make_managed<Gtk::Box>();
  rule->add_css_class("ur-vrule");
  rule->set_size_request(-1, 1);
  host->append(*rule);
  return host;
}

Gtk::Box* RowHostInner(Gtk::Box* host) {
  return dynamic_cast<Gtk::Box*>(host->get_first_child());
}
}  // namespace

// The row itself: fixed height, bottom hairline, the pane's 12px inset.
// Append content to the returned box's first child (the inset inner row).
Gtk::Box* MakePaneRow(int height) { return MakeRowHost(height); }

PaneKeyValueRow MakePaneKeyValueRow(const Glib::ustring& key, const Glib::ustring& value,
                                    int height) {
  PaneKeyValueRow out;
  auto* host = MakeRowHost(height);
  auto* inner = RowHostInner(host);
  out.key = MakeStyledLabel(key, "ur-key");
  out.key->set_hexpand(true);
  MarkDecorative(*out.key);  // the key names the value; one fact, one node
  inner->append(*out.key);
  out.value = MakeStyledLabel(value, "ur-value", 1.f);
  SetAccessibleLabel(*out.value, key + ", " + value);
  inner->append(*out.value);
  out.root = host;
  return out;
}

PaneListRow MakePaneListRow(int height) {
  PaneListRow out;
  auto* host = MakeRowHost(height);
  auto parts = BuildListRowParts();
  parts.grid->set_vexpand(true);
  RowHostInner(host)->append(*parts.grid);
  parts.grid->set_hexpand(true);
  out.dot = parts.dot;
  out.title = parts.title;
  out.meta = parts.meta;
  out.root = host;
  return out;
}

PaneListRowButton MakePaneListRowButton(int height) {
  PaneListRowButton out;
  out.root = Gtk::make_managed<Gtk::Button>();
  out.root->add_css_class("ur-pane-row");
  out.root->set_size_request(-1, height);
  auto parts = BuildListRowParts();
  parts.grid->set_hexpand(true);
  out.root->set_child(*parts.grid);
  out.dot = parts.dot;
  out.title = parts.title;
  out.meta = parts.meta;
  out.marker = parts.marker;
  return out;
}

void SetPaneListRowSelected(PaneListRowButton& row, bool selected) {
  if (!row.root) return;
  if (selected) {
    row.root->add_css_class("selected");
  } else {
    row.root->remove_css_class("selected");
  }
  if (row.marker) row.marker->set_opacity(selected ? 1.0 : 0.0);
}

namespace {
Gtk::Box* MakeTwoLineText(Gtk::Label*& title, Gtk::Label*& note,
                          const Glib::ustring& titleText, const Glib::ustring& noteText) {
  auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
  text->set_valign(Gtk::Align::CENTER);
  text->set_hexpand(true);
  title = MakeStyledLabel(titleText, "ur-row-title");
  text->append(*title);
  note = MakeStyledLabel({}, "ur-row-note");
  SetTextOrCollapse(*note, noteText);
  text->append(*note);
  return text;
}
}  // namespace

PaneTwoLineRow MakePaneTwoLineRow(const Glib::ustring& title, const Glib::ustring& note,
                                  int height) {
  PaneTwoLineRow out;
  auto* host = MakeRowHost(height);
  auto* inner = RowHostInner(host);
  inner->append(*MakeTwoLineText(out.title, out.note, title, note));
  out.trailing = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  out.trailing->set_halign(Gtk::Align::END);
  out.trailing->set_valign(Gtk::Align::CENTER);
  inner->append(*out.trailing);
  out.root = host;
  return out;
}

PaneTwoLineRowButton MakePaneTwoLineRowButton(const Glib::ustring& title,
                                              const Glib::ustring& note, int height) {
  PaneTwoLineRowButton out;
  out.root = Gtk::make_managed<Gtk::Button>();
  out.root->add_css_class("ur-pane-row");
  out.root->set_size_request(-1, height);
  auto* grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  grid->set_hexpand(true);
  grid->append(*MakeTwoLineText(out.title, out.note, title, note));
  out.value = MakeStyledLabel({}, "ur-value", 1.f);
  out.value->add_css_class("dim-label");
  out.value->set_max_width_chars(28);
  grid->append(*out.value);
  auto* chevron = Gtk::make_managed<Gtk::Image>();
  chevron->set_from_icon_name("go-next-symbolic");
  chevron->set_pixel_size(11);
  chevron->add_css_class("ur-label-faint");
  chevron->set_valign(Gtk::Align::CENTER);
  MarkDecorative(*chevron);
  grid->append(*chevron);
  out.root->set_child(*grid);
  SetAccessibleLabel(*out.root, title + (note.empty() ? "" : ". " + note));
  return out;
}

PaneTableRow MakePaneTableRow(const std::vector<int>& weights, int height,
                              size_t textColumns) {
  PaneTableRow out;
  auto* host = MakeRowHost(height);
  auto* inner = RowHostInner(host);
  inner->set_spacing(12);
  for (size_t index = 0; index < weights.size(); ++index) {
    const bool textCell = index < textColumns;
    auto* cell = MakeStyledLabel({}, textCell ? "ur-row-title" : "ur-value",
                                 textCell ? 0.f : 1.f);
    if (!textCell) cell->add_css_class("dim-label");
    cell->set_hexpand(true);
    // star weights approximated with hexpand + a minimum that narrows rather
    // than vanishes (windows MinWidth 56)
    cell->set_size_request(56 * weights[index] / std::max(1, weights[0]), -1);
    inner->append(*cell);
    out.cells.push_back(cell);
  }
  out.root = host;
  return out;
}

Gtk::Widget* MakePaneTableHeader(const std::vector<int>& weights,
                                 const std::vector<Glib::ustring>& titles,
                                 size_t textColumns) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  row->add_css_class("ur-group-header");
  for (size_t index = 0; index < weights.size() && index < titles.size(); ++index) {
    const bool textCell = index < textColumns;
    auto* cell = MakeStyledLabel(titles[index], "ur-col-header", textCell ? 0.f : 1.f);
    cell->set_hexpand(true);
    cell->set_size_request(56 * weights[index] / std::max(1, weights[0]), -1);
    row->append(*cell);
  }
  return row;
}

PaneTableStack MakePaneTableStack(PaneTableRow& row, size_t index) {
  PaneTableStack out;
  auto* host = dynamic_cast<Gtk::Box*>(row.root);
  if (host == nullptr || index >= row.cells.size()) return out;
  auto* inner = RowHostInner(host);
  Gtk::Label* cell = row.cells[index];
  int minWidth = -1;
  int minHeight = -1;
  cell->get_size_request(minWidth, minHeight);
  out.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  out.root->set_valign(Gtk::Align::CENTER);
  out.root->set_hexpand(true);
  // the same star weight the cell had, so the columns stay aligned
  out.root->set_size_request(minWidth, -1);
  out.top = MakeStyledLabel({}, "ur-row-title", 0.f);
  out.bottom = MakeStyledLabel({}, "ur-row-title", 0.f);
  out.bottom->set_visible(false);
  out.root->append(*out.top);
  out.root->append(*out.bottom);
  inner->insert_child_after(*out.root, *cell);
  inner->remove(*cell);
  row.cells[index] = out.top;
  return out;
}

PaneSearchRow MakePaneSearchRow(const Glib::ustring& placeholder) {
  PaneSearchRow out;
  auto* host = MakeRowHost(40);
  auto* inner = RowHostInner(host);
  inner->set_spacing(8);
  auto* glyph = Gtk::make_managed<Gtk::Image>();
  glyph->set_from_icon_name("system-search-symbolic");
  glyph->set_pixel_size(13);
  glyph->add_css_class("dim-label");
  glyph->set_valign(Gtk::Align::CENTER);
  MarkDecorative(*glyph);
  inner->append(*glyph);
  out.box = Gtk::make_managed<Gtk::Entry>();
  out.box->add_css_class("ur-pane-search");
  out.box->set_placeholder_text(placeholder);
  out.box->set_hexpand(true);
  out.box->set_valign(Gtk::Align::CENTER);
  SetAccessibleLabel(*out.box, placeholder);  // a placeholder is not a name
  inner->append(*out.box);
  out.root = host;
  return out;
}

// ---- validation + snackbar --------------------------------------------------

void ApplySupportingText(Gtk::Label& line, const Glib::ustring& text,
                         ValidationState state) {
  line.set_text(text);
  for (const char* cls : {"ur-danger-text", "ur-value-on", "dim-label"}) {
    line.remove_css_class(cls);
  }
  switch (state) {
    case ValidationState::Invalid: line.add_css_class("ur-danger-text"); break;
    case ValidationState::Valid: line.add_css_class("ur-value-on"); break;
    case ValidationState::NotChecked:
    case ValidationState::Validating: line.add_css_class("dim-label"); break;
  }
}

Snackbar::Snackbar() {
  bar_.add_css_class("ur-snackbar");
  message_.set_wrap(true);
  message_.set_max_width_chars(56);
  bar_.append(message_);
  auto* dismiss = Gtk::make_managed<Gtk::Button>();
  dismiss->set_icon_name("window-close-symbolic");
  dismiss->add_css_class("flat");
  dismiss->set_valign(Gtk::Align::CENTER);
  SetAccessibleLabel(*dismiss, "Dismiss");
  dismiss->signal_clicked().connect([this] { Hide(); });
  bar_.append(*dismiss);

  revealer_.set_child(bar_);
  revealer_.set_transition_type(Gtk::RevealerTransitionType::SLIDE_UP);
  revealer_.set_transition_duration(150);
  revealer_.set_reveal_child(false);
  revealer_.set_halign(Gtk::Align::CENTER);
  revealer_.set_valign(Gtk::Align::END);
  revealer_.set_margin_bottom(16);
  // MaxWidth 480 (windows AccountSnackbar)
  bar_.set_size_request(-1, -1);
}

Gtk::Widget& Snackbar::root() { return revealer_; }

void Snackbar::Show(const Glib::ustring& message, Severity severity,
                    std::optional<int> durationMs) {
  timer_.disconnect();  // a second message restarts the window
  message_.set_text(message);
  bar_.remove_css_class("ur-snackbar-error");
  bar_.remove_css_class("ur-snackbar-success");
  bar_.remove_css_class("ur-snackbar-gold");
  if (severity == Severity::Error || severity == Severity::Warning) {
    bar_.add_css_class("ur-snackbar-error");
  } else if (severity == Severity::Success) {
    bar_.add_css_class("ur-snackbar-success");
  } else if (severity == Severity::Gold) {
    // the referral gold toast (a friend joined with your code)
    bar_.add_css_class("ur-snackbar-gold");
  }
  revealer_.set_reveal_child(true);
  // an error is usually the only diagnostic the user gets; it waits for them
  const bool safeToMiss = (severity == Severity::Info || severity == Severity::Success ||
                           severity == Severity::Gold);
  const int duration = durationMs.value_or(safeToMiss ? kDefaultDurationMs : kPersistent);
  if (duration <= kPersistent) return;
  timer_ = Glib::signal_timeout().connect(
      sigc::track_obj(
          [this]() -> bool {
            Hide();
            return false;
          },
          revealer_),
      static_cast<unsigned>(duration));
}

void Snackbar::Hide() {
  timer_.disconnect();
  revealer_.set_reveal_child(false);
}

}  // namespace urnw::kit
