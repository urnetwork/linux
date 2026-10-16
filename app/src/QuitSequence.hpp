// How the app ends (apple 57a39432, 2bffa3c8). The connect commands run on
// SdkHost's queue, so a Disconnect tapped just before a Quit, a logout or a
// shutdown can still be queued, or inside its rpc, when the app ends. Ended
// at once, it never reaches the daemon, and the tunnel the user stopped stays
// up for the next sign-in to attach to.
//
// So an ending first lets the queued commands run, for at most kDrainMillis,
// then the teardown starts, off the main loop (SdkHost::ShutdownOffMainLoop:
// it takes the lock a held command keeps through its rpc, for up to the 60 s
// rpc timeout). The main loop answers, and the app ends, once the teardown is
// done or kAnswerMillis after the ending began, whichever is first, so no
// ending waits longer than that and none blocks the main loop. A command still
// held at kDrainMillis is lost, as before.
//
// A Quit tears down (Shutdown: the tunnel and the provider stop, auth stays).
// A session ending (SIGTERM at a logout or shutdown, SIGINT, SIGHUP) only lets
// the queued commands run, and leaves the daemon as they left it, as it did
// before (the app was simply killed then).
//
// Pure and main loop owned (tests/QuitSequenceTest.cpp); main.cpp drives it.
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

class QuitSequence {
 public:
  static constexpr int kDrainMillis = 5000;
  static constexpr int kAnswerMillis = 10000;

  // What the caller does next.
  struct Step {
    bool startTeardown = false;  // start the teardown off the main loop
    bool answer = false;         // end the app now
  };

  // The ending began. `teardown`: a Quit (Shutdown), not a session ending.
  // False when one has already begun (a second Quit, a signal during a Quit).
  bool Begin(bool teardown) {
    if (begun_) return false;
    begun_ = true;
    teardown_ = teardown;
    return true;
  }
  bool Begun() const { return begun_; }

  // Every command queued before the ending has run.
  Step OnDrained() { return Advance(); }
  // kDrainMillis passed: whatever the commands do, the teardown goes ahead.
  Step OnDrainCap() { return Advance(); }
  // The teardown finished.
  Step OnTeardownDone() { return Answer(); }
  // kAnswerMillis passed: the app ends whatever is still running.
  Step OnAnswerCap() { return Answer(); }

 private:
  Step Advance() {
    Step step;
    if (!begun_ || drained_ || answered_) return step;
    drained_ = true;
    if (!teardown_) return Answer();
    step.startTeardown = true;
    return step;
  }
  Step Answer() {
    Step step;
    if (!begun_ || answered_) return step;
    answered_ = true;
    step.answer = true;
    return step;
  }

  bool begun_ = false;
  bool teardown_ = false;
  bool drained_ = false;
  bool answered_ = false;
};

}  // namespace urnw
