// SPDX-License-Identifier: MPL-2.0
#include "ConnectPage.hpp"
#include "ConnectFold.hpp"
#include "DataInfo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "DisplayText.hpp"
#include "ExtenderProvideRowPaint.hpp"
#include "FailsafeNotice.hpp"
#include "FastDnsCopy.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "KeyedReconcile.hpp"
#include "KillSwitchCopy.hpp"
#include "LocationSelection.hpp"
#include "LocationsSheet.hpp"  // PeerDisplayName — shared with the chooser
#include "ProvideLine.hpp"
#include "ProvideModeGlyph.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// the fold thresholds (docs/parity/connect-page.md §0) are ConnectFold.hpp's
using connect_fold::kPaneAWidth;
using connect_fold::kPaneCWidth;
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
// The least height the activity list keeps: three rows. Pane B's fixed blocks
// above it (the chart, the transport bar, the two status rows, the header and
// the filter rows) fill a short window by themselves, and the list, the
// pane's reason to exist, was squeezed to nothing at the 480dip minimum
// height. With this floor the pane's own scroller takes over instead.
constexpr int kConnectionsListFloor = 3 * kListRowHeight;
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
/* a text action in a 28px group header (the activity filter's Clear) */
button.ur-pane-action.ur-text-action {
  padding: 0 6px; font-family: "PP Neue Montreal"; font-size: 11px; color: #F8F8F8;
}
/* the inspector's quick actions: the outlined pane button made compact, and
   filled action blue (the switches' on color) while their rule is in force */
button.ur-pane-secondary.ur-quick-action {
  min-height: 26px; margin: 0; padding: 0 10px; font-size: 12px;
}
button.ur-pane-secondary.ur-quick-action.ur-quick-on {
  background-color: #638BFC; border-color: #638BFC; color: #101010;
}
/* the inspector's Reason value when a rule decided it: a link to the rules */
button.ur-reason-link { min-height: 0; padding: 0 2px; font-weight: normal; }
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

uint64_t HostRulesSig(const std::vector<quick_action::HostRule>& rules) {
  uint64_t h = HashMix(7, rules.size());
  for (const auto& rule : rules) {
    h = HashText(h, rule.overrideId);
    h = HashMix(h, rule.hosts.size());
    for (const auto& host : rule.hosts) h = HashText(h, host);
    h = HashMix(h, (rule.hasBlockOverride ? 1u : 0u) | (rule.block ? 2u : 0u) |
                       (rule.hasRouteOverride ? 4u : 0u) | (rule.routeLocal ? 8u : 0u));
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
const char* VerdictDot(bool block, bool local) {
  if (block) return kDotCoral;
  if (local) return kDotAmber;
  return kDotGreen;
}
const char* VerdictDot(const urnet::BlockAction& action) {
  return VerdictDot(action.Block, action.Local);
}

// the dot is decorative, so the WORD is the only place the color's meaning
// exists for a screen reader
const char* VerdictWord(bool block, bool local) {
  if (block) return T_("blocked", "Blocked");
  if (local) return T_("local", "Local");
  return T_("allowed", "Allowed");
}

// The inspector's verdict line, and the copied details' Verdict.
const char* VerdictSentence(bool block, bool local) {
  if (block) return T_("adv_verdict_blocked", "Blocked — no packets sent");
  if (local) return T_("adv_verdict_local", "Bypassed the tunnel — not protected");
  return T_("adv_verdict_tunnelled", "Tunnelled through URnetwork");
}

// What decided the action: the default policy, or the kind of override.
const char* ReasonText(const urnet::BlockAction& action) {
  if (action.OverrideId.value_or(std::string()).empty()) {
    return T_("adv_reason_default", "Default policy");
  }
  if (action.BlockOverride) return T_("adv_reason_block", "Block override");
  if (action.RouteOverride) return T_("adv_reason_route", "Route override");
  return T_("adv_reason_override", "Override");
}

// What the quick actions read of a decision (QuickAction.hpp).
quick_action::Facts QuickFacts(const urnet::BlockAction& action) {
  quick_action::Facts facts;
  if (action.Hosts) facts.hosts = *action.Hosts;
  if (action.Ips) facts.ips = *action.Ips;
  if (action.MatchedHosts) facts.matchedHosts = *action.MatchedHosts;
  if (action.MatchedIps) facts.matchedIps = *action.MatchedIps;
  facts.overrideId = action.OverrideId.value_or(std::string());
  facts.hasBlockOverride = action.BlockOverride.has_value();
  facts.hasRouteOverride = action.RouteOverride.has_value();
  facts.block = action.Block;
  facts.local = action.Local;
  return facts;
}

// A quick action's label: what the press leaves behind.
const char* QuickActionText(quick_action::Label label) {
  switch (label) {
    case quick_action::Label::BlockHost:
      return T_("adv_block_host", "Block this host");
    case quick_action::Label::AllowHost:
      return T_("adv_allow_host", "Allow this host");
    case quick_action::Label::BypassTunnel:
      return T_("adv_bypass_tunnel", "Bypass the tunnel");
    case quick_action::Label::AlwaysTunnel:
      return T_("adv_always_tunnel", "Always tunnel");
  }
  return "";
}

std::string ShortId(const std::string& id) {
  return id.size() <= 12 ? id : id.substr(0, 12) + "…";
}

int64_t NowMillis() { return g_get_real_time() / 1000; }

// The reconcile key of one routing decision: its id, or for a decision the
// feed sent without one, its time and title, which hold still from push to
// push. The prefixes keep the two kinds of key apart.
std::string ConnectionRowKey(const urnet::BlockAction& action) {
  if (action.BlockActionId && !action.BlockActionId->empty()) {
    return "a:" + *action.BlockActionId;
  }
  return "t:" + std::to_string(action.Time) + ":" + BlockActionTitle(action);
}

// "N connections": how many decisions a host's group row stands for.
std::string ConnectionCountText(int64_t connections) {
  return Format(TN_("adv_connection_count", "{} connection", "{} connections",
                    static_cast<unsigned long>(connections)),
                connections);
}

// An activity row's meta line: how long ago the decision was made, then the
// totals the line has always carried ("12s ago   3.4 MiB   120 pkt"). A
// decision without a time (0) has no age rather than a 56-year one. A host's
// group row (groupConnections > 0) leads with its count, then the same
// figures summed, aged by its latest decision.
std::string ConnectionRowMeta(int64_t groupConnections, int64_t timeMs, int64_t byteCount,
                              int64_t packetCount, int64_t nowMs) {
  std::string meta;
  if (0 < groupConnections) meta = ConnectionCountText(groupConnections) + "   ";
  if (0 < timeMs) meta += RelativeTime((nowMs - timeMs) / 1000) + "   ";
  return meta + FormatByteCountCompact(byteCount) + "   " + FormatCountCompact(packetCount) +
         " pkt";
}

// What the fold reads of a decision that passed the filter.
connection_filter::FoldMember FoldMemberOf(const urnet::BlockAction& action) {
  connection_filter::FoldMember member;
  member.host = BlockActionTitle(action);
  member.block = action.Block;
  member.local = action.Local;
  member.timeMs = action.Time;
  member.byteCount = action.ByteCount;
  member.packetCount = action.PacketCount;
  return member;
}

// What the activity filter reads of a decision: its own lists, never copies.
connection_filter::Decision FilterDecision(const urnet::BlockAction& action) {
  connection_filter::Decision decision;
  decision.hosts = action.Hosts ? &*action.Hosts : nullptr;
  decision.ips = action.Ips ? &*action.Ips : nullptr;
  decision.matchedHosts = action.MatchedHosts ? &*action.MatchedHosts : nullptr;
  decision.matchedIps = action.MatchedIps ? &*action.MatchedIps : nullptr;
  decision.block = action.Block;
  decision.local = action.Local;
  return decision;
}

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
  // the dns pill capsule + the dot tones live in the shared sheet (Ui.cpp);
  // idempotent
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
  BeginPlaceholders();
  ApplyBreakpoint(widthDip_);

  // "Presenting" is the WINDOW being up; "visible" is this destination being
  // the mapped stack child. The clock needs both (§5) — sitting on Earnings
  // must not wake the process ten times a second.
  signal_map().connect([this] {
    pageVisible_ = true;
    UpdateClock();
    // the provider-only device's extender role, for the Extender switch while
    // disconnected; read at once, so the re-seed below draws it
    host_.SetProviderExtenderPolling(true);
    // a destination that was hidden while feeds moved re-seeds on entry
    if (presenting_) Resync();
  });
  signal_unmap().connect([this] {
    pageVisible_ = false;
    UpdateClock();
    host_.SetProviderExtenderPolling(false);
  });
}

ConnectPage::~ConnectPage() {
  // Order matters: alive_ FIRST, so a reliability completion already queued on
  // the main loop drops itself without dereferencing this page at all. The
  // epoch bump is belt to that brace (a completion captured on the old
  // generation is stale either way), and the clock is stopped explicitly
  // rather than left to sigc::track_obj's teardown ordering.
  *alive_ = false;
  ++(*epoch_);
  tick_.disconnect();
  // the row menu is parented by hand, so it is unparented by hand
  if (rowMenu_) rowMenu_->unparent();
}

// ---- PANE A -----------------------------------------------------------------

void ConnectPage::BuildPaneA() {
  paneA_ = kit::MakePane(T_("connect", "Connect"));
  paneA_.root->set_size_request(kPaneAWidth, -1);
  // the peers list at the foot of the pane fades out at the bottom edge while
  // it overflows (UrTheme.cpp). Pane B's live list does not: rows churning
  // under a fade flicker.
  paneA_.scroller->add_css_class("ur-fade-bottom");
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
  // the easter egg: five taps on the dot while connected, each within two
  // seconds of the previous, play the Pro celebration; silent otherwise
  auto dotTap = Gtk::GestureClick::create();
  dotTap->signal_released().connect([this](int, double, double) {
    if (renderedState_ != health::State::Connected) {
      connectedIconTaps_.Reset();
      return;
    }
    if (connectedIconTaps_.Tap(g_get_monotonic_time() / 1000) && on_connected_icon_tap) {
      on_connected_icon_tap();
    }
  });
  statusDot_->add_controller(dotTap);
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
  hero_->signal_clicked().connect([this] { RelayConnectPress(); });
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
  connectBtn_->signal_clicked().connect([this] { RelayConnectPress(); });
  paneAContent_->append(*connectBtn_);

  // 2.4b out of balance with a connection requested, the tunnel holds traffic
  // with no provider behind it: say so and offer both ways out. Disconnect is
  // its own always-enabled control so the way out never depends on the
  // connect action's reading.
  heldAlert_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  heldAlert_->set_margin_top(8);
  {
    // first when the free data refreshes, so Upgrade does not read as the only
    // way back; Why? explains the balance in the "About your data" sheet
    auto* refresh = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    freeRefreshLabel_ = Gtk::make_managed<Gtk::Label>();
    freeRefreshLabel_->set_xalign(0);
    freeRefreshLabel_->set_wrap(true);
    freeRefreshLabel_->set_hexpand(true);
    CapNatural(freeRefreshLabel_, 20);
    refresh->append(*freeRefreshLabel_);
    auto* why = Gtk::make_managed<Gtk::Button>(T_("data_info_why", "Why?"));
    why->add_css_class("flat");
    why->set_valign(Gtk::Align::CENTER);
    why->signal_clicked().connect([this] {
      if (on_open_data_info) on_open_data_info();
    });
    refresh->append(*why);
    heldAlert_->append(*refresh);
    // then whether the data is reserved or used up (ApplyBalanceRecovery)
    balanceKindLabel_ = Gtk::make_managed<Gtk::Label>();
    balanceKindLabel_->set_xalign(0);
    balanceKindLabel_->set_wrap(true);
    CapNatural(balanceKindLabel_, 20);
    balanceKindLabel_->set_visible(false);
    heldAlert_->append(*balanceKindLabel_);
  }
  {
    auto* text = Gtk::make_managed<Gtk::Label>(
        T_("insufficient_balance_held_notice",
           "Your traffic is held in the tunnel until you upgrade or disconnect."));
    text->add_css_class("ur-error-text");
    text->set_xalign(0);
    text->set_wrap(true);
    CapNatural(text, 20);
    heldAlert_->append(*text);
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    actions->set_homogeneous(true);
    auto* upgrade = Gtk::make_managed<Gtk::Button>(T_("upgrade", "Upgrade"));
    upgrade->add_css_class("ur-pane-primary");
    upgrade->signal_clicked().connect([this] {
      if (on_open_upgrade) on_open_upgrade();
    });
    actions->append(*upgrade);
    auto* disconnect = Gtk::make_managed<Gtk::Button>(T_("disconnect", "Disconnect"));
    disconnect->add_css_class("ur-pane-secondary");
    disconnect->signal_clicked().connect([this] {
      // the same in-flight feedback as the connect action's Disconnect
      disconnectRequestedAtUs_ = g_get_monotonic_time();
      ApplyConnectStatus();
      if (on_balance_disconnect) on_balance_disconnect();
    });
    actions->append(*disconnect);
    heldAlert_->append(*actions);
  }
  heldAlert_->set_visible(false);
  paneAContent_->append(*heldAlert_);

  // 2.4c a connect the user asked for that the balance blocked is retried by
  // itself once data is back (balance_notice::BalanceRecovery): say so under
  // the held alert, or on its own for a start that was refused, whose Cancel is
  // the only way to stop it
  balanceRecoveryRow_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  balanceRecoveryRow_->set_margin_top(8);
  {
    auto* text = Gtk::make_managed<Gtk::Label>(
        T_("insufficient_balance_will_reconnect", "You'll be reconnected when data is available again."));
    text->set_xalign(0);
    text->set_wrap(true);
    text->set_hexpand(true);
    CapNatural(text, 20);
    balanceRecoveryRow_->append(*text);
    balanceRecoveryCancel_ = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
    balanceRecoveryCancel_->add_css_class("flat");
    balanceRecoveryCancel_->set_valign(Gtk::Align::CENTER);
    balanceRecoveryCancel_->signal_clicked().connect([this] {
      if (on_cancel_balance_recovery) on_cancel_balance_recovery();
    });
    balanceRecoveryRow_->append(*balanceRecoveryCancel_);
  }
  balanceRecoveryRow_->set_visible(false);
  paneAContent_->append(*balanceRecoveryRow_);

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
  // the provide line under it, as the discoverable line is drawn; the line
  // collapses, never a blank row, while there is nothing to say
  provideStatsText_ = Gtk::make_managed<Gtk::Label>();
  provideStatsText_->add_css_class("ur-caption");
  provideStatsText_->set_xalign(0);
  provideStatsText_->set_margin_start(12);
  provideStatsText_->set_margin_end(12);
  provideStatsText_->set_margin_bottom(8);
  provideStatsText_->set_wrap(true);
  CapNatural(provideStatsText_, 32);
  provideStatsText_->set_visible(false);
  moreOptionsHost_->append(*provideStatsText_);

  // The provider extender row (EXTENDER.md N7), after the provide control's
  // own footer line so the segmented control keeps it. Hand built like the
  // provide row above, since the kit's two-line rows have no leading slot: the
  // dot, the title over the one-line state, and the switch. Hidden, never
  // disabled, until a device reports the role supported.
  extenderRow_ = kit::MakePaneRow(44);
  {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    line->set_hexpand(true);
    extenderDot_ = Gtk::make_managed<Gtk::Label>();
    extenderDot_->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*extenderDot_);
    line->append(*extenderDot_);
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    text->set_hexpand(true);
    text->set_valign(Gtk::Align::CENTER);
    auto* title = Gtk::make_managed<Gtk::Label>(T_("extender", "Extender"));
    title->add_css_class("ur-row-title");
    title->set_xalign(0);
    text->append(*title);
    extenderState_ = Gtk::make_managed<Gtk::Label>();
    extenderState_->add_css_class("ur-row-note");
    extenderState_->set_xalign(0);
    extenderState_->set_single_line_mode(true);
    extenderState_->set_ellipsize(Pango::EllipsizeMode::END);
    // the 330 rail holds only if a long listen failure does not ask for more;
    // the tooltip carries the whole text
    CapNatural(extenderState_, 30);
    text->append(*extenderState_);
    line->append(*text);
    extenderToggle_ = Gtk::make_managed<Gtk::Switch>();
    extenderToggle_->set_valign(Gtk::Align::CENTER);
    kit::SetAccessibleLabel(*extenderToggle_, T_("extender", "Extender"));
    extenderToggle_->property_active().signal_changed().connect([this] {
      // echo guard: a feed-driven write must not travel back to the SDK
      if (updatingControls_) return;
      OnExtenderToggled();
    });
    line->append(*extenderToggle_);
    if (auto* inner = RowInner(extenderRow_)) inner->append(*line);
  }
  extenderRow_->set_visible(false);
  moreOptionsHost_->append(*extenderRow_);
  extenderDescription_ = Gtk::make_managed<Gtk::Label>(
      T_("extender_setting_description",
         "While you are providing, this device also relays for people whose access to the "
         "network is blocked, on TCP and UDP 443 and UDP 4053."));
  extenderDescription_->add_css_class("ur-caption");
  extenderDescription_->set_xalign(0);
  extenderDescription_->set_margin_start(12);
  extenderDescription_->set_margin_end(12);
  extenderDescription_->set_margin_bottom(8);
  extenderDescription_->set_wrap(true);
  CapNatural(extenderDescription_, 32);
  extenderDescription_->set_visible(false);
  moreOptionsHost_->append(*extenderDescription_);

  // The lag.dev speed tests switch (lag.dev/PLAN.md C4, decision 18): whether
  // the extender role serves lag.dev's measurement API, so people testing their
  // connection there can measure to this device. It rides on the role, so it
  // shows only beside the Extender switch, hidden, never disabled
  // (DrawSpeedTestsRow). Built like the Extender row, its title starting under
  // the Extender title.
  speedTestsRow_ = kit::MakePaneRow(44);
  {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    line->set_hexpand(true);
    // the Extender dot's glyph, never shown, so the two titles line up
    auto* inset = Gtk::make_managed<Gtk::Label>("●");
    inset->set_opacity(0);
    inset->set_valign(Gtk::Align::CENTER);
    kit::MarkDecorative(*inset);
    line->append(*inset);
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    text->set_hexpand(true);
    text->set_valign(Gtk::Align::CENTER);
    auto* title =
        Gtk::make_managed<Gtk::Label>(T_("extender_speed_tests", "lag.dev speed tests"));
    title->add_css_class("ur-row-title");
    title->set_xalign(0);
    text->append(*title);
    auto* note = Gtk::make_managed<Gtk::Label>(T_(
        "extender_speed_tests_description",
        "Let people testing their connection on lag.dev measure latency, download and upload "
        "to this device. Tests use some of your bandwidth while they run."));
    note->add_css_class("ur-row-note");
    note->set_xalign(0);
    note->set_wrap(true);
    CapNatural(note, 30);
    text->append(*note);
    line->append(*text);
    speedTestsToggle_ = Gtk::make_managed<Gtk::Switch>();
    speedTestsToggle_->set_valign(Gtk::Align::CENTER);
    // the setting's default, until the first reading draws it
    speedTestsToggle_->set_active(true);
    kit::SetAccessibleLabel(*speedTestsToggle_,
                            T_("extender_speed_tests", "lag.dev speed tests"));
    speedTestsToggle_->property_active().signal_changed().connect([this] {
      // echo guard: a feed-driven write must not travel back to the SDK
      if (updatingControls_) return;
      OnSpeedTestsToggled();
    });
    line->append(*speedTestsToggle_);
    if (auto* inner = RowInner(speedTestsRow_)) inner->append(*line);
  }
  speedTestsRow_->set_visible(false);
  moreOptionsHost_->append(*speedTestsRow_);

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
                             std::function<void(bool)> apply,
                             const Glib::ustring& note = {}) {
    auto row = kit::MakePaneTwoLineRow(title, note, 40);
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
  // (PushPerformanceProfile): SdkHost::GetPerformanceProfile and
  // SetPerformanceProfile are the accessors.
  // a Fixed IP window keeps its one exit for the session (connect stickyExit)
  fixedIpToggle_ = addToggleRow(T_("fixed_ip", "Fixed IP"), false,
                                [this](bool) { PushPerformanceProfile(); },
                                T_("fixed_ip_subtitle",
                                   "Keeps one exit for the session; changes only if that provider goes offline."));
  fixedIpToggle_->set_sensitive(false);  // Auto pins the window size: no fixed ip
  // Strong Anonymization is the INVERSE of allow_direct
  anonToggle_ = addToggleRow(T_("strong_anonymization", "Strong Anonymization"), true,
                             [this](bool) { PushPerformanceProfile(); });
  pqeToggle_ = addToggleRow(T_("post_quantum_encryption", "Post Quantum Encryption"), false,
                            [this](bool) { PushPerformanceProfile(); });
  blockerToggle_ = addToggleRow(T_("block_ads_and_trackers", "Block ads and trackers"),
                                host_.GetBlockerEnabled(),
                                [this](bool on) { host_.SetBlockerEnabled(on); });
  // The kill switch. THREE legs behind this one control
  // (SdkHost::SetKillSwitch): the persisted preference, the device's
  // routeLocal, and — the leg that makes it a kill switch rather than a
  // preference — urnetworkd's nftables ruleset. The enforcement leg is a
  // control-socket round trip, so the completion lands later and carries what
  // the daemon says it REALLY installed.
  killSwitchToggle_ = addToggleRow(
      T_("kill_switch", "Kill switch"), host_.CurrentKillSwitch(), [this](bool on) {
        auto epoch = epoch_;
        const uint64_t seen = *epoch_;
        host_.SetKillSwitch(on, [this, epoch, seen, on](KillSwitchStatus) {
          if (*epoch != seen) return;  // a newer session owns the page
          // Read back rather than trust the write. Only a preference that did
          // not take moves the control; a FAILED enforcement leg is disclosed
          // in the line below, never hidden by reverting the switch.
          if (host_.CurrentKillSwitch() != on) {
            g_warning("connect: kill switch preference did not take (wanted %d)",
                      static_cast<int>(on));
          }
          ApplyKillSwitchUi();
        });
        ApplyKillSwitchUi();  // "Applying…" now; the control is never inert
      });
  // What is REALLY in force, under the switch. A DEDICATED wrapped line rather
  // than the row's own note: the note is trimmed to one ellipsized line, and a
  // truncated "your traffic is NOT being blocked" is the same defect as
  // showing nothing at all.
  killSwitchNote_ = Gtk::make_managed<Gtk::Label>();
  killSwitchNote_->set_xalign(0);
  killSwitchNote_->set_wrap(true);
  killSwitchNote_->set_margin_start(8);
  killSwitchNote_->set_margin_end(8);
  killSwitchNote_->set_margin_bottom(4);
  killSwitchNote_->set_visible(false);
  moreOptionsHost_->append(*killSwitchNote_);

  // The second doors to the statistics pane's sheets (ConnectFold.hpp
  // FoldDoorsShown): shown by ApplyFold exactly while pane C is folded, under
  // the pane's own title, closing pane A's fixed controls ahead of the peers
  // list. They open the same single-instance sheets pane C's doors do.
  foldDoorsHost_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  {
    auto header = kit::MakePaneGroupHeader(T_("client_statistics", "Client statistics"));
    foldDoorsHost_->append(*header.root);
    auto door = [this](const char* title, std::function<void()> open) {
      // the row's name is its title (kit::MakePaneTwoLineRowButton)
      auto row = kit::MakePaneTwoLineRowButton(title, {}, 40);
      row.root->signal_clicked().connect([open] { open(); });
      foldDoorsHost_->append(*row.root);
      return row.root;
    };
    door(T_("client_contracts", "Client contracts"), [this] { OpenContractsSheet(); });
    door(T_("split_rules", "Split rules"), [this] { OpenSplitRulesSheet(); });
    foldDoorDns_ = door(T_("custom_dns", "Custom DNS"), [this] { OpenDnsSheet(); });
    foldDoorDns_->set_sensitive(false);  // ApplyDnsCard, as pane C's edit action
    door(T_("transports", "Transports"), [this] { OpenTransportSheet(); });
    // the globe follows pane C's provider count row: shown and pressable
    // only with a session to draw (ApplyLiveStatsGroup)
    foldDoorGlobe_ = door(T_("provider_locations_title", "Provider Locations"),
                          [this] { OpenProviderLocations(); });
    foldDoorGlobe_->set_visible(false);
  }
  moreOptionsHost_->append(*foldDoorsHost_);

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

  // the remote traffic of the window by transport, directly under the remote
  // chart on its own natural-height pane row (the legend/footer wrap on a
  // narrow pane), lined up with the plot by the row's 12px inset. The whole
  // bar is the tap target for the transport settings sheet.
  auto* transportRow = kit::MakePaneRow(-1);
  transportBar_ = Gtk::make_managed<TransportBar>();
  transportBar_->set_hexpand(true);
  transportBar_->set_margin_top(10);
  transportBar_->set_margin_bottom(10);
  transportBar_->SetSurfaceColor(kUrBackground);  // the pane fill, not the card
  transportBar_->on_activate = [this] { OpenTransportSheet(); };
  if (auto* inner = RowInner(transportRow)) inner->append(*transportBar_);
  paneB_.content->append(*transportRow);

  // Windows' next two rows (connect/IPV6.md D2, EXTENDER.md K4): the ip family
  // status row directly under the transport bar, then the extender panel. Plain
  // rows on the pane's 12px inset with no rule of their own: they are not
  // tappable, and the connections group header below draws the separation.
  ipFamilyStatusRow_ = Gtk::make_managed<IpFamilyStatusRow>();
  ipFamilyStatusRow_->set_margin_start(12);
  ipFamilyStatusRow_->set_margin_end(12);
  ipFamilyStatusRow_->set_margin_top(8);
  ipFamilyStatusRow_->set_margin_bottom(8);
  paneB_.content->append(*ipFamilyStatusRow_);
  extenderPanel_ = Gtk::make_managed<ExtenderPanel>();
  extenderPanel_->set_margin_start(12);
  extenderPanel_->set_margin_end(12);
  extenderPanel_->set_margin_top(8);
  extenderPanel_->set_margin_bottom(8);
  paneB_.content->append(*extenderPanel_);

  // 3.3 the connections group header; its meta is the FULL feed count even
  // though the list caps at 200 rows
  auto connectionsHeader = kit::MakePaneGroupHeader(T_("connections", "Connections"));
  connectionsCount_ = connectionsHeader.meta;
  // the one-click reset, shown only while the filter or the search holds rows
  // back (ApplyConnectionsList writes it); its text is its accessible name
  connectionsClear_ = Gtk::make_managed<Gtk::Button>(T_("clear", "Clear"));
  connectionsClear_->add_css_class("ur-pane-action");
  connectionsClear_->add_css_class("ur-text-action");
  connectionsClear_->set_valign(Gtk::Align::CENTER);
  connectionsClear_->set_visible(false);
  connectionsClear_->signal_clicked().connect([this] { OnConnectionsClearFilters(); });
  connectionsHeader.trailing->append(*connectionsClear_);
  paneB_.content->append(*connectionsHeader.root);

  // 3.3' the verdict ratio bar under the header: 3px of allowed (green),
  // blocked (coral) and bypassed (amber), the colors the row dots print.
  // Decorative: the session rows carry the same numbers in words.
  verdictRatioBar_ = Gtk::make_managed<Gtk::DrawingArea>();
  verdictRatioBar_->set_content_height(3);
  verdictRatioBar_->set_hexpand(true);
  verdictRatioBar_->set_visible(false);  // nothing to proportion yet
  kit::MarkDecorative(*verdictRatioBar_);
  verdictRatioBar_->set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) {
        double x = 0;
        const std::pair<double, Rgba> shares[] = {{verdictRatio_.allowed, kUrGreen},
                                                  {verdictRatio_.blocked, kUrCoral},
                                                  {verdictRatio_.bypassed, kUrAmber}};
        for (const auto& [share, color] : shares) {
          const double w = share * width;
          cr->set_source_rgba(color.r, color.g, color.b, color.a);
          cr->rectangle(x, 0, w, height);
          cr->fill();
          x += w;
        }
      });
  paneB_.content->append(*verdictRatioBar_);

  // 3.3a the verdict filter (Windows' SelectorBar): the three verdicts the row
  // dots print, plus All. The labels ellipsize so the four fit the narrowest
  // two-pane activity column without widening it.
  {
    auto* row = kit::MakePaneRow(40);
    auto* segmented = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    segmented->add_css_class("linked");
    segmented->set_hexpand(true);
    segmented->set_valign(Gtk::Align::CENTER);
    auto segment = [this, segmented](Gtk::ToggleButton*& slot, const char* text,
                                     connection_filter::Verdict verdict) {
      slot = Gtk::make_managed<Gtk::ToggleButton>();
      auto* label = Gtk::make_managed<Gtk::Label>(text);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      slot->set_child(*label);
      slot->set_hexpand(true);
      kit::SetAccessibleLabel(*slot, text);
      if (slot != verdictAll_) slot->set_group(*verdictAll_);
      Gtk::ToggleButton* button = slot;
      button->signal_toggled().connect([this, button, verdict] {
        if (updatingControls_ || !button->get_active()) return;
        OnConnectionsVerdictChanged(verdict);
      });
      segmented->append(*slot);
    };
    segment(verdictAll_, T_("adv_filter_all", "All"), connection_filter::Verdict::All);
    segment(verdictBlocked_, T_("blocked", "Blocked"), connection_filter::Verdict::Blocked);
    segment(verdictTunnelled_, T_("adv_filter_tunnelled", "Tunnelled"),
            connection_filter::Verdict::Tunnelled);
    segment(verdictBypassed_, T_("adv_filter_bypassed", "Bypassed"),
            connection_filter::Verdict::Bypassed);
    verdictAll_->set_active(true);  // before any handler can read it
    if (auto* inner = RowInner(row)) inner->append(*segmented);
    paneB_.content->append(*row);
  }
  // 3.3b the host/address search (NetworkPage's search row): view-side over
  // the cached feed, so typing re-runs the reconcile and reads from the top
  {
    auto search = kit::MakePaneSearchRow(T_("adv_search_connections", "Search hosts or IPs"));
    connectionsSearch_ = search.box;
    connectionsSearch_->signal_changed().connect([this] {
      if (updatingControls_) return;
      std::string query = connection_filter::NormalizeQuery(connectionsSearch_->get_text());
      if (query == connectionsQuery_) return;
      connectionsQuery_ = std::move(query);
      ApplyConnectionsList(/*resetScroll=*/true);
    });
    // group by host rides the search row, whose field is the one control
    // here that shrinks gracefully
    if (auto* inner = RowInner(search.root)) {
      const char* groupText = T_("adv_group_by_host", "Group by host");
      auto* groupLabel = Gtk::make_managed<Gtk::Label>(groupText);
      groupLabel->add_css_class("ur-key");
      groupLabel->set_ellipsize(Pango::EllipsizeMode::END);
      kit::MarkDecorative(*groupLabel);  // the switch carries the name
      inner->append(*groupLabel);
      connectionsGroupToggle_ = Gtk::make_managed<Gtk::Switch>();
      connectionsGroupToggle_->set_valign(Gtk::Align::CENTER);
      kit::SetAccessibleLabel(*connectionsGroupToggle_, groupText);
      connectionsGroupToggle_->property_active().signal_changed().connect([this] {
        if (updatingControls_) return;
        const bool grouped = connectionsGroupToggle_->get_active();
        if (grouped == connectionsGrouped_) return;
        connectionsGrouped_ = grouped;
        ApplyConnectionsList(/*resetScroll=*/true);
      });
      inner->append(*connectionsGroupToggle_);
    }
    paneB_.content->append(*search.root);
  }

  // 3.4 the list area: the list and the empty reading SWAP, never coexist.
  // ONLY this row scrolls (§3: header/chart/group header are auto rows, the
  // list is the star row) — the pane's own scroller would otherwise drag the
  // live chart and the host count off the top the moment the feed is long
  // enough to need scrolling. A ScrolledWindow does not propagate its child's
  // natural height, so nesting it inside the pane scroller is stable: the
  // outer never scrolls while the window is tall enough, the inner takes the
  // leftover height. Below that the list keeps kConnectionsListFloor and the
  // outer scroller engages, so the list is never squeezed out of sight.
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
  connectionsScroll_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  connectionsScroll_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  connectionsScroll_->set_child(*connectionsArea_);
  connectionsScroll_->set_vexpand(true);
  connectionsScroll_->set_min_content_height(kConnectionsListFloor);
  paneB_.content->append(*connectionsScroll_);

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

  // the per-connection quick actions (QuickAction.hpp): the block and route
  // toggles and Copy details, labelled and lit by ApplyInspector from the live
  // host rules; gone while nothing is selected. A flow, so a longer language
  // wraps the three onto two lines instead of widening the 380dip rail.
  {
    auto* actions = Gtk::make_managed<Gtk::FlowBox>();
    actions->set_selection_mode(Gtk::SelectionMode::NONE);
    actions->set_homogeneous(false);
    actions->set_max_children_per_line(3);
    actions->set_column_spacing(8);
    actions->set_row_spacing(6);
    actions->set_margin_start(12);
    actions->set_margin_end(12);
    actions->set_margin_bottom(10);
    auto add = [actions](Gtk::Button*& slot, const Glib::ustring& text) {
      slot = Gtk::make_managed<Gtk::Button>(text);
      slot->add_css_class("ur-pane-secondary");
      slot->add_css_class("ur-quick-action");
      actions->append(*slot);
      // the flow's own child is not a stop of its own: Tab lands on the button
      if (auto* child = slot->get_parent()) child->set_focusable(false);
    };
    add(inspectorBlockButton_, T_("adv_block_host", "Block this host"));
    add(inspectorRouteButton_, T_("adv_bypass_tunnel", "Bypass the tunnel"));
    add(inspectorCopyButton_, T_("adv_copy_details", "Copy details"));
    inspectorBlockButton_->signal_clicked().connect([this] {
      if (const auto* action = SelectedConnectionAction()) {
        RunQuickAction(quick_action::Kind::Block, blockQuick_, QuickFacts(*action));
      }
    });
    inspectorRouteButton_->signal_clicked().connect([this] {
      if (const auto* action = SelectedConnectionAction()) {
        RunQuickAction(quick_action::Kind::Route, routeQuick_, QuickFacts(*action));
      }
    });
    inspectorCopyButton_->signal_clicked().connect([this] {
      if (const auto* action = SelectedConnectionAction()) CopyConnectionDetails(*action);
    });
    actions->set_visible(false);
    inspectorActions_ = actions;
    inspectorGroup_->append(*actions);
  }
  inspectorGroup_->append(*kit::MakeDivider());

  // The fields, rebuilt on every render, in two lists around Reason. Reason
  // is built once and only relabelled (ApplyInspectorReason): for a
  // rule-decided connection its value is the one control among the fields,
  // and a rebuild on every push took its focus and hover about once a second.
  inspectorRowsAbove_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  inspectorGroup_->append(*inspectorRowsAbove_);
  {
    auto row = kit::MakePaneKeyValueRow(T_("adv_reason", "Reason"), {}, kKeyValueRowHeight);
    CapNatural(row.key, 16);
    CapNatural(row.value, 20);
    inspectorReasonValue_ = row.value;
    // the reason names a rule, and the rules live in the split rules sheet, so
    // "why did this happen" is one press away: a button that opens it, its
    // chevron saying so, as the provider count row's does
    auto* link = Gtk::make_managed<Gtk::Button>();
    link->add_css_class("flat");
    link->add_css_class("ur-reason-link");
    link->set_valign(Gtk::Align::CENTER);
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    inspectorReasonLinkText_ = Gtk::make_managed<Gtk::Label>();
    inspectorReasonLinkText_->add_css_class("ur-value");
    inspectorReasonLinkText_->set_ellipsize(Pango::EllipsizeMode::END);
    CapNatural(inspectorReasonLinkText_, 18);
    content->append(*inspectorReasonLinkText_);
    auto* chevron = Gtk::make_managed<Gtk::Image>();
    chevron->set_from_icon_name("pan-end-symbolic");
    chevron->set_pixel_size(12);
    chevron->add_css_class("ur-key");
    kit::MarkDecorative(*chevron);
    content->append(*chevron);
    link->set_child(*content);
    link->signal_clicked().connect([this] { OpenSplitRulesSheet(); });
    link->set_visible(false);
    if (auto* inner = RowInner(row.root)) inner->append(*link);
    inspectorReasonLink_ = link;
    inspectorReasonRow_ = row.root;
    inspectorReasonRow_->set_visible(false);
    inspectorGroup_->append(*inspectorReasonRow_);
  }
  inspectorRowsBelow_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  inspectorGroup_->append(*inspectorRowsBelow_);

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
  // text + a chevron (iOS parity): the row opens the provider details
  auto* providerCountRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
  providerCountRow->append(*providerCountText_);
  auto* providerCountChevron = Gtk::make_managed<Gtk::Image>();
  providerCountChevron->set_from_icon_name("pan-end-symbolic");
  providerCountChevron->set_pixel_size(14);
  providerCountChevron->add_css_class("ur-key");
  providerCountChevron->set_valign(Gtk::Align::CENTER);
  kit::MarkDecorative(*providerCountChevron);
  providerCountRow->append(*providerCountChevron);
  providerCountLine_->set_child(*providerCountRow);
  providerCountLine_->signal_clicked().connect([this] { OpenProviderLocations(); });
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
  dnsFallbackRow_ = MakeDnsStatusRow(T_(kFastDnsLabel.key, kFastDnsLabel.english));
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

// ---- the press, and the intent it leaves behind --------------------------------

// How long a Disconnect press is allowed to hold the page on "Disconnecting…".
// It is a CEILING, not a duration: the intent normally clears the moment the
// reading actually reads down. The ceiling exists because a teardown can fail —
// the daemon can be gone, StopTunnel can throw — and a UI that waits forever for
// a completion that will never arrive is a worse lie than the one being fixed.
constexpr gint64 kDisconnectIntentUs = 8 * G_TIME_SPAN_SECOND;

// How long a loading skeleton may stand before the section settles on its
// empty reading (DESIGNSTYLE: a placeholder must resolve). The device is up
// well inside this after a sign-in; past it, the honest reading is the one
// the accessors give — unavailable rows, an empty transport track.
constexpr gint64 kPlaceholderCeilingUs = 6 * G_TIME_SPAN_SECOND;

// ---- DESIGNSTYLE "Placeholders, not pop-in" -----------------------------------

// Arm the skeletons for whatever has not been read yet: at build, and on every
// Resync (login / re-show) — a null distribution on a signed-in page means the
// device is still coming up, and the reading is worth waiting for.
void ConnectPage::BeginPlaceholders() {
  placeholdersSinceUs_ = g_get_monotonic_time();
  if (!dnsSettings_) {
    dnsSettled_ = false;
    ApplyDnsCard();
  }
  if (transportBar_ && !host_.ClientTransportDistribution()) transportBar_->BeginLoading();
}

// The ceiling: whatever is still a skeleton becomes its empty reading, in
// the same box. Idempotent — a section that settled on real data is untouched.
void ConnectPage::SettlePlaceholders() {
  placeholdersSinceUs_ = 0;
  if (!dnsSettled_) {
    dnsSettled_ = true;
    ApplyDnsCard();
  }
  if (transportBar_ && transportBar_->IsLoading()) transportBar_->SettleEmpty();
}

bool ConnectPage::DisconnectIntentLive() {
  if (disconnectRequestedAtUs_ == 0) return false;
  // SETTLED, not merely observed. Once the session is actually down the intent
  // has been honoured and must stop overriding the reading, or the page would
  // sit on "Disconnecting…" over a genuinely disconnected session.
  //
  // ONE QUESTION, THE SAME ONE THE HEADLINE ASKS: is there still a session?
  // This used to be a hand-rolled conjunction of all three copies plus two
  // string comparisons — a fourth opinion about the state, and one that could
  // disagree with the row it was gating.
  const bool readsDown = !health::SessionUp(reading_.ToSignals(/*disconnectRequested=*/false));
  if (readsDown || g_get_monotonic_time() - disconnectRequestedAtUs_ >= kDisconnectIntentUs) {
    disconnectRequestedAtUs_ = 0;
    return false;
  }
  return true;
}

void ConnectPage::ClearDisconnectIntent() {
  if (disconnectRequestedAtUs_ == 0) return;
  disconnectRequestedAtUs_ = 0;
  ApplyConnectStatus();
}

// ONE HANDLER FOR THE HERO AND THE BUTTON, and the only place a press turns
// into an action. It captures the action THAT WROTE THE LABEL (actionIsDisconnect_,
// as of the render the user was looking at), records the intent, re-renders so
// the page answers the press in the same frame, and only then relays.
//
// Relaying the action instead of a bare "toggle" is the fix for an earlier
// report: MainWindow used to re-derive it from SdkHost::Connected(), a
// different question from the one that wrote the button's label, and in every
// state where they disagreed a press on a button reading "Disconnect" ran
// StartTunnelUi() + ConnectBestAvailable(). Both sides now read one
// health::Render() over one ConnectReading, so they can no longer disagree —
// the relay stays because the page re-renders BEFORE relaying, and a question
// asked after the relay would get the POST-press answer.
void ConnectPage::RelayConnectPress() {
  // Retry stops the failed session and connects again; it is no disconnect
  // for the user to wait on
  if (actionIsRetry_) {
    disconnectRequestedAtUs_ = 0;
    if (on_retry_connect) on_retry_connect();
    return;
  }
  const bool disconnect = actionIsDisconnect_;
  disconnectRequestedAtUs_ = disconnect ? g_get_monotonic_time() : 0;
  // NOTHING IS CLEARED HERE ANY MORE. The previous fix had to drop the pushed
  // status string on a disconnect press, because that string was the only
  // thing keeping the button on "Disconnect" and nothing ever superseded it —
  // a latch that had to be broken by hand. The reading has no latch to break:
  // the button's word is health::Render()'s `action`, which is false as soon
  // as the session is down, and SdkHost::Disconnect publishes a fresh reading
  // the moment it has torn the session down.
  ApplyConnectStatus();  // answer the press NOW, before the SDK says anything
  if (on_connect_action) {
    on_connect_action(disconnect);
    return;
  }
  if (on_toggle_connect) on_toggle_connect();
}

// ---- the one status writer ----------------------------------------------------

void ConnectPage::ApplyConnectStatus() {
  // ONE READING, ONE CALL, FOUR CHANNELS OUT. The headline, the dot, the hero
  // pose and the button's word are fields of a single health::Reading — they
  // are not four branches that happen to agree today. Nothing below asks the
  // SDK anything; every input is a field of reading_, sampled together.
  //
  // Asked once, here, because DisconnectIntentLive SETTLES the intent as a
  // side effect and the label has to see the same answer the headline did.
  const bool intent = DisconnectIntentLive();
  // A disconnect the command queue has not settled yet reads as one in
  // flight too, and a connect as a session coming up (ConnectTap.hpp): the
  // reading can be from before the tap until then.
  const bool disconnecting = intent || tapPending_ == connect_tap::Pending::Disconnect;
  const health::Signals signals = connect_tap::Fold(reading_.ToSignals(disconnecting), tapPending_);
  const health::Reading view = health::Render(signals);
  renderedState_ = view.state;

  const Glib::ustring text = T_(view.textKey, view.textEnglish);
  const char* dot = kDotIdle;
  switch (view.dot) {
    case health::Dot::Green: dot = kDotGreen; break;
    case health::Dot::Connecting: dot = kDotConnecting; break;
    case health::Dot::Coral: dot = kDotCoral; break;
    case health::Dot::Amber: dot = kDotAmber; break;
    case health::Dot::Idle: dot = kDotIdle; break;
  }
  ConnectCanvas::State heroState = ConnectCanvas::State::Disconnected;
  switch (view.hero) {
    case health::Hero::Connected: heroState = ConnectCanvas::State::Connected; break;
    case health::Hero::Connecting: heroState = ConnectCanvas::State::Connecting; break;
    case health::Hero::Error: heroState = ConnectCanvas::State::Error; break;
    case health::Hero::Processing: heroState = ConnectCanvas::State::Processing; break;
    case health::Hero::Disconnected: heroState = ConnectCanvas::State::Disconnected; break;
  }
  const bool showNotProtected = view.showNotProtected;

  statusText_->set_text(text);
  statusDot_->set_markup(std::string("<span size='") + std::to_string(10 * PANGO_SCALE) +
                         "' foreground='" + dot + "'>●</span>");
  // Simple only: Advanced omits the restatement deliberately
  kit::SetTextOrCollapse(*protectionText_,
                         (!advanced_ && showNotProtected)
                             ? T_("conn_not_protected",
                                  "Your internet traffic is not protected.")
                             : "");
  // Traffic held, not exposed (Windows #27): nothing proven carries the
  // session, so what is routed into the tunnel goes nowhere, and the line says
  // whether the kill switch's floor keeps it from leaving another way.
  Glib::ustring held;
  if (health::TrafficHeld(view, signals)) {
    const KillSwitchStatus killSwitch = host_.CurrentKillSwitchStatus();
    const health::HeldLine line =
        health::HeldLineFor(killSwitch.installed_known &&
                            killSwitch.installed == ctl::KillSwitchState::Connected);
    held = T_(line.key, line.english);
  }
  kit::SetTextOrCollapse(*trafficHeldText_, held);
  // why it is not connected yet, or what Retry does (the SDK's window diagnosis)
  const health::ReasonLine reason = health::ReasonLineFor(view.state, reading_.stallReason);
  const Glib::ustring reasonText =
      reason.key ? Glib::ustring(T_(reason.key, reason.english)) : Glib::ustring();
  kit::SetTextOrCollapse(*statusReasonText_, reasonText);
  // The daemon's notice first: it says what already happened. The failsafe's
  // warning says what happens next if nothing changes.
  Glib::ustring notice = daemonNotice_;
  if (notice.empty() && failsafeArmed_) {
    const failsafe_notice::Copy armed = failsafe_notice::ArmedCopy();
    notice = T_(armed.key, armed.english);
  }
  kit::SetTextOrCollapse(*daemonNoticeText_, notice);

  canvas_->SetState(heroState);
  // the hero's accessible name IS the current status text (its content is a
  // decorative canvas, so it gets no automatic name)
  kit::SetAccessibleLabel(*hero_, text);

  // the action: filled Connect vs outlined Disconnect (four channels — word,
  // fill, dot, status line)
  //
  // IT CAME OUT OF THE SAME Render() CALL AS THE HEADLINE ABOVE. There is no
  // second expression here to keep in step with the first: `view.action` is a
  // field of the value that produced `text`, `dot` and `heroState`. Cached
  // because it is also the answer every caller gets to "what does the next
  // press do" (ConnectActionIsDisconnect, and the action RelayConnectPress
  // hands to MainWindow) — deriving the action anywhere else is what let a
  // button labelled Disconnect run the connect path.
  const bool isDisconnect = view.action == health::Action::Disconnect;
  actionIsDisconnect_ = isDisconnect;
  actionIsRetry_ = view.action == health::Action::Retry;
  connectBtn_->set_label(actionIsRetry_ ? T_("retry", "Retry")
                         : isDisconnect ? T_("disconnect", "Disconnect")
                                        : T_("connect", "Connect"));
  connectBtn_->remove_css_class(isDisconnect ? "ur-pane-primary" : "ur-pane-secondary");
  connectBtn_->add_css_class(isDisconnect ? "ur-pane-secondary" : "ur-pane-primary");
  // A teardown the user asked for is IN FLIGHT, not offered again. Leaving the
  // control live here would let a second press — on a button still reading
  // "Disconnect" while the reading has not caught up — take the connect branch
  // and start a tunnel out of a disconnect. The intent expires on its own
  // (kDisconnectIntentUs), so this can never latch off.
  connectBtn_->set_sensitive(!intent);
  hero_->set_sensitive(!intent);
  // the strip says what this row says, from this render
  if (on_status_rendered) on_status_rendered(text, dot);
}

void ConnectPage::RepublishStatus() {
  ApplyConnectStatus();
  ApplyLocationRow();
}

void ConnectPage::ApplyBalanceNotice(const balance_notice::Signals& signals) {
  heldAlert_->set_visible(balance_notice::HeldAlert(signals));
  // the alert leads with when the free data refreshes, kept current while it shows
  if (data_info::AlertShowsFreeRefresh(balance_notice::HeldAlert(signals))) {
    if (!freeRefreshTicker_.Running()) {
      freeRefreshTicker_.Start([this] {
        freeRefreshLabel_->set_text(
            Format(T_("insufficient_balance_refreshes_in", "Free data refreshes in {}."),
                   FreeRefreshCountdownText()));
      });
    }
  } else {
    freeRefreshTicker_.Stop();
  }
}

void ConnectPage::ApplyBalanceRecovery(const balance_notice::RecoveryLines& lines,
                                       int64_t reservedByteCount) {
  const std::string kindText = OutOfBalanceKindText(lines.kind, reservedByteCount);
  balanceKindLabel_->set_text(kindText);
  balanceKindLabel_->set_visible(!kindText.empty());
  balanceRecoveryRow_->set_visible(lines.willReconnect);
  balanceRecoveryCancel_->set_visible(lines.cancel);
}

void ConnectPage::OnCommandsSettled() {
  if (resyncOwed_) Resync();
}

void ConnectPage::SetTapPending(connect_tap::Pending pending) {
  if (tapPending_ == pending) return;
  tapPending_ = pending;
  ApplyConnectStatus();
  // the panes gate on the verdict this render produced
  ApplySessionRows();
  ApplySessionCardsVisibility();
  ApplyLiveStatsGroup();
}

void ConnectPage::SetDaemonNotice(const Glib::ustring& notice) {
  daemonNotice_ = notice;
  ApplyConnectStatus();
}

void ConnectPage::SetFailsafeArmed(bool armed) {
  if (armed == failsafeArmed_) return;
  failsafeArmed_ = armed;
  ApplyConnectStatus();
}

// THE ONLY WRITER OF THE CONNECTION STATE ON THIS PAGE. It takes the whole
// reading or none of it: there is no setter that can move one field forward
// and leave its siblings behind.
void ConnectPage::ApplyConnectReading(const ConnectReading& reading) {
  // The 1 Hz re-read below arrives whether or not anything moved; an unchanged
  // reading must not rebuild pane C's rows underneath the user. The intent's
  // 8 s ceiling does not depend on this — Tick() re-renders while one is live.
  if (reading == reading_ && renderedApplied_) return;
  renderedApplied_ = true;
  reading_ = reading;
  ApplyConnectStatus();
  // Pane B/C carry gates that used to test stats_.connected — a LOOSER
  // question than the headline's. Re-apply them against the verdict this
  // render just produced so "Connected to N providers" cannot appear under
  // "Connecting to providers".
  ApplySessionRows();
  ApplySessionCardsVisibility();
  ApplyLiveStatsGroup();
}

void ConnectPage::ApplyStats(const LiveStats& stats) {
  stats_ = stats;
  // the hero's grid feed — fed unconditionally, empty list included (the
  // canvas renders it as the bare lattice; an empty grid is a NORMAL state)
  canvas_->SetGrid(stats.gridPoints, stats.gridWidth, stats.gridHeight);
  // the same grid, counted by proven address family; an empty grid reads as
  // three "disconnected" columns
  if (ipFamilyStatusRow_) ipFamilyStatusRow_->SetGrid(stats.gridPoints);

  // 3.1 pane B header carries the live throughput; with no session the line
  // COLLAPSES entirely rather than reading "0 bps"
  if (paneB_.meta) {
    kit::SetTextOrCollapse(*paneB_.meta,
                           ConnectedNow()
                               ? Glib::ustring("↓ " + FormatBitRate(stats.downBitsPerSecond) +
                                               "   ↑ " + FormatBitRate(stats.upBitsPerSecond))
                               : Glib::ustring());
  }
  // provide indicator: solid dot = Network tier, ring = Public, muted = not
  // providing; the Earnings page's rule (ProvideModeGlyph.hpp)
  if (provideDot_) {
    const auto visual = ProvideModeGlyphFor(stats.provideMode, stats.providePaused);
    provideDot_->set_markup(std::string("<span foreground='") + visual.colorHex + "'>" +
                            visual.glyph + "</span>");
  }
  if (discoverableText_) {
    discoverableText_->set_text(
        stats.provideEnabled && stats.provideHasNetworkKey
            ? T_("device_discoverable", "This device is discoverable")
            : T_("device_not_discoverable",
                 "Enable provide mode to make this device discoverable"));
  }
  // The provide line: not while the count is unknown (the provider-only
  // device as a daemon that predates its client count reports it), since a
  // count there would be a guess.
  if (provideStatsText_) {
    Glib::ustring provide;
    switch (ProvideLineFor(stats.provideEnabled, stats.providePaused,
                           stats.provideClientsUnknown)) {
      case ProvideLine::Paused:
        provide = T_("providing_paused", "Providing (paused)");
        break;
      case ProvideLine::Clients:
        // the catalog carries the plural forms; never inflect here
        provide = Format(TN_("providing_client_count", "Providing to {} client",
                             "Providing to {} clients",
                             static_cast<unsigned long>(stats.provideClients)),
                         stats.provideClients);
        break;
      case ProvideLine::None:
        break;
    }
    kit::SetTextOrCollapse(*provideStatsText_, provide);
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

  ApplyLiveStatsGroup();
  ApplySessionRows();
  ApplySessionCardsVisibility();
  ApplyConnectStatus();
}

// 4.3 the provider-count entry: gated on the AGGREGATE reading (the verdict
// ApplyConnectStatus just rendered), so "Connected to 11 providers" can never
// appear under "Connecting to providers". It used to be gated on
// `stats_.connected && connected_` — two of the three copies, ANDed, which is
// a third question again.
void ConnectPage::ApplyLiveStatsGroup() {
  if (!liveStatsGroup_ || !providerCountText_) return;
  const bool connected = ConnectedNow();
  const bool connecting = ConnectingNow();
  const bool show = connected || connecting;
  liveStatsGroup_->set_visible(show);
  kit::SetTextOrCollapse(
      *providerCountText_,
      connected ? Glib::ustring(Format(TN_("connected_provider_count", "Connected to {} provider",
                                           "Connected to {} providers",
                                           static_cast<unsigned long>(stats_.providerCount)),
                                       stats_.providerCount))
      : connecting ? Glib::ustring(T_("connecting_status_indicator", "Connecting to providers"))
                   : Glib::ustring());
  if (providerCountLine_) {
    kit::SetAccessibleLabel(*providerCountLine_, providerCountText_->get_text());
    // A row that cannot act must not claim it can: with the callback
    // unassigned (the globe sheet cannot be owned here — it needs
    // MainWindow's LocationOverrideController) the row stays greyed rather
    // than swallowing the click.
    providerCountLine_->set_sensitive(show && on_open_provider_locations != nullptr);
  }
  if (foldDoorGlobe_) {
    // pane A's second door to the globe follows the same rule
    foldDoorGlobe_->set_visible(show);
    foldDoorGlobe_->set_sensitive(show && on_open_provider_locations != nullptr);
  }
}

// ---- pane B: the routing-decision list ----------------------------------------

// The activity row, built once per reconcile key. Normal: a static row, no
// tab stop (200 of them on the way to Connect would be hostile). Advanced: the
// same row as a real button, clickable, in tab order, Enter/Space; a row whose
// id the feed omitted cannot be inspected, so it renders inert rather than
// looking like its neighbours and swallowing the click. A host's group row
// is the same row over the host's aggregate, and its click is the drill-in,
// not a selection.
ConnectPage::ConnectionRow ConnectPage::BuildConnectionRow(const std::string& key,
                                                           const ConnectionItem& item) {
  ConnectionRow row;
  row.key = key;
  row.group = item.group != nullptr;
  if (!row.group) row.actionId = item.action->BlockActionId.value_or(std::string());
  row.selectable = advanced_;
  if (advanced_) {
    row.button = kit::MakePaneListRowButton(kListRowHeight);
    row.button.root->add_css_class("ur-pane-row-36");  // the CSS floor is 40
    row.root = row.button.root;
    row.dot = row.button.dot;
    row.title = row.button.title;
    row.meta = row.button.meta;
    if (row.group) {
      const std::string host = item.group->host;
      row.button.root->set_sensitive(!host.empty());
      row.button.root->signal_clicked().connect([this, host] { DrillIntoConnectionGroup(host); });
    } else {
      row.button.root->set_sensitive(!row.actionId.empty());
      const std::string id = row.actionId;
      row.button.root->signal_clicked().connect(
          [this, id] { SelectConnection(id); });  // by id, captured by value
    }
    // A secondary click, Shift+F10 or the Menu key opens the row's menu: the
    // inspector's quick actions and Copy details, on this row rather than the
    // selection, so they are reachable while pane C is folded. By key, like
    // the click.
    Gtk::Widget* anchor = row.root;
    auto press = Gtk::GestureClick::create();
    press->set_button(GDK_BUTTON_SECONDARY);
    press->signal_pressed().connect([this, key, anchor](int, double x, double y) {
      OpenConnectionRowMenu(key, *anchor, x, y);
    });
    row.root->add_controller(press);
    auto keys = Gtk::ShortcutController::create();
    keys->add_shortcut(Gtk::Shortcut::create(
        Gtk::ShortcutTrigger::parse_string("<Shift>F10|Menu"),
        Gtk::CallbackAction::create([this, key](Gtk::Widget& widget, const Glib::VariantBase&) {
          OpenConnectionRowMenu(key, widget, -1, -1);
          return true;
        })));
    row.root->add_controller(keys);
  } else {
    auto plain = kit::MakePaneListRow(kListRowHeight);
    row.root = plain.root;
    row.dot = plain.dot;
    row.title = plain.title;
    row.meta = plain.meta;
  }
  // the row's own name is the whole announcement: its parts must not be read
  // out again after it
  kit::MarkDecorative(*row.title);
  kit::MarkDecorative(*row.meta);
  UpdateConnectionRow(row, item);
  return row;
}

// Everything a push can change about a row already on screen, written in
// place: the counters and the meta line, the title, the verdict's dot and
// word. The widget, its focus and its hover are untouched. A group row
// writes the host's aggregate: the precedence verdict, the sums, the latest
// decision's age.
void ConnectPage::UpdateConnectionRow(ConnectionRow& row, const ConnectionItem& item) {
  const connection_filter::Group* group = item.group;
  const urnet::BlockAction* action = item.action;
  row.timeMs = group ? group->latestMs : action->Time;
  row.byteCount = group ? group->byteCount : action->ByteCount;
  row.packetCount = group ? group->packetCount : action->PacketCount;
  row.groupConnections = group ? group->connections : 0;
  const bool block = group ? group->blocked() : action->Block;
  const bool local = group ? group->bypassed() : action->Local;
  std::string title = group ? group->host : BlockActionTitle(*action);
  if (title.empty()) title = T_("unknown", "unknown");
  row.title->set_text(title);
  const char* dot = VerdictDot(block, local);
  if (row.dotColor != dot) {
    row.dotColor = dot;
    row.dot->set_markup(DotMarkup(7, dot));
  }
  WriteConnectionRowMeta(row, NowMillis());
  // the dot is decorative: the NAME is the only place the color's meaning
  // exists for a screen reader; a group's name carries its count too
  row.name = title;
  if (group) row.name += ", " + ConnectionCountText(row.groupConnections);
  row.name += Glib::ustring(", ") + VerdictWord(block, local);
  AnnounceConnectionRow(row);
}

void ConnectPage::WriteConnectionRowMeta(ConnectionRow& row, int64_t nowMs) {
  std::string meta =
      ConnectionRowMeta(row.groupConnections, row.timeMs, row.byteCount, row.packetCount, nowMs);
  if (meta == row.metaText) return;  // a quiet second costs no layout
  row.metaText = std::move(meta);
  row.meta->set_text(row.metaText);
}

void ConnectPage::AnnounceConnectionRow(ConnectionRow& row) {
  const bool selected = row.selectable && !row.group && !selectedConnectionId_.empty() &&
                        row.actionId == selectedConnectionId_;
  // selection rides THREE channels: the fill step and the 2px accent bar
  // (ApplyConnectionSelectionVisuals), and the announced name gaining the
  // suffix
  Glib::ustring announced =
      selected ? row.name + ", " + T_("adv_selected", "selected") : row.name;
  if (announced == row.announced) return;
  row.announced = std::move(announced);
  kit::SetAccessibleLabel(*row.root, row.announced);
}

// Incremental. The feed pushes about once a second during live traffic (the
// counters are in its fingerprint), and a rebuild on every push threw away the
// keyboard focus and hover of an Advanced row and re-announced the list to a
// screen reader. Rows are keyed by block action id (ConnectionRowKey) and
// reconciled against the visible slice (KeyedReconcile.hpp): a new decision
// is inserted, a living row is rewritten in place, a row past the cap, out of
// the feed or filtered out is removed. The verdict filter and the search
// re-evaluate membership through this same pass, so a filter change is an
// edit, not a rebuild; resetScroll reads its new result set from the top.
// Group-by-host folds the same filtered feed into one row per display host,
// keyed by the host: a second kind of row in the same pass, not a second
// list, and the cap counts groups.
void ConnectPage::ApplyConnectionsList(bool resetScroll) {
  if (!connectionsHost_) return;
  // The Advanced Mode flip changes the row type (static <-> selectable),
  // which an in-place update cannot morph: the one path that still clears,
  // and a user gesture, never a push. The handles go before the widgets.
  if (connectionRowsSelectable_ != advanced_) {
    connectionRows_.clear();
    RemoveAllChildren(*connectionsHost_);
    connectionRowsSelectable_ = advanced_;
  }

  const size_t total = blockActions_ ? blockActions_->size() : 0;
  // NEWEST FIRST. The Linux feed is delivered oldest-first
  // (BlockActionViewController::getBlockActions is the window in arrival
  // order), so the list is walked from the back — the already-shipped
  // consumer does the same (SplitRulesSheet: "live activity, newest first").
  // Read forward, the 200-row cap would keep the 200 OLDEST decisions and no
  // newly contacted host would ever appear again on a busy session.
  // The filter runs over the whole cached feed (the count needs every match),
  // and the cap applies to what passes.
  std::vector<ConnectionItem> visible;
  std::vector<std::string> keys;
  std::vector<connection_filter::FoldMember> members;
  int64_t passed = 0;
  if (blockActions_) {
    for (auto it = blockActions_->rbegin(); it != blockActions_->rend(); ++it) {
      if (!connection_filter::Passes(verdictFilter_, connectionsQuery_, FilterDecision(*it))) {
        continue;
      }
      ++passed;
      if (connectionsGrouped_) {
        members.push_back(FoldMemberOf(*it));
        continue;
      }
      if (visible.size() >= kMaxConnectionRows) continue;  // a cap, not a scroll budget
      visible.push_back(ConnectionItem{&*it, nullptr});
      keys.push_back(ConnectionRowKey(*it));
    }
  }
  // the groups outlive the reconcile below, which points into them
  const std::vector<connection_filter::Group> groups = connection_filter::FoldGroups(members);
  for (const auto& group : groups) {
    if (visible.size() >= kMaxConnectionRows) break;
    visible.push_back(ConnectionItem{nullptr, &group});
    keys.push_back("g:" + group.host);
  }

  std::vector<std::string> onScreen;
  onScreen.reserve(connectionRows_.size());
  for (const auto& row : connectionRows_) onScreen.push_back(row.key);
  for (const reconcile::Step& step : reconcile::Plan(onScreen, keys)) {
    const auto at = connectionRows_.begin() + static_cast<std::ptrdiff_t>(step.index);
    switch (step.kind) {
      case reconcile::StepKind::Remove:
        connectionsHost_->remove(*at->root);
        connectionRows_.erase(at);
        break;
      case reconcile::StepKind::Update:
        UpdateConnectionRow(*at, visible[step.wantedIndex]);
        break;
      case reconcile::StepKind::Move: {
        ConnectionRow row = std::move(connectionRows_[step.from]);
        connectionRows_.erase(connectionRows_.begin() + static_cast<std::ptrdiff_t>(step.from));
        UpdateConnectionRow(row, visible[step.wantedIndex]);
        if (step.index == 0) {
          connectionsHost_->reorder_child_at_start(*row.root);
        } else {
          connectionsHost_->reorder_child_after(*row.root, *connectionRows_[step.index - 1].root);
        }
        connectionRows_.insert(connectionRows_.begin() + static_cast<std::ptrdiff_t>(step.index),
                               std::move(row));
        break;
      }
      case reconcile::StepKind::Insert: {
        ConnectionRow row = BuildConnectionRow(keys[step.wantedIndex], visible[step.wantedIndex]);
        if (step.index == 0) {
          connectionsHost_->insert_child_at_start(*row.root);
        } else {
          connectionsHost_->insert_child_after(*row.root, *connectionRows_[step.index - 1].root);
        }
        connectionRows_.insert(at, std::move(row));
        break;
      }
    }
  }

  if (resetScroll && connectionsScroll_) connectionsScroll_->get_vadjustment()->set_value(0.0);

  if (connectionsCount_) {
    // the full feed count, even though rendering caps at 200 rows; while a
    // filter holds rows back, "N hosts of M", and folded, the hosts of the
    // decisions that passed
    const auto count = connection_filter::CountFor(
        verdictFilter_, connectionsQuery_, connectionsGrouped_,
        static_cast<int64_t>(groups.size()), passed, static_cast<int64_t>(total));
    std::string text;
    if (blockActions_) {
      text = Format(TN_("host_count", "{} host", "{} hosts",
                        static_cast<unsigned long>(count.shown)),
                    count.shown);
      if (count.ofTotal) text += " " + Format(T_("of_total", "of {}"), count.of);
    }
    kit::SetTextOrCollapse(*connectionsCount_, text);
  }
  if (connectionsClear_) {
    connectionsClear_->set_visible(
        connection_filter::ClearOffered(verdictFilter_, connectionsQuery_, connectionsGrouped_));
  }
  ApplyVerdictRatioBar();
  ApplySessionCardsVisibility();
  ApplyConnectionSelectionVisuals();
  ApplyInspector();  // a selection that aged out of the feed must SAY so
}

// The ratio bar's three shares, recomputed only on the block-actions and
// block-stats pushes: one pass over the cached window for the bypassed count
// and a redraw, so a live session pays nothing per frame. It collapses while
// there is nothing to proportion rather than drawing an empty track.
void ConnectPage::ApplyVerdictRatioBar() {
  if (!verdictRatioBar_) return;
  int64_t bypassed = 0;
  if (blockActions_) {
    for (const auto& action : *blockActions_) {
      if (!action.Block && action.Local) ++bypassed;
    }
  }
  const auto ratio = connection_filter::VerdictRatio(
      blockStats_ ? blockStats_->AllowedCount : 0, blockStats_ ? blockStats_->BlockedCount : 0,
      bypassed);
  verdictRatioBar_->set_visible(ratio.has_value());
  if (!ratio) return;
  verdictRatio_ = *ratio;
  verdictRatioBar_->queue_draw();
}

// The decision or host behind a row's key, resolved from the feed now and
// owned: the feed is live, and neither the menu nor its presses may point
// into a list the next push replaces. A group resolves to its host's
// aggregate over the same filtered feed the list folded, ruled on as the
// host itself (quick_action::GroupFacts), with its newest decision's
// deciding override; nullopt once the row has left the feed.
std::optional<ConnectPage::RowTarget> ConnectPage::ResolveRowTarget(const std::string& key) const {
  if (!blockActions_) return std::nullopt;
  if (key.rfind("g:", 0) != 0) {
    for (const auto& action : *blockActions_) {
      if (ConnectionRowKey(action) == key) return RowTarget{action, QuickFacts(action)};
    }
    return std::nullopt;
  }
  const std::string host = key.substr(2);
  std::vector<connection_filter::FoldMember> members;
  const urnet::BlockAction* newest = nullptr;
  for (auto it = blockActions_->rbegin(); it != blockActions_->rend(); ++it) {
    if (!connection_filter::Passes(verdictFilter_, connectionsQuery_, FilterDecision(*it))) {
      continue;
    }
    if (BlockActionTitle(*it) != host) continue;
    if (!newest) newest = &*it;
    members.push_back(FoldMemberOf(*it));
  }
  if (!newest) return std::nullopt;
  const connection_filter::Group group = connection_filter::FoldGroups(members).front();
  RowTarget target;
  target.facts =
      quick_action::GroupFacts(host, group.blocked(), group.bypassed(), QuickFacts(*newest));
  if (!host.empty()) target.action.Hosts = urnet::StringList{host};
  target.action.Block = group.blocked();
  target.action.Local = group.bypassed();
  target.action.Time = group.latestMs;
  target.action.ByteCount = group.byteCount;
  target.action.PacketCount = group.packetCount;
  target.action.OverrideId = newest->OverrideId;
  target.action.BlockOverride = newest->BlockOverride;
  target.action.RouteOverride = newest->RouteOverride;
  return target;
}

// The row's menu, built at open and never cached: the quick actions read the
// live host rules, and a menu that kept the state from when its row was
// built would offer to remove rules that are gone. The state and the target
// are captured into the items, so a press does exactly what the open menu
// showed, as the inspector's buttons do. x, y are the press in the anchor
// row's coordinates, or negative from the keyboard (the menu points at the
// row). One popover, on the list area, so a row the next push removes takes
// nothing with it.
void ConnectPage::OpenConnectionRowMenu(const std::string& key, Gtk::Widget& anchor, double x,
                                        double y) {
  if (!connectionsArea_) return;
  const std::optional<RowTarget> target = ResolveRowTarget(key);
  if (!target) return;  // aged out of the feed: nothing honest to offer
  const quick_action::Facts& facts = target->facts;
  const quick_action::State blockState =
      quick_action::StateFor(facts, hostRules_, quick_action::Kind::Block);
  const quick_action::State routeState =
      quick_action::StateFor(facts, hostRules_, quick_action::Kind::Route);

  if (!rowMenu_) {
    rowMenu_ = Gtk::make_managed<Gtk::Popover>();
    rowMenu_->set_parent(*connectionsArea_);
    rowMenu_->set_position(Gtk::PositionType::BOTTOM);
  }
  auto* menu = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto item = [this, menu](const char* text, bool sensitive, bool active,
                           std::function<void()> run) {
    auto* button = Gtk::make_managed<Gtk::Button>();
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_xalign(0);
    button->set_child(*label);
    button->add_css_class("flat");
    button->set_sensitive(sensitive);
    kit::SetAccessibleLabel(*button, active ? Glib::ustring(text) + ", " +
                                                  T_("adv_rule_active",
                                                     "rule active, click to remove")
                                            : Glib::ustring(text));
    button->signal_clicked().connect([this, run] {
      rowMenu_->popdown();
      run();
    });
    menu->append(*button);
  };
  item(QuickActionText(quick_action::LabelFor(blockState, quick_action::Kind::Block, facts)),
       blockState.enabled, blockState.active, [this, blockState, facts] {
         RunQuickAction(quick_action::Kind::Block, blockState, facts);
       });
  item(QuickActionText(quick_action::LabelFor(routeState, quick_action::Kind::Route, facts)),
       routeState.enabled, routeState.active, [this, routeState, facts] {
         RunQuickAction(quick_action::Kind::Route, routeState, facts);
       });
  menu->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
  item(T_("adv_copy_details", "Copy details"), true, false,
       [this, action = target->action] { CopyConnectionDetails(action); });
  rowMenu_->set_child(*menu);

  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds(anchor.gobj(), GTK_WIDGET(connectionsArea_->gobj()), &bounds)) {
    const bool pointer = 0 <= x && 0 <= y;
    rowMenu_->set_pointing_to(
        pointer ? Gdk::Rectangle(static_cast<int>(bounds.origin.x + x),
                                 static_cast<int>(bounds.origin.y + y), 1, 1)
                : Gdk::Rectangle(static_cast<int>(bounds.origin.x),
                                 static_cast<int>(bounds.origin.y),
                                 static_cast<int>(bounds.size.width),
                                 static_cast<int>(bounds.size.height)));
  }
  rowMenu_->popup();
}

// The 1s reading of the meta lines' age: one string per row from the counters
// it keeps, with no feed read and no rebuild.
void ConnectPage::RefreshConnectionRowTimes() {
  if (connectionRows_.empty()) return;
  const int64_t nowMs = NowMillis();
  for (auto& row : connectionRows_) {
    if (0 < row.timeMs) WriteConnectionRowMeta(row, nowMs);
  }
}

void ConnectPage::SelectConnection(const std::string& id) {
  // clicking the selected row clears it (a toggle)
  selectedConnectionId_ = (selectedConnectionId_ == id) ? std::string() : id;
  ApplyConnectionSelectionVisuals();
  ApplyInspector();
}

void ConnectPage::ApplyConnectionSelectionVisuals() {
  // repaint WITHOUT rebuilding: a rebuild would destroy keyboard focus
  for (auto& row : connectionRows_) {
    if (!row.selectable) continue;
    kit::SetPaneListRowSelected(row.button, !row.group && !selectedConnectionId_.empty() &&
                                                row.actionId == selectedConnectionId_);
    AnnounceConnectionRow(row);
  }
}

// The verdict filter's change, from its segment. A changed filter is a new
// result set: the same incremental pass, read from the top.
void ConnectPage::OnConnectionsVerdictChanged(connection_filter::Verdict verdict) {
  if (verdict == verdictFilter_) return;
  verdictFilter_ = verdict;
  ApplyConnectionsList(/*resetScroll=*/true);
}

// What the leaving account searched for and selected must not greet the
// next one (SignOut.hpp: each network starts fresh): Clear's own reset, in
// its one pass, with nothing selected.
void ConnectPage::ResetForSignOut() {
  // an Undo pressed after the sign-out would remove a rule by the leaving
  // account's id, and a menu would rule on its connection
  quickToast_.Dismiss();
  if (rowMenu_) rowMenu_->popdown();
  selectedConnectionId_.clear();
  OnConnectionsClearFilters();
}

// The one-click reset: the state first, then the controls behind the echo
// guard so their handlers do not run the pass again, then one pass.
void ConnectPage::OnConnectionsClearFilters() {
  verdictFilter_ = connection_filter::Verdict::All;
  connectionsQuery_.clear();
  connectionsGrouped_ = false;
  const bool wasUpdating = updatingControls_;
  updatingControls_ = true;
  if (verdictAll_) verdictAll_->set_active(true);
  if (connectionsSearch_) connectionsSearch_->set_text("");
  if (connectionsGroupToggle_) connectionsGroupToggle_->set_active(false);
  updatingControls_ = wasUpdating;
  ApplyConnectionsList(/*resetScroll=*/true);
}

// A group row's click, and the whole of its interaction: the search takes
// the host and the fold goes off, so the group opens in place through the
// filter that already exists. Not a selection (a group has no one connection
// to inspect). The same shape as Clear: state, guarded controls, one pass.
void ConnectPage::DrillIntoConnectionGroup(const std::string& host) {
  if (host.empty()) return;  // an unnamed group has nothing to search for
  connectionsGrouped_ = false;
  connectionsQuery_ = connection_filter::NormalizeQuery(host);
  const bool wasUpdating = updatingControls_;
  updatingControls_ = true;
  if (connectionsGroupToggle_) connectionsGroupToggle_->set_active(false);
  if (connectionsSearch_) connectionsSearch_->set_text(host);
  updatingControls_ = wasUpdating;
  ApplyConnectionsList(/*resetScroll=*/true);
}

void ConnectPage::ApplySessionCardsVisibility() {
  if (!connectionsHost_ || !connectionsEmpty_) return;
  // The list and the sentence SWAP; the connected flag gates which. A filter
  // that hides every row of a session that has some keeps the (empty) list:
  // the count above it says "0 hosts of M", which the sentence would deny.
  const bool showList = connection_filter::ShowList(
      ConnectedNow(), connectionsHost_->get_first_child() != nullptr,
      connection_filter::FilterActive(verdictFilter_, connectionsQuery_),
      blockActions_ ? static_cast<int64_t>(blockActions_->size()) : 0);
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
      ConnectedNow() ? Glib::ustring("↓ " + FormatBitRate(stats_.downBitsPerSecond)) : none);
  add(T_("local", "Local"),
      ConnectedNow() ? Glib::ustring("↑ " + FormatBitRate(stats_.upBitsPerSecond)) : none);
  add(T_("allowed", "Allowed"),
      blockStats_ ? Glib::ustring(FormatCountCompact(blockStats_->AllowedCount)) : none);
  add(T_("blocked", "Blocked"),
      blockStats_ ? Glib::ustring(FormatCountCompact(blockStats_->BlockedCount)) : none);
  add(T_("connections", "Connections"),
      blockActions_ ? Glib::ustring(FormatCountCompact(
                          static_cast<int64_t>(blockActions_->size())))
                    : none);
  if (advanced_) {
    // §4.3 row 6: the PRE-CLAMP connection status. This host applies NO clamp
    // (Windows' LiveStats rewrites the status to RPC_ONLY / SERVICE_DOWN and
    // Advanced reads the raw field through it), so stats_.connectionStatus IS
    // the pre-clamp reading and this row is truthful exactly as written.
    //
    // The CLAMP that used to be missing here now lives in the reading:
    // ConnectReading::tunnelBound is "a DeviceRemote is still bound over the
    // CURRENT control session", an O(1) generation compare with no round trip,
    // and health::Render refuses every non-idle row without it. So a
    // DeviceRemote whose loopback rpc to urnetworkd has gone away can no
    // longer hold the hero on Connected. What is STILL only approximated is a
    // daemon that is alive but has stopped carrying: MainWindow's status poll
    // writes that into the reading (DaemonTunnelGoneReading) rather than the
    // SDK reporting it. This raw row is deliberately unclamped — it is the
    // pre-clamp field, and Advanced exists to show it.
    add(T_("adv_raw_status", "Raw status"),
        stats_.connectionStatus.empty() ? Glib::ustring(T_("adv_none", "none"))
                                        : Glib::ustring(stats_.connectionStatus));
    // §4.3 row 7: the denominator behind every inspector "via exit" line.
    // nullopt is UNKNOWN — no session, or no snapshot has landed yet / the rpc
    // threw — and a never-read table rendered as "0" would fabricate the one
    // number this row exists to report. A table that WAS read and is empty
    // renders as a real 0.
    add(T_("adv_exits", "Exits"),
        exits_ ? Glib::ustring(FormatCountCompact(static_cast<int64_t>(exits_->size())))
               : Glib::ustring(T_("adv_none", "none")));
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
  if (present) dnsSettled_ = true;
  // DESIGNSTYLE "Placeholders, not pop-in": before the first reading the four
  // rows are up with their labels (the labels are static) and a skeleton
  // where the On/Off value goes, so the group opens at its settled 4x34 and
  // the values are replaced in place. Only a reading that comes back empty
  // swaps to the unavailable row — the error state, in the same group.
  const bool loading = !present && !dnsSettled_;
  if (dnsRowsPanel_) {
    dnsRowsPanel_->set_visible(present || loading);
    kit::SetBusy(*dnsRowsPanel_, loading);
  }
  if (dnsUnavailableRow_) dnsUnavailableRow_->set_visible(!present && !loading);
  // the editor has nothing to draft from without settings (DnsSheet::Open
  // returns false and does not present) — say so on the control
  if (dnsEditButton_) dnsEditButton_->set_sensitive(present);
  if (foldDoorDns_) foldDoorDns_->set_sensitive(present);
  ApplyDnsRecommendationPill();  // collapses with the rows
  if (loading) {
    const Glib::ustring loadingText = T_("loading", "Loading...");
    auto placeholder = [&loadingText](DnsStatusRow& row, const Glib::ustring& label) {
      if (!row.dot || !row.state) return;
      row.dot->remove_css_class("ur-dot-on");
      row.dot->add_css_class("ur-dot-off");
      row.state->remove_css_class("ur-value-on");
      row.state->set_text(T_("off", "Off"));  // the sizer: the wider of the two values
      kit::SetSkeleton(*row.state, true);
      kit::SetAccessibleLabel(*row.state, label + ", " + loadingText);
    };
    placeholder(dnsDohRow_, T_("dns_over_https", "DNS over HTTPS"));
    placeholder(dnsUnencryptedRow_, T_("unencrypted_dns", "Unencrypted DNS"));
    placeholder(dnsLocalRow_, T_("local_dns", "Local DNS"));
    placeholder(dnsFallbackRow_, T_(kFastDnsLabel.key, kFastDnsLabel.english));
    return;
  }
  if (!present) return;
  auto apply = [](DnsStatusRow& row, const Glib::ustring& label, bool on) {
    if (!row.dot || !row.state) return;
    kit::SetSkeleton(*row.state, false);
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
  apply(dnsFallbackRow_, T_(kFastDnsLabel.key, kFastDnsLabel.english),
        dnsSettings_->EnableFallback);
}

// iOS DnsRecommendationPill parity, with §6's pill keys: a regional
// recommendation NEVER falls through to the safe-defaults nudge.
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

// The §4.1 join: the FIRST address of this action that appears in the
// destination-exit table decides, and its ClientId is then looked up in the
// exit table for that exit's health. Both halves are absent-capable — with no
// snapshot (nullopt) there is nothing to join and the caller must say so
// rather than report "not in the routing table", which would assert a lookup
// that never ran.
std::optional<ConnectPage::ExitRouting> ConnectPage::RoutingForAddresses(
    const std::optional<urnet::StringList>& addresses) const {
  if (!addresses || !destinationExits_) return std::nullopt;
  for (const auto& ip : *addresses) {
    for (const auto& dest : *destinationExits_) {
      if (dest.DestinationIp != ip) continue;
      ExitRouting out;
      out.clientId = dest.ClientId.value_or(std::string());
      out.flowCount = dest.FlowCount;
      // The exit table is read separately and can be UNKNOWN while the
      // destination table is not (each getter is guarded on its own): the
      // routing is still reported, the health rows are simply not claimed.
      if (exits_) {
        for (const auto& exitRow : *exits_) {
          if (!exitRow.ClientId || *exitRow.ClientId != out.clientId) continue;
          out.haveExit = true;
          out.tier = exitRow.Tier;
          out.effectiveTier = exitRow.EffectiveTier;
          out.exitFlowCount = exitRow.FlowCount;
          out.dialFailureCount = exitRow.DialFailureCount;
          out.quarantined = exitRow.Quarantined;
          out.warning = exitRow.Warning;
          out.warningCause = exitRow.WarningCause;
          out.proven = exitRow.Proven;
          out.probeAgeSeconds = exitRow.ProbeAgeSeconds;
          break;
        }
      }
      return out;
    }
  }
  return std::nullopt;
}

void ConnectPage::ApplyInspector() {
  if (!inspectorGroup_ || !inspectorRowsAbove_ || !inspectorRowsBelow_) return;
  RemoveAllChildren(*inspectorRowsAbove_);
  RemoveAllChildren(*inspectorRowsBelow_);

  const urnet::BlockAction* action = SelectedConnectionAction();
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
    // no selection, no actions: a row left behind would offer rules on a
    // connection that is gone
    inspectorActions_->set_visible(false);
    inspectorReasonRow_->set_visible(false);
    blockQuick_ = {};
    routeQuick_ = {};
    return;
  }

  std::string title = BlockActionTitle(*action);
  if (title.empty()) title = T_("unknown", "unknown");
  inspectorTitle_->set_text(title);
  inspectorDot_->set_markup(DotMarkup(8, VerdictDot(*action)));
  inspectorVerdict_->set_text(VerdictSentence(action->Block, action->Local));
  if (inspectorClear_) inspectorClear_->set_visible(true);

  // The quick actions, derived on every render from the live host rules: the
  // press handlers act on the stored state, so a press does exactly what the
  // button showed.
  {
    const quick_action::Facts facts = QuickFacts(*action);
    blockQuick_ = quick_action::StateFor(facts, hostRules_, quick_action::Kind::Block);
    routeQuick_ = quick_action::StateFor(facts, hostRules_, quick_action::Kind::Route);
    ApplyQuickActionButton(*inspectorBlockButton_, blockQuick_, quick_action::Kind::Block, facts);
    ApplyQuickActionButton(*inspectorRouteButton_, routeQuick_, quick_action::Kind::Route, facts);
    inspectorActions_->set_visible(true);
  }

  // the fields go over Reason until it is written, then under it
  Gtk::Box* rows = inspectorRowsAbove_;
  auto add = [&rows](const Glib::ustring& key, const Glib::ustring& value) {
    auto row = kit::MakePaneKeyValueRow(key, value, kKeyValueRowHeight);
    CapNatural(row.key, 16);
    CapNatural(row.value, 20);
    rows->append(*row.root);
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
    ApplyInspectorReason(ReasonText(*action), !overrideId.empty());
    rows = inspectorRowsBelow_;
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
    // §4.1 item 10, in THREE distinguishable readings. The middle one is the
    // answer; the outer two are different kinds of "I don't know" and must not
    // be collapsed into each other.
    if (!destinationExits_) {
      // UNKNOWN: no snapshot has landed for this session (no device, the first
      // 5 s read has not returned, or the rpc threw). Rendering "Not in the
      // routing table" here would assert a lookup that never ran.
      add(T_("adv_via_exit", "Via exit"), T_("adv_none", "none"));
    } else if (const auto routing = RoutingForAddresses(action->Ips)) {
      addText(T_("adv_via_exit", "Via exit"), routing->clientId);
      add(T_("adv_exit_flows", "Flows to this destination"),
          FormatCountCompact(routing->flowCount));
      // The exit's own health, only when the client id resolved to an exit
      // record — a routed destination whose exit has aged out of the window
      // reports the routing and stops there.
      if (routing->haveExit) {
        add(T_("adv_exit_tier", "Exit tier"),
            Glib::ustring(std::to_string(routing->effectiveTier) + " / " +
                          std::to_string(routing->tier)));
        add(T_("adv_exit_flows_total", "Exit flows"), FormatCountCompact(routing->exitFlowCount));
        add(T_("adv_exit_dial_failures", "Dial failures"),
            FormatCountCompact(routing->dialFailureCount));
        add(T_("adv_exit_state", "Exit state"),
            routing->quarantined ? T_("adv_exit_quarantined", "Quarantined")
            : routing->warning   ? T_("adv_exit_warning", "Warning")
            : routing->proven    ? T_("adv_exit_proven", "Proven")
                                 : T_("adv_exit_ok", "OK"));
        if (routing->warning && !routing->warningCause.empty()) {
          addText(T_("adv_exit_warning_cause", "Warning cause"), routing->warningCause);
        }
        if (routing->probeAgeSeconds > 0) {
          add(T_("adv_probe_age", "Probe age"),
              Glib::ustring(std::to_string(routing->probeAgeSeconds) + "s"));
        }
      }
    } else {
      // Read, and this destination is genuinely not in it — the normal reading
      // for a host that resolved after the last refresh.
      add(T_("adv_via_exit", "Via exit"), T_("adv_unknown_exit", "Not in the routing table"));
    }
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

// The persistent Reason row, written in place: the plain value, or for a
// rule-decided connection the link to the split rules sheet. A push that
// changes neither writes nothing, so the link keeps its focus and hover.
void ConnectPage::ApplyInspectorReason(const Glib::ustring& reason, bool link) {
  inspectorReasonRow_->set_visible(true);
  inspectorReasonValue_->set_visible(!link);
  inspectorReasonLink_->set_visible(link);
  if (reason == inspectorReasonText_) return;
  inspectorReasonText_ = reason;
  const Glib::ustring key = T_("adv_reason", "Reason");
  inspectorReasonValue_->set_text(reason);
  kit::SetAccessibleLabel(*inspectorReasonValue_, key + ", " + reason);
  inspectorReasonLinkText_->set_text(reason);
  // a button whose content is a box has no name of its own
  kit::SetAccessibleLabel(*inspectorReasonLink_,
                          key + ", " + reason + ", " +
                              T_("adv_open_split_rules", "open split rules"));
}

// The selection in the current feed, or nullptr: nothing is selected, or the
// action aged out of the window since it was picked.
const urnet::BlockAction* ConnectPage::SelectedConnectionAction() const {
  if (selectedConnectionId_.empty() || !blockActions_) return nullptr;
  for (const auto& candidate : *blockActions_) {
    if (candidate.BlockActionId.value_or(std::string()) == selectedConnectionId_) return &candidate;
  }
  return nullptr;
}

// A quick action button for its state: outlined while a press would create a
// rule, filled action blue while one is in force and a press removes it. The
// word changes too, and the name says the on state, so the fill is never the
// only channel.
void ConnectPage::ApplyQuickActionButton(Gtk::Button& button, const quick_action::State& state,
                                         quick_action::Kind kind,
                                         const quick_action::Facts& facts) {
  const char* label = QuickActionText(quick_action::LabelFor(state, kind, facts));
  button.set_label(label);
  button.set_sensitive(state.enabled);
  if (state.active) {
    button.add_css_class("ur-quick-on");
    kit::SetAccessibleLabel(button, Glib::ustring(label) + ", " +
                                        T_("adv_rule_active", "rule active, click to remove"));
  } else {
    button.remove_css_class("ur-quick-on");
    kit::SetAccessibleLabel(button, label);
  }
}

// A quick action's press, from the inspector or a row's menu, against the
// state its button showed: remove the rule in force, or write the inverse of
// the verdict, then confirm with a toast whose Undo deletes the new rule by
// id. The overrides are re-read at once: the local-state fallback (no
// device) fires no override event.
void ConnectPage::RunQuickAction(quick_action::Kind kind, const quick_action::State& state,
                                 const quick_action::Facts& facts) {
  const quick_action::Effect effect = quick_action::EffectFor(state, kind, facts);
  std::string created;
  const char* message = nullptr;
  switch (effect) {
    case quick_action::Effect::None:
      return;
    case quick_action::Effect::RemoveRule:
      host_.RemoveBlockActionOverride(state.overrideId);
      message = T_("adv_rule_removed", "Rule removed");
      break;
    case quick_action::Effect::CreateBlock:
      created = host_.AddHostBlockRule(state.hosts, true);
      message = T_("adv_host_blocked", "This host will be blocked");
      break;
    case quick_action::Effect::CreateAllow:
      created = host_.AddHostBlockRule(state.hosts, false);
      message = T_("adv_host_allowed", "This host will be allowed");
      break;
    case quick_action::Effect::CreateBypass:
      created = host_.AddHostRouteRule(state.hosts, true);
      message = T_("adv_host_bypassed", "This host will bypass the tunnel");
      break;
    case quick_action::Effect::CreateTunnel:
      created = host_.AddHostRouteRule(state.hosts, false);
      message = T_("adv_host_tunnelled", "This host will use the tunnel");
      break;
  }
  // a create that found nowhere to write wrote nothing to confirm
  if (effect != quick_action::Effect::RemoveRule && created.empty()) return;
  // (a changed rule re-renders the inspector from there)
  ApplyOverrides(host_.BlockActionOverrides(), /*force=*/false);
  if (created.empty()) {
    quickToast_.Show(*this, message);
    return;
  }
  auto alive = alive_;
  quickToast_.Show(*this, message, T_("adv_undo", "Undo"), [this, alive, created] {
    if (!*alive) return;
    host_.RemoveBlockActionOverride(created);
    ApplyOverrides(host_.BlockActionOverrides(), /*force=*/false);
  });
}

// The inspector's fields, in its own words and order, onto the clipboard:
// host, addresses, verdict, reason, override, totals, last decision.
void ConnectPage::CopyConnectionDetails(const urnet::BlockAction& action) {
  std::string text;
  auto line = [&text](const char* key, const std::string& value) {
    if (!text.empty()) text += "\n";
    text += std::string(key) + ": " + value;
  };
  const std::string none = T_("adv_none", "none");
  const std::string hosts = JoinValues(action.Hosts);
  const std::string ips = JoinValues(action.Ips);
  line(T_("adv_host", "Host"), hosts.empty() ? none : hosts);
  line(T_("adv_addresses", "Addresses"), ips.empty() ? none : ips);
  line(T_("adv_verdict", "Verdict"), VerdictSentence(action.Block, action.Local));
  line(T_("adv_reason", "Reason"), ReasonText(action));
  const std::string overrideId = action.OverrideId.value_or(std::string());
  if (!overrideId.empty()) line(T_("adv_override_id", "Override"), overrideId);
  line(T_("adv_packets_total", "Packets (total)"), FormatCountCompact(action.PacketCount));
  line(T_("adv_bytes_total", "Bytes (total)"), FormatByteCountCompact(action.ByteCount));
  if (action.Time > 0) {
    line(T_("adv_last_decision", "Last decision"),
         RelativeTime(std::max<int64_t>(0, (NowMillis() - action.Time) / 1000)));
  }
  get_clipboard()->set_text(text);
  quickToast_.Show(*this, T_("adv_details_copied", "Connection details copied"));
}

// The 5 s exit-routing cache refresh (§4.1), fired from the clock and
// immediately when Advanced Mode turns on.
void ConnectPage::RefreshExitRouting() {
  // §4.1's gate in full: Advanced Mode, this destination on screen, window
  // presenting. A background rpc batch for a pane nobody is looking at is the
  // cost of the feature with none of the value.
  if (!advanced_ || !presenting_ || !pageVisible_) return;

  if (!host_.hasDevice()) {
    // No session. The tables are UNKNOWN, not empty — and the PREVIOUS
    // session's tables must not survive here, or the inspector could join a
    // destination ip against an exit that belonged to a device which no longer
    // exists. Dropped inline (no rpc to make) and the two surfaces re-read.
    if (exits_ || destinationExits_) {
      exits_.reset();
      destinationExits_.reset();
      ApplySessionRows();
      if (!selectedConnectionId_.empty()) ApplyInspector();
    }
    return;
  }

  // SdkHost owns the worker, the host-wide single-flight gate and the marshal
  // back to the main loop: ReadReliability is several SYNCHRONOUS device rpcs
  // taken under the host lock and must never run on the GTK loop. A read
  // already in flight returns false and this tick is SKIPPED rather than
  // queued behind the lock — the next one is 5 s away.
  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t gen = *epoch_;
  host_.RequestReliability(
      ReliabilityRead::ExitsOnly, [this, alive, epoch, gen](ReliabilitySnapshot snap) {
        // Re-checked ON ARRIVAL, not on entry: this lands one rpc batch later
        // and a logout, an Advanced-Mode toggle, a resync or the page's own
        // destruction can have happened in between. alive_ FIRST — the epoch
        // lives inside the page.
        if (!*alive || *epoch != gen) return;
        const bool hadExits = exits_.has_value();
        const size_t before = hadExits ? exits_->size() : 0;
        // Assigned wholesale, INCLUDING a nullopt: a getter that threw makes
        // the table unknown again, and keeping the last good reading would
        // quietly age into a fabrication.
        exits_ = std::move(snap.exits);
        destinationExits_ = std::move(snap.destinationExits);
        // The "Exits" session figure is the denominator behind every "via
        // exit" line; it must not wait for the next stats push to catch up.
        if (hadExits != exits_.has_value() || (exits_ && before != exits_->size())) {
          ApplySessionRows();
        }
        if (advanced_ && !selectedConnectionId_.empty()) ApplyInspector();
      });
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

void ConnectPage::RevealProvideControls() {
  if (!advanced_ && !moreOptionsExpanded_) {
    moreOptionsExpanded_ = true;
    ApplyMoreOptionsVisibility();
  }
  for (Gtk::ToggleButton* button : {provideAuto_, provideAlways_, provideNetwork_, provideNever_}) {
    if (button && button->get_active()) {
      button->grab_focus();
      break;
    }
  }
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

// ONE writer for both halves: the switch shows the REQUEST (local, instant),
// the line under it shows what urnetworkd says is actually installed (a round
// trip, and possibly a refusal). They can legitimately disagree, and the whole
// point of splitting them is that the surface can say so instead of claiming a
// protection that is not in force.
void ConnectPage::ApplyKillSwitchUi() {
  if (!killSwitchToggle_) return;
  const KillSwitchStatus status = host_.CurrentKillSwitchStatus();
  if (killSwitchToggle_->get_active() != status.requested) {
    updatingControls_ = true;
    killSwitchToggle_->set_active(status.requested);
    updatingControls_ = false;
  }
  if (killSwitchNote_ == nullptr) return;
  const KillSwitchCopy copy = KillSwitchStateLine(status);
  killSwitchNote_->set_visible(!copy.line.empty());
  if (copy.line.empty()) return;
  killSwitchNote_->set_markup("<span size='small' foreground='" +
                              HexForMarkup(copy.attention ? kUrDanger : kUrTextMuted) + "'>" +
                              Glib::Markup::escape_text(copy.line) + "</span>");
}

// ---- the provider extender row (EXTENDER.md N7) -------------------------------

// Re-reads the status, and beside a status that shows, the setting the switch
// takes its position from: the bound device's, or while disconnected the
// provider-only device's when the daemon takes the switch's write. No status
// and an unsupported role both hide the row, and a row that will not show
// reads nothing else.
void ConnectPage::ApplyExtenderProvideState() {
  DrawExtenderRow(extender::ProvideRowOf(host_.GetExtenderProvideStatus(),
                                         [this] { return host_.GetProvideExtender(); }));
  // the lag.dev speed tests switch under it, beside the row as just drawn: its
  // setting is read only when it will show
  DrawSpeedTestsRow(extender::SpeedTestsRowOf(extenderRowDrawn_,
                                              host_.MeasurementsSwitchWritable(),
                                              [this] { return host_.GetMeasurementsEnabled(); }));
}

void ConnectPage::DrawSpeedTestsRow(const extender::SpeedTestsRow& row) {
  if (!speedTestsRow_ || !speedTestsToggle_) return;
  // a reading that changes nothing is dropped
  if (speedTestsApplied_ && row == speedTestsDrawn_) return;
  speedTestsApplied_ = true;
  speedTestsDrawn_ = row;
  speedTestsRow_->set_visible(row.visible);
  if (!row.visible) return;
  // the switch shows the setting, written under the echo guard
  if (speedTestsToggle_->get_active() != row.on) {
    updatingControls_ = true;
    speedTestsToggle_->set_active(row.on);
    updatingControls_ = false;
  }
}

// The switch writes the measurements setting at once and holds its new
// position; the next reading of the setting replaces it.
void ConnectPage::OnSpeedTestsToggled() {
  if (!speedTestsToggle_) return;
  // never while hidden: a device or daemon that cannot take the write shows no
  // switch (N1)
  if (!speedTestsDrawn_.visible) return;
  const bool on = speedTestsToggle_->get_active();
  host_.SetMeasurementsEnabled(on);
  speedTestsDrawn_.on = on;
}

void ConnectPage::DrawExtenderRow(const extender::ProvideRow& row) {
  if (!extenderRow_ || !extenderDescription_) return;
  // a push that changes nothing is dropped
  if (extenderRowApplied_ && row == extenderRowDrawn_) return;
  extenderRowApplied_ = true;
  extenderRowDrawn_ = row;
  extenderRow_->set_visible(row.visible);
  extenderDescription_->set_visible(row.visible);
  if (!row.visible) return;
  const std::string text = PaintExtenderProvideRow(*extenderDot_, *extenderState_, row);
  // the switch shows the setting, written under the echo guard
  if (extenderToggle_->get_active() != row.on) {
    updatingControls_ = true;
    extenderToggle_->set_active(row.on);
    updatingControls_ = false;
  }
  // the switch is named Extender and carries the state as its description (N7)
  Glib::Value<Glib::ustring> description;
  description.init(Glib::Value<Glib::ustring>::value_type());
  description.set(text);
  extenderToggle_->update_property(Gtk::Accessible::Property::DESCRIPTION, description);
}

// The switch writes the setting at once and repaints the row with the guess
// before the listener answers; the next status replaces the guess (N7).
void ConnectPage::OnExtenderToggled() {
  if (!extenderToggle_) return;
  // never while hidden: a device that cannot take the write shows no row (N1)
  if (!extenderRowDrawn_.visible) return;
  const bool on = extenderToggle_->get_active();
  host_.SetProvideExtender(on);
  DrawExtenderRow(extender::ProvideRowGuess(on, stats_.provideEnabled));
}

// ---- connect options: the performance profile (§2.8) ---------------------------
// The Linux SdkHost exposes GetPerformanceProfile/SetPerformanceProfile, and a
// nil profile means Auto with everything off.

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
// "Best available provider".
void ConnectPage::ApplyLocationRow() {
  if (!locationText_ || !locationRow_) return;
  // renders from the cached reading: the row is re-rendered on BOTH the
  // location and the peers feed, and the SelectedLocation read is a locked
  // device call + a JSON parse
  const auto& location = selectedLocation_;
  Glib::ustring text = T_("best_available_provider", "Best available provider");
  if (!IsBestAvailableSelected(location)) {
    std::string displayName = SanitizeExternalDisplayText(location->name.value_or(std::string()));
    if (peers_) {
      for (const auto& peer : *peers_) {
        if (IsPeerSelected(location, peer)) {
          displayName = PeerDisplayName(peer);
          break;
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
  if (on_location_rendered) on_location_rendered(text);
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
    const Glib::ustring spec = SanitizeExternalDisplayText(peer.DeviceSpec);
    row.meta->set_text(spec);
    CapNatural(row.title, 18);
    CapNatural(row.meta, 14);
    kit::MarkDecorative(*row.title);
    kit::MarkDecorative(*row.meta);
    kit::SetAccessibleLabel(*row.root, name + ", " + spec);
    peersHost_->append(*row.root);
  }
}

// ---- the feeds (§5) ------------------------------------------------------------

// one shared point list, each chart picks its route's sample
void ConnectPage::PullThroughput() {
  // the same locked reads as RefreshFeeds: not while commands are outstanding
  if (host_.CommandsBusy()) return;
  auto points = std::make_shared<const urnet::ThroughputPointList>(
      host_.ThroughputPoints().value_or(urnet::ThroughputPointList()));
  const double window = static_cast<double>(host_.ThroughputWindowSeconds());
  if (remoteChart_) remoteChart_->SetPoints(points, window);
  if (blockedChart_) blockedChart_->SetPoints(points, window);
  if (localChart_) localChart_->SetPoints(points, window);
  // the transport distribution rides the same throughput tick as the points
  // (and the same 2fps clock pull); the bar dedups by value
  if (transportBar_) transportBar_->SetDistribution(host_.ClientTransportDistribution());
}

// Re-read every feed and re-apply ONLY what changed. Every read below degrades
// to nullopt with no device, and every writer renders that as its own settled
// reading — never a spinner, never a zero.
void ConnectPage::RefreshFeeds(bool force) {
  // With connect commands outstanding every read below would wait on the
  // device's state lock, which the command holds through its rpc, and freeze
  // the window: the queue's settle re-reads instead (MainWindow::
  // OnCommandsSettled).
  if (host_.CommandsBusy()) return;
  // NO STATE MAY PERSIST AFTER ITS PRODUCER STOPS PUBLISHING. The SDK simply
  // goes quiet on some teardowns — that silence is what let the old status
  // string latch on its last word for the rest of the session — so the
  // connection reading is RE-READ here, on the page's own clock, and not only
  // when something pushes. Cheap: ReadConnectReading takes no lock and reads
  // getters the stats feed already reads many times a second.
  ApplyConnectReading(host_.CurrentConnectReading());
  // the extender network: no event fires when the device goes, so a device
  // arriving or leaving is re-read here; the panel drops an unchanged reading
  if (extenderPanel_) extenderPanel_->SetStatus(host_.GetExtenderStatus());
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
      ApplyVerdictRatioBar();  // two of the bar's three inputs
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
  ApplyOverrides(host_.BlockActionOverrides(), force);
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
  if (ReadLocations(force)) {
    locationRowDirty = true;
    ApplyDnsRecommendationPill();
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
    // the extender row's listener fires nothing on registration, so a device
    // arriving, a re-show and the build all re-read the status
    ApplyExtenderProvideState();
    ApplyInspectorVisibility();
    ApplyInspector();
    // Ask urnetworkd what floor is REALLY installed. There is no push for
    // this: the daemon's reaper arms the switch on its own when a tunnel drops
    // unexpectedly, and nothing in the SDK feed knows that happened. A forced
    // refresh is device lifecycle / tab entry / window re-show, i.e. exactly
    // the moments this surface has to be right.
    auto epoch = epoch_;
    const uint64_t seen = *epoch_;
    host_.RefreshKillSwitchStatus([this, epoch, seen](KillSwitchStatus) {
      if (*epoch != seen) return;
      ApplyKillSwitchUi();
      // the held line names whether the floor is in force
      if (trafficHeldText_->get_visible()) ApplyConnectStatus();
    });
  }
}

// The overrides, read once, feed two surfaces: the split rules (an override
// whose route override forces local) behind pane C's count and the sheet,
// and the host rules the inspector's quick actions read, at full fidelity:
// a rule's kind and value are the buttons' whole state, and a rule written
// from any surface (the sheet, the row menu, another device) re-renders them.
void ConnectPage::ApplyOverrides(std::optional<urnet::BlockActionOverrideList> overrides,
                                 bool force) {
  std::optional<urnet::BlockActionOverrideList> rules;
  std::vector<quick_action::HostRule> hostRules;
  if (overrides) {
    urnet::BlockActionOverrideList kept;
    for (const auto& override_ : *overrides) {
      if (override_.RouteOverride && override_.RouteOverride->Local) kept.push_back(override_);
      if (!override_.OverrideId || !override_.Hosts || override_.Hosts->empty()) continue;
      quick_action::HostRule rule;
      rule.overrideId = *override_.OverrideId;
      rule.hosts = *override_.Hosts;
      rule.hasBlockOverride = override_.BlockOverride.has_value();
      rule.block = override_.BlockOverride && override_.BlockOverride->Block;
      rule.hasRouteOverride = override_.RouteOverride.has_value();
      rule.routeLocal = override_.RouteOverride && override_.RouteOverride->Local;
      hostRules.push_back(std::move(rule));
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
  const uint64_t hostSig = HostRulesSig(hostRules);
  if (force || hostSig != hostRulesSig_) {
    hostRulesSig_ = hostSig;
    hostRules_ = std::move(hostRules);
    if (!selectedConnectionId_.empty()) ApplyInspector();
  }
}

// The clock-driven fallback for the change feed.
//
// SdkHost::SetDrawerEventHandler is a SINGLE slot and MainWindow owns it; it
// now fans out to both surfaces (MainWindow.cpp: `if (windowVisible_ &&
// connectPage_) connectPage_->OnHostEvent(event)`), so OnHostEvent below is
// reached. It is gated on the window being visible, though, and every push
// that lands while the window is hidden is DROPPED — so the clock-driven
// re-read stays as the safety net that closes that gap on re-show, applying
// only what actually changed (each read is fingerprinted, so an idle session
// rebuilds nothing). Once a real event has landed the poll steps back to 5 s.
void ConnectPage::PollFeeds() { RefreshFeeds(false); }

bool ConnectPage::ReadLocations(bool force) {
  // The provider row shows the selection, which a Disconnect leaves in place;
  // the dns pill's regional recommendation follows the connected country.
  auto selected = host_.SelectedLocation();
  auto connected = host_.ConnectedLocation();
  const uint64_t sig = HashMix(LocationSig(selected), LocationSig(connected));
  if (!force && sig == locationSig_) return false;
  locationSig_ = sig;
  selectedLocation_ = std::move(selected);
  countryCode_ = connected && connected->country_code ? LowerCopy(*connected->country_code)
                                                      : std::string();
  countryName_ = connected ? connected->country.value_or(std::string()) : std::string();
  return true;
}

void ConnectPage::Resync() {
  ++(*epoch_);  // anything in flight against the old reading is stale
  // With connect commands outstanding (a re-show queues the controllers'
  // open behind them) every read below would wait on the lock they hold:
  // the reseed runs once the queue settles (OnCommandsSettled).
  if (host_.CommandsBusy()) {
    resyncOwed_ = true;
    return;
  }
  resyncOwed_ = false;
  SyncProvideControlMode();
  RefreshAllPanes();
  BeginPlaceholders();
  // Seed the exit tables on entry rather than waiting up to a full 5 s tick:
  // Resync is login / tab entry / window re-show, i.e. exactly the moments the
  // pane comes back on screen. The tables are deliberately NOT cleared here —
  // Resync is not a session change, and blanking them would flash the
  // inspector's UNKNOWN reading through on every window re-show.
  if (advanced_) RefreshExitRouting();
}

// The drawer's dispatcher, per group: re-read through the accessor, re-apply
// ONE surface, cascade to an OPEN sheet only.
void ConnectPage::OnHostEvent(DrawerEvent event) {
  // a real push arrived: the clock-driven poll steps back to a safety net
  eventsWired_ = true;
  switch (event) {
    case DrawerEvent::DeviceLifecycle:
      // A new device is a new routing table. The old one must not be joined
      // against — the ids in it belonged to a device that no longer exists, so
      // a hit would be a plausible WRONG answer — and it is UNKNOWN until a
      // fresh read lands, not empty.
      exits_.reset();
      destinationExits_.reset();
      RefreshAllPanes();
      // the device's arrival (or departure) IS the dns reading: what the
      // accessor returns now is the answer, so the skeleton rows settle on it.
      // The transport bar settles on its first real distribution, pulled here
      // rather than on the next throughput tick so the footer does not trail
      // the rows by half a second.
      if (!dnsSettled_) {
        dnsSettled_ = true;
        ApplyDnsCard();
      }
      if (transportBar_) transportBar_->SetDistribution(host_.ClientTransportDistribution());
      if (advanced_) RefreshExitRouting();
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
      ApplyVerdictRatioBar();  // two of the bar's three inputs
      if (splitRulesSheet_ && splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
      break;
    case DrawerEvent::Overrides:
      ApplyOverrides(host_.BlockActionOverrides(), /*force=*/true);
      break;
    case DrawerEvent::DnsSettings:
      dnsSettled_ = true;
      dnsSettings_ = host_.GetDnsResolverSettings();
      ApplyDnsCard();
      break;
    case DrawerEvent::TransportSettings:
      // the enabled flags behind the bar's unused footer follow the policy;
      // re-read the distribution so the footer does not wait for a tick
      if (transportBar_) transportBar_->SetDistribution(host_.ClientTransportDistribution());
      break;
    case DrawerEvent::ProviderTransportSettings:
      // no provider stats surface on this page (the provider bar would live
      // under a provider Local chart)
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
      ReadLocations(/*force=*/true);
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
    case DrawerEvent::ExtenderStatus:
      // the SDK coalesces this to one callback per second; the panel drops a
      // push that changes nothing, so this costs a read and a compare
      if (extenderPanel_) extenderPanel_->SetStatus(host_.GetExtenderStatus());
      break;
    case DrawerEvent::ExtenderProvideStatus:
      ApplyExtenderProvideState();
      break;
    case DrawerEvent::ProviderStatus:
      // the provider status is the earnings page's
      break;
  }
}

// ---- the reused detail sheets ---------------------------------------------------

Gtk::Window* ConnectPage::RootWindow() {
  return dynamic_cast<Gtk::Window*>(get_root());
}

void ConnectPage::OpenProviderLocations() {
  // the globe has nothing to plot without a session; while connecting the
  // sheet lists the providers known so far
  if (!ConnectedNow() && !ConnectingNow()) return;
  if (on_open_provider_locations) on_open_provider_locations();
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
  if (!dnsSheet_->Open()) {
    if (dnsEditButton_) dnsEditButton_->set_sensitive(false);
    if (foldDoorDns_) foldDoorDns_->set_sensitive(false);
  }
}

void ConnectPage::OpenTransportSheet() {
  auto* parent = RootWindow();
  if (!parent) return;
  if (!transportSheet_) {
    transportSheet_ =
        std::make_unique<TransportSheet>(*parent, host_, TransportSheet::Kind::Client);
  }
  // always presentable: with no device the draft comes from the GUI's mirror
  // or the SDK default, and the edit is applied at the next tunnel start
  transportSheet_->Open();
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

  // Simple is always one pane, capped and centred; Advanced folds on width,
  // and the third pane waits until activity keeps 330dip beside the rails.
  const int panes = connect_fold::PaneCount(advanced_, paneWidth);
  const bool three = panes == 3;
  const bool two = panes >= 2;
  paneB_.root->set_visible(two);
  paneBRule_->set_visible(two);
  paneC_.root->set_visible(three);
  paneCRule_->set_visible(three);
  // exactly one set of doors to pane C's sheets: its own, or pane A's
  if (foldDoorsHost_) foldDoorsHost_->set_visible(connect_fold::FoldDoorsShown(panes));
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
    if (transportSheet_) transportSheet_->hide();
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
  // The disconnect intent is the one piece of page state that changes with the
  // CLOCK rather than with a push: nothing from the SDK arrives to announce
  // that a teardown has taken too long. Without this the ceiling would only be
  // applied on the next unrelated event, and a wedged teardown could hold
  // "Disconnecting…" and a dead button on screen indefinitely. Re-rendered only
  // while an intent is actually outstanding, so an idle page costs nothing.
  if (disconnectRequestedAtUs_ != 0) ApplyConnectStatus();
  // the loading skeletons' ceiling is clock-driven for the same reason
  if (placeholdersSinceUs_ != 0 &&
      g_get_monotonic_time() - placeholdersSinceUs_ > kPlaceholderCeilingUs) {
    SettlePlaceholders();
  }
  // the charts ride the throughput feed; at 2fps the 60s window still reads
  // live and the read stays off the per-frame path
  if (tickCount_ % 5 == 0) PullThroughput();
  // every 10th tick (~1 s): re-read the feeds the page has no event path for
  // (see PollFeeds), re-run the open split-rules sheet and age the activity
  // rows' meta lines.
  if (tickCount_ % 10 == 0) {
    // With events reaching OnHostEvent this is already the 5 s safety net for
    // the pushes dropped while the window was hidden. A changed feed cascades
    // into an open split-rules sheet from there (as the event path does), so
    // the sheet is never re-read for nothing.
    //
    // TODO(sheet): §4.5 wants the sheet's "Ns ago" captions aged on the CLOCK.
    // SplitRulesSheet has no RefreshTimes() entry point and its Refresh()
    // rebuilds only the sections whose VALUES changed, so on an idle session
    // those captions keep the age they had when the sheet was opened (only
    // Open() forces a rebuild). Fixing it needs that entry point on the sheet.
    if (!eventsWired_ || tickCount_ % 50 == 0) PollFeeds();
    // the activity rows' age, on screen only (the rows outlive a fold)
    if (paneB_.root && paneB_.root->get_visible()) RefreshConnectionRowTimes();
    // and every 5th of those (5 s), Advanced only: the exit-routing cache
    if (tickCount_ % 50 == 0 && advanced_) RefreshExitRouting();
  }
}

}  // namespace urnw
