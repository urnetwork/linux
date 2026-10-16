// The connect commands' queue (ConnectCommandQueue.hpp): order, results
// applied on the main loop in that order, the outstanding count and its
// settle, the sign-out fence and the drain barrier. A fake main loop collects
// what the worker posts and the test runs it on its own thread, and gates
// hold a command inside its work until the test releases it, so every
// ordering below is forced rather than raced.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ConnectCommandQueue.hpp"

namespace {

using urnw::ConnectCommandQueue;

// What the worker posts, run by the test thread as the main loop would.
class FakeMainLoop {
 public:
  ConnectCommandQueue::Post Poster() {
    return [this](ConnectCommandQueue::Apply apply) {
      {
        std::scoped_lock lock(stateLock_);
        posted_.push_back(std::move(apply));
      }
      changed_.notify_all();
    };
  }
  // Waits for the worker to have posted `count` closures in all. The bound
  // only keeps a broken queue from hanging the run; nothing waits on time.
  bool WaitPosted(size_t count) {
    std::unique_lock<std::mutex> lock(stateLock_);
    return changed_.wait_for(lock, std::chrono::seconds(10),
                             [&] { return ran_ + posted_.size() >= count; });
  }
  // Runs what has been posted so far, in order.
  void RunPosted() {
    for (;;) {
      ConnectCommandQueue::Apply apply;
      {
        std::scoped_lock lock(stateLock_);
        if (posted_.empty()) return;
        apply = std::move(posted_.front());
        posted_.pop_front();
        ++ran_;
      }
      if (apply) apply();
    }
  }

 private:
  std::mutex stateLock_;
  std::condition_variable changed_;
  std::deque<ConnectCommandQueue::Apply> posted_;
  size_t ran_ = 0;
};

// Holds a command inside its work until Open.
class Gate {
 public:
  void Open() {
    {
      std::scoped_lock lock(stateLock_);
      open_ = true;
    }
    changed_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(stateLock_);
    changed_.wait(lock, [&] { return open_; });
  }
  // Signalled by the held command once it is inside its work.
  void NoteEntered() {
    {
      std::scoped_lock lock(stateLock_);
      entered_ = true;
    }
    changed_.notify_all();
  }
  bool WaitEntered() {
    std::unique_lock<std::mutex> lock(stateLock_);
    return changed_.wait_for(lock, std::chrono::seconds(10), [&] { return entered_; });
  }

 private:
  std::mutex stateLock_;
  std::condition_variable changed_;
  bool open_ = false;
  bool entered_ = false;
};

// The worker's record of what ran, in order (read after the posts landed).
class Log {
 public:
  void Add(const std::string& entry) {
    std::scoped_lock lock(stateLock_);
    entries_.push_back(entry);
  }
  std::vector<std::string> Entries() {
    std::scoped_lock lock(stateLock_);
    return entries_;
  }

 private:
  std::mutex stateLock_;
  std::vector<std::string> entries_;
};

std::string Joined(const std::vector<std::string>& entries) {
  std::string out;
  for (const auto& entry : entries) out += (out.empty() ? "" : ",") + entry;
  return out;
}

}  // namespace

UR_TEST(connectCommandQueueRunsOffTheCallingThreadInOrder) {
  FakeMainLoop main;
  ConnectCommandQueue queue(main.Poster());
  Log worker;
  std::vector<std::string> applied;
  std::vector<std::thread::id> workerThreads;
  std::mutex threadsLock;
  const auto command = [&](const std::string& name) {
    return [&, name] {
      {
        std::scoped_lock lock(threadsLock);
        workerThreads.push_back(std::this_thread::get_id());
      }
      worker.Add(name);
      return ConnectCommandQueue::Apply([&applied, name] { applied.push_back(name); });
    };
  };
  UR_EXPECT_TRUE(queue.Enqueue(ConnectCommandQueue::Kind::Connect, command("connect")));
  UR_EXPECT_TRUE(queue.Enqueue(ConnectCommandQueue::Kind::Disconnect, command("disconnect")));
  UR_EXPECT_TRUE(queue.Enqueue(ConnectCommandQueue::Kind::Device, command("read")));
  UR_EXPECT_TRUE(main.WaitPosted(3));
  // nothing applies until the main loop runs it
  UR_EXPECT_TRUE(applied.empty());
  main.RunPosted();
  UR_EXPECT_TRUE(Joined(worker.Entries()) == "connect,disconnect,read");
  UR_EXPECT_TRUE(Joined(applied) == "connect,disconnect,read");
  for (const auto& id : workerThreads) UR_EXPECT_TRUE(id != std::this_thread::get_id());
}

UR_TEST(connectCommandQueueIsBusyUntilTheLastResultApplies) {
  FakeMainLoop main;
  ConnectCommandQueue queue(main.Poster());
  int settled = 0;
  queue.SetSettledHandler([&] { ++settled; });
  Gate gate;
  UR_EXPECT_FALSE(queue.Busy());
  queue.Enqueue(ConnectCommandQueue::Kind::Connect, [&] {
    gate.NoteEntered();
    gate.Wait();
    return ConnectCommandQueue::Apply();
  });
  queue.Enqueue(ConnectCommandQueue::Kind::Disconnect, [] { return ConnectCommandQueue::Apply(); });
  UR_EXPECT_TRUE(gate.WaitEntered());
  // held inside the sdk: still outstanding, and nothing has settled
  UR_EXPECT_TRUE(queue.Busy());
  gate.Open();
  UR_EXPECT_TRUE(main.WaitPosted(2));
  // both ran, but their results have not applied on the main loop yet
  UR_EXPECT_TRUE(queue.Busy());
  UR_EXPECT_EQ(0, settled);
  main.RunPosted();
  UR_EXPECT_FALSE(queue.Busy());
  // once, when the last of them applied
  UR_EXPECT_EQ(1, settled);
}

UR_TEST(connectCommandQueueSkipsTheLeavingAccountsConnects) {
  FakeMainLoop main;
  ConnectCommandQueue queue(main.Poster());
  Log worker;
  Gate gate;
  int settled = 0;
  queue.SetSettledHandler([&] { ++settled; });
  // the sign-out's own disconnect, held inside the sdk
  queue.Enqueue(ConnectCommandQueue::Kind::Disconnect, [&] {
    gate.NoteEntered();
    gate.Wait();
    worker.Add("disconnect");
    return ConnectCommandQueue::Apply();
  });
  // a connect tapped while it is held
  queue.Enqueue(ConnectCommandQueue::Kind::Connect, [&] {
    worker.Add("queued connect");
    return ConnectCommandQueue::Apply();
  });
  UR_EXPECT_TRUE(gate.WaitEntered());
  queue.BeginSignOut();
  UR_EXPECT_TRUE(queue.SigningOut());
  // from here no connect is admitted, and a disconnect still is
  UR_EXPECT_FALSE(queue.Enqueue(ConnectCommandQueue::Kind::Connect, [&] {
    worker.Add("late connect");
    return ConnectCommandQueue::Apply();
  }));
  UR_EXPECT_TRUE(queue.Enqueue(ConnectCommandQueue::Kind::Disconnect, [&] {
    worker.Add("late disconnect");
    return ConnectCommandQueue::Apply();
  }));
  gate.Open();
  UR_EXPECT_TRUE(main.WaitPosted(3));
  main.RunPosted();
  // the queued connect never reached the sdk; both disconnects did
  UR_EXPECT_TRUE(Joined(worker.Entries()) == "disconnect,late disconnect");
  // and the skipped connect still counted out, so the queue settles
  UR_EXPECT_FALSE(queue.Busy());
  UR_EXPECT_EQ(1, settled);
}

UR_TEST(connectCommandQueueAdmitsTheNextSignInsConnects) {
  FakeMainLoop main;
  ConnectCommandQueue queue(main.Poster());
  Log worker;
  queue.BeginSignOut();
  queue.EndSignOut();
  UR_EXPECT_FALSE(queue.SigningOut());
  // the fence ended with the sign-out's device: the next account connects
  UR_EXPECT_TRUE(queue.Enqueue(ConnectCommandQueue::Kind::Connect, [&] {
    worker.Add("next account connect");
    return ConnectCommandQueue::Apply();
  }));
  UR_EXPECT_TRUE(main.WaitPosted(1));
  main.RunPosted();
  UR_EXPECT_TRUE(Joined(worker.Entries()) == "next account connect");
}

UR_TEST(connectCommandQueueDrainsTheQueuedCommandsBeforeTheBarrier) {
  FakeMainLoop main;
  ConnectCommandQueue queue(main.Poster());
  Gate gate;
  std::vector<std::string> applied;
  queue.Enqueue(ConnectCommandQueue::Kind::Disconnect, [&] {
    gate.NoteEntered();
    gate.Wait();
    return ConnectCommandQueue::Apply([&applied] { applied.push_back("disconnect"); });
  });
  bool drained = false;
  queue.WhenDrained([&] {
    drained = true;
    applied.push_back("barrier");
  });
  UR_EXPECT_TRUE(gate.WaitEntered());
  main.RunPosted();
  // the disconnect is still held: the barrier waits for it
  UR_EXPECT_FALSE(drained);
  gate.Open();
  UR_EXPECT_TRUE(main.WaitPosted(2));
  main.RunPosted();
  UR_EXPECT_TRUE(drained);
  UR_EXPECT_TRUE(Joined(applied) == "disconnect,barrier");
}

UR_TEST(connectCommandQueueRefusesWorkOnceStopped) {
  FakeMainLoop main;
  ConnectCommandQueue queue(main.Poster());
  queue.Stop();
  UR_EXPECT_FALSE(queue.Enqueue(ConnectCommandQueue::Kind::Disconnect,
                                [] { return ConnectCommandQueue::Apply(); }));
  // a barrier on a stopped queue has nothing left to wait for
  bool drained = false;
  queue.WhenDrained([&] { drained = true; });
  UR_EXPECT_TRUE(drained);
  UR_EXPECT_FALSE(queue.Busy());
}
