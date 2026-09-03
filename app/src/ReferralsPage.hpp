// ReferralsPage — the "Refer and earn" page, reached from the Account
// destination's Referrals row (the android/apple Referrals screen; windows
// ReferralsPage). One pane:
//
//   1. the SHARED referral pieces (ReferralPanel.hpp): the referral progress
//      card and the gold king-frog panel — the same two the onboarding
//      "Refer friends" step shows, so the wording, the bonus figure and the
//      crowned state cannot drift between the two surfaces. The code with copy
//      and share lives in the panel.
//   2. the figures: total referrals and the referral points earned
//      (payout_linked_account).
//   3. the referral network: who referred THIS network, set or unlinked in
//      the sheet that used to live on Account's pane B.
//
// Every referral number comes from the balance store (the server's referral
// terms: cap and bonus GiB/day) — nothing here hardcodes the bonus. The page
// repaints from the store on every balance change; the API reads it owns
// (points, referral network) follow the Account page's six-state contract and
// its 20 s watchdog (AccountFlow), for the same reasons.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <gtkmm.h>
#include <urnetwork_sdk.hpp>

#include "AccountPage.hpp"  // AccountFlow + AccountFieldState (the shared contract)
#include "PaneKit.hpp"
#include "ReferralRoyalty.hpp"
#include "SdkHost.hpp"
#include "SubscriptionBalance.hpp"

namespace urnw {

class ReferralPanel;
class ReferralProgressBox;
class ReferralNetworkSheet;

class ReferralsPage : public Gtk::Box {
 public:
  ReferralsPage(SdkHost& host, SubscriptionBalanceStore& balance);
  ~ReferralsPage() override;

  // nav-select + auth-change load: the card from the store, the points and the
  // referral network from the API. Bumps the stale-async epoch first.
  void Load();
  // The store's referral code / count / terms moved: repaint the card and the
  // total. Called from the window's balance relay.
  void OnBalanceChanged();
  // Drop everything that described the signed-out network.
  void ResetForSignOut();
  // --preview-ui: no API reads; every field settles on its no-session state.
  void SetPreviewMode(bool on);
  void ShowPreviewState();

  std::function<void()> on_back;  // the header's "‹ Account"
  std::function<bool()> sheet_open;
  std::function<void(bool open)> on_sheet_open_changed;
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;

 private:
  void BuildPane();
  bool CanCallApi();
  ReferralTerms Terms() const;
  void ApplyCard();
  void ApplyTotal(AccountFieldState state);
  void LoadPoints();
  void ApplyPoints(AccountFieldState state, double points);
  void LoadReferralNetwork();
  void ApplyReferralNetworkValue(AccountFieldState state, const std::string& name);
  void SettleNoSession();

  Gtk::Window* RootWindow();
  bool BeginSheet(const char* what);
  void EndSheet();
  void WireSheet(Gtk::Window& sheet);
  void ShowReferralNetworkSheet();

  SdkHost& host_;
  SubscriptionBalanceStore& balance_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  kit::Pane pane_;
  ReferralProgressBox* progress_ = nullptr;
  ReferralPanel* panel_ = nullptr;
  Gtk::Label* totalValue_ = nullptr;
  Gtk::Label* pointsValue_ = nullptr;
  kit::PaneTwoLineRowButton referralNetworkRow_;

  AccountFlow pointsFlow_;
  AccountFlow referralNetworkFlow_;
  bool previewMode_ = false;
  bool sheetShowing_ = false;
  std::unique_ptr<ReferralNetworkSheet> referralSheet_;
};

}  // namespace urnw
