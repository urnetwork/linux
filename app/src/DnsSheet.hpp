// "Custom DNS" editor sheet (port of the apple DnsSettingsView). Edits a
// draft copy of the device dns resolver settings; Update applies the whole
// draft via setDnsResolverSettings and closes. Sections: the regional
// recommendation / most-secure default panel, the four resolver switches, the
// local-dns-fallback switch, the well-known regional server suggestions, and
// the four editable server lists (IPv4 + IPv6 sublists with validation).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

class DnsSheet : public Gtk::Window {
 public:
  DnsSheet(Gtk::Window& parent, SdkHost& host);

  // Loads the current settings into a fresh draft. Returns false (and does
  // not present) when no settings are available.
  bool Open();

  // Field-wise equality of two resolver settings (nullopt server lists compare
  // equal to empty ones). The canonical comparison for "are these settings the
  // same" — the connect drawer's Custom DNS pill reuses it to decide whether the
  // applied settings match the regional recommendation / safe defaults.
  static bool SettingsEqual(const urnet::DnsResolverSettings& a,
                            const urnet::DnsResolverSettings& b);

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

  void BuildUi();
  Gtk::Box* AddSwitchRow(Gtk::Box& parent, const std::string& title, const std::string& detail,
                         Gtk::Switch*& outSwitch, bool urnet::DnsResolverSettings::*flag);
  void AddListSection(Gtk::Box& parent, const std::string& title, const std::string& placeholder,
                      bool doh, std::optional<urnet::StringList> urnet::DnsResolverSettings::*v4,
                      std::optional<urnet::StringList> urnet::DnsResolverSettings::*v6);
  void BuildSuggestions();

  void SyncFromDraft();                // widgets <- draft (guarded)
  void RebuildListRows(size_t index);  // one sublist's value rows
  void RebuildRemoteDnsRows();         // the sublist backing the suggestions
  void SyncSuggestionRow(SuggestionRow& row);
  void RebuildPanel();
  void RefreshDirty();
  void AddListValue(size_t index);

  static void Normalize(urnet::DnsResolverSettings& settings);
  std::string CountryDotMarkup(const std::string& countryCode) const;

  SdkHost& host_;
  urnet::DnsResolverSettings draft_;
  urnet::DnsResolverSettings original_;
  std::optional<urnet::DnsResolverSettings> recommendation_;
  std::optional<urnet::DnsResolverSettings> defaults_;
  std::string countryCode_;  // lowercased connected country code
  std::string countryName_;
  bool updating_ = false;  // guards switch handlers during widget sync

  Gtk::Switch* remoteDohSwitch_ = nullptr;
  Gtk::Switch* localDohSwitch_ = nullptr;
  Gtk::Switch* remoteDnsSwitch_ = nullptr;
  Gtk::Switch* localDnsSwitch_ = nullptr;
  Gtk::Switch* fallbackSwitch_ = nullptr;
  Gtk::Box* panelBox_ = nullptr;
  Gtk::Box* suggestionsSection_ = nullptr;
  Gtk::Box* suggestionsBox_ = nullptr;
  std::vector<SuggestionRow> suggestionRows_;
  std::vector<ListField> listFields_;
  Gtk::Button* updateBtn_ = nullptr;
};

}  // namespace urnw
