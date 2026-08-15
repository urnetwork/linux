// SPDX-License-Identifier: MPL-2.0
#include "ConnectPage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "LocationsSheet.hpp"  // PeerDisplayName — shared with the chooser
#include "Ui.hpp"

namespace urnw {
namespace {

// the fold thresholds (docs/parity/connect-page.md §0)
constexpr int kThreePaneDip = 1000;
constexpr int kTwoPaneDip = 640;
constexpr int kPaneAWidth = 330;
constexpr int kPaneCWidth = 380;
constexpr int kSimpleCap = 480;
constexpr int kHeroAdvanced = 190;
constexpr int kHeroSimple = 320;

// pane metrics (§1): the chart slots and the list/key-value row rhythms
constexpr int kRemoteChartHeight = 150;  // pane B header chart
constexpr int kPaneCChartHeight = 132;   // pane C blocked + local charts
constexpr int kListRowHeight = 36;       // connections / contracts / split rules
constexpr int kKeyValueRowHeight = 34;   // session figures, inspector, dns
constexpr int kPeerRowHeight = 34;       // the peers list (§1: peers rows are 34)
constexpr size_t kMaxConnectionRows = 200;  // a cap, not a scroll budget
// The shell chrome between the toplevel and this page: HomeShell's nav rail is
// pinned at 220dip (HomeShell.cpp kNavExpandedWidth). Only used before the
// page has an allocation of its own — the fold table is defined on the width
// the PANES share, and MainWindow can only report the window's.
constexpr int kShellChromeDip = 220;

// The page's own CSS, one priority ABOVE UrTheme (APPLICATION + 1) so a
// two-class rule here settles a metric the shared sheet fixes for every
// button row.
//
// Why it has to exist: kit::MakePaneListRowButton pins its height with
// set_size_request, but `button.ur-pane-row` carries `min-height: 40px`, and
// GTK4 takes the MAX of the two — so the Advanced (selectable) activity row
// measured 41 against the Normal row's 36 and the whole list re-laid itself on
// a mode toggle. The declared min-height is the CONTENT box, and the 1px
// bottom hairline is drawn outside it, while the kit's static row counts its
// hairline INSIDE its request: 35 + 1 == 36 makes the two forms the same
// height to the pixel.
constexpr const char* kPageCss = R"CSS(
button.ur-pane-row.ur-pane-row-36 { min-height: 35px; }
button.ur-pane-row.ur-pane-row-34 { min-height: 33px; }
/* the 12px supporting voice ON TOP of a key/value type role (§4.1 inspector
   verdict, §4.6 dns state cells) — a size beside a role, one provider */
.ur-key.ur-text-12, .ur-value.ur-text-12 { font-size: 12px; }
)CSS";

void EnsurePageCss() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kPageCss);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
}

// status dot colors (§8.1)
constexpr const char* kDotGreen = "#87FB67";
constexpr const char* kDotConnecting = "#E6EA23";
constexpr const char* kDotCoral = "#FF6C58";
constexpr const char* kDotIdle = "#2A60FF";
// the third verdict: sent AROUND the tunnel — allowed and unprotected
constexpr const char* kDotAmber = "#F5C242";
constexpr const char* kDotFaint = "#5A5A5A";

std::string UpperCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string LowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A FIXED-width rail (pane A 330, pane C 380) only holds its width if nothing
// inside it asks for more: an ellipsizing label still reports its whole text
// as its NATURAL width, a wrapping one reports its whole unwrapped line, and
// GtkBox hands surplus out to naturals BEFORE it hands it to the expanding
// pane. Capping the natural at N characters keeps the rails at their spec
// widths and costs nothing at render time (the label still wraps/ellipsizes to
// whatever it is actually allocated).
void CapNatural(Gtk::Label* label, int chars) {
  if (label) label->set_max_width_chars(chars);
}

Gtk::Label* MakeSupportingLine() {
  auto* line = Gtk::make_managed<Gtk::Label>();
  line->add_css_class("ur-caption");
  line->set_xalign(0);
  line->set_wrap(true);
  line->set_visible(false);
  CapNatural(line, 34);
  return line;
}

// ---- feed fingerprints --------------------------------------------------
// The SDK structs carry no operator==, and the clock-driven poll re-reads
// every feed once a second: a list rebuild destroys hover and keyboard focus,
// so each surface compares a cheap value fingerprint first and only rebuilds
// on a real change. nullopt (no session) hashes to a distinct value from an
// empty list — "unavailable" and "nothing yet" are different readings.
constexpr uint64_t kAbsentSig = 0x9e3779b97f4a7c15ull;

uint64_t HashMix(uint64_t h, uint64_t v) {
  return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}
uint64_t HashText(uint64_t h, const std::string& text) {
  h = HashMix(h, text.size());
  for (const unsigned char c : text) h = HashMix(h, c);
  return h;
}
uint64_t HashList(uint64_t h, const std::optional<urnet::StringList>& values) {
  if (!values) return HashMix(h, 1);
  h = HashMix(h, values->size());
  for (const auto& value : *values) h = HashText(h, value);
  return h;
}

uint64_t BlockActionsSig(const std::optional<urnet::BlockActionList>& actions) {
  if (!actions) return kAbsentSig;
  uint64_t h = HashMix(1, actions->size());
  for (const auto& action : *actions) {
    h = HashText(h, action.BlockActionId.value_or(std::string()));
    h = HashMix(h, static_cast<uint64_t>(action.Time));
    h = HashMix(h, static_cast<uint64_t>(action.ByteCount));
    h = HashMix(h, static_cast<uint64_t>(action.PacketCount));
    h = HashMix(h, (action.Block ? 1u : 0u) | (action.Local ? 2u : 0u));
  }
  return h;
}

uint64_t ContractRowsSig(const std::optional<urnet::ContractPeerRowList>& rows) {
  if (!rows) return kAbsentSig;
  uint64_t h = HashMix(2, rows->size());
  for (const auto& row : *rows) {
    h = HashText(h, row.ClientId);
    h = HashMix(h, static_cast<uint64_t>(row.SendByteCount));
    h = HashMix(h, static_cast<uint64_t>(row.ReceiveByteCount));
    h = HashMix(h, static_cast<uint64_t>(row.LastActivityMillis));
    h = HashMix(h, row.Closing ? 1u : 0u);
  }
  return h;
}

uint64_t SplitRulesSig(const std::optional<urnet::BlockActionOverrideList>& rules) {
  if (!rules) return kAbsentSig;
  uint64_t h = HashMix(3, rules->size());
  for (const auto& rule : *rules) {
    h = HashText(h, rule.OverrideId.value_or(std::string()));
    h = HashList(h, rule.Hosts);
    h = HashMix(h, rule.RouteOverride && rule.RouteOverride->Local ? 1u : 0u);
  }
  return h;
}

uint64_t PeersSig(const std::optional<urnet::NetworkPeerList>& peers, int64_t count) {
  if (!peers) return kAbsentSig;
  uint64_t h = HashMix(4, static_cast<uint64_t>(count));
  h = HashMix(h, peers->size());
  for (const auto& peer : *peers) {
    h = HashText(h, peer.ClientId.value_or(std::string()));
    h = HashText(h, peer.DeviceName);
    h = HashText(h, peer.DeviceSpec);
    h = HashMix(h, peer.ProvideEnabled ? 1u : 0u);
  }
  return h;
}

uint64_t ProfileSig(const std::optional<urnet::PerformanceProfile>& profile) {
  if (!profile) return kAbsentSig;
  uint64_t h = HashText(5, profile->window_type);
  h = HashMix(h, profile->allow_direct ? 1u : 0u);
  h = HashMix(h, profile->post_quantum_encryption ? 1u : 0u);
  if (profile->window_size) {
    h = HashMix(h, static_cast<uint64_t>(profile->window_size->window_size_min));
    h = HashMix(h, static_cast<uint64_t>(profile->window_size->window_size_max));
  }
  return h;
}

// the colored ● every list/status dot is painted with (the kit builds the
// label; the caller paints it per state)
std::string DotMarkup(int sizePx, const char* colorHex) {
  return std::string("<span size='") + std::to_string(sizePx * PANGO_SCALE) +
         "' foreground='" + colorHex + "'>●</span>";
}

// The pane kit's row host carries the 12px inset on its FIRST child; content
// goes in there (PaneKit.hpp: "append content to the returned box's first
// child"). Null-safe so a kit change cannot crash a render.
Gtk::Box* RowInner(Gtk::Widget* rowRoot) {
  auto* host = dynamic_cast<Gtk::Box*>(rowRoot);
  return host ? dynamic_cast<Gtk::Box*>(host->get_first_child()) : nullptr;
}

std::string JoinValues(const std::optional<urnet::StringList>& values) {
  if (!values) return {};
  std::string out;
  for (const auto& value : *values) {
    if (!out.empty()) out += ", ";
    out += value;
  }
  return out;
}

// iOS title precedence: the matched host is what the ROUTING DECISION was
// about; the requested host, then the addresses, are the fallbacks.
std::string BlockActionTitle(const urnet::BlockAction& action) {
  if (action.MatchedHosts && !action.MatchedHosts->empty()) return action.MatchedHosts->front();
  if (action.Hosts && !action.Hosts->empty()) return action.Hosts->front();
  if (action.MatchedIps && !action.MatchedIps->empty()) return action.MatchedIps->front();
  if (action.Ips && !action.Ips->empty()) return action.Ips->front();
  return {};
}

// THREE verdicts, not two: blocked (coral), local — sent around the tunnel,
// allowed and unprotected (amber), tunnelled (green).
const char* VerdictDot(const urnet::BlockAction& action) {
  if (action.Block) return kDotCoral;
  if (action.Local) return kDotAmber;
  return kDotGreen;
}

// the dot is decorative, so the WORD is the only place the color's meaning
// exists for a screen reader
const char* VerdictWord(const urnet::BlockAction& action) {
  if (action.Block) return T_("blocked", "Blocked");
  if (action.Local) return T_("local", "Local");
  return T_("allowed", "Allowed");
}

std::string ShortId(const std::string& id) {
  return id.size() <= 12 ? id : id.substr(0, 12) + "…";
}

int64_t NowMillis() { return g_get_real_time() / 1000; }

// The one-row empty sentence a pane group renders instead of a HOLE: the
// key/value row species carrying only its key.
kit::PaneKeyValueRow MakeEmptySentenceRow(const Glib::ustring& text, int height) {
  auto row = kit::MakePaneKeyValueRow(text, {}, height);
  // The kit hides the KEY (a key only names its value) and announces the
  // VALUE as "{key}, {value}". Here the key IS the sentence and the value is
  // an empty label with no extent, so the announcement would sit on a node
  // that renders nothing while the words are hidden from the tree. Put the
  // reading back on the node that carries it.
  if (row.key) {
    gtk_accessible_update_state(GTK_ACCESSIBLE(row.key->gobj()), GTK_ACCESSIBLE_STATE_HIDDEN,
                                FALSE, -1);
    kit::SetAccessibleLabel(*row.key, text);
    row.key->set_wrap(false);
    CapNatural(row.key, 34);
  }
  if (row.value) kit::MarkDecorative(*row.value);
  if (row.root) kit::SetAccessibleLabel(*row.root, text);
  return row;
}

// The connected-country reading behind the dns pill and the provider row, so
// a poll can tell "nothing changed" without touching a widget.
uint64_t LocationSig(const std::optional<urnet::ConnectLocation>& location) {
  if (!location) return kAbsentSig;
  uint64_t h = HashText(6, location->name.value_or(std::string()));
  h = HashText(h, location->country.value_or(std::string()));
  h = HashText(h, location->country_code.value_or(std::string()));
  if (location->connect_location_id) {
    h = HashText(h, location->connect_location_id->client_id.value_or(std::string()));
    h = HashText(h, location->connect_location_id->location_id.value_or(std::string()));
    h = HashMix(h, location->connect_location_id->best_available.value_or(false) ? 1u : 0u);
  }
  return h;
}

}  // namespace

ConnectPage::ConnectPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  add_css_class("ur-pane");
  // the dns pill capsule + the dot tones live in the drawer sheet (Ui.cpp);
  // idempotent, and the legacy drawer may not have been built yet
  EnsureDrawerCss();
  EnsurePageCss();
  BuildPaneA();
  paneBRule_ = kit::MakePaneVRule();
  append(*paneBRule_);
  BuildPaneB();
  paneCRule_ = kit::MakePaneVRule();
  append(*paneCRule_);
  BuildPaneC();
  ApplyConnectStatus();
  ApplyMoreOptionsVisibility();
  // Seed every pane B/C surface at build time: a page that has never had a
  // session must still SETTLE on its empty/unavailable readings rather than
  // sit blank (docs/parity/connect-page.md §8).
  RefreshAllPanes();
  ApplyBreakpoint(widthDip_);

  // "Presenting" is the WINDOW being up; "visible" is this destination being
  // the mapped stack child. The clock needs both (§5) — sitting on Earnings
  // must not wake the process ten times a second.
  signal_map().connect([this] {
    pageVisible_ = true;
    UpdateClock();
    // a destination that was hidden while feeds moved re-seeds on entry
    if (presenting_) Resync();
  });
  signal_unmap().connect([this] {
    pageVisible_ = false;
    UpdateClock();
  });
}

// ---- PANE A -----------------------------------------------------------------

void ConnectPage::BuildPaneA() {
  paneA_ = kit::MakePane(T_("connect", "Connect"));
  paneA_.root->set_size_request(kPaneAWidth, -1);
  kit::SetAccessibleLabel(*paneA_.root, T_("connect", "Connect"));
  // §0 Simple Mode: "MaxWidth 480 and HorizontalAlignment Center". GTK has no
  // max-width and set_size_request is a FLOOR (it would push the window's own
  // minimum past its advertised 400dip and cap nothing), so the column rides
  // an AdwClamp — a real maximum that still shrinks.
  paneAContent_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  paneAClamp_ = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(paneAClamp_), kSimpleCap);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(paneAClamp_), kSimpleCap);
  adw_clamp_set_child(ADW_CLAMP(paneAClamp_), GTK_WIDGET(paneAContent_->gobj()));
  paneA_.content->append(*Glib::wrap(paneAClamp_));

  // 2.1 status row: a 10px dot pinned to the first line + the 20sp status
  // line, with three collapsible supporting lines under it.
  auto* statusRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  statusRow->set_margin_start(12);
  statusRow->set_margin_end(12);
  statusRow->set_margin_top(14);
  statusRow->set_margin_bottom(14);
  statusDot_ = Gtk::make_managed<Gtk::Label>();
  statusDot_->set_valign(Gtk::Align::START);
  statusDot_->set_margin_top(7);
  kit::MarkDecorative(*statusDot_);
  statusRow->append(*statusDot_);
  auto* statusColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  statusColumn->set_hexpand(true);
  statusText_ = Gtk::make_managed<Gtk::Label>();
  statusText_->add_css_class("ur-body-large");
  statusText_->set_xalign(0);
  statusText_->set_wrap(true);
  {
    Pango::AttrList attrs;
    auto size = Pango::Attribute::create_attr_size_absolute(20 * PANGO_SCALE);
    attrs.insert(size);
    auto weight = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
    attrs.insert(weight);
    statusText_->set_attributes(attrs);
  }
  // a wrapping line's natural width is its whole unwrapped text: capped so a
  // long status ("Disconnecting — your traffic is still…") cannot pull the
  // 330dip rail wider than the rail
  CapNatural(statusText_, 20);
  statusColumn->append(*statusText_);
  protectionText_ = MakeSupportingLine();
  statusColumn->append(*protectionText_);
  trafficHeldText_ = MakeSupportingLine();
  statusColumn->append(*trafficHeldText_);
  statusReasonText_ = MakeSupportingLine();
  statusColumn->append(*statusReasonText_);
  // the daemon-session line (linux-only: "service not running" and friends
  // are DISTINCT actionable states, never a blank — MIGRATION.md)
  daemonNoticeText_ = MakeSupportingLine();
  statusColumn->append(*daemonNoticeText_);
  statusRow->append(*statusColumn);
  paneAContent_->append(*statusRow);
  paneAContent_->append(*kit::MakeDivider());

  // 2.2 the hero: a full-width transparent button wrapping the canvas; the
  // canvas is decorative, the button carries click/focus/name.
  hero_ = Gtk::make_managed<Gtk::Button>();
  hero_->add_css_class("flat");
  hero_->set_has_frame(false);
  hero_->set_margin_top(8);
  hero_->set_margin_bottom(8);
  canvas_ = Gtk::make_managed<ConnectCanvas>();
  // §0: the hero host is a MaxWidth (190 Advanced / 320 Simple), not a floor —
  // the canvas already clamps its own side to [168,288] off the host width, so
  // a size request here would only pin the window's minimum wide.
  heroClamp_ = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(heroClamp_), kHeroAdvanced);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(heroClamp_), kHeroAdvanced);
  adw_clamp_set_child(ADW_CLAMP(heroClamp_), GTK_WIDGET(canvas_->gobj()));
  hero_->set_child(*Glib::wrap(heroClamp_));
  hero_->signal_clicked().connect([this] {
    if (on_toggle_connect) on_toggle_connect();
  });
  {  // hover + keyboard focus ring (desktop affordances the phones lack)
    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_enter().connect([this](double, double) { canvas_->SetHovered(true); });
    motion->signal_leave().connect([this] { canvas_->SetHovered(false); });
    hero_->add_controller(motion);
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_enter().connect([this] {
      // keyboard-sourced focus only: a ring on mouse press is noise
      canvas_->SetFocusRingVisible(gtk_widget_has_visible_focus(GTK_WIDGET(hero_->gobj())));
    });
    focus->signal_leave().connect([this] { canvas_->SetFocusRingVisible(false); });
    hero_->add_controller(focus);
  }
  paneAContent_->append(*hero_);
  paneAContent_->append(*kit::MakeDivider());

  // 2.3 the selected-provider row -> the location chooser
  locationRow_ = Gtk::make_managed<Gtk::Button>();
  locationRow_->add_css_class("ur-pane-row");
  locationRow_->set_size_request(-1, 48);
  {
    auto* grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    column->set_valign(Gtk::Align::CENTER);
    column->set_hexpand(true);
    auto* caption = Gtk::make_managed<Gtk::Label>(
        T_("selected_provider", "Selected provider"));
    caption->add_css_class("ur-caption");
    caption->set_xalign(0);
    kit::MarkDecorative(*caption);
    column->append(*caption);
    locationText_ = Gtk::make_managed<Gtk::Label>(
        T_("best_available_provider", "Best available provider"));
    locationText_->add_css_class("ur-row-title");
    locationText_->set_xalign(0);
    locationText_->set_ellipsize(Pango::EllipsizeMode::END);
    CapNatural(locationText_, 24);
    kit::MarkDecorative(*locationText_);
    column->append(*locationText_);
    grid->append(*column);
    auto* chevron = Gtk::make_managed<Gtk::Image>();
    chevron->set_from_icon_name("go-next-symbolic");
    chevron->set_pixel_size(14);
    chevron->add_css_class("ur-label-faint");
    chevron->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*chevron);
    grid->append(*chevron);
    locationRow_->set_child(*grid);
  }
  locationRow_->signal_clicked().connect([this] {
    if (on_open_locations) on_open_locations();
  });
  // a Button whose child is a Box has NO automatic name; ApplyLocationRow
  // re-writes it with the provider on every location reading
  kit::SetAccessibleLabel(*locationRow_,
                          Glib::ustring(T_("selected_provider", "Selected provider")) + ", " +
                              locationText_->get_text());
  paneAContent_->append(*locationRow_);

  // 2.4 the connect action: filled = there is something to do, outlined =
  // you are fine (the fill IS a status channel)
  connectBtn_ = Gtk::make_managed<Gtk::Button>(T_("connect", "Connect"));
  connectBtn_->add_css_class("ur-pane-primary");
  connectBtn_->signal_clicked().connect([this] {
    if (on_toggle_connect) on_toggle_connect();
  });
  paneAContent_->append(*connectBtn_);

  // "More options" disclosure (Simple only) gating provide/options/peers
  moreOptionsToggle_ = Gtk::make_managed<Gtk::Button>(
      T_("more_options", "More options"));
  moreOptionsToggle_->add_css_class("ur-pane-row");
  moreOptionsToggle_->add_css_class("ur-pane-row-36");  // §2.7 MinHeight 36
  moreOptionsToggle_->set_size_request(-1, kListRowHeight);
  moreOptionsToggle_->signal_clicked().connect([this] {
    moreOptionsExpanded_ = !moreOptionsExpanded_;
    ApplyMoreOptionsVisibility();
  });
  paneAContent_->append(*moreOptionsToggle_);

  moreOptionsHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  paneAContent_->append(*moreOptionsHost_);

  // provide group: the indicator dot + the 4-item control mode segmented row
  auto provideHeader = kit::MakePaneGroupHeader(T_("provide", "Provide"));
  moreOptionsHost_->append(*provideHeader.root);
  {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_start(12);
    row->set_margin_end(12);
    row->set_margin_top(8);
    row->set_margin_bottom(8);
    provideDot_ = Gtk::make_managed<Gtk::Label>();
    provideDot_->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*provideDot_);
    row->append(*provideDot_);
    auto* segmented = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    segmented->add_css_class("linked");
    segmented->set_hexpand(true);
    provideAuto_ = Gtk::make_managed<Gtk::ToggleButton>(T_("auto", "Auto"));
    provideAlways_ = Gtk::make_managed<Gtk::ToggleButton>(T_("always", "Always"));
    provideNetwork_ = Gtk::make_managed<Gtk::ToggleButton>(T_("network", "Network"));
    provideNever_ = Gtk::make_managed<Gtk::ToggleButton>(T_("never", "Never"));
    provideAlways_->set_group(*provideAuto_);
    provideNetwork_->set_group(*provideAuto_);
    provideNever_->set_group(*provideAuto_);
    provideNever_->set_active(true);  // providing is opt-in
    for (Gtk::ToggleButton* button :
         {provideAuto_, provideAlways_, provideNetwork_, provideNever_}) {
      button->set_hexpand(true);
      segmented->append(*button);
      button->signal_toggled().connect([this, button] {
        if (button->get_active()) ApplyProvideControlMode();
      });
    }
    row->append(*segmented);
    moreOptionsHost_->append(*row);
  }
  discoverableText_ = Gtk::make_managed<Gtk::Label>();
  discoverableText_->add_css_class("ur-caption");
  discoverableText_->set_xalign(0);
  discoverableText_->set_margin_start(12);
  discoverableText_->set_margin_end(12);
  discoverableText_->set_margin_bottom(8);
  discoverableText_->set_wrap(true);
  CapNatural(discoverableText_, 32);
  moreOptionsHost_->append(*discoverableText_);

  // connect options: the four toggle rows, each writing through SdkHost with
  // an echo guard (the load writes the same control)
  auto optionsHeader = kit::MakePaneGroupHeader(T_("connect_options", "Connect options"));
  moreOptionsHost_->append(*optionsHeader.root);

  // the connection-mode segmented control (§2.8): Auto | Web | Streaming, the
  // window_type half of the PerformanceProfile
  {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_start(8);
    row->set_margin_end(8);
    row->set_margin_top(4);
    row->set_margin_bottom(4);
    auto* segmented = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    segmented->add_css_class("linked");
    segmented->set_hexpand(true);
    modeAuto_ = Gtk::make_managed<Gtk::ToggleButton>(T_("window_type_auto", "Auto"));
    modeWeb_ = Gtk::make_managed<Gtk::ToggleButton>(T_("window_type_quality", "Web"));
    modeStreaming_ = Gtk::make_managed<Gtk::ToggleButton>(T_("window_type_speed", "Streaming"));
    modeWeb_->set_group(*modeAuto_);
    modeStreaming_->set_group(*modeAuto_);
    modeAuto_->set_active(true);  // a nil profile IS Auto
    for (Gtk::ToggleButton* button : {modeAuto_, modeWeb_, modeStreaming_}) {
      button->set_hexpand(true);
      segmented->append(*button);
      button->signal_toggled().connect([this, button] {
        // one push per SELECTION, not per member toggling off
        if (button->get_active()) OnConnectionModeChanged();
      });
    }
    row->append(*segmented);
    moreOptionsHost_->append(*row);
  }

  auto addToggleRow = [this](const Glib::ustring& title, bool initial,
                             std::function<void(bool)> apply) {
    auto row = kit::MakePaneTwoLineRow(title, {}, 40);
    auto* toggle = Gtk::make_managed<Gtk::Switch>();
    toggle->set_valign(Gtk::Align::CENTER);
    toggle->set_active(initial);
    kit::SetAccessibleLabel(*toggle, title);
    toggle->property_active().signal_changed().connect([this, toggle, apply] {
      // echo guard: a feed-driven write must not travel back to the SDK
      if (updatingControls_) return;
      apply(toggle->get_active());
    });
    row.trailing->append(*toggle);
    moreOptionsHost_->append(*row.root);
    return toggle;
  };
  // The three PerformanceProfile toggles ride ONE writer
  // (PushPerformanceProfile) exactly as ConnectDrawer::ApplyControls does:
  // SdkHost::GetPerformanceProfile/SetPerformanceProfile are the accessors and
  // have been since the drawer shipped.
  fixedIpToggle_ = addToggleRow(T_("fixed_ip", "Fixed IP"), false,
                                [this](bool) { PushPerformanceProfile(); });
  fixedIpToggle_->set_sensitive(false);  // Auto pins the window size: no fixed ip
  // Strong Anonymization is the INVERSE of allow_direct
  anonToggle_ = addToggleRow(T_("strong_anonymization", "Strong Anonymization"), true,
                             [this](bool) { PushPerformanceProfile(); });
  pqeToggle_ = addToggleRow(T_("post_quantum_encryption", "Post Quantum Encryption"), false,
                            [this](bool) { PushPerformanceProfile(); });
  blockerToggle_ = addToggleRow(T_("block_ads_and_trackers", "Block ads and trackers"),
                                host_.GetBlockerEnabled(),
                                [this](bool on) { host_.SetBlockerEnabled(on); });
  // the kill switch is !routeLocal (apple SettingsForm parity)
  killSwitchToggle_ = addToggleRow(T_("kill_switch", "Kill switch"), !host_.GetRouteLocal(),
                                   [this](bool on) { host_.SetRouteLocal(!on); });

  // §2.8 network peers: the count line over the peer rows. A group header with
  // nothing under it is exactly the HOLE §8 forbids — the group is either a
  // list or a one-row sentence.
  auto peersHeader = kit::MakePaneGroupHeader(T_("network_peers", "Network peers"));
  peersMeta_ = peersHeader.meta;
  moreOptionsHost_->append(*peersHeader.root);

  peersLine_ = Gtk::make_managed<Gtk::Button>();
  peersLine_->add_css_class("ur-pane-row");
  peersLine_->add_css_class("ur-pane-row-36");
  peersLine_->set_size_request(-1, kListRowHeight);
  {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    peersDot_ = Gtk::make_managed<Gtk::Label>();
    peersDot_->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*peersDot_);
    line->append(*peersDot_);
    peersText_ = Gtk::make_managed<Gtk::Label>();
    peersText_->add_css_class("ur-key");
    peersText_->set_xalign(0);
    peersText_->set_hexpand(true);
    peersText_->set_ellipsize(Pango::EllipsizeMode::END);
    CapNatural(peersText_, 30);
    kit::MarkDecorative(*peersText_);  // the BUTTON carries the name
    line->append(*peersText_);
    peersLine_->set_child(*line);
  }
  // the same chooser the provider row opens
  peersLine_->signal_clicked().connect([this] {
    if (on_open_locations) on_open_locations();
  });
  moreOptionsHost_->append(*peersLine_);

  peersHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  moreOptionsHost_->append(*peersHost_);

  append(*paneA_.root);
}

// ---- PANE B: activity --------------------------------------------------------

void ConnectPage::BuildPaneB() {
  paneB_ = kit::MakePane(T_("activity", "Activity"));
  paneB_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*paneB_.root, T_("activity", "Activity"));

  // 3.2 the header chart: the remote route, 150 tall, inside a pane row so it
  // carries the 12px inset and the bottom hairline and is CLIPPED to its host
  // (an unclipped drawing bleeds across the 1px rule into pane C).
  auto* chartRow = kit::MakePaneRow(kRemoteChartHeight);
  remoteChart_ = Gtk::make_managed<TransferChart>(T_("remote", "Remote"),
                                                  TransferChart::Route::Remote, kUrGreen, kUrPink);
  remoteChart_->set_hexpand(true);
  remoteChart_->set_vexpand(true);
  if (auto* inner = RowInner(chartRow)) inner->append(*remoteChart_);
  paneB_.content->append(*chartRow);

  // 3.3 the connections group header; its meta is the FULL feed count even
  // though the list caps at 200 rows
  auto connectionsHeader = kit::MakePaneGroupHeader(T_("connections", "Connections"));
  connectionsCount_ = connectionsHeader.meta;
  paneB_.content->append(*connectionsHeader.root);

  // 3.4 the list area: the list and the empty reading SWAP, never coexist.
  // ONLY this row scrolls (§3: header/chart/group header are auto rows, the
  // list is the star row) — the pane's own scroller would otherwise drag the
  // live chart and the host count off the top the moment the feed is long
  // enough to need scrolling. A ScrolledWindow does not propagate its child's
  // natural height, so nesting it inside the pane scroller is stable: the
  // outer never scrolls, the inner takes the leftover height.
  connectionsArea_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  connectionsArea_->set_vexpand(true);
  connectionsHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  connectionsHost_->set_vexpand(false);
  connectionsHost_->set_valign(Gtk::Align::START);
  connectionsArea_->append(*connectionsHost_);
  // the empty state is a centred glyph + sentence INSIDE the full-height pane,
  // never a short card
  connectionsEmpty_ = kit::MakeEmptyState(
      "network-transmit-receive-symbolic",
      T_("contracts_appear_connected", "Contracts appear here while connected."));
  connectionsEmpty_->set_vexpand(true);
  connectionsEmpty_->set_valign(Gtk::Align::CENTER);
  connectionsArea_->append(*connectionsEmpty_);
  auto* connectionsScroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  connectionsScroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  connectionsScroll->set_child(*connectionsArea_);
  connectionsScroll->set_vexpand(true);
  paneB_.content->append(*connectionsScroll);

  append(*paneB_.root);
}

// ---- PANE C: statistics / inspector ------------------------------------------

void ConnectPage::BuildPaneC() {
  paneC_ = kit::MakePane(T_("client_statistics", "Client statistics"));
  paneC_.root->set_size_request(kPaneCWidth, -1);
  // §0 column table: statistics is a FIXED 380 rail and activity is the `*`
  // column that takes all the surplus. MakePane sets its title hexpand, and a
  // child's expand flag PROPAGATES to its ancestors unless one of them sets
  // its own — without this pin GtkBox splits every surplus pixel between the
  // two expanding panes and the rail balloons.
  paneC_.root->set_hexpand(false);
  kit::SetAccessibleLabel(*paneC_.root, T_("client_statistics", "Client statistics"));

  // the inspector sits ABOVE the charts: the selection is the most specific
  // thing on screen (Advanced Mode only)
  BuildInspectorGroup();

  // 4.2 the two chart slots, 132 each
  auto addChart = [this](TransferChart*& slot, const char* legend, TransferChart::Route route,
                         const Rgba& byteColor, const Rgba& packetColor) {
    auto* row = kit::MakePaneRow(kPaneCChartHeight);
    slot = Gtk::make_managed<TransferChart>(legend, route, byteColor, packetColor);
    slot->set_hexpand(true);
    slot->set_vexpand(true);
    if (auto* inner = RowInner(row)) inner->append(*slot);
    paneC_.content->append(*row);
  };
  addChart(blockedChart_, T_("blocked", "Blocked"), TransferChart::Route::Block, kUrCoral,
           kUrMutedCoral);
  addChart(localChart_, T_("local", "Local"), TransferChart::Route::Local, kUrGreen, kUrPink);

  BuildDataUsageGroup();
  BuildContractsGroup();
  BuildSplitRulesGroup();
  BuildDnsGroup();

  append(*paneC_.root);
}

// 4.1 the connection inspector — Advanced Mode ONLY; in Normal mode the whole
// group is GONE, not empty.
void ConnectPage::BuildInspectorGroup() {
  inspectorGroup_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  kit::SetAccessibleLabel(*inspectorGroup_, T_("adv_inspector", "Inspector"));

  auto header = kit::MakePaneGroupHeader(T_("adv_inspector", "Inspector"));
  inspectorClear_ = Gtk::make_managed<Gtk::Button>();
  inspectorClear_->set_icon_name("edit-clear-symbolic");
  inspectorClear_->add_css_class("ur-pane-action");
  // a glyph is not a name
  kit::SetAccessibleLabel(*inspectorClear_, T_("adv_clear_selection", "Clear selection"));
  inspectorClear_->set_visible(false);
  inspectorClear_->signal_clicked().connect([this] {
    selectedConnectionId_.clear();
    ApplyConnectionSelectionVisuals();  // repaint, never rebuild
    ApplyInspector();
  });
  header.trailing->append(*inspectorClear_);
  inspectorGroup_->append(*header.root);

  // the headline: what it is, then the verdict with its dot
  auto* headline = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3);
  headline->set_margin_start(12);
  headline->set_margin_end(12);
  headline->set_margin_top(10);
  headline->set_margin_bottom(10);
  inspectorTitle_ = Gtk::make_managed<Gtk::Label>();
  inspectorTitle_->add_css_class("ur-row-title");
  inspectorTitle_->set_xalign(0);
  inspectorTitle_->set_ellipsize(Pango::EllipsizeMode::END);
  CapNatural(inspectorTitle_, 26);
  {
    Pango::AttrList attrs;
    auto size = Pango::Attribute::create_attr_size_absolute(15 * PANGO_SCALE);
    attrs.insert(size);
    auto weight = Pango::Attribute::create_attr_weight(Pango::Weight::SEMIBOLD);
    attrs.insert(weight);
    inspectorTitle_->set_attributes(attrs);
  }
  headline->append(*inspectorTitle_);
  auto* verdictRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 7);
  inspectorDot_ = Gtk::make_managed<Gtk::Label>();
  inspectorDot_->set_valign(Gtk::Align::CENTER);
  kit::MarkDecorative(*inspectorDot_);
  verdictRow->append(*inspectorDot_);
  inspectorVerdict_ = Gtk::make_managed<Gtk::Label>();
  inspectorVerdict_->add_css_class("ur-key");
  // §4.1: the verdict line is the KEY role at 12px — the supporting-text voice,
  // one step under the 13px value rows beneath it
  inspectorVerdict_->add_css_class("ur-text-12");
  inspectorVerdict_->set_xalign(0);
  inspectorVerdict_->set_wrap(true);
  CapNatural(inspectorVerdict_, 34);
  verdictRow->append(*inspectorVerdict_);
  headline->append(*verdictRow);
  inspectorGroup_->append(*headline);
  inspectorGroup_->append(*kit::MakeDivider());

  inspectorRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  inspectorGroup_->append(*inspectorRows_);

  inspectorGroup_->set_visible(false);  // Normal mode: gone, not empty
  paneC_.content->append(*inspectorGroup_);
}

// 4.3 data_usage: the provider-count entry (only with a session) over the
// session figures.
void ConnectPage::BuildDataUsageGroup() {
  auto header = kit::MakePaneGroupHeader(T_("data_usage", "Data usage"));
  paneC_.content->append(*header.root);

  liveStatsGroup_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  providerCountLine_ = Gtk::make_managed<Gtk::Button>();
  providerCountLine_->add_css_class("ur-pane-row");
  // §4.3 MinHeight 34: the CSS floor for a button row is 40, so the row must
  // pin BOTH or it opens the Data usage group ~7px taller than the five 34px
  // session rows right under it
  providerCountLine_->add_css_class("ur-pane-row-34");
  providerCountLine_->set_size_request(-1, kKeyValueRowHeight);
  providerCountText_ = Gtk::make_managed<Gtk::Label>();
  providerCountText_->add_css_class("ur-key");
  providerCountText_->set_xalign(0);
  providerCountText_->set_hexpand(true);
  providerCountText_->set_ellipsize(Pango::EllipsizeMode::END);
  CapNatural(providerCountText_, 30);
  providerCountLine_->set_child(*providerCountText_);
  providerCountLine_->signal_clicked().connect([this] {
    if (!connected_) return;  // the globe has nothing to plot without a session
    if (on_open_provider_locations) on_open_provider_locations();
  });
  liveStatsGroup_->append(*providerCountLine_);
  liveStatsGroup_->set_visible(false);  // collapsed with no session: no blank rows
  paneC_.content->append(*liveStatsGroup_);

  sessionHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  paneC_.content->append(*sessionHost_);
}

// 4.4 contracts: the group is the entry point to the (reused) contracts sheet.
void ConnectPage::BuildContractsGroup() {
  auto header = kit::MakePaneGroupHeader(T_("client_contracts", "Client contracts"));
  auto* open = Gtk::make_managed<Gtk::Button>();
  open->set_icon_name("go-next-symbolic");
  open->add_css_class("ur-pane-action");
  kit::SetAccessibleLabel(*open, T_("client_contracts", "Client contracts"));
  open->signal_clicked().connect([this] { OpenContractsSheet(); });
  header.trailing->append(*open);
  paneC_.content->append(*header.root);

  contractsHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  paneC_.content->append(*contractsHost_);
}

// 4.5 split rules: count in the header, rules under it, the sheet behind the
// trailing action.
void ConnectPage::BuildSplitRulesGroup() {
  auto header = kit::MakePaneGroupHeader(T_("split_rules", "Split rules"));
  splitRuleCountText_ = header.meta;
  auto* open = Gtk::make_managed<Gtk::Button>();
  open->set_icon_name("go-next-symbolic");
  open->add_css_class("ur-pane-action");
  kit::SetAccessibleLabel(*open, T_("split_rules", "Split rules"));
  open->signal_clicked().connect([this] { OpenSplitRulesSheet(); });
  header.trailing->append(*open);
  paneC_.content->append(*header.root);

  splitRulesHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  paneC_.content->append(*splitRulesHost_);
}

ConnectPage::DnsStatusRow ConnectPage::MakeDnsStatusRow(const Glib::ustring& title) {
  DnsStatusRow row;
  auto* host = kit::MakePaneRow(kKeyValueRowHeight);
  auto* inner = RowInner(host);
  row.root = host;
  if (!inner) return row;
  inner->set_spacing(8);
  // the off tone is faint at 40% — a CSS pair that exists (Ui.cpp) and carries
  // no competing pane-row class, so it wins cleanly
  row.dot = Gtk::make_managed<Gtk::Label>("●");
  row.dot->add_css_class("ur-dot-off");
  row.dot->add_css_class("ur-caption-11");
  row.dot->set_valign(Gtk::Align::CENTER);
  kit::MarkDecorative(*row.dot);
  inner->append(*row.dot);
  auto* label = Gtk::make_managed<Gtk::Label>(title);
  label->add_css_class("ur-key");
  label->set_xalign(0);
  label->set_hexpand(true);
  label->set_valign(Gtk::Align::CENTER);
  label->set_ellipsize(Pango::EllipsizeMode::END);
  CapNatural(label, 22);
  kit::MarkDecorative(*label);  // the state value re-announces "label, value"
  inner->append(*label);
  row.state = Gtk::make_managed<Gtk::Label>(T_("off", "Off"));
  row.state->add_css_class("ur-value");
  // §4.6: the state cell is the VALUE role at FontSize 12
  row.state->add_css_class("ur-text-12");
  row.state->add_css_class("dim-label");
  row.state->set_xalign(1);
  row.state->set_valign(Gtk::Align::CENTER);
  kit::SetAccessibleLabel(*row.state, title + ", " + T_("off", "Off"));
  inner->append(*row.state);
  return row;
}

// 4.6 custom DNS: the unapplied-recommendation pill, the four resolver
// readings, and the "unavailable" ROW they swap with.
void ConnectPage::BuildDnsGroup() {
  auto header = kit::MakePaneGroupHeader(T_("custom_dns", "Custom DNS"));
  dnsEditButton_ = Gtk::make_managed<Gtk::Button>();
  dnsEditButton_->set_icon_name("document-edit-symbolic");
  dnsEditButton_->add_css_class("ur-pane-action");
  kit::SetAccessibleLabel(*dnsEditButton_, T_("custom_dns", "Custom DNS"));
  dnsEditButton_->signal_clicked().connect([this] { OpenDnsSheet(); });
  // DnsSheet::Open() refuses to present with no settings to draft from, and a
  // control that cannot act must not hover, focus and depress as if it could
  // (ApplyDnsCard re-decides on every dns reading).
  dnsEditButton_->set_sensitive(false);
  header.trailing->append(*dnsEditButton_);
  paneC_.content->append(*header.root);

  // the coral-subtle capsule (linux-reuse.md 2.6: "pill unchanged") riding a
  // 34px pane row
  dnsPillRow_ = kit::MakePaneRow(kKeyValueRowHeight);
  if (auto* inner = RowInner(dnsPillRow_)) {
    auto* pill = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    pill->add_css_class("ur-dns-pill");
    pill->set_halign(Gtk::Align::START);
    pill->set_valign(Gtk::Align::CENTER);
    dnsPillDot_ = Gtk::make_managed<Gtk::Label>("●");
    dnsPillDot_->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*dnsPillDot_);
    pill->append(*dnsPillDot_);
    dnsPillText_ = Gtk::make_managed<Gtk::Label>();
    dnsPillText_->set_xalign(0);
    dnsPillText_->set_ellipsize(Pango::EllipsizeMode::END);
    CapNatural(dnsPillText_, 34);
    pill->append(*dnsPillText_);
    inner->append(*pill);
  }
  dnsPillRow_->set_visible(false);
  paneC_.content->append(*dnsPillRow_);

  dnsRowsPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  dnsDohRow_ = MakeDnsStatusRow(T_("dns_over_https", "DNS over HTTPS"));
  dnsRowsPanel_->append(*dnsDohRow_.root);
  dnsUnencryptedRow_ = MakeDnsStatusRow(T_("unencrypted_dns", "Unencrypted DNS"));
  dnsRowsPanel_->append(*dnsUnencryptedRow_.root);
  dnsLocalRow_ = MakeDnsStatusRow(T_("local_dns", "Local DNS"));
  dnsRowsPanel_->append(*dnsLocalRow_.root);
  dnsFallbackRow_ = MakeDnsStatusRow(T_("local_dns_fallback", "Local DNS fallback"));
  dnsRowsPanel_->append(*dnsFallbackRow_.root);
  paneC_.content->append(*dnsRowsPanel_);

  // "no settings / no device" is a DISTINCT reading from "everything off" —
  // the ROW swaps, not just the text
  {
    const Glib::ustring text = T_("dns_settings_unavailable", "DNS settings unavailable");
    auto row = MakeEmptySentenceRow(text, kKeyValueRowHeight);
    // TODO(theme): .ur-key.ur-label-faint — UrTheme (APPLICATION+1) carries no
    // faint tone beside .ur-key, so the faint foreground rides pango markup.
    row.key->set_markup("<span foreground='" + std::string(kDotFaint) + "'>" +
                        Glib::Markup::escape_text(text) + "</span>");
    dnsUnavailableRow_ = row.root;
  }
  dnsUnavailableRow_->set_visible(false);
  paneC_.content->append(*dnsUnavailableRow_);
}

// ---- the one status writer ----------------------------------------------------

void ConnectPage::ApplyConnectStatus() {
  // RenderHealth reconciliation: the button label and the status line must
  // derive from ONE reading, so the hero can never lag the line above it.
  const std::string status = UpperCopy(connectStatus_);
  const bool connecting = (status == "CONNECTING" || status == "DESTINATION_SET");
  const bool failed = (status == "CONNECT_FAILED");

  Glib::ustring text;
  const char* dot = kDotIdle;
  ConnectCanvas::State heroState = ConnectCanvas::State::Disconnected;
  bool showNotProtected = false;

  if (stats_.insufficientBalance) {
    // balance overrides the connection reading (processing wins over blocked)
    text = T_("insufficient_balance_add_balance_or_plan",
              "Insufficient balance — add balance or a plan");
    dot = kDotCoral;
    heroState = ConnectCanvas::State::Error;
  } else if (connected_ && stats_.connected) {
    text = T_("connected", "Connected");
    dot = kDotGreen;
    heroState = ConnectCanvas::State::Connected;
  } else if (connecting) {
    text = T_("connecting_status_indicator", "Connecting to providers");
    dot = kDotConnecting;
    heroState = ConnectCanvas::State::Connecting;
  } else if (failed) {
    text = T_("conn_failed", "Couldn't connect");
    dot = kDotCoral;
    heroState = ConnectCanvas::State::Error;
  } else {
    text = T_("disconnected", "Disconnected");
    dot = kDotIdle;
    heroState = ConnectCanvas::State::Disconnected;
    showNotProtected = true;
  }

  statusText_->set_text(text);
  statusDot_->set_markup(std::string("<span size='") + std::to_string(10 * PANGO_SCALE) +
                         "' foreground='" + dot + "'>●</span>");
  // Simple only: Advanced omits the restatement deliberately
  kit::SetTextOrCollapse(*protectionText_,
                         (!advanced_ && showNotProtected)
                             ? T_("conn_not_protected",
                                  "Your internet traffic is not protected.")
                             : "");
  kit::SetTextOrCollapse(*daemonNoticeText_, daemonNotice_);

  canvas_->SetState(heroState);
  // the hero's accessible name IS the current status text (its content is a
  // decorative canvas, so it gets no automatic name)
  kit::SetAccessibleLabel(*hero_, text);

  // the action: filled Connect vs outlined Disconnect (four channels — word,
  // fill, dot, status line)
  const bool isDisconnect = connected_ || stats_.connected || connecting;
  connectBtn_->set_label(isDisconnect ? T_("disconnect", "Disconnect")
                                      : T_("connect", "Connect"));
  connectBtn_->remove_css_class(isDisconnect ? "ur-pane-primary" : "ur-pane-secondary");
  connectBtn_->add_css_class(isDisconnect ? "ur-pane-secondary" : "ur-pane-primary");
}

void ConnectPage::SetDaemonNotice(const Glib::ustring& notice) {
  daemonNotice_ = notice;
  ApplyConnectStatus();
}

void ConnectPage::SetConnected(bool connected) {
  connected_ = connected;
  ApplyConnectStatus();
}

void ConnectPage::SetConnectionStatus(const std::string& status) {
  connectStatus_ = status;
  ApplyConnectStatus();
}

void ConnectPage::ApplyStats(const LiveStats& stats) {
  stats_ = stats;
  // the hero's grid feed — fed unconditionally, empty list included (the
  // canvas renders it as the bare lattice; an empty grid is a NORMAL state)
  canvas_->SetGrid(stats.gridPoints, stats.gridWidth, stats.gridHeight);

  // 3.1 pane B header carries the live throughput; with no session the line
  // COLLAPSES entirely rather than reading "0 bps"
  if (paneB_.meta) {
    kit::SetTextOrCollapse(*paneB_.meta,
                           stats.connected
                               ? Glib::ustring("↓ " + FormatBitRate(stats.downBitsPerSecond) +
                                               "   ↑ " + FormatBitRate(stats.upBitsPerSecond))
                               : Glib::ustring());
  }
  // provide indicator: solid dot = Network tier, ring = Public
  if (provideDot_) {
    const char* glyph = "●";
    const char* color = "#FF6C58";
    switch (stats.provideMode) {
      case 3: glyph = "◉"; color = stats.providePaused ? "#F5C242" : "#87FB67"; break;
      case 1:
      case 2: color = "#87FB67"; break;
      default: break;
    }
    provideDot_->set_markup(std::string("<span foreground='") + color + "'>" + glyph +
                            "</span>");
  }
  if (discoverableText_) {
    discoverableText_->set_text(
        stats.provideEnabled && stats.provideHasNetworkKey
            ? T_("device_discoverable", "This device is discoverable")
            : T_("device_not_discoverable",
                 "Enable provide mode to make this device discoverable"));
  }
  // §2: pane A's header strip carries PaneATitle and nothing else — the
  // provider count is pane C's ProviderCountLine (§4.3) and rendering it twice
  // puts the same sentence on screen in two places (and ellipsizes the
  // letterspaced "Connect" title against it in the 330dip rail).
  //
  // The connected country (dns pill) and the selected provider are NOT read
  // here either: SelectedLocation takes SdkHost's mutex — the one StartTunnel
  // and Logout hold across a whole daemon handshake — and parses the connect
  // location per call, while ApplyStats rides every throughput sample. Both
  // ride the location reading instead (RefreshFeeds / DrawerEvent::Location).

  // 4.3 the provider-count entry: gated on the AGGREGATE reading, so
  // "Connected to N providers" can never appear under "Finding providers…"
  if (liveStatsGroup_ && providerCountText_) {
    liveStatsGroup_->set_visible(stats.connected);
    const bool show = stats.connected && connected_;
    kit::SetTextOrCollapse(
        *providerCountText_,
        show ? Glib::ustring(Format(TN_("connected_provider_count", "Connected to {} provider",
                                        "Connected to {} providers",
                                        static_cast<unsigned long>(stats.providerCount)),
                                    stats.providerCount))
             : Glib::ustring());
    if (providerCountLine_) {
      kit::SetAccessibleLabel(*providerCountLine_, providerCountText_->get_text());
      // A row that cannot act must not claim it can. NOTE: while
      // on_open_provider_locations is unassigned this reads as a permanently
      // greyed row — see the TODO(wiring) on the callback in ConnectPage.hpp;
      // the globe sheet cannot be owned here (it needs MainWindow's
      // LocationOverrideController).
      providerCountLine_->set_sensitive(show && on_open_provider_locations != nullptr);
    }
  }

  ApplySessionRows();
  ApplySessionCardsVisibility();
  ApplyConnectStatus();
}

// ---- pane B: the routing-decision list ----------------------------------------

void ConnectPage::ApplyConnectionsList() {
  if (!connectionsHost_) return;
  // drop the row handles BEFORE the widgets they point at go away
  connectionRows_.clear();
  connectionIds_.clear();
  connectionNames_.clear();
  RemoveAllChildren(*connectionsHost_);

  const size_t total = blockActions_ ? blockActions_->size() : 0;
  if (blockActions_) {
    size_t shown = 0;
    // NEWEST FIRST. The Linux feed is delivered oldest-first
    // (BlockActionViewController::getBlockActions is the window in arrival
    // order), so the list is walked from the back — the already-shipped
    // consumer does the same (SplitRulesSheet: "live activity, newest first").
    // Read forward, the 200-row cap would keep the 200 OLDEST decisions and no
    // newly contacted host would ever appear again on a busy session.
    for (auto it = blockActions_->rbegin(); it != blockActions_->rend(); ++it) {
      if (shown >= kMaxConnectionRows) break;  // a cap, not a scroll budget
      const urnet::BlockAction& action = *it;
      std::string title = BlockActionTitle(action);
      if (title.empty()) title = T_("unknown", "unknown");
      const std::string meta = FormatByteCountCompact(action.ByteCount) + "   " +
                               FormatCountCompact(action.PacketCount) + " pkt";
      const char* dot = VerdictDot(action);
      const std::string id = action.BlockActionId.value_or(std::string());
      // the dot is decorative: the NAME is the only place the color's meaning
      // exists for a screen reader
      const Glib::ustring name = Glib::ustring(title) + ", " + VerdictWord(action);
      if (advanced_) {
        // Advanced: EVERY row is a real button — clickable, in tab order,
        // Enter/Space. A row whose id the feed omitted cannot be inspected, so
        // it renders inert (disabled) rather than looking identical to its
        // neighbours and swallowing the click.
        auto row = kit::MakePaneListRowButton(kListRowHeight);
        row.root->add_css_class("ur-pane-row-36");  // the CSS floor is 40
        row.title->set_text(title);
        row.meta->set_text(meta);
        row.dot->set_markup(DotMarkup(7, dot));
        // the row's own name is the whole announcement: its parts must not be
        // read out again after it
        kit::MarkDecorative(*row.title);
        kit::MarkDecorative(*row.meta);
        kit::SetAccessibleLabel(*row.root, name);
        row.root->set_sensitive(!id.empty());
        row.root->signal_clicked().connect(
            [this, id] { SelectConnection(id); });  // by id, captured by value
        connectionsHost_->append(*row.root);
        connectionRows_.push_back(row);
        connectionIds_.push_back(id);
        connectionNames_.push_back(name);
      } else {
        // Normal: static rows — 200 tab stops on the way to Connect is hostile
        auto row = kit::MakePaneListRow(kListRowHeight);
        row.title->set_text(title);
        row.meta->set_text(meta);
        row.dot->set_markup(DotMarkup(7, dot));
        kit::MarkDecorative(*row.title);
        kit::MarkDecorative(*row.meta);
        kit::SetAccessibleLabel(*row.root, name);
        connectionsHost_->append(*row.root);
      }
      ++shown;
    }
  }

  if (connectionsCount_) {
    // ALWAYS the full feed count, even though rendering caps at 200 rows
    kit::SetTextOrCollapse(
        *connectionsCount_,
        blockActions_ ? Glib::ustring(Format(TN_("host_count", "{} host", "{} hosts",
                                                 static_cast<unsigned long>(total)),
                                             total))
                      : Glib::ustring());
  }
  ApplySessionCardsVisibility();
  ApplyConnectionSelectionVisuals();
  ApplyInspector();  // a selection that aged out of the feed must SAY so
}

void ConnectPage::SelectConnection(const std::string& id) {
  // clicking the selected row clears it (a toggle)
  selectedConnectionId_ = (selectedConnectionId_ == id) ? std::string() : id;
  ApplyConnectionSelectionVisuals();
  ApplyInspector();
}

void ConnectPage::ApplyConnectionSelectionVisuals() {
  // repaint WITHOUT rebuilding: a rebuild would destroy keyboard focus
  const size_t rows = std::min(connectionRows_.size(),
                               std::min(connectionIds_.size(), connectionNames_.size()));
  for (size_t i = 0; i < rows; ++i) {
    const bool selected =
        !selectedConnectionId_.empty() && connectionIds_[i] == selectedConnectionId_;
    kit::SetPaneListRowSelected(connectionRows_[i], selected);
    if (!connectionRows_[i].root) continue;
    // selection rides THREE channels: the fill step and the 2px accent bar
    // (both above), and the announced name gaining the suffix
    kit::SetAccessibleLabel(
        *connectionRows_[i].root,
        selected ? connectionNames_[i] + ", " + T_("adv_selected", "selected")
                 : connectionNames_[i]);
  }
}

void ConnectPage::ApplySessionCardsVisibility() {
  if (!connectionsHost_ || !connectionsEmpty_) return;
  // the list and the sentence SWAP; the connected flag gates which
  const bool showList = stats_.connected && connectionsHost_->get_first_child() != nullptr;
  connectionsHost_->set_visible(showList);
  connectionsEmpty_->set_visible(!showList);
}

// ---- pane C: the session figures ----------------------------------------------

void ConnectPage::ApplySessionRows() {
  if (!sessionHost_) return;
  RemoveAllChildren(*sessionHost_);
  auto add = [this](const Glib::ustring& key, const Glib::ustring& value) {
    auto row = kit::MakePaneKeyValueRow(key, value, kKeyValueRowHeight);
    CapNatural(row.key, 18);
    CapNatural(row.value, 16);
    sessionHost_->append(*row.root);
    return row;
  };
  // an em dash is "no session", NOT a zero
  const Glib::ustring none = T_("adv_na", "—");
  add(T_("remote", "Remote"),
      stats_.connected ? Glib::ustring("↓ " + FormatBitRate(stats_.downBitsPerSecond)) : none);
  add(T_("local", "Local"),
      stats_.connected ? Glib::ustring("↑ " + FormatBitRate(stats_.upBitsPerSecond)) : none);
  add(T_("allowed", "Allowed"),
      blockStats_ ? Glib::ustring(FormatCountCompact(blockStats_->AllowedCount)) : none);
  add(T_("blocked", "Blocked"),
      blockStats_ ? Glib::ustring(FormatCountCompact(blockStats_->BlockedCount)) : none);
  add(T_("connections", "Connections"),
      blockActions_ ? Glib::ustring(FormatCountCompact(
                          static_cast<int64_t>(blockActions_->size())))
                    : none);
  if (advanced_) {
    // Advanced sees through the simplification: the pre-clamp status.
    // TODO(sdk-wiring): LiveStats.rawConnectionStatus / LiveStats.rpcOnly — the
    // Linux LiveStats carries no rpc-only clamp, so this is the ONLY status the
    // page has; it is not relabelled as something it is not.
    add(T_("adv_raw_status", "Raw status"),
        stats_.connectionStatus.empty() ? Glib::ustring(T_("adv_none", "none"))
                                        : Glib::ustring(stats_.connectionStatus));
    // TODO(sdk-wiring): Sdk().ReadReliability() — no exits table is readable on
    // Linux, so the denominator is UNKNOWN, never 0.
    add(T_("adv_exits", "Exits"), T_("adv_none", "none"));
  }
}

// ---- pane C: contracts ---------------------------------------------------------

void ConnectPage::ApplyContractsList() {
  if (!contractsHost_) return;
  RemoveAllChildren(*contractsHost_);
  if (!contractRows_ || contractRows_->empty()) {
    // still a ROW on the grid — never a hole
    auto row = MakeEmptySentenceRow(
        T_("contracts_appear_connected", "Contracts appear here while connected."),
        kKeyValueRowHeight);
    contractsHost_->append(*row.root);
    return;
  }
  for (const auto& peer : *contractRows_) {
    auto row = kit::MakePaneListRow(kListRowHeight);
    const bool live = 0 < peer.LastActivityMillis && !peer.Closing;
    row.dot->set_markup(DotMarkup(7, live ? kDotGreen : kDotFaint));
    // Advanced shows the full client id and lets you select it
    row.title->set_text(advanced_ ? peer.ClientId : ShortId(peer.ClientId));
    // COPYABLE, not a tab stop: Gtk::Label::set_selectable also makes the
    // label focusable, which would inject one keyboard stop per open contract
    // into pane C (§3 states the doctrine for the sibling list). The spec asks
    // for text selection, and a mouse drag still selects with focus off.
    row.title->set_selectable(advanced_);
    row.title->set_can_focus(false);
    row.title->set_focus_on_click(false);
    CapNatural(row.title, 16);
    row.meta->set_text("↑ " + FormatByteCountCompact(peer.SendByteCount) + "   ↓ " +
                       FormatByteCountCompact(peer.ReceiveByteCount));
    CapNatural(row.meta, 18);
    // the row's name is the whole announcement (the full id, always)
    kit::MarkDecorative(*row.title);
    kit::MarkDecorative(*row.meta);
    kit::SetAccessibleLabel(*row.root,
                            Glib::ustring(T_("contract", "Contract")) + ", " + peer.ClientId);
    contractsHost_->append(*row.root);
  }
}

// ---- pane C: split rules --------------------------------------------------------

void ConnectPage::ApplySplitRuleCount() {
  size_t count = splitRules_ ? splitRules_->size() : 0;
  if (splitRuleCountText_) {
    kit::SetTextOrCollapse(
        *splitRuleCountText_,
        splitRules_ ? Glib::ustring(Format(TN_("split_rule_count", "{} split rule",
                                               "{} split rules",
                                               static_cast<unsigned long>(count)),
                                           count))
                    : Glib::ustring());
  }
  if (!splitRulesHost_) return;
  RemoveAllChildren(*splitRulesHost_);
  if (count == 0) {
    auto row = MakeEmptySentenceRow(
        T_("app_split_active_none", "No app split — all apps use the VPN"), kKeyValueRowHeight);
    splitRulesHost_->append(*row.root);
    return;
  }
  for (const auto& rule : *splitRules_) {
    auto row = kit::MakePaneListRow(kListRowHeight);
    const bool routeLocal = rule.RouteOverride && rule.RouteOverride->Local;
    // amber = sent around the tunnel, the same "not protected, on purpose"
    // color the connections table uses
    row.dot->set_markup(DotMarkup(7, routeLocal ? kDotAmber : kDotGreen));
    const size_t hosts = rule.Hosts ? rule.Hosts->size() : 0;
    row.title->set_text(hosts > 0 ? rule.Hosts->front() : std::string(T_("unknown", "unknown")));
    row.meta->set_text(hosts > 1 ? Glib::ustring(Format(TN_("host_count", "{} host", "{} hosts",
                                                            static_cast<unsigned long>(hosts)),
                                                        hosts))
                                 : Glib::ustring(routeLocal ? T_("local", "Local")
                                                            : T_("remote", "Remote")));
    CapNatural(row.title, 20);
    CapNatural(row.meta, 12);
    kit::SetAccessibleLabel(*row.root, row.title->get_text() + ", " + row.meta->get_text());
    // one fact, one node: the row's name already says both halves
    kit::MarkDecorative(*row.title);
    kit::MarkDecorative(*row.meta);
    splitRulesHost_->append(*row.root);
  }
}

// ---- pane C: custom dns ----------------------------------------------------------

void ConnectPage::ApplyDnsCard() {
  const bool present = dnsSettings_.has_value();
  if (dnsRowsPanel_) dnsRowsPanel_->set_visible(present);
  if (dnsUnavailableRow_) dnsUnavailableRow_->set_visible(!present);
  // the editor has nothing to draft from without settings (DnsSheet::Open
  // returns false and does not present) — say so on the control
  if (dnsEditButton_) dnsEditButton_->set_sensitive(present);
  ApplyDnsRecommendationPill();  // collapses with the rows
  if (!present) return;
  auto apply = [](DnsStatusRow& row, const Glib::ustring& label, bool on) {
    if (!row.dot || !row.state) return;
    row.dot->remove_css_class(on ? "ur-dot-off" : "ur-dot-on");
    row.dot->add_css_class(on ? "ur-dot-on" : "ur-dot-off");
    if (on) {
      row.state->add_css_class("ur-value-on");
    } else {
      row.state->remove_css_class("ur-value-on");
    }
    const Glib::ustring text = on ? T_("on", "On") : T_("off", "Off");
    row.state->set_text(text);
    kit::SetAccessibleLabel(*row.state, label + ", " + text);
  };
  apply(dnsDohRow_, T_("dns_over_https", "DNS over HTTPS"),
        dnsSettings_->EnableRemoteDoh || dnsSettings_->EnableLocalDoh);
  apply(dnsUnencryptedRow_, T_("unencrypted_dns", "Unencrypted DNS"),
        dnsSettings_->EnableRemoteDns || dnsSettings_->EnableLocalDns);
  apply(dnsLocalRow_, T_("local_dns", "Local DNS"),
        dnsSettings_->EnableLocalDoh || dnsSettings_->EnableLocalDns);
  apply(dnsFallbackRow_, T_("local_dns_fallback", "Local DNS fallback"),
        dnsSettings_->EnableFallback);
}

// iOS DnsRecommendationPill parity (and ConnectDrawer::RefreshDnsPill verbatim,
// with §6's pill keys): a regional recommendation NEVER falls through to the
// safe-defaults nudge.
void ConnectPage::ApplyDnsRecommendationPill() {
  if (!dnsPillRow_ || !dnsPillDot_ || !dnsPillText_) return;
  auto show = [this](const std::string& text, const std::string& countryCode) {
    if (countryCode.empty()) {
      dnsPillDot_->set_visible(false);
    } else {
      Rgba dot{0.5, 0.5, 0.5, 1.0};
      dot = ParseHexColor(urnet::getColorHex(countryCode), dot);
      dnsPillDot_->set_markup("<span foreground='" + HexForMarkup(dot) + "'>●</span>");
      dnsPillDot_->set_visible(true);
    }
    dnsPillText_->set_text(text);
    dnsPillRow_->set_visible(true);
  };

  // nothing to compare against: the rows already read "unavailable"
  if (!dnsSettings_) {
    dnsPillRow_->set_visible(false);
    return;
  }
  if (!countryCode_.empty()) {
    if (const auto recommended = urnet::getRecommendedDnsResolverSettings(countryCode_)) {
      if (!DnsSheet::SettingsEqual(*dnsSettings_, *recommended)) {
        const std::string country =
            !countryName_.empty() ? countryName_ : UpperCopy(countryCode_);
        show(Format(T_("dns_pill_recommended",
                       "There are unapplied recommended settings for {}"),
                    country),
             countryCode_);
      } else {
        dnsPillRow_->set_visible(false);  // already on the regional recommendation
      }
      return;  // never fall through to the default nudge
    }
  }
  if (const auto defaults = urnet::getDefaultDnsResolverSettings()) {
    if (!DnsSheet::SettingsEqual(*dnsSettings_, *defaults)) {
      show(T_("dns_pill_default", "The default safe settings are not applied"), "");
      return;
    }
  }
  dnsPillRow_->set_visible(false);
}

// ---- pane C: the connection inspector -------------------------------------------

void ConnectPage::ApplyInspectorVisibility() {
  if (inspectorGroup_) inspectorGroup_->set_visible(advanced_);
}

void ConnectPage::ApplyInspector() {
  if (!inspectorGroup_ || !inspectorRows_) return;
  RemoveAllChildren(*inspectorRows_);

  const urnet::BlockAction* action = nullptr;
  if (!selectedConnectionId_.empty() && blockActions_) {
    for (const auto& candidate : *blockActions_) {
      if (candidate.BlockActionId.value_or(std::string()) == selectedConnectionId_) {
        action = &candidate;
        break;
      }
    }
  }

  if (!action) {
    // two DISTINGUISHABLE empty readings: nothing picked vs picked-and-gone
    inspectorTitle_->set_text(selectedConnectionId_.empty()
                                  ? T_("adv_no_selection", "No connection selected")
                                  : T_("adv_selection_gone",
                                       "That connection is no longer listed"));
    inspectorDot_->set_markup(DotMarkup(8, kDotFaint));
    inspectorVerdict_->set_text(
        T_("adv_select_a_row", "Select a row in Activity to inspect it"));
    if (inspectorClear_) inspectorClear_->set_visible(false);
    return;
  }

  std::string title = BlockActionTitle(*action);
  if (title.empty()) title = T_("unknown", "unknown");
  inspectorTitle_->set_text(title);
  inspectorDot_->set_markup(DotMarkup(8, VerdictDot(*action)));
  inspectorVerdict_->set_text(
      action->Block ? T_("adv_verdict_blocked", "Blocked — no packets sent")
                    : (action->Local ? T_("adv_verdict_local",
                                          "Bypassed the tunnel — not protected")
                                     : T_("adv_verdict_tunnelled", "Tunnelled through URnetwork")));
  if (inspectorClear_) inspectorClear_->set_visible(true);

  auto add = [this](const Glib::ustring& key, const Glib::ustring& value) {
    auto row = kit::MakePaneKeyValueRow(key, value, kKeyValueRowHeight);
    CapNatural(row.key, 16);
    CapNatural(row.value, 20);
    inspectorRows_->append(*row.root);
    return row;
  };
  const Glib::ustring none = T_("adv_none", "none");
  auto addText = [&add, &none](const Glib::ustring& key, const std::string& value) {
    add(key, value.empty() ? none : Glib::ustring(value));
  };

  addText(T_("adv_host", "Host"), JoinValues(action->Hosts));
  addText(T_("adv_addresses", "Addresses"), JoinValues(action->Ips));
  {
    std::string matched = JoinValues(action->MatchedHosts);
    if (matched.empty()) matched = JoinValues(action->MatchedIps);
    if (!matched.empty()) add(T_("adv_matched", "Matched"), matched);
  }
  add(T_("adv_protected", "Protected"),
      action->Block ? Glib::ustring(T_("adv_na", "—"))
                    : Glib::ustring(action->Local ? T_("off", "Off") : T_("on", "On")));
  {
    const std::string overrideId = action->OverrideId.value_or(std::string());
    const char* reason = T_("adv_reason_default", "Default policy");
    if (!overrideId.empty()) {
      if (action->BlockOverride) {
        reason = T_("adv_reason_block", "Block override");
      } else if (action->RouteOverride) {
        reason = T_("adv_reason_route", "Route override");
      } else {
        reason = T_("adv_reason_override", "Override");
      }
    }
    add(T_("adv_reason", "Reason"), reason);
    if (!overrideId.empty()) add(T_("adv_override_id", "Override"), overrideId);
  }
  // TOTALS: a block action carries no per-direction split and the label says so
  add(T_("adv_packets_total", "Packets (total)"), FormatCountCompact(action->PacketCount));
  add(T_("adv_bytes_total", "Bytes (total)"), FormatByteCountCompact(action->ByteCount));
  if (action->Time > 0) {
    // an AGE, not a duration — nothing records when a connection closed
    const int64_t secondsAgo = std::max<int64_t>(0, (NowMillis() - action->Time) / 1000);
    add(T_("adv_last_decision", "Last decision"), RelativeTime(secondsAgo));
  }
  if (!action->Block) {
    // TODO(sdk-wiring): Sdk().ReadReliability() — SdkHost exposes no exits /
    // destination-exits table, so the exit-routing rows have nothing to resolve
    // against. Absent-not-guessed: the row says the destination is not in the
    // routing table rather than inventing an exit.
    add(T_("adv_via_exit", "Via exit"), T_("adv_unknown_exit", "Not in the routing table"));
    if (!countryName_.empty()) {
      // labelled as the SESSION's, because per-exit geo is not bridged
      add(T_("adv_session_exit_country", "Session exit country"), countryName_);
    }
  }
  {
    auto row = add(T_("adv_action_id", "Action id"),
                   ShortId(action->BlockActionId.value_or(std::string())));
    if (row.value) {
      // copyable, but NOT a new keyboard stop (set_selectable also sets
      // focusable in GTK4)
      row.value->set_selectable(true);
      row.value->set_can_focus(false);
      row.value->set_focus_on_click(false);
    }
  }
}

// The 5s exit-routing cache refresh (§4.1). SdkHost has no ReadReliability, so
// there is nothing to run on a worker and nothing to cache; the inspector's
// exit rows already render the real "not in the routing table" reading.
void ConnectPage::RefreshExitRouting() {
  // TODO(sdk-wiring): Sdk().ReadReliability() -> exits_/destinationExits_
  // (several synchronous device RPCs; single-flight on a worker thread, then
  // marshal back and re-run ApplyInspector only if something is selected).
  //
  // The completion MUST carry the stale-async guard, because it can land after
  // a logout, an Advanced-Mode toggle or a teardown has rebuilt the inspector:
  //   auto epoch = epoch_; const uint64_t gen = *epoch;
  //   ... on the worker ...
  //   PostToMain([this, epoch, gen, result] {
  //     if (*epoch != gen) return;        // re-checked ON ARRIVAL, not on entry
  //     if (!advanced_ || !presenting_) return;
  //     exits_ = ...; if (!selectedConnectionId_.empty()) ApplyInspector();
  //   });
}

// ---- provide control mode -----------------------------------------------------

void ConnectPage::ApplyProvideControlMode() {
  if (syncingProvide_) return;
  std::string mode = "never";
  if (provideAuto_ && provideAuto_->get_active()) mode = "auto";
  else if (provideAlways_ && provideAlways_->get_active()) mode = "always";
  else if (provideNetwork_ && provideNetwork_->get_active()) mode = "network";
  host_.SetProvideControlMode(mode);
}

void ConnectPage::SyncProvideControlMode() {
  const std::string mode = host_.GetProvideControlMode();
  syncingProvide_ = true;
  if (mode == "auto" && provideAuto_) provideAuto_->set_active(true);
  else if (mode == "always" && provideAlways_) provideAlways_->set_active(true);
  else if (mode == "network" && provideNetwork_) provideNetwork_->set_active(true);
  else if (provideNever_) provideNever_->set_active(true);
  syncingProvide_ = false;
}

// inbound echoes of the two device-owned toggles (§5): no-op when already
// equal, otherwise written under the guard so the handler cannot bounce back
void ConnectPage::ApplyBlockerUi() {
  if (!blockerToggle_) return;
  const bool on = host_.GetBlockerEnabled();
  if (blockerToggle_->get_active() == on) return;
  updatingControls_ = true;
  blockerToggle_->set_active(on);
  updatingControls_ = false;
}

void ConnectPage::ApplyKillSwitchUi() {
  if (!killSwitchToggle_) return;
  const bool on = !host_.GetRouteLocal();  // kill switch ON = routeLocal off
  if (killSwitchToggle_->get_active() == on) return;
  updatingControls_ = true;
  killSwitchToggle_->set_active(on);
  updatingControls_ = false;
}

// ---- connect options: the performance profile (§2.8) ---------------------------
// ConnectDrawer::RefreshControls/ApplyControls, reused verbatim: the Linux
// SdkHost has exposed GetPerformanceProfile/SetPerformanceProfile since the
// drawer shipped, and a nil profile means Auto with everything off.

void ConnectPage::SeedConnectControls() {
  if (!modeAuto_ || !fixedIpToggle_ || !anonToggle_ || !pqeToggle_) return;
  const auto profile = host_.GetPerformanceProfile();
  updatingControls_ = true;
  if (profile && profile->window_type == urnet::WindowTypeQuality) {
    modeWeb_->set_active(true);
  } else if (profile && profile->window_type == urnet::WindowTypeSpeed) {
    modeStreaming_->set_active(true);
  } else {
    modeAuto_->set_active(true);
  }
  // Strong Anonymization is the INVERSE of allow_direct
  anonToggle_->set_active(!(profile && profile->allow_direct));
  pqeToggle_->set_active(profile && profile->post_quantum_encryption);
  fixedIpToggle_->set_active(profile && profile->window_size &&
                             profile->window_size->window_size_min == 1 &&
                             profile->window_size->window_size_max == 1);
  fixedIpToggle_->set_sensitive(!modeAuto_->get_active());
  updatingControls_ = false;
}

void ConnectPage::OnConnectionModeChanged() {
  if (updatingControls_) return;
  const bool autoMode = modeAuto_ && modeAuto_->get_active();
  if (autoMode && fixedIpToggle_ && fixedIpToggle_->get_active()) {
    // Auto forces Fixed IP off QUIETLY (an auto profile carries no pinned
    // window size): inside the guard, one push after
    updatingControls_ = true;
    fixedIpToggle_->set_active(false);
    updatingControls_ = false;
  }
  if (fixedIpToggle_) fixedIpToggle_->set_sensitive(!autoMode);
  PushPerformanceProfile();
}

void ConnectPage::PushPerformanceProfile() {
  if (updatingControls_) return;
  if (!modeAuto_ || !fixedIpToggle_ || !anonToggle_ || !pqeToggle_) return;
  const bool autoMode = modeAuto_->get_active();
  // ALWAYS a profile, even for window type auto, so the orthogonal settings
  // (allow direct, post quantum encryption) persist and apply in every mode
  urnet::PerformanceProfile profile;
  profile.allow_direct = !anonToggle_->get_active();
  profile.post_quantum_encryption = pqeToggle_->get_active();
  if (autoMode) {
    profile.window_type = urnet::WindowTypeAuto;  // no fixed window type or size
  } else {
    profile.window_type =
        modeWeb_->get_active() ? urnet::WindowTypeQuality : urnet::WindowTypeSpeed;
    urnet::WindowSizeSettings windowSize{};
    const bool fixed = fixedIpToggle_->get_active();
    windowSize.window_size_min = fixed ? 1 : 2;
    windowSize.window_size_max = fixed ? 1 : 4;
    profile.window_size = windowSize;
  }
  host_.SetPerformanceProfile(profile);
  profileSig_ = ProfileSig(profile);  // our own write is not a feed change
}

// ---- pane A: the selected provider and the network peers -----------------------

// §2.3: the row says WHERE you are connecting. A selected network peer resolves
// to its device name (the raw client id is not a place), and no selection is
// "Best available provider" — ConnectDrawer::RefreshControls' logic verbatim.
void ConnectPage::ApplyLocationRow() {
  if (!locationText_ || !locationRow_) return;
  // renders from the cached reading: the row is re-rendered on BOTH the
  // location and the peers feed, and the SelectedLocation read is a locked
  // device call + a JSON parse
  const auto& location = selectedLocation_;
  Glib::ustring text = T_("best_available_provider", "Best available provider");
  const bool bestAvailable =
      !location || (location->connect_location_id &&
                    location->connect_location_id->best_available.value_or(false));
  if (!bestAvailable) {
    std::string displayName = location->name.value_or(std::string());
    if (location->connect_location_id && location->connect_location_id->client_id &&
        !location->connect_location_id->client_id->empty()) {
      if (peers_) {
        for (const auto& peer : *peers_) {
          if (peer.ClientId && *peer.ClientId == *location->connect_location_id->client_id) {
            displayName = PeerDisplayName(peer);
            break;
          }
        }
      }
    }
    if (!displayName.empty()) text = displayName;
  }
  locationText_->set_text(text);
  // the row is a Button whose child is a Box: it has NO automatic name
  kit::SetAccessibleLabel(*locationRow_,
                          Glib::ustring(T_("selected_provider", "Selected provider")) + ", " +
                              text);
}

// §2.8 PeersLine: a stale zero is never presented as fact — with the peer feed
// unavailable (no session / RPC down) the line says so, in gray.
void ConnectPage::ApplyPeerCount() {
  if (!peersText_ || !peersDot_ || !peersLine_) return;
  Glib::ustring text;
  const char* dot = kDotFaint;
  if (!peers_) {
    text = T_("peer_discovery_disabled", "Peer discovery disabled until connected");
    dot = "#989898";
  } else {
    text = Format(TN_("network_peer_count", "You have {} other device online",
                      "You have {} other devices online",
                      static_cast<unsigned long>(peerCount_)),
                  peerCount_);
    dot = 0 < peerCount_ ? kDotGreen : kDotAmber;
  }
  peersText_->set_text(text);
  peersDot_->set_markup(DotMarkup(8, dot));
  kit::SetAccessibleLabel(*peersLine_, text);
  if (peersMeta_) {
    kit::SetTextOrCollapse(*peersMeta_, peers_ && 0 < peerCount_
                                            ? Glib::ustring(FormatCountCompact(peerCount_))
                                            : Glib::ustring());
  }
}

void ConnectPage::ApplyPeersList() {
  if (!peersHost_) return;
  RemoveAllChildren(*peersHost_);
  if (!peers_ || peers_->empty()) {
    // a group is a list or a one-row sentence, never a hole (§8)
    auto row = MakeEmptySentenceRow(
        T_("peer_discovery_disabled", "Peer discovery disabled until connected"),
        kPeerRowHeight);
    peersHost_->append(*row.root);
    return;
  }
  for (const auto& peer : *peers_) {
    auto row = kit::MakePaneListRow(kPeerRowHeight);
    row.dot->set_markup(DotMarkup(7, peer.ProvideEnabled ? kDotGreen : kDotFaint));
    const Glib::ustring name = PeerDisplayName(peer);
    row.title->set_text(name);
    // what the device IS — two phones with the same name are still distinct
    row.meta->set_text(peer.DeviceSpec);
    CapNatural(row.title, 18);
    CapNatural(row.meta, 14);
    kit::MarkDecorative(*row.title);
    kit::MarkDecorative(*row.meta);
    kit::SetAccessibleLabel(*row.root, name + ", " + Glib::ustring(peer.DeviceSpec));
    peersHost_->append(*row.root);
  }
}

// ---- the feeds (§5) ------------------------------------------------------------

// one shared point list, each chart picks its route's sample
void ConnectPage::PullThroughput() {
  auto points = std::make_shared<const urnet::ThroughputPointList>(
      host_.ThroughputPoints().value_or(urnet::ThroughputPointList()));
  const double window = static_cast<double>(host_.ThroughputWindowSeconds());
  if (remoteChart_) remoteChart_->SetPoints(points, window);
  if (blockedChart_) blockedChart_->SetPoints(points, window);
  if (localChart_) localChart_->SetPoints(points, window);
}

// Re-read every feed and re-apply ONLY what changed. Every read below degrades
// to nullopt with no device, and every writer renders that as its own settled
// reading — never a spinner, never a zero.
void ConnectPage::RefreshFeeds(bool force) {
  {
    auto actions = host_.BlockActions();
    const uint64_t sig = BlockActionsSig(actions);
    if (force || sig != blockActionsSig_) {
      blockActionsSig_ = sig;
      blockActions_ = std::move(actions);
      ApplyConnectionsList();  // also re-runs the visibility + the inspector
      ApplySessionRows();      // the "Connections" figure counts the same feed
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
    }
  }
  {
    auto stats = host_.BlockStatsSnapshot();
    const bool changed =
        stats.has_value() != blockStats_.has_value() ||
        (stats && blockStats_ && (stats->AllowedCount != blockStats_->AllowedCount ||
                                  stats->BlockedCount != blockStats_->BlockedCount));
    if (force || changed) {
      blockStats_ = stats;
      ApplySessionRows();
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
    }
  }
  {
    auto rows = host_.ContractRows();
    const uint64_t sig = ContractRowsSig(rows);
    if (force || sig != contractRowsSig_) {
      contractRowsSig_ = sig;
      contractRows_ = std::move(rows);
      ApplyContractsList();
      if (contractsSheet_ && contractsSheet_->is_visible()) contractsSheet_->Refresh();
    }
  }
  {
    std::optional<urnet::BlockActionOverrideList> rules;
    if (auto overrides = host_.BlockActionOverrides()) {
      urnet::BlockActionOverrideList kept;
      for (const auto& override_ : *overrides) {
        // a "split rule" is an override whose route override forces local
        if (override_.RouteOverride && override_.RouteOverride->Local) kept.push_back(override_);
      }
      rules = std::move(kept);
    }
    const uint64_t sig = SplitRulesSig(rules);
    if (force || sig != splitRulesSig_) {
      splitRulesSig_ = sig;
      splitRules_ = std::move(rules);
      ApplySplitRuleCount();
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
    }
  }
  {
    auto settings = host_.GetDnsResolverSettings();
    const bool changed =
        settings.has_value() != dnsSettings_.has_value() ||
        (settings && dnsSettings_ && !DnsSheet::SettingsEqual(*settings, *dnsSettings_));
    if (force || changed) {
      dnsSettings_ = std::move(settings);
      ApplyDnsCard();
    }
  }
  // The provider row reads BOTH of the next two feeds (a selected network peer
  // is shown by its device name), so the location lands first and the row is
  // rendered at most once per pass.
  bool locationRowDirty = false;
  {
    // the connected country drives the dns recommendation, and the selected
    // location drives the provider row: ONE locked read, on change only
    auto location = host_.SelectedLocation();
    const uint64_t sig = LocationSig(location);
    if (force || sig != locationSig_) {
      locationSig_ = sig;
      selectedLocation_ = std::move(location);
      countryCode_ = selectedLocation_ && selectedLocation_->country_code
                         ? LowerCopy(*selectedLocation_->country_code)
                         : std::string();
      countryName_ =
          selectedLocation_ ? selectedLocation_->country.value_or(std::string()) : std::string();
      locationRowDirty = true;
      ApplyDnsRecommendationPill();
    }
  }
  {
    // the peers feed drives BOTH the peers group and the provider row's peer
    // name resolution; nullopt is "discovery unavailable", not "no peers"
    auto peers = host_.ConnectedProvidePeers();
    const int64_t count = host_.ConnectedPeerCount();
    const uint64_t sig = PeersSig(peers, count);
    if (force || sig != peersSig_) {
      peersSig_ = sig;
      peers_ = std::move(peers);
      peerCount_ = count;
      ApplyPeerCount();
      ApplyPeersList();
      locationRowDirty = true;  // a peer's device name comes off this list
    }
  }
  if (locationRowDirty) ApplyLocationRow();
  {
    // the connect-options controls echo the device/persisted profile
    const uint64_t sig = ProfileSig(host_.GetPerformanceProfile());
    if (force || sig != profileSig_) {
      profileSig_ = sig;
      SeedConnectControls();
    }
  }
  // both of these are no-ops when the control already agrees
  ApplyBlockerUi();
  ApplyKillSwitchUi();
  if (force) {
    PullThroughput();
    ApplyInspectorVisibility();
    ApplyInspector();
  }
}

// The clock-driven fallback for the change feed.
//
// TODO(wiring): SdkHost::SetDrawerEventHandler is a SINGLE slot and MainWindow
// owns it, routing every DrawerEvent to the legacy drawer only
// (MainWindow.cpp: `if (windowVisible_ && drawer_) drawer_->OnHostEvent(event)`).
// Until it also calls `connectPage_->OnHostEvent(event)`, OnHostEvent below is
// unreachable and panes B and C would never see a single SDK push: the charts
// would sit flat, the routing list would freeze on the snapshot taken when the
// window was shown, and the contracts / split-rules / DNS surfaces would keep
// rendering state the user has already changed. The page therefore re-reads its
// own feeds off the clock, applying only what actually changed (each read is
// fingerprinted, so an idle session rebuilds nothing). The moment a real event
// lands, the poll drops to a slow safety net.
void ConnectPage::PollFeeds() { RefreshFeeds(false); }

void ConnectPage::Resync() {
  ++(*epoch_);  // anything in flight against the old reading is stale
  SyncProvideControlMode();
  RefreshAllPanes();
}

// The drawer's dispatcher, per group: re-read through the accessor, re-apply
// ONE surface, cascade to an OPEN sheet only.
void ConnectPage::OnHostEvent(DrawerEvent event) {
  // a real push arrived: the clock-driven poll steps back to a safety net
  eventsWired_ = true;
  switch (event) {
    case DrawerEvent::DeviceLifecycle:
      RefreshAllPanes();
      break;
    case DrawerEvent::Throughput:
      PullThroughput();
      break;
    case DrawerEvent::BlockActions:
      blockActions_ = host_.BlockActions();
      blockActionsSig_ = BlockActionsSig(blockActions_);
      ApplyConnectionsList();
      ApplySessionRows();  // the "Connections" figure counts the same feed
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
      break;
    case DrawerEvent::BlockStats:
      blockStats_ = host_.BlockStatsSnapshot();
      ApplySessionRows();
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
      break;
    case DrawerEvent::Overrides:
      if (auto overrides = host_.BlockActionOverrides()) {
        urnet::BlockActionOverrideList rules;
        for (const auto& override_ : *overrides) {
          if (override_.RouteOverride && override_.RouteOverride->Local) {
            rules.push_back(override_);
          }
        }
        splitRules_ = std::move(rules);
      } else {
        splitRules_.reset();
      }
      splitRulesSig_ = SplitRulesSig(splitRules_);
      ApplySplitRuleCount();
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
      break;
    case DrawerEvent::DnsSettings:
      dnsSettings_ = host_.GetDnsResolverSettings();
      ApplyDnsCard();
      break;
    case DrawerEvent::Blocker:
      ApplyBlockerUi();
      break;
    case DrawerEvent::RouteLocal:
      ApplyKillSwitchUi();
      break;
    case DrawerEvent::Contracts:
      // the SDK view controller already coalesced the two streams: one event
      // per real change, and the settled snapshot is re-read here
      contractRows_ = host_.ContractRows();
      contractRowsSig_ = ContractRowsSig(contractRows_);
      ApplyContractsList();
      if (contractsSheet_ && contractsSheet_->is_visible()) contractsSheet_->Refresh();
      break;
    case DrawerEvent::Location: {
      // the provider row and the dns pill's regional recommendation both
      // follow the connected location — ONE locked read for both
      auto location = host_.SelectedLocation();
      locationSig_ = LocationSig(location);
      selectedLocation_ = std::move(location);
      countryCode_ = selectedLocation_ && selectedLocation_->country_code
                         ? LowerCopy(*selectedLocation_->country_code)
                         : std::string();
      countryName_ =
          selectedLocation_ ? selectedLocation_->country.value_or(std::string()) : std::string();
      ApplyLocationRow();
      ApplyDnsRecommendationPill();
      break;
    }
    case DrawerEvent::Profile:
      // pane A's connect-options group owns the performance profile
      profileSig_ = ProfileSig(host_.GetPerformanceProfile());
      SeedConnectControls();
      break;
    case DrawerEvent::Peers:
      // the peers line/list AND the provider row's peer-name resolution
      peers_ = host_.ConnectedProvidePeers();
      peerCount_ = host_.ConnectedPeerCount();
      peersSig_ = PeersSig(peers_, peerCount_);
      ApplyPeerCount();
      ApplyPeersList();
      ApplyLocationRow();
      break;
    case DrawerEvent::Locations:
      // the location chooser is MainWindow's sheet
      break;
    case DrawerEvent::ProviderIdentities:
    case DrawerEvent::ProviderLocations:
    case DrawerEvent::ProviderSelection:
      // MainWindow owns the globe sheet and the location-override tracking, so
      // they must survive with this page unbuilt
      break;
  }
}

// ---- the reused detail sheets ---------------------------------------------------

Gtk::Window* ConnectPage::RootWindow() {
  return dynamic_cast<Gtk::Window*>(get_root());
}

void ConnectPage::OpenContractsSheet() {
  auto* parent = RootWindow();
  if (!parent) return;
  if (!contractsSheet_) {
    contractsSheet_ = std::make_unique<ContractsSheet>(*parent, host_);
    // §4.4: on close, hand the view controller back to the top. The sheet
    // reports at-top=false on every scroll and only re-arms in Open(), and a
    // scrolled-away VC FREEZES membership + order and collects new rows into
    // its pending count — which is the same feed this pane's contracts group
    // renders. Without this, one scroll-then-close freezes pane C's contracts
    // list for the rest of the session.
    contractsSheet_->signal_hide().connect([this] { host_.SetContractsAtTop(true); });
  }
  contractsSheet_->Open();
}

void ConnectPage::OpenSplitRulesSheet() {
  auto* parent = RootWindow();
  if (!parent) return;
  if (!splitRulesSheet_) {
    splitRulesSheet_ = std::make_unique<SplitRulesSheet>(*parent, host_);
  }
  splitRulesSheet_->Open();
}

void ConnectPage::OpenDnsSheet() {
  auto* parent = RootWindow();
  if (!parent) return;
  if (!dnsSheet_) {
    dnsSheet_ = std::make_unique<DnsSheet>(*parent, host_);
  }
  // Open() returns false and does NOT present when there are no settings — the
  // group's own "DNS settings unavailable" row is the honest reading, and the
  // trailing action is desensitized in that state (ApplyDnsCard), so a click
  // that produces nothing cannot happen. Re-decide here too: the feed may have
  // gone away between the last reading and the press.
  if (!dnsSheet_->Open() && dnsEditButton_) dnsEditButton_->set_sensitive(false);
}

// ---- structure ----------------------------------------------------------------

void ConnectPage::ApplyMoreOptionsVisibility() {
  // Advanced: the disclosure is GONE and the host is unconditionally visible.
  // Simple: the host is gated behind the row (collapsed by default).
  const bool showToggle = !advanced_;
  moreOptionsToggle_->set_visible(showToggle);
  moreOptionsHost_->set_visible(advanced_ || moreOptionsExpanded_);
}

void ConnectPage::SetAdvancedMode(bool on) {
  if (advanced_ == on) return;
  advanced_ = on;
  if (!on) {
    selectedConnectionId_.clear();
    moreOptionsExpanded_ = false;
  }
  if (heroClamp_) {
    // the hero host is a MAXIMUM (190 Advanced / 320 Simple), never a floor
    adw_clamp_set_maximum_size(ADW_CLAMP(heroClamp_), on ? kHeroAdvanced : kHeroSimple);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(heroClamp_), on ? kHeroAdvanced : kHeroSimple);
  }
  ApplyMoreOptionsVisibility();
  ApplyConnectionsList();  // the row TYPE changes (static <-> selectable)
  ApplySessionRows();
  ApplyContractsList();    // Advanced shows full, selectable client ids
  ApplyInspectorVisibility();
  ApplyInspector();
  ApplyConnectStatus();
  // turning Advanced on refreshes the exit-routing cache immediately
  if (on) RefreshExitRouting();
  ApplyFold(/*force=*/true);
}

void ConnectPage::ApplyBreakpoint(int widthDip) {
  widthDip_ = widthDip;
  ApplyFold(/*force=*/false);
}

void ConnectPage::ApplyFold(bool force) {
  // §0 measures the fold on the PANE GRID's own width. MainWindow can only
  // report the toplevel's, and this page sits behind HomeShell's 220dip nav
  // rail: folding on the window width turns the third pane on while the panes
  // have 780dip to share, which is less than pane A + pane C + the two rules.
  const int allocated = get_width();
  const int paneWidth = allocated > 0 ? allocated : std::max(0, widthDip_ - kShellChromeDip);
  const bool changed = (paneWidth != foldWidth_);
  foldWidth_ = paneWidth;
  if (!changed && !force) return;

  // Simple is ALWAYS one pane, capped and centred; Advanced folds on width.
  const bool three = advanced_ && paneWidth >= kThreePaneDip;
  const bool two = advanced_ && paneWidth >= kTwoPaneDip;
  paneB_.root->set_visible(two);
  paneBRule_->set_visible(two);
  paneC_.root->set_visible(three);
  paneCRule_->set_visible(three);
  if (two) {
    paneA_.root->set_size_request(kPaneAWidth, -1);
    paneA_.root->set_hexpand(false);
  } else {
    // one pane: connect takes the width
    paneA_.root->set_size_request(-1, -1);
    paneA_.root->set_hexpand(true);
  }
  if (paneAClamp_) {
    // Simple: MaxWidth 480, centred. Advanced/rail: no cap — the clamp is a
    // maximum, so a value past any real width is the same as "off" and the
    // pane's own minimum stays whatever its content needs.
    const int cap = advanced_ ? 100000 : kSimpleCap;
    adw_clamp_set_maximum_size(ADW_CLAMP(paneAClamp_), cap);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(paneAClamp_), cap);
  }

  // Our own allocation is one frame behind a fold that changed which panes
  // exist: re-read it once at idle so the decision settles on the real width.
  // Terminates as soon as the reading stops moving (the `changed` gate above).
  if (!foldRecheckPending_) {
    foldRecheckPending_ = true;
    Glib::signal_idle().connect(sigc::track_obj(
        [this]() -> bool {
          // cleared FIRST so the re-read may queue one more pass if the fold
          // moved the allocation again; a one-shot source removes itself by
          // returning false (never disconnect a slot from inside itself)
          foldRecheckPending_ = false;
          ApplyFold(/*force=*/false);
          return false;
        },
        *this));
  }
}

void ConnectPage::SetPresentationActive(bool active) {
  if (presenting_ == active) return;
  presenting_ = active;
  if (active) {
    Resync();  // login / tab entry / window re-show: reseed every surface
  } else {
    ++(*epoch_);  // nothing in flight may write into a torn-down page
    // A sheet may not outlive the surface that feeds it: the same transition
    // closes the SDK view controllers these sheets read (contract rows, block
    // actions/overrides, dns settings), and the window merely HIDES to the
    // tray — a modal child left visible would float alone over the desktop
    // showing dead data and block input when the window is shown again.
    if (contractsSheet_) contractsSheet_->hide();
    if (splitRulesSheet_) splitRulesSheet_->hide();
    if (dnsSheet_) dnsSheet_->hide();
  }
  UpdateClock();
}

// §5: the clock runs while the window is presenting AND this destination is
// the mapped one. Sitting on Earnings must not wake the process ten times a
// second (or re-read three locked feeds every second for an invisible sheet).
void ConnectPage::UpdateClock() {
  const bool run = presenting_ && pageVisible_;
  canvas_->SetPresentationActive(run);
  if (run == tick_.connected()) return;
  if (run) {
    tick_ = Glib::signal_timeout().connect(
        sigc::track_obj(
            [this]() -> bool {
              Tick();
              return true;
            },
            *this),
        100);
  } else {
    tick_.disconnect();
  }
}

void ConnectPage::Tick() {
  if (!presenting_ || !pageVisible_) return;
  canvas_->Tick();
  ++tickCount_;
  // the charts ride the throughput feed; at 2fps the 60s window still reads
  // live and the read stays off the per-frame path
  if (tickCount_ % 5 == 0) PullThroughput();
  // every 10th tick (~1 s): re-read the feeds the page has no event path for
  // (see PollFeeds) and re-run the open split-rules sheet.
  if (tickCount_ % 10 == 0) {
    // Once MainWindow routes DrawerEvent here this drops to a 5s safety net.
    // A changed feed cascades into an open split-rules sheet from there (as
    // the event path does), so the sheet is never re-read for nothing.
    //
    // TODO(sheet): §4.5 wants the sheet's "Ns ago" captions aged on the CLOCK.
    // SplitRulesSheet has no RefreshTimes() entry point and its Refresh()
    // rebuilds only the sections whose VALUES changed, so on an idle session
    // those captions keep the age they had when the sheet was opened (only
    // Open() forces a rebuild). Fixing it needs that entry point on the sheet.
    if (!eventsWired_ || tickCount_ % 50 == 0) PollFeeds();
    // and every 5th of those (5 s), Advanced only: the exit-routing cache
    if (tickCount_ % 50 == 0 && advanced_) RefreshExitRouting();
  }
}

}  // namespace urnw
