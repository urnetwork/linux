// SettingsPage — the SETTINGS destination of the Windows-parity shell
// (windows SettingsPage.{h,cpp} + SettingsSheets.cpp; spec
// docs/parity/settings.md). "Preferences in three columns": three EQUAL
// full-bleed panes separated by 1px rules, every row code-built from the kit
// so the whole destination is one row species per list.
//
//   Pane A "General" — what the app DOES: the General group (product updates,
//     automatic update checks, a check now with the last check's outcome) then
//     the Connections group (kill switch + its
//     two honesty disclosures, blocked locations, VLESS, app split rules, the
//     VPN service row).
//   Pane B "Device" — what this machine IS: the Device group (name, spec),
//     Post Quantum Identity, then Advanced (the advanced-mode toggle and
//     Save logs).
//   Pane C "About" — what the app IS: the version rows, Licenses, and Stay
//     in touch.
//
// A foldable pane owns no content without a second door (SettingsFold.hpp):
// while About is folded an "About" group at the foot of pane A carries the
// version rows, Licenses and Stay in touch (the default window is narrower
// than About's gate, the attributions some licenses require must never
// depend on the window being wide, and the DePIN Hub and protocol links have
// no other door), and while Device is folded an "Advanced" group after it
// carries the advanced-mode toggle. Exactly one copy of each shows.
//
// Neither the account-subject sections (security / referrals / plan / danger)
// nor Sign out live here: R4 moved them to the ACCOUNT destination's hosts.
// This page owns General/Connections/Device/PQI/Advanced/About only.
//
// THREE INVARIANTS THIS PAGE IS BUILT AROUND:
//
//  1. The advanced-mode toggle is the one writer and it only writes (both of
//     its doors, pane B's and pane A's fold copy). It calls
//     SdkHost::SetAdvancedMode (persist first, publish second) and never
//     applies anything itself; the standing value comes back through the
//     host's handler into SetAdvancedMode(bool) below — the SAME path a
//     disk-restored value takes, so toggle-now and on-at-launch cannot render
//     differently. One echo guard keeps that apply, which writes both, from
//     re-entering the handler as a user edit.
//  2. Every async field terminates in exactly one of six states (FieldState):
//     NoSession / NoDevice / Loading / Loaded / Empty / Failed. NoDevice
//     exists because "signed in but the service is not up" must not say
//     "please login" — that is a lie. A dash is never an answer.
//  3. The kill switch reads BACK rather than trusting a write: it is the one
//     toggle where a wrong state costs privacy. Kill switch == !routeLocal;
//     the inversion lives in SdkHost, never in this view.
//
// Fold table (windows MainWindow::ApplyBreakpoint): >= 1400 dip three panes;
// 900..1399 About folds; < 900 Device folds too and only General remains.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <gtkmm.h>

#include "PaneKit.hpp"
#include "SdkHost.hpp"
#include "Ui.hpp"
#include "UpdateChecker.hpp"

namespace urnw {

// Reused sheets (docs/parity/linux-reuse.md): the split-rules editor and the
// provider-identities list already exist in this tree and are opened as-is.
class SplitRulesSheet;
class ProviderIdentitiesSheet;
// Built for this destination, file-local to SettingsPage.cpp (spec §6.1/§6.2).
class SettingsDeviceNameSheet;
class SettingsBlockedLocationsSheet;
class LicensesSheet;
// The VLESS editor (VlessSheet.hpp); the login screen's network sheet opens
// its own instance for the same space.
class VlessSheet;
// The apps excluded from the tunnel through urnetwork-exclude
// (ExcludeAppsSheet.hpp).
class ExcludeAppsSheet;

// §2.1 — the six terminal states of every async field on this destination and
// its sheets. NoDevice is NOT a nicety: "signed in but the service is not up"
// rendered as "please login" is a lie, and a dash cannot distinguish nothing
// from not-loaded from failed. Namespace scope because the page and its two
// sheets all terminate in it.
enum class SettingsFieldState { NoSession, NoDevice, Loading, Loaded, Empty, Failed };

class SettingsPage : public Gtk::Box {
 public:
  explicit SettingsPage(SdkHost& host);
  ~SettingsPage() override;

  // nav-select + auth-change API loads (windows LoadSettings, which fires on
  // navigation to Settings AND to Account). Bumps the stale-async epoch, then:
  // local device state with no round trips, and — only with a session — the
  // preferences and device-info reads. With no session every server-backed
  // field lands on NoSession and NOTHING is left on a spinner. This no-session
  // branch IS the Settings-owned half of windows ResetForSignOut (the account
  // half lives on the Account destination).
  void Load();

  // The D5 apply path: MainWindow's advanced-mode handler calls this with the
  // standing value (bind-then-replay, so a disk-restored true is never lost).
  // Writes both toggles under the echo guard so the apply cannot echo back
  // out through SdkHost as a user edit.
  void SetAdvancedMode(bool on);

  // The window's updater (UpdateChecker.hpp). The auto-check toggle writes
  // its preference through it, Check now asks it for a check, and every
  // snapshot it publishes is replayed into the line under Check now (the last
  // check's outcome) and the "Update available" row: Install / Relaunch for
  // the AppImage, the release page plus the package-manager command for every
  // other install. Bind-then-replay, like the advanced-mode handler.
  void SetUpdateChecker(UpdateChecker* checker);
  void ApplyUpdate(const UpdateChecker::Snapshot& snap);

  // The spec's pane-fold table (1400 / 900 dip). A folded pane is hidden
  // together with its rule — never left as a zero-width column.
  void ApplyBreakpoint(int widthDip);

  // PUBLIC because the Network destination's detail pane is a SECOND door to
  // the same sheet; the blocked list is a network-API read this page owns.
  void ShowBlockedLocationsSheet();

  // Feed slots the window's DrawerEvent dispatcher forwards (already
  // marshalled onto the GTK loop). RouteLocal keeps the kill switch honest
  // when it is changed from the connect surface; ProviderIdentities cascades
  // into the identities sheet while it is open.
  // TODO(sdk-wiring): SdkHost::SetSettingsObserver — a dedicated slot so this
  // page does not ride the drawer feed once the host grows one.
  void OnRouteLocalEvent();
  void OnProviderIdentitiesEvent();

  // The snackbar surface belongs to the shell (windows binds SettingsPage's
  // snackbar_ to the SUPPORT destination's InfoBar — a flagged quirk this port
  // deliberately fixes by emitting to the window-level bar). error=true is the
  // persistent treatment (Warning/Error stay until dismissed).
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;
  // "Notify me when my internet is slow" changed (already saved): the window
  // applies it to the notification at once.
  std::function<void(bool on)> on_slow_internet_notify;
  // Device > Manage devices: the window's device list (ManageDevicesSheet),
  // where a removal frees a slot of the network's concurrent client limit.
  std::function<void()> on_open_manage_devices;

 private:
  // ---- construction --------------------------------------------------------
  void BuildGeneralSection(Gtk::Box& host);
  void BuildConnectionsSection(Gtk::Box& host);
  void BuildDeviceSection(Gtk::Box& host);
  void BuildIdentitySection(Gtk::Box& host);
  void BuildAdvancedSection(Gtk::Box& host);
  void BuildAdvancedFoldSection(Gtk::Box& host);
  Gtk::Switch* AddAdvancedModeToggle(Gtk::Box& host);
  // The version rows and Stay in touch, each built twice like Licenses.
  void BuildVersionSection(Gtk::Box& host);
  void BuildStayInTouchSection(Gtk::Box& host);
  // The "Licenses" row (built twice: in About, and in pane A's fold copy).
  void AddLicensesRow(Gtk::Box& host);

  // ---- loads ---------------------------------------------------------------
  // No round trips: client id off the device, kill switch off LocalState.
  // Runs at build time and on every Load().
  void ApplyLocalDeviceState();
  void LoadPreferences();  // accountPreferencesGet -> the product-updates toggle
  void LoadDeviceInfo();   // getNetworkClients -> this client's name + spec

  // ---- handlers ------------------------------------------------------------
  void OnProductUpdatesToggled();
  void OnKillSwitchToggled();
  // The one writer for the "what is actually in force" line under the switch.
  // Deliberately never touches the switch: the switch is the request.
  void ApplyKillSwitchState();
  void OnAdvancedModeToggled(Gtk::Switch& source);
  void SaveLogsToFile();
  void ConfirmUninstallService();

  // ---- sheets --------------------------------------------------------------
  Gtk::Window* RootWindow();  // the transient parent, resolved lazily
  void ShowDeviceNameSheet();
  void ShowAppSplitRulesSheet();
  void ShowExcludeAppsSheet();
  void ShowIdentitySheet();
  void ShowLicensesSheet();
  void ShowVlessSheet();

  // ---- helpers -------------------------------------------------------------
  void Snack(const Glib::ustring& message, bool error);

  SdkHost& host_;
  // stale-async guard: bumped by Load() and the destructor; a completion
  // carrying an older value is dropped before it touches the page
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  // ---- the pane shell ------------------------------------------------------
  kit::Pane paneA_;  // General
  kit::Pane paneB_;  // Device
  kit::Pane paneC_;  // About
  Gtk::Widget* ruleB_ = nullptr;  // A | B
  Gtk::Widget* ruleC_ = nullptr;  // B | C
  // three EQUAL columns: a horizontal size group pins the panes to one
  // request, hexpand then splits the remainder evenly between them
  Glib::RefPtr<Gtk::SizeGroup> paneSizes_;
  int lastFold_ = -1;  // 3 / 2 / 1 panes; -1 = never applied
  // pane A's second doors: the About group (version rows, Licenses) while
  // pane C is folded, the Advanced group (the toggle) while pane B is
  Gtk::Box* aboutFoldHost_ = nullptr;
  Gtk::Box* deviceFoldHost_ = nullptr;

  // ---- Pane A: General -----------------------------------------------------
  Gtk::Switch* productUpdates_ = nullptr;
  Gtk::Label* productUpdatesState_ = nullptr;
  Gtk::Widget* productUpdatesStateRow_ = nullptr;  // hidden when the line is empty
  bool applyingPreference_ = false;  // echo guard: the load writes IsOn
  bool preferencesLoaded_ = false;   // the toggle is inert until the value is known
  Gtk::Switch* autoCheckUpdates_ = nullptr;
  // Launch URnetwork on system startup (LaunchAtStartup.hpp): the user's
  // autostart entry, so no session gate and no pref; the row hides without
  // the template.
  Gtk::Switch* launchAtStartup_ = nullptr;
  Gtk::Widget* launchAtStartupRow_ = nullptr;
  bool applyingLaunchAtStartup_ = false;  // echo guard
  void ApplyLaunchAtStartup();
  void OnLaunchAtStartupToggled();
  // Check now, and the state line under it that says what the last check came
  // to (hidden while there is nothing to say); one writer, ApplyUpdateState.
  Gtk::Button* checkNow_ = nullptr;
  Gtk::Label* updateState_ = nullptr;
  Gtk::Widget* updateStateRow_ = nullptr;
  sigc::connection holdEnd_;  // re-reads the line when GitHub's hold ends
  void ApplyUpdateState(const UpdateChecker::Snapshot& snap);
  // The update notice (hidden until a newer release is known): a two-line row
  // whose action verb follows the checker's phase, plus a selectable prose
  // row carrying the command or the saved file's path.
  UpdateChecker* updates_ = nullptr;
  UpdateChecker::Snapshot updateSnapshot_;  // what the button acts on
  Gtk::Widget* updateRow_ = nullptr;
  Gtk::Label* updateTitle_ = nullptr;
  Gtk::Label* updateNote_ = nullptr;
  Gtk::Button* updateButton_ = nullptr;
  Gtk::Widget* updateCommandRow_ = nullptr;
  Gtk::Label* updateCommand_ = nullptr;
  void OnUpdateButton();
  void OpenLink(const std::string& url);

  // ---- Pane A: Connections -------------------------------------------------
  // Local preference (prefs::kConnectOnLaunchKey), no echo guard: sole writer.
  Gtk::Switch* connectOnLaunch_ = nullptr;
  // Local preference (slow_internet::kNotifyPrefKey), the same: sole writer.
  Gtk::Switch* slowInternetNotify_ = nullptr;
  Gtk::Switch* killSwitch_ = nullptr;
  bool applyingKillSwitch_ = false;  // echo guard
  // What is REALLY in force (SdkHost::KillSwitchStatus), rendered under the
  // switch. Hidden while the switch is off — there is nothing to disclose.
  Gtk::Label* killSwitchState_ = nullptr;
  Gtk::Widget* killSwitchStateRow_ = nullptr;
  // the WHOLE row hides together (a caption pointing at a hidden button is
  // worse than no row at all)
  Gtk::Box* serviceRowHost_ = nullptr;
  Gtk::Button* serviceUninstall_ = nullptr;

  // ---- Pane B: Device ------------------------------------------------------
  kit::PaneTwoLineRowButton deviceNameRow_;
  Gtk::Label* deviceSpecValue_ = nullptr;
  std::string clientId_;    // "" with no device
  std::string deviceName_;  // cached for the edit sheet's prefill

  // ---- Pane B: Advanced ----------------------------------------------------
  Gtk::Switch* advancedMode_ = nullptr;
  Gtk::Switch* advancedModeFold_ = nullptr;  // pane A's copy (deviceFoldHost_)
  bool applyingAdvancedMode_ = false;  // re-entrancy guard on the apply path, both toggles

  // ---- sheets --------------------------------------------------------------
  std::unique_ptr<SettingsDeviceNameSheet> deviceNameSheet_;
  std::unique_ptr<SettingsBlockedLocationsSheet> blockedSheet_;
  std::unique_ptr<SplitRulesSheet> splitRulesSheet_;
  std::unique_ptr<ProviderIdentitiesSheet> identitiesSheet_;
  std::unique_ptr<LicensesSheet> licensesSheet_;
  std::unique_ptr<VlessSheet> vlessSheet_;
  std::unique_ptr<ExcludeAppsSheet> excludeAppsSheet_;
  std::unique_ptr<Gtk::Window> confirmDialog_;  // the uninstall confirmation
};

}  // namespace urnw
