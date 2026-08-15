// DeveloperPage — the Advanced-Mode DEVELOPER destination (windows
// DeveloperPage.{h,cpp} parity; docs/parity/support-developer.md §2).
//
// A card-model page: intro card (session-less actions + the connect hint's
// three distinguishable absent-states), Measurements, the Exits /
// Destinations / Probe-suite tables (identity-keyed rebuild: the row SET is
// rebuilt only when the identity string changes, cell TEXT is written on
// every poll), and the five reliability-override sections bound field-by-
// field to urnet::ReliabilitySettings.
//
// The three shipped-bug invariants (spec §2.11) are load-bearing here:
//   1. every edit is a read-modify-write of the WHOLE struct from a FRESH
//      read on the bridge — never from the on-screen snapshot;
//   2. a nil settings read means "nothing is in force", NEVER "everything is
//      off" — the sections hide, and NOTHING ever writes a zeroed struct;
//   3. an echo guard (applying_) keeps the 5s poll from writing every value
//      back to the device and racing user edits.
//
// All rpcs ride ONE serial FIFO bridge thread (spec §2.14): edits stay
// ordered, a stale poll cannot be painted over a newer edit snapshot, and the
// destructor joins so no job can outlive the page. Polling (5000 ms) is gated
// on advanced && selected && presenting && built, reconciled on every change.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtkmm.h>
#include <urnetwork_sdk.hpp>

#include "LogTailClient.hpp"
#include "SdkHost.hpp"

namespace urnw {

class DeveloperPage : public Gtk::Box {
 public:
  explicit DeveloperPage(SdkHost& host);
  ~DeveloperPage() override;

  // nav-select + auth-change API loads: bump the epoch, build on first
  // selection, reconcile the poll gate, read once.
  void Load();
  // The poll gate's four AND-ed inputs; the page reconciles internally.
  void SetAdvancedMode(bool on);
  void SetPresenting(bool presenting);  // window visible AND not minimized
  void SetSelected(bool selected);      // this destination is the selected nav item
  // wide >= 1000 dip: cap 1800, overrides beside measurements; narrow: cap
  // 1000, overrides stack below (spec §2.2).
  void ApplyBreakpoint(int widthDip);

 private:
  // one consistent read of the reliability surface (spec §2.13's seven rpcs)
  struct Snapshot {
    bool haveDevice = false;
    bool remoteConnected = false;
    std::optional<urnet::ReliabilitySettings> settings;  // nullopt = nothing in force
    std::optional<urnet::ReliabilityMetrics> metrics;
    std::vector<urnet::Exit> exits;
    std::vector<urnet::DestinationExit> destinationExits;
    bool probeSuiteRunning = false;
    std::vector<urnet::ProbeResult> probeResults;

    // ---- the session log (read over the CONTROL socket, not the device) ----
    // Read BEFORE the !hasDevice() fold in ReadSnapshot, deliberately: the log
    // comes from the daemon, not from a DeviceRemote, and the moment you most
    // need it is the moment the tunnel refused to start and there is no device
    // at all. Everything below is "what this poll learned", never accumulated
    // state — the page owns the buffer.
    bool logRead = false;               // a fetch was ATTEMPTED this poll
    bool logOk = false;                 // ...and the daemon answered ok:true
    std::vector<ctl::LogLine> logLines; // NEW lines only, oldest first
    int64_t logCursor = 0;              // where the conversation stands now
    int64_t logDropped = 0;             // ring gap crossed by THIS reply
    bool logRestarted = false;          // the daemon's seq rewound: a new process
    DaemonSessionState logState = DaemonSessionState::Unreachable;
    std::string logError;               // the daemon's message, verbatim
    std::string logCode;                // machine-readable rejection code
  };

  enum class Action {
    ResetMetrics,
    ResetSettings,
    ProbeAllExits,
    SimulateNetworkChange,
    Sync,
    MigrateExit,
  };
  enum class Fault { Drop, Stall, Unstall };

  // ---- the serial FIFO bridge (spec §2.14) --------------------------------
  // ONE worker thread carries every rpc this page makes: edits stay ordered
  // (each is an absolute read-modify-write), results are delivered in
  // production order, and the destructor stops, CLEARS queued jobs, and
  // JOINS so no job can be inside SdkHost after teardown.
  class Bridge {
   public:
    Bridge();
    ~Bridge();
    void Submit(std::function<void()> job);
    void Shutdown();  // idempotent: stop, clear queue, notify, join

   private:
    void Run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stop_ = false;
    std::thread thread_;
  };

  struct DevCard {
    Gtk::Box* root = nullptr;
    Gtk::Box* body = nullptr;
  };
  struct MetricRowUi {
    Gtk::Widget* root = nullptr;
    Gtk::Label* value = nullptr;
  };
  struct BoolRowUi {
    size_t specIndex = 0;  // into the file-local bool spec table
    Gtk::Switch* control = nullptr;
  };
  struct NumRowUi {
    size_t specIndex = 0;  // into the file-local number spec table
    Gtk::SpinButton* control = nullptr;
    Gtk::Label* effective = nullptr;
  };
  struct ExitRowUi {
    std::string clientId;
    std::vector<Gtk::Label*> cells;  // 6 text cells; col 6 is the action cluster
  };
  struct LabelRowUi {
    std::vector<Gtk::Label*> cells;
  };

  // build
  void EnsureBuilt();  // page content is built lazily on first selection
  DevCard MakeDevCard(const Glib::ustring& heading);
  void BuildIntroCard();
  void BuildMeasurementsCard();
  void BuildExitsCard();
  void BuildDestinationsCard();
  void BuildProbeSuiteCard();
  void BuildSessionLogCard();
  void BuildOverrideSections();
  void AddBoolRow(Gtk::Box* body, size_t specIndex);
  void AddNumRow(Gtk::Box* body, size_t specIndex);

  // polling
  void ReconcilePoll();
  void SubmitPoll();          // coalesced: one poll queued-or-running at a time
  Snapshot ReadSnapshot();    // bridge thread only

  // apply (UI thread)
  void ApplySnapshot(const Snapshot& snap);
  void ApplySettings(const Snapshot& snap);
  void ApplyMetrics(const urnet::ReliabilityMetrics& metrics);
  void ApplyExits(const std::vector<urnet::Exit>& exits);
  void ApplyDestinations(const std::vector<urnet::DestinationExit>& destinations);
  void ApplyProbeSuite(const Snapshot& snap);
  void SetLastAction(const Glib::ustring& text);

  // ---- session log (UI thread) --------------------------------------------
  void ApplySessionLog(const Snapshot& snap);
  // The four failure states in the words they already ship in (MainWindow's
  // daemon copy), plus the refused state. Empty = nothing to say.
  Glib::ustring SessionLogStatusText(const Snapshot& snap) const;
  void AppendSessionLogRow(const ctl::LogLine& line);
  void ClearSessionLogRows();
  void ScrollSessionLogToBottom();
  void CopySessionLog();
  void SaveSessionLog();
  // Exactly what is on screen, plus a banner naming which half of the split
  // this is. Nothing is synthesized into it — a saved log the app invented
  // lines into is not a log.
  std::string ComposeSessionLogText() const;

  // actions (submit to the bridge; outcome reported AFTER the fact)
  void RunAction(Action action, const Glib::ustring& described,
                 const std::string& exitClientId = {});
  void RunFaultAction(Fault fault, const std::string& clientId);
  void RunShuffleExits();
  void RunProbeSuite(bool start);
  void EditSettings(std::function<void(urnet::ReliabilitySettings&)> mutate);
  void OnBoolToggled(size_t uiIndex);
  void OnNumChanged(size_t uiIndex);

  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);  // stale-async guard
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  bool built_ = false;
  bool advanced_ = false;   // seeded false; the shell pushes the real value
  bool selected_ = false;
  bool presenting_ = false;
  bool wide_ = false;
  bool applying_ = false;   // echo guard: true while a snapshot writes controls
  std::atomic<bool> pollPending_{false};
  sigc::connection pollTimer_;

  // skeleton (built in the ctor; cards land in the hosts on EnsureBuilt)
  Gtk::ScrolledWindow scroller_;
  GtkWidget* clampWidget_ = nullptr;  // AdwClamp: the capped, centred column
  Gtk::Box column_{Gtk::Orientation::VERTICAL, 0};
  Gtk::Box topStack_{Gtk::Orientation::VERTICAL, 16};
  Gtk::Box tablesStack_{Gtk::Orientation::VERTICAL, 16};
  Gtk::Box columnsRow_{Gtk::Orientation::HORIZONTAL, 0};
  Gtk::Box mainStack_{Gtk::Orientation::VERTICAL, 16};
  Gtk::Box sideHostWide_{Gtk::Orientation::VERTICAL, 0};
  Gtk::Box narrowHost_{Gtk::Orientation::VERTICAL, 0};
  Gtk::Box sideStack_{Gtk::Orientation::VERTICAL, 16};

  // intro card
  Gtk::Label* connectHint_ = nullptr;
  Gtk::Label* lastAction_ = nullptr;
  Gtk::Label* updateCheckText_ = nullptr;
  Gtk::Button* simulateBtn_ = nullptr;
  Gtk::Button* syncBtn_ = nullptr;
  Gtk::Button* checkUpdatesBtn_ = nullptr;

  // the two INDEPENDENT visibility groups (spec §2.5)
  std::vector<Gtk::Widget*> deviceCards_;    // visible iff snapshot.haveDevice
  std::vector<Gtk::Widget*> settingsCards_;  // visible iff settings in force

  // measurements
  std::vector<MetricRowUi> metricRows_;
  Gtk::Label* noFailuresLine_ = nullptr;

  // tables: identity-keyed rebuild state. optional<string> so the FIRST apply
  // of an empty list still rebuilds (creates the empty-state row).
  Gtk::Box* exitsBody_ = nullptr;
  std::optional<std::string> exitsIdentity_;
  std::vector<ExitRowUi> exitRows_;
  Gtk::Box* destinationsBody_ = nullptr;
  std::optional<std::string> destinationsIdentity_;
  std::vector<LabelRowUi> destinationRows_;
  Gtk::Box* probeBody_ = nullptr;
  std::optional<std::string> probeIdentity_;
  std::vector<LabelRowUi> probeRows_;
  Gtk::Label* probeState_ = nullptr;
  Gtk::Button* probeStartBtn_ = nullptr;
  Gtk::Button* probeStopBtn_ = nullptr;

  // ---- session log ---------------------------------------------------------
  // NOT in deviceCards_/settingsCards_: this card is visible in EVERY state,
  // including no-session, because a refused bring-up is exactly when its
  // content matters.
  Gtk::ScrolledWindow* logScroller_ = nullptr;
  Gtk::Box* logBody_ = nullptr;         // the rows; own overflow, never widens the page
  Gtk::Label* logStatusLine_ = nullptr; // Loading / Empty / Failed / Refused, in words
  Gtk::Label* logGapLine_ = nullptr;    // "N lines were dropped before this point"
  Gtk::CheckButton* logFollow_ = nullptr;
  std::deque<ctl::LogLine> logBuffer_;  // UI thread only; capped at kLogTailClientCap
  std::deque<Gtk::Widget*> logRows_;    // the rendered tail of logBuffer_
  int64_t logDroppedTotal_ = 0;         // ACCUMULATED: the reply only reports its own gap
  bool logRestartNoted_ = false;        // the daemon restarted under us; said in words
  bool logFollowing_ = true;            // auto-scroll; self-disables on a scroll up
  bool applyingFollow_ = false;         // echo guard for the Follow check button
  bool scrollingToBottom_ = false;      // ...and for the programmatic scroll
  // bridge thread only: urnet::version() is asked for once, lazily, because the
  // page can be constructed before the SDK is initialized.
  bool logSdkVersionSet_ = false;

  // The GUI-side log transport. Declared BEFORE bridge_ so it is destroyed
  // AFTER it: the destructor body joins the bridge first, so no fetch can be
  // inside this object when it dies.
  LogTailClient logTail_;

  // override rows
  std::vector<BoolRowUi> boolRows_;
  std::vector<NumRowUi> numRows_;

  // LAST member: destroyed FIRST, and Shutdown() runs in the dtor body, so no
  // bridge job can touch a half-destroyed page.
  Bridge bridge_;
};

}  // namespace urnw
