// The connect drawer (port of the apple ConnectActions + ConnectStatsSections):
// a vertical column under the connect controls with
//   1. the insufficient-balance banner (ContractStatus.InsufficientBalance and
//      not Pro), opening the upgrade flow,
//   2. the connection controls card (selected location read-only, Connection
//      Mode Auto|Web|Streaming, Fixed IP, Strong Anonymization — all wired to
//      the device performance profile),
//   3. the three stats cards (Client statistics with the remote + blocked
//      charts, Local statistics with the local chart + split-rule count,
//      Custom DNS status), each opening its detail sheet,
//   4. the "Block ads and trackers" switch card, and
//   5. the plan + usage card (apple ConnectActions:169-204): the Guest/Free/
//      Pro plan label, the used/pending/available usage bar with the daily
//      balance + referral rows, "Get UR Pro" into the upgrade sheet, and the
//      redeem-balance-code entry point. The Linux app has no separate Account
//      surface yet, so this card is the balance home.
// All SDK access goes through the SdkHost accessors; change events arrive via
// OnHostEvent on the GTK main loop. Handles the no-device (tunnel down) state
// gracefully: charts/lists show their empty states and the toggles read/write
// the persisted local state. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include <gtkmm.h>

#include "ContractsSheet.hpp"
#include "DnsSheet.hpp"
#include "RedeemCodeSheet.hpp"
#include "SdkHost.hpp"
#include "SplitRulesSheet.hpp"
#include "SubscriptionBalance.hpp"
#include "TransferChart.hpp"
#include "UpgradeSheet.hpp"
#include "UsageBar.hpp"

namespace urnw {

class ConnectDrawer : public Gtk::Box {
 public:
  ConnectDrawer(SdkHost& host, Gtk::Window& parent, SubscriptionBalanceStore& balance);

  // SdkHost drawer events, already marshalled onto the GTK main loop.
  void OnHostEvent(DrawerEvent event);
  // Full resync (device lifecycle changes, window re-shown).
  void RefreshAll();

  // Balance store change feed (GTK main loop): plan card + banner + sheets.
  void OnBalanceChanged();
  // ContractStatus.InsufficientBalance from the live stats feed.
  void SetInsufficientBalance(bool insufficient);
  // Opens the upgrade sheet (also the target of the insufficient banner).
  void OpenUpgrade();

  // Guest plan card: route into the guest -> full-account create page.
  std::function<void()> on_create_account;

 private:
  void BuildInsufficientBanner();
  void BuildControlsCard();
  void BuildClientStatsCard();
  void BuildLocalStatsCard();
  void BuildDnsCard();
  void BuildBlockerCard();
  void BuildPlanCard();

  void RefreshControls();      // selected location + performance profile
  void ApplyControls();        // performance profile <- control states
  void PullThroughput();       // feed the three charts
  void RefreshSplitRuleCount();
  void RefreshDnsCard();
  void RefreshBlocker();
  void RefreshPlanCard();      // plan label + usage bar + banner visibility

  SdkHost& host_;
  Gtk::Window& parent_;
  SubscriptionBalanceStore& balance_;

  // connection controls
  Gtk::Label* locationDot_ = nullptr;
  Gtk::Label* locationName_ = nullptr;
  Gtk::Label* locationCaption_ = nullptr;
  Gtk::ToggleButton* modeAuto_ = nullptr;
  Gtk::ToggleButton* modeWeb_ = nullptr;
  Gtk::ToggleButton* modeStreaming_ = nullptr;
  Gtk::Switch* fixedIpSwitch_ = nullptr;
  Gtk::Switch* anonSwitch_ = nullptr;
  bool updatingControls_ = false;

  // stats cards
  TransferChart* remoteChart_ = nullptr;
  TransferChart* blockChart_ = nullptr;
  TransferChart* localChart_ = nullptr;
  Gtk::Label* splitRuleCount_ = nullptr;

  // custom dns status card
  struct DnsStatusRow {
    Gtk::Label* dot = nullptr;
    Gtk::Label* state = nullptr;
  };
  Gtk::Box* dnsRowsBox_ = nullptr;
  Gtk::Label* dnsUnavailable_ = nullptr;
  DnsStatusRow dnsDohRow_;
  DnsStatusRow dnsUnencryptedRow_;
  DnsStatusRow dnsLocalRow_;
  DnsStatusRow dnsFallbackRow_;

  // blocker
  Gtk::Switch* blockerSwitch_ = nullptr;
  bool updatingBlocker_ = false;

  // insufficient-balance banner + plan card
  Gtk::Box* insufficientBanner_ = nullptr;
  bool insufficientBalance_ = false;
  Gtk::Label* planLabel_ = nullptr;
  Gtk::Button* getProBtn_ = nullptr;
  Gtk::Button* createAccountBtn_ = nullptr;
  UsageBar* usageBar_ = nullptr;

  bool introPlayed_ = false;  // entrance animation runs on the first map only

  // detail sheets (kept alive, hidden on close)
  std::unique_ptr<ContractsSheet> contractsSheet_;
  std::unique_ptr<SplitRulesSheet> splitRulesSheet_;
  std::unique_ptr<DnsSheet> dnsSheet_;
  std::unique_ptr<RedeemCodeSheet> redeemSheet_;
  std::unique_ptr<UpgradeSheet> upgradeSheet_;
};

}  // namespace urnw
