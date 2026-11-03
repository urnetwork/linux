// SPDX-License-Identifier: MPL-2.0
#include "BittensorKeyHost.hpp"

#include <glib.h>

#include <algorithm>
#include <utility>

#include "BittensorKeyStore.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// one fixed id, so a post replaces the last one and a withdraw always finds it
constexpr const char* kReminderNoticeId = "bittensor-key-reminder";
// the hint the KDE clipboard (Klipper), and the clipboard managers that follow
// it, read as "a secret: keep it out of the history"
constexpr const char* kSecretHintMime = "x-kde-passwordManagerHint";

// A flow made just to ask, with no Api (the SDK's NeedsBackup and ForgetNetwork),
// on the worker. Nothing it does starts a goroutine, so it ends with the call.
// `mayAskToUnlock`: the sign-out's question asks the desktop nothing, and a
// locked store warns nothing; forgetting a deleted network may ask.
urnet::BittensorLocalKeyFlow AskingFlow(const std::string& networkId, bool mayAskToUnlock) {
  const urnet::Api none;
  return urnet::newBittensorLocalKeyFlow(none, networkId, NewBittensorKeyStore(mayAskToUnlock));
}

}  // namespace

// ---- the worker ------------------------------------------------------------------

BittensorKeyWorker::BittensorKeyWorker() : state_(std::make_shared<State>()) {}

BittensorKeyWorker::~BittensorKeyWorker() {
  {
    std::scoped_lock lock(state_->mutex);
    state_->stop = true;
    state_->jobs.clear();
  }
  state_->wake.notify_all();
  // A job may be waiting on a keyring prompt nobody answers: the thread is let
  // go, and ends with its job. It holds only what the job captured.
  if (thread_.joinable()) thread_.detach();
}

void BittensorKeyWorker::Post(std::function<void()> job) {
  {
    std::scoped_lock lock(state_->mutex);
    if (state_->stop) return;
    state_->jobs.push_back(std::move(job));
    if (!state_->started) {
      state_->started = true;
      std::shared_ptr<State> state = state_;
      thread_ = std::thread([state] {
        for (;;) {
          std::function<void()> next;
          {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock, [&] { return state->stop || !state->jobs.empty(); });
            if (state->stop) return;
            next = std::move(state->jobs.front());
            state->jobs.pop_front();
          }
          // an exception escaping a std::thread is std::terminate
          try {
            next();
          } catch (...) {
            g_warning("bittensor key: a store call failed");
          }
        }
      });
    }
  }
  state_->wake.notify_one();
}

// ---- the host ----------------------------------------------------------------------

BittensorKeyHost::BittensorKeyHost() = default;

BittensorKeyHost::~BittensorKeyHost() {
  *alive_ = false;
  reminderTimer_.disconnect();
  copyClear_.disconnect();
  if (box_) box_->flow.close();
}

void BittensorKeyHost::Visit(const urnet::Api& api, const std::string& networkId, bool guest) {
  if (networkId.empty()) {
    SignedOut();
    return;
  }
  // another network, or an Api from another network space, is another flow
  if (box_ && (box_->networkId != networkId || box_->apiHandle != api.handle())) Retire();
  if (!box_) {
    auto box = std::make_shared<FlowBox>();
    box->networkId = networkId;
    box->apiHandle = api.handle();
    // the store's first read runs here, on the main loop, where it asks the
    // desktop nothing; the Reload below reads again where it may
    box->flow = urnet::newBittensorLocalKeyFlow(api, networkId, NewBittensorKeyStore());
    if (!box->flow) {
      g_warning("bittensor key: the flow could not be made");
      return;
    }
    std::weak_ptr<bool> alive = alive_;
    std::weak_ptr<FlowBox> weakBox = box;
    box->sub = box->flow.addBittensorLocalKeyFlowListener([this, alive, weakBox] {
      PostToMain([this, alive, weakBox] {
        const std::shared_ptr<bool> live = alive.lock();
        const std::shared_ptr<FlowBox> current = weakBox.lock();
        if (!live || !*live || !current || current != box_) return;
        Refresh();
      });
    });
    box_ = box;
    observedLinkGeneration_ = 0;
  }
  box_->flow.setGuest(guest);
  Refresh();
  Reload();
  if (openScreenPending_) {
    openScreenPending_ = false;
    OpenScreen();
  }
}

void BittensorKeyHost::SignedOut() {
  openScreenPending_ = false;
  reminderTimer_.disconnect();
  Retire();
  card_ = urnet::BittensorLocalKeyCard{};
  screen_ = urnet::BittensorLocalKeyScreen{};
  words_.clear();
  if (on_changed) on_changed();
}

void BittensorKeyHost::Retire() {
  if (!box_) return;
  box_->flow.close();
  retired_.push_back(std::move(box_));
  box_.reset();
}

void BittensorKeyHost::Refresh() {
  if (!box_) return;
  urnet::BittensorLocalKeyCard card = box_->flow.getCard().value_or(urnet::BittensorLocalKeyCard{});
  urnet::BittensorLocalKeyScreen screen =
      box_->flow.getScreen().value_or(urnet::BittensorLocalKeyScreen{});
  if (screen.done && screen.open) {
    box_->flow.closeScreen();
    screen = box_->flow.getScreen().value_or(urnet::BittensorLocalKeyScreen{});
  }
  // saved or deleted since the last look: a reminder still to come would
  // remind of nothing
  const bool reminderEnded = card_.backup_reminder && !card.backup_reminder;
  card_ = std::move(card);
  screen_ = std::move(screen);
  words_ = screen_.revealed ? box_->flow.revealedWords().value_or(urnet::StringList{})
                            : std::vector<std::string>{};
  if (reminderEnded) ApplyReminder(box_->flow.reminder(0, 0, 0, false));
  const bool linked = card_.link_generation != observedLinkGeneration_;
  observedLinkGeneration_ = card_.link_generation;
  if (on_changed) on_changed();
  if (linked && on_linked) on_linked();
}

void BittensorKeyHost::OnWorker(std::function<void(urnet::BittensorLocalKeyFlow& flow)> call,
                                std::function<void()> done) {
  if (!box_) {
    if (done) done();
    return;
  }
  std::shared_ptr<FlowBox> box = box_;
  std::weak_ptr<bool> alive = alive_;
  worker_.Post([this, box, alive, call = std::move(call), done = std::move(done)] {
    call(box->flow);
    // the flow's own publish, posted during the call, runs first
    PostToMain([this, box, alive, done] {
      const std::shared_ptr<bool> live = alive.lock();
      if (!live || !*live) return;
      if (box == box_) Refresh();
      if (done) done();
    });
  });
}

std::string BittensorKeyHost::FundsDisclaimerAt(const char* place) const {
  if (!box_) return {};
  return box_->flow.fundsDisclaimerAt(place);
}

// ---- inputs ------------------------------------------------------------------------

void BittensorKeyHost::SetConsent(bool active, const std::string& coldkeySs58,
                                  int64_t fromEpoch) {
  if (box_) box_->flow.setConsent(active, coldkeySs58, fromEpoch);
}

void BittensorKeyHost::SetConsentUnavailable() {
  if (box_) box_->flow.setConsentUnavailable();
}

void BittensorKeyHost::SetGuest(bool guest) {
  if (box_) box_->flow.setGuest(guest);
}

void BittensorKeyHost::SetTop200Registered(bool registered) {
  if (box_) box_->flow.setTop200Registered(registered);
}

void BittensorKeyHost::EarningsRead(int64_t currentEpoch, int64_t claimOpenMillis,
                                    int64_t epochBlocks, bool claimable) {
  if (!box_) return;
  box_->flow.setCurrentEpoch(currentEpoch);
  ApplyReminder(box_->flow.reminder(currentEpoch, claimOpenMillis, epochBlocks, claimable));
}

// ---- the card ------------------------------------------------------------------------

void BittensorKeyHost::SetUpPayouts(bool replaceConfirmed) {
  if (box_) box_->flow.setUpPayouts(replaceConfirmed);
}

void BittensorKeyHost::SaveFirst(bool replaceConfirmed) {
  if (box_) box_->flow.saveFirst(replaceConfirmed);
}

void BittensorKeyHost::FinishSetup(bool replaceConfirmed) {
  if (box_) box_->flow.finishSetup(replaceConfirmed);
}

void BittensorKeyHost::Reload() {
  OnWorker([](urnet::BittensorLocalKeyFlow& flow) { flow.reload(); });
}

void BittensorKeyHost::OpenScreen() {
  if (!box_) {
    openScreenPending_ = true;
    return;
  }
  OnWorker([](urnet::BittensorLocalKeyFlow& flow) { flow.openScreen(); });
}

// ---- the Key screen ----------------------------------------------------------------

void BittensorKeyHost::Reveal(bool authenticated) {
  OnWorker([authenticated](urnet::BittensorLocalKeyFlow& flow) { flow.reveal(authenticated); });
}

void BittensorKeyHost::Hide() {
  if (box_) box_->flow.hide();
}

void BittensorKeyHost::ConfirmDelete(std::function<void()> done) {
  OnWorker([](urnet::BittensorLocalKeyFlow& flow) { flow.confirmDelete(); }, std::move(done));
}

void BittensorKeyHost::ConfirmKeep() {
  OnWorker([](urnet::BittensorLocalKeyFlow& flow) { flow.confirmKeep(); });
}

void BittensorKeyHost::CancelConfirmation() {
  if (box_) box_->flow.cancelConfirmation();
}

urnet::BittensorLocalKeyWordCheck BittensorKeyHost::CheckWords(const std::string& first,
                                                               const std::string& second) const {
  if (!box_) return {};
  return box_->flow.checkWords(first, second).value_or(urnet::BittensorLocalKeyWordCheck{});
}

void BittensorKeyHost::SubmitWords(const std::string& first, const std::string& second,
                                   bool authenticated) {
  if (box_) box_->flow.submitWords(first, second, authenticated);
}

void BittensorKeyHost::RetrySaveNow() {
  if (box_) box_->flow.retrySaveNow();
}

void BittensorKeyHost::CloseScreen() {
  if (box_) box_->flow.closeScreen();
}

bool BittensorKeyHost::CopyPhrase() {
  if (!box_) return false;
  const std::string phrase = box_->flow.phraseToCopy();
  auto display = Gdk::Display::get_default();
  if (phrase.empty() || !display) return false;
  Glib::Value<Glib::ustring> text;
  text.init(Glib::Value<Glib::ustring>::value_type());
  text.set(phrase);
  static const char kSecret[] = "secret";
  std::vector<Glib::RefPtr<Gdk::ContentProvider>> providers = {
      Gdk::ContentProvider::create(text),
      Gdk::ContentProvider::create(kSecretHintMime,
                                   Glib::Bytes::create(kSecret, sizeof(kSecret) - 1)),
  };
  copied_ = Gdk::ContentProvider::create(providers);
  display->get_clipboard()->set_content(copied_);
  copyClear_.disconnect();
  std::weak_ptr<bool> alive = alive_;
  copyClear_ = Glib::signal_timeout().connect_seconds(
      [this, alive]() -> bool {
        const std::shared_ptr<bool> live = alive.lock();
        if (!live || !*live) return false;
        auto display = Gdk::Display::get_default();
        if (display && copied_) {
          auto clipboard = display->get_clipboard();
          // only this copy goes: anything copied since stays
          auto held = clipboard->get_content();
          if (held && held->gobj() == copied_->gobj()) clipboard->unset_content();
        }
        copied_.reset();
        return false;
      },
      static_cast<unsigned>(urnet::BittensorLocalKeyCopySeconds));
  return true;
}

// ---- the one notification ------------------------------------------------------------

void BittensorKeyHost::ApplyReminder(
    const std::optional<urnet::BittensorLocalKeyReminder>& reminder) {
  if (!reminder || !box_) return;
  const std::string& action = reminder->action;
  if (action == urnet::BittensorLocalKeyReminderCancel) {
    reminderTimer_.disconnect();
    WithdrawReminder();
    return;
  }
  if (action == urnet::BittensorLocalKeyReminderDeliverNow) {
    reminderTimer_.disconnect();
    if (PostReminder()) box_->flow.reminderDelivered();
    return;
  }
  if (action != urnet::BittensorLocalKeyReminderSchedule) return;
  // this app's own timer, while it runs: nothing is reported until it posts
  const int64_t nowMillis = g_get_real_time() / 1000;
  const int64_t seconds = std::max<int64_t>(1, (reminder->at_millis - nowMillis + 999) / 1000);
  reminderTimer_.disconnect();
  std::weak_ptr<bool> alive = alive_;
  std::weak_ptr<FlowBox> weakBox = box_;
  reminderTimer_ = Glib::signal_timeout().connect_seconds(
      [this, alive, weakBox]() -> bool {
        const std::shared_ptr<bool> live = alive.lock();
        const std::shared_ptr<FlowBox> box = weakBox.lock();
        if (!live || !*live || !box || box != box_) return false;
        // what the flow says now: a key saved meanwhile cancels it
        const auto now = box->flow.reminder(0, 0, 0, true);
        if (now && now->action == urnet::BittensorLocalKeyReminderDeliverNow && PostReminder()) {
          box->flow.reminderDelivered();
        }
        return false;
      },
      static_cast<unsigned>(std::min<int64_t>(seconds, G_MAXUINT)));
}

bool BittensorKeyHost::PostReminder() {
  auto app = Gio::Application::get_default();
  if (!app) return false;
  auto notification = Gio::Notification::create(
      T_("bittensor_key_reminder_title_2", "Your first SN25α is claimable"));
  notification->set_body(T_("bittensor_key_reminder_body_2",
                            "Back up your Bittensor recovery phrase now. If you lose this device "
                            "before saving it, your SN25α can't be reached."));
  notification->set_default_action(std::string("app.") + kReminderAction);
  app->send_notification(kReminderNoticeId, notification);
  return true;
}

void BittensorKeyHost::WithdrawReminder() {
  if (auto app = Gio::Application::get_default()) app->withdraw_notification(kReminderNoticeId);
}

// ---- outside Earnings ------------------------------------------------------------------

namespace {

BittensorKeyWorker& AskingWorker() {
  static BittensorKeyWorker worker;
  return worker;
}

}  // namespace

void BittensorKeyNeedsBackup(const std::string& networkId,
                             std::function<void(bool needsBackup)> done) {
  if (networkId.empty()) {
    if (done) done(false);
    return;
  }
  AskingWorker().Post([networkId, done = std::move(done)] {
    urnet::BittensorLocalKeyFlow flow = AskingFlow(networkId, /*mayAskToUnlock=*/false);
    const bool needs = flow && flow.needsBackup();
    if (flow) flow.close();
    PostToMain([done, needs] {
      if (done) done(needs);
    });
  });
}

void BittensorKeyForgetNetwork(const std::string& networkId, std::function<void()> done) {
  // the reminder goes with the account, whatever the store says
  if (auto app = Gio::Application::get_default()) app->withdraw_notification(kReminderNoticeId);
  if (networkId.empty()) {
    if (done) done();
    return;
  }
  AskingWorker().Post([networkId, done = std::move(done)] {
    urnet::BittensorLocalKeyFlow flow = AskingFlow(networkId, /*mayAskToUnlock=*/true);
    if (flow) {
      if (!flow.forgetNetwork()) {
        g_warning("bittensor key: the deleted network's key could not be shown gone");
      }
      flow.close();
    }
    PostToMain([done] {
      if (done) done();
    });
  });
}

}  // namespace urnw
