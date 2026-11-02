// The out-of-balance notice: when the Connect page's held alert shows and when
// the desktop notification is posted and withdrawn.
//
// Out of balance is a billing state, not a dropped tunnel. While a connection
// is requested the tunnel keeps capturing with no provider behind it, so the
// user is told that their traffic is held and that Disconnect releases it.
// Nothing here ever disconnects: the notice only informs, and the user's own
// Disconnect (MainWindow::ToggleConnect) is the one way out. The daemon's
// dead-tunnel failsafe holds for the same state, read from the same status
// (TunnelWatchdog.hpp OutOfBalanceHold), so the sentence stays true.
//
// An episode starts when insufficient balance is first seen and ends when it
// clears. The notification is posted at most once per episode, at the first
// observation where the gate holds and a connection is requested (a Pro plan
// or a balance poll at entry defers it). It is withdrawn when the episode ends
// or the connection is no longer requested; a disconnect does not re-arm it
// within the episode.
//
// A new connect is a different matter: with no balance it is blocked and the
// entry point shows the upgrade path instead (DecideStartConnect, below). That gate
// only ever applies to starting a session, never to one that is already up.
//
// Pure and dependency-free so tests/InsufficientBalanceNoticeTest.cpp runs
// without GTK. Not thread safe: driven from the GTK main loop only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace urnw {
namespace balance_notice {

struct Signals {
  // ContractStatus.InsufficientBalance from the live connect reading.
  bool insufficientBalance = false;
  bool pro = false;
  // A purchase confirmation poll is in flight; the balance is about to change.
  bool polling = false;
  // A destination is selected, i.e. the tunnel is asked to carry traffic.
  bool connectRequested = false;
};

// The existing out-of-balance gate (mac ConnectActions).
inline bool Gate(const Signals& s) { return s.insufficientBalance && !s.pro && !s.polling; }

// The tunnel is holding traffic for an out-of-balance account: the Connect
// page shows the held alert with Upgrade and Disconnect.
inline bool HeldAlert(const Signals& s) { return Gate(s) && s.connectRequested; }

// Posts and withdraws the desktop notification through a sink with Post() and
// Withdraw(). The sink is a template parameter so tests can count calls.
class Tracker {
 public:
  template <class Sink>
  void Observe(const Signals& s, Sink& sink) {
    if (!s.insufficientBalance) {
      // the episode ended: withdraw and re-arm
      Withdraw(sink);
      posted_ = false;
      return;
    }
    if (!s.connectRequested) {
      // nothing is held any more; posted_ stays set so the episode does not
      // post again after a disconnect
      Withdraw(sink);
      return;
    }
    if (!posted_ && Gate(s)) {
      posted_ = true;
      shown_ = true;
      sink.Post();
    }
  }

  bool Shown() const { return shown_; }

 private:
  template <class Sink>
  void Withdraw(Sink& sink) {
    if (!shown_) return;
    shown_ = false;
    sink.Withdraw();
  }

  bool posted_ = false;
  bool shown_ = false;
};

// ---- the start-connect gate ----------------------------------------------------
// Connect with no balance is blocked; a connection that runs out of balance
// stays up (urnetwork/android#483). Every connect entry point asks this before
// it starts anything: the tray menu, the Connect page, a location pick, connect
// on launch and the post-sign-in connect.

// What a connect entry point is asking for.
enum class ConnectAttempt {
  // No session is up: this press would start one.
  Start,
  // A session is up (health::SessionUp): a location change or a re-assertion
  // of the tunnel the user never left. Never blocked, never disconnected.
  AlreadyConnected,
};

inline ConnectAttempt ClassifyConnect(bool sessionUp) {
  return sessionUp ? ConnectAttempt::AlreadyConnected : ConnectAttempt::Start;
}

// Whether the entry point must not start the tunnel and should show the
// upgrade path instead. Only a Start is ever blocked, by the same Gate as the
// held alert.
inline bool BlockConnect(ConnectAttempt attempt, const Signals& s) {
  return attempt == ConnectAttempt::Start && Gate(s);
}

// Out of balance outlives the reading that reported it. The SDK clears the
// contract status whenever the destination changes, the user's Disconnect
// included, so after a Disconnect the live reading says nothing about the
// balance, and a Connect press would start a tunnel that can only hold
// traffic again. The latch keeps the last known out-of-balance state until
// there is evidence the balance changed:
//   - providers attached on a live session with no insufficient balance (the
//     contracts went through), or
//   - the subscription balance rose above its lowest value seen since the
//     state was latched (a refill, a redeemed code or a purchase landed).
// Pro and a purchase poll are handled by Gate. Reset on sign-in and sign-out.
class OutOfBalanceLatch {
 public:
  struct Observation {
    // ContractStatus.InsufficientBalance from the live connect reading.
    bool insufficientBalance = false;
    // A session is up and the controller reports providers attached.
    bool providersConnected = false;
    // The subscription balance has been fetched, and its available bytes.
    bool balanceKnown = false;
    long long availableBytes = 0;
  };

  void Observe(const Observation& o) {
    if (o.insufficientBalance) {
      latched_ = true;
      NoteBalance(o);
      return;
    }
    if (!latched_) return;
    if (o.providersConnected) {
      Reset();
      return;
    }
    if (o.balanceKnown && lowKnown_ && o.availableBytes > lowBytes_) {
      Reset();
      return;
    }
    NoteBalance(o);
  }

  void Reset() {
    latched_ = false;
    lowKnown_ = false;
    lowBytes_ = 0;
  }

  bool OutOfBalance() const { return latched_; }

 private:
  void NoteBalance(const Observation& o) {
    if (!o.balanceKnown) return;
    if (!lowKnown_ || o.availableBytes < lowBytes_) {
      lowKnown_ = true;
      lowBytes_ = o.availableBytes;
    }
  }

  bool latched_ = false;
  bool lowKnown_ = false;
  long long lowBytes_ = 0;
};

// ---- the account balance on a fresh start ------------------------------------
// The latch and the live reading only know about a balance a session has run
// into. On a fresh start with an account that is already empty there is no
// contract status yet, so the first Connect used to start a tunnel that could
// only hold traffic. The subscription balance the app already fetches answers
// that, but only while it is fresh: a balance read minutes ago can be zero for
// an account funded since (and the other way round), so a stale one is read
// again before it can block, with a short timeout. A check that fails does not
// block: the server refuses the contract anyway, and the held alert and the
// notice cover that case. Same rule on every platform (android
// accountBalanceExhausted, windows BalanceGate.h, apple
// InsufficientBalancePolicy.swift).

// How recent a fetched balance must be to block a connect on its own.
inline constexpr int64_t kBalanceFreshMillis = 60 * 1000;
// How long a start connect waits for a balance read before it goes ahead.
inline constexpr int64_t kBalanceCheckTimeoutMillis = 5 * 1000;

// The subscription balance as last fetched. Times are on one monotonic clock,
// injected by the caller.
struct AccountBalance {
  bool known = false;
  bool pro = false;
  int64_t availableBytes = 0;
  // bytes held in open contracts, returned when they close
  int64_t openTransferBytes = 0;
  int64_t fetchedAtMillis = 0;
};

// Nothing left: none available and none held in open contracts. Unknown and
// Pro are never exhausted.
inline bool AccountBalanceExhausted(const AccountBalance& b) {
  return b.known && !b.pro && b.availableBytes <= 0 && b.openTransferBytes <= 0;
}

inline bool BalanceFresh(const AccountBalance& b, int64_t nowMillis) {
  return b.known && nowMillis - b.fetchedAtMillis <= kBalanceFreshMillis;
}

struct StartConnectInputs {
  // ContractStatus.InsufficientBalance from the live connect reading.
  bool insufficientBalance = false;
  // OutOfBalanceLatch::OutOfBalance
  bool latched = false;
  bool pro = false;
  bool polling = false;
  // health::SessionUp: a session is up, so this is not a start
  bool sessionUp = false;
  AccountBalance balance;
  // When the last balance check for a start connect failed or timed out, or
  // -1 for none. Within the fresh window it lets the connect go ahead.
  int64_t failedCheckAtMillis = -1;
  int64_t nowMillis = 0;
};

enum class StartConnectStep {
  // start (or keep) the connection
  Start,
  // do not start: show the upgrade path
  Upgrade,
  // the balance is stale: read it, then decide again
  FetchBalance,
};

// The start-connect decision every entry point asks. A session that is up is
// never refused. A start is refused by the live reading or the latch, then by
// a fresh balance with nothing left; with no fresh balance it is read first,
// unless a read for this attempt just failed.
inline StartConnectStep DecideStartConnect(const StartConnectInputs& in) {
  const ConnectAttempt attempt = ClassifyConnect(in.sessionUp);
  Signals s;
  s.insufficientBalance = in.insufficientBalance || in.latched;
  s.pro = in.pro;
  s.polling = in.polling;
  s.connectRequested = in.sessionUp;
  if (BlockConnect(attempt, s)) return StartConnectStep::Upgrade;
  if (attempt != ConnectAttempt::Start || in.pro || in.polling) return StartConnectStep::Start;
  if (!BalanceFresh(in.balance, in.nowMillis)) {
    const bool checkJustFailed = 0 <= in.failedCheckAtMillis &&
                                 in.nowMillis - in.failedCheckAtMillis <= kBalanceFreshMillis;
    return checkJustFailed ? StartConnectStep::Start : StartConnectStep::FetchBalance;
  }
  s.insufficientBalance = AccountBalanceExhausted(in.balance);
  return BlockConnect(attempt, s) ? StartConnectStep::Upgrade : StartConnectStep::Start;
}

// ---- reserved or used up, and recovering by itself ----------------------------
// Out of balance reads the same whether the balance is used up or only held as
// Pending by open (or abandoned) connections, which return what they do not
// use as they close. The alert says which (OutOfBalanceKindFor), and a connect
// the user asked for that the balance blocked recovers by itself
// (BalanceRecovery): a start the gate refused waits with the press (the retry
// the gate was handed), and a requested connection held out of balance waits
// too. Every reading and balance change is fed in, and once the available
// balance is back at kRecoveryThresholdBytes the connect is retried: the
// refused press runs again past the gate, or the held connection is rebuilt,
// which asks for new contracts and drops the latched status a held connection
// that sends nothing never clears. The same rules as android
// BalanceRecovery.kt.
//
// The retry is bounded:
//   - Once per recovery. A block arms it, and so does a reading below the
//     threshold; a retry disarms it. Only a reading fetched at or after the
//     arming counts, so a balance read before the block never retries.
//   - At most kRecoveryMaxRetries in a row. A new ask from the user refills
//     them, and so does a connection that stays out of the block for
//     kRecoveryBudgetResetMillis after a retry.
// It never connects a user who did not ask: only a refused press or a
// connection already requested is retried, and the user's Disconnect, another
// connect, a sign-out or Cancel ends the wait (Clear).

// The available balance at which data counts as back for a blocked connect,
// and the reserved balance whose return could bring it back. The server grants
// a contract down to 1 MiB, but a connection opens several at once and ramps
// each to 128 MiB, so a few MiB back would only block again.
inline constexpr int64_t kRecoveryThresholdBytes = 64LL * 1024 * 1024;
// Retries in a row before the user has to act again.
inline constexpr int kRecoveryMaxRetries = 3;
// How long a retried connection stays out of the block to refill the retries.
inline constexpr int64_t kRecoveryBudgetResetMillis = 10LL * 60 * 1000;

enum class OutOfBalanceKind {
  // no balance known, Pro, or the balance reads available again: say neither
  Unknown,
  // enough is reserved (Pending) that its return could bring data back
  Reserved,
  // nothing meaningful is reserved: out until the free refresh or an upgrade
  Exhausted,
};

// Reserved or used up, by the same threshold the recovery waits for: reserved
// when at least that much is held by open connections. No time is promised for
// its return; connections close when they are used up or end.
inline OutOfBalanceKind OutOfBalanceKindFor(const AccountBalance& b) {
  if (!b.known || b.pro) return OutOfBalanceKind::Unknown;
  if (kRecoveryThresholdBytes <= b.availableBytes) return OutOfBalanceKind::Unknown;
  if (kRecoveryThresholdBytes <= b.openTransferBytes) return OutOfBalanceKind::Reserved;
  return OutOfBalanceKind::Exhausted;
}

// What the Connect page says about the recovery.
struct RecoveryState {
  // a refused start is waiting for the balance (Cancel clears it)
  bool startWaiting = false;
  // a retry is still allowed: "You'll be reconnected when data is available again."
  bool retriesLeft = true;
};

enum class RecoveryStepKind {
  None,
  // run the refused press again (RecoveryStep::target), past the gate
  Start,
  // rebuild the held connection (connect again to its location)
  Rebuild,
};

template <class Target>
struct RecoveryStep {
  RecoveryStepKind kind = RecoveryStepKind::None;
  Target target{};
};

// Not thread safe: driven from the GTK main loop only. Times are on the
// monotonic clock AccountBalance::fetchedAtMillis is stamped with.
template <class Target>
class BalanceRecovery {
 public:
  RecoveryState State() const {
    RecoveryState state;
    state.startWaiting = refusedStart_.has_value();
    state.retriesLeft = retries_ < kRecoveryMaxRetries;
    return state;
  }

  // The gate refused a start the user asked for: wait for the balance to run
  // it again. A new ask refills the retries.
  void StartRefused(Target target, int64_t nowMillis) {
    refusedStart_ = std::move(target);
    retries_ = 0;
    Arm(nowMillis);
  }

  // The user took the connect into their own hands (connected, disconnected,
  // signed out, or cancelled the wait): nothing is waiting any more.
  void Clear() {
    refusedStart_.reset();
    armedAtMillis_.reset();
    retries_ = 0;
  }

  // One observation: whether the out-of-balance gate holds, whether a
  // connection is requested, and the last balance reading. Returns the retry
  // to make now, at most one per recovery.
  RecoveryStep<Target> Observe(bool gate, bool connectRequested, const AccountBalance& balance,
                               int64_t nowMillis) {
    const bool heldNow = gate && connectRequested;
    if (heldNow && !held_) Arm(nowMillis);  // a connection the user asked for is newly held
    held_ = heldNow;
    if (connectRequested && !heldNow && 0 < retries_ &&
        kRecoveryBudgetResetMillis <= nowMillis - lastRetryAtMillis_) {
      retries_ = 0;
    }
    if (!refusedStart_ && !heldNow) {
      armedAtMillis_.reset();
      return {};
    }
    if (!balance.known) return {};
    if (balance.availableBytes < kRecoveryThresholdBytes) {
      Arm(balance.fetchedAtMillis);
      return {};
    }
    if (!armedAtMillis_ || balance.fetchedAtMillis < *armedAtMillis_ ||
        kRecoveryMaxRetries <= retries_) {
      return {};
    }
    armedAtMillis_.reset();
    ++retries_;
    lastRetryAtMillis_ = nowMillis;
    RecoveryStep<Target> step;
    if (refusedStart_) {
      step.kind = RecoveryStepKind::Start;
      step.target = std::move(*refusedStart_);
      refusedStart_.reset();
    } else {
      step.kind = RecoveryStepKind::Rebuild;
    }
    return step;
  }

 private:
  void Arm(int64_t atMillis) {
    if (!armedAtMillis_) armedAtMillis_ = atMillis;
  }

  std::optional<Target> refusedStart_;
  bool held_ = false;
  std::optional<int64_t> armedAtMillis_;
  int retries_ = 0;
  int64_t lastRetryAtMillis_ = 0;
};

// What the Connect page adds for the balance: whether the data is reserved or
// used up, shown in the held alert, and the recovery's promise, shown on its
// own row under it. A refused start waits outside the held alert too (nothing
// is held while it waits), so its row shows alone, with Cancel, the only way
// to stop it.
struct RecoveryLines {
  OutOfBalanceKind kind = OutOfBalanceKind::Unknown;
  // "You'll be reconnected when data is available again."
  bool willReconnect = false;
  // Cancel beside it: a refused start has no Disconnect to stop it
  bool cancel = false;
};

inline RecoveryLines RecoveryLinesFor(const Signals& s, OutOfBalanceKind kind,
                                      const RecoveryState& recovery) {
  RecoveryLines lines;
  lines.kind = HeldAlert(s) ? kind : OutOfBalanceKind::Unknown;
  lines.willReconnect = recovery.retriesLeft && (recovery.startWaiting || HeldAlert(s));
  lines.cancel = lines.willReconnect && recovery.startWaiting;
  return lines;
}

}  // namespace balance_notice
}  // namespace urnw
