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
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "SnLedgerPresentation.hpp"

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
using urnw::snledger::Phase;
using urnw::snledger::SectionFor;
using urnw::snledger::SectionView;
using urnw::snledger::Status;

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

SectionView Tile(bool hasColdkey, Load load, const std::vector<Account>& accounts,
                 bool configured, bool claiming = false) {
  return SectionFor(hasColdkey, load, accounts, configured, claiming, Tag);
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
  UR_EXPECT_FALSE(view.showPools);
  AccountView expected;
  expected.noId = 1;
  expected.claimable = "a3241000000";
  expected.paid = "a1879000000";
  expected.earned = "a5120000000";
  UR_EXPECT_EQ(1u, view.accounts.size());
  UR_EXPECT_TRUE(view.accounts.front() == expected);
  UR_EXPECT_TRUE(view.showClaim);
  UR_EXPECT_TRUE(view.claimEnabled);
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

// CL-2: with more than one pool each account names its pool, and Claim
// counts every account
UR_TEST(SnLedgerSeveralPoolsNameTheirPool) {
  const std::vector<Account> accounts = {LedgerAccount(1, 42, 2'000'000'000, 2'000'000'000),
                                         LedgerAccount(3, 5, 1'000'000'000, 0)};
  const SectionView view = Tile(true, Load::Ready, accounts, true);
  UR_EXPECT_TRUE(view.showPools);
  UR_EXPECT_EQ(2u, view.accounts.size());
  UR_EXPECT_EQ(int64_t{1}, view.accounts[0].noId);
  UR_EXPECT_EQ(int64_t{3}, view.accounts[1].noId);
  UR_EXPECT_EQ(int64_t{1'000'000'000}, urnw::snledger::ClaimableRao(accounts));
  UR_EXPECT_TRUE(view.claimEnabled);
}

// ---- the claim sheet -------------------------------------------------------------

// the rows of a sheet over two pools, the second fully paid
ClaimProgress TwoPoolSheet() {
  return ClaimProgress({LedgerAccount(1, 42, 5'120'000'000, 1'879'000'000),
                        LedgerAccount(2, 9, 1'000'000'000, 1'000'000'000),
                        LedgerAccount(3, 5, 700'000'000, 0)});
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

// APPS-SPEC.md claim (CL-4): needs_gas fails the row, keeps it claimable and
// turns on the top-up until the next press
UR_TEST(SnLedgerNeedsGasTurnsOnTheTopUp) {
  ClaimProgress progress = TwoPoolSheet();
  progress.Start();
  UR_EXPECT_TRUE(progress.Failed(1, 42, "needs_gas: the gas key holds 0.004 TAO"));
  UR_EXPECT_TRUE(progress.NeedsGas());
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->phase == Phase::Failed);
  UR_EXPECT_TRUE(RowOf(progress, 1, 42)->message == "needs_gas: the gas key holds 0.004 TAO");
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
  UR_EXPECT_TRUE(FailureFor("proof_mismatch: the proof does not verify") == Failure::Other);
  UR_EXPECT_TRUE(FailureFor("not_claimable: below the transfer floor") == Failure::Other);
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
// bounds, 2 minutes to read and 6 per account sent in turn, plus a minute, so
// it never offers Claim again while the SDK still sends
UR_TEST(SnLedgerTheWatchdogOutlastsEveryClaimTheSdkSends) {
  using urnw::snledger::ClaimWatchdogMs;
  constexpr int kMinute = 60'000;
  UR_EXPECT_EQ(9 * kMinute, ClaimWatchdogMs({}));
  UR_EXPECT_EQ(9 * kMinute, ClaimWatchdogMs({OnePool()}));
  // the paid-out pool sends nothing
  UR_EXPECT_EQ(15 * kMinute, ClaimWatchdogMs({LedgerAccount(1, 42, 5'120'000'000, 1'879'000'000),
                                              LedgerAccount(2, 9, 1'000'000'000, 1'000'000'000),
                                              LedgerAccount(3, 5, 700'000'000, 0)}));
  // an absurd answer saturates instead of overflowing the timer
  const std::vector<Account> many(10'000, OnePool());
  UR_EXPECT_EQ(std::numeric_limits<int>::max(), ClaimWatchdogMs(many));
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
  UR_EXPECT_TRUE(needsGas.find("return snledger::NeedsGas(progress_.NeedsGas(), gas_ &&") !=
                 std::string::npos);
  const std::string start = LedgerFunctionBody(page, "void EarningsPage::StartLedgerClaim()");
  const size_t settle =
      start.find("if (!SettleFlow(ledgerClaimFlow_, generation, \"v2 claim\")) return;");
  UR_EXPECT_TRUE(settle != std::string::npos);
  UR_EXPECT_TRUE(start.find("LoadWalletLayer();", settle) != std::string::npos);
  // the watchdog outlasts the SDK's own bounds for every account it may claim
  UR_EXPECT_TRUE(start.find("const int watchdogMs = snledger::ClaimWatchdogMs(ledgerAccounts_);") !=
                 std::string::npos);
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
                           "!NeedsGas());") != std::string::npos);
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
  UR_EXPECT_TRUE(v1.find("if (epochs.empty() || claiming_) return;") != std::string::npos);
  UR_EXPECT_TRUE(v2.find("if (claiming_) return;") != std::string::npos);
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
// show the stale English
UR_TEST(SnLedgerEnglishMatchesTheCatalogMsgids) {
  const std::string page = ReadLedgerSource("EarningsPage.cpp");
  const std::string catalog = ReadLedgerSource("../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  for (const char* key : {"sn_ledger_title", "sn_ledger_claimable", "sn_ledger_paid",
                          "sn_ledger_earned", "sn_ledger_pending", "sn_ledger_unavailable",
                          "sn_ledger_pool"}) {
    const std::vector<std::string> fallbacks = LedgerFallbacks(page, key);
    if (fallbacks.empty()) UR_FAIL(std::string("EarningsPage.cpp does not show ") + key);
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
