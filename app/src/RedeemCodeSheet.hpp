// "Redeem Code" sheet (port of the apple RedeemBalanceCodeSheet): a balance
// code is a 26-character secret purchased at ur.io; the sheet gates on the
// SDK's urnet::isBalanceCodeFormatValid, redeems through
// Api::redeemBalanceCode, answers from the SDK's classification
// (BalanceCodeRedeem.hpp: already redeemed / invalid / unknown inline), and
// on success flips to a confirmation of what the code added (its data, or a
// gift code's "A year of Pro is yours." or the day a queued year starts) and
// reads the balance once. Either is on the network when the redeem answers, so
// it does not use the upgrade flow's Pro confirmation poll. A gift code the
// network already holds, found in its redeemed list after a lost or refused
// answer, confirms as the gift. The entry has room for a code in groups of
// four, as a gift card prints it.
//
// Below the entry, the redeemed-codes history (the apple Balance Codes screen
// / the Windows Account panel list): Api::getNetworkRedeemedBalanceCodes ->
// masked secret, +data (or a gift code's "A year of Pro"), redeemed date,
// expiry. Linux has no Account surface, so the history lives here; refreshed
// on open and after a successful redeem.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtkmm.h>

#include <functional>

#include "BalanceCodeRedeem.hpp"
#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"

namespace urnw {

class RedeemCodeSheet : public Gtk::Window {
 public:
  RedeemCodeSheet(Gtk::Window& parent, SdkHost& host, SubscriptionBalanceStore& balance);

  void Open();  // reset to a fresh entry state and present

  // The network has the code, redeemed now or earlier: the window refreshes
  // the Account page's redeemed-codes pane (AccountPage::LoadBalanceCodes), so
  // the code shows there without a full page load.
  std::function<void()> on_redeemed;

 private:
  void BuildUi();
  void Redeem();
  // the sheet's answer: urnet::classifyBalanceCodeRedeem's outcome, after the
  // redeemed-code list when the redeem did not credit
  // (BalanceCodeRedeemNoticeAfterList)
  void ShowRedeemOutcome(BalanceCodeRedeemNotice notice,
                         const std::optional<urnet::RedeemBalanceCodeResult>& result);
  void SetRedeeming(bool redeeming);
  void RefreshCodes();  // Api::getNetworkRedeemedBalanceCodes -> the history list

  SdkHost& host_;
  SubscriptionBalanceStore& balance_;
  bool redeeming_ = false;
  // invalidates in-flight redeem/list callbacks after the sheet is reset/closed
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  Gtk::Box* entryBox_ = nullptr;    // the code form
  Gtk::Box* successBox_ = nullptr;  // the redeemed confirmation
  Gtk::Label* successAmount_ = nullptr;  // "+5 GiB", or a gift code's "A year of Pro"
  Gtk::Entry* codeEntry_ = nullptr;
  Gtk::Label* errorLabel_ = nullptr;
  Gtk::Button* redeemBtn_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  Gtk::ScrolledWindow* codesScroller_ = nullptr;  // caps a long history's height
  Gtk::Grid* codesGrid_ = nullptr;                // code / data / redeemed / expires rows
  Gtk::Label* codesEmpty_ = nullptr;              // successful fetch, no codes
  Gtk::Label* codesError_ = nullptr;              // fetch FAILED (distinct from empty)
};

}  // namespace urnw
