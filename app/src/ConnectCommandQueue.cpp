// See ConnectCommandQueue.hpp.
// SPDX-License-Identifier: MPL-2.0
#include "ConnectCommandQueue.hpp"

#include <cstdio>
#include <exception>
#include <utility>

namespace urnw {

ConnectCommandQueue::ConnectCommandQueue(Post post) : post_(std::move(post)) {
  worker_ = std::thread([this] { run(); });
}

ConnectCommandQueue::~ConnectCommandQueue() { Stop(); }

bool ConnectCommandQueue::Enqueue(Kind kind, Work work) {
  // from the moment a sign-out starts, nothing connects the account leaving
  if (kind == Kind::Connect && signingOut_) return false;
  {
    std::scoped_lock lock(stateLock_);
    if (stopped_) return false;
    Item item;
    item.kind = kind;
    item.generation = generation_.load();
    item.work = std::move(work);
    items_.push_back(std::move(item));
  }
  ++main_->outstanding;
  wake_.notify_one();
  return true;
}

void ConnectCommandQueue::BeginSignOut() {
  signingOut_ = true;
  // the leaving account's queued connects are a generation behind from here
  generation_.fetch_add(1);
}

void ConnectCommandQueue::WhenDrained(std::function<void()> then) {
  const bool queued = Enqueue(Kind::Device, [then] { return Apply(then); });
  // stopped: there is nothing left to wait for
  if (!queued && then) then();
}

void ConnectCommandQueue::Stop() {
  {
    std::scoped_lock lock(stateLock_);
    if (stopped_ && !worker_.joinable()) return;
    stopped_ = true;
    items_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

void ConnectCommandQueue::run() {
  for (;;) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(stateLock_);
      wake_.wait(lock, [this] { return stopped_ || !items_.empty(); });
      if (stopped_) return;
      item = std::move(items_.front());
      items_.pop_front();
    }
    Apply apply;
    // a connect made in a sign-in that has signed out since never runs
    const bool skipped =
        item.kind == Kind::Connect && item.generation != generation_.load();
    if (!skipped && item.work) {
      // an escaping exception on a worker thread is std::terminate
      try {
        apply = item.work();
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[connect] a queued command threw: %s\n", e.what());
      } catch (...) {
        std::fprintf(stderr, "[connect] a queued command threw\n");
      }
    }
    // Every command applies, a skipped one too, so the count settles.
    std::shared_ptr<MainState> main = main_;
    post_([main, apply = std::move(apply)] {
      if (apply) apply();
      if (--main->outstanding == 0 && main->settled) main->settled();
    });
  }
}

}  // namespace urnw
