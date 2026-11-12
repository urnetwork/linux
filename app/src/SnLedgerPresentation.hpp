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
// amount the vault paid, or failed, where already_claimed is paid and
// needs_gas leaves the account claimable, nothing having been sent for it, with
// the sheet's gas top-up. The top-up also shows while the gas key holds less
// than a v2 claim needs. The page reads the accounts again after Done.
//
// Capture the flag (sn CAPTURETHEFLAG.md §10) gives the operator a second pool,
// and the coldkey an account there beside its pool account (kind ctf, pool
// 2^32 + n). Each account is then its own row, named by its kind: "Capture the
// flag", and "Provider pool" beside it. A pool the server could not read just
// now (the SDK's unavailable pools) is a row that says so, with no figure,
// never a zero, and the other rows show as usual. The one Claim, which claims
// every pool (the SDK sends them in one transaction), reads "Claim both" while
// it claims the pool's account and the capture the flag account. URnetwork's
// claim relay lists the same per-pool rows on its sheet, and on the tile of a
// network with no wallet here. Without a capture the flag account, as with
// every server before it, the tile and the v2 sheet read as before, but for
// the claim minimum. The tile ends with a line for every capture the flag win
// whose award is not claimed yet: "You won one of the Kenya provider slots",
// read again after a claim until the server reads the wins it paid claimed
// (WinsAfterClaim); the newest read's good answer shows, a network's answer
// beats a client's, and a read that fails keeps the lines (WinsLines). A claim
// in flight, v1's or v2's, is the session's, not a load's (ClaimInFlight), and
// while one runs its sheet shows what it moves (ClaimProgress::Figure).
//
// The claim minimum (owner, 2026-11-11, for every app): any account whose
// claimable is under the claim relay's transfer floor, alone or beside others,
// says it is below the claim minimum, and no Claim counts it: the runtime
// refuses a transfer that small, so the app never offers a claim the chain
// will refuse. The runtime's own refusal of one (not_claimable, AmountTooLow)
// reads the same. The SDK still tries such an account when a press claims the
// others, and the sheet then shows what came of it, as Android does: claimed,
// still claimable when the gas key could not pay for it (needs_gas), or Failed
// with the SDK's message.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/SnLedgerPresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace urnw::snledger {

// The kinds of pool (the SDK's SnLedgerPoolKind*): an operator's pool, which
// pays its providers and extenders, and its capture the flag pool. The SDK
// reads a kind it does not know, and an entry from a server that predates
// kinds, as a pool.
constexpr const char* kKindPool = "pool";
constexpr const char* kKindCtf = "ctf";
// The SDK's SnLedgerCtfPoolIdBase: operator n's capture the flag pool is 2^32
// + n, and no operator pool's id reaches it.
constexpr int64_t kCtfPoolIdBase = int64_t{1} << 32;

inline bool IsCtf(const std::string& kind) { return kind == kKindCtf; }

// The coldkey's v2 account in one operator pool (the SDK's SnLedgerAccount).
// Amounts are rao (1 α = 1e9 rao).
struct Account {
  std::string vault;
  int64_t noId = 0;
  std::string kind = kKindPool;  // kKindPool or kKindCtf
  int64_t index = 0;
  int64_t finalizedRao = 0;  // the cumulative in the latest finalized root
  int64_t claimableRao = 0;  // finalized less paid: what a claim transfers now
  int64_t paidRao = 0;       // already transferred to the coldkey
  bool pending = false;      // a root that is not final yet holds the account
  int64_t pendingRao = 0;    // the account's cumulative in that root
};

// A pool the server could not read just now (the SDK's
// SnLedgerUnavailablePool): whether the coldkey has an account there is not
// known.
struct UnavailablePool {
  int64_t noId = 0;
  std::string kind = kKindPool;
};

// The read of the accounts: in flight, answered, or unavailable (the SDK's
// ledger_unavailable, or any read that failed: the balances are not known).
enum class Load { Loading, Ready, Unavailable };

// ---- the pools' names ---------------------------------------------------------

// How a row names its pool, in the store's words for its kind.
enum class PoolName {
  None,      // no name: the tile's one pool, as before capture the flag
  Numbered,  // sn_ledger_pool, "Pool {no_id}"
  Provider,  // sn_ledger_provider_pool, "Provider pool": the one pool beside capture the flag
  Ctf,       // sn_ctf_pool, "Capture the flag"
};

// A pool a list holds, drawn or not: an account's or an unavailable pool's.
struct PoolRef {
  int64_t noId = 0;
  std::string kind;
};

// How the rows of a list name their pools, from every pool the list holds. A
// capture the flag account is Ctf. Beside one, the one pool is Provider, and
// several pools are Numbered so they stay apart. Without one every pool is
// Numbered, as before capture the flag.
struct PoolNaming {
  bool ctf = false;  // the list holds a capture the flag pool: two kinds
  size_t pools = 0;  // the distinct pools of kind pool it holds

  static PoolNaming Of(const std::vector<PoolRef>& refs) {
    PoolNaming naming;
    std::vector<int64_t> pools;
    for (const PoolRef& ref : refs) {
      if (IsCtf(ref.kind)) {
        naming.ctf = true;
      } else if (std::find(pools.begin(), pools.end(), ref.noId) == pools.end()) {
        pools.push_back(ref.noId);
      }
    }
    naming.pools = pools.size();
    return naming;
  }

  PoolName NameOf(const std::string& kind) const {
    if (IsCtf(kind)) return PoolName::Ctf;
    return ctf && pools == 1 ? PoolName::Provider : PoolName::Numbered;
  }
};

// Whether a claimable waits under the transfer floor, the least a claim can
// move (the claim relay's, in rao of α at its last price; 0 without one): no
// claim, URnetwork's or the device's, moves it until it rises above.
inline bool UnderFloor(int64_t claimableRao, int64_t transferFloorRao) {
  return 0 < claimableRao && 0 < transferFloorRao && claimableRao < transferFloorRao;
}

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

// One row of the tile: an account's figures, formatted, or a pool the server
// could not read, which has none.
struct AccountView {
  int64_t noId = 0;
  PoolName pool = PoolName::None;  // the name over the row
  bool unavailable = false;        // sn_ledger_pool_unavailable, and no figure: never a zero
  std::string claimable;
  std::string paid;
  std::string earned;
  std::string pendingIncrease;  // sn_ledger_pending's figure; empty: no pending line
  bool underFloor = false;      // sn_ledger_pool_under_floor under the figures

  bool operator==(const AccountView& other) const {
    return noId == other.noId && pool == other.pool && unavailable == other.unavailable &&
           claimable == other.claimable && paid == other.paid && earned == other.earned &&
           pendingIncrease == other.pendingIncrease && underFloor == other.underFloor;
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
  // one row per account, and one per pool the server could not read, by pool
  std::vector<AccountView> accounts;
  bool showClaim = false;     // Claim, which opens the v2 claim sheet
  bool claimEnabled = false;
  bool claimBoth = false;     // Claim reads sn_ledger_claim_both: it claims both kinds of pool

  bool operator==(const SectionView& other) const {
    return visible == other.visible && status == other.status && accounts == other.accounts &&
           showClaim == other.showClaim && claimEnabled == other.claimEnabled &&
           claimBoth == other.claimBoth;
  }
};

// The tile for the page. Hidden without a coldkey: the accounts are a
// coldkey's. Before the release configures v2 claims only a read that returned
// accounts shows, without Claim. Configured, the tile shows in every state with
// Claim, enabled while the read is in, something is claimable and no claim is
// in flight (v1 or v2: the gas key sends one claim at a time). While a v2
// claim is in flight (`ledgerClaimInFlight`, the session's) Claim stays
// enabled whatever the read says: it opens the claim's sheet on its rows,
// where Claim waits for it, so a page left during the claim and come back to
// can show it. (Android's tile reopens its claim in flight from Claim too, but
// there Claim follows the last read, which a refresh during the claim may find
// with nothing left to claim.)
//
// Each pool in `unavailable` is a row among the accounts, by its pool, and
// hides none of them. An account under `transferFloorRao`, alone or beside
// others, says so and counts for no Claim, and Claim, which claims every pool,
// reads Claim both while it claims exactly the pool's account and the capture
// the flag account.
inline SectionView SectionFor(bool hasColdkey, Load load, const std::vector<Account>& accounts,
                              const std::vector<UnavailablePool>& unavailable,
                              int64_t transferFloorRao, bool claimConfigured, bool claiming,
                              const std::function<std::string(int64_t)>& formatAlpha,
                              bool ledgerClaimInFlight = false) {
  SectionView view;
  const bool ready = load == Load::Ready;
  if (!hasColdkey || (!claimConfigured && (!ready || accounts.empty()))) return view;
  view.visible = true;
  view.showClaim = claimConfigured;
  // the v2 claim in flight: Claim opens its sheet, whatever the read says
  view.claimEnabled = claimConfigured && ledgerClaimInFlight;
  if (!ready) {
    view.status = load == Load::Loading ? Status::Loading : Status::Unavailable;
    return view;
  }
  if (accounts.empty() && unavailable.empty()) {
    view.status = Status::NothingYet;
    return view;
  }
  // the accounts in the SDK's order, each unavailable pool before the first
  // account of a later pool
  struct Row {
    const Account* account = nullptr;
    const UnavailablePool* unavailable = nullptr;
  };
  std::vector<Row> rows;
  std::vector<PoolRef> refs;
  for (const Account& account : accounts) {
    rows.push_back(Row{&account, nullptr});
    refs.push_back(PoolRef{account.noId, account.kind});
  }
  for (const UnavailablePool& pool : unavailable) {
    const auto later = std::find_if(rows.begin(), rows.end(), [&pool](const Row& row) {
      return row.account != nullptr && pool.noId < row.account->noId;
    });
    rows.insert(later, Row{nullptr, &pool});
    refs.push_back(PoolRef{pool.noId, pool.kind});
  }
  const PoolNaming naming = PoolNaming::Of(refs);
  // the tile's one pool has no name, as before capture the flag
  const auto nameOf = [&naming, &rows](const std::string& kind) {
    return rows.size() == 1 && !IsCtf(kind) ? PoolName::None : naming.NameOf(kind);
  };
  int64_t claimableRao = 0;
  size_t poolsClaimable = 0;
  size_t ctfsClaimable = 0;
  for (const Row& entry : rows) {
    AccountView row;
    if (entry.unavailable != nullptr) {
      row.noId = entry.unavailable->noId;
      row.pool = nameOf(entry.unavailable->kind);
      row.unavailable = true;
      view.accounts.push_back(row);
      continue;
    }
    const Account& account = *entry.account;
    row.noId = account.noId;
    row.pool = nameOf(account.kind);
    row.claimable = formatAlpha(account.claimableRao);
    row.paid = formatAlpha(account.paidRao);
    row.earned = formatAlpha(account.finalizedRao);
    const int64_t increase = PendingIncreaseRao(account);
    if (increase > 0) row.pendingIncrease = formatAlpha(increase);
    row.underFloor = UnderFloor(account.claimableRao, transferFloorRao);
    if (0 < account.claimableRao && !row.underFloor) {
      claimableRao += account.claimableRao;
      if (IsCtf(account.kind)) {
        ++ctfsClaimable;
      } else {
        ++poolsClaimable;
      }
    }
    view.accounts.push_back(row);
  }
  view.claimEnabled = claimConfigured && ((claimableRao > 0 && !claiming) || ledgerClaimInFlight);
  view.claimBoth = claimConfigured && poolsClaimable == 1 && ctfsClaimable == 1;
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
// send and confirm each account's claim alone in turn, and a minute more. Two
// or more accounts go in one batch first, which takes up to 6 to prepare and
// send and up to 6 more for its receipt (sdk 5b401282: each on its own
// deadline); an account the batch did not reach is then sent alone, so the
// SDK may still send each account alone after it. The SDK reads the ledger
// again, so a pool that was unavailable may have an account to claim by then:
// each counts as one. Giving up sooner would offer Claim again while the SDK
// still sends with the gas key.
inline int ClaimWatchdogMs(const std::vector<Account>& accounts,
                           const std::vector<UnavailablePool>& unavailable) {
  int64_t claims = static_cast<int64_t>(unavailable.size());
  for (const Account& account : accounts) {
    if (account.claimableRao > 0) ++claims;
  }
  // in claim times of 6 minutes: each account alone, and the batch's two
  const int64_t sends = claims < 2 ? 1 : claims + 2;
  const int64_t minutes = 2 + 6 * sends + 1;
  return static_cast<int>(std::min<int64_t>(minutes * 60'000, std::numeric_limits<int>::max()));
}

// How long the page waits for a v1 claim's Done before it gives up on it: past
// the SDK's own bounds, which claim the epochs one after another, each within 6
// minutes (its reads, its transaction and up to 4 minutes for the receipt),
// and a minute more. Giving up sooner would offer Claim again while the SDK
// still sends with the gas key: a second claim of an epoch whose first
// transaction is still pending sends again, and reverts.
inline int EpochClaimWatchdogMs(size_t epochs) {
  const int64_t minutes = 6 * static_cast<int64_t>(std::max<size_t>(epochs, 1)) + 1;
  return static_cast<int>(std::min<int64_t>(minutes * 60'000, std::numeric_limits<int>::max()));
}

// The v1 claim's answers so far, in order (owner, 2026-11-12: a claim in flight
// is the session's, not its sheet's). The v1 sheet keeps its own rows, so a
// sheet opened during the claim is given the claim's epochs and every answer
// since, and draws the claim in progress as a sheet left open would; one
// closed meanwhile loses none of them. The claim's end drops them (Clear): a
// sheet opened after it reads the claims again. Main loop only.
class EpochClaimAnswers {
 public:
  struct Answer {
    enum class Kind { Sent, Confirmed, Failed };
    Kind kind = Kind::Sent;
    int64_t epoch = 0;
    std::string txHash;     // Sent, Confirmed
    int64_t amountRao = 0;  // Confirmed
    std::string message;    // Failed
  };

  // Claim pressed for `epochs`: none of their answers yet.
  void Start(std::vector<int64_t> epochs) {
    epochs_ = std::move(epochs);
    answers_.clear();
  }
  void Sent(int64_t epoch, std::string txHash) {
    Answer answer;
    answer.kind = Answer::Kind::Sent;
    answer.epoch = epoch;
    answer.txHash = std::move(txHash);
    answers_.push_back(std::move(answer));
  }
  void Confirmed(int64_t epoch, std::string txHash, int64_t amountRao) {
    Answer answer;
    answer.kind = Answer::Kind::Confirmed;
    answer.epoch = epoch;
    answer.txHash = std::move(txHash);
    answer.amountRao = amountRao;
    answers_.push_back(std::move(answer));
  }
  void Failed(int64_t epoch, std::string message) {
    Answer answer;
    answer.kind = Answer::Kind::Failed;
    answer.epoch = epoch;
    answer.message = std::move(message);
    answers_.push_back(std::move(answer));
  }
  // The claim is over (its Done, its watchdog's give-up, a sign-out).
  void Clear() {
    epochs_.clear();
    answers_.clear();
  }
  const std::vector<int64_t>& Epochs() const { return epochs_; }
  const std::vector<Answer>& Answers() const { return answers_; }

 private:
  std::vector<int64_t> epochs_;
  std::vector<Answer> answers_;
};

// The stable code a message of the SDK starts with ("code: detail").
inline std::string CodeOf(const std::string& message) { return message.substr(0, message.find(':')); }

// How the sheet reads a failed claim. The SDK's messages start with one of its
// stable codes ("code: detail").
enum class Failure {
  NeedsGas,        // needs_gas: the gas key cannot pay for it; nothing was sent, so it is still claimable
  AlreadyClaimed,  // already_claimed: a third party claimed it, so it is paid
  Unavailable,     // ledger_unavailable: the pool could not be read; nothing was sent for it
  UnderFloor,      // not_claimable, AmountTooLow: the runtime refuses an amount under its floor
  Other,           // the message, in the app's words for its code
};

inline Failure FailureFor(const std::string& message) {
  const std::string code = CodeOf(message);
  if (code == "needs_gas") return Failure::NeedsGas;
  if (code == "already_claimed") return Failure::AlreadyClaimed;
  if (code == "ledger_unavailable") return Failure::Unavailable;
  if (code == "not_claimable" && message.find("AmountTooLow") != std::string::npos) {
    return Failure::UnderFloor;
  }
  return Failure::Other;
}

enum class Phase {
  Claimable,
  UnderFloor,   // sn_ledger_pool_under_floor: no claim moves it yet, and a press counts it out
  Unavailable,  // sn_ledger_pool_unavailable: its pool could not be read; no amount
  Sending,
  Sent,
  Claimed,
  Failed,
};

// One account on the claim sheet, or a pool the server could not read.
struct ClaimRow {
  int64_t noId = 0;
  int64_t index = 0;  // 0 for a pool the server could not read: the SDK names it so
  std::string kind = kKindPool;
  // the claimable; once claimed, what the vault's Claimed event paid; 0 while
  // not known, which shows no amount
  int64_t amountRao = 0;
  Phase phase = Phase::Claimable;
  std::string txHash;   // once sent: the explorer link
  std::string message;  // a failed claim's SDK message
  // a pool the server could not read, whose account is not known: the first
  // event the SDK reports for an account of that pool makes it that account's
  bool placeholder = false;
};

// The claim sheet's rows over its presses of Claim. Each press claims every
// account with something to claim (SnLedgerClaim): one transaction for one
// account, one batch for several, after which an account the batch did not
// reach is sent alone (Sent again, with its own transaction). It reports each
// account by (noId, index): Sent (every account of a batch with the batch's
// transaction), then Confirmed or Failed; then Done once. The SDK reads the
// ledger again, so it may report an account the sheet did not list. A pool the
// server could not read fails with ledger_unavailable, its noId and index 0,
// before any account's event. A failure about no single account (no wallet, v2
// claims not configured, the ledger or the chain unreachable, nothing to
// claim) comes with noId 0. Whenever the SDK claims accounts alone, the
// largest goes first, and an account the gas key cannot pay for fails
// needs_gas with nothing sent for it (sdk-ctf-v2 §2.1): it stays claimable.
//
// The SDK claims every account with something to claim, those the sheet lists
// below the claim minimum too, so each row shows what the SDK reports of it,
// as Android's rows do (snLedgerClaimProgressAfter). A press never counts an
// account under the sheet's floor, whatever came of it before: Android's
// dialog reads the accounts again, floor and all, before each claim.
class ClaimProgress {
 public:
  ClaimProgress() = default;
  // One row per account with something to claim, in the SDK's order, and one
  // per pool the server could not read, before the first account of a later
  // pool. An account under `transferFloorRao`, alone or beside others, waits
  // below the claim minimum.
  ClaimProgress(const std::vector<Account>& accounts, const std::vector<UnavailablePool>& unavailable,
                int64_t transferFloorRao)
      : floor_(transferFloorRao) {
    for (const Account& account : accounts) refs_.push_back(PoolRef{account.noId, account.kind});
    for (const UnavailablePool& pool : unavailable) refs_.push_back(PoolRef{pool.noId, pool.kind});
    naming_ = PoolNaming::Of(refs_);
    for (const Account& account : accounts) {
      if (account.claimableRao <= 0) continue;
      ClaimRow row;
      row.noId = account.noId;
      row.index = account.index;
      row.kind = account.kind;
      row.amountRao = account.claimableRao;
      if (UnderFloor(account.claimableRao, transferFloorRao)) row.phase = Phase::UnderFloor;
      rows_.push_back(row);
    }
    for (const UnavailablePool& pool : unavailable) {
      const ClaimRow row = Placeholder(pool.noId, pool.kind);
      const auto later = std::find_if(rows_.begin(), rows_.end(), [&pool](const ClaimRow& other) {
        return !other.placeholder && pool.noId < other.noId;
      });
      rows_.insert(later, row);
    }
  }

  const std::vector<ClaimRow>& Rows() const { return rows_; }
  // The row's name: the sheet names every pool, a lone one too (sn_ledger_pool).
  PoolName NameOf(const ClaimRow& row) const { return naming_.NameOf(row.kind); }
  // A press claims the pool's account and the capture the flag account
  // together, and nothing else: Claim reads Claim both.
  bool ClaimsBoth() const {
    size_t pools = 0;
    size_t ctfs = 0;
    for (const ClaimRow& row : rows_) {
      if (!Takes(row)) continue;
      if (IsCtf(row.kind)) {
        ++ctfs;
      } else {
        ++pools;
      }
    }
    return pools == 1 && ctfs == 1;
  }
  bool Busy() const { return busy_; }
  // A claim came back needs_gas: the top-up shows, and Claim waits for the
  // next sheet, as on the v1 sheet. The accounts it could not pay for are
  // still claimable.
  bool NeedsGas() const { return needsGas_; }
  // What a press claims: every row not claimed yet, short of one below the
  // claim minimum or of a pool that could not be read. A failed claim stays
  // claimable, so the user may top up gas and try again.
  int64_t ClaimableRao() const {
    int64_t total = 0;
    for (const ClaimRow& row : rows_) {
      if (Takes(row)) total += row.amountRao;
    }
    return total;
  }
  // ClaimableRao is a figure to show, as on the other apps' sheets: a row a
  // press takes holds a known amount, or no row is left at all (every row
  // claimed, or none), a true zero. While the rows left are below the claim
  // minimum, in a pool that could not be read, of an amount not known yet, or
  // in flight, they say what they are, and there is no such figure, never a
  // zero; and Claim names no amount (ClaimableRao 0). A press in flight has its
  // own figure (Figure).
  bool TotalKnown() const {
    bool left = false;
    for (const ClaimRow& row : rows_) {
      if (row.phase == Phase::Claimed) continue;
      if (Takes(row) && 0 < row.amountRao) return true;
      left = true;
    }
    return !left;
  }
  // What the press in flight is moving (every app, 2026-11-12): the accounts
  // it sends whose amount is known and not under the sheet's floor, the amount
  // Claim named when it was pressed; 0 between presses.
  int64_t MovingRao() const { return busy_ ? moving_ : 0; }
  // The sheet's figure, or none. While a press is out, what it is moving
  // (MovingRao), as Apple's and Windows' sheets show it, and none for a press
  // of nothing known; between presses, what a press claims while that is a
  // figure to show (TotalKnown), a true zero once nothing is left. Never an
  // account's under the floor, in a pool that could not be read or of an
  // amount not known, and never a zero for a claim.
  std::optional<int64_t> Figure() const {
    if (busy_) return 0 < moving_ ? std::optional<int64_t>(moving_) : std::nullopt;
    if (TotalKnown()) return ClaimableRao();
    return std::nullopt;
  }
  // The last press's failure about no account (noId 0: no wallet, v2 claims
  // not configured, the ledger or the chain unreachable, nothing to claim), the
  // SDK's message, or "": the rows stay as they are and the sheet says it, a
  // sheet opened after it too.
  const std::string& FailureMessage() const { return failure_; }
  bool AnyClaimed() const {
    for (const ClaimRow& row : rows_) {
      if (row.phase == Phase::Claimed) return true;
    }
    return false;
  }
  // This press paid an account: claimed, or claimed already by another claim,
  // which vault2 paid to the same coldkey (Android's snLedgerClaimPaid). The
  // capture the flag wins it paid read claimed once the finalized head covers
  // it, so the page reads them again on the apps' schedule (WinsAfterClaim).
  bool Paid() const { return paid_; }

  // Claim pressed: what it moves is what Claim named (MovingRao), and every
  // row the press claims is sending. One under the sheet's floor that the last
  // press left claimable or failed waits below the claim minimum again, as the
  // sheet's read listed it, until the SDK reports on it.
  void Start() {
    moving_ = ClaimableRao();
    busy_ = true;
    needsGas_ = false;
    read_ = false;
    paid_ = false;
    failure_.clear();
    reportedUnavailable_.clear();
    for (ClaimRow& row : rows_) {
      if (row.phase != Phase::Claimable && row.phase != Phase::Failed) continue;
      row.phase = Takes(row) ? Phase::Sending : Phase::UnderFloor;
      row.message.clear();
    }
  }
  void Sent(int64_t noId, int64_t index, const std::string& txHash) {
    read_ = true;
    ClaimRow& row = Find(noId, index);
    row.phase = Phase::Sent;
    row.txHash = txHash;
  }
  void Confirmed(int64_t noId, int64_t index, const std::string& txHash, int64_t amountRao) {
    read_ = true;
    paid_ = true;
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
    // the SDK's own read of the ledger answered: an account's or a pool's
    // failure, or nothing to claim in any pool, which it never says while a
    // pool is unavailable
    if (0 < noId || CodeOf(message) == "not_claimable") read_ = true;
    if (noId <= 0) {
      failure_ = message;
      return false;
    }
    if (failure == Failure::Unavailable) {
      // the pool could not be read, so nothing was sent for it: its rows not
      // sent say so, a sent or claimed one stays, and a pool without a row
      // gets one
      reportedUnavailable_.push_back(noId);
      bool listed = false;
      for (ClaimRow& row : rows_) {
        if (row.noId != noId) continue;
        listed = true;
        if (row.phase == Phase::Sent || row.phase == Phase::Claimed) continue;
        row.phase = Phase::Unavailable;
        row.message.clear();
      }
      if (!listed) Add(Placeholder(noId, KindOfPoolId(noId)));
      return true;
    }
    ClaimRow& row = Find(noId, index);
    if (failure == Failure::AlreadyClaimed) {
      // paid already: nothing to retry, and the read after Done shows it
      row.phase = Phase::Claimed;
      row.message.clear();
      paid_ = true;
      return true;
    }
    // the runtime's refusal of an amount under its floor is the claim
    // minimum, which the relay's price may not have shown: the row waits, out
    // of the next press, alone or beside others. Only this reads below the
    // claim minimum once the SDK has tried the account
    if (failure == Failure::UnderFloor) {
      row.phase = Phase::UnderFloor;
      row.message.clear();
      return true;
    }
    // the gas key could not pay for its claim, so nothing was sent for it:
    // it is still claimable, no transaction claims it now, and the sheet's
    // top-up says what to add; so too for one the sheet listed below the
    // claim minimum, which the SDK tried
    if (failure == Failure::NeedsGas) {
      if (row.phase != Phase::Claimed) {
        row.phase = Phase::Claimable;
        row.txHash.clear();
      }
      row.message.clear();
      return true;
    }
    // any other failure is Failed, with the SDK's message, whatever the
    // sheet listed the account as
    row.phase = Phase::Failed;
    row.message = message;
    return true;
  }
  // The press is over. A row the SDK did not claim (nothing it verified was
  // claimable) is as it was before the press. A pool the sheet's read could
  // not read keeps its row while the press stopped before the SDK's own read;
  // once that read answered (Android's snLedgerClaimSettled) without naming
  // the pool unavailable or reporting an account of it, the pool was read and
  // held nothing to claim, and its row goes.
  void Done() {
    busy_ = false;
    for (ClaimRow& row : rows_) {
      if (row.phase == Phase::Sending) row.phase = Phase::Claimable;
    }
    if (!read_) return;
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                               [this](const ClaimRow& row) {
                                 return row.placeholder && row.phase == Phase::Unavailable &&
                                        std::find(reportedUnavailable_.begin(),
                                                  reportedUnavailable_.end(),
                                                  row.noId) == reportedUnavailable_.end();
                               }),
                rows_.end());
  }

 private:
  // A press takes the row: not claimed yet (claimable, or failed and so worth
  // another try), and not under the sheet's floor, whatever the SDK said of it
  // since. An account the SDK reported without a listed amount counts.
  bool Takes(const ClaimRow& row) const {
    return (row.phase == Phase::Claimable || row.phase == Phase::Failed) &&
           !UnderFloor(row.amountRao, floor_);
  }

  // A pool's kind by its id, as the SDK reads an entry without a kind.
  static std::string KindOfPoolId(int64_t noId) {
    return kCtfPoolIdBase <= noId ? kKindCtf : kKindPool;
  }

  // The row of a pool the server could not read: no account, index 0 as the
  // SDK names it, no amount.
  static ClaimRow Placeholder(int64_t noId, const std::string& kind) {
    ClaimRow row;
    row.noId = noId;
    row.kind = kind;
    row.phase = Phase::Unavailable;
    row.placeholder = true;
    return row;
  }

  // Appends a row the read did not list, and names the pools again with it.
  ClaimRow& Add(const ClaimRow& row) {
    rows_.push_back(row);
    refs_.push_back(PoolRef{row.noId, row.kind});
    naming_ = PoolNaming::Of(refs_);
    return rows_.back();
  }

  // The row for an account the SDK reports, (noId, index): its own row, else
  // its pool's placeholder, which becomes the account's (the pool was read
  // after all), else a new row of the kind its pool id says (the ledger moved
  // since the read). An account's row is no placeholder.
  ClaimRow& Find(int64_t noId, int64_t index) {
    for (ClaimRow& row : rows_) {
      if (row.noId == noId && row.index == index) {
        row.placeholder = false;
        return row;
      }
    }
    for (ClaimRow& row : rows_) {
      if (row.noId == noId && row.placeholder) {
        row.index = index;
        row.placeholder = false;
        return row;
      }
    }
    ClaimRow row;
    row.noId = noId;
    row.index = index;
    row.kind = KindOfPoolId(noId);
    return Add(row);
  }

  std::vector<ClaimRow> rows_;
  std::vector<PoolRef> refs_;  // every pool of the read, and of the rows added since
  PoolNaming naming_;
  int64_t floor_ = 0;  // the transfer floor at the sheet's read; 0 without one
  bool busy_ = false;
  bool needsGas_ = false;
  // this press: the SDK's own read of the ledger answered, and the pools it
  // named unavailable
  bool read_ = false;
  std::vector<int64_t> reportedUnavailable_;
  bool paid_ = false;  // this press paid an account (Paid)
  int64_t moving_ = 0;  // what this press moves (MovingRao), from its Start
  std::string failure_;  // this press's failure about no account (FailureMessage)
};

// A claim in flight, v2's or v1's, the signed-in session's rather than a
// load's (owner, 2026-11-12). Each visit to Earnings loads the page again, and
// a load drops every answer armed for the load before it; the claim's answers
// count by this instead, which only the claim's own end and a sign-out move.
// So a page that was left during a claim and came back still has it in
// flight, and its events and its Done land. For the v2 claim the tile's Claim
// opens a sheet on its rows (SectionFor's ledgerClaimInFlight), where Claim
// waits for it, and after its Done the accounts are read again and the wins
// on the apps' schedule (WinsAfterClaim); its rows are the sheet's, shared
// with it, so the sheet pressed again claims on from them. The v1 claim has no
// rows here: its sheet keeps its own, and the tile's Claim waits for it. One
// claim at a time, as the gas key sends one: the page starts one only while
// none is in flight. Main loop only.
class ClaimInFlight {
 public:
  // Claim pressed: the claim in flight from now. `rows`, the v2 sheet's, are
  // its rows, their press started; the v1 claim has none. Each of its answers
  // carries the ticket returned.
  uint64_t Start(std::shared_ptr<ClaimProgress> rows = nullptr) {
    inFlight_ = true;
    rows_ = std::move(rows);
    if (rows_) rows_->Start();
    return ++ticket_;
  }
  bool InFlight() const { return inFlight_; }
  // Whether an answer of claim `ticket` counts: only while that claim is in
  // flight, never after its Done, the watchdog's give-up or a sign-out.
  bool Current(uint64_t ticket) const { return inFlight_ && ticket == ticket_; }
  // The rows of the v2 claim in flight, which its answers move and a sheet
  // opened during it draws; null between claims, and for the v1 claim.
  const std::shared_ptr<ClaimProgress>& Rows() const { return rows_; }
  // Its Done, or the watchdog's give-up: the press is over, and no later answer
  // of it counts. Whether it paid an account (ClaimProgress::Paid; the v1
  // claim says none).
  bool End() {
    if (!inFlight_) return false;
    inFlight_ = false;
    ++ticket_;
    if (!rows_) return false;
    rows_->Done();
    const bool paid = rows_->Paid();
    rows_.reset();
    return paid;
  }
  // A sign-out: the claim was the departed account's, and nothing of it counts
  // any more.
  void Drop() {
    inFlight_ = false;
    rows_.reset();
    ++ticket_;
  }

 private:
  std::shared_ptr<ClaimProgress> rows_;
  uint64_t ticket_ = 0;
  bool inFlight_ = false;
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

// ---- URnetwork's claim relay: its sheet's per-pool rows ---------------------------

// One account as URnetwork's claim relay reports it (the SDK's
// SnClaimRelayAccount), as its row reads it. Amounts are rao of α.
struct RelayAccount {
  std::string coldkeySs58;
  int64_t noId = 0;
  std::string kind = kKindPool;
  int64_t claimableRao = 0;
  // URnetwork's latest claim of the account: its transaction, the explorer's
  // page of it and what it paid; "" and 0 for a claim anyone else sent, or none
  std::string claimTxHash;
  std::string claimTxUrl;
  int64_t claimAmountRao = 0;
};

// What a relay row says, where Pending or Claimed sits.
enum class RelayStatus {
  Pending,      // pending_data_key: URnetwork claims it
  UnderFloor,   // sn_ledger_pool_under_floor
  Claimed,      // claim_confirmed: URnetwork's claim is its latest, and nothing is claimable since
  Unavailable,  // sn_ledger_pool_unavailable: the pool could not be read, so no amount
};

// One row of the relay sheet.
struct RelayRow {
  int64_t noId = 0;
  PoolName pool = PoolName::Numbered;
  std::string coldkeySs58;  // named only while the rows are of more than one coldkey
  int64_t amountRao = 0;    // the claimable, or what URnetwork's claim paid; 0 shows none
  RelayStatus status = RelayStatus::Pending;
  std::string txUrl;  // a Claimed row's transaction page (TransactionLink); "" without one

  bool operator==(const RelayRow& other) const {
    return noId == other.noId && pool == other.pool && coldkeySs58 == other.coldkeySs58 &&
           amountRao == other.amountRao && status == other.status && txUrl == other.txUrl;
  }
};

// The relay sheet's per-pool rows (Android's snClaimRelayRows): each account
// with something to claim, Pending or below the claim minimum; each URnetwork
// claimed with nothing claimable since, Claimed with its transaction; then one
// row per pool the server could not read. Every account the relay reports is
// listed, alone or beside others, on every server, as Android has listed them
// since the claimed state: the sheet's balance adds up only the primary
// coldkey's accounts, and a lone account below the claim minimum says so.
inline std::vector<RelayRow> RelayRowsFor(const std::vector<RelayAccount>& accounts,
                                          const std::vector<UnavailablePool>& unavailable,
                                          int64_t transferFloorRao) {
  std::vector<PoolRef> refs;
  for (const RelayAccount& account : accounts) refs.push_back(PoolRef{account.noId, account.kind});
  for (const UnavailablePool& pool : unavailable) refs.push_back(PoolRef{pool.noId, pool.kind});
  const PoolNaming naming = PoolNaming::Of(refs);
  std::vector<RelayRow> rows;
  std::vector<std::string> coldkeys;
  for (const RelayAccount& account : accounts) {
    const bool claimed = !account.claimTxHash.empty() && account.claimableRao <= 0;
    if (account.claimableRao <= 0 && !claimed) continue;
    RelayRow row;
    row.noId = account.noId;
    row.pool = naming.NameOf(account.kind);
    row.coldkeySs58 = account.coldkeySs58;
    if (claimed) {
      row.status = RelayStatus::Claimed;
      row.amountRao = account.claimAmountRao;
      row.txUrl = TransactionLink(account.claimTxUrl);
    } else {
      row.status = UnderFloor(account.claimableRao, transferFloorRao) ? RelayStatus::UnderFloor
                                                                      : RelayStatus::Pending;
      row.amountRao = account.claimableRao;
    }
    if (std::find(coldkeys.begin(), coldkeys.end(), account.coldkeySs58) == coldkeys.end()) {
      coldkeys.push_back(account.coldkeySs58);
    }
    rows.push_back(row);
  }
  if (coldkeys.size() < 2) {
    for (RelayRow& row : rows) row.coldkeySs58.clear();
  }
  for (const UnavailablePool& pool : unavailable) {
    RelayRow row;
    row.noId = pool.noId;
    row.pool = naming.NameOf(pool.kind);
    row.status = RelayStatus::Unavailable;
    rows.push_back(row);
  }
  return rows;
}

// Whether the relay's claimable is a total to show (Android's
// snClaimRelayTotalKnown): it adds up the primary coldkey's accounts, leaving
// out the pools the server could not read. With none of the coldkey's pools
// read there is nothing to add up: the unavailable rows say so, never a zero.
inline bool RelayTotalKnown(const std::vector<RelayAccount>& accounts, const std::string& coldkeySs58,
                            const std::vector<UnavailablePool>& unavailable) {
  if (unavailable.empty()) return true;
  return std::any_of(accounts.begin(), accounts.end(), [&coldkeySs58](const RelayAccount& account) {
    return account.coldkeySs58 == coldkeySs58;
  });
}

// The tile's rows for a network with no wallet on this device, under the sum
// the relay claims (Android's snClaimRelayTileRows): the primary coldkey's
// accounts with something to claim, once there are two or one of them is a
// capture the flag account, or beside a pool the server could not read; then
// one row per such pool. With one pool's account alone the sum says it all
// (RelayTileSumUnderFloor says when it is below the claim minimum). Each row
// is Pending or below the claim minimum.
inline std::vector<RelayRow> RelayTileRowsFor(const std::vector<RelayAccount>& accounts,
                                              const std::string& coldkeySs58,
                                              const std::vector<UnavailablePool>& unavailable,
                                              int64_t transferFloorRao) {
  std::vector<PoolRef> refs;
  for (const RelayAccount& account : accounts) refs.push_back(PoolRef{account.noId, account.kind});
  for (const UnavailablePool& pool : unavailable) refs.push_back(PoolRef{pool.noId, pool.kind});
  const PoolNaming naming = PoolNaming::Of(refs);
  std::vector<RelayRow> rows;
  bool ctf = false;
  for (const RelayAccount& account : accounts) {
    if (account.coldkeySs58 != coldkeySs58 || account.claimableRao <= 0) continue;
    RelayRow row;
    row.noId = account.noId;
    row.pool = naming.NameOf(account.kind);
    row.amountRao = account.claimableRao;
    row.status = UnderFloor(account.claimableRao, transferFloorRao) ? RelayStatus::UnderFloor
                                                                    : RelayStatus::Pending;
    ctf = ctf || IsCtf(account.kind);
    rows.push_back(row);
  }
  if (rows.size() < 2 && !ctf && unavailable.empty()) return {};
  for (const UnavailablePool& pool : unavailable) {
    RelayRow row;
    row.noId = pool.noId;
    row.pool = naming.NameOf(pool.kind);
    row.status = RelayStatus::Unavailable;
    rows.push_back(row);
  }
  return rows;
}

// Whether the tile's sum, drawn without rows (RelayTileRowsFor has none), is
// one account below the claim minimum: it says so under the sum, as the
// wallet's tile says it under a lone account's figures.
inline bool RelayTileSumUnderFloor(const std::vector<RelayAccount>& accounts,
                                   const std::string& coldkeySs58,
                                   const std::vector<UnavailablePool>& unavailable,
                                   int64_t transferFloorRao) {
  if (!RelayTileRowsFor(accounts, coldkeySs58, unavailable, transferFloorRao).empty()) return false;
  return std::any_of(accounts.begin(), accounts.end(),
                     [&coldkeySs58, transferFloorRao](const RelayAccount& account) {
                       return account.coldkeySs58 == coldkeySs58 &&
                              UnderFloor(account.claimableRao, transferFloorRao);
                     });
}

// ---- capture the flag: the lines of won slots ---------------------------------------

// The roles that win a country's slots (the SDK's SnCtfRole*).
constexpr const char* kRoleProvider = "provider";
constexpr const char* kRoleExtender = "extender";

// Whether a win's award has been claimed (the SDK's SnCtfWinClaimState*, the
// route's claimed false / true / null). Only an unclaimed win has a line; an
// unknown one reads neither as claimed nor as unclaimed.
constexpr const char* kClaimStateUnclaimed = "unclaimed";
constexpr const char* kClaimStateClaimed = "claimed";
constexpr const char* kClaimStateUnknown = "unknown";

// One capture the flag win (GET /sn/ctf/client, the SDK's SnCtfWin): the role
// that won one of a country's five slots in the epoch's auction, and the
// country. The SDK asks with the network's credential, so the wins are those
// of all of the network's clients, as ur.io shows them.
struct Win {
  std::string role;     // kRoleProvider or kRoleExtender; a later server may add another
  std::string country;  // lowercase ISO 3166-1 alpha-2
  int64_t epoch = 0;
  std::string claimState = kClaimStateUnclaimed;  // kClaimState*
};

// A win's line: "You won one of the Kenya provider slots"
// (sn_ctf_won_provider_slot) or the extender's (sn_ctf_won_extender_slot), by
// the role that won. None without its country or a role this build knows: the
// line never guesses.
enum class WonSlot { None, Provider, Extender };

inline WonSlot WonSlotFor(const Win& win) {
  if (win.country.empty()) return WonSlot::None;
  if (win.role == kRoleProvider) return WonSlot::Provider;
  if (win.role == kRoleExtender) return WonSlot::Extender;
  return WonSlot::None;
}

// One line on the tile.
struct WonLine {
  WonSlot slot = WonSlot::None;
  std::string country;

  bool operator==(const WonLine& other) const {
    return slot == other.slot && country == other.country;
  }
};

// The tile's lines (owner, 2026-11-11): one for every win whose award is not
// claimed yet (the SDK's UnclaimedWins), newest first, and within an epoch in
// the SDK's order, providers before extenders. A win claimed or of unknown
// claim, or one the line would have to guess at, has none. Two wins of one
// role and country (two epochs, or two of the network's clients) are two
// lines.
inline std::vector<WonLine> WonLinesFor(std::vector<Win> wins) {
  std::stable_sort(wins.begin(), wins.end(),
                   [](const Win& a, const Win& b) { return a.epoch > b.epoch; });
  std::vector<WonLine> lines;
  for (const Win& win : wins) {
    const WonSlot slot = WonSlotFor(win);
    if (win.claimState != kClaimStateUnclaimed || slot == WonSlot::None) continue;
    lines.push_back(WonLine{slot, win.country});
  }
  return lines;
}

// One good answer of GET /sn/ctf/client (the SDK's SnCtfClientResult): the
// wins, and the client it answered for (ClientId). That is "" for the
// network's answer, whose wins span all of the network's clients, and set for
// one client's answer: the SDK's fallback to the client token after the
// network's read failed, or a server before the network's answer, which lists
// that client's wins alone. A list of wins alone, converted, is the network's
// answer.
struct WinsAnswer {
  std::vector<Win> wins;
  std::string clientId;  // "" for the network's answer

  WinsAnswer(std::vector<Win> answerWins = {}, std::string answerClientId = {})
      : wins(std::move(answerWins)), clientId(std::move(answerClientId)) {}
  // the network's answer, which lists every one of its clients' wins
  bool Network() const { return clientId.empty(); }
};

// The wins the lines are drawn from (every app, 2026-11-12). Reads overlap:
// each visit's load, a claim's Done and the reads after it start at their
// times, and their answers land in any order. The lines show the newest read's
// good answer, by the order the reads started: an answer replaces the wins
// unless they already show a newer read's, so an older answer that lands late
// never replaces a newer one. A read that failed (the SDK's ctf_unavailable),
// an answer that cannot be read, or a read that could not be asked (no steady
// device), all nullopt, changes nothing: it never blanks the lines, nor keeps
// an older read's good answer still to land from showing.
//
// A network's answer beats a client's (every app, 2026-11-12): a client's
// answer lists one client's wins, so once a network's answer has shown since
// the last sign-out (Reset), a client's answer is a read that failed and
// replaces nothing. It shows only while no network's answer has, and a
// network's answer replaces it, whichever read started first.
//
// Only a good answer that no longer lists a win takes its line away, or a
// sign-out (Reset), after which no answer of a read from before shows and the
// first read shows none until it answers. As Windows' snpayout::CtfWinsLines
// and Android's SnCtfWinsLines. Main loop only.
class WinsLines {
 public:
  // A read starts: its number, which its answer carries.
  uint64_t Start() { return ++started_; }
  // The answer of read `read`: true when the wins changed.
  bool Apply(uint64_t read, std::optional<WinsAnswer> answer) {
    if (!answer || read <= resetAt_) return false;
    const bool replaces =
        answer->Network() ? !network_ || shown_ < read : !network_ && shown_ < read;
    if (!replaces) return false;
    shown_ = read;
    network_ = answer->Network();
    wins_ = std::move(answer->wins);
    return true;
  }
  // A sign-out: no wins, and no answer of a read from before shows.
  void Reset() {
    wins_.clear();
    resetAt_ = started_;
    shown_ = started_;
    network_ = false;
  }
  const std::vector<Win>& Wins() const { return wins_; }

 private:
  uint64_t started_ = 0;  // the newest read's number
  uint64_t resetAt_ = 0;  // the reads up to this one started before the last sign-out
  // the read whose good answer the wins are, or, after a sign-out, the newest
  // read from before it
  uint64_t shown_ = 0;
  bool network_ = false;  // the wins are a network's answer
  std::vector<Win> wins_;
};

// When the wins are read again after a v2 claim that paid an account, in ms
// after its Done, beside the read at Done (every app's schedule). The SDK's
// Done comes once the claim is in a block, but the server says a win is
// claimed only once vault2's claimed at the finalized head covers it, so the
// read at Done can still list it.
constexpr std::array<int, 3> kWinsReadsAfterClaimMs = {30'000, 60'000, 120'000};

// The reads of the wins after a v2 claim's Done: one at Done, after any claim,
// and when the claim paid an account (claimed, or claimed already by another)
// one more at each of kWinsReadsAfterClaimMs, each timer set as the last one
// fires, so a slow answer moves none of them. Each read is as every read: the
// newest read's good answer shows, and a failure keeps the lines (WinsLines).
// The next claim that paid starts the reads over, and one that paid nothing
// leaves them to run. A visit to Earnings, which loads the page again, leaves
// them to run; Stop() drops what is still to come when the user signs out, and
// the page's timers do nothing once the page is gone
// (EarningsPage::WinsAfterClaimPorts). Main loop only.
class WinsAfterClaim {
 public:
  // Runs `fire` once, `delayMs` from now, on the main loop.
  using Schedule = std::function<void(int delayMs, std::function<void()> fire)>;

  struct Ports {
    std::function<void()> read;  // reads the wins into the lines (WinsLines)
    Schedule schedule;
  };

  explicit WinsAfterClaim(Ports ports) : ports_(std::move(ports)) {}
  WinsAfterClaim(const WinsAfterClaim&) = delete;
  WinsAfterClaim& operator=(const WinsAfterClaim&) = delete;

  void Done(bool paid) {
    if (paid) ++run_;
    Read();
    if (paid) Next(run_, 0);
  }

  void Stop() { ++run_; }

 private:
  void Read() {
    if (ports_.read) ports_.read();
  }

  // The read at step `step` of the schedule, for run `run`: none once another
  // run started or Stop() came.
  void Next(uint64_t run, size_t step) {
    if (step >= kWinsReadsAfterClaimMs.size() || !ports_.schedule) return;
    const int since = step == 0 ? 0 : kWinsReadsAfterClaimMs[step - 1];
    ports_.schedule(kWinsReadsAfterClaimMs[step] - since, [this, run, step] {
      if (run != run_) return;
      Read();
      Next(run, step + 1);
    });
  }

  Ports ports_;
  uint64_t run_ = 0;
};

}  // namespace urnw::snledger
