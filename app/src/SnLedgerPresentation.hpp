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
// Capture the flag (sn CAPTURETHEFLAG.md §10) gives the operator a second pool,
// and the coldkey an account there beside its pool account (kind ctf, pool
// 2^32 + n). Each account is then its own row, named by its kind: "Capture the
// flag", and "Provider pool" beside it. A pool the server could not read just
// now (the SDK's unavailable pools) is a row that says so, with no figure,
// never a zero, and the other rows show as usual. Beside a capture the flag
// account a row under the claim relay's transfer floor says it is below the
// claim minimum, and the one Claim, which claims every pool (the SDK sends them
// in one transaction), reads "Claim both" while it claims the pool's account
// and the capture the flag account. URnetwork's claim relay lists the same
// per-pool rows on its sheet, and on the tile of a network with no wallet
// here. Without a capture the flag account, as with every server before it,
// the tile and the sheets read as before.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/SnLedgerPresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
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
// in flight (v1 or v2: the gas key sends one claim at a time).
//
// Each pool in `unavailable` is a row among the accounts, by its pool, and
// hides none of them. Beside a capture the flag account an account under
// `transferFloorRao` says so and counts for no Claim, and Claim, which claims
// every pool, reads Claim both while it claims exactly the pool's account and
// the capture the flag account.
inline SectionView SectionFor(bool hasColdkey, Load load, const std::vector<Account>& accounts,
                              const std::vector<UnavailablePool>& unavailable,
                              int64_t transferFloorRao, bool claimConfigured, bool claiming,
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
    row.underFloor = naming.ctf && UnderFloor(account.claimableRao, transferFloorRao);
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
  view.claimEnabled = claimConfigured && claimableRao > 0 && !claiming;
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

// How the sheet reads a failed claim. The SDK's messages start with one of its
// stable codes ("code: detail").
enum class Failure {
  NeedsGas,        // needs_gas: the gas key cannot pay; the sheet's top-up shows
  AlreadyClaimed,  // already_claimed: a third party claimed it, so it is paid
  Unavailable,     // ledger_unavailable: the pool could not be read; nothing was sent for it
  UnderFloor,      // not_claimable, AmountTooLow: the runtime refuses an amount under its floor
  Other,           // the message, in the app's words for its code
};

inline Failure FailureFor(const std::string& message) {
  const std::string code = message.substr(0, message.find(':'));
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
// server could not read fails with ledger_unavailable, its noId and index 0. A
// failure about no single account (no wallet, v2 claims not configured, the
// ledger or the chain unreachable, nothing to claim) comes with noId 0.
class ClaimProgress {
 public:
  ClaimProgress() = default;
  // One row per account with something to claim, in the SDK's order, and one
  // per pool the server could not read, before the first account of a later
  // pool. Beside a capture the flag account, an account under
  // `transferFloorRao` waits below the claim minimum.
  ClaimProgress(const std::vector<Account>& accounts, const std::vector<UnavailablePool>& unavailable,
                int64_t transferFloorRao) {
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
      if (naming_.ctf && UnderFloor(account.claimableRao, transferFloorRao)) {
        row.phase = Phase::UnderFloor;
      }
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
      if (row.phase != Phase::Claimable && row.phase != Phase::Failed) continue;
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
  // next sheet, as on the v1 sheet.
  bool NeedsGas() const { return needsGas_; }
  // What a press claims: every row not claimed yet, short of one below the
  // claim minimum or of a pool that could not be read. A failed claim stays
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

  // Claim pressed: every row the press claims is sending.
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
    if (failure == Failure::Unavailable) {
      // the pool could not be read, so nothing was sent for it: its rows not
      // sent say so, a sent or claimed one stays, and a pool without a row
      // gets one
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
      return true;
    }
    // beside a capture the flag account the runtime's refusal of an amount
    // under its floor is the claim minimum, which the relay's price may not
    // have shown: the row waits, out of the next press
    if (naming_.ctf && failure == Failure::UnderFloor) {
      row.phase = Phase::UnderFloor;
      row.message.clear();
      return true;
    }
    // below the claim minimum the vault refuses it, and no retry helps until
    // it rises: the row keeps saying why
    if (row.phase == Phase::UnderFloor) return true;
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
// with something to claim, Pending or, beside a capture the flag account, below
// the claim minimum; each URnetwork claimed with nothing claimable since,
// Claimed with its transaction; then one row per pool the server could not
// read. Only beside a capture the flag account or an unavailable pool: without
// either, the sheet's balance says it all, as before capture the flag.
inline std::vector<RelayRow> RelayRowsFor(const std::vector<RelayAccount>& accounts,
                                          const std::vector<UnavailablePool>& unavailable,
                                          int64_t transferFloorRao) {
  std::vector<PoolRef> refs;
  for (const RelayAccount& account : accounts) refs.push_back(PoolRef{account.noId, account.kind});
  for (const UnavailablePool& pool : unavailable) refs.push_back(PoolRef{pool.noId, pool.kind});
  const PoolNaming naming = PoolNaming::Of(refs);
  std::vector<RelayRow> rows;
  if (!naming.ctf && unavailable.empty()) return rows;
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
      row.status = naming.ctf && UnderFloor(account.claimableRao, transferFloorRao)
                       ? RelayStatus::UnderFloor
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

// The tile's rows for a network with no wallet on this device, under the sum
// the relay claims (Android's snClaimRelayTileRows): the primary coldkey's
// accounts with something to claim, Pending or, beside a capture the flag
// account, below the claim minimum; then one row per pool the server could not
// read. Only with a capture the flag account among them or an unavailable pool:
// with the pool's account alone the sum says it all, as before capture the
// flag.
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
    row.status = naming.ctf && UnderFloor(account.claimableRao, transferFloorRao)
                     ? RelayStatus::UnderFloor
                     : RelayStatus::Pending;
    ctf = ctf || IsCtf(account.kind);
    rows.push_back(row);
  }
  if (!ctf && unavailable.empty()) return {};
  for (const UnavailablePool& pool : unavailable) {
    RelayRow row;
    row.noId = pool.noId;
    row.pool = naming.NameOf(pool.kind);
    row.status = RelayStatus::Unavailable;
    rows.push_back(row);
  }
  return rows;
}

// ---- capture the flag: the line of a won slot ---------------------------------------

// The roles that win a country's slots (the SDK's SnCtfRole*).
constexpr const char* kRoleProvider = "provider";
constexpr const char* kRoleExtender = "extender";

// This device's latest capture the flag win (GET /sn/ctf/client, the SDK's
// SnCtfWin): the role that won one of a country's five slots, and the country.
struct Win {
  std::string role;     // kRoleProvider or kRoleExtender
  std::string country;  // lowercase ISO 3166-1 alpha-2
};

// The line with the capture the flag account: "You won one of the Kenya
// provider slots" (sn_ctf_won_provider_slot) or the extender's
// (sn_ctf_won_extender_slot), by the role that won. None without a win, its
// country or a role this build knows: the line never guesses.
enum class WonSlot { None, Provider, Extender };

inline WonSlot WonSlotFor(const std::optional<Win>& win) {
  if (!win || win->country.empty()) return WonSlot::None;
  if (win->role == kRoleProvider) return WonSlot::Provider;
  if (win->role == kRoleExtender) return WonSlot::Extender;
  return WonSlot::None;
}

}  // namespace urnw::snledger
