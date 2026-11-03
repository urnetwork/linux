// The Settlement v2 tile on the Earnings page and its claim sheet, decided in
// pure steps so every display rule and every step of a claim is deterministic
// and testable; the page and the sheet only draw what this decided.
//
// From its switch epoch an operator pool settles in vault2: the provider's
// coldkey holds one account per pool, a cumulative that never expires, which
// the SDK reads from the operator's ledger (SnLedgerAccounts). The tile sits
// beside the v1 claims, which stay as they are: Claimable first, then Paid and
// Earned, and "Pending: +X, awaiting finalization" while a root that is not
// final yet raises the account. A ledger that cannot be read is unavailable,
// never zero. The release configures v2 claims with the chain settings'
// Vault2Address; until then the tile shows only the balances the ledger
// returns, and no Claim. Claim opens the v2 claim sheet, whose rows follow the
// SDK's progress (SnLedgerClaim): sent with the transaction, claimed with the
// amount the vault paid, or failed, where needs_gas turns on the sheet's gas
// top-up and already_claimed is paid. The top-up also shows while the gas key
// holds less than a v2 claim needs. The page reads the accounts again after
// Done.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/SnLedgerPresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace urnw::snledger {

// The coldkey's v2 account in one operator pool (the SDK's SnLedgerAccount).
// Amounts are rao (1 α = 1e9 rao).
struct Account {
  std::string vault;
  int64_t noId = 0;
  int64_t index = 0;
  int64_t finalizedRao = 0;  // the cumulative in the latest finalized root
  int64_t claimableRao = 0;  // finalized less paid: what a claim transfers now
  int64_t paidRao = 0;       // already transferred to the coldkey
  bool pending = false;      // a root that is not final yet holds the account
  int64_t pendingRao = 0;    // the account's cumulative in that root
};

// The read of the accounts: in flight, answered, or unavailable (the SDK's
// ledger_unavailable, or any read that failed: the balances are not known).
enum class Load { Loading, Ready, Unavailable };

// What the pending root adds to the account, or 0 when it adds nothing.
inline int64_t PendingIncreaseRao(const Account& account) {
  if (!account.pending || account.pendingRao <= account.finalizedRao) return 0;
  return account.pendingRao - account.finalizedRao;
}

// What a claim transfers now, over every account.
inline int64_t ClaimableRao(const std::vector<Account>& accounts) {
  int64_t total = 0;
  for (const Account& account : accounts) total += account.claimableRao;
  return total;
}

// One account's figures, formatted.
struct AccountView {
  int64_t noId = 0;
  std::string claimable;
  std::string paid;
  std::string earned;
  std::string pendingIncrease;  // sn_ledger_pending's figure; empty: no pending line

  bool operator==(const AccountView& other) const {
    return noId == other.noId && claimable == other.claimable && paid == other.paid &&
           earned == other.earned && pendingIncrease == other.pendingIncrease;
  }
};

// The line under the figures.
enum class Status {
  None,         // the accounts' figures alone
  Loading,      // loading
  Unavailable,  // sn_ledger_unavailable, and no figure: never a zero
  NothingYet,   // no_claims_yet_linux: the ledger holds no account for the coldkey
};

// What the Settlement v2 tile shows.
struct SectionView {
  bool visible = false;
  Status status = Status::None;
  std::vector<AccountView> accounts;
  bool showPools = false;     // sn_ledger_pool over each account: there is more than one
  bool showClaim = false;     // Claim, which opens the v2 claim sheet
  bool claimEnabled = false;

  bool operator==(const SectionView& other) const {
    return visible == other.visible && status == other.status && accounts == other.accounts &&
           showPools == other.showPools && showClaim == other.showClaim &&
           claimEnabled == other.claimEnabled;
  }
};

// The tile for the page. Hidden without a coldkey: the accounts are a
// coldkey's. Before the release configures v2 claims only a read that returned
// accounts shows, without Claim. Configured, the tile shows in every state with
// Claim, enabled while the read is in, something is claimable and no claim is
// in flight (v1 or v2: the gas key sends one claim at a time).
inline SectionView SectionFor(bool hasColdkey, Load load, const std::vector<Account>& accounts,
                              bool claimConfigured, bool claiming,
                              const std::function<std::string(int64_t)>& formatAlpha) {
  SectionView view;
  const bool ready = load == Load::Ready;
  if (!hasColdkey || (!claimConfigured && (!ready || accounts.empty()))) return view;
  view.visible = true;
  view.showClaim = claimConfigured;
  if (!ready) {
    view.status = load == Load::Loading ? Status::Loading : Status::Unavailable;
    return view;
  }
  if (accounts.empty()) {
    view.status = Status::NothingYet;
    return view;
  }
  view.showPools = accounts.size() > 1;
  for (const Account& account : accounts) {
    AccountView row;
    row.noId = account.noId;
    row.claimable = formatAlpha(account.claimableRao);
    row.paid = formatAlpha(account.paidRao);
    row.earned = formatAlpha(account.finalizedRao);
    const int64_t increase = PendingIncreaseRao(account);
    if (increase > 0) row.pendingIncrease = formatAlpha(increase);
    view.accounts.push_back(row);
  }
  view.claimEnabled = claimConfigured && ClaimableRao(accounts) > 0 && !claiming;
  return view;
}

// ---- the claim -------------------------------------------------------------------

// What a v2 claim needs in the gas key, in TAO. The SDK sends a claim only when
// the key covers its whole gas limit (the estimate, about 2.5M, plus a quarter)
// at the price it signs with: about 0.0173 TAO at a 5 gwei gas price, for about
// 236k gas used. The sheet suggests this top-up too.
constexpr double kClaimGasTao = 0.02;

// Whether the sheet shows the gas top-up and holds Claim: a claim came back
// needs_gas, or the gas key's balance is known and below what a v2 claim needs.
// An unknown balance holds nothing: the SDK checks it before it sends.
inline bool NeedsGas(bool claimNeededGas, bool balanceKnown, double balanceTao) {
  return claimNeededGas || (balanceKnown && balanceTao < kClaimGasTao);
}

// How long the page waits for a claim's Done before it gives up on it: past the
// SDK's own bounds, 2 minutes to read the ledger and vault2, then up to 6 to
// send and confirm each account's claim in turn, and a minute more. Giving up
// sooner would offer Claim again while the SDK still sends with the gas key.
inline int ClaimWatchdogMs(const std::vector<Account>& accounts) {
  int64_t claims = 0;
  for (const Account& account : accounts) {
    if (account.claimableRao > 0) ++claims;
  }
  const int64_t minutes = 2 + 6 * std::max<int64_t>(claims, 1) + 1;
  return static_cast<int>(std::min<int64_t>(minutes * 60'000, std::numeric_limits<int>::max()));
}

// How the sheet reads a failed claim. The SDK's messages start with one of its
// stable codes ("code: detail").
enum class Failure {
  NeedsGas,        // needs_gas: the gas key cannot pay; the sheet's top-up shows
  AlreadyClaimed,  // already_claimed: a third party claimed it, so it is paid
  Other,           // the message, in the app's words for its code
};

inline Failure FailureFor(const std::string& message) {
  const std::string code = message.substr(0, message.find(':'));
  if (code == "needs_gas") return Failure::NeedsGas;
  if (code == "already_claimed") return Failure::AlreadyClaimed;
  return Failure::Other;
}

enum class Phase { Claimable, Sending, Sent, Claimed, Failed };

// One account on the claim sheet.
struct ClaimRow {
  int64_t noId = 0;
  int64_t index = 0;
  int64_t amountRao = 0;  // the claimable; once claimed, what the vault's Claimed event paid
  Phase phase = Phase::Claimable;
  std::string txHash;   // once sent: the explorer link
  std::string message;  // a failed claim's SDK message
};

// The claim sheet's rows over its presses of Claim. Each press claims every
// account with something to claim in turn (SnLedgerClaim), which reports each
// account by (noId, index): Sent, then Confirmed or Failed; then Done once. A
// failure about no single account (no wallet, v2 claims not configured, the
// ledger or the chain unreachable, nothing to claim) comes with noId 0.
class ClaimProgress {
 public:
  ClaimProgress() = default;
  // One row per account with something to claim.
  explicit ClaimProgress(const std::vector<Account>& accounts) {
    for (const Account& account : accounts) {
      if (account.claimableRao <= 0) continue;
      ClaimRow row;
      row.noId = account.noId;
      row.index = account.index;
      row.amountRao = account.claimableRao;
      rows_.push_back(row);
    }
  }

  const std::vector<ClaimRow>& Rows() const { return rows_; }
  bool Busy() const { return busy_; }
  // A claim came back needs_gas: the top-up shows, and Claim waits for the
  // next sheet, as on the v1 sheet.
  bool NeedsGas() const { return needsGas_; }
  // What a press claims: every row not claimed yet. A failed claim stays
  // claimable, so the user may top up gas and try again.
  int64_t ClaimableRao() const {
    int64_t total = 0;
    for (const ClaimRow& row : rows_) {
      if (row.phase == Phase::Claimable || row.phase == Phase::Failed) total += row.amountRao;
    }
    return total;
  }
  bool AnyClaimed() const {
    for (const ClaimRow& row : rows_) {
      if (row.phase == Phase::Claimed) return true;
    }
    return false;
  }

  // Claim pressed: every row not claimed yet is sending.
  void Start() {
    busy_ = true;
    needsGas_ = false;
    for (ClaimRow& row : rows_) {
      if (row.phase == Phase::Claimable || row.phase == Phase::Failed) {
        row.phase = Phase::Sending;
        row.message.clear();
      }
    }
  }
  void Sent(int64_t noId, int64_t index, const std::string& txHash) {
    ClaimRow& row = Find(noId, index);
    row.phase = Phase::Sent;
    row.txHash = txHash;
  }
  void Confirmed(int64_t noId, int64_t index, const std::string& txHash, int64_t amountRao) {
    ClaimRow& row = Find(noId, index);
    row.phase = Phase::Claimed;
    if (!txHash.empty()) row.txHash = txHash;
    if (amountRao > 0) row.amountRao = amountRao;
  }
  // False for a failure about no account (noId 0): the rows stay as they are
  // and the sheet shows the message itself.
  bool Failed(int64_t noId, int64_t index, const std::string& message) {
    const Failure failure = FailureFor(message);
    if (failure == Failure::NeedsGas) needsGas_ = true;
    if (noId <= 0) return false;
    ClaimRow& row = Find(noId, index);
    if (failure == Failure::AlreadyClaimed) {
      // paid already: nothing to retry, and the read after Done shows it
      row.phase = Phase::Claimed;
      row.message.clear();
      return true;
    }
    row.phase = Phase::Failed;
    row.message = message;
    return true;
  }
  // The press is over. A row the SDK did not claim (nothing it verified was
  // claimable) is as it was before the press.
  void Done() {
    busy_ = false;
    for (ClaimRow& row : rows_) {
      if (row.phase == Phase::Sending) row.phase = Phase::Claimable;
    }
  }

 private:
  // The row for (noId, index). An account the SDK claims that the sheet did not
  // list (the ledger moved since the read) gets its own row.
  ClaimRow& Find(int64_t noId, int64_t index) {
    for (ClaimRow& row : rows_) {
      if (row.noId == noId && row.index == index) return row;
    }
    ClaimRow row;
    row.noId = noId;
    row.index = index;
    rows_.push_back(row);
    return rows_.back();
  }

  std::vector<ClaimRow> rows_;
  bool busy_ = false;
  bool needsGas_ = false;
};

// The Transaction link of URnetwork's claim (the claim relay's 4d): the
// explorer page the SDK names from the chain settings (ClaimTxUrl), opened in
// the browser only when it is a web page, an https URL with nothing that isn't
// printable in it. "" is no link.
inline std::string TransactionLink(const std::string& url) {
  static const std::string kScheme = "https://";
  if (url.size() <= kScheme.size() || url.compare(0, kScheme.size(), kScheme) != 0) return {};
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7f) return {};
  }
  return url;
}

}  // namespace urnw::snledger
