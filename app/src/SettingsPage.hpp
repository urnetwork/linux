// SettingsPage — the SETTINGS destination of the Windows-parity shell
// (windows SettingsPage.{h,cpp} + SettingsSheets.cpp; spec
// docs/parity/settings.md). "Preferences in three columns": three EQUAL
// full-bleed panes separated by 1px rules, every row code-built from the kit
// so the whole destination is one row species per list.
//
//   Pane A "General" — what the app DOES: the General group (product updates,
//     automatic update checks) then the Connections group (kill switch + its
//     two honesty disclosures, blocked locations, app split rules, the VPN
//     service row).
//   Pane B "Device" — what this machine IS: the Device group (name, spec),
//     Post Quantum Identity, then Advanced (the advanced-mode toggle and
//     Save logs).
//   Pane C "About" — what the app IS: the version rows and Stay in touch.
//
// Neither the account-subject sections (security / referrals / plan / danger)
// nor Sign out live here: R4 moved them to the ACCOUNT destination's hosts.
// This page owns General/Connections/Device/PQI/Advanced/About only.
//
// THREE INVARIANTS THIS PAGE IS BUILT AROUND:
//
//  1. The advanced-mode toggle is THE ONE WRITER and it ONLY WRITES. It calls
//     SdkHost::SetAdvancedMode (persist FIRST, publish SECOND) and never
//     applies anything itself; the standing value comes back through the
//     host's handler into SetAdvancedMode(bool) below — the SAME path a
//     disk-restored value takes, so toggle-now and on-at-launch cannot render
//     differently. An echo guard keeps that apply from re-entering the
//     handler as a user edit.
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

namespace urnw {

// Reused sheets (docs/parity/linux-reuse.md): the split-rules editor and the
// provider-identities list already exist in this tree and are opened as-is.
class SplitRulesSheet;
class ProviderIdentitiesSheet;
// Built for this destination, file-local to SettingsPage.cpp (spec §6.1/§6.2).
class SettingsDeviceNameSheet;
class SettingsBlockedLocationsSheet;

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
  // No-op when the switch already reads `on`; otherwise written under the echo
  // guard so the apply cannot echo back out through SdkHost as a user edit.
  void SetAdvancedMode(bool on);

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

 private:
  // ---- construction --------------------------------------------------------
  void BuildGeneralSection(Gtk::Box& host);
  void BuildConnectionsSection(Gtk::Box& host);
  void BuildDeviceSection(Gtk::Box& host);
  void BuildIdentitySection(Gtk::Box& host);
  void BuildAdvancedSection(Gtk::Box& host);
  void BuildVersionSection(Gtk::Box& host);
  void BuildStayInTouchSection(Gtk::Box& host);

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
  void OnAdvancedModeToggled();
  void SaveLogsToFile();
  void ConfirmUninstallService();

  // ---- sheets --------------------------------------------------------------
  Gtk::Window* RootWindow();  // the transient parent, resolved lazily
  void ShowDeviceNameSheet();
  void ShowAppSplitRulesSheet();
  void ShowIdentitySheet();

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

  // ---- Pane A: General -----------------------------------------------------
  Gtk::Switch* productUpdates_ = nullptr;
  Gtk::Label* productUpdatesState_ = nullptr;
  Gtk::Widget* productUpdatesStateRow_ = nullptr;  // hidden when the line is empty
  bool applyingPreference_ = false;  // echo guard: the load writes IsOn
  bool preferencesLoaded_ = false;   // the toggle is inert until the value is known
  Gtk::Switch* autoCheckUpdates_ = nullptr;

  // ---- Pane A: Connections -------------------------------------------------
  // Local preference (prefs::kConnectOnLaunchKey), no echo guard: sole writer.
  Gtk::Switch* connectOnLaunch_ = nullptr;
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
  bool applyingAdvancedMode_ = false;  // re-entrancy guard on the apply path

  // ---- sheets --------------------------------------------------------------
  std::unique_ptr<SettingsDeviceNameSheet> deviceNameSheet_;
  std::unique_ptr<SettingsBlockedLocationsSheet> blockedSheet_;
  std::unique_ptr<SplitRulesSheet> splitRulesSheet_;
  std::unique_ptr<ProviderIdentitiesSheet> identitiesSheet_;
  std::unique_ptr<Gtk::Window> confirmDialog_;  // the uninstall confirmation
};

}  // namespace urnw
