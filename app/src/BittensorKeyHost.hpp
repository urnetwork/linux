// The app's side of the SDK's one-tap Bittensor key (sdk BittensorLocalKeyFlow;
// apple BittensorKeyModel.swift): the signed-in network's flow, mirrored onto
// the GTK main loop for the Earnings page's payout card and Key screen, and what
// only this platform does for it: the Secret Service store (BittensorKeyStore),
// the clipboard and the one notification. Device authentication is the Key
// screen's (BittensorKeyAuth). Every decision about the key is the flow's: the
// card's and the screen's states, what each offers, the disclaimer each shows,
// the two-word check and when to sign. Nothing here makes a key or signs.
//
// One flow per signed-in network, made on the first visit of Earnings and read
// again (Reload) on each later one: that is where a key comes back after a
// sign-out, a crash or a reinstall. Calls that read the Secret Service, and so
// may ask the desktop to unlock it, run on this host's worker thread, never on
// the main loop: Reload, OpenScreen, Reveal, ConfirmDelete, ConfirmKeep, and
// the sign-out and account-deletion questions (NeedsBackup, ForgetNetwork).
// The flow's own actions (SetUpPayouts, FinishSetup, SubmitWords, RetrySaveNow)
// run on the SDK's goroutines. Its listener fires on any thread; the mirror is
// read again on the main loop.
//
// A flow that ends (a sign-out, another network) is closed, which cancels a
// consent on its way and lets a phrase in memory go, and is then kept: the SDK
// may still call the store it was made with from a goroutine, and the C++
// wrapper holds that store's callbacks.
//
// THE CLIPBOARD. Copy puts the phrase on the clipboard only while the words
// show (the flow's PhraseToCopy), with the hint the KDE clipboard and the
// managers that follow it read as "a secret: keep it out of the history"
// (x-kde-passwordManagerHint), and takes it back after the SDK's
// BittensorLocalKeyCopySeconds if the clipboard still holds that copy.
//
// THE NOTIFICATION. The flow's Reminder says what to do on each Earnings read.
// No Linux notification server holds one for later, so a scheduled reminder is
// this app's own timer, which posts it while the app runs; only a posted one is
// reported (ReminderDelivered), and a later read asks again. Its default action
// (app.bittensor-key-reminder, main.cpp) opens Earnings on the Key screen.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtkmm.h>

#include <urnetwork_sdk.hpp>

namespace urnw {

// One worker thread, started on the first job, for the calls that read the
// Secret Service and so may wait on the desktop's unlock prompt: in order, and
// never on the main loop. Let go (not joined) when it ends, so a prompt nobody
// answers never holds up the app's exit; it holds only what its jobs captured.
class BittensorKeyWorker {
 public:
  BittensorKeyWorker();
  ~BittensorKeyWorker();
  BittensorKeyWorker(const BittensorKeyWorker&) = delete;
  BittensorKeyWorker& operator=(const BittensorKeyWorker&) = delete;
  void Post(std::function<void()> job);

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stop = false;
    bool started = false;
  };
  std::shared_ptr<State> state_;
  std::thread thread_;
};

class BittensorKeyHost {
 public:
  // the notification's default action, registered on the application (main.cpp)
  static constexpr const char* kReminderAction = "bittensor-key-reminder";

  BittensorKeyHost();
  ~BittensorKeyHost();
  BittensorKeyHost(const BittensorKeyHost&) = delete;
  BittensorKeyHost& operator=(const BittensorKeyHost&) = delete;

  // ---- the session (main loop) --------------------------------------------------
  // A visit of Earnings, signed in to `networkId` through `api`: that network's
  // flow, made the first time, then the store read again on the worker. A guest
  // gets no key.
  void Visit(const urnet::Api& api, const std::string& networkId, bool guest);
  // Signed out, or no network: the flow ends, and the reminder's timer stops.
  void SignedOut();
  bool HasFlow() const { return box_ != nullptr; }

  // ---- what the screens draw (main loop) ------------------------------------------
  const urnet::BittensorLocalKeyCard& Card() const { return card_; }
  const urnet::BittensorLocalKeyScreen& Screen() const { return screen_; }
  // the words while they show, else empty
  const std::vector<std::string>& Words() const { return words_; }
  // the disclaimer a screen of the easy flows shows (urnet::BittensorFundsDisclaimerPlace*)
  std::string FundsDisclaimerAt(const char* place) const;

  // the mirror changed: draw again
  std::function<void()> on_changed;
  // a consent went through this flow: read the consent and the earnings again
  std::function<void()> on_linked;

  // ---- what the rest of Earnings tells the flow (main loop) ----------------------
  void SetConsent(bool active, const std::string& coldkeySs58, int64_t fromEpoch);
  void SetConsentUnavailable();
  void SetGuest(bool guest);
  void SetTop200Registered(bool registered);
  // The earnings read: the epoch for "Payouts start next epoch", and the one
  // reminder (0 for anything not known).
  void EarningsRead(int64_t currentEpoch, int64_t claimOpenMillis, int64_t epochBlocks,
                    bool claimable);

  // ---- the card -------------------------------------------------------------------
  void SetUpPayouts(bool replaceConfirmed);
  void SaveFirst(bool replaceConfirmed);
  void FinishSetup(bool replaceConfirmed);
  // the store again (Retry, and every visit)
  void Reload();
  // the Key screen for the stored key, now, or once the flow is made
  void OpenScreen();

  // ---- the Key screen ---------------------------------------------------------------
  void Reveal(bool authenticated);
  void Hide();
  // `done` runs on the main loop once the flow has answered (the mirror is
  // already read again by then): a confirmation is up, or a failure says why.
  void ConfirmDelete(std::function<void()> done = {});
  void ConfirmKeep();
  void CancelConfirmation();
  urnet::BittensorLocalKeyWordCheck CheckWords(const std::string& first,
                                               const std::string& second) const;
  void SubmitWords(const std::string& first, const std::string& second, bool authenticated);
  void RetrySaveNow();
  // Leaving: a phrase in memory is discarded.
  void CloseScreen();
  // Copy, while the words show; false with nothing to copy.
  bool CopyPhrase();

 private:
  // The flow and what keeps its callbacks alive: the listener's subscription,
  // and the network and Api it was made for.
  struct FlowBox {
    urnet::BittensorLocalKeyFlow flow;
    std::optional<urnet::Sub> sub;
    std::string networkId;
    uint64_t apiHandle = 0;
  };

  // Mirrors the flow (main loop). A finished screen closes itself.
  void Refresh();
  // Runs `call` with the current flow on the worker, then `done` on the main
  // loop, after the mirror is read again.
  void OnWorker(std::function<void(urnet::BittensorLocalKeyFlow& flow)> call,
                std::function<void()> done = {});
  void Retire();
  void ApplyReminder(const std::optional<urnet::BittensorLocalKeyReminder>& reminder);
  bool PostReminder();
  void WithdrawReminder();

  std::shared_ptr<FlowBox> box_;
  // ended flows, kept for the callbacks their goroutines may still make
  std::vector<std::shared_ptr<FlowBox>> retired_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  urnet::BittensorLocalKeyCard card_;
  urnet::BittensorLocalKeyScreen screen_;
  std::vector<std::string> words_;
  int64_t observedLinkGeneration_ = 0;
  // an OpenScreen that came before the flow was made
  bool openScreenPending_ = false;
  sigc::connection reminderTimer_;
  // the copy the clipboard may still hold, and its 60 s
  Glib::RefPtr<Gdk::ContentProvider> copied_;
  sigc::connection copyClear_;
  BittensorKeyWorker worker_;
};

// ---- outside Earnings: the sign-out and the account deletion ------------------------
// Each asks the store through a flow made just to ask, with no Api, on a worker
// of its own, and answers on the main loop.

// Whether `networkId` has a key here whose phrase isn't known to be saved: the
// sign-out and account-deletion warnings (3e to 3g). False with no key, and
// for a store that can't answer: a sign-out keeps the key anyway.
void BittensorKeyNeedsBackup(const std::string& networkId,
                             std::function<void(bool needsBackup)> done);
// After an account deletion: the key and its notes go, and the reminder.
void BittensorKeyForgetNetwork(const std::string& networkId, std::function<void()> done);

}  // namespace urnw
