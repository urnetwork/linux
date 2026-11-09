// The "Custom DNS" sheet (the apple DnsSettingsView's port), with the
// third-party DNS presets of connect ADBLOCK3RD.md F1 and F2. Under the
// connection region row and the "Traffic DNS" heading come the picks the sdk
// lists (GetDnsPicks), in its order: URnetwork (default), the preset groups,
// Custom, and the exit country's Regional pick. A default, preset or regional
// pick saves at once; a personal preset ("Use my account") saves the profile id
// typed in the field under it; Custom opens today's editor -- the four resolver
// switches, the regional server suggestions and the four lists -- which Update
// saves. Under the selected preset: what its provider receives, the resolver's
// status and verification with "Use URnetwork DNS" and "Change location", and
// this connection's blocked lookups, read again every few seconds while the
// sheet shows (the count changes with no callback). Fast DNS on connect saves
// at once too. A selection a release dropped shows its notice above the picks.
//
// Every save starts from one of the sdk's helpers on the settings the sheet was
// seeded from (DnsResolverSettingsWithPreset, WithFallback, AsCustom), so the
// selection and every field this app does not model survive it, and every save
// is a compare-and-set through SdkHost (apple's editor safety): written only
// while the resolver still reads what the sheet was seeded from, else the sheet
// re-seeds from what it reads now. What a selection and a status say is the
// sdk's (DescribeDnsResolverSettings, DescribeDnsResolverStatus), in the words
// of the key ids they carry (DnsPresetsPresentation.hpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "ConnectionRegionSheet.hpp"
#include "SdkHost.hpp"

namespace urnw {

class DnsSheet : public Gtk::Window {
 public:
  DnsSheet(Gtk::Window& parent, SdkHost& host);
  ~DnsSheet() override;

  // Loads the current settings and presents. Returns false (and does not
  // present) when no settings are available.
  bool Open();

  // Field-wise equality of two resolver settings, the selection (PresetId and
  // PresetPersonalId) included (nullopt server lists compare equal to empty
  // ones). The canonical comparison for "are these settings the same": the
  // compare-and-set of every save, and the Connect page's change detection.
  static bool SettingsEqual(const urnet::DnsResolverSettings& a,
                            const urnet::DnsResolverSettings& b);

  // The Connect page relays these while the sheet shows: the resolver's status
  // changed (DrawerEvent::DnsStatus, or a device came or went), and the
  // settings changed (DrawerEvent::DnsSettings), which re-seeds the sheet
  // unless Custom or the profile id field holds an edit.
  void RefreshStatus();
  void RefreshSettings();

  // "Change location" under a failing status: the window's location chooser.
  std::function<void()> on_change_location;

 private:
  // one editable IPv4/IPv6 sublist of a server-list section
  struct ListField {
    std::optional<urnet::StringList> urnet::DnsResolverSettings::*field = nullptr;
    bool doh = false;  // https-url validation instead of ip validation
    Gtk::Box* rowsBox = nullptr;
    Gtk::Entry* entry = nullptr;
    Gtk::Button* addBtn = nullptr;
  };
  struct SuggestionRow {
    std::string ipv4;
    Gtk::Switch* toggle = nullptr;
  };
  // What a re-seed does with an edit in progress: a pick or a profile id
  // saved drops it; Fast DNS on connect, saved beside it, keeps it.
  enum class Edits { Drop, Keep };
  // What a written save does: a pick stays on the sheet to show what it
  // selected, Custom's Update closes it as the editor always did.
  enum class AfterWrite { Stay, Close };

  void BuildUi();
  Gtk::Box* AddSwitchRow(Gtk::Box& parent, const std::string& title, const std::string& detail,
                         Gtk::Switch*& outSwitch, bool urnet::DnsResolverSettings::*flag);
  void AddListSection(Gtk::Box& parent, const std::string& title, const std::string& placeholder,
                      bool doh, std::optional<urnet::StringList> urnet::DnsResolverSettings::*v4,
                      std::optional<urnet::StringList> urnet::DnsResolverSettings::*v6);
  void BuildSuggestions();

  void SyncFromDraft();                // the Custom editor's widgets <- draft (guarded)
  void RebuildListRows(size_t index);  // one sublist's value rows
  void RebuildRemoteDnsRows();         // the sublist backing the suggestions
  void SyncSuggestionRow(SuggestionRow& row);
  void RefreshDirty();
  void AddListValue(size_t index);

  static void Normalize(urnet::DnsResolverSettings& settings);
  std::string CountryDotMarkup(const std::string& countryCode) const;
  // The country's name for a regional pick: the connection's, the locale's,
  // or its code.
  std::string CountryName(const std::string& countryCode) const;

  // The connection region row (ingress dns), apart from the draft.
  void RefreshConnectionRegion();
  void OpenConnectionRegion();
  Gtk::Button* connectionRegionBtn_ = nullptr;
  std::unique_ptr<ConnectionRegionSheet> connectionRegionSheet_;

  // ---- the picks ------------------------------------------------------------
  // Seeds the sheet from `settings` (Open, a write, a change the device
  // reported): the picks, the selection's lines and the Custom draft.
  void Seed(urnet::DnsResolverSettings settings, Edits edits);
  // The picks' rows, under the selection and anything tapped that needs more.
  void RebuildPicks();
  Gtk::Widget* BuildPickRow(const urnet::DnsPick& pick, Gtk::CheckButton*& group, bool active);
  void BuildPersonalPanel(Gtk::Box& details, const urnet::DnsPreset& preset);
  void BuildStatusPanel(Gtk::Box& details);
  // Whether the settings the sheet was seeded from select `pick`.
  bool IsSelection(const urnet::DnsPick& pick) const;
  // A pick tapped: one that needs nothing more saves; Custom and a personal
  // preset open what they need first.
  void OnPick(const urnet::DnsPick& pick);
  // Saves a default, preset or regional pick (DnsResolverSettingsWithPreset).
  void SelectPick(const std::string& presetId);
  void SavePersonalId(const std::string& presetId);
  void ShowPersonalError(const std::string& errorId,
                         const std::optional<urnet::DnsPreset>& preset);
  void SaveFallback(bool enableFallback);
  // The one compare-and-set every save goes through.
  void Write(urnet::DnsResolverSettings next, AfterWrite after, Edits edits);
  // Runs `action` once the signal that asked for it has returned: an action
  // that re-seeds rebuilds the picks, and the row whose control asked must not
  // be destroyed from inside its own signal.
  void Defer(std::function<void()> action);
  // Whether Custom's draft or the profile id field holds an edit the
  // settings do not.
  bool CustomEdited() const;
  bool PersonalEdited() const;

  // ---- the status under the selected preset -----------------------------------
  void RenderStatus();
  void RenderBlockedCount();
  void StartBlockedCountPoll();
  void StopBlockedCountPoll();

  SdkHost& host_;
  // the settings the sheet was seeded from: every save's base, and what its
  // compare-and-set checks the resolver still reads
  urnet::DnsResolverSettings original_;
  // Custom's draft, and what it was seeded with (the settings as custom)
  urnet::DnsResolverSettings draft_;
  urnet::DnsResolverSettings customSeed_;
  std::optional<urnet::DnsSelectionSummary> summary_;
  urnet::DnsPickList picks_;
  // the device's dns resolver status: nullopt with no device (tunnel down)
  std::optional<urnet::DnsResolverStatus> status_;
  std::optional<urnet::DnsStatusSummary> statusSummary_;
  int64_t blockedCount_ = 0;
  sigc::connection blockedCountPoll_;
  std::string countryCode_;  // lowercased connected country code
  std::string countryName_;
  bool updating_ = false;  // guards switch handlers during widget sync
  bool syncing_ = false;   // guards the picks' toggles while they are built
  // a pick tapped that needs more before it saves: Custom (Update) or a
  // personal preset (its profile id)
  bool pendingCustom_ = false;
  std::string pendingPersonal_;
  // the profile id field's text, kept across a rebuild of its row
  std::string personalInput_;

  // fixed widgets
  Gtk::ScrolledWindow* scroller_ = nullptr;
  Gtk::Box* droppedBanner_ = nullptr;
  Gtk::Label* droppedText_ = nullptr;
  Gtk::Label* picksError_ = nullptr;
  Gtk::Box* picksBox_ = nullptr;      // the picks through Custom
  Gtk::Box* customBox_ = nullptr;     // Custom's editor, under its pick
  Gtk::Box* tailPicksBox_ = nullptr;  // the picks after Custom (Regional)
  Gtk::Label* fastDnsPresetNote_ = nullptr;
  Gtk::Box* actionBar_ = nullptr;     // Custom's Update
  // the rebuilt rows' widgets, reset with each rebuild
  Gtk::CheckButton* firstPresetRadio_ = nullptr;
  Gtk::Entry* personalEntry_ = nullptr;
  Gtk::Label* personalErrorLine_ = nullptr;
  Gtk::Box* statusBox_ = nullptr;
  Gtk::Label* statusLine_ = nullptr;
  Gtk::Label* statusDetail_ = nullptr;
  Gtk::Label* verificationLine_ = nullptr;
  Gtk::Box* statusActions_ = nullptr;
  Gtk::Button* useDefaultBtn_ = nullptr;
  Gtk::Button* changeLocationBtn_ = nullptr;
  Gtk::Label* blockedCountLine_ = nullptr;
  Gtk::Label* blockedFloorNote_ = nullptr;

  Gtk::Switch* remoteDohSwitch_ = nullptr;
  Gtk::Switch* localDohSwitch_ = nullptr;
  Gtk::Switch* remoteDnsSwitch_ = nullptr;
  Gtk::Switch* localDnsSwitch_ = nullptr;
  Gtk::Switch* fallbackSwitch_ = nullptr;
  Gtk::Box* suggestionsSection_ = nullptr;
  Gtk::Box* suggestionsBox_ = nullptr;
  std::vector<SuggestionRow> suggestionRows_;
  std::vector<ListField> listFields_;
  Gtk::Button* updateBtn_ = nullptr;
};

}  // namespace urnw
