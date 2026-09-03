// SPDX-License-Identifier: MPL-2.0
#include "DeveloperPage.hpp"

#include <adwaita.h>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

// The version stamp on the intro card: meson passes -DUR_APP_VERSION into the
// binaries (see meson.build); the fallback keeps this TU self-contained, the
// same idiom SdkHost.cpp uses. Transcripts pasted from this screen carry it.
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

namespace urnw {
namespace {

// ---- cadence + geometry (spec §2.13, §2.2, §2.7, §2.8, §2.9) ---------------
constexpr int kPollIntervalMs = 5000;  // iOS ReliabilityStore.pollInterval parity
constexpr int kWideCapDip = 1800;
constexpr int kNarrowCapDip = 1000;
// Six star columns 2/2/1/1/2/2 plus a FIXED trailing actions cell. The fixed
// width comes off the top before the stars divide, so the four row buttons
// keep their width at 1100 dip AND maximized.
const std::vector<int> kExitColumns{2, 2, 1, 1, 2, 2};
constexpr int kExitActionsWidthPx = 270;
const std::vector<int> kDestinationColumns{3, 2, 1};
const std::vector<int> kProbeColumns{3, 1, 1, 1, 1, 1, 2};
constexpr int kTableRowHeight = 36;
constexpr int kExitRowHeight = 48;  // four buttons ride this row

// ---- the session log card ---------------------------------------------------
// time | source | text. ALL THREE read left as text (textColumns = 3): a log
// line is prose, and the kit's default of one text column would right-align the
// source and the message against the figure rule.
const std::vector<int> kLogColumns{2, 1, 9};
constexpr int kLogRowHeight = 20;  // a dense tail, not a table of records
// How many rows exist as widgets. The client buffer holds ctl::kLogTailClientCap
// (2000) — Copy and Save carry all of it — but 2000 live GTK rows is a cost
// nobody is looking at: the tail is what you read.
constexpr size_t kLogRenderCap = 500;
constexpr int kLogScrollerMinHeight = 200;
constexpr int kLogScrollerMaxHeight = 420;

// ---- the localization convention (spec §2, "Dev(key, english)") -----------
// NONE of the dev_* keys are in the store today: the English literals ARE the
// shipped strings, and T_ renders them until the store grows the key. Two
// derived key shapes keep the table below readable and the extraction greppable
// (`grep -oE '"dev_[a-z0-9_]+"' DeveloperPage.cpp | sort -u`):
//   <key>_detail — the 11px explanation under a row's title
//   <key>_value  — a metric row's composed value format
// Both carry their English here exactly as the spec's tables spell it.
std::string DetailKey(const char* key) { return std::string(key) + "_detail"; }

// ---- the override rows (spec §2.10) ----------------------------------------
// Section order in code is Detection, Placement, Recovery, Probing,
// Observability — Observability LAST, carrying the reset button (doc §7.13
// disagrees; code wins). `order` is the row's position WITHIN its section:
// bool and number rows interleave, so the two tables are merged by it at build
// time. 12 toggles + 22 number boxes = 34 rows.
enum class Section { Detection, Placement, Recovery, Probing, Observability };

struct BoolSpec {
  Section section;
  int order;
  const char* key;
  const char* label;
  const char* detail;
  bool urnet::ReliabilitySettings::*field;
};

struct NumSpec {
  Section section;
  int order;
  const char* key;
  const char* label;
  const char* detail;
  bool millis;          // duration formatting for the effective label
  const char* zeroKey;  // nullptr => 0 renders through the normal formatter
  const char* zeroLabel;
  int64_t urnet::ReliabilitySettings::*i64;  // exactly one of i64/i32 is set
  int32_t urnet::ReliabilitySettings::*i32;
};

const BoolSpec kBoolSpecs[] = {
    // Detection — how an exit is judged to be failing, and how fast
    {Section::Detection, 1, "dev_probe_stalled_exits",
     "Probe stalled exits before dropping",
     "When an exit stalls, ping it once before convicting. A congested but alive exit "
     "answers and keeps its flows; a dead one is still dropped",
     &urnet::ReliabilitySettings::BusyProbe},
    {Section::Detection, 7, "dev_demote_before_removing", "Demote before removing",
     "Ambiguous verdicts bench an exit instead of tearing down its flows; removal needs "
     "sustained evidence or an empty exit",
     &urnet::ReliabilitySettings::SoftVerdictDemote},
    // Placement — which exit a flow lands on, and how the pool is shaped
    {Section::Placement, 0, "dev_live_tier_demotion", "Live tier demotion",
     "Failing dials and survived verdicts push a provider down the ranking within a "
     "second; promotion back needs clean minutes and a proven connect",
     &urnet::ReliabilitySettings::EffectiveTierSelection},
    {Section::Placement, 2, "dev_sticky_site_affinity", "Sticky site affinity",
     "A site's new connections stay on the exit its earlier ones already use, even past "
     "the flow cap, so a busy site keeps one egress IP",
     &urnet::ReliabilitySettings::AffinityStickyPastCap},
    {Section::Placement, 3, "dev_follow_benched_exits", "Follow benched exits",
     "A quarantined exit keeps its own sites' new connections through the early bench, "
     "when the verdict is least proven. New sites still avoid it",
     &urnet::ReliabilitySettings::QuarantineGroupFollow},
    {Section::Placement, 7, "dev_keep_a_spare_exit_warm", "Keep a spare exit warm",
     "Size each window one exit beyond its target so a replacement is already connected. "
     "Off waits until a loss to backfill",
     &urnet::ReliabilitySettings::StandingReserve},
    {Section::Placement, 10, "dev_group_ips_by_site", "Group IPs by site",
     "Keeps a site on one exit when its hostname is not visible",
     &urnet::ReliabilitySettings::ClusterAffinityFallback},
    {Section::Placement, 11, "dev_converge_late_named_flows", "Converge late-named flows",
     "Moves later connections onto the exit the first one already uses",
     &urnet::ReliabilitySettings::ServerNameAffinityBridge},
    // Recovery — getting a flow moving again after its exit fails
    {Section::Recovery, 0, "dev_rebind_quic_on_exit_loss", "Rebind QUIC on exit loss",
     "Re-pin established QUIC flows to a live exit inside the removal instead of tearing "
     "them down",
     &urnet::ReliabilitySettings::QuicRebindOnExitLoss},
    {Section::Recovery, 1, "dev_retry_refused_connects_elsewhere",
     "Retry refused connects elsewhere",
     "When a provider can't reach a site, move the connection to another exit instead of "
     "letting it hang",
     &urnet::ReliabilitySettings::DialFailureRerace},
    {Section::Recovery, 2, "dev_signal_udp_teardown", "Signal UDP teardown",
     "Tells DNS and QUIC the path is gone instead of going silent",
     &urnet::ReliabilitySettings::UdpTeardownSignal},
    // Probing — proving an exit can actually reach real destinations
    {Section::Probing, 0, "dev_probe_providers", "Probe providers",
     "Qualify exits by dialing real sites through them. An answer proves the exit; "
     "silence never counts against it",
     &urnet::ReliabilitySettings::ProviderProbe},
};

const NumSpec kNumSpecs[] = {
    // Detection
    {Section::Detection, 0, "dev_drop_stalled_exits_fast", "Drop stalled exits fast",
     "How long an exit may stop delivering before it is dropped, in ms. Off waits 30s",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::SendStallTimeoutMillis, nullptr},
    {Section::Detection, 2, "dev_busy_probe_wait", "Busy probe wait",
     "How long the stall probe waits for an answer before convicting, in ms. Off derives "
     "half the stall bar",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::BusyProbeBudgetMillis, nullptr},
    {Section::Detection, 3, "dev_suspend_detector", "Suspend detector",
     "How much timer overshoot reads as the machine being suspended rather than an exit "
     "stalling, in ms, so a resumed machine does not convict every exit at once. Off "
     "disables it",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::SchedulerPauseToleranceMillis,
     nullptr},
    {Section::Detection, 4, "dev_suspend_recovery_window", "Suspend recovery window",
     "How long verdicts stay held after a detected suspend, in ms, giving transports time "
     "to re-register. Off uses the built-in 5s",
     true, nullptr, nullptr,
     &urnet::ReliabilitySettings::SchedulerPauseRecoveryTimeoutMillis, nullptr},
    {Section::Detection, 5, "dev_cut_dead_connects_early", "Cut dead connects early",
     "Drop an exit that has established nothing sooner when two sibling exits are "
     "receiving, in ms. Off waits the full 30s connect bar",
     true, nullptr, nullptr,
     &urnet::ReliabilitySettings::BlackholeConnectComparativeTimeoutMillis, nullptr},
    {Section::Detection, 6, "dev_keep_quiet_providers_longer", "Keep quiet providers longer",
     "How long a provider still acknowledging traffic may return nothing before it is "
     "dropped, in ms. Off keeps them until they stop acknowledging",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::BlackholeReceiveTimeoutMillis,
     nullptr},
    // Placement
    {Section::Placement, 1, "dev_max_connections_per_exit", "Max connections per exit",
     "Losing an exit kills every connection on it. Lower spreads the damage; a site may "
     "then use more than one exit IP",
     false, "dev_unlimited", "Unlimited", nullptr,
     &urnet::ReliabilitySettings::MaxFlowsPerExit},
    {Section::Placement, 4, "dev_follow_window", "Follow window",
     "How long into a bench a site's new connections keep following their exit, in ms. "
     "Off scatters immediately",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::GroupFollowWindowMillis, nullptr},
    {Section::Placement, 5, "dev_removal_storm_limit", "Removal storm limit",
     "How many verdict removals are allowed per window before the rest are deferred; a "
     "burst is more likely one local cause than many failures",
     false, "off", "Off", nullptr, &urnet::ReliabilitySettings::RemovalBudgetCount},
    {Section::Placement, 6, "dev_removal_storm_window", "Removal storm window",
     "The window the removal limit is counted over, in ms. Off (like a limit of 0) turns "
     "the breaker off",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::RemovalBudgetWindowMillis, nullptr},
    {Section::Placement, 8, "dev_load_corroboration", "Load corroboration",
     "Extra silent destinations required per this many flows before a busy exit can be "
     "benched on soft evidence. Off keeps the flat minimum",
     false, "off", "Off", nullptr,
     &urnet::ReliabilitySettings::BlackholeLoadCorroboration},
    {Section::Placement, 9, "dev_corroborate_silent_exits", "Corroborate silent exits",
     "How many distinct destinations must be silent before an exit is convicted on "
     "no-receive, so one dead site cannot remove a working exit",
     false, "off", "Off", nullptr, &urnet::ReliabilitySettings::MinBlackholeDestinations},
    // Recovery
    {Section::Recovery, 3, "dev_release_stuck_retransmits", "Release stuck retransmits",
     "How long retransmits are held before one is released, in ms. Off waits 30s", true,
     nullptr, nullptr, &urnet::ReliabilitySettings::TcpCollapseMaxHoldMillis, nullptr},
    {Section::Recovery, 4, "dev_longer_tcp_idle_timeout", "Longer TCP idle timeout",
     "How long a TCP connection may sit idle, in ms. Off uses the UDP bound", true, nullptr,
     nullptr, &urnet::ReliabilitySettings::TcpSequenceIdleTimeoutMillis, nullptr},
    {Section::Recovery, 5, "dev_udp_idle_timeout", "UDP idle timeout",
     "How long a non-TCP flow may sit idle before it is reaped, in ms", true, nullptr,
     nullptr, &urnet::ReliabilitySettings::SequenceIdleTimeoutMillis, nullptr},
    {Section::Recovery, 6, "dev_uplink_silence_gate", "Uplink silence gate",
     "How long the whole tunnel may be silent before provider verdicts are held as "
     "inadmissible, in ms. 0 convicts as before",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::UplinkStalenessGateMillis, nullptr},
    {Section::Recovery, 7, "dev_fast_first_exit_poll", "Fast first-exit poll",
     "How often a connecting flow re-checks an empty window, in ms, so the first request "
     "leaves right after the first exit lands. Off waits the 2s retry pace",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::FormationPollTimeoutMillis,
     nullptr},
    // Probing
    {Section::Probing, 1, "dev_probe_wait", "Probe wait",
     "How long a qualification probe waits for an answer, in ms. Off uses the built-in 4s. "
     "It only bounds waiting for proof, it never convicts",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::ProbeTimeoutMillis, nullptr},
    {Section::Probing, 2, "dev_probe_hosts_per_pass", "Probe hosts per pass",
     "How many health sites one qualification pass dials through an exit. 0 probes the "
     "entire embedded list; a smaller number rotates through it in blocks",
     false, "dev_all", "All", nullptr, &urnet::ReliabilitySettings::ProbeSampleHostCount},
    {Section::Probing, 3, "dev_probe_silence_streak", "Probe silence streak",
     "How many consecutive probe passes an exit may answer with total silence before it is "
     "warned out of new-flow placement. Placement only",
     false, "off", "Off", nullptr, &urnet::ReliabilitySettings::ProbeSilenceWarnStreak},
    {Section::Probing, 4, "dev_candidates_per_slot", "Candidates evaluated per slot",
     "How many providers a window expansion pings and ranks per slot it needs, keeping the "
     "best. 1 evaluates exactly what it needs",
     false, "dev_one_min", "1 (min)", nullptr,
     &urnet::ReliabilitySettings::EvaluationPoolMultiple},
    // Observability — what the session writes to the log
    {Section::Observability, 0, "dev_state_heartbeat", "State heartbeat",
     "How often one line summarizing live state is written to the log for later forensics, "
     "in ms. Off silences it",
     true, nullptr, nullptr, &urnet::ReliabilitySettings::HeartbeatIntervalMillis, nullptr},
};

constexpr size_t kBoolSpecCount = sizeof(kBoolSpecs) / sizeof(kBoolSpecs[0]);
constexpr size_t kNumSpecCount = sizeof(kNumSpecs) / sizeof(kNumSpecs[0]);
// The spec's inventory, pinned: 12 toggles + 22 number boxes = 34 override
// rows. ReliabilitySettings also carries SharedFateMinExits,
// SharedFateWindowMillis, ScoredPlacement, PlacementHysteresisPct,
// PlacementDemoteConsecutive, RewardInstrumentation and QuarantineDampening,
// which this UI deliberately does NOT expose — the whole-struct
// read-modify-write round-trips them untouched.
static_assert(kBoolSpecCount == 12, "12 toggles (spec §2.10)");
static_assert(kNumSpecCount == 22, "22 number boxes (spec §2.10)");

// ---- the 13 measurement rows (spec §2.6) -----------------------------------
// Row order IS the apply-index order in ApplyMetrics — never reorder one side
// without the other.
struct MetricSpec {
  const char* key;
  const char* label;
  const char* detail;
};

const MetricSpec kMetricSpecs[] = {
    {"dev_flows_opened", "Flows opened",
     "Total since reset, so runs of different lengths compare"},
    {"dev_provider_connect_failures", "Provider connect failures",
     "Times a provider reported it could not open the upstream connection"},
    {"dev_moved_to_another_exit", "Moved to another exit",
     "How many of those failures were quietly moved instead of hanging"},
    {"dev_probes", "Probes", "Qualification probes this session"},
    {"dev_busy_probes", "Busy probes",
     "Liveness pings fired at stalled exits; acquitted ones answered and were kept"},
    {"dev_verdicts_held", "Verdicts held",
     "Convictions withheld because this machine's own uplink, not the provider, was silent "
     "(uplink / transport)"},
    {"dev_removals_deferred", "Removals deferred",
     "Removals the storm breaker held back after a correlated burst"},
    {"dev_suspends_caught", "Suspends caught",
     "Host suspends the detector caught, each one a batch of verdicts held instead of "
     "executed on a just-resumed machine"},
    {"dev_quic_flows_rebound", "QUIC flows rebound",
     "Flows moved to a warm exit inside a removal; accepted means the server took the path "
     "change without a re-dial"},
    {"dev_blast_radius", "Blast radius",
     "Connections lost per provider failure. Lower is better"},
    {"dev_worst_single_failure", "Worst single failure",
     "The one the user actually feels"},
    {"dev_recovery_time", "Recovery time",
     "From an exit dying to that site answering again"},
    {"dev_never_came_back", "Never came back", "Sites abandoned rather than recovered"},
};

constexpr size_t kMetricCount = sizeof(kMetricSpecs) / sizeof(kMetricSpecs[0]);
// The row order IS the apply-index order in ApplyMetrics's switch.
static_assert(kMetricCount == 13, "13 metric rows (spec §2.6)");

// ---- formatting (spec §2.3) -------------------------------------------------

std::string FormatOneDecimal(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f", value);
  return buffer;
}

// 0 -> "Off"; < 1s -> "{n}ms"; < 1m -> seconds (one decimal unless whole);
// otherwise integer minutes.
std::string FormatDurationMillis(int64_t ms) {
  if (ms == 0) return T_("off", "Off");
  if (ms < 1000) return std::to_string(ms) + "ms";
  if (ms < 60000) {
    if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
    return FormatOneDecimal(static_cast<double>(ms) / 1000.0) + "s";
  }
  return std::to_string(ms / 60000) + "m";
}

// Exit and destination client ids are ULIDs sharing their leading time bytes —
// render the last 8 characters (iOS parity).
std::string ShortId(const std::string& id) {
  if (id.size() <= 8) return id;
  return id.substr(id.size() - 8);
}

// ---- text species -----------------------------------------------------------
// The theme carries no 11px-faint / 13px-muted class, and the kit's cell
// classes win the cascade over .dim-label / .ur-danger-text (both sit in the
// brand sheet, which loads at a higher provider priority than the drawer
// sheet). So size and colour ride a pango span — an attribute beats a class —
// while the body face still comes from .ur-caption.
// A null colour spans the SIZE only and leaves the foreground to the theme —
// which is how the spec's #F8F8F8 body text is reached (the palette's kUrText
// is pure white, and body text is deliberately never pure white).
std::string Span(const Glib::ustring& text, int sizePx, const Rgba* color) {
  std::string out = "<span size='" + std::to_string(sizePx * PANGO_SCALE) + "'";
  if (color != nullptr) out += " foreground='" + HexForMarkup(*color) + "'";
  out += ">" + Glib::Markup::escape_text(text).raw() + "</span>";
  return out;
}

Gtk::Label* MakeTextLine(const Glib::ustring& text, int sizePx, const Rgba* color) {
  auto* line = Gtk::make_managed<Gtk::Label>();
  line->add_css_class("ur-caption");  // the body face; size/colour from the span
  line->set_xalign(0);
  line->set_wrap(true);
  line->set_markup(Span(text, sizePx, color));
  return line;
}

// Text AND visibility in one call (the kit's rule: a box spends its spacing on
// a child that drew nothing), then the size/colour span restored on top.
void SetLineOrCollapse(Gtk::Label* line, const Glib::ustring& text, int sizePx,
                       const Rgba* color) {
  if (!line) return;
  kit::SetTextOrCollapse(*line, text);
  if (!text.empty()) line->set_markup(Span(text, sizePx, color));
}

// ---- table cells ------------------------------------------------------------
// Prepared once per rebuild: a mono cell drops the kit's body-face class (the
// brand sheet outranks the drawer's .ur-mono-*), everything else keeps it.
void PrepCell(Gtk::Label& cell, const char* monoClass) {
  if (monoClass == nullptr) return;
  cell.remove_css_class("ur-row-title");
  cell.remove_css_class("ur-value");
  cell.add_css_class(monoClass);
}

void WriteCell(Gtk::Label& cell, const Glib::ustring& text, int sizePx, const Rgba* color) {
  if (text.empty()) {
    cell.set_text("");
    return;
  }
  cell.set_markup(Span(text, sizePx, color));
}

// ---- action buttons (spec §2.3 MakeActionButton) ---------------------------
// primary = the pale-yellow accent pill (windows AccentButtonStyle, here
// .suggested-action); otherwise a flat card-row button wearing the accent as
// its FOREGROUND. The 48px URButton pills are the sign-in CTA role — never
// used here.
Gtk::Button* MakeActionButton(const Glib::ustring& text, bool primary) {
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->set_halign(Gtk::Align::START);
  button->set_valign(Gtk::Align::CENTER);
  if (primary) {
    button->set_label(text);
    button->add_css_class("suggested-action");
  } else {
    button->add_css_class("flat");
    auto* label = Gtk::make_managed<Gtk::Label>();
    label->set_markup(Span(text, 13, &kUrAccent));
    button->set_child(*label);
  }
  kit::SetAccessibleLabel(*button, text);
  return button;
}

// The destructive twin: fault injection is destructive and is coloured as such
// (danger #F8523B), with NO confirm modal — Advanced Mode's audience acts
// deliberately, and the report line plus the log naming the exit are the record.
Gtk::Button* MakeDangerButton(const Glib::ustring& text) {
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->set_valign(Gtk::Align::CENTER);
  button->add_css_class("destructive-action");
  button->add_css_class("flat");
  button->set_label(text);
  return button;
}

// ---- the card-body setting row (windows MakeSettingRow) --------------------
// Title + a WRAPPING 11px faint explanation + a trailing control slot. The kit
// has no card-body row species: MakePaneTwoLineRow trims its note to one line
// (this screen's explanations ARE its documentation and must wrap) and
// MakeSettingsCard brings its own card, which inside a section card would read
// as two edges 16px apart. This is the windows row, built from kit typography.
struct SettingRow {
  Gtk::Box* root = nullptr;
  Gtk::Label* title = nullptr;
  Gtk::Box* trailing = nullptr;
};

SettingRow MakeSettingRow(const Glib::ustring& label, const Glib::ustring& detail) {
  SettingRow row;
  row.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  text->set_hexpand(true);
  text->set_margin_end(16);
  text->set_valign(Gtk::Align::CENTER);
  row.title = Gtk::make_managed<Gtk::Label>(label);
  row.title->add_css_class("ur-body");
  row.title->set_xalign(0);
  row.title->set_wrap(true);
  text->append(*row.title);
  if (!detail.empty()) text->append(*MakeTextLine(detail, 11, &kUrTextFaint));
  row.root->append(*text);
  row.trailing = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  row.trailing->set_halign(Gtk::Align::END);
  row.trailing->set_valign(Gtk::Align::CENTER);
  row.root->append(*row.trailing);
  return row;
}

// MakePaneRow/MakePaneTableRow return a bordered host; content lives in its
// inset first child.
Gtk::Box* RowInner(Gtk::Widget* host) {
  auto* box = dynamic_cast<Gtk::Box*>(host);
  return box ? dynamic_cast<Gtk::Box*>(box->get_first_child()) : nullptr;
}

// RAII echo guard: ~46 property writes sit between set and clear, and an
// exception must not leave the flag stuck — a stuck flag makes the whole
// surface silently read-only.
struct ScopedFlag {
  explicit ScopedFlag(bool& flag) : flag_(flag) { flag_ = true; }
  ~ScopedFlag() { flag_ = false; }
  bool& flag_;
};

struct ScopedAtomicFlag {
  explicit ScopedAtomicFlag(std::atomic<bool>& flag) : flag_(flag) {}
  ~ScopedAtomicFlag() { flag_.store(false); }
  std::atomic<bool>& flag_;
};

// One rpc, guarded: a throwing getter costs its own field, not the snapshot.
template <typename T, typename Fn>
T ReadGuarded(const char* what, Fn&& fn, T fallback) {
  try {
    return fn();
  } catch (const std::exception& e) {
    g_warning("developer: %s threw: %s", what, e.what());
  } catch (...) {
    g_warning("developer: %s threw", what);
  }
  return fallback;
}

// ---- session-log line rendering --------------------------------------------

// Wall clock, local, seconds resolution — enough to line a log line up against
// a `journalctl -f` beside it, which is the whole point of showing a time.
std::string FormatClockTime(int64_t unixMs) {
  static const char* kUnknown = "--:--:--";
  if (unixMs <= 0) return kUnknown;  // the daemon's clock failed: say so, never guess
  GDateTime* moment = g_date_time_new_from_unix_local(unixMs / 1000);
  if (moment == nullptr) return kUnknown;
  gchar* formatted = g_date_time_format(moment, "%H:%M:%S");
  std::string out = formatted != nullptr ? formatted : kUnknown;
  g_free(formatted);
  g_date_time_unref(moment);
  return out;
}

std::string FormatStampIso(int64_t unixMs) {
  if (unixMs <= 0) return "(no timestamp)";
  GDateTime* moment = g_date_time_new_from_unix_local(unixMs / 1000);
  if (moment == nullptr) return "(no timestamp)";
  gchar* formatted = g_date_time_format(moment, "%Y-%m-%d %H:%M:%S");
  std::string out = formatted != nullptr ? formatted : "(no timestamp)";
  g_free(formatted);
  g_date_time_unref(moment);
  return out;
}

// The reliability stream: `[rel] event=<name> key=value` plus the session
// banner the Windows README points at ("read the session banner in the log").
// Matching the "[rel]" tag rather than the full "[rel] event=" prefix on
// purpose — the banner and the event lines share the tag, and this is the
// stream the Advanced-Mode knobs are supposed to prove themselves in.
bool LineIsReliability(const std::string& text) {
  return text.find("[rel]") != std::string::npos;
}

// A DISPLAY HINT ONLY — never a verdict, and never used to hide anything. The
// daemon's own breadcrumbs carry no severity field, so failure-shaped lines are
// coloured by their words. A miss costs a colour, not a line.
bool LineIsFailure(const std::string& text) {
  static const char* kMarkers[] = {"failed", "error", "could not", "cannot",
                                   "rejecting", "no credentials"};
  std::string lowered;
  lowered.reserve(text.size());
  for (const char c : text) {
    lowered.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  for (const char* marker : kMarkers) {
    if (lowered.find(marker) != std::string::npos) return true;
  }
  return false;
}

// The outcome of one action, carried from the bridge thread to the UI thread.
struct ActionOutcome {
  bool issued = false;    // false: no device, or the rpc threw
  bool declined = false;  // the SDK's negative not-found sentinel
  bool hasCount = false;  // MigrateExit / ProbeAllExits answer with a count
  int64_t count = 0;      // 0 is a REAL answer ("nothing needed moving")
};

}  // namespace

// ---- the serial FIFO bridge (spec §2.14) -----------------------------------

DeveloperPage::Bridge::Bridge() {
  thread_ = std::thread([this] { Run(); });
}

DeveloperPage::Bridge::~Bridge() { Shutdown(); }

void DeveloperPage::Bridge::Submit(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return;  // after Shutdown nothing may enter SdkHost again
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void DeveloperPage::Bridge::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return;  // idempotent
    stop_ = true;
    jobs_.clear();  // queued work is dropped, not run against a dying page
  }
  cv_.notify_all();
  // The join is NOT bounded by one rpc (a job can block on SdkHost's lock held
  // across bootstrap), so a quit against a hung service can take seconds —
  // still the right trade against use-after-free. In practice the bridge is
  // idle at quit because polling stops when the page is not presenting.
  if (thread_.joinable()) thread_.join();
}

void DeveloperPage::Bridge::Run() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    // Nothing may escape a job: an uncaught exception on this thread is
    // std::terminate.
    try {
      job();
    } catch (const std::exception& e) {
      g_warning("developer: bridge job threw: %s", e.what());
    } catch (...) {
      g_warning("developer: bridge job threw");
    }
  }
}

// ---- construction -----------------------------------------------------------

DeveloperPage::DeveloperPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 0), host_(host) {
  EnsureBrandCss();   // .ur-body / .ur-caption / .ur-col-header / the pane rows
  EnsureDrawerCss();  // .ur-card / .ur-mono-* / suggested-action
  set_hexpand(true);
  set_vexpand(true);

  scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller_.set_hexpand(true);
  scroller_.set_vexpand(true);

  // The windows cap grid is * : 1000* : * with MaxWidth on the middle column;
  // AdwClamp is the GTK equivalent (never a width request on the panel itself).
  clampWidget_ = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clampWidget_), kNarrowCapDip);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clampWidget_), kNarrowCapDip);
  gtk_widget_set_hexpand(clampWidget_, TRUE);
  adw_clamp_set_child(ADW_CLAMP(clampWidget_), GTK_WIDGET(column_.gobj()));
  column_.set_margin(24);  // the card-page gutter

  // Row 0: the intro card, full composition width. Row 1: the three tables,
  // also full width at every size (a 7-column table in half a column is a
  // column of ellipses). Row 2: measurements | overrides.
  column_.append(topStack_);
  tablesStack_.set_margin_top(16);
  column_.append(tablesStack_);
  mainStack_.set_valign(Gtk::Align::START);
  mainStack_.set_hexpand(true);
  mainStack_.set_margin_top(16);
  mainStack_.set_margin_bottom(24);
  // wide: measurements and the overrides split the cap evenly (windows: star +
  // a 1* side column). A hidden child takes no share, so the same homogeneous
  // row is also the narrow single column.
  columnsRow_.set_homogeneous(true);
  columnsRow_.append(mainStack_);
  sideHostWide_.set_hexpand(true);
  sideHostWide_.set_visible(false);  // narrow by default; ApplyBreakpoint re-places
  columnsRow_.append(sideHostWide_);
  column_.append(columnsRow_);
  column_.append(narrowHost_);

  // default narrow placement (the code-applied margins, not the markup default)
  sideStack_.set_valign(Gtk::Align::START);
  sideStack_.set_hexpand(true);
  sideStack_.set_margin_top(16);
  sideStack_.set_margin_bottom(24);
  narrowHost_.append(sideStack_);

  scroller_.set_child(*Glib::wrap(clampWidget_));
  append(scroller_);
}

DeveloperPage::~DeveloperPage() {
  // Order matters: orphan every marshaled completion FIRST (they check alive_
  // and the epoch before touching a widget), then stop the timer, then stop,
  // clear and JOIN the bridge — after this returns no job can be inside
  // SdkHost or inside a half-destroyed page.
  *alive_ = false;
  ++*epoch_;
  if (pollTimer_.connected()) pollTimer_.disconnect();
  bridge_.Shutdown();
}

// ---- lifecycle --------------------------------------------------------------

void DeveloperPage::Load() {
  ++*epoch_;  // drop every completion armed for the previous load
  EnsureBuilt();
  selected_ = true;
  ReconcilePoll();
  // Refresh/Load is never gated: it only re-reads, and it is how a user retries
  // after starting the service. If a poll is already in flight the coalescer
  // drops this one — its (now stale-epoch) result is discarded and the next
  // tick repaints within the interval.
  SubmitPoll();
}

void DeveloperPage::SetAdvancedMode(bool on) {
  if (advanced_ == on) return;
  advanced_ = on;
  ReconcilePoll();
}

void DeveloperPage::SetPresenting(bool presenting) {
  // Window visible AND not minimized. Focus loss deliberately does NOT stop the
  // poll — a developer watching this screen beside a terminal keeps their feed.
  if (presenting_ == presenting) return;
  presenting_ = presenting;
  ReconcilePoll();
}

void DeveloperPage::SetSelected(bool selected) {
  if (selected_ == selected) return;
  selected_ = selected;
  if (selected_) EnsureBuilt();  // ~40 controls: a user who never opens it pays nothing
  ReconcilePoll();
}

void DeveloperPage::ApplyBreakpoint(int widthDip) {
  const bool wide = widthDip >= static_cast<int>(kit::kWideBreakpointDip);
  if (wide_ == wide) return;  // no-op unless the boolean flips
  wide_ = wide;

  // One gate moves the cap and the overrides' position together: wide puts the
  // override sections BESIDE measurements ("change a threshold on the right,
  // watch a count on the left"); narrow stacks them below.
  adw_clamp_set_maximum_size(ADW_CLAMP(clampWidget_), wide ? kWideCapDip : kNarrowCapDip);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clampWidget_),
                                     wide ? kWideCapDip : kNarrowCapDip);

  // sideStack_ is a member widget, so the C++ wrapper keeps it alive across the
  // remove/append reparenting.
  if (auto* parent = dynamic_cast<Gtk::Box*>(sideStack_.get_parent())) {
    parent->remove(sideStack_);
  }
  sideStack_.set_margin_start(wide ? 20 : 0);
  sideStack_.set_margin_top(16);
  sideStack_.set_margin_end(0);
  sideStack_.set_margin_bottom(24);
  sideHostWide_.set_visible(wide);
  if (wide) {
    sideHostWide_.append(sideStack_);
  } else {
    narrowHost_.append(sideStack_);
  }
}

// ---- construction of the cards ---------------------------------------------

DeveloperPage::DevCard DeveloperPage::MakeDevCard(const Glib::ustring& heading) {
  DevCard card;
  card.root = MakeCard(12);
  if (!heading.empty()) {
    // windows UrCardLabelStyle: the display-adjacent face at 22 Bold. The theme
    // carries the face on .ur-wordmark (PP NeueBit Bold); the size is pinned
    // here rather than adding a hex or a class.
    auto* label = Gtk::make_managed<Gtk::Label>(heading);
    label->add_css_class("ur-wordmark");
    Pango::AttrList attrs;
    auto size = Pango::Attribute::create_attr_size_absolute(22 * PANGO_SCALE);
    attrs.insert(size);
    label->set_attributes(attrs);
    label->set_xalign(0);
    card.root->append(*label);
  }
  card.body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  card.root->append(*card.body);
  return card;
}

void DeveloperPage::EnsureBuilt() {
  if (built_) return;
  built_ = true;

  BuildIntroCard();
  BuildMeasurementsCard();
  BuildExitsCard();
  BuildDestinationsCard();
  BuildProbeSuiteCard();
  // Between the probe suite and the override sections, per the spec: it needs
  // the full-width tables lane, and it is what you scroll to AFTER acting on a
  // knob to see whether the knob did anything.
  BuildSessionLogCard();
  BuildOverrideSections();

  // Render the no-session state immediately: with no jwt and no device every
  // read below degrades, and the page must open in a real state rather than an
  // empty one. The first poll replaces this.
  ApplySnapshot(Snapshot{});
}

void DeveloperPage::BuildIntroCard() {
  DevCard card = MakeDevCard(T_("dev_developer", "Developer"));

  card.body->append(*MakeTextLine(
      T_("dev_intro",
         "Tools for diagnosing connection freezes. These act on the live connection."),
      13, &kUrTextMuted));
  card.body->append(*MakeTextLine(
      Glib::ustring(T_("dev_app_version", "App version")) + " " + UR_APP_VERSION, 11,
      &kUrTextFaint));

  // THREE distinguishable absent-states (§2.4): a diagnostic screen must not
  // collapse them. Visible iff no override is in force.
  connectHint_ = MakeTextLine({}, 14, nullptr);
  connectHint_->set_visible(false);
  card.body->append(*connectHint_);

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

  // Refresh — PRIMARY and never gated: it only re-reads, and it is how a user
  // retries after starting the service.
  auto* refresh = MakeActionButton(T_("dev_refresh", "Refresh"), true);
  refresh->signal_clicked().connect([this] { SubmitPoll(); });
  actions->append(*refresh);

  simulateBtn_ = MakeActionButton(
      T_("dev_simulate_network_change", "Simulate network change"), false);
  simulateBtn_->set_sensitive(false);  // enabled iff the snapshot has a device
  simulateBtn_->signal_clicked().connect([this] {
    RunAction(Action::SimulateNetworkChange,
              T_("dev_simulate_network_change", "Simulate network change"));
  });
  actions->append(*simulateBtn_);

  syncBtn_ = MakeActionButton(T_("dev_sync", "Sync"), false);
  syncBtn_->set_sensitive(false);
  syncBtn_->signal_clicked().connect(
      [this] { RunAction(Action::Sync, T_("dev_sync", "Sync")); });
  actions->append(*syncBtn_);

  // Check for updates — NEVER gated on windows (an unauthenticated HTTP GET
  // needs no session, device or service).
  checkUpdatesBtn_ = MakeActionButton(T_("dev_check_updates", "Check for updates"), false);
  checkUpdatesBtn_->signal_clicked().connect([this] {
    // TODO(sdk-wiring): urnw::pages::Updates().CheckNow() / Updates().Current()
    // — this tree carries no UpdateChecker at all, so there is nothing to run
    // and nothing to replay into ApplyUpdateCheck. Report the real outcome (the
    // check did not run) instead of a fabricated "no update"; the app log
    // carries the reason.
    g_warning("developer: update check unavailable (no UpdateChecker in this build)");
    SetLineOrCollapse(updateCheckText_,
                      T_("dev_update_check_failed",
                         "The update check failed — see the app log."),
                      12, &kUrTextMuted);
  });
  actions->append(*checkUpdatesBtn_);
  card.body->append(*actions);

  // The action report line and the update-check line: collapsed until they have
  // something to say.
  lastAction_ = MakeTextLine({}, 12, &kUrTextMuted);
  lastAction_->set_visible(false);
  card.body->append(*lastAction_);
  updateCheckText_ = MakeTextLine({}, 12, &kUrTextMuted);
  updateCheckText_->set_visible(false);
  card.body->append(*updateCheckText_);

  card.body->append(*MakeTextLine(
      T_("dev_actions_are_requests",
         "Actions are requests: confirm them in the measurements and exit rows, which "
         "refresh underneath."),
      11, &kUrTextFaint));

  topStack_.append(*card.root);

  // TODO(sdk-wiring): SdkHost::SetModeNoticeObserver / SdkHost::RefreshModeNotice
  // — the window-level session-mode notice (spec §2.15) is driven from this
  // page on windows (a SEPARATE observer slot from the shell's own handler).
  // The linux SdkHost exposes no mode-notice surface, so nothing is bound here
  // and no notice is synthesized.
}

void DeveloperPage::BuildMeasurementsCard() {
  DevCard card = MakeDevCard(T_("dev_measurements", "Measurements"));
  card.body->append(*MakeTextLine(
      T_("dev_measurements_detail",
         "What a provider failure costs. Reset, run a test, read back."),
      11, &kUrTextFaint));

  for (size_t index = 0; index < kMetricCount; ++index) {
    const MetricSpec& spec = kMetricSpecs[index];
    SettingRow row = MakeSettingRow(T_(spec.key, spec.label),
                                    T_(DetailKey(spec.key).c_str(), spec.detail));
    auto* value = Gtk::make_managed<Gtk::Label>("0");
    value->add_css_class("ur-mono-13");  // figures line up or they cannot be read
    value->set_xalign(1);
    row.trailing->append(*value);
    card.body->append(*row.root);
    metricRows_.push_back(MetricRowUi{row.root, value});

    // The "no failures yet" line sits between rows 8 and 9 (it explains why the
    // four failure rows below are absent).
    if (index == 8) {
      noFailuresLine_ =
          MakeTextLine(T_("dev_no_provider_failures", "No provider failures yet."), 13,
                       &kUrTextMuted);
      card.body->append(*noFailuresLine_);
    }
  }

  auto* reset = MakeActionButton(T_("dev_reset_measurements", "Reset measurements"), false);
  reset->signal_clicked().connect([this] {
    RunAction(Action::ResetMetrics, T_("dev_reset_measurements", "Reset measurements"));
  });
  card.body->append(*reset);

  mainStack_.append(*card.root);
  deviceCards_.push_back(card.root);  // needs a SESSION and nothing more
}

void DeveloperPage::BuildExitsCard() {
  DevCard card = MakeDevCard(T_("dev_exits", "Exits"));

  // Header and rows share the SAME width array (a per-row Auto would size per
  // row and misalign the columns), plus the fixed trailing actions cell.
  auto* header = kit::MakePaneTableHeader(
      kExitColumns,
      {T_("dev_col_exit", "Exit"), T_("dev_col_window", "Window"),
       T_("dev_col_tier", "Tier"), T_("dev_col_flows", "Flows"),
       T_("dev_col_failed_dials", "Failed dials"), T_("dev_col_state", "State")},
      1);
  if (auto* headerBox = dynamic_cast<Gtk::Box*>(header)) {
    auto* actionsHeader = Gtk::make_managed<Gtk::Label>(T_("dev_col_actions", "Actions"));
    actionsHeader->add_css_class("ur-col-header");
    actionsHeader->set_xalign(0);
    actionsHeader->set_hexpand(false);
    actionsHeader->set_size_request(kExitActionsWidthPx, -1);
    headerBox->append(*actionsHeader);
  }
  card.body->append(*header);

  exitsBody_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  card.body->append(*exitsBody_);

  card.body->append(*MakeTextLine(
      T_("dev_exits_actions_detail",
         "Drop, Stall and Shuffle degrade the live connection on purpose, immediately. "
         "Nothing here is queued or retried — an action that misses is reported, not "
         "replayed."),
      12, &kUrTextMuted));

  // Shuffle acts on the whole window, so it lives under the table, not in a row.
  auto* shuffle = MakeActionButton(T_("dev_shuffle_exits", "Shuffle exit window"), false);
  shuffle->signal_clicked().connect([this] { RunShuffleExits(); });
  card.body->append(*shuffle);

  tablesStack_.append(*card.root);
  deviceCards_.push_back(card.root);
}

void DeveloperPage::BuildDestinationsCard() {
  DevCard card = MakeDevCard(T_("dev_destinations", "Destinations"));
  card.body->append(*MakeTextLine(
      T_("dev_destinations_detail",
         "Which exit each destination's flows are landing on. A site spread over several "
         "rows is a site that lost its single egress IP."),
      11, &kUrTextFaint));

  card.body->append(*kit::MakePaneTableHeader(
      kDestinationColumns,
      {T_("dev_col_destination", "Destination"), T_("dev_col_exit", "Exit"),
       T_("dev_col_flows", "Flows")},
      1));

  destinationsBody_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  card.body->append(*destinationsBody_);

  tablesStack_.append(*card.root);
  deviceCards_.push_back(card.root);
}

void DeveloperPage::BuildProbeSuiteCard() {
  DevCard card = MakeDevCard(T_("dev_probe_suite", "Probe suite"));
  card.body->append(*MakeTextLine(
      T_("dev_probe_suite_detail",
         "Dials real targets through the live connection and times each stage. Runs with "
         "the SDK's own default configuration."),
      11, &kUrTextFaint));

  probeState_ = MakeTextLine(T_("dev_probe_suite_idle", "Not running."), 13, &kUrTextMuted);
  card.body->append(*probeState_);

  // The pair reads as one control: half of two always-live buttons would no-op.
  auto* controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  probeStartBtn_ = MakeActionButton(T_("dev_probe_suite_start", "Start probe suite"), false);
  probeStartBtn_->signal_clicked().connect([this] { RunProbeSuite(true); });
  controls->append(*probeStartBtn_);
  probeStopBtn_ = MakeActionButton(T_("dev_probe_suite_stop", "Stop"), false);
  probeStopBtn_->set_sensitive(false);
  probeStopBtn_->signal_clicked().connect([this] { RunProbeSuite(false); });
  controls->append(*probeStopBtn_);
  card.body->append(*controls);

  card.body->append(*kit::MakePaneTableHeader(
      kProbeColumns,
      {T_("dev_col_target", "Target"), T_("dev_col_kind", "Kind"),
       T_("dev_col_dns", "DNS"), T_("dev_col_connect", "Connect"),
       T_("dev_col_ttfb", "TTFB"), T_("dev_col_total", "Total"),
       T_("dev_col_outcome", "Outcome")},
      1));

  probeBody_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  card.body->append(*probeBody_);

  tablesStack_.append(*card.root);
  deviceCards_.push_back(card.root);
}

// ---- the session log card ---------------------------------------------------
// The read half of the Advanced-Mode workflow: the write half (the ~34 knobs
// below) already shipped, and the Windows README states the loop outright —
// flip a knob, then read the session banner in the log to see whether it did
// anything. On Linux the DeviceLocal that emits those lines lives in
// urnetworkd, whose log directory is 0700 root-owned, so this card is the only
// way this process can show them.
//
// DELIBERATELY NOT IN deviceCards_. Every other card on this page folds when
// there is no session; this one must not. The moment you most need the log is
// the moment start_tunnel refused and there is no device to read.
void DeveloperPage::BuildSessionLogCard() {
  DevCard card = MakeDevCard(T_("dev_session_log", "Session log"));

  card.body->append(*MakeTextLine(
      T_("dev_session_log_detail",
         "What the daemon has written since it started. The reliability lines ([rel] "
         "event=...) are how a knob below proves it changed something."),
      11, &kUrTextFaint));

  auto* controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

  // Follow: auto-scroll, on by default, and it turns ITSELF off the moment the
  // user scrolls up — a tail that yanks you back to the bottom while you are
  // reading is worse than no tail.
  logFollow_ = Gtk::make_managed<Gtk::CheckButton>(T_("dev_session_log_follow", "Follow"));
  logFollow_->set_active(true);
  logFollow_->set_valign(Gtk::Align::CENTER);
  logFollow_->signal_toggled().connect([this] {
    if (applyingFollow_) return;  // echo guard: the scroll handler flips this control
    logFollowing_ = logFollow_->get_active();
    if (logFollowing_) ScrollSessionLogToBottom();
  });
  controls->append(*logFollow_);

  auto* copyButton = MakeActionButton(T_("copy", "Copy"), false);
  copyButton->signal_clicked().connect([this] { CopySessionLog(); });
  controls->append(*copyButton);

  auto* saveButton = MakeActionButton(T_("save_logs", "Save logs"), false);
  saveButton->signal_clicked().connect([this] { SaveSessionLog(); });
  controls->append(*saveButton);
  card.body->append(*controls);

  // The gap/restart meta line: a ring that wrapped and a daemon that restarted
  // are both STATED here rather than silently changing what the tail contains.
  logGapLine_ = MakeTextLine({}, 11, &kUrAmber);
  logGapLine_->set_visible(false);
  card.body->append(*logGapLine_);

  // The one line that carries Loading / Empty / Failed / Refused. Kept as a
  // single reusable label instead of building and destroying an empty-state
  // widget per state: one writer per surface, and it can never be the case that
  // the card shows nothing at all.
  logStatusLine_ = kit::MakePaneEmptyLine(T_("loading", "Loading..."));
  // The kit sets vexpand on this species (it is normally the only thing in a
  // pane that fills). GTK4 propagates a child's expand flag up through its
  // ancestors, which would stretch the whole card column — clear it.
  logStatusLine_->set_vexpand(false);
  card.body->append(*logStatusLine_);

  logScroller_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  // Its OWN overflow on both axes. propagate_natural_width(false) is the
  // load-bearing half: a 300-character [rel] line must scroll inside this box,
  // never widen the destination (the page body must not scroll horizontally).
  logScroller_->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
  logScroller_->set_propagate_natural_width(false);
  logScroller_->set_propagate_natural_height(true);
  logScroller_->set_min_content_height(kLogScrollerMinHeight);
  logScroller_->set_max_content_height(kLogScrollerMaxHeight);
  logScroller_->set_hexpand(true);
  logScroller_->set_vexpand(false);  // same GTK4 propagation rule as above
  logScroller_->set_visible(false);  // nothing read yet: the status line speaks
  logBody_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  logBody_->set_valign(Gtk::Align::START);
  logScroller_->set_child(*logBody_);
  card.body->append(*logScroller_);

  // Bottom-anchoring rides the adjustment rather than a post-append call: the
  // upper bound is only correct after GTK has allocated the new rows, and
  // `changed` is exactly the signal that fires then.
  if (auto adjustment = logScroller_->get_vadjustment()) {
    adjustment->signal_changed().connect([this] {
      if (logFollowing_) ScrollSessionLogToBottom();
    });
    adjustment->signal_value_changed().connect([this] {
      if (scrollingToBottom_ || !logFollowing_) return;  // our own write, or already off
      auto current = logScroller_->get_vadjustment();
      if (!current) return;
      const double bottom = current->get_upper() - current->get_page_size();
      // 2px of slack: a wheel tick landing a subpixel short of the bottom is
      // not a request to stop following.
      if (current->get_value() >= bottom - 2.0) return;
      logFollowing_ = false;
      applyingFollow_ = true;
      if (logFollow_ != nullptr) logFollow_->set_active(false);
      applyingFollow_ = false;
    });
  }

  tablesStack_.append(*card.root);
  // NOT pushed into deviceCards_ — see the note above.
}

void DeveloperPage::BuildOverrideSections() {
  struct SectionSpec {
    Section section;
    const char* key;
    const char* label;
  };
  const SectionSpec sections[] = {
      {Section::Detection, "dev_detection", "Detection"},
      {Section::Placement, "dev_placement", "Placement"},
      {Section::Recovery, "dev_recovery", "Recovery"},
      {Section::Probing, "dev_probing", "Probing"},
      {Section::Observability, "dev_observability", "Observability"},
  };

  for (const SectionSpec& section : sections) {
    DevCard card = MakeDevCard(T_(section.key, section.label));

    // Merge the two spec tables by the in-section order so a section reads in
    // the windows build order however its rows are typed.
    struct PlanRow {
      int order;
      bool boolean;
      size_t index;
    };
    std::vector<PlanRow> plan;
    for (size_t index = 0; index < kBoolSpecCount; ++index) {
      if (kBoolSpecs[index].section == section.section) {
        plan.push_back(PlanRow{kBoolSpecs[index].order, true, index});
      }
    }
    for (size_t index = 0; index < kNumSpecCount; ++index) {
      if (kNumSpecs[index].section == section.section) {
        plan.push_back(PlanRow{kNumSpecs[index].order, false, index});
      }
    }
    std::sort(plan.begin(), plan.end(),
              [](const PlanRow& a, const PlanRow& b) { return a.order < b.order; });
    for (const PlanRow& row : plan) {
      if (row.boolean) {
        AddBoolRow(card.body, row.index);
      } else {
        AddNumRow(card.body, row.index);
      }
    }

    if (section.section == Section::Probing) {
      auto* probeAll =
          MakeActionButton(T_("dev_probe_all_exits_now", "Probe all exits now"), false);
      probeAll->signal_clicked().connect([this] {
        RunAction(Action::ProbeAllExits, T_("dev_probed_exits", "Probed exits"));
      });
      card.body->append(*probeAll);
    }
    if (section.section == Section::Observability) {
      auto* reset = MakeActionButton(
          T_("dev_reset_to_shipped_defaults", "Reset to shipped defaults"), false);
      reset->signal_clicked().connect([this] {
        RunAction(Action::ResetSettings,
                  T_("dev_reset_to_shipped_defaults", "Reset to shipped defaults"));
      });
      card.body->append(*reset);
    }

    sideStack_.append(*card.root);
    // The override sections are their OWN visibility group: they show only
    // while a settings read is in force. Gating the device cards on settings
    // instead once hid a perfectly readable exits table.
    settingsCards_.push_back(card.root);
  }
}

void DeveloperPage::AddBoolRow(Gtk::Box* body, size_t specIndex) {
  const BoolSpec& spec = kBoolSpecs[specIndex];
  SettingRow row = MakeSettingRow(T_(spec.key, spec.label),
                                  T_(DetailKey(spec.key).c_str(), spec.detail));
  auto* toggle = Gtk::make_managed<Gtk::Switch>();
  toggle->set_valign(Gtk::Align::CENTER);
  toggle->set_halign(Gtk::Align::END);
  // A switch with no name is the stated failure mode of this row species.
  kit::SetAccessibleLabel(*toggle, T_(spec.key, spec.label));
  const size_t uiIndex = boolRows_.size();
  toggle->property_active().signal_changed().connect(
      [this, uiIndex] { OnBoolToggled(uiIndex); });
  row.trailing->append(*toggle);
  body->append(*row.root);
  boolRows_.push_back(BoolRowUi{specIndex, toggle});
}

void DeveloperPage::AddNumRow(Gtk::Box* body, size_t specIndex) {
  const NumSpec& spec = kNumSpecs[specIndex];
  SettingRow row = MakeSettingRow(T_(spec.key, spec.label),
                                  T_(DetailKey(spec.key).c_str(), spec.detail));

  auto* effective = Gtk::make_managed<Gtk::Label>();
  effective->add_css_class("ur-caption");
  effective->set_xalign(1);
  effective->set_size_request(72, -1);
  row.trailing->append(*effective);

  // Without a maximum a typed value above INT32_MAX wraps NEGATIVE through the
  // cast into the live reliability stack; 1e12 ms is ~35 years, past meaning and
  // short of double->int64 UB.
  const double maximum = spec.i64 ? 1e12 : 2147483647.0;
  auto adjustment = Gtk::Adjustment::create(0.0, 0.0, maximum, 1.0, 100.0, 0.0);
  auto* box = Gtk::make_managed<Gtk::SpinButton>(adjustment, 1.0, 0);
  box->set_numeric(true);
  // A rejected edit snaps back to the value in force rather than showing an
  // unapplied number (windows ValidationMode InvalidInputOverwritten).
  box->set_update_policy(Gtk::SpinButton::UpdatePolicy::IF_VALID);
  box->set_size_request(120, -1);
  box->set_valign(Gtk::Align::CENTER);
  kit::SetAccessibleLabel(*box, T_(spec.key, spec.label));
  const size_t uiIndex = numRows_.size();
  box->signal_value_changed().connect([this, uiIndex] { OnNumChanged(uiIndex); });
  row.trailing->append(*box);

  body->append(*row.root);
  numRows_.push_back(NumRowUi{specIndex, box, effective});
}

// ---- polling (spec §2.13) ---------------------------------------------------

void DeveloperPage::ReconcilePoll() {
  // All four gates ANDed, reconciled on every change. What Advanced Mode buys
  // here is that the poll (seven synchronous rpcs into the service per tick)
  // stops when the destination is unreachable.
  const bool on = advanced_ && selected_ && presenting_ && built_;
  if (on == pollTimer_.connected()) return;
  if (on) {
    g_message("developer: poll started (%d ms)", kPollIntervalMs);
    pollTimer_ = Glib::signal_timeout().connect(
        [this] {
          SubmitPoll();
          return true;
        },
        kPollIntervalMs);
    SubmitPoll();  // on gate-on: start the timer AND read once
  } else {
    pollTimer_.disconnect();
    g_message("developer: poll stopped");
  }
}

void DeveloperPage::SubmitPoll() {
  if (!built_) return;
  bool expected = false;
  // One poll queued-or-running at a time: a read slower than the interval must
  // not stack more behind SdkHost's lock.
  if (!pollPending_.compare_exchange_strong(expected, true)) return;

  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  bridge_.Submit([this, alive, epoch, seen] {
    // Cleared on EVERY path: a stuck flag wedges both the timer and Refresh for
    // the process lifetime, and the screen then looks merely stale, not broken.
    ScopedAtomicFlag clear(pollPending_);
    const Snapshot snap = ReadSnapshot();
    PostToMain([this, alive, epoch, seen, snap] {
      if (!*alive || *epoch != seen) return;  // a newer Load() owns the page
      ApplySnapshot(snap);
    });
  });
}

DeveloperPage::Snapshot DeveloperPage::ReadSnapshot() {
  // ONE consistent read: the parts must not disagree about which session they
  // describe. Every getter is guarded on its own — a throwing getter costs its
  // field, not the whole snapshot — and the list getters are null-guarded (a
  // suite that never ran answers with a nil slice).
  Snapshot snap;

  // The session log comes from the CONTROL SOCKET, not from a DeviceRemote, so
  // it is read BEFORE the no-device fold below. That order is the whole point:
  // the case where the log is the only thing that can explain what happened is
  // exactly the case where start_tunnel refused and there is no device at all.
  if (!logSdkVersionSet_) {
    // Asked for lazily, here on the bridge, because the page can be constructed
    // before the SDK is initialized. Unset fails CLOSED as a version mismatch,
    // which is the right direction — an unversioned pair must never be told the
    // log is simply empty.
    logSdkVersionSet_ = true;
    try {
      logTail_.SetLocalSdkVersion(urnet::version());
    } catch (const std::exception& e) {
      g_warning("developer: urnet::version threw: %s", e.what());
    }
  }
  LogTailClient::FetchResult fetched = logTail_.Fetch(ctl::kLogTailDefaultMaxLines);
  snap.logRead = true;
  snap.logOk = fetched.ok;
  snap.logLines = std::move(fetched.lines);
  snap.logCursor = fetched.cursor;
  snap.logDropped = fetched.dropped;
  snap.logRestarted = fetched.restarted;
  snap.logState = fetched.state;
  snap.logError = std::move(fetched.error);
  snap.logCode = std::move(fetched.code);

  if (!host_.hasDevice()) return snap;  // no session: haveDevice false, all cards fold
  snap.haveDevice = true;
  urnet::DeviceRemote& device = host_.device();
  snap.remoteConnected =
      ReadGuarded<bool>("getRemoteConnected", [&] { return device.getRemoteConnected(); },
                        false);
  snap.settings = ReadGuarded<std::optional<urnet::ReliabilitySettings>>(
      "getReliabilitySettings", [&] { return device.getReliabilitySettings(); },
      std::nullopt);
  snap.metrics = ReadGuarded<std::optional<urnet::ReliabilityMetrics>>(
      "getReliabilityMetrics", [&] { return device.getReliabilityMetrics(); }, std::nullopt);
  const auto exits = ReadGuarded<std::optional<urnet::ExitList>>(
      "getExits", [&] { return device.getExits(); }, std::nullopt);
  if (exits) snap.exits = *exits;
  const auto destinations = ReadGuarded<std::optional<urnet::DestinationExitList>>(
      "getDestinationExits", [&] { return device.getDestinationExits(); }, std::nullopt);
  if (destinations) snap.destinationExits = *destinations;
  snap.probeSuiteRunning = ReadGuarded<bool>(
      "probeSuiteRunning", [&] { return device.probeSuiteRunning(); }, false);
  const auto results = ReadGuarded<std::optional<urnet::ProbeResultList>>(
      "getProbeResults", [&] { return device.getProbeResults(); }, std::nullopt);
  if (results) snap.probeResults = *results;
  return snap;
}

// ---- apply ------------------------------------------------------------------

void DeveloperPage::ApplySnapshot(const Snapshot& snap) {
  // Two INDEPENDENT visibility groups, because the things behind them fail
  // independently (spec §2.5).
  for (Gtk::Widget* card : deviceCards_) card->set_visible(snap.haveDevice);
  for (Gtk::Widget* card : settingsCards_) card->set_visible(snap.settings.has_value());

  ApplySettings(snap);
  // An absent metrics object renders zeros with the no-failures line: the "not
  // loaded at all" story is carried by the hint and the device-card gate, never
  // by dashes.
  ApplyMetrics(snap.metrics.value_or(urnet::ReliabilityMetrics{}));
  ApplyExits(snap.exits);
  ApplyDestinations(snap.destinationExits);
  ApplyProbeSuite(snap);
  // Last, and OUTSIDE both visibility groups: the log card renders in every
  // state this page can be in, including the no-session one the two loops above
  // just folded everything else for.
  ApplySessionLog(snap);
}

void DeveloperPage::ApplySettings(const Snapshot& snap) {
  const bool inForce = snap.settings.has_value();

  // The three absent-states, in the order a session reaches them.
  Glib::ustring hint;
  if (!snap.haveDevice) {
    hint = T_("dev_no_device", "No session. Sign in and connect to use these tools.");
  } else if (!snap.remoteConnected) {
    hint = T_("dev_service_detached",
              "The URnetwork service is not attached, so the live connection cannot be "
              "read.");
  } else if (!inForce) {
    hint = T_("dev_nothing_in_force",
              "No reliability override is in force, so the settings sections are hidden "
              "rather than shown at zero. The measurements and exit readout below are "
              "live.");
  }
  // Visibility rule: the hint shows iff NO override is in force — which is
  // exactly when one of the three sentences above is non-empty, so the one
  // text+visibility call carries it (one writer per surface).
  SetLineOrCollapse(connectHint_, hint, 14, nullptr);

  // Enabled iff there is a device — NOT gated on settings being in force.
  if (simulateBtn_) simulateBtn_->set_sensitive(snap.haveDevice);
  if (syncBtn_) syncBtn_->set_sensitive(snap.haveDevice);

  if (!inForce) return;  // nil means "nothing is in force": write NOTHING back

  // Echo guard: without it every 5s refresh would write every value back to the
  // device (~34 rpcs a poll) and race the user's edits.
  ScopedFlag guard(applying_);
  const urnet::ReliabilitySettings& settings = *snap.settings;

  for (const BoolRowUi& row : boolRows_) {
    const BoolSpec& spec = kBoolSpecs[row.specIndex];
    row.control->set_active(settings.*(spec.field));
  }
  for (const NumRowUi& row : numRows_) {
    const NumSpec& spec = kNumSpecs[row.specIndex];
    const int64_t value =
        spec.i64 ? settings.*(spec.i64) : static_cast<int64_t>(settings.*(spec.i32));
    // A focused box is never overwritten by the poll — a 5s tick must not stomp
    // an in-progress edit; the effective label still updates.
    if (!row.control->has_focus()) {
      row.control->set_value(static_cast<double>(value));
    }
    Glib::ustring effective;
    if (value == 0 && spec.zeroKey != nullptr) {
      effective = T_(spec.zeroKey, spec.zeroLabel);
    } else if (spec.millis) {
      effective = FormatDurationMillis(value);
    } else {
      effective = std::to_string(value);
    }
    row.effective->set_text(effective);
  }
}

void DeveloperPage::ApplyMetrics(const urnet::ReliabilityMetrics& metrics) {
  if (metricRows_.size() < kMetricCount) return;

  const bool anyLoss = metrics.ExitLossEvents > 0;
  for (size_t index = 0; index < kMetricCount; ++index) {
    Glib::ustring value;
    bool visible = true;
    switch (index) {
      case 0:
        value = std::to_string(metrics.FlowsOpened);
        break;
      case 1:
        value = std::to_string(metrics.DialFailuresIntercepted);
        break;
      case 2:
        value = std::to_string(metrics.FlowsReraced);
        break;
      case 3:
        value = Format(T_("dev_probes_value", "{0} sent / {1} answered"), metrics.ProbesSent,
                       metrics.ProbesAnswered);
        break;
      case 4:
        value = Format(T_("dev_busy_probes_value", "{0} sent / {1} acquitted"),
                       metrics.BusyProbesSent, metrics.BusyProbesAcquitted);
        break;
      case 5:
        value = Format(T_("dev_verdicts_held_value", "{0} / {1}"),
                       metrics.VerdictsHeldUplinkStale, metrics.VerdictsHeldTransportDown);
        break;
      case 6:  // zero is noise: the row hides rather than reading 0
        value = std::to_string(metrics.RemovalsDeferred);
        visible = metrics.RemovalsDeferred > 0;
        break;
      case 7:
        value = std::to_string(metrics.SchedulerPausesDetected);
        visible = metrics.SchedulerPausesDetected > 0;
        break;
      case 8:
        value = Format(T_("dev_quic_flows_rebound_value",
                          "{0} ({1} accepted / {2} re-dialed)"),
                       metrics.FlowsRebound, metrics.RebindsAccepted,
                       metrics.RebindsRedialed);
        visible = metrics.FlowsRebound > 0;
        break;
      case 9:
        value = Format(T_("dev_blast_radius_value", "{0} per failure"),
                       FormatOneDecimal(metrics.MeanFlowsLostPerExitLoss));
        visible = anyLoss;
        break;
      case 10:
        value = Format(T_("dev_worst_single_failure_value", "{} connections"),
                       metrics.MaxFlowsLostInOneEvent);
        visible = anyLoss;
        break;
      case 11:
        value = Format(T_("dev_recovery_time_value", "avg {0}, worst {1}"),
                       FormatDurationMillis(metrics.RecoveryMeanMillis),
                       FormatDurationMillis(metrics.RecoveryMaxMillis));
        visible = anyLoss;
        break;
      case 12:
        value = Format(T_("dev_never_came_back_value", "{0} of {1}"), metrics.RecoveryMissed,
                       metrics.FlowsLostToExit);
        visible = anyLoss;
        break;
      default:
        break;
    }
    metricRows_[index].value->set_text(value);
    metricRows_[index].root->set_visible(visible);
  }
  // Visible iff nothing has been lost yet — it explains the four absent rows.
  if (noFailuresLine_) noFailuresLine_->set_visible(!anyLoss);
}

void DeveloperPage::ApplyExits(const std::vector<urnet::Exit>& exits) {
  if (exitsBody_ == nullptr) return;

  // Exits with no client id are DROPPED entirely: no stable identity means
  // nothing to migrate and nothing to name the row with — never mint a label.
  std::vector<const urnet::Exit*> rows;
  std::string identity;
  for (const urnet::Exit& exit : exits) {
    if (!exit.ClientId || exit.ClientId->empty()) continue;
    rows.push_back(&exit);
    identity += *exit.ClientId + ";";
  }

  // Identity-keyed rebuild: the row SET is rebuilt only when the identity
  // string changes; cell TEXT is written on every poll. The identity is an
  // optional so the FIRST apply of an empty list still rebuilds (a bare string
  // would compare equal to its initial value and the empty-state row would
  // never be created — and that state is reachable: connected, in force, no
  // exits yet).
  if (!exitsIdentity_ || *exitsIdentity_ != identity) {
    exitsIdentity_ = identity;
    exitRows_.clear();
    RemoveAllChildren(*exitsBody_);
    if (rows.empty()) {
      exitsBody_->append(*kit::MakeEmptyState("network-offline-symbolic",
                                              T_("dev_no_exits", "No exits. Connect first.")));
    }
    for (const urnet::Exit* exit : rows) {
      const std::string clientId = *exit->ClientId;
      const std::string shortId = ShortId(clientId);
      kit::PaneTableRow row = kit::MakePaneTableRow(kExitColumns, kExitRowHeight, 1);
      PrepCell(*row.cells[0], "ur-mono-13");
      auto* cluster = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
      cluster->set_valign(Gtk::Align::CENTER);
      cluster->set_hexpand(false);
      cluster->set_size_request(kExitActionsWidthPx, -1);

      // Migrate is NOT destructive — it moves flows off an exit — so it wears
      // the accent, not danger.
      auto* migrate = MakeActionButton(T_("dev_migrate", "Migrate"), false);
      migrate->set_halign(Gtk::Align::CENTER);
      kit::SetAccessibleLabel(*migrate,
                              Glib::ustring(T_("dev_migrate", "Migrate")) + " " + shortId);
      migrate->signal_clicked().connect([this, clientId, shortId] {
        RunAction(Action::MigrateExit,
                  Glib::ustring(T_("dev_migrate_exit", "Migrated exit")) + " " + shortId,
                  clientId);
      });
      cluster->append(*migrate);

      auto addFault = [&](const char* key, const char* english, Fault fault) {
        auto* button = MakeDangerButton(T_(key, english));
        kit::SetAccessibleLabel(*button, Glib::ustring(T_(key, english)) + " " + shortId);
        button->signal_clicked().connect(
            [this, fault, clientId] { RunFaultAction(fault, clientId); });
        cluster->append(*button);
      };
      addFault("dev_drop", "Drop", Fault::Drop);
      // Stall and Unstall are BOTH always shown and enabled: urnet::Exit
      // carries no stalled flag, so the client cannot know which one is
      // meaningful, and a toggle would have to lie about its state.
      addFault("dev_stall", "Stall", Fault::Stall);
      addFault("dev_unstall", "Unstall", Fault::Unstall);

      if (auto* inner = RowInner(row.root)) inner->append(*cluster);
      exitsBody_->append(*row.root);
      exitRows_.push_back(ExitRowUi{clientId, row.cells});
    }
  }

  if (exitRows_.size() != rows.size()) return;  // rebuild raced a shorter list
  for (size_t index = 0; index < rows.size(); ++index) {
    const urnet::Exit& exit = *rows[index];
    const std::vector<Gtk::Label*>& cells = exitRows_[index].cells;
    WriteCell(*cells[0], ShortId(exitRows_[index].clientId), 13, nullptr);
    // "auto" is the SDK's own word for an unset window type, not a label.
    WriteCell(*cells[1], exit.WindowType.empty() ? std::string("auto") : exit.WindowType, 12,
              &kUrTextMuted);
    Glib::ustring tier = std::to_string(exit.Tier);
    if (exit.Tier < exit.EffectiveTier) {
      tier += "→" + std::to_string(exit.EffectiveTier);  // live demotion
    }
    WriteCell(*cells[2], tier, 12, &kUrTextMuted);
    WriteCell(*cells[3], std::to_string(exit.FlowCount), 12, &kUrTextMuted);
    WriteCell(*cells[4], std::to_string(exit.DialFailureCount), 12,
              exit.DialFailureCount > 0 ? &kUrDanger : &kUrTextMuted);

    Glib::ustring state;
    auto addPart = [&state](const Glib::ustring& part) {
      if (!state.empty()) state += " · ";
      state += part;
    };
    if (exit.Quarantined) {
      addPart(T_("dev_state_benched", "benched"));
    } else if (exit.Warning) {
      // the cause VERBATIM: a new go-side cause renders without an app update
      addPart(exit.WarningCause.empty() ? Glib::ustring(T_("dev_state_warned", "warned"))
                                        : Glib::ustring(exit.WarningCause));
    }
    if (exit.Done) addPart(T_("dev_state_done", "done"));
    if (exit.P2pOnly) addPart(T_("dev_state_p2p", "p2p"));
    // absence of `proven` means "not yet proven", never "bad"
    if (exit.Proven) addPart(T_("dev_state_proven", "proven"));
    // The state cell TRIMS rather than wraps: the kit pins one height per list
    // (windows' auto-height grid rows can wrap; a fixed-height row would clip
    // the second line instead), and the full state rides the row's tooltip.
    cells[5]->set_tooltip_text(state);
    WriteCell(*cells[5], state, 12,
              (exit.Quarantined || exit.Warning) ? &kUrDanger : &kUrTextMuted);
  }
}

void DeveloperPage::ApplyDestinations(
    const std::vector<urnet::DestinationExit>& destinations) {
  if (destinationsBody_ == nullptr) return;

  std::string identity;
  for (const urnet::DestinationExit& destination : destinations) {
    identity += destination.DestinationIp + ";";
  }
  if (!destinationsIdentity_ || *destinationsIdentity_ != identity) {
    destinationsIdentity_ = identity;
    destinationRows_.clear();
    RemoveAllChildren(*destinationsBody_);
    if (destinations.empty()) {
      destinationsBody_->append(*kit::MakeEmptyState(
          "view-list-symbolic", T_("dev_no_destinations", "No destinations yet.")));
    }
    for (size_t index = 0; index < destinations.size(); ++index) {
      kit::PaneTableRow row = kit::MakePaneTableRow(kDestinationColumns, kTableRowHeight, 1);
      PrepCell(*row.cells[0], "ur-mono-13");
      PrepCell(*row.cells[1], "ur-mono-12");
      destinationsBody_->append(*row.root);
      destinationRows_.push_back(LabelRowUi{row.cells});
    }
  }

  // Cells are rewritten every poll: this table churns faster than exits, and
  // rebuilding it per poll would make it unreadable.
  if (destinationRows_.size() != destinations.size()) return;
  for (size_t index = 0; index < destinations.size(); ++index) {
    const urnet::DestinationExit& destination = destinations[index];
    const std::vector<Gtk::Label*>& cells = destinationRows_[index].cells;
    WriteCell(*cells[0], destination.DestinationIp, 13, nullptr);
    const bool haveExit = destination.ClientId && !destination.ClientId->empty();
    // an em-dash, not a zero-length cell: no exit is a fact, not a blank
    WriteCell(*cells[1], haveExit ? ShortId(*destination.ClientId) : std::string("—"),
              12, &kUrTextMuted);
    WriteCell(*cells[2], std::to_string(destination.FlowCount), 12, &kUrTextMuted);
  }
}

void DeveloperPage::ApplyProbeSuite(const Snapshot& snap) {
  if (probeState_ != nullptr) {
    SetLineOrCollapse(probeState_,
                      snap.probeSuiteRunning
                          ? T_("dev_probe_suite_running", "Running.")
                          : T_("dev_probe_suite_idle", "Not running."),
                      13, snap.probeSuiteRunning ? &kUrAccent : &kUrTextMuted);
  }
  if (probeStartBtn_) probeStartBtn_->set_sensitive(!snap.probeSuiteRunning);
  if (probeStopBtn_) probeStopBtn_->set_sensitive(snap.probeSuiteRunning);
  if (probeBody_ == nullptr) return;

  // A target legitimately appears once per probe kind, so the identity is
  // Name + "/" + Kind — a repeating suite rewrites its latencies in place.
  std::string identity;
  for (const urnet::ProbeResult& result : snap.probeResults) {
    identity += result.Name + "/" + result.Kind + ";";
  }
  if (!probeIdentity_ || *probeIdentity_ != identity) {
    probeIdentity_ = identity;
    probeRows_.clear();
    RemoveAllChildren(*probeBody_);
    if (snap.probeResults.empty()) {
      probeBody_->append(*kit::MakeEmptyState(
          "system-search-symbolic",
          T_("dev_no_probe_results", "No probe results yet. Start the suite.")));
    }
    for (size_t index = 0; index < snap.probeResults.size(); ++index) {
      kit::PaneTableRow row = kit::MakePaneTableRow(kProbeColumns, kTableRowHeight, 1);
      PrepCell(*row.cells[0], "ur-mono-13");
      probeBody_->append(*row.root);
      probeRows_.push_back(LabelRowUi{row.cells});
    }
  }

  if (probeRows_.size() != snap.probeResults.size()) return;
  for (size_t index = 0; index < snap.probeResults.size(); ++index) {
    const urnet::ProbeResult& result = snap.probeResults[index];
    const std::vector<Gtk::Label*>& cells = probeRows_[index].cells;
    WriteCell(*cells[0], result.Name, 13, nullptr);
    WriteCell(*cells[1], result.Kind, 12, &kUrTextMuted);
    // A zero stage means "did not happen / not measured" — an HTTP probe handed
    // an address records no DNS time, and rendering 0ms would claim a
    // measurement that was never taken.
    auto stage = [](int64_t ms) {
      return ms <= 0 ? std::string("—") : std::to_string(ms) + "ms";
    };
    WriteCell(*cells[2], stage(result.DnsMillis), 12, &kUrTextMuted);
    WriteCell(*cells[3], stage(result.ConnectMillis), 12, &kUrTextMuted);
    WriteCell(*cells[4], stage(result.TtfbMillis), 12, &kUrTextMuted);
    WriteCell(*cells[5], stage(result.TotalMillis), 12, &kUrTextMuted);
    if (result.Ok) {
      Glib::ustring outcome = T_("dev_probe_ok", "ok");
      if (result.ByteCount > 0) {
        outcome += " · " + FormatByteCountCompact(result.ByteCount);
      }
      WriteCell(*cells[6], outcome, 12, &kUrTextMuted);
    } else {
      // the SDK error VERBATIM when it has one: new go-side failures render
      // without an app update
      WriteCell(*cells[6],
                result.Error.empty() ? Glib::ustring(T_("dev_probe_failed", "failed"))
                                     : Glib::ustring(result.Error),
                12, &kUrDanger);
    }
  }
}

void DeveloperPage::SetLastAction(const Glib::ustring& text) {
  SetLineOrCollapse(lastAction_, text, 12, &kUrTextMuted);
}

// ---- the session log (spec: the four distinguishable states) ---------------

void DeveloperPage::ClearSessionLogRows() {
  if (logBody_ == nullptr) return;
  RemoveAllChildren(*logBody_);
  logRows_.clear();
}

void DeveloperPage::ScrollSessionLogToBottom() {
  if (logScroller_ == nullptr) return;
  auto adjustment = logScroller_->get_vadjustment();
  if (!adjustment) return;
  // Guarded so the value_changed handler does not read our own write as the
  // user scrolling away and turn Follow off on every append.
  scrollingToBottom_ = true;
  adjustment->set_value(adjustment->get_upper() - adjustment->get_page_size());
  scrollingToBottom_ = false;
}

void DeveloperPage::AppendSessionLogRow(const ctl::LogLine& line) {
  if (logBody_ == nullptr) return;
  // textColumns = 3: all three cells read LEFT. The kit's default of one text
  // column right-aligns the rest against the figure rule, which for a log turns
  // the message column into a ragged right edge.
  kit::PaneTableRow row = kit::MakePaneTableRow(kLogColumns, kLogRowHeight, 3);
  if (row.cells.size() < 3) return;
  for (Gtk::Label* cell : row.cells) PrepCell(*cell, "ur-mono-12");

  // The message is NOT trimmed. An ellipsized log line reads as the whole line,
  // and the one that got cut is always the one you needed; it scrolls
  // horizontally inside the card instead. Selectable so a single line can be
  // pasted into a bug report without exporting the file.
  row.cells[2]->set_ellipsize(Pango::EllipsizeMode::NONE);
  row.cells[2]->set_selectable(true);

  const Rgba* messageColor = &kUrTextMuted;
  if (LineIsReliability(line.text)) {
    messageColor = &kUrAccent;  // the stream the knobs above are judged by
  } else if (LineIsFailure(line.text)) {
    messageColor = &kUrDanger;
  }
  WriteCell(*row.cells[0], FormatClockTime(line.unix_ms), 11, &kUrTextFaint);
  WriteCell(*row.cells[1], line.source, 11, &kUrTextFaint);
  WriteCell(*row.cells[2], line.text, 12, messageColor);

  logBody_->append(*row.root);
  logRows_.push_back(row.root);
  while (logRows_.size() > kLogRenderCap) {
    Gtk::Widget* oldest = logRows_.front();
    logRows_.pop_front();
    logBody_->remove(*oldest);
  }
}

Glib::ustring DeveloperPage::SessionLogStatusText(const Snapshot& snap) const {
  // 1. LOADING — the first poll has not answered yet. Distinct from empty: we
  //    have not looked, so we cannot say there is nothing.
  if (!snap.logRead) return T_("loading", "Loading...");

  // 2. REFUSED — reachable only if the owner picks the gated policy. It exists
  //    in this state machine from the first version deliberately: a refusal
  //    bolted on later becomes a blank.
  if (snap.logCode == ctl::kCodeLogAccessDenied) {
    return T_("dev_session_log_denied",
              "This account may drive the daemon but may not read its log.");
  }

  // 3. FAILED — each daemon-session state in the words it ALREADY ships in
  //    (MainWindow's daemon copy), because they are four different problems
  //    with four different fixes and none may collapse into a blank.
  switch (snap.logState) {
    case DaemonSessionState::Unreachable:
      return T_("daemon_unreachable",
                "The URnetwork system service is not running. Install or start it, then "
                "try again.");
    case DaemonSessionState::DaemonTooOld:
      return T_("daemon_too_old",
                "The URnetwork system service is out of date. Update it to connect.");
    case DaemonSessionState::ClientTooOld:
      return T_("app_too_old_for_daemon",
                "This app is older than the installed URnetwork system service. Update the "
                "app to connect.");
    case DaemonSessionState::SdkMismatch:
      return T_("daemon_sdk_mismatch",
                "The app and the URnetwork system service are different builds. Update both "
                "to the same version.");
    case DaemonSessionState::Error:
      // The daemon's own message VERBATIM when it has one — including the
      // "unknown verb" a daemon predating log_tail answers with, which is a
      // real and actionable fact about the pair.
      return snap.logError.empty()
                 ? Glib::ustring(T_("something_went_wrong", "Something went wrong."))
                 : Glib::ustring(snap.logError);
    case DaemonSessionState::Ok:
      break;
  }

  // 4. EMPTY — reachable, answered, and it has genuinely written nothing.
  //    Deliberately NOT the shipped no_log_files_found ("No log file found"),
  //    which is a different fact and belongs to the Settings row.
  if (!logBuffer_.empty()) return {};  // rows are on screen; they speak for themselves
  return T_("dev_session_log_empty", "The daemon has not written anything yet.");
}

void DeveloperPage::ApplySessionLog(const Snapshot& snap) {
  if (logBody_ == nullptr) return;

  // A daemon restart rewinds the seq space. Interleaving two processes' lines
  // would produce a plausible WRONG order, so the previous run is dropped and
  // the fact is said in words on the meta line rather than fabricated into a
  // log row (nothing this app invents may end up in an exported log).
  if (snap.logRestarted) {
    logBuffer_.clear();
    ClearSessionLogRows();
    logDroppedTotal_ = 0;  // the previous run's gaps are not this run's
    logRestartNoted_ = true;
  }

  // ACCUMULATED, because a reply only reports the gap IT crossed: after the
  // first fetch past a wrap the cursor is beyond the hole and every later reply
  // reports 0. Without the running total the marker would blink once and vanish.
  logDroppedTotal_ += snap.logDropped;

  for (const ctl::LogLine& line : snap.logLines) {
    logBuffer_.push_back(line);
    AppendSessionLogRow(line);
  }
  while (logBuffer_.size() > ctl::kLogTailClientCap) logBuffer_.pop_front();

  Glib::ustring meta;
  if (logRestartNoted_) {
    meta = T_("dev_session_log_restarted",
              "The daemon restarted. This is a new log; the previous run's lines are gone.");
  }
  if (logDroppedTotal_ > 0) {
    const Glib::ustring dropped =
        Format(T_("dev_session_log_dropped", "{} lines were dropped before this point"),
               logDroppedTotal_);
    meta = meta.empty() ? dropped : meta + " " + dropped;
  }
  SetLineOrCollapse(logGapLine_, meta, 11, &kUrAmber);

  const bool failed = snap.logRead && snap.logState != DaemonSessionState::Ok;
  SetLineOrCollapse(logStatusLine_, SessionLogStatusText(snap), 13,
                    failed ? &kUrDanger : &kUrTextMuted);

  // A failure does NOT blank what was already read: lines already delivered are
  // still true, and they are usually what explains the failure.
  if (logScroller_ != nullptr) logScroller_->set_visible(!logRows_.empty());
}

std::string DeveloperPage::ComposeSessionLogText() const {
  std::string out = "== urnetwork daemon session log (urnetworkd) ==\n";
  out += "# This is the DAEMON half. The app's own log is exported from "
         "Settings > Save logs.\n";
  out += "# lines: " + std::to_string(logBuffer_.size());
  if (logDroppedTotal_ > 0) {
    out += ", dropped before this point: " + std::to_string(logDroppedTotal_);
  }
  if (logRestartNoted_) out += ", daemon restarted during this session";
  out += "\n\n";
  for (const ctl::LogLine& line : logBuffer_) {
    out += FormatStampIso(line.unix_ms) + "  " + line.source + "  " + line.text + "\n";
  }
  return out;
}

void DeveloperPage::CopySessionLog() {
  // The FULL buffer, not the rendered tail — the same rule the kit's CopyField
  // follows (what is copied is the value, never the display of it).
  get_clipboard()->set_text(ComposeSessionLogText());
  SetLastAction(T_("copy", "Copy"));
}

void DeveloperPage::SaveSessionLog() {
  auto* root = dynamic_cast<Gtk::Window*>(get_root());
  if (root == nullptr) {
    g_warning("developer: save session log dropped (page has no window yet)");
    return;
  }
  // Composed NOW, on the UI thread, so the file is what was on screen when the
  // button was pressed rather than whatever the poll had reached by the time
  // the user picked a filename.
  const std::string text = ComposeSessionLogText();

  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, T_("export_logs", "Export Logs"));
  gtk_file_dialog_set_initial_name(dialog, "urnetworkd-session.log");
  struct SavePayload {
    DeveloperPage* page;
    std::shared_ptr<bool> alive;
    std::shared_ptr<uint64_t> epoch;
    uint64_t seen;
    std::string text;
  };
  auto* payload = new SavePayload{this, alive_, epoch_, *epoch_, text};
  gtk_file_dialog_save(
      dialog, GTK_WINDOW(root->gobj()), nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<SavePayload> owned(static_cast<SavePayload*>(data));
        GFile* target =
            gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
        if (target == nullptr) return;  // dismissed: a cancel is never a failure
        GError* error = nullptr;
        const bool ok =
            g_file_replace_contents(target, owned->text.data(), owned->text.size(), nullptr,
                                    FALSE, G_FILE_CREATE_NONE, nullptr, nullptr,
                                    &error) != FALSE;
        if (!ok) {
          g_warning("developer: save session log failed: %s",
                    error != nullptr ? error->message : "(no error text)");
        }
        g_clear_error(&error);
        g_object_unref(target);
        // BOTH guards, and only after the file work: the completion can land
        // after the page was destroyed or a newer Load() took ownership.
        if (!*owned->alive || *owned->epoch != owned->seen) return;
        owned->page->SetLastAction(ok ? T_("save_logs", "Save logs")
                                      : T_("something_went_wrong", "Something went wrong."));
      },
      payload);
  g_object_unref(dialog);
}

// ---- actions (spec §2.12) ---------------------------------------------------

void DeveloperPage::RunAction(Action action, const Glib::ustring& described,
                              const std::string& exitClientId) {
  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  bridge_.Submit([this, alive, epoch, seen, action, described, exitClientId] {
    ActionOutcome outcome;
    if (host_.hasDevice()) {
      urnet::DeviceRemote& device = host_.device();
      try {
        switch (action) {
          case Action::ResetMetrics:
            device.resetReliabilityMetrics();
            outcome.issued = true;
            break;
          case Action::ResetSettings:
            device.resetReliabilitySettings();
            outcome.issued = true;
            break;
          case Action::SimulateNetworkChange:
            device.simulateNetworkChange();
            outcome.issued = true;
            break;
          case Action::Sync:
            device.sync();
            outcome.issued = true;
            break;
          case Action::ProbeAllExits: {
            const int64_t count = device.probeAllExits();
            outcome.issued = true;
            // A NEGATIVE return is the SDK's not-found sentinel, NOT a count
            // (the first windows version rendered "affected -1"); 0 IS a real
            // answer, so the test is < 0 and never <= 0.
            outcome.declined = count < 0;
            outcome.hasCount = !outcome.declined;
            outcome.count = count;
            break;
          }
          case Action::MigrateExit: {
            if (exitClientId.empty()) break;  // empty id = skip
            const int64_t count = device.migrateExit(exitClientId);
            outcome.issued = true;
            outcome.declined = count < 0;
            outcome.hasCount = !outcome.declined;
            outcome.count = count;
            break;
          }
        }
      } catch (const std::exception& e) {
        g_warning("developer: action threw: %s", e.what());
        outcome = ActionOutcome{};
      } catch (...) {
        g_warning("developer: action threw");
        outcome = ActionOutcome{};
      }
    }
    // Every action path re-reads the snapshot and reapplies it BEFORE
    // reporting, so the report lands under a readout that already moved.
    const Snapshot snap = ReadSnapshot();
    PostToMain([this, alive, epoch, seen, snap, outcome, described] {
      if (!*alive || *epoch != seen) return;
      ApplySnapshot(snap);
      // The OUTCOME is reported after the fact — never "Requested: X" before
      // submission, which with no session rendered a request line under "No
      // session…".
      Glib::ustring report;
      if (!outcome.issued) {
        report = T_("dev_not_issued", "Not issued: there is no live session to act on.");
      } else if (outcome.declined) {
        report = T_("dev_action_declined", "No change: the SDK declined that action.");
      } else if (outcome.hasCount) {
        report = described + ": " + T_("dev_count_affected", "affected") + " " +
                 std::to_string(outcome.count);
      } else {
        report = Glib::ustring(T_("dev_requested", "Requested:")) + " " + described;
      }
      SetLastAction(report);
    });
  });
}

void DeveloperPage::RunFaultAction(Fault fault, const std::string& clientId) {
  if (clientId.empty()) return;  // immediate-or-nothing: nothing to aim at
  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  const std::string shortId = ShortId(clientId);
  bridge_.Submit([this, alive, epoch, seen, fault, clientId, shortId] {
    bool issued = false;
    bool ok = false;
    if (host_.hasDevice()) {
      urnet::DeviceRemote& device = host_.device();
      try {
        // ONE call, no client-side retry, nothing queued into sync state: a
        // drop replayed after an RPC reconnect hits a different, healthy exit
        // (the SDK pins this with
        // TestDeviceRemoteAdvancedModeActionsAreNeverQueued). No confirm modal
        // either — the report line and the log naming the exit are the record.
        switch (fault) {
          case Fault::Drop:
            ok = device.dropExit(clientId);
            break;
          case Fault::Stall:
            ok = device.stallExit(clientId, true);
            break;
          case Fault::Unstall:
            ok = device.stallExit(clientId, false);
            break;
        }
        issued = true;
      } catch (const std::exception& e) {
        g_warning("developer: fault action threw: %s", e.what());
      } catch (...) {
        g_warning("developer: fault action threw");
      }
    }
    const Snapshot snap = ReadSnapshot();
    PostToMain([this, alive, epoch, seen, snap, fault, issued, ok, shortId] {
      if (!*alive || *epoch != seen) return;
      ApplySnapshot(snap);
      Glib::ustring report;
      if (!issued) {
        report = T_("dev_not_issued", "Not issued: there is no live session to act on.");
      } else if (!ok) {
        // "the SDK did it" is separated from "the SDK declined": an action
        // aimed at an exit that left the window says so instead of claiming
        // success.
        report = Glib::ustring(T_("dev_declined_exit",
                                  "No change: the SDK declined that exit")) +
                 " " + shortId;
      } else {
        switch (fault) {
          case Fault::Drop:
            report = Glib::ustring(T_("dev_dropped_exit", "Dropped exit")) + " " + shortId;
            break;
          case Fault::Stall:
            report = Glib::ustring(T_("dev_stalled_exit", "Stalled exit")) + " " + shortId;
            break;
          case Fault::Unstall:
            report = Glib::ustring(T_("dev_unstalled_exit", "Cleared stall on exit")) + " " +
                     shortId;
            break;
        }
      }
      SetLastAction(report);
    });
  });
}

void DeveloperPage::RunShuffleExits() {
  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  bridge_.Submit([this, alive, epoch, seen] {
    bool issued = false;
    if (host_.hasDevice()) {
      try {
        host_.device().shuffleExits();
        issued = true;
      } catch (const std::exception& e) {
        g_warning("developer: shuffleExits threw: %s", e.what());
      } catch (...) {
        g_warning("developer: shuffleExits threw");
      }
    }
    const Snapshot snap = ReadSnapshot();
    PostToMain([this, alive, epoch, seen, snap, issued] {
      if (!*alive || *epoch != seen) return;
      ApplySnapshot(snap);
      // shuffleExits is void on both Device forms, so "requested" is the
      // ceiling; the exits table above is where the reshuffle is visible.
      SetLastAction(issued
                        ? Glib::ustring(T_("dev_requested", "Requested:")) + " " +
                              T_("dev_shuffle_exits", "Shuffle exit window")
                        : Glib::ustring(T_("dev_not_issued",
                                           "Not issued: there is no live session to act "
                                           "on.")));
    });
  });
}

void DeveloperPage::RunProbeSuite(bool start) {
  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  bridge_.Submit([this, alive, epoch, seen, start] {
    bool issued = false;
    bool started = false;
    if (host_.hasDevice()) {
      urnet::DeviceRemote& device = host_.device();
      try {
        if (start) {
          // NEVER a default-constructed ProbeSuiteConfig — that is a suite with
          // zero concurrency and zero timeout, the same bug class as writing a
          // zeroed ReliabilitySettings. The SDK's own defaults or nothing.
          started = device.startProbeSuite(urnet::getDefaultProbeSuiteConfig());
        } else {
          device.stopProbeSuite();
        }
        issued = true;
      } catch (const std::exception& e) {
        g_warning("developer: probe suite call threw: %s", e.what());
      } catch (...) {
        g_warning("developer: probe suite call threw");
      }
    }
    // Running state and results come back on the ordinary poll (one consistent
    // read), not from the action.
    const Snapshot snap = ReadSnapshot();
    PostToMain([this, alive, epoch, seen, snap, start, issued, started] {
      if (!*alive || *epoch != seen) return;
      ApplySnapshot(snap);
      Glib::ustring report;
      if (!issued) {
        report = T_("dev_not_issued", "Not issued: there is no live session to act on.");
      } else if (!start) {
        report = T_("dev_probe_suite_stopped", "Probe suite stopped");
      } else if (started) {
        report = T_("dev_probe_suite_started", "Probe suite started");
      } else {
        report = T_("dev_probe_suite_not_started",
                    "Probe suite did not start: no live session, or one is already "
                    "running.");
      }
      SetLastAction(report);
    });
  });
}

// ---- edit semantics (spec §2.11) --------------------------------------------

void DeveloperPage::EditSettings(std::function<void(urnet::ReliabilitySettings&)> mutate) {
  auto alive = alive_;
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  bridge_.Submit([this, alive, epoch, seen, mutate] {
    const bool haveDevice = host_.hasDevice();
    bool applied = false;
    if (haveDevice) {
      urnet::DeviceRemote& device = host_.device();
      try {
        // Read-modify-write of the WHOLE struct from a FRESH read: an edit
        // never writes from the snapshot on screen (one poll interval stale —
        // it would revert concurrent changes), and the seven fields this UI
        // never exposes round-trip untouched.
        std::optional<urnet::ReliabilitySettings> current = device.getReliabilitySettings();
        if (current) {
          mutate(*current);
          device.setReliabilitySettings(current);
          applied = true;
        }
        // nullopt => NO-OP. Never substitute a default-constructed struct on
        // the write path: a zeroed struct installs an all-zero override that
        // disables the entire reliability stack, and sync() latches it.
      } catch (const std::exception& e) {
        g_warning("developer: settings edit threw: %s", e.what());
      } catch (...) {
        g_warning("developer: settings edit threw");
      }
    }
    // Re-read what the device APPLIED, not what was asked.
    const Snapshot snap = ReadSnapshot();
    PostToMain([this, alive, epoch, seen, snap, haveDevice, applied] {
      if (!*alive || *epoch != seen) return;
      ApplySnapshot(snap);
      if (applied) return;
      // Never a silent spring-back: a write that could not happen says which
      // of the two reasons it was.
      SetLastAction(haveDevice
                        ? T_("dev_not_applied",
                             "Not applied: there is no reliability override in force to "
                             "change.")
                        : T_("dev_not_issued",
                             "Not issued: there is no live session to act on."));
    });
  });
}

void DeveloperPage::OnBoolToggled(size_t uiIndex) {
  if (applying_) return;  // echo guard: a snapshot is writing the controls
  if (uiIndex >= boolRows_.size()) return;
  const BoolRowUi& row = boolRows_[uiIndex];
  const BoolSpec& spec = kBoolSpecs[row.specIndex];
  const bool value = row.control->get_active();
  auto field = spec.field;
  EditSettings([field, value](urnet::ReliabilitySettings& settings) {
    settings.*field = value;
  });
}

void DeveloperPage::OnNumChanged(size_t uiIndex) {
  if (applying_) return;
  if (uiIndex >= numRows_.size()) return;
  const NumRowUi& row = numRows_[uiIndex];
  const NumSpec& spec = kNumSpecs[row.specIndex];
  const double raw = row.control->get_value();
  // A cleared box or a negative number writes NOTHING.
  if (std::isnan(raw) || raw < 0.0) return;
  // Clamp BEFORE the cast: a typed 1e30 cast straight to an integer is UB, not
  // a big number.
  const double maximum = spec.i64 ? 1e12 : 2147483647.0;
  const int64_t value = static_cast<int64_t>(raw > maximum ? maximum : raw);
  auto i64 = spec.i64;
  auto i32 = spec.i32;
  EditSettings([i64, i32, value](urnet::ReliabilitySettings& settings) {
    if (i64 != nullptr) {
      settings.*i64 = value;
    } else if (i32 != nullptr) {
      settings.*i32 = static_cast<int32_t>(value);
    }
  });
}

}  // namespace urnw
