// The Settlement v2 tile and its claim sheet on the Earnings page
// (SnLedgerPresentation.hpp): which tile shows in every state of the read
// (loading, unavailable, no account, accounts), with v2 claims configured or
// not and with or without a coldkey; its figures (Claimable, Paid, Earned and
// what a pending root adds); and the claim sheet's rows through a claim (sent,
// claimed with the vault's amount, needs gas, already claimed, another
// failure, a failure about no account, Done), the gas a v2 claim needs (and a
// v1 claim, on the v1 sheet) and how long the page waits for one; and which
// Transaction link of URnetwork's claim the tile and its sheet open. The page and
// the sheets need GTK and the SDK, so their wiring and their English fallbacks
// (which must be the catalog's msgids) are read from the sources and po/en.po.
//
// Capture the flag (sn CAPTURETHEFLAG.md §10): a server before it reads as
// before, but for the claim minimum; beside a capture the flag account each row
// is named by its kind, Claim reads Claim both while both kinds have something
// to claim, and one press claims both; a row below the claim minimum says so,
// alone or beside others (owner, 2026-11-11); an account the gas key could not
// pay for stays claimable; a pool the server could not read is a row of its
// own and hides no other, and leaves the sheet once the SDK's read finds it
// with nothing to claim; the relay's per-pool rows and total, as Android's;
// and a line for every win not claimed yet, which a read that fails keeps. A
// claim in flight is the session's, so it outlives a visit to another page.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "SnLedgerPresentation.hpp"
#include "SnPayoutPresentation.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

using urnw::snledger::Account;
using urnw::snledger::AccountView;
using urnw::snledger::ClaimProgress;
using urnw::snledger::ClaimRow;
using urnw::snledger::Failure;
using urnw::snledger::FailureFor;
using urnw::snledger::Load;
using urnw::snledger::NewestRead;
using urnw::snledger::Phase;
using urnw::snledger::PoolName;
using urnw::snledger::RelayAccount;
using urnw::snledger::RelayRow;
using urnw::snledger::RelayRowsFor;
using urnw::snledger::RelayStatus;
using urnw::snledger::SectionFor;
using urnw::snledger::SectionView;
using urnw::snledger::Status;
using urnw::snledger::UnavailablePool;
using urnw::snledger::ClaimInFlight;
using urnw::snledger::kWinsReadsAfterClaimMs;
using urnw::snledger::Win;
using urnw::snledger::WinsAfterClaim;
using urnw::snledger::WinsLines;
using urnw::snledger::WonLine;
using urnw::snledger::WonLinesFor;
using urnw::snledger::WonSlot;
using urnw::snledger::WonSlotFor;

namespace {

// names each figure by the rao it was formatted from
std::string Tag(int64_t rao) { return "a" + std::to_string(rao); }

Account LedgerAccount(int64_t noId, int64_t index, int64_t finalizedRao, int64_t paidRao,
                      bool pending = false, int64_t pendingRao = 0) {
  Account account;
  account.vault = "0x5a3e0000000000000000000000000000000000a2";
  account.noId = noId;
  account.index = index;
  account.finalizedRao = finalizedRao;
  account.paidRao = paidRao;
  account.claimableRao = finalizedRao - paidRao;
  account.pending = pending;
  account.pendingRao = pendingRao;
  return account;
}

// one pool account: 5.12 α finalized, 1.879 α of it paid
Account OnePool() { return LedgerAccount(1, 42, 5'120'000'000, 1'879'000'000); }

// UR's capture the flag pool, 2^32 + 1
constexpr int64_t kCtfPool = (int64_t{1} << 32) + 1;

// the coldkey's capture the flag account: `claimableRao` finalized, none paid
Account CtfAccount(int64_t claimableRao, int64_t index = 7) {
  Account account = LedgerAccount(kCtfPool, index, claimableRao, 0);
  account.kind = urnw::snledger::kKindCtf;
  return account;
}

UnavailablePool Unreadable(int64_t noId, const char* kind) {
  UnavailablePool pool;
  pool.noId = noId;
  pool.kind = kind;
  return pool;
}

// the transfer floor at the relay's last price: 0.03 α
constexpr int64_t kFloor = 30'000'000;

SectionView Tile(bool hasColdkey, Load load, const std::vector<Account>& accounts,
                 bool configured, bool claiming = false,
                 const std::vector<UnavailablePool>& unavailable = {}, int64_t floorRao = 0) {
  return SectionFor(hasColdkey, load, accounts, unavailable, floorRao, configured, claiming, Tag);
}

// CL-2: the accounts are a coldkey's; with none nothing shows, whatever the read
UR_TEST(SnLedgerNothingShowsWithoutAColdkey) {
  for (const Load load : {Load::Loading, Load::Ready, Load::Unavailable}) {
    for (const bool configured : {false, true}) {
      UR_EXPECT_TRUE(Tile(false, load, {}, configured) == SectionView{});
      UR_EXPECT_TRUE(Tile(false, load, {OnePool()}, configured) == SectionView{});
    }
  }
}

// CL-2, CL-4: before the release configures v2 claims, only balances the ledger
// returned show, and never a Claim
UR_TEST(SnLedgerBeforeActivationOnlyReturnedBalancesShow) {
  UR_EXPECT_TRUE(Tile(true, Load::Loading, {}, false) == SectionView{});
  UR_EXPECT_TRUE(Tile(true, Load::Unavailable, {}, false) == SectionView{});
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {}, false) == SectionView{});
  const SectionView view = Tile(true, Load::Ready, {OnePool()}, false);
  UR_EXPECT_TRUE(view.visible);
  UR_EXPECT_TRUE(view.status == Status::None);
  UR_EXPECT_EQ(1u, view.accounts.size());
  UR_EXPECT_FALSE(view.showClaim);
  UR_EXPECT_FALSE(view.claimEnabled);
}

// CL-2: configured, the tile shows while loading, with Claim off
UR_TEST(SnLedgerLoadingSaysSoWithClaimOff) {
  const SectionView view = Tile(true, Load::Loading, {OnePool()}, true);
  UR_EXPECT_TRUE(view.visible);
  UR_EXPECT_TRUE(view.status == Status::Loading);
  UR_EXPECT_TRUE(view.accounts.empty());
  UR_EXPECT_TRUE(view.showClaim);
  UR_EXPECT_FALSE(view.claimEnabled);
}

// CL-2: a ledger that cannot be read shows "unavailable" and no figure at all,
// never a zero, and keeps Claim disabled
UR_TEST(SnLedgerUnavailableIsNeverZero) {
  const std::vector<Account> none;
  for (const std::vector<Account>& accounts : {none, std::vector<Account>{OnePool()}}) {
    const SectionView view = Tile(true, Load::Unavailable, accounts, true);
    UR_EXPECT_TRUE(view.visible);
    UR_EXPECT_TRUE(view.status == Status::Unavailable);
    UR_EXPECT_TRUE(view.accounts.empty());
    UR_EXPECT_TRUE(view.showClaim);
    UR_EXPECT_FALSE(view.claimEnabled);
  }
}

// CL-2: an answer with no account is a real answer: nothing to claim yet
UR_TEST(SnLedgerNoAccountHasNothingToClaimYet) {
  const SectionView view = Tile(true, Load::Ready, {}, true);
  UR_EXPECT_TRUE(view.visible);
  UR_EXPECT_TRUE(view.status == Status::NothingYet);
  UR_EXPECT_TRUE(view.accounts.empty());
  UR_EXPECT_TRUE(view.showClaim);
  UR_EXPECT_FALSE(view.claimEnabled);
}

// CL-2: Claimable is the claimable, Paid the paid, Earned the finalized
// cumulative; Claim is on with something claimable
UR_TEST(SnLedgerFiguresAreClaimablePaidAndEarned) {
  const SectionView view = Tile(true, Load::Ready, {OnePool()}, true);
  UR_EXPECT_TRUE(view.visible);
  UR_EXPECT_TRUE(view.status == Status::None);
  AccountView expected;
  expected.noId = 1;
  expected.pool = PoolName::None;  // the one pool goes unnamed
  expected.claimable = "a3241000000";
  expected.paid = "a1879000000";
  expected.earned = "a5120000000";
  UR_EXPECT_EQ(1u, view.accounts.size());
  UR_EXPECT_TRUE(view.accounts.front() == expected);
  UR_EXPECT_TRUE(view.showClaim);
  UR_EXPECT_TRUE(view.claimEnabled);
  UR_EXPECT_FALSE(view.claimBoth);
}

// CL-2: a pending root shows as pending: what it adds over the finalized
// cumulative, and only when it adds something
UR_TEST(SnLedgerPendingShowsTheIncreaseAwaitingFinalization) {
  const Account raised = LedgerAccount(1, 42, 5'120'000'000, 1'879'000'000, true, 5'870'000'000);
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {raised}, true).accounts.front().pendingIncrease ==
                 "a750000000");
  const Account level = LedgerAccount(1, 42, 5'120'000'000, 0, true, 5'120'000'000);
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {level}, true).accounts.front().pendingIncrease.empty());
  const Account lower = LedgerAccount(1, 42, 5'120'000'000, 0, true, 5'000'000'000);
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {lower}, true).accounts.front().pendingIncrease.empty());
  const Account notPending = LedgerAccount(1, 42, 5'120'000'000, 0, false, 9'000'000'000);
  UR_EXPECT_TRUE(
      Tile(true, Load::Ready, {notPending}, true).accounts.front().pendingIncrease.empty());
  UR_EXPECT_EQ(int64_t{0}, urnw::snledger::PendingIncreaseRao(notPending));
}

// CL-2: an account only in the pending root shows zeros it really has, the
// pending amount, and Claim off
UR_TEST(SnLedgerAnAccountOnlyInThePendingRoot) {
  const SectionView view =
      Tile(true, Load::Ready, {LedgerAccount(1, 7, 0, 0, true, 2'000'000'000)}, true);
  UR_EXPECT_EQ(1u, view.accounts.size());
  UR_EXPECT_TRUE(view.accounts.front().claimable == "a0");
  UR_EXPECT_TRUE(view.accounts.front().earned == "a0");
  UR_EXPECT_TRUE(view.accounts.front().pendingIncrease == "a2000000000");
  UR_EXPECT_TRUE(view.showClaim);
  UR_EXPECT_FALSE(view.claimEnabled);
}

// CL-4: Claim waits while any claim is in flight (one gas key, one claim at a
// time) and needs something claimable
UR_TEST(SnLedgerClaimWaitsForAClaimInFlight) {
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {OnePool()}, true, false).claimEnabled);
  UR_EXPECT_FALSE(Tile(true, Load::Ready, {OnePool()}, true, true).claimEnabled);
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {OnePool()}, true, true).showClaim);
  const Account paidOut = LedgerAccount(1, 42, 5'120'000'000, 5'120'000'000);
  UR_EXPECT_FALSE(Tile(true, Load::Ready, {paidOut}, true).claimEnabled);
}

// While the v2 claim is in flight (the session's, owner 2026-11-12), Claim
// stays enabled (Android's reopens its claim too, though there Claim follows
// the last read): it opens the claim's sheet on its rows, where Claim waits
// for it, so a page left during the claim and come back to shows it, whatever
// the page's read says meanwhile (loading, unavailable, or the accounts the
// claim already paid). A v1 claim in flight still holds it.
UR_TEST(SnLedgerCtf_ClaimOpensTheV2ClaimInFlight) {
  const Account paidOut = LedgerAccount(1, 42, 5'120'000'000, 5'120'000'000);
  for (const Load load : {Load::Ready, Load::Loading, Load::Unavailable}) {
    const SectionView view =
        SectionFor(true, load, {paidOut}, {}, 0, /*claimConfigured=*/true, /*claiming=*/true, Tag,
                   /*ledgerClaimInFlight=*/true);
    UR_EXPECT_TRUE(view.visible && view.showClaim && view.claimEnabled);
  }
  UR_EXPECT_TRUE(SectionFor(true, Load::Ready, {OnePool()}, {}, 0, true, true, Tag, true).claimEnabled);
  // a v1 claim in flight: the v2 Claim waits, as before
  UR_EXPECT_FALSE(SectionFor(true, Load::Ready, {OnePool()}, {}, 0, true, true, Tag, false).claimEnabled);
  // and without v2 claims configured there is no Claim to open it with
  UR_EXPECT_FALSE(SectionFor(true, Load::Ready, {OnePool()}, {}, 0, false, true, Tag, true).claimEnabled);
}

// CL-2: with more than one pool each account names its pool, and Claim
// counts every account
UR_TEST(SnLedgerSeveralPoolsNameTheirPool) {
  const std::vector<Account> accounts = {LedgerAccount(1, 42, 2'000'000'000, 2'000'000'000),
                                         LedgerAccount(3, 5, 1'000'000'000, 0)};
  const SectionView view = Tile(true, Load::Ready, accounts, true);
  UR_EXPECT_EQ(2u, view.accounts.size());
  UR_EXPECT_EQ(int64_t{1}, view.accounts[0].noId);
  UR_EXPECT_EQ(int64_t{3}, view.accounts[1].noId);
  UR_EXPECT_TRUE(view.accounts[0].pool == PoolName::Numbered);
  UR_EXPECT_TRUE(view.accounts[1].pool == PoolName::Numbered);
  UR_EXPECT_EQ(int64_t{1'000'000'000}, urnw::snledger::ClaimableRao(accounts));
  UR_EXPECT_TRUE(view.claimEnabled);
  UR_EXPECT_FALSE(view.claimBoth);
}

// ---- capture the flag on the tile ------------------------------------------------

// A server before capture the flag sends no kind, which the SDK reads as a pool
// (the Account's default): the tile reads as before: no name over the one
// pool, numbered pools beside each other, Claim on for whatever is claimable
// above the claim minimum, and never Claim both
UR_TEST(SnLedgerCtf_AServerBeforeItReadsAsBefore) {
  for (const int64_t floor : {int64_t{0}, kFloor}) {
    const SectionView one = Tile(true, Load::Ready, {OnePool()}, true, false, {}, floor);
    UR_EXPECT_TRUE(one.status == Status::None);
    UR_EXPECT_EQ(1u, one.accounts.size());
    UR_EXPECT_TRUE(one.accounts.front().pool == PoolName::None);
    UR_EXPECT_FALSE(one.accounts.front().underFloor);
    UR_EXPECT_FALSE(one.accounts.front().unavailable);
    UR_EXPECT_TRUE(one.claimEnabled);
    UR_EXPECT_FALSE(one.claimBoth);
    UR_EXPECT_TRUE(one == Tile(true, Load::Ready, {OnePool()}, true));
    const SectionView two = Tile(true, Load::Ready, {OnePool(), LedgerAccount(3, 5, 700'000'000, 0)},
                                 true, false, {}, floor);
    UR_EXPECT_TRUE(two.accounts[0].pool == PoolName::Numbered);
    UR_EXPECT_TRUE(two.accounts[1].pool == PoolName::Numbered);
    UR_EXPECT_FALSE(two.accounts[1].underFloor);
    UR_EXPECT_TRUE(two.claimEnabled);
    UR_EXPECT_FALSE(two.claimBoth);
  }
}

// the claim minimum holds for every account, alone or beside others, from
// every server (owner, 2026-11-11, as in every app): a lone pool account under
// the floor says so under its figures, which stay, and counts for no Claim,
// so the tile never offers a claim the runtime refuses; without a price
// (floor 0) nothing is under it, and at the floor it is claimable
UR_TEST(SnLedgerCtf_ALoneAccountBelowTheClaimMinimumSaysSo) {
  const Account tiny = LedgerAccount(1, 42, 10'000'000, 0);  // under the 0.03 α floor
  const SectionView lone = Tile(true, Load::Ready, {tiny}, true, false, {}, kFloor);
  UR_EXPECT_EQ(1u, lone.accounts.size());
  UR_EXPECT_TRUE(lone.accounts.front().pool == PoolName::None);  // still unnamed
  UR_EXPECT_TRUE(lone.accounts.front().underFloor);
  UR_EXPECT_TRUE(lone.accounts.front().claimable == "a10000000");
  UR_EXPECT_TRUE(lone.showClaim);
  UR_EXPECT_FALSE(lone.claimEnabled);
  UR_EXPECT_FALSE(lone.claimBoth);
  const SectionView unpriced = Tile(true, Load::Ready, {tiny}, true, false, {}, 0);
  UR_EXPECT_FALSE(unpriced.accounts.front().underFloor);
  UR_EXPECT_TRUE(unpriced.claimEnabled);
  const SectionView at =
      Tile(true, Load::Ready, {LedgerAccount(1, 42, kFloor, 0)}, true, false, {}, kFloor);
  UR_EXPECT_FALSE(at.accounts.front().underFloor);
  UR_EXPECT_TRUE(at.claimEnabled);
  // two operators' pools, one of them small: it says so, and Claim claims the other
  const SectionView two =
      Tile(true, Load::Ready, {OnePool(), LedgerAccount(3, 5, 10'000'000, 0)}, true, false, {}, kFloor);
  UR_EXPECT_FALSE(two.accounts[0].underFloor);
  UR_EXPECT_TRUE(two.accounts[1].underFloor);
  UR_EXPECT_TRUE(two.claimEnabled);
  // beside a pool the server could not read too
  const SectionView partial = Tile(true, Load::Ready, {tiny}, true, false,
                                   {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}, kFloor);
  UR_EXPECT_TRUE(partial.accounts[0].underFloor);
  UR_EXPECT_TRUE(partial.accounts[0].pool == PoolName::Provider);
  UR_EXPECT_FALSE(partial.claimEnabled);
}

// CAPTURETHEFLAG.md §10: the capture the flag account is its own row, Capture
// the flag, and the pool beside it is the Provider pool; both claimable, Claim
// claims both and says so
UR_TEST(SnLedgerCtf_EachAccountIsARowNamedByItsKind) {
  const SectionView view =
      Tile(true, Load::Ready, {OnePool(), CtfAccount(615'000'000)}, true, false, {}, kFloor);
  UR_EXPECT_TRUE(view.status == Status::None);
  UR_EXPECT_EQ(2u, view.accounts.size());
  UR_EXPECT_EQ(int64_t{1}, view.accounts[0].noId);
  UR_EXPECT_TRUE(view.accounts[0].pool == PoolName::Provider);
  UR_EXPECT_EQ(kCtfPool, view.accounts[1].noId);
  UR_EXPECT_TRUE(view.accounts[1].pool == PoolName::Ctf);
  UR_EXPECT_TRUE(view.accounts[1].claimable == "a615000000");
  UR_EXPECT_TRUE(view.accounts[1].earned == "a615000000");
  UR_EXPECT_TRUE(view.claimEnabled);
  UR_EXPECT_TRUE(view.claimBoth);
  // a claim in flight holds Claim, and it keeps its words
  const SectionView claiming =
      Tile(true, Load::Ready, {OnePool(), CtfAccount(615'000'000)}, true, true, {}, kFloor);
  UR_EXPECT_FALSE(claiming.claimEnabled);
  UR_EXPECT_TRUE(claiming.claimBoth);
  // before the release configures v2 claims there is no Claim to word
  UR_EXPECT_FALSE(
      Tile(true, Load::Ready, {OnePool(), CtfAccount(615'000'000)}, false, false, {}, kFloor).claimBoth);
}

// the coldkey's only account may be its capture the flag one: it is still named
UR_TEST(SnLedgerCtf_ALoneCaptureTheFlagAccountIsNamed) {
  const SectionView view = Tile(true, Load::Ready, {CtfAccount(615'000'000)}, true);
  UR_EXPECT_EQ(1u, view.accounts.size());
  UR_EXPECT_TRUE(view.accounts.front().pool == PoolName::Ctf);
  UR_EXPECT_TRUE(view.claimEnabled);
  UR_EXPECT_FALSE(view.claimBoth);  // one kind
}

// a capture the flag account is often below the claim minimum: it says so,
// counts for no Claim, and Claim claims the pool's alone; at or above the floor
// it is claimable; without a price (floor 0) nothing is under it
UR_TEST(SnLedgerCtf_BelowTheClaimMinimumSaysSoAndCountsForNoClaim) {
  const SectionView under =
      Tile(true, Load::Ready, {OnePool(), CtfAccount(12'000'000)}, true, false, {}, kFloor);
  UR_EXPECT_FALSE(under.accounts[0].underFloor);
  UR_EXPECT_TRUE(under.accounts[1].underFloor);
  UR_EXPECT_TRUE(under.accounts[1].claimable == "a12000000");  // the figure stays
  UR_EXPECT_TRUE(under.claimEnabled);
  UR_EXPECT_FALSE(under.claimBoth);
  const SectionView at = Tile(true, Load::Ready, {OnePool(), CtfAccount(kFloor)}, true, false, {}, kFloor);
  UR_EXPECT_FALSE(at.accounts[1].underFloor);
  UR_EXPECT_TRUE(at.claimBoth);
  const SectionView unpriced = Tile(true, Load::Ready, {OnePool(), CtfAccount(12'000'000)}, true);
  UR_EXPECT_FALSE(unpriced.accounts[1].underFloor);
  UR_EXPECT_TRUE(unpriced.claimBoth);
  // the pool's own account may be the small one
  const Account tinyPool = LedgerAccount(1, 42, 12'000'000, 0);
  const SectionView pool =
      Tile(true, Load::Ready, {tinyPool, CtfAccount(615'000'000)}, true, false, {}, kFloor);
  UR_EXPECT_TRUE(pool.accounts[0].underFloor);
  UR_EXPECT_TRUE(pool.claimEnabled);
  UR_EXPECT_FALSE(pool.claimBoth);
  // with everything below it, there is nothing to claim
  const SectionView none =
      Tile(true, Load::Ready, {tinyPool, CtfAccount(12'000'000)}, true, false, {}, kFloor);
  UR_EXPECT_FALSE(none.claimEnabled);
  UR_EXPECT_FALSE(none.claimBoth);
  // nothing claimable is not below anything
  const SectionView paid = Tile(true, Load::Ready,
                                {LedgerAccount(1, 42, 5'000'000, 5'000'000), CtfAccount(615'000'000)},
                                true, false, {}, kFloor);
  UR_EXPECT_FALSE(paid.accounts[0].underFloor);
}

// CAPTURETHEFLAG.md §10: a pool in unavailable_pools is a row of its own,
// unavailable and without a figure, never a zero, and hides none of the others
UR_TEST(SnLedgerCtf_AnUnavailablePoolHidesNoOther) {
  const SectionView ctf = Tile(true, Load::Ready, {OnePool()}, true, false,
                               {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}, kFloor);
  UR_EXPECT_TRUE(ctf.status == Status::None);
  UR_EXPECT_EQ(2u, ctf.accounts.size());
  UR_EXPECT_TRUE(ctf.accounts[0].pool == PoolName::Provider);
  UR_EXPECT_FALSE(ctf.accounts[0].unavailable);
  UR_EXPECT_TRUE(ctf.accounts[0].claimable == "a3241000000");
  AccountView unreadable;
  unreadable.noId = kCtfPool;
  unreadable.pool = PoolName::Ctf;
  unreadable.unavailable = true;
  UR_EXPECT_TRUE(ctf.accounts[1] == unreadable);  // no figure at all
  UR_EXPECT_TRUE(ctf.claimEnabled);  // the pool's account is still claimed
  UR_EXPECT_FALSE(ctf.claimBoth);
  // the pool unreadable: its row comes first, by its pool, and the capture
  // the flag account shows as usual
  const SectionView pool = Tile(true, Load::Ready, {CtfAccount(615'000'000)}, true, false,
                                {Unreadable(1, urnw::snledger::kKindPool)}, kFloor);
  UR_EXPECT_EQ(2u, pool.accounts.size());
  UR_EXPECT_EQ(int64_t{1}, pool.accounts[0].noId);
  UR_EXPECT_TRUE(pool.accounts[0].unavailable);
  UR_EXPECT_TRUE(pool.accounts[0].pool == PoolName::Provider);
  UR_EXPECT_TRUE(pool.accounts[0].claimable.empty());
  UR_EXPECT_FALSE(pool.accounts[1].unavailable);
  UR_EXPECT_TRUE(pool.accounts[1].pool == PoolName::Ctf);
  UR_EXPECT_TRUE(pool.claimEnabled);
}

// with no account read, an unavailable pool is still not "nothing to claim
// yet": whether the coldkey has an account there is not known
UR_TEST(SnLedgerCtf_UnavailableIsNotNothingYet) {
  const SectionView view =
      Tile(true, Load::Ready, {}, true, false, {Unreadable(1, urnw::snledger::kKindPool)});
  UR_EXPECT_TRUE(view.status == Status::None);
  UR_EXPECT_EQ(1u, view.accounts.size());
  UR_EXPECT_TRUE(view.accounts.front().unavailable);
  UR_EXPECT_TRUE(view.accounts.front().pool == PoolName::None);  // the one pool goes unnamed
  UR_EXPECT_FALSE(view.claimEnabled);
  const SectionView both = Tile(true, Load::Ready, {}, true, false,
                                {Unreadable(1, urnw::snledger::kKindPool),
                                 Unreadable(kCtfPool, urnw::snledger::kKindCtf)});
  UR_EXPECT_EQ(2u, both.accounts.size());
  UR_EXPECT_TRUE(both.accounts[0].pool == PoolName::Provider);
  UR_EXPECT_TRUE(both.accounts[1].pool == PoolName::Ctf);
  // before the release configures v2 claims only balances the ledger returned show
  UR_EXPECT_TRUE(Tile(true, Load::Ready, {}, false, false,
                      {Unreadable(1, urnw::snledger::kKindPool)}) == SectionView{});
  // a ledger that cannot be read at all is the tile's Unavailable, as before
  const SectionView whole = Tile(true, Load::Unavailable, {}, true, false,
                                 {Unreadable(1, urnw::snledger::kKindPool)});
  UR_EXPECT_TRUE(whole.status == Status::Unavailable);
  UR_EXPECT_TRUE(whole.accounts.empty());
}

// several operators' pools beside capture the flag stay apart by number, and
// Claim, which then claims three accounts, is no "both" (Android's
// snLedgerClaimsBoth: exactly the two, one of them capture the flag's)
UR_TEST(SnLedgerCtf_SeveralPoolsBesideItStayNumbered) {
  const SectionView view = Tile(true, Load::Ready,
                                {OnePool(), LedgerAccount(3, 5, 1'000'000'000, 0), CtfAccount(615'000'000)},
                                true);
  UR_EXPECT_TRUE(view.accounts[0].pool == PoolName::Numbered);
  UR_EXPECT_TRUE(view.accounts[1].pool == PoolName::Numbered);
  UR_EXPECT_TRUE(view.accounts[2].pool == PoolName::Ctf);
  UR_EXPECT_TRUE(view.claimEnabled);
  UR_EXPECT_FALSE(view.claimBoth);
  // with the third paid out, Claim claims the two again
  const SectionView two = Tile(true, Load::Ready,
                               {OnePool(), LedgerAccount(3, 5, 1'000'000'000, 1'000'000'000),
                                CtfAccount(615'000'000)},
                               true);
  UR_EXPECT_TRUE(two.claimBoth);
}

// ---- the claim sheet -------------------------------------------------------------

// the rows of a sheet over two pools, the second fully paid
ClaimProgress TwoPoolSheet() {
  return ClaimProgress({LedgerAccount(1, 42, 5'120'000'000, 1'879'000'000),
                        LedgerAccount(2, 9, 1'000'000'000, 1'000'000'000),
                        LedgerAccount(3, 5, 700'000'000, 0)},
                       {}, 0);
}

const ClaimRow* RowOf(const ClaimProgress& progress, int64_t noId, int64_t index) {
  for (const ClaimRow& row : progress.Rows()) {
    if (row.noId == noId && row.index == index) return &row;
  }
  return nullptr;
}

// APPS-SPEC.md claim (CL-4): the sheet lists every account with something to claim
UR_TEST(SnLedgerClaimSheetListsWhatCanBeClaimed) {
  const ClaimProgress progress = TwoPoolSheet();
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, 2, 9) == nullptr);
  UR_EXPECT_EQ(int64_t{3'241'000'000}, RowOf(progress, 1, 42)->amountRao);
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->phase == Phase::Claimable);
  UR_EXPECT_EQ(int64_t{3'941'000'000}, progress.ClaimableRao());
  UR_EXPECT_FALSE(progress.Busy());
  UR_EXPECT_FALSE(progress.NeedsGas());
  UR_EXPECT_FALSE(progress.AnyClaimed());
}

// APPS-SPEC.md claim (CL-4): a press sends every row; Sent carries the
// transaction, Confirmed the amount the vault's Claimed event paid
UR_TEST(SnLedgerClaimSentThenConfirmedWithTheAmount) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  UR_EXPECT_TRUE(progress.Busy());
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Sending);
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->phase == Phase::Sending);
  progress.Sent(1, 42, "0xabc1");
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Sent);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->txHash == "0xabc1");
  progress.Confirmed(1, 42, "0xabc1", 3'240'999'999);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimed);
  UR_EXPECT_EQ(int64_t{3'240'999'999}, RowOf(progress, 1, 42)->amountRao);
  UR_EXPECT_TRUE(progress.AnyClaimed());
  UR_EXPECT_EQ(int64_t{0}, progress.ClaimableRao());  // the other row is still sending
  progress.Done();
  UR_EXPECT_FALSE(progress.Busy());
}

// APPS-SPEC.md claim (CL-4): a Confirmed without an amount keeps what was
// claimable, and a Confirmed without a transaction keeps the one Sent gave
UR_TEST(SnLedgerConfirmedWithoutAnAmountKeepsTheClaimable) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  progress.Sent(3, 5, "0xdef2");
  progress.Confirmed(3, 5, "", 0);
  UR_EXPECT_EQ(int64_t{700'000'000}, RowOf(progress, 3, 5)->amountRao);
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->txHash == "0xdef2");
}

// APPS-SPEC.md claim (CL-4), sdk-ctf-v2 §2.1: needs_gas sent nothing for the
// account, which stays claimable, not failed, and turns on the top-up until
// the next press
UR_TEST(SnLedgerNeedsGasTurnsOnTheTopUp) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(1, 42, "needs_gas: the gas key holds 0.004 TAO"));
  UR_EXPECT_TRUE(progress.NeedsGas());
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimable);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->message.empty());
  progress.Done();
  UR_EXPECT_EQ(int64_t{3'941'000'000}, progress.ClaimableRao());
  progress.Start();
  UR_EXPECT_FALSE(progress.NeedsGas());
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Sending);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->message.empty());
}

// APPS-SPEC.md claim rule 4 (CL-4): another claim paid the account first,
// which is success: claimed, nothing to retry
UR_TEST(SnLedgerAlreadyClaimedIsPaid) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(3, 5, "already_claimed: NothingToClaim: another claim paid"));
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->message.empty());
  UR_EXPECT_FALSE(progress.NeedsGas());
  progress.Done();
  UR_EXPECT_EQ(int64_t{3'241'000'000}, progress.ClaimableRao());
}

// APPS-SPEC.md claim (CL-4): any other failure keeps its message for the
// app's words and the row claimable
UR_TEST(SnLedgerOtherFailuresKeepTheMessage) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(1, 42, "claim_failed: TooManyStakingHotkeys"));
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Failed);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->message == "claim_failed: TooManyStakingHotkeys");
  UR_EXPECT_FALSE(progress.NeedsGas());
  progress.Done();
  UR_EXPECT_EQ(int64_t{3'941'000'000}, progress.ClaimableRao());
}

// APPS-SPEC.md claim (CL-4): a failure about no account (noId 0) is the
// sheet's message and leaves every row as it was; needs_gas still turns on
// the top-up
UR_TEST(SnLedgerAFailureAboutNoAccountIsTheSheets) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  UR_EXPECT_FALSE(progress.Failed(0, 0, "ledger_unavailable: unreachable"));
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, 0, 0) == nullptr);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Sending);
  UR_EXPECT_FALSE(progress.NeedsGas());
  UR_EXPECT_FALSE(progress.Failed(0, 0, "needs_gas: the gas key holds nothing"));
  UR_EXPECT_TRUE(progress.NeedsGas());
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimable);
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->phase == Phase::Claimable);
}

// APPS-SPEC.md claim (CL-4): after Done a row the SDK did not claim (nothing it
// verified was claimable) is claimable again, and a claimed one stays claimed
UR_TEST(SnLedgerDoneRestoresRowsTheSdkDidNotClaim) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  progress.Sent(1, 42, "0xabc1");
  progress.Confirmed(1, 42, "0xabc1", 3'241'000'000);
  progress.Done();
  UR_EXPECT_FALSE(progress.Busy());
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(progress, 3, 5)->phase == Phase::Claimable);
  UR_EXPECT_EQ(int64_t{700'000'000}, progress.ClaimableRao());
}

// APPS-SPEC.md claim (CL-4): an account the SDK claims that the sheet did not
// list (the ledger moved since the read) gets its own row
UR_TEST(SnLedgerAnAccountTheSheetDidNotListGetsARow) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  progress.Sent(4, 7, "0x0777");
  UR_EXPECT_EQ(3u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, 4, 7)->phase == Phase::Sent);
  progress.Confirmed(4, 7, "0x0777", 12'000'000);
  UR_EXPECT_EQ(int64_t{12'000'000}, RowOf(progress, 4, 7)->amountRao);
}

// APPS-SPEC.md claim (CL-4): the SDK's stable codes, with or without detail
UR_TEST(SnLedgerFailureCodes) {
  UR_EXPECT_TRUE(FailureFor("needs_gas: the gas key holds 0 TAO") == Failure::NeedsGas);
  UR_EXPECT_TRUE(FailureFor("needs_gas") == Failure::NeedsGas);
  UR_EXPECT_TRUE(FailureFor("already_claimed: NothingToClaim") == Failure::AlreadyClaimed);
  UR_EXPECT_TRUE(FailureFor("ledger_unavailable: pool 4294967297") == Failure::Unavailable);
  // the runtime's refusal of an amount under its floor, as the SDK words it
  UR_EXPECT_TRUE(FailureFor("not_claimable: AmountTooLow: the claim is below the runtime's "
                            "transfer floor") == Failure::UnderFloor);
  UR_EXPECT_TRUE(FailureFor("proof_mismatch: the proof does not verify") == Failure::Other);
  UR_EXPECT_TRUE(FailureFor("not_claimable: below the transfer floor") == Failure::Other);
  UR_EXPECT_TRUE(FailureFor("not_claimable: the ledger lists this account in another vault2") ==
                 Failure::Other);
  UR_EXPECT_TRUE(FailureFor("claim_failed: AmountTooLow") == Failure::Other);
  UR_EXPECT_TRUE(FailureFor("the gas ran out") == Failure::Other);
  UR_EXPECT_TRUE(FailureFor("") == Failure::Other);
}

// APPS-SPEC.md claim rule 3 (CL-4): a v2 claim needs about 0.02 TAO in the gas
// key, so below that the sheet shows the top-up and holds Claim; so does a
// claim that came back needs_gas; an unknown balance holds nothing
UR_TEST(SnLedgerAV2ClaimNeedsItsGasLimitInTheGasKey) {
  using urnw::snledger::kClaimGasTao;
  using urnw::snledger::NeedsGas;
  UR_EXPECT_TRUE(kClaimGasTao == 0.02);
  UR_EXPECT_TRUE(NeedsGas(false, true, 0.0));
  UR_EXPECT_TRUE(NeedsGas(false, true, 0.005));  // the v1 sheet's old top-up is not enough
  UR_EXPECT_TRUE(NeedsGas(false, true, 0.0199));
  UR_EXPECT_FALSE(NeedsGas(false, true, 0.02));
  UR_EXPECT_FALSE(NeedsGas(false, true, 0.5));
  UR_EXPECT_FALSE(NeedsGas(false, false, 0.0));
  UR_EXPECT_TRUE(NeedsGas(true, true, 0.5));
  UR_EXPECT_TRUE(NeedsGas(true, false, 0.0));
}

// APPS-SPEC.md claim (CL-4): the page gives up on a claim only past the SDK's
// bounds, 2 minutes to read and 6 per account sent alone in turn, plus a
// minute, so it never offers Claim again while the SDK still sends. Two or
// more accounts go in one batch first, 6 minutes to prepare and send it and 6
// for its receipt, after which each account the batch did not reach is sent
// alone. The SDK reads the ledger again, so an unavailable pool may hold one
// more account by then
UR_TEST(SnLedgerTheWatchdogOutlastsEveryClaimTheSdkSends) {
  using urnw::snledger::ClaimWatchdogMs;
  constexpr int kMinute = 60'000;
  UR_EXPECT_EQ(9 * kMinute, ClaimWatchdogMs({}, {}));
  UR_EXPECT_EQ(9 * kMinute, ClaimWatchdogMs({OnePool()}, {}));
  // the paid-out pool sends nothing: two accounts, the batch's 12 minutes and
  // each alone
  UR_EXPECT_EQ(27 * kMinute, ClaimWatchdogMs({LedgerAccount(1, 42, 5'120'000'000, 1'879'000'000),
                                              LedgerAccount(2, 9, 1'000'000'000, 1'000'000'000),
                                              LedgerAccount(3, 5, 700'000'000, 0)},
                                             {}));
  UR_EXPECT_EQ(27 * kMinute, ClaimWatchdogMs({OnePool(), CtfAccount(615'000'000)}, {}));
  // the capture the flag pool unreadable at the read: its account may be
  // claimed too, so it counts as one
  UR_EXPECT_EQ(27 * kMinute,
               ClaimWatchdogMs({OnePool()}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}));
  UR_EXPECT_EQ(9 * kMinute, ClaimWatchdogMs({}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}));
  // an absurd answer saturates instead of overflowing the timer
  const std::vector<Account> many(10'000, OnePool());
  UR_EXPECT_EQ(std::numeric_limits<int>::max(), ClaimWatchdogMs(many, {}));
}

// ---- capture the flag on the claim sheet --------------------------------------------

// the sheet over the pool's account, a capture the flag account and a pool the
// server could not read
ClaimProgress CtfSheet(int64_t ctfClaimableRao,
                       const std::vector<UnavailablePool>& unavailable = {}) {
  return ClaimProgress({OnePool(), CtfAccount(ctfClaimableRao)}, unavailable, kFloor);
}

// CAPTURETHEFLAG.md §10: one press claims both pools, and the sheet says Claim
// both; a batch reports every account it claimed with its transaction
UR_TEST(SnLedgerCtf_OnePressClaimsBothPools) {
  ClaimProgress progress = CtfSheet(615'000'000);
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(progress.NameOf(*RowOf(progress, 1, 42)) == PoolName::Provider);
  UR_EXPECT_TRUE(progress.NameOf(*RowOf(progress, kCtfPool, 7)) == PoolName::Ctf);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->kind == urnw::snledger::kKindCtf);
  UR_EXPECT_TRUE(progress.ClaimsBoth());
  UR_EXPECT_EQ(int64_t{3'241'000'000 + 615'000'000}, progress.ClaimableRao());
  progress.Start();
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Sending);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Sending);
  // the batch: one transaction for both
  progress.Sent(1, 42, "0xba7c");
  progress.Sent(kCtfPool, 7, "0xba7c");
  progress.Confirmed(1, 42, "0xba7c", 3'241'000'000);
  progress.Confirmed(kCtfPool, 7, "0xba7c", 615'000'000);
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->txHash == "0xba7c");
  UR_EXPECT_EQ(int64_t{0}, progress.ClaimableRao());
  UR_EXPECT_FALSE(progress.ClaimsBoth());  // nothing left to claim
  // a sheet of one kind never says Claim both, as before capture the flag
  UR_EXPECT_FALSE(TwoPoolSheet().ClaimsBoth());
}

// a press claims what is left: a failed capture the flag claim, say, alone
UR_TEST(SnLedgerCtf_ARetryClaimsWhatIsLeft) {
  ClaimProgress progress = CtfSheet(615'000'000);
  progress.Start();
  progress.Sent(1, 42, "0xba7c");
  progress.Confirmed(1, 42, "0xba7c", 3'241'000'000);
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7, "claim_failed: the batch reverted"));
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Failed);
  UR_EXPECT_EQ(int64_t{615'000'000}, progress.ClaimableRao());
  UR_EXPECT_FALSE(progress.ClaimsBoth());
}

// below the claim minimum the capture the flag row says so: not in the total,
// not sent, and the vault's refusal of it leaves it saying why; the pool's
// account is claimed alone, so Claim does not say both
UR_TEST(SnLedgerCtf_BelowTheClaimMinimumOnTheSheet) {
  ClaimProgress progress = CtfSheet(12'000'000);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::UnderFloor);
  UR_EXPECT_EQ(int64_t{12'000'000}, RowOf(progress, kCtfPool, 7)->amountRao);
  UR_EXPECT_EQ(int64_t{3'241'000'000}, progress.ClaimableRao());
  UR_EXPECT_FALSE(progress.ClaimsBoth());
  progress.Start();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::UnderFloor);
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7,
                                 "not_claimable: AmountTooLow: the claim is below the runtime's "
                                 "transfer floor"));
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::UnderFloor);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->message.empty());
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::UnderFloor);
  // its needs_gas still turns on the top-up
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7, "needs_gas: the gas key holds nothing"));
  UR_EXPECT_TRUE(progress.NeedsGas());
  // the SDK may still claim it, the floor having moved: then it is claimed
  ClaimProgress moved = CtfSheet(12'000'000);
  moved.Start();
  moved.Sent(kCtfPool, 7, "0xf100");
  moved.Confirmed(kCtfPool, 7, "0xf100", 12'000'000);
  UR_EXPECT_TRUE(RowOf(moved, kCtfPool, 7)->phase == Phase::Claimed);
  // and a paid one is paid
  ClaimProgress paid = CtfSheet(12'000'000);
  UR_EXPECT_TRUE(paid.Failed(kCtfPool, 7, "already_claimed: NothingToClaim"));
  UR_EXPECT_TRUE(RowOf(paid, kCtfPool, 7)->phase == Phase::Claimed);
  // a lone pool account, as from a server before capture the flag, waits
  // below the claim minimum too (owner, 2026-11-11): nothing for a press
  ClaimProgress before({LedgerAccount(1, 42, 12'000'000, 0)}, {}, kFloor);
  UR_EXPECT_TRUE(before.Rows().front().phase == Phase::UnderFloor);
  UR_EXPECT_EQ(int64_t{12'000'000}, before.Rows().front().amountRao);
  UR_EXPECT_EQ(int64_t{0}, before.ClaimableRao());
  UR_EXPECT_TRUE(before.NameOf(before.Rows().front()) == PoolName::Numbered);
  before.Start();
  UR_EXPECT_TRUE(before.Rows().front().phase == Phase::UnderFloor);
  // without a price it is claimable
  const ClaimProgress unpriced({LedgerAccount(1, 42, 12'000'000, 0)}, {}, 0);
  UR_EXPECT_TRUE(unpriced.Rows().front().phase == Phase::Claimable);
  UR_EXPECT_EQ(int64_t{12'000'000}, unpriced.ClaimableRao());
}

// CAPTURETHEFLAG.md §10: a pool the server could not read is a row of its own,
// with no amount, never sent; the SDK's ledger_unavailable for it (index 0)
// leaves it so, and the other pool is claimed
UR_TEST(SnLedgerCtf_AnUnavailablePoolOnTheSheet) {
  ClaimProgress progress = ClaimProgress({OnePool()}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)},
                                         kFloor);
  UR_EXPECT_EQ(2u, progress.Rows().size());
  const ClaimRow* unreadable = RowOf(progress, kCtfPool, 0);
  UR_EXPECT_TRUE(unreadable != nullptr && unreadable->phase == Phase::Unavailable);
  UR_EXPECT_TRUE(progress.NameOf(*unreadable) == PoolName::Ctf);
  UR_EXPECT_TRUE(progress.NameOf(*RowOf(progress, 1, 42)) == PoolName::Provider);
  UR_EXPECT_EQ(int64_t{3'241'000'000}, progress.ClaimableRao());
  UR_EXPECT_FALSE(progress.ClaimsBoth());
  progress.Start();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 0)->phase == Phase::Unavailable);
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 0)->phase == Phase::Unavailable);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 0)->message.empty());
  progress.Sent(1, 42, "0x0001");
  progress.Confirmed(1, 42, "0x0001", 3'241'000'000);
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 0)->phase == Phase::Unavailable);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimed);
  // the pool unreadable: its row comes first, by its pool
  const ClaimProgress pool({CtfAccount(615'000'000)}, {Unreadable(1, urnw::snledger::kKindPool)}, kFloor);
  UR_EXPECT_EQ(2u, pool.Rows().size());
  UR_EXPECT_EQ(int64_t{1}, pool.Rows()[0].noId);
  UR_EXPECT_TRUE(pool.Rows()[0].phase == Phase::Unavailable);
  UR_EXPECT_TRUE(pool.NameOf(pool.Rows()[0]) == PoolName::Provider);
  UR_EXPECT_EQ(kCtfPool, pool.Rows()[1].noId);
  UR_EXPECT_TRUE(pool.Rows()[1].phase == Phase::Claimable);
}

// a pool that turns unreadable between the read and the claim: its account's row
// reads unavailable, and no second row appears for it
UR_TEST(SnLedgerCtf_APoolThatTurnsUnavailableKeepsOneRow) {
  ClaimProgress progress = CtfSheet(615'000'000);
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Unavailable);
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Unavailable);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimable);
  UR_EXPECT_EQ(int64_t{3'241'000'000}, progress.ClaimableRao());
  // one the sheet never listed gets a row of its own, unavailable
  ClaimProgress unlisted = TwoPoolSheet();
  UR_EXPECT_TRUE(unlisted.Failed(kCtfPool, 0, "ledger_unavailable: unreachable"));
  UR_EXPECT_TRUE(RowOf(unlisted, kCtfPool, 0)->phase == Phase::Unavailable);
}

// the first capture the flag root may land while the sheet is open: the SDK
// claims an account the sheet did not list, which is named by its pool id
UR_TEST(SnLedgerCtf_ACaptureTheFlagAccountTheSheetDidNotList) {
  ClaimProgress progress = ClaimProgress({OnePool()}, {}, kFloor);
  UR_EXPECT_TRUE(progress.NameOf(*RowOf(progress, 1, 42)) == PoolName::Numbered);
  progress.Start();
  progress.Sent(kCtfPool, 3, "0xba7c");
  const ClaimRow* ctf = RowOf(progress, kCtfPool, 3);
  UR_EXPECT_TRUE(ctf != nullptr && ctf->kind == urnw::snledger::kKindCtf);
  UR_EXPECT_TRUE(progress.NameOf(*ctf) == PoolName::Ctf);
  UR_EXPECT_TRUE(progress.NameOf(*RowOf(progress, 1, 42)) == PoolName::Provider);
}

// the SDK reads the ledger again: a pool unavailable at the sheet's read may be
// read at the claim. Its account then takes the pool's row, which reads as the
// account from then on, with no second row beside it
UR_TEST(SnLedgerCtf_AnUnavailablePoolReadAtTheClaimTakesItsRow) {
  ClaimProgress progress({OnePool()}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}, kFloor);
  progress.Start();
  progress.Sent(1, 42, "0xba7c");
  progress.Sent(kCtfPool, 7, "0xba7c");
  UR_EXPECT_EQ(2u, progress.Rows().size());
  const ClaimRow* ctf = RowOf(progress, kCtfPool, 7);
  UR_EXPECT_TRUE(ctf != nullptr && ctf->phase == Phase::Sent && !ctf->placeholder);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 0) == nullptr);
  UR_EXPECT_EQ(int64_t{0}, ctf->amountRao);  // not known until the vault pays it
  progress.Confirmed(kCtfPool, 7, "0xba7c", 615'000'000);
  UR_EXPECT_EQ(int64_t{615'000'000}, RowOf(progress, kCtfPool, 7)->amountRao);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Claimed);
  // a refusal of that account is the account's, not swallowed as unavailable,
  // even when its index is 0 (the tree's first leaf)
  ClaimProgress first({OnePool()}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}, kFloor);
  first.Start();
  UR_EXPECT_TRUE(first.Failed(kCtfPool, 0, "claim_failed: the batch transaction reverted"));
  UR_EXPECT_EQ(2u, first.Rows().size());
  UR_EXPECT_TRUE(RowOf(first, kCtfPool, 0)->phase == Phase::Failed);
  UR_EXPECT_FALSE(RowOf(first, kCtfPool, 0)->placeholder);
  // the pool still unreadable at the claim: its row stays as it is
  ClaimProgress still({OnePool()}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}, kFloor);
  still.Start();
  UR_EXPECT_TRUE(still.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  UR_EXPECT_EQ(2u, still.Rows().size());
  UR_EXPECT_TRUE(RowOf(still, kCtfPool, 0)->phase == Phase::Unavailable);
  UR_EXPECT_TRUE(RowOf(still, kCtfPool, 0)->placeholder);
}

// the runtime's refusal of an amount under its floor (not_claimable,
// AmountTooLow) is the claim minimum, where the relay's price did not show it
// (no price, or no relay): the row waits below it, out of the total and the
// next press, beside capture the flag or not
UR_TEST(SnLedgerCtf_TheRuntimesFloorIsTheClaimMinimum) {
  const std::string tooLow =
      "not_claimable: AmountTooLow: the claim is below the runtime's transfer floor";
  ClaimProgress progress = ClaimProgress({OnePool(), CtfAccount(12'000'000)}, {}, 0);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Claimable);
  UR_EXPECT_TRUE(progress.ClaimsBoth());
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7, tooLow));
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::UnderFloor);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->message.empty());
  progress.Sent(1, 42, "0x0001");
  progress.Confirmed(1, 42, "0x0001", 3'241'000'000);
  progress.Done();
  UR_EXPECT_EQ(int64_t{0}, progress.ClaimableRao());
  UR_EXPECT_FALSE(progress.ClaimsBoth());
  // the pool's account below it, beside a capture the flag account, waits too
  ClaimProgress pool = ClaimProgress({LedgerAccount(1, 42, 12'000'000, 0), CtfAccount(615'000'000)}, {}, 0);
  pool.Start();
  UR_EXPECT_TRUE(pool.Failed(1, 42, tooLow));
  UR_EXPECT_TRUE(RowOf(pool, 1, 42)->phase == Phase::UnderFloor);
  // a lone pool account, as from a server before capture the flag, waits too
  ClaimProgress before = ClaimProgress({LedgerAccount(1, 42, 12'000'000, 0)}, {}, 0);
  before.Start();
  UR_EXPECT_TRUE(before.Failed(1, 42, tooLow));
  UR_EXPECT_TRUE(RowOf(before, 1, 42)->phase == Phase::UnderFloor);
  UR_EXPECT_TRUE(RowOf(before, 1, 42)->message.empty());
  before.Done();
  UR_EXPECT_EQ(int64_t{0}, before.ClaimableRao());
}

// ledger_unavailable for a pool changes only its rows not sent yet: a row the
// press sent or the vault paid keeps saying so
UR_TEST(SnLedgerCtf_AnUnavailablePoolKeepsWhatWasSentOrPaid) {
  ClaimProgress progress = CtfSheet(615'000'000);
  progress.Start();
  progress.Sent(kCtfPool, 7, "0xba7c");
  progress.Confirmed(kCtfPool, 7, "0xba7c", 615'000'000);
  UR_EXPECT_TRUE(progress.Failed(1, 42, "claim_failed: the batch transaction reverted"));
  progress.Done();
  // a second press: the capture the flag pool is unreadable now
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Claimed);
  UR_EXPECT_EQ(int64_t{615'000'000}, RowOf(progress, kCtfPool, 7)->amountRao);
  UR_EXPECT_TRUE(progress.AnyClaimed());
  // the pool's own pool unreadable: its unsent row says so
  UR_EXPECT_TRUE(progress.Failed(1, 0, "ledger_unavailable: the pool could not be read"));
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Unavailable);
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Unavailable);
}

// sdk.interface.md §3: an account the batch did not attempt (it ran short of
// gas) is claimed alone after it: a second Sent, with its own transaction, on
// the same row, then its Confirmed
UR_TEST(SnLedgerCtf_AnAccountTheBatchDidNotAttemptIsSentAlone) {
  ClaimProgress progress = CtfSheet(615'000'000);
  progress.Start();
  progress.Sent(1, 42, "0xba7c");
  progress.Sent(kCtfPool, 7, "0xba7c");
  progress.Confirmed(1, 42, "0xba7c", 3'241'000'000);
  progress.Sent(kCtfPool, 7, "0xa10e");
  UR_EXPECT_EQ(2u, progress.Rows().size());
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Sent);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->txHash == "0xa10e");
  progress.Confirmed(kCtfPool, 7, "0xa10e", 615'000'000);
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->txHash == "0xa10e");
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->txHash == "0xba7c");
}

// sdk-ctf-v2 §2.1: a gas key that cannot pay for the batch has the largest
// account claimed alone first; an account it then cannot pay for fails
// needs_gas with nothing sent: it stays claimable, in the total and the next
// press, the top-up shows, and a retry claims what is left
UR_TEST(SnLedgerCtf_TheLargestIsClaimedAndTheOthersStayClaimable) {
  ClaimProgress progress = CtfSheet(615'000'000);
  progress.Start();
  progress.Sent(1, 42, "0x1a96");
  progress.Confirmed(1, 42, "0x1a96", 3'241'000'000);
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7,
                                 "needs_gas: this claim needs up to 0.0130 TAO of gas, the gas key "
                                 "0x9a1c has 0.0040; send TAO to 5Gh…"));
  progress.Done();
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Claimed);
  const ClaimRow* ctf = RowOf(progress, kCtfPool, 7);
  UR_EXPECT_TRUE(ctf->phase == Phase::Claimable);
  UR_EXPECT_TRUE(ctf->message.empty());
  UR_EXPECT_EQ(int64_t{615'000'000}, ctf->amountRao);
  UR_EXPECT_TRUE(progress.NeedsGas());
  UR_EXPECT_EQ(int64_t{615'000'000}, progress.ClaimableRao());
  UR_EXPECT_TRUE(progress.AnyClaimed());
  progress.Start();
  UR_EXPECT_TRUE(RowOf(progress, kCtfPool, 7)->phase == Phase::Sending);
  UR_EXPECT_FALSE(progress.NeedsGas());
  // an account a batch sent but did not attempt, then not paid for alone: no
  // transaction claims it, so it links none
  ClaimProgress batch = CtfSheet(615'000'000);
  batch.Start();
  batch.Sent(1, 42, "0xba7c");
  batch.Sent(kCtfPool, 7, "0xba7c");
  batch.Confirmed(1, 42, "0xba7c", 3'241'000'000);
  UR_EXPECT_TRUE(batch.Failed(kCtfPool, 7, "needs_gas: the gas key cannot pay for this claim"));
  UR_EXPECT_TRUE(RowOf(batch, kCtfPool, 7)->phase == Phase::Claimable);
  UR_EXPECT_TRUE(RowOf(batch, kCtfPool, 7)->txHash.empty());
  UR_EXPECT_TRUE(RowOf(batch, 1, 42)->txHash == "0xba7c");
  // one the sheet listed below the claim minimum, which the SDK tried, is
  // still claimable too, as on Android (SnLedgerCtf_AnAccountBelowTheClaim
  // MinimumShowsWhatTheSdkSaid)
  ClaimProgress small = CtfSheet(12'000'000);
  small.Start();
  UR_EXPECT_TRUE(small.Failed(kCtfPool, 7, "needs_gas: the gas key cannot pay for this claim"));
  UR_EXPECT_TRUE(RowOf(small, kCtfPool, 7)->phase == Phase::Claimable);
  UR_EXPECT_TRUE(small.NeedsGas());
}

// Android's snLedgerClaimProgressAfter: the SDK claims every account with
// something to claim, one the sheet lists below the claim minimum too, and the
// row says what came of it. Only the runtime's AmountTooLow reads below the
// claim minimum; the gas key unable to pay for it leaves it claimable, with
// the top-up; any other failure is Failed, with the SDK's message; a claim of
// it is claimed. A press never counts it, as the sheet's read listed it: no
// Claim the runtime would refuse at the relay's price
UR_TEST(SnLedgerCtf_AnAccountBelowTheClaimMinimumShowsWhatTheSdkSaid) {
  for (const char* message :
       {"claim_failed: the claim transaction reverted",
        "proof_mismatch: the ledger's proof does not verify against vault2's root",
        "chain_rpc_error: eth_sendRawTransaction: nonce too low",
        "not_claimable: NothingToClaim"}) {
    ClaimProgress progress = CtfSheet(12'000'000);
    progress.Start();
    progress.Sent(1, 42, "0x0001");
    progress.Confirmed(1, 42, "0x0001", 3'241'000'000);
    UR_EXPECT_TRUE_MSG(message, progress.Failed(kCtfPool, 7, message));
    progress.Done();
    const ClaimRow* ctf = RowOf(progress, kCtfPool, 7);
    UR_EXPECT_TRUE_MSG(message, ctf->phase == Phase::Failed);
    UR_EXPECT_TRUE_MSG(message, ctf->message == message);
    // out of the next press: no amount, no Claim both, no Claim for it alone
    UR_EXPECT_EQ(int64_t{0}, progress.ClaimableRao());
    UR_EXPECT_FALSE(progress.ClaimsBoth());
    // a press waits below the claim minimum again, as the sheet's read
    // listed it, until the SDK reports on it
    progress.Start();
    UR_EXPECT_TRUE_MSG(message, RowOf(progress, kCtfPool, 7)->phase == Phase::UnderFloor);
    UR_EXPECT_TRUE_MSG(message, RowOf(progress, kCtfPool, 7)->message.empty());
  }
  // the gas key could not pay for it: still claimable, with the top-up, and
  // still no Claim for it
  ClaimProgress gas = CtfSheet(12'000'000);
  gas.Start();
  gas.Sent(1, 42, "0x0001");
  gas.Confirmed(1, 42, "0x0001", 3'241'000'000);
  UR_EXPECT_TRUE(gas.Failed(kCtfPool, 7, "needs_gas: the gas key cannot pay for this claim"));
  gas.Done();
  UR_EXPECT_TRUE(RowOf(gas, kCtfPool, 7)->phase == Phase::Claimable);
  UR_EXPECT_TRUE(RowOf(gas, kCtfPool, 7)->message.empty());
  UR_EXPECT_TRUE(gas.NeedsGas());
  UR_EXPECT_EQ(int64_t{0}, gas.ClaimableRao());
  // AmountTooLow alone reads below the claim minimum
  ClaimProgress tooLow = CtfSheet(12'000'000);
  tooLow.Start();
  UR_EXPECT_TRUE(tooLow.Failed(kCtfPool, 7, "not_claimable: AmountTooLow: below the runtime's floor"));
  UR_EXPECT_TRUE(RowOf(tooLow, kCtfPool, 7)->phase == Phase::UnderFloor);
  UR_EXPECT_TRUE(RowOf(tooLow, kCtfPool, 7)->message.empty());
  // the pool's account under the floor, batched with capture the flag's, the
  // batch reverted: both Failed, and the retry takes capture the flag's alone
  ClaimProgress pool({LedgerAccount(1, 42, 12'000'000, 0), CtfAccount(615'000'000)}, {}, kFloor);
  UR_EXPECT_TRUE(RowOf(pool, 1, 42)->phase == Phase::UnderFloor);
  pool.Start();
  pool.Sent(1, 42, "0x0002");
  pool.Sent(kCtfPool, 7, "0x0002");
  UR_EXPECT_TRUE(pool.Failed(1, 42, "claim_failed: the batch transaction reverted"));
  UR_EXPECT_TRUE(pool.Failed(kCtfPool, 7, "claim_failed: the batch transaction reverted"));
  pool.Done();
  UR_EXPECT_TRUE(RowOf(pool, 1, 42)->phase == Phase::Failed);
  UR_EXPECT_TRUE(RowOf(pool, kCtfPool, 7)->phase == Phase::Failed);
  UR_EXPECT_EQ(int64_t{615'000'000}, pool.ClaimableRao());
  UR_EXPECT_FALSE(pool.ClaimsBoth());
  pool.Start();
  UR_EXPECT_TRUE(RowOf(pool, 1, 42)->phase == Phase::UnderFloor);
  UR_EXPECT_TRUE(RowOf(pool, kCtfPool, 7)->phase == Phase::Sending);
}

// Android's snLedgerClaimSettled: a pool the sheet's read could not read keeps
// its row while the press stopped before the SDK's own read; once that read
// answered without naming the pool unavailable or reporting an account of it,
// the pool was read with nothing to claim, and its row goes at Done
UR_TEST(SnLedgerCtf_AnUnavailablePoolTheClaimFoundEmptyLeaves) {
  const std::vector<UnavailablePool> ctf = {Unreadable(kCtfPool, urnw::snledger::kKindCtf)};
  // the SDK claimed the pool's account and said nothing of capture the flag's pool
  ClaimProgress read({OnePool()}, ctf, kFloor);
  read.Start();
  read.Sent(1, 42, "0x0001");
  read.Confirmed(1, 42, "0x0001", 3'241'000'000);
  UR_EXPECT_EQ(2u, read.Rows().size());  // not before Done
  read.Done();
  UR_EXPECT_EQ(1u, read.Rows().size());
  UR_EXPECT_TRUE(RowOf(read, kCtfPool, 0) == nullptr);
  // the name the read gave stays: the pool is still there
  UR_EXPECT_TRUE(read.NameOf(*RowOf(read, 1, 42)) == PoolName::Provider);
  // nothing to claim in any pool, which the SDK never says while one is unavailable
  ClaimProgress nothing({}, ctf, kFloor);
  nothing.Start();
  UR_EXPECT_FALSE(nothing.Failed(0, 0, "not_claimable: no settlement v2 account has anything to claim"));
  nothing.Done();
  UR_EXPECT_TRUE(nothing.Rows().empty());
  // still unavailable to the SDK's read: the row stays
  ClaimProgress still({OnePool()}, ctf, kFloor);
  still.Start();
  UR_EXPECT_TRUE(still.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  still.Sent(1, 42, "0x0001");
  still.Done();
  UR_EXPECT_TRUE(RowOf(still, kCtfPool, 0) != nullptr);
  UR_EXPECT_TRUE(RowOf(still, kCtfPool, 0)->phase == Phase::Unavailable);
  // stopped before the SDK's read (the chain, the ledger, a watchdog): the row stays
  ClaimProgress stopped({OnePool()}, ctf, kFloor);
  stopped.Start();
  UR_EXPECT_FALSE(stopped.Failed(0, 0, "ledger_unavailable: the ledger could not be read"));
  UR_EXPECT_FALSE(stopped.Failed(0, 0, "chain_rpc_unreachable: timeout"));
  stopped.Done();
  UR_EXPECT_TRUE(RowOf(stopped, kCtfPool, 0) != nullptr);
  ClaimProgress silent({OnePool()}, ctf, kFloor);
  silent.Start();
  silent.Done();
  UR_EXPECT_TRUE(RowOf(silent, kCtfPool, 0) != nullptr);
  // its account found at the claim took the row: it stays, as that account
  ClaimProgress found({OnePool()}, ctf, kFloor);
  found.Start();
  UR_EXPECT_TRUE(found.Failed(kCtfPool, 7, "not_claimable: AmountTooLow: below the runtime's floor"));
  found.Done();
  UR_EXPECT_TRUE(RowOf(found, kCtfPool, 7) != nullptr);
  UR_EXPECT_TRUE(RowOf(found, kCtfPool, 7)->phase == Phase::UnderFloor);
  // a second press reads again: a pool unavailable in it keeps its row
  ClaimProgress again({OnePool()}, ctf, kFloor);
  again.Start();
  UR_EXPECT_TRUE(again.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  again.Failed(1, 42, "claim_failed: reverted");
  again.Done();
  again.Start();
  UR_EXPECT_TRUE(again.Failed(kCtfPool, 0, "ledger_unavailable: the pool could not be read"));
  again.Sent(1, 42, "0x0002");
  again.Done();
  UR_EXPECT_TRUE(RowOf(again, kCtfPool, 0) != nullptr);
}

// Claim both is the pool's account and the capture the flag account, exactly
UR_TEST(SnLedgerCtf_ClaimBothIsExactlyThoseTwo) {
  UR_EXPECT_TRUE(CtfSheet(615'000'000).ClaimsBoth());
  const ClaimProgress three({OnePool(), LedgerAccount(3, 5, 1'000'000'000, 0), CtfAccount(615'000'000)},
                            {}, kFloor);
  UR_EXPECT_FALSE(three.ClaimsBoth());
  const ClaimProgress lone({CtfAccount(615'000'000)}, {}, kFloor);
  UR_EXPECT_FALSE(lone.ClaimsBoth());
}

// ---- URnetwork's claim relay: the sheet's per-pool rows ------------------------------

RelayAccount Relay(int64_t noId, const char* kind, int64_t claimableRao,
                   const char* coldkey = "5F3sColdkey") {
  RelayAccount account;
  account.coldkeySs58 = coldkey;
  account.noId = noId;
  account.kind = kind;
  account.claimableRao = claimableRao;
  return account;
}

// Android's snClaimRelayRows, on every server: Claim now's sheet lists every
// account the relay reports, a lone pool account too, so one below the claim
// minimum says so, and a network paid to two coldkeys shows the balance its
// total leaves out
UR_TEST(SnLedgerCtf_RelayRowsListEveryAccount) {
  using urnw::snledger::kKindPool;
  const std::vector<RelayRow> lone = RelayRowsFor({Relay(1, kKindPool, 3'241'000'000)}, {}, kFloor);
  UR_EXPECT_EQ(1u, lone.size());
  RelayRow pool;
  pool.noId = 1;
  pool.pool = PoolName::Numbered;
  pool.amountRao = 3'241'000'000;
  pool.status = RelayStatus::Pending;
  UR_EXPECT_TRUE(lone.size() == 1 && lone[0] == pool);
  const std::vector<RelayRow> small = RelayRowsFor({Relay(1, kKindPool, 12'000'000)}, {}, kFloor);
  UR_EXPECT_EQ(1u, small.size());
  UR_EXPECT_TRUE(small.size() == 1 && small[0].status == RelayStatus::UnderFloor);
  UR_EXPECT_TRUE(small.size() == 1 && small[0].amountRao == 12'000'000);
  const std::vector<RelayRow> unpriced = RelayRowsFor({Relay(1, kKindPool, 12'000'000)}, {}, 0);
  UR_EXPECT_TRUE(unpriced.size() == 1 && unpriced[0].status == RelayStatus::Pending);
  const std::vector<RelayRow> two = RelayRowsFor(
      {Relay(1, kKindPool, 3'241'000'000), Relay(1, kKindPool, 500'000'000, "5GxOtherColdkey")}, {},
      kFloor);
  UR_EXPECT_EQ(2u, two.size());
  UR_EXPECT_TRUE(two.size() == 2 && two[0].coldkeySs58 == "5F3sColdkey");
  UR_EXPECT_TRUE(two.size() == 2 && two[1].coldkeySs58 == "5GxOtherColdkey");
  UR_EXPECT_TRUE(two.size() == 2 && two[1].pool == PoolName::Numbered);
  // URnetwork's claim of a lone account: Claimed with its transaction
  RelayAccount claimed = Relay(1, kKindPool, 0);
  claimed.claimTxHash = "0x" + std::string(64, 'c');
  claimed.claimTxUrl = "https://evm.taostats.io/tx/" + claimed.claimTxHash;
  claimed.claimAmountRao = 3'241'000'000;
  const std::vector<RelayRow> paid = RelayRowsFor({claimed}, {}, kFloor);
  UR_EXPECT_EQ(1u, paid.size());
  UR_EXPECT_TRUE(paid.size() == 1 && paid[0].status == RelayStatus::Claimed);
  UR_EXPECT_TRUE(paid.size() == 1 && paid[0].txUrl == claimed.claimTxUrl);
  // nothing to claim and no claim of URnetwork's: no row
  UR_EXPECT_TRUE(RelayRowsFor({Relay(1, kKindPool, 0)}, {}, kFloor).empty());
  UR_EXPECT_TRUE(RelayRowsFor({}, {}, kFloor).empty());
}

// Android's snClaimRelayTotalKnown: the relay's claimable adds up the primary
// coldkey's accounts, so it is a total to show while one of them was read, or
// nothing is unavailable; with none of its pools read there is no total,
// never a zero
UR_TEST(SnLedgerCtf_TheRelaysTotalNeedsOneOfTheColdkeysPools) {
  using urnw::snledger::kKindCtf;
  using urnw::snledger::kKindPool;
  using urnw::snledger::RelayTotalKnown;
  UR_EXPECT_TRUE(RelayTotalKnown({Relay(1, kKindPool, 3'241'000'000)}, "5F3sColdkey", {}));
  UR_EXPECT_TRUE(RelayTotalKnown({}, "5F3sColdkey", {}));  // nothing at all: a zero that is one
  UR_EXPECT_TRUE(RelayTotalKnown({Relay(1, kKindPool, 0)}, "5F3sColdkey",
                                 {Unreadable(kCtfPool, kKindCtf)}));
  UR_EXPECT_FALSE(RelayTotalKnown({}, "5F3sColdkey", {Unreadable(1, kKindPool)}));
  // another coldkey's account is not the primary's
  UR_EXPECT_FALSE(RelayTotalKnown({Relay(1, kKindPool, 500'000'000, "5GxOtherColdkey")}, "5F3sColdkey",
                                  {Unreadable(1, kKindPool), Unreadable(kCtfPool, kKindCtf)}));
}

// Android's snClaimRelayRows: each pool with something to claim, Pending, or
// below the claim minimum; Claimed with URnetwork's transaction once nothing is
// claimable since; a pool with neither has no row
UR_TEST(SnLedgerCtf_RelayRowsSayEachPoolsStatus) {
  using urnw::snledger::kKindCtf;
  using urnw::snledger::kKindPool;
  const std::vector<RelayRow> pending =
      RelayRowsFor({Relay(1, kKindPool, 3'241'000'000), Relay(kCtfPool, kKindCtf, 12'000'000)}, {},
                   kFloor);
  UR_EXPECT_EQ(2u, pending.size());
  RelayRow pool;
  pool.noId = 1;
  pool.pool = PoolName::Provider;
  pool.amountRao = 3'241'000'000;
  pool.status = RelayStatus::Pending;
  UR_EXPECT_TRUE(pending[0] == pool);  // one coldkey: none named
  UR_EXPECT_TRUE(pending[1].pool == PoolName::Ctf);
  UR_EXPECT_TRUE(pending[1].status == RelayStatus::UnderFloor);
  UR_EXPECT_EQ(int64_t{12'000'000}, pending[1].amountRao);
  // URnetwork claimed both in one batch: each Claimed with what it paid
  RelayAccount claimedPool = Relay(1, kKindPool, 0);
  claimedPool.claimTxHash = "0x" + std::string(64, 'b');
  claimedPool.claimTxUrl = "https://evm.taostats.io/tx/" + claimedPool.claimTxHash;
  claimedPool.claimAmountRao = 3'241'000'000;
  RelayAccount quiet = Relay(kCtfPool, kKindCtf, 0);  // nothing to claim, no claim
  const std::vector<RelayRow> claimed = RelayRowsFor({claimedPool, quiet}, {}, kFloor);
  UR_EXPECT_EQ(1u, claimed.size());
  UR_EXPECT_TRUE(claimed[0].status == RelayStatus::Claimed);
  UR_EXPECT_TRUE(claimed[0].pool == PoolName::Provider);  // beside the quiet account
  UR_EXPECT_EQ(int64_t{3'241'000'000}, claimed[0].amountRao);
  UR_EXPECT_TRUE(claimed[0].txUrl == claimedPool.claimTxUrl);
  // a claimable balance since URnetwork's claim: Pending, not Claimed
  RelayAccount since = claimedPool;
  since.claimableRao = 40'000'000;
  UR_EXPECT_TRUE(RelayRowsFor({since, quiet}, {}, kFloor).front().status == RelayStatus::Pending);
  // a transaction page that is not a web page is no link
  RelayAccount odd = claimedPool;
  odd.claimTxUrl = "javascript:alert(1)";
  UR_EXPECT_TRUE(RelayRowsFor({odd, quiet}, {}, kFloor).front().txUrl.empty());
  // without a price nothing is below the minimum
  UR_EXPECT_TRUE(RelayRowsFor({Relay(1, kKindPool, 3'241'000'000),
                               Relay(kCtfPool, kKindCtf, 12'000'000)},
                              {}, 0)[1]
                     .status == RelayStatus::Pending);
}

// a pool the server could not read is a row, unavailable and without an
// amount, whatever the relay listed; the other rows show as usual
UR_TEST(SnLedgerCtf_RelayRowsCarryUnavailablePools) {
  using urnw::snledger::kKindCtf;
  using urnw::snledger::kKindPool;
  const std::vector<RelayRow> rows =
      RelayRowsFor({Relay(1, kKindPool, 3'241'000'000)}, {Unreadable(kCtfPool, kKindCtf)}, kFloor);
  UR_EXPECT_EQ(2u, rows.size());
  UR_EXPECT_TRUE(rows[0].pool == PoolName::Provider);
  UR_EXPECT_TRUE(rows[0].status == RelayStatus::Pending);
  RelayRow unreadable;
  unreadable.noId = kCtfPool;
  unreadable.pool = PoolName::Ctf;
  unreadable.status = RelayStatus::Unavailable;
  UR_EXPECT_TRUE(rows[1] == unreadable);
  // the pool's own pool unreadable, nothing else listed
  const std::vector<RelayRow> alone = RelayRowsFor({}, {Unreadable(1, kKindPool)}, kFloor);
  UR_EXPECT_EQ(1u, alone.size());
  UR_EXPECT_TRUE(alone[0].status == RelayStatus::Unavailable);
  UR_EXPECT_TRUE(alone[0].pool == PoolName::Numbered);
}

// a network paid to more than one coldkey names each row's
UR_TEST(SnLedgerCtf_RelayRowsNameTheColdkeyOnlyWhenThereAreSeveral) {
  using urnw::snledger::kKindCtf;
  using urnw::snledger::kKindPool;
  const std::vector<RelayRow> rows =
      RelayRowsFor({Relay(1, kKindPool, 3'241'000'000, "5F3sColdkey"),
                    Relay(kCtfPool, kKindCtf, 615'000'000, "5F3sColdkey"),
                    Relay(1, kKindPool, 500'000'000, "5GxOtherColdkey")},
                   {}, kFloor);
  UR_EXPECT_EQ(3u, rows.size());
  UR_EXPECT_TRUE(rows[0].coldkeySs58 == "5F3sColdkey");
  UR_EXPECT_TRUE(rows[1].coldkeySs58 == "5F3sColdkey");
  UR_EXPECT_TRUE(rows[2].coldkeySs58 == "5GxOtherColdkey");
  // the one pool, held by both coldkeys, is still the provider pool
  UR_EXPECT_TRUE(rows[0].pool == PoolName::Provider);
  UR_EXPECT_TRUE(rows[2].pool == PoolName::Provider);
}

// the tile of a network with no wallet here (Android's snClaimRelayTileRows):
// under the sum the relay claims, the primary coldkey's accounts with something
// to claim, beside capture the flag or an unavailable pool, then those pools;
// with the pool's account alone the sum says it all, as before
UR_TEST(SnLedgerCtf_TheRelayTileListsThePrimaryColdkeysPools) {
  using urnw::snledger::kKindCtf;
  using urnw::snledger::kKindPool;
  using urnw::snledger::RelayTileRowsFor;
  // before capture the flag: no rows, also for a network paid to two coldkeys
  UR_EXPECT_TRUE(RelayTileRowsFor({Relay(1, kKindPool, 3'241'000'000)}, "5F3sColdkey", {}, kFloor).empty());
  UR_EXPECT_TRUE(RelayTileRowsFor({Relay(1, kKindPool, 3'241'000'000),
                                   Relay(1, kKindPool, 500'000'000, "5GxOtherColdkey")},
                                  "5F3sColdkey", {}, kFloor)
                     .empty());
  // two operators' pools of the primary coldkey: the sum adds up two, so each
  // is a row (Android's snLedgerNamesPools), one below the minimum saying so
  const std::vector<RelayRow> pools = RelayTileRowsFor(
      {Relay(1, kKindPool, 3'241'000'000), Relay(3, kKindPool, 12'000'000)}, "5F3sColdkey", {}, kFloor);
  UR_EXPECT_EQ(2u, pools.size());
  UR_EXPECT_TRUE(pools.size() == 2 && pools[0].pool == PoolName::Numbered);
  UR_EXPECT_TRUE(pools.size() == 2 && pools[0].status == RelayStatus::Pending);
  UR_EXPECT_TRUE(pools.size() == 2 && pools[1].status == RelayStatus::UnderFloor);
  // the pool's and the capture the flag account of the primary coldkey, the
  // other coldkey's left to Claim now's sheet
  const std::vector<RelayRow> rows =
      RelayTileRowsFor({Relay(1, kKindPool, 3'241'000'000), Relay(kCtfPool, kKindCtf, 12'000'000),
                        Relay(1, kKindPool, 500'000'000, "5GxOtherColdkey")},
                       "5F3sColdkey", {}, kFloor);
  UR_EXPECT_EQ(2u, rows.size());
  UR_EXPECT_TRUE(rows[0].pool == PoolName::Provider);
  UR_EXPECT_TRUE(rows[0].status == RelayStatus::Pending);
  UR_EXPECT_EQ(int64_t{3'241'000'000}, rows[0].amountRao);
  UR_EXPECT_TRUE(rows[0].coldkeySs58.empty());
  UR_EXPECT_TRUE(rows[1].pool == PoolName::Ctf);
  UR_EXPECT_TRUE(rows[1].status == RelayStatus::UnderFloor);
  // the capture the flag account with nothing to claim: the pool's row alone
  // is the sum, so no rows
  UR_EXPECT_TRUE(RelayTileRowsFor({Relay(1, kKindPool, 3'241'000'000), Relay(kCtfPool, kKindCtf, 0)},
                                  "5F3sColdkey", {}, kFloor)
                     .empty());
  // a lone capture the flag account is named
  const std::vector<RelayRow> lone =
      RelayTileRowsFor({Relay(kCtfPool, kKindCtf, 615'000'000)}, "5F3sColdkey", {}, kFloor);
  UR_EXPECT_EQ(1u, lone.size());
  UR_EXPECT_TRUE(lone[0].pool == PoolName::Ctf);
  // an unavailable pool: the sum leaves it out, and its row says so
  const std::vector<RelayRow> partial = RelayTileRowsFor(
      {Relay(1, kKindPool, 3'241'000'000)}, "5F3sColdkey", {Unreadable(kCtfPool, kKindCtf)}, kFloor);
  UR_EXPECT_EQ(2u, partial.size());
  UR_EXPECT_TRUE(partial[0].pool == PoolName::Provider);
  UR_EXPECT_TRUE(partial[1].status == RelayStatus::Unavailable);
  UR_EXPECT_TRUE(partial[1].pool == PoolName::Ctf);
}

// the tile's sum, drawn without rows, is one account: below the claim minimum
// it says so under the sum, as the wallet tile's lone account does, on every
// server; rows say it for themselves
UR_TEST(SnLedgerCtf_TheRelayTilesLoneSumBelowTheClaimMinimumSaysSo) {
  using urnw::snledger::kKindCtf;
  using urnw::snledger::kKindPool;
  using urnw::snledger::RelayTileSumUnderFloor;
  UR_EXPECT_TRUE(RelayTileSumUnderFloor({Relay(1, kKindPool, 12'000'000)}, "5F3sColdkey", {}, kFloor));
  UR_EXPECT_FALSE(RelayTileSumUnderFloor({Relay(1, kKindPool, 12'000'000)}, "5F3sColdkey", {}, 0));
  UR_EXPECT_FALSE(
      RelayTileSumUnderFloor({Relay(1, kKindPool, 3'241'000'000)}, "5F3sColdkey", {}, kFloor));
  UR_EXPECT_FALSE(RelayTileSumUnderFloor({Relay(1, kKindPool, 0)}, "5F3sColdkey", {}, kFloor));
  // another coldkey's small account is not the sum's
  UR_EXPECT_FALSE(RelayTileSumUnderFloor(
      {Relay(1, kKindPool, 3'241'000'000), Relay(1, kKindPool, 12'000'000, "5GxOtherColdkey")},
      "5F3sColdkey", {}, kFloor));
  // with rows, each says it itself
  UR_EXPECT_FALSE(RelayTileSumUnderFloor({Relay(kCtfPool, kKindCtf, 12'000'000)}, "5F3sColdkey", {},
                                         kFloor));
  UR_EXPECT_FALSE(RelayTileSumUnderFloor({Relay(1, kKindPool, 12'000'000)}, "5F3sColdkey",
                                         {Unreadable(kCtfPool, kKindCtf)}, kFloor));
}

// ---- capture the flag: the line of a won slot ---------------------------------------

Win WonIn(int64_t epoch, const char* role, const char* country,
          const char* claimState = urnw::snledger::kClaimStateUnclaimed) {
  Win win;
  win.role = role;
  win.country = country;
  win.epoch = epoch;
  win.claimState = claimState;
  return win;
}

// a win's line follows the role that won; none without a country or a role
// this build knows
UR_TEST(SnLedgerCtf_TheWonSlotLineFollowsTheRole) {
  UR_EXPECT_TRUE(WonSlotFor(WonIn(8, "provider", "ke")) == WonSlot::Provider);
  UR_EXPECT_TRUE(WonSlotFor(WonIn(8, "extender", "is")) == WonSlot::Extender);
  UR_EXPECT_TRUE(WonSlotFor(WonIn(8, "provider", "")) == WonSlot::None);
  UR_EXPECT_TRUE(WonSlotFor(WonIn(8, "head", "ke")) == WonSlot::None);
  UR_EXPECT_TRUE(WonSlotFor(WonIn(8, "", "ke")) == WonSlot::None);
}

// owner, 2026-11-11: a line for every win not claimed yet (the SDK's
// UnclaimedWins), one each, newest first, providers before extenders within
// an epoch as the SDK lists them; none for no wins, a win claimed or of
// unknown claim, or one the line would guess at
UR_TEST(SnLedgerCtf_EveryUnclaimedWinIsALine) {
  using urnw::snledger::kClaimStateClaimed;
  using urnw::snledger::kClaimStateUnknown;
  UR_EXPECT_TRUE(WonLinesFor({}).empty());
  const std::vector<WonLine> lines =
      WonLinesFor({WonIn(8, "provider", "ke"), WonIn(10, "provider", "is"), WonIn(10, "extender", "is"),
                   WonIn(9, "extender", "ke")});
  UR_EXPECT_EQ(4u, lines.size());
  if (lines.size() == 4) {
    UR_EXPECT_TRUE(lines[0] == (WonLine{WonSlot::Provider, "is"}));
    UR_EXPECT_TRUE(lines[1] == (WonLine{WonSlot::Extender, "is"}));
    UR_EXPECT_TRUE(lines[2] == (WonLine{WonSlot::Extender, "ke"}));
    UR_EXPECT_TRUE(lines[3] == (WonLine{WonSlot::Provider, "ke"}));
  }
  // one role's slot of one country twice (two epochs, or two of the network's
  // clients): two wins, two lines
  UR_EXPECT_EQ(2u, WonLinesFor({WonIn(9, "provider", "ke"), WonIn(8, "provider", "ke")}).size());
  UR_EXPECT_EQ(2u, WonLinesFor({WonIn(9, "provider", "ke"), WonIn(9, "provider", "ke")}).size());
  // a win claimed or of unknown claim has none; nor one without a country or a
  // role this build knows
  const std::vector<WonLine> left =
      WonLinesFor({WonIn(10, "provider", "is", kClaimStateClaimed), WonIn(10, "extender", "is", kClaimStateUnknown),
                   WonIn(9, "provider", ""), WonIn(9, "head", "ke"), WonIn(8, "extender", "ke")});
  UR_EXPECT_EQ(1u, left.size());
  if (left.size() == 1) UR_EXPECT_TRUE(left[0] == (WonLine{WonSlot::Extender, "ke"}));
  UR_EXPECT_TRUE(WonLinesFor({WonIn(9, "provider", "ke", "")}).empty());
}

// ---- the wins after a claim (every app's schedule) ----------------------------------

// GET /sn/ctf/client as the SDK reads it: one answer per read in turn, the last
// one for every read after it, except the reads in `failing` (counted from 0),
// which fail (ctf_unavailable). Each read answers at once, through the page's
// WinsLines as its LoadCtfWins and ApplyCtfWins use it: the newest good answer
// shows, and a failure keeps the lines.
struct FakeWinsRoute {
  std::vector<std::vector<Win>> answers;
  std::vector<size_t> failing;
  size_t reads = 0;
  WinsLines wins;
  std::vector<WonLine> lines;

  void Read() {
    const uint64_t read = wins.Start();
    std::optional<std::vector<Win>> answer;
    if (std::find(failing.begin(), failing.end(), reads) == failing.end()) {
      answer = answers[std::min(reads, answers.size() - 1)];
    }
    wins.Apply(read, std::move(answer));
    lines = WonLinesFor(wins.Wins());
    ++reads;
  }
};

// The main loop's one-shot timers on a clock the test moves: each fires once,
// in time order, as the clock passes it.
struct FakeClock {
  int64_t now = 0;
  std::vector<int> delays;  // every delay asked for, in order
  std::vector<std::pair<int64_t, std::function<void()>>> timers;  // due, fire

  WinsAfterClaim::Schedule Port() {
    return [this](int delayMs, std::function<void()> fire) {
      delays.push_back(delayMs);
      timers.emplace_back(now + delayMs, std::move(fire));
    };
  }

  void AdvanceTo(int64_t ms) {
    for (;;) {
      auto due = std::min_element(timers.begin(), timers.end(),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });
      if (due == timers.end() || due->first > ms) break;
      now = due->first;
      std::function<void()> fire = std::move(due->second);
      timers.erase(due);
      fire();
    }
    now = ms;
  }
};

// The SDK's Done comes once the claim is in a block, but the route reads
// vault2's claimed at the finalized head, so its first answers after the claim
// still list a win the claim paid. After a claim that paid an account the wins
// are read at Done and again 30 s, 60 s and 120 s after it, and the line goes
// at the first read that finds the win claimed
UR_TEST(SnLedgerCtf_TheWinsAClaimPaidAreReadAgainUntilTheServerReadsThemClaimed) {
  UR_EXPECT_TRUE((kWinsReadsAfterClaimMs == std::array<int, 3>{30'000, 60'000, 120'000}));
  FakeWinsRoute route;
  route.answers = {{WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is", urnw::snledger::kClaimStateClaimed)},
                   {}};
  FakeClock clock;
  WinsAfterClaim after({[&route] { route.Read(); }, clock.Port()});
  route.Read();  // the page's load, before the claim
  UR_EXPECT_EQ(1u, route.lines.size());
  // at Done: still unclaimed at the finalized head
  after.Done(/*paid=*/true);
  UR_EXPECT_EQ(2u, route.reads);
  UR_EXPECT_EQ(1u, route.lines.size());
  clock.AdvanceTo(29'999);
  UR_EXPECT_EQ(2u, route.reads);
  // 30 s after Done: still unclaimed
  clock.AdvanceTo(30'000);
  UR_EXPECT_EQ(3u, route.reads);
  UR_EXPECT_EQ(1u, route.lines.size());
  // 60 s: the finalized head covers the claim, and the line goes
  clock.AdvanceTo(60'000);
  UR_EXPECT_EQ(4u, route.reads);
  UR_EXPECT_TRUE(route.lines.empty());
  // 120 s: the last read
  clock.AdvanceTo(120'000);
  UR_EXPECT_EQ(5u, route.reads);
  UR_EXPECT_TRUE(route.lines.empty());
  // and none after it
  clock.AdvanceTo(24 * 3'600'000);
  UR_EXPECT_EQ(5u, route.reads);
  UR_EXPECT_TRUE((clock.delays == std::vector<int>{30'000, 30'000, 60'000}));
}

// a claim that paid nothing reads the wins once, at Done, as before, and
// leaves the reads an earlier claim that paid is still owed
UR_TEST(SnLedgerCtf_TheWinsAfterAClaimThatPaidNothingAreReadOnce) {
  FakeWinsRoute route;
  route.answers = {{WonIn(10, "provider", "is")}};
  FakeClock clock;
  WinsAfterClaim after({[&route] { route.Read(); }, clock.Port()});
  after.Done(/*paid=*/false);
  UR_EXPECT_EQ(1u, route.reads);
  UR_EXPECT_EQ(1u, route.lines.size());
  clock.AdvanceTo(600'000);
  UR_EXPECT_EQ(1u, route.reads);
  UR_EXPECT_TRUE(clock.delays.empty());
  // a claim that paid at 600 s, then one 10 s later that failed before it
  // reached any account: its own read at Done, and the first one's reads at
  // 630, 660 and 720 s go on, the line going once the win reads claimed
  FakeWinsRoute paid;
  paid.answers = {{WonIn(10, "provider", "is")}, {WonIn(10, "provider", "is")},
                  {WonIn(10, "provider", "is")}, {}};
  WinsAfterClaim later({[&paid] { paid.Read(); }, clock.Port()});
  later.Done(/*paid=*/true);
  clock.AdvanceTo(610'000);
  later.Done(/*paid=*/false);
  UR_EXPECT_EQ(2u, paid.reads);
  clock.AdvanceTo(630'000);
  UR_EXPECT_EQ(3u, paid.reads);
  UR_EXPECT_EQ(1u, paid.lines.size());
  clock.AdvanceTo(660'000);
  UR_EXPECT_EQ(4u, paid.reads);
  UR_EXPECT_TRUE(paid.lines.empty());
  clock.AdvanceTo(720'000);
  UR_EXPECT_EQ(5u, paid.reads);
  clock.AdvanceTo(3'600'000);
  UR_EXPECT_EQ(5u, paid.reads);
}

// a sign-out stops the reads still to come, and the next claim's Done starts
// them over: the earlier claim's timers then read nothing
UR_TEST(SnLedgerCtf_TheWinsAfterAClaimStopOnSignOutAndStartOverWithTheNextClaim) {
  FakeWinsRoute route;
  route.answers = {{WonIn(10, "provider", "is")}};
  FakeClock clock;
  WinsAfterClaim after({[&route] { route.Read(); }, clock.Port()});
  after.Done(true);
  clock.AdvanceTo(30'000);
  UR_EXPECT_EQ(2u, route.reads);
  after.Stop();
  clock.AdvanceTo(600'000);
  UR_EXPECT_EQ(2u, route.reads);
  // a claim's Done at 600 s, and another's at 610 s
  after.Done(true);
  UR_EXPECT_EQ(3u, route.reads);
  clock.AdvanceTo(610'000);
  after.Done(true);
  UR_EXPECT_EQ(4u, route.reads);
  clock.AdvanceTo(630'000);
  UR_EXPECT_EQ(4u, route.reads);
  clock.AdvanceTo(640'000);
  UR_EXPECT_EQ(5u, route.reads);
  clock.AdvanceTo(670'000);
  UR_EXPECT_EQ(6u, route.reads);
  clock.AdvanceTo(730'000);
  UR_EXPECT_EQ(7u, route.reads);
  clock.AdvanceTo(3'600'000);
  UR_EXPECT_EQ(7u, route.reads);
}

// ---- a wins read that fails, and reads that overlap (every app, 2026-11-12) ---------------

// A read that fails keeps the lines of the last read that answered: only an
// answer that no longer lists a win takes its line away (and a sign-out, the
// page's ResetForSignOut and SettleAllEmpty). A session's first read draws
// none until it answers
UR_TEST(SnLedgerCtf_AWinsReadThatFailsKeepsTheLines) {
  const std::vector<Win> both = {WonIn(10, "provider", "is"), WonIn(9, "extender", "ke")};
  WinsLines wins;
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), both));
  UR_EXPECT_FALSE(wins.Apply(wins.Start(), std::nullopt));
  UR_EXPECT_EQ(2u, wins.Wins().size());
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), std::vector<Win>{}));
  UR_EXPECT_TRUE(wins.Wins().empty());
  // a failure between two reads that answer leaves the lines as they were
  FakeWinsRoute route;
  route.answers = {both, both, {WonIn(10, "provider", "is")}};
  route.failing = {1};
  route.Read();
  UR_EXPECT_TRUE(route.lines == WonLinesFor(both));
  route.Read();  // ctf_unavailable
  UR_EXPECT_EQ(2u, route.reads);
  UR_EXPECT_TRUE(route.lines == WonLinesFor(both));
  route.Read();  // the Kenya slot is no longer listed: its line goes
  UR_EXPECT_EQ(1u, route.lines.size());
  if (route.lines.size() == 1) UR_EXPECT_TRUE(route.lines[0] == (WonLine{WonSlot::Provider, "is"}));
  // the first read of a session fails: no line, until a read answers
  FakeWinsRoute signedIn;
  signedIn.answers = {both};
  signedIn.failing = {0};
  signedIn.Read();
  UR_EXPECT_TRUE(signedIn.lines.empty());
  signedIn.Read();
  UR_EXPECT_TRUE(signedIn.lines == WonLinesFor(both));
}

// the reads after a claim are reads like any other: the one at Done and the
// one 30 s after it fail, and the line stays; at 60 s the route answers with
// the win claimed, and the line goes
UR_TEST(SnLedgerCtf_TheReadsAfterAClaimThatFailKeepTheLines) {
  FakeWinsRoute route;
  route.answers = {{WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is", urnw::snledger::kClaimStateClaimed)}};
  route.failing = {1, 2};
  FakeClock clock;
  WinsAfterClaim after({[&route] { route.Read(); }, clock.Port()});
  route.Read();  // the page's load
  UR_EXPECT_EQ(1u, route.lines.size());
  after.Done(/*paid=*/true);
  UR_EXPECT_EQ(2u, route.reads);
  UR_EXPECT_EQ(1u, route.lines.size());
  clock.AdvanceTo(30'000);
  UR_EXPECT_EQ(3u, route.reads);
  UR_EXPECT_EQ(1u, route.lines.size());
  clock.AdvanceTo(60'000);
  UR_EXPECT_EQ(4u, route.reads);
  UR_EXPECT_TRUE(route.lines.empty());
  clock.AdvanceTo(120'000);
  UR_EXPECT_EQ(5u, route.reads);
  UR_EXPECT_TRUE(route.lines.empty());
}

// Reads overlap (each visit's load, a claim's Done and the reads after it) and
// their answers land in any order. The lines show the newest read's good
// answer, by the order the reads started (Apple's and Windows' too): an older
// answer that lands late never replaces a newer one, and a newer read that
// fails never keeps an older good answer still to land from showing. A sign-out
// drops every read from before it
UR_TEST(SnLedgerCtf_OverlappingWinsReadsShowTheNewestGoodAnswer) {
  const std::vector<Win> both = {WonIn(10, "provider", "is"), WonIn(9, "extender", "ke")};
  const std::vector<Win> iceland = {WonIn(10, "provider", "is")};
  WinsLines wins;
  // an older read's answer lands after a newer one's, in each order
  const uint64_t older = wins.Start();
  const uint64_t newer = wins.Start();
  UR_EXPECT_TRUE(wins.Apply(newer, iceland));
  UR_EXPECT_FALSE(wins.Apply(older, both));
  UR_EXPECT_EQ(1u, wins.Wins().size());
  const uint64_t first = wins.Start();
  const uint64_t second = wins.Start();
  UR_EXPECT_TRUE(wins.Apply(first, both));
  UR_EXPECT_TRUE(wins.Apply(second, iceland));
  UR_EXPECT_EQ(1u, wins.Wins().size());
  // a newer read fails first, then the older read's good answer lands: it shows
  const uint64_t slow = wins.Start();
  const uint64_t failed = wins.Start();
  UR_EXPECT_FALSE(wins.Apply(failed, std::nullopt));
  UR_EXPECT_TRUE(wins.Apply(slow, std::vector<Win>{}));
  UR_EXPECT_TRUE(wins.Wins().empty());
  // a read from before a sign-out shows nowhere after it, and the next
  // session's first read shows none until it answers
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), both));
  const uint64_t departed = wins.Start();
  wins.Reset();
  UR_EXPECT_TRUE(wins.Wins().empty());
  UR_EXPECT_FALSE(wins.Apply(departed, both));
  UR_EXPECT_TRUE(wins.Wins().empty());
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), iceland));
  UR_EXPECT_EQ(1u, wins.Wins().size());
}

// The read at Done waits on a slow route; the read 30 s after Done finds the
// win claimed and its line goes; the read at Done's late answer, which still
// lists it, is older and brings nothing back
UR_TEST(SnLedgerCtf_ASlowReadAtDoneNeverBringsAPaidLineBack) {
  const std::vector<Win> unclaimed = {WonIn(10, "provider", "is")};
  WinsLines wins;
  std::vector<std::pair<uint64_t, std::optional<std::vector<Win>>>> outstanding;
  FakeClock clock;
  size_t reads = 0;
  // the read at Done answers last; every other read answers at once, claimed
  WinsAfterClaim after({[&] {
                          const uint64_t read = wins.Start();
                          if (reads++ == 0) {
                            outstanding.emplace_back(read, unclaimed);
                            return;
                          }
                          wins.Apply(read, std::vector<Win>{});
                        },
                        clock.Port()});
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), unclaimed));  // the page's load
  after.Done(/*paid=*/true);
  UR_EXPECT_EQ(1u, wins.Wins().size());
  clock.AdvanceTo(30'000);
  UR_EXPECT_TRUE(wins.Wins().empty());
  UR_EXPECT_EQ(1u, outstanding.size());
  if (outstanding.size() == 1) {
    UR_EXPECT_FALSE(wins.Apply(outstanding[0].first, outstanding[0].second));
  }
  UR_EXPECT_TRUE(wins.Wins().empty());
}

// ---- a network's answer beats a client's (every app, 2026-11-12) --------------------

// The SDK reads with the network's credential, and with the client token when
// that read fails, and a server before the network's answer answers the client
// token alone: a client's answer (its ClientId set) lists that client's wins
// only. Once a network's answer has shown since the sign-in, a client's answer
// replaces nothing, as a read that failed; it shows only while no network's
// answer has, and a network's answer replaces it whichever read started first
UR_TEST(SnLedgerCtf_ANetworksAnswerBeatsAClients) {
  using urnw::snledger::WinsAnswer;
  const std::string client = "0198f1a2-7c55-7c3e-9a4b-0d3c2f1e5a6b";
  const std::vector<Win> both = {WonIn(10, "provider", "is"), WonIn(9, "extender", "ke")};
  const std::vector<Win> iceland = {WonIn(10, "provider", "is")};
  UR_EXPECT_TRUE(WinsAnswer(both).Network());
  UR_EXPECT_FALSE(WinsAnswer(iceland, client).Network());
  // a client's answer that lands after a network's leaves the network's lines
  // alone, newer or not, whatever it lists
  WinsLines wins;
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), WinsAnswer(both)));
  UR_EXPECT_FALSE(wins.Apply(wins.Start(), WinsAnswer(iceland, client)));
  UR_EXPECT_FALSE(wins.Apply(wins.Start(), WinsAnswer({}, client)));
  UR_EXPECT_TRUE(WonLinesFor(wins.Wins()) == WonLinesFor(both));
  // the network's next answer replaces its last
  UR_EXPECT_TRUE(wins.Apply(wins.Start(), WinsAnswer(iceland)));
  UR_EXPECT_EQ(1u, wins.Wins().size());
  // a client's answers alone, as from a server before the network's answer,
  // show their lines; a failure between them keeps them
  WinsLines old;
  UR_EXPECT_TRUE(old.Apply(old.Start(), WinsAnswer(both, client)));
  UR_EXPECT_TRUE(WonLinesFor(old.Wins()) == WonLinesFor(both));
  UR_EXPECT_FALSE(old.Apply(old.Start(), std::nullopt));
  UR_EXPECT_TRUE(WonLinesFor(old.Wins()) == WonLinesFor(both));
  UR_EXPECT_TRUE(old.Apply(old.Start(), WinsAnswer(iceland, client)));
  UR_EXPECT_EQ(1u, old.Wins().size());
  // the network's read is slow and a newer read fell back to the client
  // token: the client's answer shows first, an older client's never replaces
  // it, and the network's replaces it though its read started first
  WinsLines overlap;
  const uint64_t network = overlap.Start();
  const uint64_t older = overlap.Start();
  const uint64_t fallback = overlap.Start();
  UR_EXPECT_TRUE(overlap.Apply(fallback, WinsAnswer(iceland, client)));
  UR_EXPECT_FALSE(overlap.Apply(older, WinsAnswer({}, client)));
  UR_EXPECT_EQ(1u, overlap.Wins().size());
  UR_EXPECT_TRUE(overlap.Apply(network, WinsAnswer(both)));
  UR_EXPECT_TRUE(WonLinesFor(overlap.Wins()) == WonLinesFor(both));
  // a sign-out starts over: no lines, no answer of a read from before, and a
  // client's answer shows again until a network's does
  const uint64_t departed = overlap.Start();
  overlap.Reset();
  UR_EXPECT_TRUE(overlap.Wins().empty());
  UR_EXPECT_FALSE(overlap.Apply(departed, WinsAnswer(both)));
  UR_EXPECT_TRUE(overlap.Apply(overlap.Start(), WinsAnswer(iceland, client)));
  UR_EXPECT_EQ(1u, overlap.Wins().size());
  UR_EXPECT_TRUE(overlap.Apply(overlap.Start(), WinsAnswer(std::vector<Win>{})));
  UR_EXPECT_FALSE(overlap.Apply(overlap.Start(), WinsAnswer(iceland, client)));
  UR_EXPECT_TRUE(overlap.Wins().empty());
}

// ---- the v2 claim in flight: the session's, not a load's (owner, 2026-11-12) -----------

// what a press paid (Android's snLedgerClaimPaid): an account claimed, or
// claimed already by another claim, which vault2 paid to the same coldkey;
// each press starts unpaid
UR_TEST(SnLedgerCtf_APressPaysWhenAnAccountIsClaimed) {
  ClaimProgress progress = CtfSheet(615'000'000);
  UR_EXPECT_FALSE(progress.Paid());
  progress.Start();
  progress.Sent(1, 42, "0xba7c");
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7, "needs_gas: the gas key holds 0.004 TAO"));
  UR_EXPECT_FALSE(progress.Failed(0, 0, "chain_rpc_unreachable: dial tcp: i/o timeout"));
  UR_EXPECT_TRUE(progress.Failed(1, 42, "claim_failed: the claim transaction reverted"));
  progress.Done();
  UR_EXPECT_FALSE(progress.Paid());
  progress.Start();
  progress.Confirmed(1, 42, "0xba7d", 3'241'000'000);
  UR_EXPECT_TRUE(progress.Paid());
  progress.Done();
  UR_EXPECT_TRUE(progress.Paid());  // until the next press
  progress.Start();
  UR_EXPECT_FALSE(progress.Paid());
  UR_EXPECT_TRUE(progress.Failed(kCtfPool, 7, "already_claimed: NothingToClaim: another claim paid this account first"));
  UR_EXPECT_TRUE(progress.Paid());
}

// Each visit to Earnings loads the page again, and a load drops every answer
// armed for the load before it, as a claim's answers were. They count by the
// session's ClaimInFlight instead, which no load touches (the page's gate is
// SnLedgerCtf_TheClaimIsTheSessionsNotALoads): the claim's calls through a
// user who closes the sheet during a claim, leaves Earnings and comes back. The
// claim is still in flight, a sheet opened then draws its rows, and its events
// and its Done land, after which the wins are read on the apps' schedule
UR_TEST(SnLedgerCtf_AClaimOutlivesLeavingEarningsAndComingBack) {
  ClaimInFlight claim;
  UR_EXPECT_FALSE(claim.InFlight());
  UR_EXPECT_TRUE(claim.Rows() == nullptr);
  FakeWinsRoute route;
  route.answers = {{WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is")},
                   {WonIn(10, "provider", "is", urnw::snledger::kClaimStateClaimed)}};
  FakeClock clock;
  WinsAfterClaim after({[&route] { route.Read(); }, clock.Port()});
  route.Read();  // the visit's read
  // the sheet on the accounts as read, and its Claim
  auto sheet = std::make_shared<ClaimProgress>(CtfSheet(615'000'000));
  const uint64_t ticket = claim.Start(sheet);
  UR_EXPECT_TRUE(claim.InFlight() && claim.Current(ticket));
  UR_EXPECT_TRUE(claim.Rows() == sheet && sheet->Busy());
  claim.Rows()->Sent(1, 42, "0xba7c");
  // the user closes the sheet, leaves Earnings and comes back: the page loads
  // again, which reads the wins and touches nothing of the claim
  sheet.reset();
  route.Read();
  UR_EXPECT_TRUE(claim.InFlight() && claim.Current(ticket));
  claim.Rows()->Sent(kCtfPool, 7, "0xba7c");
  claim.Rows()->Confirmed(1, 42, "0xba7c", 3'241'000'000);
  // a sheet opened now draws the claim in flight: its rows, and no Claim yet
  const std::shared_ptr<ClaimProgress> reopened = claim.Rows();
  UR_EXPECT_TRUE(reopened != nullptr && reopened->Busy());
  UR_EXPECT_TRUE(RowOf(*reopened, 1, 42)->phase == Phase::Claimed);
  UR_EXPECT_TRUE(RowOf(*reopened, kCtfPool, 7)->phase == Phase::Sent);
  UR_EXPECT_EQ(int64_t{0}, reopened->ClaimableRao());
  // its last event and its Done land: it paid, and the wins are read at Done
  // and again on the schedule until the server reads the win claimed
  UR_EXPECT_TRUE(claim.Current(ticket));
  claim.Rows()->Confirmed(kCtfPool, 7, "0xba7c", 615'000'000);
  UR_EXPECT_TRUE(claim.Current(ticket));
  const bool paid = claim.End();
  UR_EXPECT_TRUE(paid);
  after.Done(paid);
  UR_EXPECT_FALSE(claim.InFlight() || claim.Current(ticket));
  UR_EXPECT_FALSE(reopened->Busy());
  UR_EXPECT_TRUE(RowOf(*reopened, kCtfPool, 7)->phase == Phase::Claimed);
  UR_EXPECT_EQ(3u, route.reads);
  UR_EXPECT_EQ(1u, route.lines.size());
  clock.AdvanceTo(30'000);
  UR_EXPECT_EQ(4u, route.reads);
  UR_EXPECT_TRUE(route.lines.empty());
  // a sheet opened after it draws the accounts read again, and the next claim
  // is a claim of its own
  UR_EXPECT_TRUE(claim.Rows() == nullptr);
  UR_EXPECT_FALSE(claim.End());
  const uint64_t next = claim.Start(std::make_shared<ClaimProgress>(CtfSheet(615'000'000)));
  UR_EXPECT_TRUE(next != ticket && claim.Current(next) && !claim.Current(ticket));
}

// A sign-out drops the claim in flight: nothing of it counts any more, and the
// next account's claim is one of its own. The watchdog's give-up ends a claim
// too, for good: the press is over, and a late answer counts for nothing
UR_TEST(SnLedgerCtf_ASignOutOrTheWatchdogEndsTheClaimInFlight) {
  ClaimInFlight claim;
  const uint64_t departed = claim.Start(std::make_shared<ClaimProgress>(CtfSheet(615'000'000)));
  claim.Rows()->Sent(1, 42, "0xba7c");
  claim.Drop();
  UR_EXPECT_FALSE(claim.InFlight() || claim.Current(departed));
  UR_EXPECT_TRUE(claim.Rows() == nullptr);
  UR_EXPECT_FALSE(claim.End());  // its Done reads nothing again
  auto next = std::make_shared<ClaimProgress>(TwoPoolSheet());
  const uint64_t ticket = claim.Start(next);
  UR_EXPECT_TRUE(claim.Current(ticket) && !claim.Current(departed));
  claim.Rows()->Sent(1, 42, "0xabc1");
  // the watchdog
  UR_EXPECT_FALSE(claim.End());
  UR_EXPECT_FALSE(claim.Current(ticket));
  UR_EXPECT_FALSE(next->Busy());
  UR_EXPECT_TRUE(RowOf(*next, 1, 42)->phase == Phase::Sent);
  UR_EXPECT_TRUE(RowOf(*next, 3, 5)->phase == Phase::Claimable);
}

// The v1 claim's in-flight state is a ClaimInFlight without rows (its sheet
// keeps its own): its answers count by its ticket while it is in flight, and
// only its Done, its watchdog's give-up and a sign-out end it. That no load
// touches it, so leaving Earnings and coming back keeps it, is the page's
// wiring's (SnLedgerCtf_TheV1ClaimIsTheSessionsNotALoads: its answers check
// only the ticket, never the load's epoch, and no load names claim_). Here:
// in flight it holds the page's Claiming, as a v2 claim does, and its Done
// ends it, rather than the watchdog
UR_TEST(SnLedgerCtf_AV1ClaimOutlivesLeavingEarningsAndComingBack) {
  ClaimInFlight claim;
  ClaimInFlight ledgerClaim;
  const auto claiming = [&] { return claim.InFlight() || ledgerClaim.InFlight(); };
  UR_EXPECT_FALSE(claiming());
  const uint64_t ticket = claim.Start();
  UR_EXPECT_TRUE(claim.InFlight() && claim.Current(ticket));
  UR_EXPECT_TRUE(claim.Rows() == nullptr);
  // a v2 Claim waits for it: the gas key sends one claim at a time
  UR_EXPECT_TRUE(claiming());
  // its Done lands and ends it: Claim is offered again, and a v1 claim pays no
  // v2 account, so the wins are not read on its account
  UR_EXPECT_FALSE(claim.End());
  UR_EXPECT_FALSE(claiming() || claim.Current(ticket));
  UR_EXPECT_FALSE(claim.End());
  // the watchdog's give-up is final: a late answer counts for nothing
  const uint64_t next = claim.Start();
  UR_EXPECT_TRUE(next != ticket && claim.Current(next) && !claim.Current(ticket));
  UR_EXPECT_FALSE(claim.End());
  UR_EXPECT_FALSE(claim.Current(next));
  // a sign-out drops one in flight, and the next account's claim is its own
  const uint64_t departed = claim.Start();
  claim.Drop();
  UR_EXPECT_FALSE(claim.InFlight() || claim.Current(departed));
  UR_EXPECT_TRUE(claim.Current(claim.Start()));
  // and a v2 claim in flight holds a v1 Claim the same way
  claim.Drop();
  ledgerClaim.Start(std::make_shared<ClaimProgress>(CtfSheet(615'000'000)));
  UR_EXPECT_TRUE(claiming() && !claim.InFlight());
}

// The v1 claim's watchdog waits past the SDK's own bounds, as the v2 claim's
// does: the SDK claims the epochs one after another, each within 6 minutes (its
// reads, its transaction and up to 4 minutes for the receipt). The 3 minutes
// it waited before gave up on a claim of one slow receipt, or of several
// epochs, while the SDK still sent, and offered Claim again: a second claim of
// a pending epoch sends again, and reverts
UR_TEST(SnLedgerCtf_TheV1WatchdogOutlastsEveryEpochTheSdkClaims) {
  using urnw::snledger::EpochClaimWatchdogMs;
  constexpr int64_t kSdkEpochMs = 6 * 60'000;
  for (size_t epochs : {size_t{1}, size_t{2}, size_t{8}, size_t{40}}) {
    UR_EXPECT_TRUE(EpochClaimWatchdogMs(epochs) > static_cast<int64_t>(epochs) * kSdkEpochMs);
  }
  UR_EXPECT_EQ(7 * 60'000, EpochClaimWatchdogMs(1));
  UR_EXPECT_EQ(49 * 60'000, EpochClaimWatchdogMs(8));
  // no epoch is the SDK's one pass all the same
  UR_EXPECT_EQ(EpochClaimWatchdogMs(1), EpochClaimWatchdogMs(0));
  // never past what a timer takes
  UR_EXPECT_EQ(std::numeric_limits<int>::max(), EpochClaimWatchdogMs(size_t{1} << 40));
}

// A v1 sheet opened during the v1 claim draws it in progress (owner,
// 2026-11-12: a sheet opened again shows the claim; none of its events is
// lost when its sheet is closed). The sheet keeps its own rows, so the page
// keeps the claim's epochs and its answers, in order, for a sheet opened
// later to replay; the claim's end drops them, and the next sheet reads the
// claims again
UR_TEST(SnLedgerCtf_AV1SheetOpenedDuringTheClaimDrawsItsAnswers) {
  using urnw::snledger::EpochClaimAnswers;
  EpochClaimAnswers answers;
  answers.Start({1218, 1217});
  answers.Sent(1218, "0xa1");
  answers.Confirmed(1218, "0xa1", 3'241'000'000);
  answers.Failed(1217, "needs_gas: the gas key holds 0.004 TAO");
  UR_EXPECT_TRUE((answers.Epochs() == std::vector<int64_t>{1218, 1217}));
  UR_EXPECT_EQ(3u, answers.Answers().size());
  if (answers.Answers().size() == 3) {
    const auto& sent = answers.Answers()[0];
    UR_EXPECT_TRUE(sent.kind == EpochClaimAnswers::Answer::Kind::Sent && sent.epoch == 1218 &&
                   sent.txHash == "0xa1");
    const auto& confirmed = answers.Answers()[1];
    UR_EXPECT_TRUE(confirmed.kind == EpochClaimAnswers::Answer::Kind::Confirmed && confirmed.epoch == 1218 &&
                   confirmed.txHash == "0xa1" && confirmed.amountRao == 3'241'000'000);
    const auto& failed = answers.Answers()[2];
    UR_EXPECT_TRUE(failed.kind == EpochClaimAnswers::Answer::Kind::Failed && failed.epoch == 1217 &&
                   failed.message == "needs_gas: the gas key holds 0.004 TAO");
  }
  // the next claim starts afresh, and the claim's end drops them
  answers.Start({1217});
  UR_EXPECT_TRUE(answers.Answers().empty() && answers.Epochs().size() == 1);
  answers.Clear();
  UR_EXPECT_TRUE(answers.Answers().empty() && answers.Epochs().empty());
}


// A failure about no account (noId 0) leaves the rows as they are: the sheet
// says it, and so does a sheet opened on the claim after it (it was closed when
// the failure came); the next press starts without it
UR_TEST(SnLedgerCtf_AFailureAboutNoAccountStaysWithTheClaim) {
  ClaimProgress progress = CtfSheet(615'000'000);
  progress.Start();
  UR_EXPECT_TRUE(progress.FailureMessage().empty());
  UR_EXPECT_FALSE(progress.Failed(0, 0, "chain_rpc_unreachable: dial tcp: i/o timeout"));
  UR_EXPECT_TRUE(progress.FailureMessage() == "chain_rpc_unreachable: dial tcp: i/o timeout");
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Sending);
  progress.Done();
  UR_EXPECT_TRUE(progress.FailureMessage() == "chain_rpc_unreachable: dial tcp: i/o timeout");
  progress.Start();
  UR_EXPECT_TRUE(progress.FailureMessage().empty());
  // an account's failure is its row's, not the sheet's
  UR_EXPECT_TRUE(progress.Failed(1, 42, "claim_failed: the claim transaction reverted"));
  UR_EXPECT_TRUE(progress.FailureMessage().empty());
}

// ---- the claim sheet's figure (as Apple's, Windows' and Android's) ----------------------

// The figure is what a press claims while an account above the floor is left,
// or a true zero once nothing is left at all; there is none while the rows left
// are below the claim minimum, in a pool that could not be read, of an amount
// not known yet, or in flight, and then Claim names no amount (ClaimableRao is
// 0), never "Claim 0". An account under the floor never counts, whatever the
// SDK said of it since
UR_TEST(SnLedgerCtf_TheSheetHasNoFigureWhileNothingAboveTheFloorIsLeft) {
  ClaimProgress above = CtfSheet(615'000'000);
  UR_EXPECT_TRUE(above.TotalKnown());
  UR_EXPECT_EQ(int64_t{3'241'000'000 + 615'000'000}, above.ClaimableRao());
  // a lone account under the floor, and a lone pool that could not be read
  const ClaimProgress lone({CtfAccount(20'000'000)}, {}, kFloor);
  UR_EXPECT_FALSE(lone.TotalKnown());
  UR_EXPECT_EQ(int64_t{0}, lone.ClaimableRao());
  const ClaimProgress unread({}, {Unreadable(kCtfPool, urnw::snledger::kKindCtf)}, kFloor);
  UR_EXPECT_FALSE(unread.TotalKnown());
  // nothing at all, as a read that answered says: a true zero (the page opens
  // no sheet before a read answers: SnLedgerCtf_TheSheetsFigureIsWired)
  const ClaimProgress none({}, {}, kFloor);
  UR_EXPECT_TRUE(none.TotalKnown());
  UR_EXPECT_EQ(int64_t{0}, none.ClaimableRao());
  // the pool's account above the floor, capture the flag's under it
  ClaimProgress claim = CtfSheet(20'000'000);
  UR_EXPECT_TRUE(claim.TotalKnown());
  UR_EXPECT_EQ(int64_t{3'241'000'000}, claim.ClaimableRao());
  claim.Start();
  UR_EXPECT_FALSE(claim.TotalKnown());  // in flight
  claim.Sent(1, 42, "0xba7c");
  claim.Confirmed(1, 42, "0xba7c", 3'241'000'000);
  // the SDK tried the one under the floor too, and the gas key fell short:
  // claimable again, and still under the floor
  UR_EXPECT_TRUE(claim.Failed(kCtfPool, 7, "needs_gas: the gas key holds 0.004 TAO"));
  claim.Done();
  UR_EXPECT_TRUE(RowOf(claim, kCtfPool, 7)->phase == Phase::Claimable);
  UR_EXPECT_FALSE(claim.TotalKnown());
  UR_EXPECT_EQ(int64_t{0}, claim.ClaimableRao());
  // every row claimed: a true zero
  ClaimProgress all = CtfSheet(615'000'000);
  all.Start();
  all.Confirmed(1, 42, "0xba7c", 3'241'000'000);
  all.Confirmed(kCtfPool, 7, "0xba7c", 615'000'000);
  all.Done();
  UR_EXPECT_TRUE(all.TotalKnown());
  UR_EXPECT_EQ(int64_t{0}, all.ClaimableRao());
  // an account the SDK reported that the sheet did not list: amount not known
  ClaimProgress unknown({}, {}, kFloor);
  unknown.Start();
  UR_EXPECT_TRUE(unknown.Failed(2, 9, "needs_gas: the gas key holds 0.004 TAO"));
  unknown.Done();
  UR_EXPECT_TRUE(RowOf(unknown, 2, 9) != nullptr);
  UR_EXPECT_FALSE(unknown.TotalKnown());
}

// While a press is out the sheet's figure is what it is moving (every app,
// 2026-11-12), as Apple's and Windows' sheets show it: the accounts it sends of
// a known amount above the floor, the amount Claim named. It never counts an
// account under the floor, in a pool that could not be read or of an amount not
// known, it holds through the press whatever the SDK says of an account, and a
// press of nothing known shows none, never a zero. Between presses the figure
// is TotalKnown's: what a press claims, or a true zero
UR_TEST(SnLedgerCtf_TheFigureDuringAClaimIsWhatItMoves) {
  // the pool's account above the floor, capture the flag's under it, and a
  // pool that could not be read
  ClaimProgress claim = CtfSheet(20'000'000, {Unreadable(5, urnw::snledger::kKindPool)});
  UR_EXPECT_TRUE(claim.Figure() == std::optional<int64_t>(3'241'000'000));
  UR_EXPECT_EQ(int64_t{0}, claim.MovingRao());
  claim.Start();
  UR_EXPECT_TRUE(claim.Busy());
  UR_EXPECT_EQ(int64_t{3'241'000'000}, claim.MovingRao());
  UR_EXPECT_TRUE(claim.Figure() == std::optional<int64_t>(3'241'000'000));
  // the SDK's events move none of it: the account sent, the one under the
  // floor tried and failed, then the account claimed
  claim.Sent(1, 42, "0xba7c");
  UR_EXPECT_TRUE(claim.Failed(kCtfPool, 7, "claim_failed: the claim transaction reverted"));
  UR_EXPECT_TRUE(claim.Figure() == std::optional<int64_t>(3'241'000'000));
  claim.Confirmed(1, 42, "0xba7c", 3'240'999'999);
  UR_EXPECT_TRUE(claim.Figure() == std::optional<int64_t>(3'241'000'000));
  claim.Done();
  // after it: nothing above the floor left, so no figure
  UR_EXPECT_EQ(int64_t{0}, claim.MovingRao());
  UR_EXPECT_FALSE(claim.Figure().has_value());
  // a row that fails mid-press is what the next press would claim, but the
  // figure stays what this press moves until its Done
  ClaimProgress two = TwoPoolSheet();
  two.Start();
  UR_EXPECT_TRUE(two.Failed(3, 5, "claim_failed: the claim transaction reverted"));
  UR_EXPECT_TRUE(two.TotalKnown());
  UR_EXPECT_EQ(int64_t{700'000'000}, two.ClaimableRao());
  UR_EXPECT_TRUE(two.Figure() == std::optional<int64_t>(3'941'000'000));
  two.Done();
  UR_EXPECT_TRUE(two.Figure() == std::optional<int64_t>(3'941'000'000));
  // a press of only an account whose amount is not known: no figure, never 0
  ClaimProgress unknown({}, {}, kFloor);
  unknown.Start();
  UR_EXPECT_TRUE(unknown.Failed(2, 9, "needs_gas: the gas key holds 0.004 TAO"));
  unknown.Done();
  unknown.Start();
  UR_EXPECT_TRUE(unknown.Busy());
  UR_EXPECT_FALSE(unknown.Figure().has_value());
  // a press with nothing under the floor counts everything it sends
  ClaimProgress whole = CtfSheet(615'000'000);
  whole.Start();
  UR_EXPECT_TRUE(whole.Figure() == std::optional<int64_t>(3'241'000'000 + 615'000'000));
}

// ---- the reads of the claimable state (owner, 2026-11-12) ----------------------------

// The page's reads of the claims, the accounts and the gas overlap, and their
// answers land in any order: only the newest read of a kind counts, whichever
// answer lands first, each kind on its own, and an answer that needs no read
// (no wallet) drops the reads still out
UR_TEST(SnLedgerCtf_OnlyTheNewestReadOfAKindCounts) {
  NewestRead claims;
  NewestRead accounts;
  const uint64_t visit = claims.Start();
  UR_EXPECT_TRUE(claims.Current(visit));
  const uint64_t done = claims.Start();
  // the newer read's answer counts, landing first or last; the older one's never
  UR_EXPECT_TRUE(claims.Current(done));
  UR_EXPECT_FALSE(claims.Current(visit));
  UR_EXPECT_TRUE(claims.Current(done));
  UR_EXPECT_FALSE(claims.Current(visit));
  // another kind's reads move none of these
  const uint64_t ledger = accounts.Start();
  UR_EXPECT_TRUE(accounts.Current(ledger));
  UR_EXPECT_TRUE(claims.Current(done));
  accounts.Start();
  UR_EXPECT_TRUE(claims.Current(done));
  UR_EXPECT_FALSE(accounts.Current(ledger));
  // no wallet: the empty layer needs no read, and the read still out drops
  claims.Start();
  UR_EXPECT_FALSE(claims.Current(done));
}

// The page's claims and accounts as EarningsPage::LoadWalletLayer and
// ApplySnWallet move them (their wiring is
// SnLedgerCtf_TheReadsOfTheClaimableStateAreWired): Loading from a read's
// start, keeping the last total, as ApplyClaims(Loading) does; then the newest
// read's answer, and no older one's. Its Claims are the page's with no claim
// in flight: the v1 Claim of the unclaimed tile and the payout line
// (snpayout::V1ClaimEnabled), and the v2 tile's (SectionFor)
struct WalletLayer {
  NewestRead claimsReads;
  NewestRead ledgerReads;
  bool claimsRead = false;  // the claims read answered: claimsState_ is Ready
  int64_t totalClaimableRao = 0;
  Load ledgerLoad = Load::Loading;
  std::vector<Account> accounts;

  // LoadWalletLayer: the claims and the accounts Loading, and a read of each
  std::pair<uint64_t, uint64_t> Read() {
    claimsRead = false;
    ledgerLoad = Load::Loading;
    accounts.clear();
    return {claimsReads.Start(), ledgerReads.Start()};
  }
  void ClaimsAnswer(uint64_t read, int64_t total) {
    if (!claimsReads.Current(read)) return;
    claimsRead = true;
    totalClaimableRao = total;
  }
  void AccountsAnswer(uint64_t read, std::vector<Account> answer) {
    if (!ledgerReads.Current(read)) return;
    ledgerLoad = Load::Ready;
    accounts = std::move(answer);
  }
  // ApplySnWallet with no coldkey: nothing to read, at once
  void NoWallet() {
    claimsReads.Start();
    ledgerReads.Start();
    claimsRead = true;
    totalClaimableRao = 0;
    ledgerLoad = Load::Ready;
    accounts.clear();
  }
  bool V1Claim() const {
    return urnw::snpayout::V1ClaimEnabled(/*v1InFlight=*/false, claimsRead, totalClaimableRao,
                                          /*claiming=*/false);
  }
  bool V2Claim() const {
    return SectionFor(true, ledgerLoad, accounts, {}, 0, /*claimConfigured=*/true,
                      /*claiming=*/false, Tag)
        .claimEnabled;
  }
};

// The race (owner, 2026-11-12: "a stale subnet read can land after the read at
// Done"). The user comes back to Earnings as a claim of epoch 7 (1 α) and of
// the pool's account runs: the visit's read starts during the claim and is
// slow. The claim's Done reads again, and that read finds what the claim paid
// gone. The visit's read, which still lists it, lands last: it is dropped, so
// the page keeps the Done's answer and neither Claim is offered on what the
// claim paid. Landing before the Done's answer instead, it is dropped too: the
// page stays Loading and both Claims wait until the Done's read answers, though
// the total the page still holds is the claim's
UR_TEST(SnLedgerCtf_AReadFromBeforeAClaimsDoneNeverLandsOverIt) {
  const Account paid = LedgerAccount(1, 42, 5'120'000'000, 5'120'000'000);
  for (const bool visitAnswersFirst : {false, true}) {
    WalletLayer page;
    // the read the claim was pressed on: epoch 7 and the pool's account
    const auto pressed = page.Read();
    page.ClaimsAnswer(pressed.first, 1'000'000'000);
    page.AccountsAnswer(pressed.second, {OnePool()});
    UR_EXPECT_TRUE(page.V1Claim());
    UR_EXPECT_TRUE(page.V2Claim());
    // during the claim the user comes back to Earnings: the visit's read
    const auto visit = page.Read();
    // the claim's Done reads again
    const auto done = page.Read();
    UR_EXPECT_FALSE(page.V1Claim());
    UR_EXPECT_FALSE(page.V2Claim());
    if (visitAnswersFirst) {
      page.ClaimsAnswer(visit.first, 1'000'000'000);
      page.AccountsAnswer(visit.second, {OnePool()});
      UR_EXPECT_FALSE(page.claimsRead);
      UR_EXPECT_TRUE(page.ledgerLoad == Load::Loading);
      UR_EXPECT_EQ(int64_t{1'000'000'000}, page.totalClaimableRao);
      UR_EXPECT_FALSE(page.V1Claim());
      UR_EXPECT_FALSE(page.V2Claim());
    }
    page.ClaimsAnswer(done.first, 0);
    page.AccountsAnswer(done.second, {paid});
    UR_EXPECT_TRUE(page.claimsRead);
    UR_EXPECT_TRUE(page.ledgerLoad == Load::Ready);
    if (!visitAnswersFirst) {
      page.ClaimsAnswer(visit.first, 1'000'000'000);
      page.AccountsAnswer(visit.second, {OnePool()});
    }
    UR_EXPECT_EQ(int64_t{0}, page.totalClaimableRao);
    UR_EXPECT_TRUE(page.accounts.size() == 1 && page.accounts[0].claimableRao == 0);
    UR_EXPECT_FALSE(page.V1Claim());
    UR_EXPECT_FALSE(page.V2Claim());
    // the next read still offers what is left to claim
    const auto next = page.Read();
    page.ClaimsAnswer(next.first, 500'000'000);
    page.AccountsAnswer(next.second, {OnePool()});
    UR_EXPECT_TRUE(page.V1Claim());
    UR_EXPECT_TRUE(page.V2Claim());
  }
}

// The same with two claims: a v2 claim's Done reads again, and while that read
// is out a v1 sheet left open during the v2 claim claims epoch 7. The v1
// claim's Done reads again, and the v2 claim's read, which started before the
// v1 claim and still lists epoch 7 as claimable, lands last: it is dropped
UR_TEST(SnLedgerCtf_AReadFromBeforeTheLastOfTwoClaimsIsDropped) {
  const Account paid = LedgerAccount(1, 42, 5'120'000'000, 5'120'000'000);
  WalletLayer page;
  const auto pressed = page.Read();
  page.ClaimsAnswer(pressed.first, 1'000'000'000);
  page.AccountsAnswer(pressed.second, {OnePool()});
  const auto v2Done = page.Read();
  const auto v1Done = page.Read();
  page.ClaimsAnswer(v1Done.first, 0);
  page.AccountsAnswer(v1Done.second, {paid});
  UR_EXPECT_FALSE(page.V1Claim());
  page.ClaimsAnswer(v2Done.first, 1'000'000'000);
  page.AccountsAnswer(v2Done.second, {paid});
  UR_EXPECT_EQ(int64_t{0}, page.totalClaimableRao);
  UR_EXPECT_FALSE(page.V1Claim());
  UR_EXPECT_FALSE(page.V2Claim());
}

// A wallet read that finds no coldkey empties the claims and the accounts at
// once, an answer that needs no read: a read still out for the coldkey before
// (a claim's Done's, say) is dropped when it lands, and puts nothing back
UR_TEST(SnLedgerCtf_NoColdkeyDropsTheReadsStillOut) {
  WalletLayer page;
  const auto out = page.Read();
  page.NoWallet();
  page.ClaimsAnswer(out.first, 1'000'000'000);
  page.AccountsAnswer(out.second, {OnePool()});
  UR_EXPECT_EQ(int64_t{0}, page.totalClaimableRao);
  UR_EXPECT_TRUE(page.accounts.empty());
  UR_EXPECT_FALSE(page.V1Claim());
  UR_EXPECT_FALSE(page.V2Claim());
}

// ---- the page's wiring and English, read from the sources ----------------------

std::string ReadLedgerSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The body of the function whose definition starts with `signature`, or ""
// (braces balanced from the first '{' after the signature).
std::string LedgerFunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = source.find('{', at);
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') ++depth;
    if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
  }
  return std::string();
}

// Every English fallback passed to T_("key", ...) in `source`: the adjacent
// string literals after the key, joined.
std::vector<std::string> LedgerFallbacks(const std::string& source, const std::string& key) {
  std::vector<std::string> out;
  const std::string open = "T_(\"" + key + "\",";
  for (size_t at = source.find(open); at != std::string::npos; at = source.find(open, at + 1)) {
    std::string text;
    size_t p = at + open.size();
    while (p < source.size()) {
      while (p < source.size() && (source[p] == ' ' || source[p] == '\n')) ++p;
      if (p >= source.size() || source[p] != '"') break;
      for (++p; p < source.size() && source[p] != '"'; ++p) {
        if (source[p] == '\\' && p + 1 < source.size()) ++p;
        text += source[p];
      }
      ++p;
    }
    out.push_back(text);
  }
  return out;
}

// CL-2: the tile draws SectionFor's decision, and its Claim opens the v2 sheet
UR_TEST(SnLedgerPageDrawsTheDecision) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  UR_EXPECT_TRUE(!page.empty());
  const std::string tile = LedgerFunctionBody(page, "void EarningsPage::RebuildLedgerTile()");
  UR_EXPECT_TRUE(tile.find("snledger::SectionFor(") != std::string::npos);
  // a v2 claim in flight leaves Claim enabled, to open its sheet
  // (SnLedgerCtf_ClaimOpensTheV2ClaimInFlight); a v1 claim's holds it
  UR_EXPECT_TRUE(tile.find("TransferFloorRao(), ledgerConfigured_, Claiming(), FormatAlphaRao,\n"
                           "                           ledgerClaim_.InFlight());") != std::string::npos);
  UR_EXPECT_TRUE(page.find("ledgerClaimButton_->signal_clicked().connect(\n"
                           "        sigc::mem_fun(*this, &EarningsPage::OnLedgerClaim));") !=
                 std::string::npos);
  // the v1 tile keeps its own Claim
  UR_EXPECT_TRUE(page.find("claimButton_->signal_clicked().connect(sigc::mem_fun(*this, "
                           "&EarningsPage::OnClaim));") != std::string::npos);
}

// APPS-SPEC.md (CL-4): the GUI is unprivileged, so the v2 read and the v2
// claim go through the daemon's DeviceRemote like the v1 claim does, and
// "configured" is the chain settings' v2 vault
UR_TEST(SnLedgerReadsAndClaimsThroughTheDeviceRemote) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string read =
      LedgerFunctionBody(page, "void FetchLedger(SdkHost& host, LedgerDone done)");
  UR_EXPECT_TRUE(read.find("urnet::DeviceRemote& device = host.device();") != std::string::npos);
  UR_EXPECT_TRUE(read.find("device.syncSnWallet(") != std::string::npos);
  UR_EXPECT_TRUE(read.find("device.snLedgerAccounts(") != std::string::npos);
  UR_EXPECT_TRUE(read.find("!settings->vault2_address.empty()") != std::string::npos);
  const std::string claim = LedgerFunctionBody(
      page, "void LedgerClaim(SdkHost& host, urnet::SnLedgerClaimCallback callback)");
  UR_EXPECT_TRUE(claim.find("host.device().snLedgerClaim(") != std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()")
                     .find("sn::LedgerClaim(host_, std::move(callback));") != std::string::npos);
}

// APPS-SPEC.md (CL-4): Sent links the explorer, needs_gas uses the v1 top-up
// at a v2 claim's gas, and Done reads the accounts again
UR_TEST(SnLedgerClaimSheetWiring) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string open = LedgerFunctionBody(page, "void EarningsPage::OnLedgerClaim()");
  UR_EXPECT_TRUE(open.find("return sn::ExplorerTxUrl(host_, txHash);") != std::string::npos);
  UR_EXPECT_TRUE(open.find("sheet->on_claim = [this] { StartLedgerClaim(); };") !=
                 std::string::npos);
  const std::string rebuild = LedgerFunctionBody(page, "void LedgerClaimSheet::Rebuild()");
  UR_EXPECT_TRUE(rebuild.find("MakeLinkLabel(ShortHex(row.txHash), url, on_open_link)") !=
                 std::string::npos);
  // the v1 sheet's gas panel, at what a v2 claim needs
  UR_EXPECT_TRUE(rebuild.find("BuildGasPanel(*gasPanel_, *this, gas_, NeedsGas(), "
                              "snledger::kClaimGasTao, copiedReset_);") != std::string::npos);
  UR_EXPECT_TRUE(page.find("BuildGasPanel(*gasPanel_, *this, gas_, NeedsGas(), kSuggestedGasTao, "
                           "copiedReset_);") != std::string::npos);
  const std::string needsGas = LedgerFunctionBody(page, "bool LedgerClaimSheet::NeedsGas() const");
  UR_EXPECT_TRUE(needsGas.find("return snledger::NeedsGas(progress_->NeedsGas(), gas_ &&") !=
                 std::string::npos);
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  const size_t settle =
      start.find("if (!SettleFlow(ledgerClaimFlow_, generation, \"v2 claim\")) return;");
  UR_EXPECT_TRUE(settle != std::string::npos);
  UR_EXPECT_TRUE(start.find("LoadWalletLayer();", settle) != std::string::npos);
  // the watchdog outlasts the SDK's own bounds for every account it may claim,
  // in a pool that was unavailable too
  UR_EXPECT_TRUE(start.find("const int watchdogMs = snledger::ClaimWatchdogMs(ledgerAccounts_, "
                            "ledgerUnavailable_);") != std::string::npos);
  UR_EXPECT_TRUE(start.find("BeginFlow(ledgerClaimFlow_, watchdogMs, [this] {") !=
                 std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::LoadWalletLayer()")
                     .find("sn::FetchLedger(") != std::string::npos);
}

// VERSION2-LAUNCH.md §2.6: the SDK signs every v1 claim with at least 2,700,000
// gas (below that the vault defers the payment) and sends it only when the gas
// key holds that limit at 1.1 x the gas price, else needs_gas. So the v1 sheet
// shows the top-up and holds Claim below that limit's cost at 5 gwei, and the
// top-up asks for a v2 claim's 0.02 TAO, which covers it
UR_TEST(SnLedgerAV1ClaimNeedsItsGasFloorInTheGasKey) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  // the value of the page's `constexpr double <name> = <value>;`, or -1
  const auto constant = [&page](const std::string& name) {
    const std::string declaration = "\nconstexpr double " + name + " = ";
    const size_t at = page.find(declaration);
    if (at == std::string::npos) return -1.0;
    return std::strtod(page.c_str() + at + declaration.size(), nullptr);
  };
  const double floorTao = 2'700'000 * 1.1 * 5e9 / 1e18;  // 0.01485
  const double minimum = constant("kMinGasTao");
  const double suggested = constant("kSuggestedGasTao");
  UR_EXPECT_TRUE(minimum == 0.015);
  UR_EXPECT_TRUE(suggested == 0.02);
  UR_EXPECT_TRUE(suggested == urnw::snledger::kClaimGasTao);
  UR_EXPECT_TRUE(floorTao <= minimum && minimum <= suggested);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "bool ClaimAlphaSheet::NeedsGas() const")
                     .find("return gas_ && gas_->balanceKnown && gas_->tao < kMinGasTao;") !=
                 std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void ClaimAlphaSheet::RebuildAction()")
                     .find("claimButton_->set_sensitive(claimsAvailable_ && !busy_ && rao > 0 && "
                           "!NeedsGas() &&\n                              !(claim_held && claim_held()));") !=
                 std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void ClaimAlphaSheet::RebuildGas()")
                     .find("BuildGasPanel(*gasPanel_, *this, gas_, NeedsGas(), kSuggestedGasTao, "
                           "copiedReset_);") != std::string::npos);
}

// CL-4: one gas key sends one claim at a time: v1 and v2 refuse to start
// while either is in flight, and each redraws both tiles when it starts and ends
UR_TEST(SnLedgerOneClaimAtATime) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string v1 = LedgerFunctionBody(page, "void EarningsPage::StartClaim(");
  const std::string v2 = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  UR_EXPECT_TRUE(v1.find("if (epochs.empty() || Claiming()) return;") != std::string::npos);
  UR_EXPECT_TRUE(v2.find("if (Claiming()) return;") != std::string::npos);
  // a claim of either kind in flight holds both tiles' Claim, and is the
  // session's (ClaimInFlight), so no load clears it
  UR_EXPECT_TRUE(ReadLedgerSource("EarningsPage.hpp")
                     .find("  bool Claiming() const { return claim_.InFlight() || ledgerClaim_.InFlight(); }") !=
                 std::string::npos);
  UR_EXPECT_TRUE(page.find("claiming_") == std::string::npos);
  // the v1 tiles' Claim opens the v1 claim in flight, and waits for a v2 one
  // (SnLedgerCtf_AV1SheetOpenedDuringTheClaimDrawsItsAnswers; the rule is
  // snpayout::V1ClaimEnabled's, and SnLedgerCtf_TheV1ClaimWaitsForTheClaimsRead
  // pins both Claims to it)
  UR_EXPECT_TRUE(page.find("claimButton_->set_sensitive(\n"
                           "      snpayout::V1ClaimEnabled(claim_.InFlight(), ready, totalClaimableRao_, Claiming()));") !=
                 std::string::npos);
  UR_EXPECT_TRUE(page.find("payoutClaimButton_->set_sensitive(\n"
                           "      view.showClaim && snpayout::V1ClaimEnabled(claim_.InFlight(), claimsState_ == Fetch::Ready,\n"
                           "                                                 totalClaimableRao_, Claiming()));") !=
                 std::string::npos);
  for (const std::string* body : {&v1, &v2}) {
    size_t count = 0;
    for (size_t at = body->find("RebuildLedgerTile();"); at != std::string::npos;
         at = body->find("RebuildLedgerTile();", at + 1)) {
      ++count;
    }
    UR_EXPECT_EQ(3u, count);  // start, watchdog, done
  }
}

// CL-2: the new strings are localized under the store keys the Android app
// shows them with: their English is the catalog's msgid, or every locale would
// show the stale English. The pools' names are ClaimRelay.cpp's, which the
// tile and both sheets share; capture the flag's strings are the store's
// sn_ctf_* and pool row keys (localizations' capture-the-flag.test.mjs)
UR_TEST(SnLedgerEnglishMatchesTheCatalogMsgids) {
  const std::string page =
      ReadLedgerSource("EarningsPage.cpp") + ReadLedgerSource("ClaimRelay.cpp");
  const std::string catalog = ReadLedgerSource("../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  for (const char* key : {"sn_ledger_title", "sn_ledger_claimable", "sn_ledger_paid",
                          "sn_ledger_earned", "sn_ledger_pending", "sn_ledger_unavailable",
                          "sn_ledger_pool", "sn_ctf_pool", "sn_ledger_provider_pool",
                          "sn_ledger_pool_under_floor", "sn_ledger_pool_unavailable",
                          "sn_ledger_claim_both", "sn_ctf_won_provider_slot",
                          "sn_ctf_won_extender_slot"}) {
    const std::vector<std::string> fallbacks = LedgerFallbacks(page, key);
    if (fallbacks.empty()) UR_FAIL(std::string("the Earnings page does not show ") + key);
    for (const std::string& english : fallbacks) {
      const std::string entry =
          std::string("msgctxt \"") + key + "\"\nmsgid \"" + english + "\"\n";
      if (catalog.find(entry) == std::string::npos) {
        UR_FAIL(std::string(key) + ": \"" + english + "\" is not the catalog's msgid");
      }
    }
  }
  // the spec's words for a pending root
  UR_EXPECT_TRUE(catalog.find("msgid \"Pending: +{}, awaiting finalization\"") !=
                 std::string::npos);
  // the won slot's line takes the country's name as its one placeholder
  UR_EXPECT_TRUE(catalog.find("msgid \"You won one of the {} provider slots\"") !=
                 std::string::npos);
  UR_EXPECT_TRUE(catalog.find("msgid \"You won one of the {} extender slots\"") !=
                 std::string::npos);
}

// ---- capture the flag's wiring, read from the sources ---------------------------

// CAPTURETHEFLAG.md §10: the read carries each account's kind and the pools the
// server could not read, for the tile, the v2 sheet and the relay's rows alike
UR_TEST(SnLedgerCtf_TheReadsCarryKindsAndUnavailablePools) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string relay = ReadLedgerSource("ClaimRelay.cpp");
  const std::string read = LedgerFunctionBody(page, "void FetchLedger(SdkHost& host, LedgerDone done)");
  UR_EXPECT_TRUE(read.find("row.kind = account.kind;") != std::string::npos);
  UR_EXPECT_TRUE(read.find("UnavailablePoolsOf(result->unavailable_pools)") != std::string::npos);
  const std::string pools = LedgerFunctionBody(relay, "std::vector<snledger::UnavailablePool> UnavailablePoolsOf(");
  UR_EXPECT_TRUE(pools.find("row.noId = pool.no_id;") != std::string::npos);
  UR_EXPECT_TRUE(pools.find("row.kind = pool.kind;") != std::string::npos);
  const std::string accounts =
      LedgerFunctionBody(relay, "std::vector<snledger::RelayAccount> RelayAccountsOf(");
  UR_EXPECT_TRUE(accounts.find("row.kind = account.kind;") != std::string::npos);
  UR_EXPECT_TRUE(accounts.find("row.claimTxUrl = account.claim_tx_url.value_or(std::string());") !=
                 std::string::npos);
  // the pure rules' kinds, pool ids and roles are the SDK's
  UR_EXPECT_TRUE(relay.find("static_assert(std::string_view(snledger::kKindCtf) == "
                            "urnet::SnLedgerPoolKindCtf,") != std::string::npos);
  UR_EXPECT_TRUE(relay.find("static_assert(snledger::kCtfPoolIdBase == urnet::SnLedgerCtfPoolIdBase,") !=
                 std::string::npos);
  UR_EXPECT_TRUE(relay.find("static_assert(std::string_view(snledger::kRoleProvider) == "
                            "urnet::SnCtfRoleProvider,") != std::string::npos);
  // the page keeps the unavailable pools with the accounts, and a new wallet's
  // read starts without them
  const std::string apply = LedgerFunctionBody(page, "void EarningsPage::ApplyLedger(");
  UR_EXPECT_TRUE(apply.find("ledgerUnavailable_ = std::move(unavailable);") != std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::ApplySnWallet(")
                     .find("ledgerUnavailable_.clear();") != std::string::npos);
}

// the tile draws each row as SectionFor decided it: the pool's name, Unavailable
// right now in place of the figures, Below the claim minimum under them, and
// Claim both; the floor is the relay's
UR_TEST(SnLedgerCtf_TheTileDrawsEachRow) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string tile = LedgerFunctionBody(page, "void EarningsPage::RebuildLedgerTile()");
  UR_EXPECT_TRUE(tile.find("snledger::SectionFor(connected, ledgerLoad_, ledgerAccounts_, "
                           "ledgerUnavailable_,\n                           TransferFloorRao(),") !=
                 std::string::npos);
  UR_EXPECT_TRUE(tile.find("if (account.pool != snledger::PoolName::None) {") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("LedgerPoolName(account.pool, account.noId)") != std::string::npos);
  const size_t unavailable = tile.find("if (account.unavailable) {");
  UR_EXPECT_TRUE(unavailable != std::string::npos);
  // no figure before the row moves on
  const size_t next = tile.find("continue;", unavailable);
  UR_EXPECT_TRUE(next != std::string::npos &&
                 tile.find("T_(\"sn_ledger_pool_unavailable\"", unavailable) < next &&
                 tile.find("account.claimable", unavailable) > next);
  UR_EXPECT_TRUE(tile.find("if (account.underFloor) {") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("ledgerClaimButton_->set_label(view.claimBoth ? "
                           "T_(\"sn_ledger_claim_both\", \"Claim both\")") != std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "int64_t EarningsPage::TransferFloorRao() const")
                     .find("claimStatus_ && !claimStatus_->error ? claimStatus_->transfer_floor_rao "
                           ": 0;") != std::string::npos);
  // a network with no wallet here: the relay's sum, while one of the
  // coldkey's pools was read, saying when it is one account below the claim
  // minimum; what it adds up pool by pool, and the pools it leaves out, with
  // Pending left to the relay's part below (Android's tile rows)
  const size_t known = tile.find("if (snledger::RelayTotalKnown(relayAccounts, coldkey, unavailable)) {");
  UR_EXPECT_TRUE(known != std::string::npos);
  UR_EXPECT_TRUE(known < tile.find("FormatAlphaRao(claimStatus_->claimable_rao)"));
  UR_EXPECT_TRUE(tile.find("if (snledger::RelayTileSumUnderFloor(relayAccounts, coldkey, unavailable, "
                           "floorRao)) {\n"
                           "        block->append(*MakeWrappedNote(\n"
                           "            T_(\"sn_ledger_pool_under_floor\", \"Below the claim minimum\")") !=
                 std::string::npos);
  UR_EXPECT_TRUE(tile.find("AppendRelayRows(*ledgerRows_,\n"
                           "                    snledger::RelayTileRowsFor(relayAccounts, coldkey, "
                           "unavailable, floorRao),\n"
                           "                    [this](const std::string& url) { OpenLink(url); }, "
                           "/*pendingPill=*/false);") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("RelayAccountsOf(*claimStatus_)") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("UnavailablePoolsOf(claimStatus_->unavailable_pools)") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("const int64_t floorRao = claimStatus_->transfer_floor_rao;") !=
                 std::string::npos);
}

// CAPTURETHEFLAG.md §10: one action claims both pools: the sheet's one Claim is
// one SnLedgerClaim, which sends them in one batch, and it reads Claim both
// while it claims both; its rows name each pool and say what waits
UR_TEST(SnLedgerCtf_OneActionClaimsBothPools) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string open = LedgerFunctionBody(page, "void EarningsPage::OnLedgerClaim()");
  // between claims the sheet's rows are the accounts as last read, at the
  // relay's floor (during one, the claim's: SnLedgerCtf_TheClaimIsTheSessionsNotALoads)
  UR_EXPECT_TRUE(open.find("rows = std::make_shared<snledger::ClaimProgress>(ledgerAccounts_, "
                           "ledgerUnavailable_,\n"
                           "                                                     TransferFloorRao());") !=
                 std::string::npos);
  UR_EXPECT_TRUE(open.find("std::make_shared<LedgerClaimSheet>(*root, wallet_->coldkeySs58, "
                           "std::move(rows),") != std::string::npos);
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  size_t claims = 0;
  for (size_t at = start.find("sn::LedgerClaim("); at != std::string::npos;
       at = start.find("sn::LedgerClaim(", at + 1)) {
    ++claims;
  }
  UR_EXPECT_EQ(1u, claims);
  const std::string rebuild = LedgerFunctionBody(page, "void LedgerClaimSheet::Rebuild()");
  UR_EXPECT_TRUE(rebuild.find("LedgerPoolName(progress_->NameOf(row), row.noId)") != std::string::npos);
  UR_EXPECT_TRUE(rebuild.find("claimButton_->set_label(progress_->ClaimsBoth()\n"
                              "                              ? Glib::ustring(T_(\"sn_ledger_claim_both\", "
                              "\"Claim both\"))") != std::string::npos);
  UR_EXPECT_TRUE(rebuild.find("case snledger::Phase::UnderFloor:\n"
                              "        chipText = T_(\"sn_ledger_pool_under_floor\", \"Below the claim "
                              "minimum\");") != std::string::npos);
  UR_EXPECT_TRUE(rebuild.find("case snledger::Phase::Unavailable:\n"
                              "        chipText = T_(\"sn_ledger_pool_unavailable\", \"Unavailable right "
                              "now\");") != std::string::npos);
  // an unreadable pool, or an account whose amount the SDK has not paid yet,
  // shows no amount, never a zero
  UR_EXPECT_TRUE(rebuild.find("if (row.phase != snledger::Phase::Unavailable && 0 < row.amountRao) {\n"
                              "      auto* amount = MakeSizedLabel(FormatAlphaRao(row.amountRao)") !=
                 std::string::npos);
}

// the relay sheet lists each pool's account (snledger::RelayRowsFor) under the
// balance, with the store's words for each status, and the claimed banner
// leaves its transaction to the rows that link it
UR_TEST(SnLedgerCtf_TheRelaySheetListsEachPool) {
  const std::string relay = ReadLedgerSource("ClaimRelay.cpp");
  const std::string sheet = LedgerFunctionBody(relay, "void ClaimRelaySheet::Apply(");
  UR_EXPECT_TRUE(sheet.find("const std::vector<snledger::RelayAccount> accounts = "
                            "RelayAccountsOf(*status);") != std::string::npos);
  UR_EXPECT_TRUE(sheet.find("UnavailablePoolsOf(status->unavailable_pools);") != std::string::npos);
  UR_EXPECT_TRUE(sheet.find("snledger::RelayRowsFor(accounts, unavailable, status->transfer_floor_rao);") !=
                 std::string::npos);
  UR_EXPECT_TRUE(sheet.find("AppendRelayRows(*content_, rows, openLink);") != std::string::npos);
  // the total only while one of the coldkey's pools was read: never a zero
  const size_t known = sheet.find("if (snledger::RelayTotalKnown(accounts, coldkey, unavailable)) {");
  UR_EXPECT_TRUE(known != std::string::npos);
  UR_EXPECT_TRUE(known < sheet.find("T_(\"sn_ledger_claimable\", \"Claimable\")"));
  UR_EXPECT_TRUE(known < sheet.find("urnet::formatAlpha(status->claimable_rao)"));
  UR_EXPECT_TRUE(sheet.find("/*transaction=*/!RowsLinkATransaction(rows)") != std::string::npos);
  const std::string rows = LedgerFunctionBody(relay, "void AppendRelayRows(");
  // the tile's rows leave Pending to the relay's part under them
  UR_EXPECT_TRUE(rows.find("if (pendingPill) status = T_(\"pending_data_key\", \"Pending\");") !=
                 std::string::npos);
  UR_EXPECT_TRUE(rows.find("if (!status.empty()) {") != std::string::npos);
  for (const char* lookup :
       {"T_(\"pending_data_key\", \"Pending\")",
        "T_(\"sn_ledger_pool_under_floor\", \"Below the claim minimum\")",
        "T_(\"claim_confirmed\", \"Claimed\")",
        "T_(\"sn_ledger_pool_unavailable\", \"Unavailable right now\")",
        "LedgerPoolName(row.pool, row.noId)", "RowTransactionLink(row.txUrl)"}) {
    UR_EXPECT_TRUE_MSG(lookup, rows.find(lookup) != std::string::npos);
  }
  // the names, one place for the tile and both sheets
  const std::string names = LedgerFunctionBody(relay, "Glib::ustring LedgerPoolName(");
  UR_EXPECT_TRUE(names.find("T_(\"sn_ctf_pool\", \"Capture the flag\")") != std::string::npos);
  UR_EXPECT_TRUE(names.find("T_(\"sn_ledger_provider_pool\", \"Provider pool\")") != std::string::npos);
  UR_EXPECT_TRUE(names.find("Format(T_(\"sn_ledger_pool\", \"Pool {}\"), noId)") != std::string::npos);
}

// the won slots' lines: GET /sn/ctf/client through the device, every win the
// SDK lists as not claimed yet, read on every load and after a v2 claim's
// Done, forgotten on sign-out, each worded by the role that won with the
// country's name
UR_TEST(SnLedgerCtf_TheWonSlotLineIsWired) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string fetch = LedgerFunctionBody(page, "void FetchCtfWins(SdkHost& host, CtfWinsDone done)");
  UR_EXPECT_TRUE(fetch.find("host.device().snCtfClient(") != std::string::npos);
  // a read that failed, an answer that could not be read and a read that could
  // not be asked (no steady device) are no answer, never "no wins": the lines
  // the last answer drew stay (SnLedgerCtf_AWinsReadThatFailsKeepsTheLines)
  UR_EXPECT_TRUE(fetch.find("if (!host.DeviceSteady()) {\n    done(std::nullopt);") != std::string::npos);
  UR_EXPECT_TRUE(fetch.find("if (err || !result || result->error || !result->unclaimed_wins) {\n"
                            "      done(std::nullopt);") != std::string::npos);
  // the newest read's good answer shows (SnLedgerCtf_OverlappingWinsReadsShow
  // TheNewestGoodAnswer)
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::ApplyCtfWins(")
                     .find("if (ctfWins_.Apply(read, std::move(answer))) RebuildLedgerTile();") !=
                 std::string::npos);
  UR_EXPECT_TRUE(ReadLedgerSource("EarningsPage.hpp").find("  snledger::WinsLines ctfWins_;") !=
                 std::string::npos);
  UR_EXPECT_TRUE(fetch.find("for (const urnet::SnCtfWin& unclaimed : *result->unclaimed_wins) {") !=
                 std::string::npos);
  for (const char* field : {"win.role = unclaimed.role;", "win.country = unclaimed.country;",
                            "win.epoch = unclaimed.epoch;", "win.claimState = unclaimed.claim_state;"}) {
    UR_EXPECT_TRUE_MSG(field, fetch.find(field) != std::string::npos);
  }
  // with the client the answer is for, "" for the network's: a network's
  // answer beats a client's (SnLedgerCtf_ANetworksAnswerBeatsAClients)
  UR_EXPECT_TRUE(fetch.find("snledger::WinsAnswer answer;\n"
                            "    // \"\" for the network's answer, the client's id for the client token's\n"
                            "    answer.clientId = result->client_id;") != std::string::npos);
  UR_EXPECT_TRUE(fetch.find("answer.wins.push_back(std::move(win));") != std::string::npos);
  UR_EXPECT_TRUE(fetch.find("done(std::move(answer));") != std::string::npos);
  UR_EXPECT_TRUE(fetch.find("latest_win") == std::string::npos);
  // the claim states are the SDK's
  const std::string relay = ReadLedgerSource("ClaimRelay.cpp");
  UR_EXPECT_TRUE(relay.find("static_assert(std::string_view(snledger::kClaimStateUnclaimed) == "
                            "urnet::SnCtfWinClaimStateUnclaimed,") != std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::LoadEarnings()").find("LoadCtfWins();") !=
                 std::string::npos);
  // a sign-out, and no session: no lines, and no answer of a read from before
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::SettleAllEmpty()").find("ctfWins_.Reset();") !=
                 std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::ResetForSignOut()").find("ctfWins_.Reset();") !=
                 std::string::npos);
  // a win the claim paid reads claimed once the finalized head covers the
  // claim: the lines are read again after Done (SnLedgerCtf_TheWinsAfterA
  // ClaimAreWired)
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  const size_t settle = start.find("if (!SettleFlow(ledgerClaimFlow_, generation, \"v2 claim\")) return;");
  UR_EXPECT_TRUE(settle != std::string::npos &&
                 start.find("winsAfterClaim_.Done(paid);", settle) != std::string::npos);
  const std::string tile = LedgerFunctionBody(page, "void EarningsPage::RebuildLedgerTile()");
  UR_EXPECT_TRUE(tile.find("for (const snledger::WonLine& won : snledger::WonLinesFor(ctfWins_.Wins())) {") !=
                 std::string::npos);
  UR_EXPECT_TRUE(tile.find("const std::string country = CountryNameOrCode(won.country);") !=
                 std::string::npos);
  UR_EXPECT_TRUE(tile.find("won.slot == snledger::WonSlot::Provider") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("Format(T_(\"sn_ctf_won_provider_slot\", \"You won one of the {} provider "
                           "slots\"), country)") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("Format(T_(\"sn_ctf_won_extender_slot\", \"You won one of the {} extender "
                           "slots\"), country)") != std::string::npos);
}

// The wins after a v2 claim (snledger::WinsAfterClaim, whose schedule the tests
// above run): read through the page's LoadCtfWins on the main loop's timers,
// which do nothing once the page is gone; the claim's Done says whether it paid
// an account, which a Confirmed or an already_claimed says (ClaimProgress::Paid,
// SnLedgerCtf_APressPaysWhenAnAccountIsClaimed); and a sign-out, or no
// session, stops them
UR_TEST(SnLedgerCtf_TheWinsAfterAClaimAreWired) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  UR_EXPECT_TRUE(page.find("      consent_(ConsentPorts()),\n"
                           "      winsAfterClaim_(WinsAfterClaimPorts()) {") != std::string::npos);
  const std::string ports =
      LedgerFunctionBody(page, "snledger::WinsAfterClaim::Ports EarningsPage::WinsAfterClaimPorts()");
  UR_EXPECT_TRUE(ports.find("ports.read = [this] { LoadCtfWins(); };") != std::string::npos);
  UR_EXPECT_TRUE(ports.find("ports.schedule = [alive = alive_](int delayMs, std::function<void()> fire) {\n"
                            "    Glib::signal_timeout().connect_once(\n"
                            "        [alive, fire = std::move(fire)] {\n"
                            "          if (*alive) fire();\n"
                            "        },\n"
                            "        delayMs);") != std::string::npos);
  // alive_ is set before the initializer list reaches winsAfterClaim_
  const std::string header = ReadLedgerSource("EarningsPage.hpp");
  const size_t alive = header.find("std::shared_ptr<bool> alive_");
  const size_t member = header.find("snledger::WinsAfterClaim winsAfterClaim_;");
  UR_EXPECT_TRUE(alive != std::string::npos && member != std::string::npos && alive < member);
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  // each press starts unpaid (ClaimProgress::Start), and its Confirmed and
  // already_claimed reach the claim's rows, which say whether it paid
  const size_t claiming = start.find("const uint64_t claim = ledgerClaim_.Start(");
  UR_EXPECT_TRUE(claiming != std::string::npos);
  UR_EXPECT_TRUE(claiming < start.find("sn::LedgerClaim(host_, std::move(callback));"));
  const size_t confirmed = start.find("callback.confirmed =");
  const size_t failed = start.find("callback.failed =");
  const size_t done = start.find("callback.done =");
  UR_EXPECT_TRUE(confirmed != std::string::npos && failed != std::string::npos && done != std::string::npos);
  const std::string onConfirmed = start.substr(confirmed, failed - confirmed);
  UR_EXPECT_TRUE(onConfirmed.find("ledgerClaim_.Rows()->Confirmed(noId, index, txHash, amountRao);") !=
                 std::string::npos);
  const std::string onFailed = start.substr(failed, done - failed);
  UR_EXPECT_TRUE(onFailed.find("ledgerClaim_.Rows()->Failed(noId, index, message);") != std::string::npos);
  // the read at Done is the schedule's own: no second read beside it
  const std::string onDone = start.substr(done);
  UR_EXPECT_TRUE(onDone.find("LoadCtfWins();") == std::string::npos);
  const size_t paid = onDone.find("const bool paid = ledgerClaim_.End();");
  UR_EXPECT_TRUE(paid != std::string::npos && paid < onDone.find("winsAfterClaim_.Done(paid);"));
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::ResetForSignOut()").find("winsAfterClaim_.Stop();") !=
                 std::string::npos);
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::SettleAllEmpty()").find("winsAfterClaim_.Stop();") !=
                 std::string::npos);
  // a read needs a session; its number, taken as it starts, orders its answer
  // among the others', in place of the load's epoch, which would drop an older
  // read's good answer behind a newer read that fails; nothing lands once the
  // page is gone
  const std::string load = LedgerFunctionBody(page, "void EarningsPage::LoadCtfWins()");
  UR_EXPECT_TRUE(load.find("if (!CanCallApi()) return;") != std::string::npos);
  const size_t number = load.find("const uint64_t read = ctfWins_.Start();");
  UR_EXPECT_TRUE(number != std::string::npos && number < load.find("sn::FetchCtfWins("));
  UR_EXPECT_TRUE(load.find("if (!*alive) return;\n      ApplyCtfWins(read, std::move(answer));") !=
                 std::string::npos);
  UR_EXPECT_TRUE(load.find("*epoch") == std::string::npos && load.find("epoch_") == std::string::npos);
}

// The v2 claim is the session's, not a load's (owner, 2026-11-12;
// SnLedgerCtf_AClaimOutlivesLeavingEarningsAndComingBack runs ClaimInFlight):
// every answer of it counts by the page's ClaimInFlight while the page lives,
// never by the load's epoch, which each visit to Earnings bumps, nor by any
// other gate: each answer captures only the page, its life and the claim's
// ticket, and its one guard comes first. No load touches the claim; a sheet
// opened during it draws its rows; its watchdog ends it; and a sign-out drops
// it and the v1 claim, their watchdogs and Claim's wait, and closes their
// sheets
UR_TEST(SnLedgerCtf_TheClaimIsTheSessionsNotALoads) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  UR_EXPECT_TRUE(start.find("*epoch") == std::string::npos && start.find("epoch_") == std::string::npos);
  UR_EXPECT_TRUE(start.find("const uint64_t claim = ledgerClaim_.Start(\n      sheet ? sheet->Progress()") !=
                 std::string::npos);
  UR_EXPECT_TRUE(start.find("auto alive = alive_;") != std::string::npos);
  const std::string guard = "      if (!*alive || !ledgerClaim_.Current(claim)) return;\n";
  // each answer: what it captures, its guard first, and no other way out
  struct Answer {
    const char* at;
    const char* opening;
    size_t returns;
  };
  const Answer answers[] = {
      {"callback.sent =",
       "callback.sent = [this, alive, claim](int64_t noId, int64_t index, std::string txHash) {\n"
       "    PostToMain([this, alive, claim, noId, index, txHash = std::move(txHash)] {\n",
       1},
      {"callback.confirmed =",
       "callback.confirmed = [this, alive, claim](int64_t noId, int64_t index, std::string txHash,\n"
       "                                            int64_t amountRao) {\n"
       "    PostToMain([this, alive, claim, noId, index, txHash = std::move(txHash), amountRao] {\n",
       1},
      {"callback.failed =",
       "callback.failed = [this, alive, claim](int64_t noId, int64_t index, std::string message) {\n"
       "    PostToMain([this, alive, claim, noId, index, message = std::move(message)] {\n",
       1},
      // and the watchdog's settle, which only a give-up already final fails
      {"callback.done =",
       "callback.done = [this, alive, claim, generation] {\n"
       "    PostToMain([this, alive, claim, generation] {\n",
       2},
  };
  for (const Answer& answer : answers) {
    const size_t from = start.find(answer.at);
    const size_t next = start.find("callback.", from + 1);
    const std::string body = from == std::string::npos ? std::string() : start.substr(from, next - from);
    UR_EXPECT_TRUE_MSG(answer.at, body.find(std::string(answer.opening) + guard) == 0);
    size_t returns = 0;
    for (size_t at = body.find("return;"); at != std::string::npos; at = body.find("return;", at + 1)) {
      ++returns;
    }
    UR_EXPECT_EQ(answer.returns, returns);
  }
  UR_EXPECT_TRUE(start.find("ledgerClaim_.Rows()->Sent(noId, index, txHash);") != std::string::npos);
  // the watchdog's give-up is final: no later answer of the claim counts
  const size_t watchdog = start.find("BeginFlow(ledgerClaimFlow_, watchdogMs, [this] {");
  const size_t ended = start.find("ledgerClaim_.End();", watchdog);
  UR_EXPECT_TRUE(watchdog != std::string::npos && ended != std::string::npos &&
                 ended < start.find("auto alive = alive_;"));
  // no load touches the claim, and only the claim's start, its ends and a
  // sign-out move it
  for (const char* loader : {"void EarningsPage::Load()", "void EarningsPage::LoadEarnings()",
                             "void EarningsPage::LoadWalletLayer()", "void EarningsPage::SettleAllEmpty()"}) {
    const std::string body = LedgerFunctionBody(page, loader);
    UR_EXPECT_TRUE_MSG(loader, !body.empty() && body.find("ledgerClaim") == std::string::npos &&
                                   body.find("claim_.") == std::string::npos);
  }
  const auto count = [&page](const std::string& what) {
    size_t n = 0;
    for (size_t at = page.find(what); at != std::string::npos; at = page.find(what, at + 1)) ++n;
    return n;
  };
  UR_EXPECT_EQ(1u, count("ledgerClaim_.Start("));
  UR_EXPECT_EQ(2u, count("ledgerClaim_.End()"));  // its Done, its watchdog
  UR_EXPECT_EQ(1u, count("ledgerClaim_.Drop()"));
  // a sheet opened during the claim draws its rows
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::OnLedgerClaim()")
                     .find("std::shared_ptr<snledger::ClaimProgress> rows = ledgerClaim_.Rows();") !=
                 std::string::npos);
  // the sheet draws the page's rows and moves none itself
  UR_EXPECT_TRUE(page.find("  std::shared_ptr<snledger::ClaimProgress> progress_;") != std::string::npos);
  for (const char* gone : {"LedgerClaimSheet::OnSending(", "LedgerClaimSheet::OnSent(",
                           "LedgerClaimSheet::OnConfirmed(", "LedgerClaimSheet::OnFailed(",
                           "LedgerClaimSheet::OnDone("}) {
    UR_EXPECT_TRUE_MSG(gone, page.find(gone) == std::string::npos);
  }
  // a sign-out drops both claims and their watchdogs, which frees Claim, and
  // closes both claims' sheets
  const std::string signOut = LedgerFunctionBody(page, "void EarningsPage::ResetForSignOut()");
  UR_EXPECT_TRUE(signOut.find("  ledgerClaim_.Drop();\n"
                              "  claim_.Drop();\n"
                              "  claimAnswers_.Clear();\n"
                              "  claimRows_.clear();\n"
                              "  ++claimFlow_.generation;\n"
                              "  claimFlow_.timer.disconnect();\n"
                              "  ++ledgerClaimFlow_.generation;\n"
                              "  ledgerClaimFlow_.timer.disconnect();\n"
                              "  if (auto open = claimSheet_.lock()) open->set_visible(false);\n"
                              "  if (auto open = ledgerClaimSheet_.lock()) open->set_visible(false);") !=
                 std::string::npos);
  UR_EXPECT_TRUE(signOut.find("Load();") != std::string::npos);
  // the page's end: nothing of the claim lands once alive_ is false
  const std::string end = LedgerFunctionBody(page, "EarningsPage::~EarningsPage()");
  UR_EXPECT_TRUE(end.find("*alive_ = false;") != std::string::npos);
  UR_EXPECT_TRUE(end.find("ledgerClaimFlow_.timer.disconnect();") != std::string::npos);
  const std::string header = ReadLedgerSource("EarningsPage.hpp");
  UR_EXPECT_TRUE(header.find("snledger::ClaimInFlight ledgerClaim_;") != std::string::npos);
  UR_EXPECT_TRUE(header.find("snledger::ClaimInFlight claim_;") != std::string::npos);
  UR_EXPECT_TRUE(header.find("ledgerClaimPaid_") == std::string::npos);
  UR_EXPECT_TRUE(header.find("claiming_") == std::string::npos);
}

// The v1 claim is the session's too (owner, 2026-11-12;
// SnLedgerCtf_AV1ClaimOutlivesLeavingEarningsAndComingBack runs ClaimInFlight
// without rows): every answer counts by the page's claim_ while the page
// lives, never by the load's epoch, so leaving Earnings and coming back no
// longer keeps Claim held until the 3-minute watchdog. Each answer captures
// only the page, its life and the claim's ticket, and its one guard comes
// first; the watchdog's give-up and its Done end it
UR_TEST(SnLedgerCtf_TheV1ClaimIsTheSessionsNotALoads) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartClaim(");
  UR_EXPECT_TRUE(start.find("*epoch") == std::string::npos && start.find("epoch_") == std::string::npos);
  const size_t claiming = start.find("const uint64_t claim = claim_.Start();");
  UR_EXPECT_TRUE(claiming != std::string::npos && claiming > start.find("if (!sn::ClaimsAvailable(host_)) {") &&
                 claiming < start.find("if (sheet) sheet->OnSending(epochs);"));
  UR_EXPECT_TRUE(start.find("auto alive = alive_;") != std::string::npos);
  const std::string guard = "      if (!*alive || !claim_.Current(claim)) return;\n";
  struct Answer {
    const char* at;
    const char* opening;
    size_t returns;
  };
  const Answer answers[] = {
      {"events.sent =",
       "events.sent = [this, alive, claim](int64_t claimEpoch, std::string txHash) {\n"
       "    PostToMain([this, alive, claim, claimEpoch, txHash = std::move(txHash)] {\n",
       1},
      {"events.confirmed =",
       "events.confirmed = [this, alive, claim](int64_t claimEpoch, std::string txHash,\n"
       "                                          int64_t amountRao) {\n"
       "    PostToMain([this, alive, claim, claimEpoch, txHash = std::move(txHash), amountRao] {\n",
       1},
      {"events.failed =",
       "events.failed = [this, alive, claim](int64_t claimEpoch, std::string message) {\n"
       "    PostToMain([this, alive, claim, claimEpoch, message = std::move(message)] {\n",
       1},
      // and the watchdog's settle, which only a give-up already final fails
      {"events.done =",
       "events.done = [this, alive, claim, generation] {\n"
       "    PostToMain([this, alive, claim, generation] {\n",
       2},
  };
  for (const Answer& answer : answers) {
    const size_t from = start.find(answer.at);
    const size_t next = start.find("events.", from + 1);
    const std::string body = from == std::string::npos ? std::string() : start.substr(from, next - from);
    UR_EXPECT_TRUE_MSG(answer.at, body.find(std::string(answer.opening) + guard) == 0);
    size_t returns = 0;
    for (size_t at = body.find("return;"); at != std::string::npos; at = body.find("return;", at + 1)) {
      ++returns;
    }
    UR_EXPECT_EQ(answer.returns, returns);
  }
  // the watchdog waits past the SDK's bounds (SnLedgerCtf_TheV1WatchdogOutlasts
  // EveryEpochTheSdkClaims), its give-up is final, and the Done after the
  // settle ends the claim
  UR_EXPECT_TRUE(start.find("const int watchdogMs = snledger::EpochClaimWatchdogMs(epochs.size());\n"
                            "  const uint32_t generation = BeginFlow(claimFlow_, watchdogMs, [this] {") !=
                 std::string::npos);
  UR_EXPECT_TRUE(page.find("kChainTimeoutMs") == std::string::npos);
  const size_t watchdog = start.find("BeginFlow(claimFlow_, watchdogMs, [this] {");
  const size_t ended = start.find("claim_.End();", watchdog);
  UR_EXPECT_TRUE(watchdog != std::string::npos && ended != std::string::npos &&
                 ended < start.find("auto alive = alive_;"));
  const size_t done = start.find("events.done =");
  const size_t settle = start.find("if (!SettleFlow(claimFlow_, generation, \"claim\")) return;", done);
  UR_EXPECT_TRUE(settle != std::string::npos && start.find("claim_.End();", settle) != std::string::npos &&
                 start.find("LoadWalletLayer();", settle) != std::string::npos);
  const auto count = [&page](const std::string& what) {
    size_t n = 0;
    for (size_t at = page.find(what); at != std::string::npos; at = page.find(what, at + 1)) ++n;
    return n;
  };
  UR_EXPECT_EQ(1u, count("claim_.Start("));
  UR_EXPECT_EQ(2u, count("claim_.End()"));  // its Done, its watchdog
  UR_EXPECT_EQ(1u, count("claim_.Drop()"));
}

// The page keeps the v1 claim's answers for a sheet opened during it
// (SnLedgerCtf_AV1SheetOpenedDuringTheClaimDrawsItsAnswers): each answer, once
// it counts, is kept before it reaches a sheet; the claim's Done, its watchdog
// and a sign-out drop them; a sheet opened during the claim replays them; and
// the v1 tiles' Claim opens it while it is in flight
UR_TEST(SnLedgerCtf_TheV1ClaimsAnswersAreWired) {
  // the page: each answer, once it counts, is kept before it reaches a sheet
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartClaim(");
  UR_EXPECT_TRUE(start.find("const uint64_t claim = claim_.Start();\n  claimAnswers_.Start(epochs);") !=
                 std::string::npos);
  const std::string guard = "if (!*alive || !claim_.Current(claim)) return;\n";
  for (const char* kept : {"claimAnswers_.Sent(claimEpoch, txHash);",
                           "claimAnswers_.Confirmed(claimEpoch, txHash, amountRao);",
                           "claimAnswers_.Failed(claimEpoch, message);"}) {
    const size_t at = start.find(kept);
    UR_EXPECT_TRUE_MSG(kept, at != std::string::npos && start.rfind(guard, at) != std::string::npos);
  }
  // its Done, its watchdog and a sign-out drop them
  const auto count = [&page](const std::string& what) {
    size_t n = 0;
    for (size_t at = page.find(what); at != std::string::npos; at = page.find(what, at + 1)) ++n;
    return n;
  };
  UR_EXPECT_EQ(3u, count("claimAnswers_.Clear();"));
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void EarningsPage::ResetForSignOut()").find("claimAnswers_.Clear();") !=
                 std::string::npos);
  // the claim's own rows, as the sheet that pressed it listed them, which a
  // sheet opened during it is built from whatever the claims read since (a
  // failed read leaves none), and which the claim's end drops with its answers
  UR_EXPECT_TRUE(start.find("claimAnswers_.Start(epochs);\n"
                            "  // the claim's own rows, as the sheet that pressed it lists them: a sheet\n"
                            "  // opened during the claim is built from them\n"
                            "  claimRows_ = sheet ? sheet->ClaimsOf(epochs) : ClaimRowsOf(claims_, epochs);") !=
                 std::string::npos);
  UR_EXPECT_EQ(3u, count("claimAnswers_.Clear();\n  claimRows_.clear();") +
                       count("claimAnswers_.Clear();\n    claimRows_.clear();") +
                       count("claimAnswers_.Clear();\n      claimRows_.clear();"));
  UR_EXPECT_EQ(3u, count("claimRows_.clear();"));
  // a sheet opened during the claim replays them: its epochs sending, then each answer
  const std::string open = LedgerFunctionBody(page, "void EarningsPage::OpenClaimSheet(bool allowActions)");
  const size_t rows = open.find("const std::vector<SnClaimRow>& rows = claim_.InFlight() ? claimRows_ : claims_;\n"
                                "  auto sheet = std::make_shared<ClaimAlphaSheet>(*root, wallet_->coldkeySs58, rows, gas_,");
  UR_EXPECT_TRUE(rows != std::string::npos);
  const size_t replay = open.find("if (claim_.InFlight()) {\n    sheet->OnSending(claimAnswers_.Epochs());\n"
                                  "    for (const snledger::EpochClaimAnswers::Answer& answer : claimAnswers_.Answers()) {");
  UR_EXPECT_TRUE(replay != std::string::npos && replay < open.find("PresentSheet(sheet);"));
  for (const char* drawn : {"sheet->OnSent(answer.epoch, answer.txHash);",
                            "sheet->OnConfirmed(answer.epoch, answer.txHash, answer.amountRao);",
                            "sheet->OnFailed(answer.epoch, answer.message);"}) {
    UR_EXPECT_TRUE_MSG(drawn, open.find(drawn) != std::string::npos);
  }
  // and the v1 tiles' Claim opens it while it is in flight (SnLedgerOneClaimAtATime)
  UR_EXPECT_TRUE(page.find("claimButton_->set_sensitive(\n"
                           "      snpayout::V1ClaimEnabled(claim_.InFlight(), ready, totalClaimableRao_, Claiming()));") !=
                 std::string::npos);
}

// The v2 sheet's Claim waits for a v1 claim in flight: the relay's "Claim it
// yourself" opens the v2 sheet whatever the tiles say, and the press would do
// nothing while the gas key sends the v1 claim. The page redraws an open v2
// sheet when the v1 claim ends, by its Done or its watchdog
UR_TEST(SnLedgerCtf_TheV2SheetsClaimWaitsForAV1Claim) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string rebuild = LedgerFunctionBody(page, "void LedgerClaimSheet::Rebuild()");
  UR_EXPECT_TRUE(rebuild.find("claimButton_->set_sensitive(claimsAvailable_ && !progress_->Busy() && rao > 0 && "
                              "!NeedsGas() &&\n                              !(claim_held && claim_held()));") !=
                 std::string::npos);
  const std::string open = LedgerFunctionBody(page, "void EarningsPage::OnLedgerClaim()");
  const size_t held = open.find("sheet->claim_held = [this] { return claim_.InFlight(); };\n  sheet->Redraw();");
  UR_EXPECT_TRUE(held != std::string::npos && held < open.find("PresentSheet(sheet);"));
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartClaim(");
  size_t redraws = 0;
  for (size_t at = start.find("if (auto open = ledgerClaimSheet_.lock()) open->Redraw();"); at != std::string::npos;
       at = start.find("if (auto open = ledgerClaimSheet_.lock()) open->Redraw();", at + 1)) {
    ++redraws;
  }
  UR_EXPECT_EQ(2u, redraws);  // its watchdog, its Done
  // and the other way: a v2 claim in flight holds the v1 sheet's Claim, which
  // is redrawn at the v2 claim's end (the payout line's Claim, which opens the
  // v1 sheet, follows both claims: SnLedgerCtf_ThePayoutLinesClaimFollowsBothClaims)
  UR_EXPECT_TRUE(LedgerFunctionBody(page, "void ClaimAlphaSheet::RebuildAction()")
                     .find("!NeedsGas() &&\n                              !(claim_held && claim_held()));") !=
                 std::string::npos);
  const std::string openV1 = LedgerFunctionBody(page, "void EarningsPage::OpenClaimSheet(bool allowActions)");
  const size_t heldV1 = openV1.find("sheet->claim_held = [this] { return ledgerClaim_.InFlight(); };");
  UR_EXPECT_TRUE(heldV1 != std::string::npos && heldV1 < openV1.find("sheet->SetGas(gas_);"));
  const std::string ledger = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  size_t v1Redraws = 0;
  for (size_t at = ledger.find("if (auto open = claimSheet_.lock()) open->Redraw();"); at != std::string::npos;
       at = ledger.find("if (auto open = claimSheet_.lock()) open->Redraw();", at + 1)) {
    ++v1Redraws;
  }
  UR_EXPECT_EQ(2u, v1Redraws);  // the v2 claim's watchdog, its Done
}

// The payout line's Claim, which opens the v1 sheet, is drawn again at each
// claim's start and end, v1's and v2's, as the tiles are: it waits for a claim
// in flight (but the v1 claim's own, which it opens), where it stayed as the
// last read left it
UR_TEST(SnLedgerCtf_ThePayoutLinesClaimFollowsBothClaims) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  for (const char* signature : {"void EarningsPage::StartClaim(", "void EarningsPage::StartLedgerClaim()"}) {
    const std::string body = LedgerFunctionBody(page, signature);
    size_t payout = 0;
    for (size_t at = body.find("RebuildPayoutLine();\n"); at != std::string::npos;
         at = body.find("RebuildPayoutLine();\n", at + 1)) {
      // each beside the tiles' redraw
      UR_EXPECT_TRUE_MSG(signature, body.compare(body.find_first_not_of(' ', at + 21), 23, "RebuildUnclaimedTile();") == 0);
      ++payout;
    }
    UR_EXPECT_EQ(3u, payout);  // the start, the watchdog, the Done
  }
}

// The reads of the claimable state are numbered as they start, and only the
// newest read of each kind counts (SnLedgerCtf_AReadFromBeforeAClaimsDoneNeverLandsOverIt):
// LoadWalletLayer starts a read of the claims and one of the accounts as it
// shows both Loading, and each answer checks its kind's newest read right
// after the load's epoch, before it applies anything; LoadGas the same for the
// gas; a wallet read that finds no coldkey supersedes both reads; and these
// are the three kinds' only reads, so no answer lands unchecked. Each claim's
// Done starts both again (LoadWalletLayer after its settle:
// SnLedgerClaimSheetWiring, SnLedgerCtf_TheV1ClaimsAnswersAreWired)
UR_TEST(SnLedgerCtf_TheReadsOfTheClaimableStateAreWired) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string layer = LedgerFunctionBody(page, "void EarningsPage::LoadWalletLayer()");
  const size_t loading = layer.find("  ApplyClaims(std::nullopt, 0, Fetch::Loading);\n"
                                    "  ApplyLedger(snledger::Load::Loading, {}, {}, ledgerConfigured_);\n");
  const size_t started = layer.find("  const uint64_t claimsRead = claimsReads_.Start();\n"
                                    "  const uint64_t ledgerRead = ledgerReads_.Start();\n");
  UR_EXPECT_TRUE(loading != std::string::npos && started != std::string::npos && loading < started);
  UR_EXPECT_TRUE(started < layer.find("sn::FetchClaims(") && started < layer.find("sn::FetchLedger("));
  struct Answer {
    const char* name;
    const std::string* body;
    const char* call;
    const char* post;
    const char* guard;
    std::vector<const char*> applies;
  };
  const std::string gas = LedgerFunctionBody(page, "void EarningsPage::LoadGas()");
  const Answer answers[] = {
      {"claims", &layer, "sn::FetchClaims(host_, [this, epoch, seen, claimsRead](",
       "PostToMain([this, epoch, seen, claimsRead, ",
       "      if (*epoch != seen) return;\n"
       "      if (!claimsReads_.Current(claimsRead)) return;",
       {"ApplyClaims(std::nullopt, 0, Fetch::Failed, text);", "ApplyClaims(std::move(claims), total, Fetch::Ready"}},
      {"accounts", &layer, "sn::FetchLedger(host_, [this, epoch, seen, ledgerRead](",
       "PostToMain([this, epoch, seen, ledgerRead, ",
       "      if (*epoch != seen) return;\n"
       "      if (!ledgerReads_.Current(ledgerRead)) return;",
       {"ApplyLedger(snledger::Load::Unavailable, {}, {}, configured);",
        "ApplyLedger(snledger::Load::Ready, *accounts, unavailable, configured);"}},
      {"gas", &gas, "sn::FetchGas(host_, [this, epoch, seen, read](",
       "PostToMain([this, epoch, seen, read, ",
       "      if (*epoch != seen) return;\n"
       "      if (!gasReads_.Current(read)) return;",
       {"ApplyGas(std::move(gas));"}},
  };
  for (const Answer& answer : answers) {
    const std::string& body = *answer.body;
    const size_t call = body.find(answer.call);
    const size_t post = body.find(answer.post, call);
    const size_t guard = body.find(answer.guard, post);
    // the guard is the answer's first step: nothing between the PostToMain and it
    const size_t open = body.find("{\n", post);
    UR_EXPECT_TRUE_MSG(answer.name, call != std::string::npos && post != std::string::npos &&
                                        guard != std::string::npos && open != std::string::npos &&
                                        open + 2 == guard);
    for (const char* apply : answer.applies) {
      const size_t at = body.find(apply, guard);
      UR_EXPECT_TRUE_MSG(apply, at != std::string::npos && guard < at);
    }
  }
  UR_EXPECT_TRUE(gas.find("const uint64_t read = gasReads_.Start();") < gas.find("sn::FetchGas("));
  // no coldkey: the empty layer at once, and the reads still out drop
  const std::string wallet = LedgerFunctionBody(page, "void EarningsPage::ApplySnWallet(");
  const size_t none = wallet.find("if (!wallet_) {");
  const size_t superseded = wallet.find("    claimsReads_.Start();\n    ledgerReads_.Start();\n", none);
  UR_EXPECT_TRUE(none != std::string::npos && superseded != std::string::npos &&
                 superseded < wallet.find("claims_.clear();", none));
  // these are the only reads of the three kinds, and the only starts
  const auto count = [&page](const std::string& what) {
    size_t n = 0;
    for (size_t at = page.find(what); at != std::string::npos; at = page.find(what, at + 1)) ++n;
    return n;
  };
  UR_EXPECT_EQ(1u, count("sn::FetchClaims("));
  UR_EXPECT_EQ(1u, count("sn::FetchLedger("));
  UR_EXPECT_EQ(1u, count("sn::FetchGas("));
  UR_EXPECT_EQ(2u, count("claimsReads_.Start()"));
  UR_EXPECT_EQ(2u, count("ledgerReads_.Start()"));
  UR_EXPECT_EQ(1u, count("gasReads_.Start()"));
  // each claim's Done reads both again
  for (const char* signature : {"void EarningsPage::StartClaim(", "void EarningsPage::StartLedgerClaim()"}) {
    const std::string start = LedgerFunctionBody(page, signature);
    const size_t done = start.find(".done = [this, alive, claim, generation] {");
    UR_EXPECT_TRUE_MSG(signature, done != std::string::npos &&
                                      start.find("LoadWalletLayer();", done) != std::string::npos);
  }
  const std::string header = ReadLedgerSource("EarningsPage.hpp");
  for (const char* member : {"  snledger::NewestRead claimsReads_;\n", "  snledger::NewestRead ledgerReads_;\n",
                             "  snledger::NewestRead gasReads_;\n"}) {
    UR_EXPECT_TRUE_MSG(member, header.find(member) != std::string::npos);
  }
}

// The v1 Claims that open the claim sheet, the unclaimed tile's and the payout
// line's, are snpayout::V1ClaimEnabled's, on the claims read: while a read is
// out (a claim's Done starts one) what the page holds is the read before it's,
// whose total may still count what the claim paid, so both wait. The sheet
// waits too: between claims it lists the claims as last read, so it does not
// open while a read is out, as the v2 sheet does not, and says it is loading;
// a v1 claim in flight opens it with the claim's own rows
UR_TEST(SnLedgerCtf_TheV1ClaimWaitsForTheClaimsRead) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string tile = LedgerFunctionBody(page, "void EarningsPage::RebuildUnclaimedTile()");
  UR_EXPECT_TRUE(tile.find("const bool ready = claimsState_ == Fetch::Ready;") != std::string::npos);
  UR_EXPECT_TRUE(tile.find("claimButton_->set_sensitive(\n"
                           "      snpayout::V1ClaimEnabled(claim_.InFlight(), ready, totalClaimableRao_, Claiming()));") !=
                 std::string::npos);
  const std::string payout = LedgerFunctionBody(page, "void EarningsPage::RebuildPayoutLine()");
  UR_EXPECT_TRUE(payout.find("payoutClaimButton_->set_sensitive(\n"
                             "      view.showClaim && snpayout::V1ClaimEnabled(claim_.InFlight(), claimsState_ == Fetch::Ready,\n"
                             "                                                 totalClaimableRao_, Claiming()));") !=
                 std::string::npos);
  // no other sensitivity for either, and none by the rule before
  const auto count = [&page](const std::string& what) {
    size_t n = 0;
    for (size_t at = page.find(what); at != std::string::npos; at = page.find(what, at + 1)) ++n;
    return n;
  };
  UR_EXPECT_EQ(1u, count("payoutClaimButton_->set_sensitive("));
  UR_EXPECT_EQ(2u, count("snpayout::V1ClaimEnabled("));
  UR_EXPECT_EQ(0u, count("(ready && totalClaimableRao_ > 0 && !Claiming())"));
  UR_EXPECT_EQ(0u, count("(claim_.InFlight() || !Claiming())"));
  const std::string open = LedgerFunctionBody(page, "void EarningsPage::OnClaim()");
  const size_t waits = open.find("  if (!claim_.InFlight() && claimsState_ == Fetch::Loading) {\n"
                                 "    Notify(T_(\"loading\", \"Loading...\"), kit::Snackbar::Severity::Info);\n"
                                 "    return;\n"
                                 "  }\n");
  UR_EXPECT_TRUE(waits != std::string::npos && waits < open.find("OpenClaimSheet(CanCallApi());"));
}

// The sheet draws ClaimProgress's figure: none while its total is not known,
// and Claim names an amount only while a press claims one, never "Claim 0"
// (SnLedgerCtf_TheSheetHasNoFigureWhileNothingAboveTheFloorIsLeft). A sheet
// opened on a claim that failed about no account says why
// (SnLedgerCtf_AFailureAboutNoAccountStaysWithTheClaim). Between claims a sheet
// opens only on a read that answered: while one is out or failed it has no
// rows, which would read as a true zero, so "Claim it yourself" says so instead
UR_TEST(SnLedgerCtf_TheSheetsFigureIsWired) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string rebuild = LedgerFunctionBody(page, "void LedgerClaimSheet::Rebuild()");
  const size_t known = rebuild.find("const std::optional<int64_t> figure = progress_->Figure();\n"
                                    "  totalValue_->set_visible(figure.has_value());\n"
                                    "  if (figure) {\n"
                                    "    totalValue_->set_markup(");
  UR_EXPECT_TRUE(known != std::string::npos);
  UR_EXPECT_TRUE(rebuild.find("Glib::Markup::escape_text(FormatAlphaRao(*figure))") != std::string::npos);
  UR_EXPECT_TRUE(rebuild.find("TotalKnown()") == std::string::npos);
  size_t markups = 0;
  for (size_t at = rebuild.find("set_markup("); at != std::string::npos; at = rebuild.find("set_markup(", at + 1)) {
    ++markups;
  }
  UR_EXPECT_EQ(1u, markups);
  UR_EXPECT_TRUE(rebuild.find(": 0 < rao ? Glib::ustring(Format(T_(\"claim_amount_button\", \"Claim {}\"),") !=
                 std::string::npos);
  UR_EXPECT_TRUE(rebuild.find(": Glib::ustring(T_(\"claim\", \"Claim\")));") != std::string::npos);
  const std::string sheet =
      LedgerFunctionBody(page, "LedgerClaimSheet::LedgerClaimSheet(Gtk::Window& parent, std::string coldkey,");
  UR_EXPECT_TRUE(sheet.find("} else if (!progress_->FailureMessage().empty()) {\n"
                            "    // opened on a claim that failed about no account: why nothing moved\n"
                            "    ShowError(SnErrorMessage(progress_->FailureMessage(),") != std::string::npos);
  const std::string open = LedgerFunctionBody(page, "void EarningsPage::OnLedgerClaim()");
  const size_t wait = open.find("if (!rows && ledgerLoad_ != snledger::Load::Ready) {");
  UR_EXPECT_TRUE(wait != std::string::npos && wait < open.find("std::make_shared<LedgerClaimSheet>("));
  UR_EXPECT_TRUE(open.find("Notify(T_(\"loading\", \"Loading...\"), kit::Snackbar::Severity::Info);") !=
                 std::string::npos);
  UR_EXPECT_TRUE(open.find("Notify(T_(\"sn_ledger_unavailable\", \"Unavailable\"), "
                           "kit::Snackbar::Severity::Error);") != std::string::npos);
}

// The v1 sheet's figure while its claim runs is what the claim is moving, the
// epochs it sends, as Claim named them (every app, 2026-11-12), where it read 0
// before; its Claim names no amount while there is nothing to claim, never
// "Claim 0"; and at Done the figure is what Claim would claim again
UR_TEST(SnLedgerCtf_TheV1SheetsFigureDuringAClaim) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string rows = LedgerFunctionBody(page, "void ClaimAlphaSheet::RebuildRows()");
  UR_EXPECT_TRUE(rows.find("const int64_t figure = busy_ ? movingRao_ : ClaimableRao();\n"
                           "  const bool shown = busy_ ? 0 < movingRao_ : !unsettled;\n"
                           "  totalValue_->set_visible(shown);\n"
                           "  totalCaption_->set_visible(shown);") != std::string::npos);
  // after the claim no figure while an epoch it sent has no outcome yet (its
  // watchdog gave up): what may still land is never a 0
  UR_EXPECT_TRUE(rows.find("if (row.phase == Phase::Sending || row.phase == Phase::Sent) unsettled = true;") !=
                 std::string::npos);
  UR_EXPECT_TRUE(rows.find("Glib::Markup::escape_text(FormatAlphaRao(figure))") != std::string::npos);
  UR_EXPECT_TRUE(rows.find("FormatAlphaRao(ClaimableRao())") == std::string::npos);
  UR_EXPECT_TRUE(rows.find("const size_t count = busy_ ? movingCount_ : ClaimableEpochs().size();") !=
                 std::string::npos);
  const std::string sending = LedgerFunctionBody(page, "void ClaimAlphaSheet::OnSending(");
  UR_EXPECT_TRUE(sending.find("movingRao_ = 0;\n  movingCount_ = 0;") != std::string::npos);
  UR_EXPECT_TRUE(sending.find("row->phase = Phase::Sending;\n      row->message.clear();\n"
                              "      movingRao_ += row->claim.amountRao;\n      ++movingCount_;") !=
                 std::string::npos);
  const std::string done = LedgerFunctionBody(page, "void ClaimAlphaSheet::OnDone()");
  const size_t rebuilt = done.find("RebuildRows();");
  UR_EXPECT_TRUE(rebuilt != std::string::npos && done.find("busy_ = false;") < rebuilt);
  const std::string action = LedgerFunctionBody(page, "void ClaimAlphaSheet::RebuildAction()");
  UR_EXPECT_TRUE(action.find("claimButton_->set_label(0 < rao ? Glib::ustring(Format(T_(\"claim_amount_button\", "
                             "\"Claim {}\"),") != std::string::npos);
  UR_EXPECT_TRUE(action.find(": Glib::ustring(T_(\"claim\", \"Claim\")));") != std::string::npos);
}

// The Transaction link of URnetwork's claim (the claim relay's 4d): the SDK's
// explorer page opens only as an https web page; anything else is no link.
UR_TEST(SnLedger_TheClaimsTransactionLinkIsAnHttpsPage) {
  const std::string page = "https://evm.taostats.io/tx/0x" + std::string(64, 'a');
  UR_EXPECT_TRUE(urnw::snledger::TransactionLink(page) == page);
  for (const char* url :
       {"", "https://", "http://evm.taostats.io/tx/0x1", "HTTPS://evm.taostats.io/tx/0x1",
        "javascript:alert(1)", "file:///etc/passwd", "evm.taostats.io/tx/0x1",
        "https://evm.taostats.io/tx/0x1 more", "https://evm.taostats.io/tx/0x1\n",
        "https://evm.taostats.io/\x7f/tx"}) {
    UR_EXPECT_TRUE_MSG(url, urnw::snledger::TransactionLink(url).empty());
  }
}

}  // namespace
