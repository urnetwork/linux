// The connect drawer (port of the apple ConnectActions + ConnectStatsSections):
// a vertical column under the connect controls with
//   1. the insufficient-balance banner (ContractStatus.InsufficientBalance and
//      not Pro), opening the upgrade flow,
//   2. the connection controls card (selected location read-only, Connection
//      Mode Auto|Web|Streaming, Fixed IP, Strong Anonymization, Post Quantum
//      Encryption — all wired to the device performance profile; always a
//      profile, window type "auto" for Auto, so the orthogonal toggles persist
//      in every mode — plus the Kill switch, the inverted device routeLocal,
//      which is not part of the profile),
//   3. the three stats cards (Client statistics with the remote chart, the
//      transport distribution bar (opening the transport settings editor) and
//      the blocked chart, Local statistics with the local chart + split-rule
//      count, Custom DNS status), each opening its detail sheet,
//   4. the "Block ads and trackers" switch card, and
//   5. the plan + usage card (apple ConnectActions:169-204): the Guest/Free/
//      Pro plan label, the used/pending/available usage bar with the daily
//      balance + referral rows, "Get UR Pro" into the upgrade sheet, and the
//      redeem-balance-code entry point. The Linux app has no separate Account
//      surface yet, so this card is the balance home, and
//   6. the Post Quantum Identity panel (apple account tab parity): this
//      device's identity key identicon/hash/client id, the verified provider
//      identity deck, the provider identities list and the share dialog.
// All SDK access goes through the SdkHost accessors; change events arrive via
// OnHostEvent on the GTK main loop. Handles the no-device (tunnel down) state
// gracefully: charts/lists show their empty states and the toggles read/write
// the persisted local state. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <gtkmm.h>

#include "ContractsSheet.hpp"
#include "DnsSheet.hpp"
#include "LocationsSheet.hpp"
#include "PostQuantumIdentity.hpp"
#include "RedeemCodeSheet.hpp"
#include "SdkHost.hpp"
#include "SplitRulesSheet.hpp"
#include "SubscriptionBalance.hpp"
#include "TransferChart.hpp"
#include "TransportBar.hpp"
#include "TransportSheet.hpp"
#include "UpgradeSheet.hpp"
#include "UsageBar.hpp"

namespace urnw {

class ConnectDrawer : public Gtk::Box {
 public:
  ConnectDrawer(SdkHost& host, Gtk::Window& parent, SubscriptionBalanceStore& balance);
  // Bumps epoch_ so a kill-switch completion still in flight against the
  // daemon cannot land on a destroyed drawer.
  ~ConnectDrawer() override;

  // SdkHost drawer events, already marshalled onto the GTK main loop.
  void OnHostEvent(DrawerEvent event);
  // Full resync (device lifecycle changes, window re-shown).
  void RefreshAll();
  // The daemon's real DNS verdict (StatusReply::dns_applied / dns_detail),
  // pushed in by MainWindow's 5s daemon poll. The drawer cannot fetch this
  // itself: ControlClient::Status() is a blocking socket round-trip and this
  // widget refreshes off SDK change events.
  void SetTunnelDnsState(bool connected, bool applied, const Glib::ustring& detail);

  // Balance store change feed (GTK main loop): plan card + banner + sheets.
  void OnBalanceChanged();
  // ContractStatus.InsufficientBalance from the live stats feed.
  void SetInsufficientBalance(bool insufficient);
  // Opens the upgrade sheet (also the target of the insufficient banner).
  void OpenUpgrade();

  // Opens the location/provider chooser (owned here); called from the drawer's
  // location row and from the home screen's peers status line.
  void OpenLocationChooser();

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
  void BuildPqiPanel();  // post quantum identity card (the drawer's bottom card)

  void RefreshControls();      // selected location + performance profile
  void ApplyControls();        // performance profile <- control states
  void PullThroughput();       // feed the three charts + the transport bar
  void RefreshTransportBar();  // the window's transport distribution (client)
  void RefreshSplitRuleCount();
  void RefreshDnsCard();
  // The "unapplied recommended settings" nudge pill atop the Custom DNS card:
  // shown when the applied settings differ from the connected country's regional
  // recommendation (with the country color dot) or, absent one, the safe
  // defaults; hidden when they already match (apple DnsRecommendationPill).
  void RefreshDnsPill(const std::optional<urnet::DnsResolverSettings>& current);
  void RefreshBlocker();
  void RefreshKillSwitch();    // kill switch = !routeLocal
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
  Gtk::Switch* pqeSwitch_ = nullptr;
  bool updatingControls_ = false;
  // kill switch (own handler — not a profile control). The switch is the
  // REQUEST; the note under it is what urnetworkd says is actually in force,
  // and the two can legitimately disagree.
  Gtk::Switch* killSwitch_ = nullptr;
  Gtk::Label* killSwitchNote_ = nullptr;
  bool updatingKillSwitch_ = false;
  // Stale-completion guard for the kill-switch write, which round-trips to the
  // daemon on a worker and can land after this drawer is gone.
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  // stats cards
  TransferChart* remoteChart_ = nullptr;
  TransportBar* transportBar_ = nullptr;  // under the remote chart; opens the transport sheet
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
  // The daemon's ACTUAL DNS verdict, as opposed to the four preference rows
  // above it. Fed by MainWindow's daemon poll; see SetTunnelDnsState.
  Gtk::Label* dnsTunnelState_ = nullptr;
  Gtk::Box* dnsPill_ = nullptr;       // the recommendation nudge capsule (hidden when applied)
  Gtk::Label* dnsPillDot_ = nullptr;  // country-color dot (only for a regional recommendation)
  Gtk::Label* dnsPillText_ = nullptr;
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

  // post quantum identity panel (owns the identities list + share dialog)
  PostQuantumIdentityPanel* pqiPanel_ = nullptr;

  bool introPlayed_ = false;  // entrance animation runs on the first map only

  // detail sheets (kept alive, hidden on close)
  std::unique_ptr<ContractsSheet> contractsSheet_;
  std::unique_ptr<SplitRulesSheet> splitRulesSheet_;
  std::unique_ptr<LocationsSheet> locationsSheet_;
  std::unique_ptr<DnsSheet> dnsSheet_;
  std::unique_ptr<TransportSheet> transportSheet_;  // the client transport policy editor
  std::unique_ptr<RedeemCodeSheet> redeemSheet_;
  std::unique_ptr<UpgradeSheet> upgradeSheet_;
};

}  // namespace urnw
