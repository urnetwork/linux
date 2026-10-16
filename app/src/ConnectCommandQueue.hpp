// The connect commands' one queue (apple ConnectViewModel's command queue,
// apple 8a8d0658, 5c174fda, 852baf62). The user's connect and disconnect, and
// the device work that has to come after them (the presentation's controllers
// opening and closing, the reading taken once they are done), run on one
// worker thread in the order they were made, never on the GTK main loop.
//
// Why a worker: on a DeviceRemote every setter holds the device's state lock
// through its rpc to the daemon, and every getter, listener add, sub close and
// controller close takes that lock too. A command the daemon does not answer
// holds it for the rpc timeout (60 s), and a start_tunnel waits on the control
// socket for up to 150 s under polkit. Run on the main loop, any of them froze
// the window, and a tap's own state could not render until it returned.
//
// What each command's work returns runs on the main loop (Post), in queue
// order, so results apply in the order the commands were made. A command is
// outstanding from Enqueue until its result applied; Busy says whether one
// is, and the settled handler runs on the main loop when the last one has
// applied, which is when a tap's held state hands back to the device's.
//
// The sign-out fence: every command is stamped with the sign-in generation it
// was made in. BeginSignOut advances it, so a connect still queued for the
// account that is leaving never runs (its result is an empty apply), while
// its disconnects and device work still do. Until EndSignOut no connect is
// admitted at all. The next sign-in's commands carry the new generation, so
// the fence never blocks them.
//
// Thread contract: Enqueue, Busy, SetSettledHandler, BeginSignOut, EndSignOut,
// SigningOut and WhenDrained are main loop only. Work runs on the worker;
// Post is called on the worker. Stop (and the destructor) joins the worker:
// queued commands are dropped and the one running finishes first, which is
// unbounded, the same trade SdkHost's other workers make against a
// use-after-free.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace urnw {

class ConnectCommandQueue {
 public:
  enum class Kind {
    Connect,     // starts or drives a connection: skipped once its sign-in signs out
    Disconnect,  // ends one: always runs
    Device,      // device work ordered behind the commands: always runs
  };
  // What a command's work leaves for the main loop (may be empty).
  using Apply = std::function<void()>;
  // Runs on the worker.
  using Work = std::function<Apply()>;
  // Hands a closure to the main loop (PostToMain). Called on the worker.
  using Post = std::function<void(Apply)>;

  explicit ConnectCommandQueue(Post post);
  ~ConnectCommandQueue();
  ConnectCommandQueue(const ConnectCommandQueue&) = delete;
  ConnectCommandQueue& operator=(const ConnectCommandQueue&) = delete;

  // False, with nothing queued, for a connect while a sign-out runs, and for
  // anything once stopped.
  bool Enqueue(Kind kind, Work work);
  // A command is enqueued whose result has not applied yet.
  bool Busy() const { return main_->outstanding > 0; }
  // Runs on the main loop each time the last outstanding result has applied.
  void SetSettledHandler(std::function<void()> settled) { main_->settled = std::move(settled); }

  // A sign-out starts: the leaving account's queued connects are skipped, and
  // none is admitted until EndSignOut.
  void BeginSignOut();
  // The sign-out is over (its device is gone): connects are admitted again,
  // for the next sign-in.
  void EndSignOut() { signingOut_ = false; }
  bool SigningOut() const { return signingOut_; }

  // `then` runs on the main loop once every command enqueued before this call
  // has run and applied (a Device command of its own, so it counts as one).
  void WhenDrained(std::function<void()> then);

  // Drops what is queued, lets the running command finish, joins. Idempotent.
  void Stop();

 private:
  struct Item {
    Kind kind = Kind::Device;
    uint64_t generation = 0;
    Work work;
  };
  // Main-loop state, shared with the posted closures so a closure that runs
  // late never reaches into a destroyed queue.
  struct MainState {
    int64_t outstanding = 0;
    std::function<void()> settled;
  };

  void run();

  Post post_;
  std::shared_ptr<MainState> main_ = std::make_shared<MainState>();
  bool signingOut_ = false;               // main loop only
  std::atomic<uint64_t> generation_{1};  // advanced on the main loop, read by the worker
  std::mutex stateLock_;
  std::condition_variable wake_;
  std::deque<Item> items_;  // guarded by stateLock_
  bool stopped_ = false;    // guarded by stateLock_
  std::thread worker_;
};

}  // namespace urnw
