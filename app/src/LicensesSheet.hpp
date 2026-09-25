// Settings -> About -> Licenses: the open source software and data attributions
// this app includes, with each one's license and any notice it requires.
//
// The list is the SDK's embedded license.yml (urnet::getLicenses with
// URNET_LICENSE_APP_LINUX), so there is no network read and nothing depends on
// a session, a device or the daemon: the sheet works signed out and with the
// service down, which is exactly when someone may go looking for it.
//
// Two pages in one sheet, the list and one entry's detail:
//   list    title, intro, "Data attributions" (kind "data") then "Open source
//           software" (every other kind), one row per entry in the SDK's order:
//           name over "version · spdx", and the entry's NOTICE in full under
//           them when it has one (GeoLite2's attribution is required text and
//           is never trimmed).
//   detail  name, version · spdx, the notice (emphasized), the copyright lines,
//           the "Project page" link, and the full license text in a monospace,
//           selectable, scrollable view.
// Escape on the detail goes back to the list; on the list it closes the sheet.
//
// The first read parses ~570 KB of YAML in the SDK (~10 ms) and then JSON on
// this side, so it runs on a worker thread once per process and every later
// open renders from the cached result.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <vector>

#include <gtkmm.h>

#include "LicensesPresentation.hpp"

namespace urnw {

class LicensesSheet : public Gtk::Window {
 public:
  explicit LicensesSheet(Gtk::Window& parent);
  ~LicensesSheet() override;

  // Presents the sheet on its list page (the first open starts the load).
  void Open();

 private:
  // The load's three outcomes. Failed is not Empty: the SDK returning nothing
  // means the embedded file could not be read, which is a defect to say out
  // loud rather than render as a list with no rows.
  enum class LoadState { Loading, Loaded, Failed };

  void BuildListPage();
  void BuildDetailPage();
  void Load();
  void Render();
  void AppendSection(const Glib::ustring& header, const std::vector<LicenseEntry>& entries);
  void ShowDetail(const LicenseEntry& entry);
  void ShowList();

  // Orphans the worker's completion once the sheet is gone.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  LoadState state_ = LoadState::Loading;
  bool loadInFlight_ = false;
  std::shared_ptr<const LicenseSections> sections_;

  Gtk::Stack stack_;
  // list page
  Gtk::ScrolledWindow* listScroll_ = nullptr;
  Gtk::Box* listRows_ = nullptr;  // the two sections, rebuilt by Render()
  Gtk::Label* status_ = nullptr;  // Loading / Failed; hidden once Loaded
  // detail page
  Gtk::ScrolledWindow* detailScroll_ = nullptr;
  Gtk::Label* detailName_ = nullptr;
  Gtk::Label* detailMeta_ = nullptr;
  Gtk::Box* detailNotice_ = nullptr;  // the emphasized notice block
  Gtk::Label* detailNoticeText_ = nullptr;
  Gtk::Label* detailCopyright_ = nullptr;
  Gtk::Label* detailLink_ = nullptr;
  Gtk::TextView* detailText_ = nullptr;
};

}  // namespace urnw
