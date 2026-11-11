// The device list (owner, 2026-11-08: the linux app at parity with apple,
// android and windows): the network's devices and hosted proxies, each
// removable but this one, so the user frees a slot of the concurrent client
// limit for this device. Opened from the client limit notice's Manage devices
// (a Pro plan), from Settings > Device, and for a sign-in the limit refused. The
// list is the sdk's ManageDevicesViewController, which SdkHost opens
// (OpenManageDevices) on the bound device when there is one, so a removal that
// frees a slot ends the device's hold at once, else on the Api, and for a
// refused sign-in on that sign-in's token, so a removal runs its registration
// again. This sheet renders the snapshots (ManageDevicesPresentation.hpp
// decides the words) and asks for a refresh or a removal; a removal asks first,
// inside the sheet, in the list's place. Above the list it says the network's
// Pro features in use when the snapshot has a prompt (ProFeaturesNotice.hpp;
// owner decision 2026-11-11), with Get Pro, the window's upgrade path.
//
// Threading is SessionsPage's: the list publishes on its own goroutines, the
// update is posted to the main loop and dropped unless the sheet is alive and
// still on the open it came from (alive_, epoch_). Everything else runs on the
// main loop, and every call into SdkHost returns at once.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <gtkmm.h>

#include "ManageDevicesPresentation.hpp"
#include "SdkHost.hpp"

namespace urnw {

class ManageDevicesSheet : public Gtk::Window {
 public:
  ManageDevicesSheet(Gtk::Window& parent, SdkHost& host);
  ~ManageDevicesSheet() override;

  // Opens the list and presents the sheet. `forSignIn`: the list of the
  // sign-in the client limit refused, with `message` (what the refusal said)
  // above it. A second Open replaces the first.
  void Open(bool forSignIn, const Glib::ustring& message);
  bool ForSignIn() const { return forSignIn_; }
  bool IsOpen() const { return get_visible(); }
  // What a retry of the refused sign-in answered, above the list: refused
  // again, or another failure.
  void SetMessage(const Glib::ustring& message);
  // Takes the sheet down: the sign-in it was opened for went through, or the
  // account it lists signed out. The list closes with it.
  void Dismiss();

  // The Pro features notice's Get Pro: the window's upgrade path. The sheet
  // hides first, so the upgrade comes up over the window rather than beside
  // this modal sheet.
  std::function<void()> on_get_pro;
  // The sheet hid, however it hid: the window reads the network's proxies
  // again (a removal may have taken a SOCKS or WireGuard proxy with it).
  std::function<void()> on_closed;

 private:
  void BuildUi();
  void Apply(const SdkHost::ManageDevicesUpdate& update);
  void Render();
  manage_devices::Words MakeWords() const;
  // The confirmation, in the list's place, and its two answers.
  void AskRemove(const manage_devices::Device& device);
  void ConfirmRemove();
  void CancelRemove();
  // The sheet hid, by Done, Escape, the close button or Dismiss: the list
  // closes, and a snapshot still on its way is dropped.
  void OnHidden();

  SdkHost& host_;
  bool forSignIn_ = false;
  // marshaled work's liveness and the open it belongs to
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Label* message_ = nullptr;
  // the network's Pro features in use, above the list: the prompt's title and
  // body, and Get Pro (none for a refused sign-in's list)
  Gtk::Box* proFeaturesBox_ = nullptr;
  Gtk::Label* proFeaturesTitle_ = nullptr;
  Gtk::Label* proFeaturesBody_ = nullptr;
  Gtk::Button* proFeaturesGetPro_ = nullptr;
  // the list: the help, the state line with its Retry, the spinner, the rows
  Gtk::Box* listBox_ = nullptr;
  Gtk::Label* statusText_ = nullptr;
  Gtk::Button* retryButton_ = nullptr;
  Gtk::Spinner* loading_ = nullptr;
  Gtk::Box* rowsBox_ = nullptr;
  // the confirmation
  Gtk::Box* confirmBox_ = nullptr;
  Gtk::Label* confirmBody_ = nullptr;
  std::optional<manage_devices::Device> confirming_;
  manage_devices::Snapshot snapshot_;
  bool canManage_ = true;
};

}  // namespace urnw
