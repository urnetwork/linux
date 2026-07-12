// "Redeem Code" sheet (port of the apple RedeemBalanceCodeSheet): a balance
// code is a 26-character secret purchased at ur.io; the sheet validates the
// length, redeems through Api::redeemBalanceCode, shows the invalid state
// inline, and on success flips to a confirmation and re-polls the balance
// (SubscriptionBalanceStore::StartConfirmationPolling, the same bridge-the-
// webhook-gap poll the upgrade flow uses).
//
// Below the entry, the redeemed-codes history (the apple Balance Codes screen
// / the Windows Account panel list): Api::getNetworkRedeemedBalanceCodes ->
// masked secret, +data, redeemed date, expiry. Linux has no Account surface,
// so the history lives here; refreshed on open and after a successful redeem.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtkmm.h>

#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"

namespace urnw {

class RedeemCodeSheet : public Gtk::Window {
 public:
  RedeemCodeSheet(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance);

  void Open();  // reset to a fresh entry state and present

 private:
  void BuildUi();
  void Redeem();
  void SetRedeeming(bool redeeming);
  void RefreshCodes();  // Api::getNetworkRedeemedBalanceCodes -> the history list

  SdkHost& host_;
  SubscriptionBalanceStore& balance_;
  bool redeeming_ = false;
  // invalidates in-flight redeem/list callbacks after the sheet is reset/closed
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Box* entryBox_ = nullptr;    // the code form
  Gtk::Box* successBox_ = nullptr;  // the redeemed confirmation
  Gtk::Entry* codeEntry_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
  Gtk::Button* redeemBtn_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  Gtk::ScrolledWindow* codesScroller_ = nullptr;  // caps a long history's height
  Gtk::Grid* codesGrid_ = nullptr;                // code / data / redeemed / expires rows
  Gtk::Label* codesEmpty_ = nullptr;              // "No balance codes found"
};

}  // namespace urnw
