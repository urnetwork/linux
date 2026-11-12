// Claims through URnetwork's claim relay on the Earnings page (apple
// ClaimRelaySheet.swift; the approved mocks' 4a to 4e). URnetwork claims every
// consented network's settlement v2 balance and pays the gas: by itself once a
// balance is worth the automatic threshold, and on Claim now, at most once in a
// period for each coldkey. Small amounts wait and say why. The device's own
// claim with its gas key stays as the fallback (the v2 claim sheet).
//
// The SDK decides all of it (SnClaimStatus, SnClaimNow on the device): who
// claims, why a balance waits, whether Claim now can act and why not, whether
// the device's own claim is there, and whether URnetwork's claim is the latest
// (ShowsClaimed, with its transaction, amount and coldkey: 4d). These views
// only word it.
//
// 4d's banner is the store's one sentence for it on every app,
// sn_claim_relay_claimed ("Claimed by URnetwork. 0.8420 SN25α is on your key,
// 5F3s…"), drawn in AppendClaimedBanner.
//
// Capture the flag (sn CAPTURETHEFLAG.md §10): Claim now claims every pool of
// the coldkey, its pool's account and its capture the flag account, and the
// SDK's status sums them. The sheet lists each account under the total, as
// Android does (snledger::RelayRowsFor), one below the claim minimum saying so,
// alone or not, and a pool the server could not read as unavailable, never
// zero. With none of the coldkey's pools read there is no total, only those
// rows (snledger::RelayTotalKnown).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

#include "SnLedgerPresentation.hpp"

namespace urnw {

// The answer to a Claim now tap (the SDK's SnClaimNowResult).
struct ClaimNowOutcome {
  enum class Kind {
    None,       // not asked yet
    Requested,  // taken: URnetwork sends it first, within the day's budget
    Refused,    // not taken: `reason` (SnClaimNowReason* or SnClaimReason*), `nextAtMillis`
    Failed,     // `message`
  };
  Kind kind = Kind::None;
  std::string reason;
  int64_t nextAtMillis = 0;
  std::string message;
};

// URnetwork is the one claiming: it is, or it will once a balance or a new day
// allows. Otherwise the device's own claim is the way for now.
bool ClaimRelayClaims(const urnet::SnClaimStatusResult& status);

// The relay's part of the Settlement v2 tile (4a, 4e): who claims, the pending
// pill with why, and Claim now with its note, or, while the relay doesn't
// claim, Claim now off and the device's own claim. Once URnetwork's claim is
// the latest, the claim with its transaction, and when Claim now is available
// again.
void AppendClaimRelaySection(Gtk::Box& into, const urnet::SnClaimStatusResult& status,
                             const std::function<void()>& claimNow,
                             const std::function<void()>& selfClaim,
                             const std::function<void(const std::string& url)>& openLink);

// The transaction link of URnetwork's claim: the SDK's explorer page, when it
// names a web page. "" when there is none to open.
std::string ClaimTransactionLink(const urnet::SnClaimStatusResult& status);

// The name over a pool's row in the Settlement v2 tile and the claim sheets:
// Capture the flag, Provider pool or Pool {no_id} (snledger::PoolName); "" for
// none.
Glib::ustring LedgerPoolName(snledger::PoolName pool, int64_t noId);

// The pools the server could not read just now, as the SDK lists them (its
// unavailable_pools); none for a whole answer.
std::vector<snledger::UnavailablePool> UnavailablePoolsOf(
    const std::optional<urnet::SnLedgerUnavailablePoolList>& pools);

// The relay's accounts, as its per-pool rows read them.
std::vector<snledger::RelayAccount> RelayAccountsOf(const urnet::SnClaimStatusResult& status);

// The relay's per-pool rows (snledger::RelayRowsFor on Claim now's sheet,
// snledger::RelayTileRowsFor on the tile): each pool by name, with its coldkey
// while the rows are of more than one, its amount, and Pending, Below the claim
// minimum, Claimed with its transaction, or Unavailable right now in place of
// an amount. Without `pendingPill` a Pending row says nothing of it, which the
// relay's part under the tile says with why (Android's tile rows). Nothing for
// no rows.
void AppendRelayRows(Gtk::Box& into, const std::vector<snledger::RelayRow>& rows,
                     const std::function<void(const std::string& url)>& openLink,
                     bool pendingPill = true);

// Claim now's sheet (4b, 4c, 4d): the balance, where it lands, why it waits,
// and Claim now or why it can't act, with the device's own claim as the
// fallback; or URnetwork's claim with its transaction, Done, and when Claim now
// is available again; with the claim sheet's disclaimer.
class ClaimRelaySheet : public Gtk::Window {
 public:
  explicit ClaimRelaySheet(Gtk::Window& parent);

  void Apply(const std::optional<urnet::SnClaimStatusResult>& status,
             const ClaimNowOutcome& outcome, bool claimingNow, const std::string& fundsDisclaimer);

  std::function<void()> on_claim_now;
  std::function<void()> on_self_claim;
  std::function<void(const std::string& url)> on_open_link;

 private:
  Gtk::Box* content_ = nullptr;
  Gtk::Box* disclaimer_ = nullptr;
  Gtk::Button* close_ = nullptr;
};

}  // namespace urnw
