// "Transports" editor sheet (port of the apple TransportSettingsView, presented
// like DnsSheet). Edits a draft copy of the device transport policy -- one
// carrier, or Auto with a per-carrier enable -- and Update applies the whole
// draft via setTransportSettings (setProviderTransportSettings for the
// provider kind) and closes. Sections: the transport mode (Auto / H3 / H1 /
// whodis / whodis pump as check rows with a color dot and a one-line
// description), the enable switches under Auto (in the SDK order; the last
// enabled carrier's switch is shown disabled), and "Restore default
// transports" while the draft is not the SDK default.
//
// The draft is an SDK policy value edited ONLY through the SDK's by-value
// helpers (urnet::transportSettingsWithMode / WithAutoModeEnabled; dirty via
// urnet::transportSettingsEqual; the mode list from
// urnet::selectableTransportModes), so the editing rules -- a newly enabled
// carrier takes its default priority, the last enabled carrier can't be
// disabled, the fixed preference order -- live in one place for every platform.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

class TransportSheet : public Gtk::Window {
 public:
  // Which device policy the sheet edits: the client policy (the carrier this
  // device uses to reach providers) or the provider policy (the carrier it
  // uses while providing for others).
  enum class Kind { Client, Provider };

  TransportSheet(Gtk::Window& parent, SdkHost& host, Kind kind);

  // Loads the current policy into a fresh draft (the SDK default when the
  // device has none yet) and presents.
  void Open();

 private:
  struct ModeRow {
    std::string mode;  // urnet::TransportModeAuto or a selectable mode
    Gtk::Image* check = nullptr;
  };
  struct AutoRow {
    std::string mode;
    Gtk::Switch* toggle = nullptr;
    Gtk::Image* constrained = nullptr;
  };

  void BuildUi();
  Gtk::Widget* MakeModeRow(const std::string& mode);
  void SyncFromDraft();  // widgets <- draft (guarded)
  void RefreshDirty();
  std::optional<urnet::TransportSettings> DefaultSettings() const;
  std::optional<urnet::TransportSettings> CurrentSettings() const;
  bool IsAuto() const;

  SdkHost& host_;
  Kind kind_;
  std::optional<urnet::TransportSettings> draft_;
  std::optional<urnet::TransportSettings> original_;
  std::optional<urnet::TransportStatus> status_;
  urnet::StringList modes_;  // the selectable modes, SDK order
  bool updating_ = false;    // guards switch handlers during widget sync

  std::vector<ModeRow> modeRows_;
  std::vector<AutoRow> autoRows_;
  Gtk::Box* autoSection_ = nullptr;
  Gtk::Box* degradedNotice_ = nullptr;
  Gtk::Label* degradedText_ = nullptr;
  Gtk::Box* restoreSection_ = nullptr;
  Gtk::Button* updateBtn_ = nullptr;
};

}  // namespace urnw
