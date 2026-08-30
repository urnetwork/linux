// The component kit — the GTK port of windows:UrComponents.{h,cpp} plus the
// pane-shell styles at the foot of App.xaml. Every Windows-parity destination
// is assembled from THESE builders so the pages come out of one kit rather
// than seven re-inventions: one row species per list, one group rhythm, one
// status-field shape. CSS classes live in UrTheme.cpp's pane-shell block;
// each builder applies the class of the same role, so a pane built here and
// the styles are one system.
//
// Accessibility carries over from the Windows kit: decorative marks (dots,
// glyphs beside a label that says the same word) are hidden from the
// accessibility tree; value fields re-announce as "Label, value"; a button
// whose content is a box gets an explicit accessible label from its caller.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

namespace urnw::kit {

// ---- the one desktop breakpoint (windows UrComponents.h) -------------------
// Below it: the centred single column; at or above: the horizontal pane
// composition. Home additionally folds at 640 and grows a third pane at 1800.
inline constexpr double kWideBreakpointDip = 1000.0;
inline constexpr double kUltraWideDip = 1800.0;

// Set a line's text AND its visibility in one call: an empty string collapses
// the element instead of leaving a row of nothing behind (a box spends its
// spacing on a child that drew nothing).
void SetTextOrCollapse(Gtk::Label& line, const Glib::ustring& text);

// Mark a widget decorative: hidden from the accessibility tree (the windows
// kit's AccessibilityView::Raw).
void MarkDecorative(Gtk::Widget& widget);
// Give a widget an explicit accessible label (a button whose content is a box
// has NO automatic name).
void SetAccessibleLabel(Gtk::Widget& widget, const Glib::ustring& label);

// A 1px rule in the border token (horizontal).
Gtk::Widget* MakeDivider();
// The vertical rule BETWEEN two panes.
Gtk::Widget* MakePaneVRule();

// A section header above a card: symbolic icon + 16 SemiBold label.
Gtk::Widget* MakeSectionHeader(const char* iconName, const Glib::ustring& text);

// "There is nothing here yet": a large muted glyph over one sentence, centred
// — never a bare dash (a dash cannot distinguish nothing / not loaded /
// failed). The Card variant is for a panel with no card around it already.
Gtk::Widget* MakeEmptyState(const char* iconName, const Glib::ustring& text);
Gtk::Widget* MakeEmptyStateCard(const char* iconName, const Glib::ustring& text);
// The empty state of a pane that FILLS: one centred muted line mid-pane,
// never a short card (windows MakePaneEmptyLine).
Gtk::Label* MakePaneEmptyLine(const Glib::ustring& text);

// PageHeader: page title in the display face + optional muted description.
Gtk::Widget* MakePageHeader(const Glib::ustring& title,
                            const Glib::ustring& description = {});

// MetricCard: a boxed stat tile — muted caption over a big condensed value.
struct MetricCard {
  Gtk::Widget* root = nullptr;
  Gtk::Label* label = nullptr;
  Gtk::Label* value = nullptr;
};
MetricCard MakeMetricCard(const Glib::ustring& label, const Glib::ustring& value = {});

// SettingsCard: leading icon, title + optional description, trailing slot for
// a control or chevron (point the control's accessible label at the title).
struct SettingsCard {
  Gtk::Widget* root = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* description = nullptr;
  Gtk::Box* trailing = nullptr;
};
SettingsCard MakeSettingsCard(const char* iconName, const Glib::ustring& title,
                              const Glib::ustring& description = {});

// CopyField: caption, value (optionally masked), copy button that writes the
// FULL value — the masked display never reaches the clipboard.
struct CopyField {
  Gtk::Widget* root = nullptr;
  Gtk::Label* value = nullptr;
  Gtk::Button* copy = nullptr;
};
CopyField MakeCopyField(const Glib::ustring& label, const Glib::ustring& value,
                        bool masked = false);

// PlanUsageCard: plan name + value, a host for a UsageBar, a legend row.
struct PlanUsageCard {
  Gtk::Widget* root = nullptr;
  Gtk::Label* planValue = nullptr;
  Gtk::Box* usageBarHost = nullptr;
  Gtk::Box* legend = nullptr;
};
PlanUsageCard MakePlanUsageCard(const Glib::ustring& planLabel,
                                const Glib::ustring& planValue = {});

// ---- the persistent status strip -------------------------------------------
// One field: optional 8px state dot, an 11sp caption (the field's NAME,
// hidden at narrow widths at no cost to a screen reader), the value beside
// it. SetStatusFieldValue rewrites the announced name as "Label, value".
struct StatusField {
  Gtk::Widget* root = nullptr;
  Gtk::Label* dot = nullptr;      // colored ● markup; null unless withDot
  Gtk::Label* caption = nullptr;  // null when label was empty
  Gtk::Label* value = nullptr;
  Glib::ustring name;
};
StatusField MakeStatusField(const Glib::ustring& label, bool withDot = false,
                            const Glib::ustring& accessibleName = {});
void SetStatusFieldValue(StatusField& field, const Glib::ustring& value);
// paint the field's dot (markup ●) in a color; no-op without a dot
void SetStatusFieldDot(StatusField& field, const std::string& colorHex);
Gtk::Widget* MakeStatusSeparator();

// ---- the pane shell --------------------------------------------------------
// A pane: full-bleed vertical column on the page fill; opens with a 40px
// header strip (title + right meta) and scrolls independently underneath.
struct Pane {
  Gtk::Box* root = nullptr;     // the whole pane column
  Gtk::Label* title = nullptr;  // header title
  Gtk::Label* meta = nullptr;   // header right-aligned figure
  Gtk::Box* header = nullptr;   // append header actions here
  Gtk::Box* content = nullptr;  // scrolled content column (append rows/groups)
};
Pane MakePane(const Glib::ustring& title, const Glib::ustring& meta = {});

// The 28px strip that opens a group inside a pane: letterspaced caption left,
// optional count right, optional trailing icon-command slot.
struct PaneGroupHeader {
  Gtk::Widget* root = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* meta = nullptr;
  Gtk::Box* trailing = nullptr;
};
PaneGroupHeader MakePaneGroupHeader(const Glib::ustring& title,
                                    const Glib::ustring& meta = {});

// The row itself: fixed height, bottom hairline, the pane's 12px inset.
Gtk::Box* MakePaneRow(int height);

// key on the left, value hard right, one line each, both trimmed (34px).
struct PaneKeyValueRow {
  Gtk::Widget* root = nullptr;
  Gtk::Label* key = nullptr;
  Gtk::Label* value = nullptr;
};
PaneKeyValueRow MakePaneKeyValueRow(const Glib::ustring& key,
                                    const Glib::ustring& value = {}, int height = 34);

// A list row: leading state dot, a title that trims, a right-aligned figure
// (36px). One row species per pane layout.
struct PaneListRow {
  Gtk::Widget* root = nullptr;
  Gtk::Label* dot = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* meta = nullptr;
};
PaneListRow MakePaneListRow(int height = 36);

// The SAME row, selectable: identical grid on a button root, plus the 2px
// leading accent marker. Selection rides THREE channels: fill step, accent
// bar (a shape, not color-alone), and the accessible name gaining
// ", selected". The caller MUST SetAccessibleLabel the root.
struct PaneListRowButton {
  Gtk::Button* root = nullptr;
  Gtk::Label* dot = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* meta = nullptr;
  Gtk::Box* marker = nullptr;  // the 2px leading accent bar
};
PaneListRowButton MakePaneListRowButton(int height = 36);
void SetPaneListRowSelected(PaneListRowButton& row, bool selected);

// The two-line row: title + one TRIMMED explanation line, trailing control
// slot (44px — a list of explained rows picks the tall height throughout).
struct PaneTwoLineRow {
  Gtk::Widget* root = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* note = nullptr;
  Gtk::Box* trailing = nullptr;
};
PaneTwoLineRow MakePaneTwoLineRow(const Glib::ustring& title,
                                  const Glib::ustring& note = {}, int height = 44);

// The same two-line row as a button (opens something): trailing muted value +
// chevron, same metrics, same hairline.
struct PaneTwoLineRowButton {
  Gtk::Button* root = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Label* note = nullptr;
  Gtk::Label* value = nullptr;
};
PaneTwoLineRowButton MakePaneTwoLineRowButton(const Glib::ustring& title,
                                              const Glib::ustring& note = {},
                                              int height = 44);

// A table row: N star-weighted cells, one fixed height, bottom hairline. The
// leading `textColumns` cells read left as text; the rest read right as
// figures. Its header strip shares the weights so the columns stay aligned.
struct PaneTableRow {
  Gtk::Widget* root = nullptr;
  std::vector<Gtk::Label*> cells;
};
PaneTableRow MakePaneTableRow(const std::vector<int>& weights, int height = 36,
                              size_t textColumns = 1);
Gtk::Widget* MakePaneTableHeader(const std::vector<int>& weights,
                                 const std::vector<Glib::ustring>& titles,
                                 size_t textColumns = 1);

// The search field row at the top of a list pane (40px, squared off).
struct PaneSearchRow {
  Gtk::Widget* root = nullptr;
  Gtk::Entry* box = nullptr;
};
PaneSearchRow MakePaneSearchRow(const Glib::ustring& placeholder);

// ---- validation + snackbar -------------------------------------------------
enum class ValidationState { NotChecked, Validating, Valid, Invalid };
// Renders text on a field's supporting line in the state's color: danger for
// Invalid, brand green for Valid, muted otherwise. Empty text still applies
// the color so a cleared line cannot flash the previous verdict.
void ApplySupportingText(Gtk::Label& line, const Glib::ustring& text,
                         ValidationState state);

// A transient bar that dismisses itself — but only when what it says is safe
// to miss: Info/Success time out (4s), Warning/Error stay until dismissed
// (the message is often the user's only diagnostic). Owns a bottom-centered
// revealer the window overlays across its content.
class Snackbar {
 public:
  static constexpr int kDefaultDurationMs = 4000;
  static constexpr int kPersistent = 0;
  enum class Severity { Info, Success, Warning, Error, Gold };

  Snackbar();
  Gtk::Widget& root();  // overlay this at the window bottom-center

  void Show(const Glib::ustring& message, Severity severity = Severity::Info,
            std::optional<int> durationMs = std::nullopt);
  void Hide();

 private:
  Gtk::Revealer revealer_;
  Gtk::Box bar_{Gtk::Orientation::HORIZONTAL, 12};
  Gtk::Label message_;
  sigc::connection timer_;
};

}  // namespace urnw::kit
