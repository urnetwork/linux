// How the app ends (QuitSequence.hpp): the queued connect commands first,
// bounded, then a Quit's teardown off the main loop, and an answer at the
// latest at the cap. The sequence is pure and driven by hand here; the caps
// are events the test fires, never waits. The call sites are read from the
// sources: before, a Quit ran Shutdown on the main loop and ended at once, and
// a SIGTERM (a logout or a shutdown) killed the app with a tapped disconnect
// still queued.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>

#include "QuitSequence.hpp"

namespace {

using urnw::QuitSequence;
namespace wiring = urnw::testing::wiring;

}  // namespace

UR_TEST(quitSequenceDrainsThenTearsDownThenAnswers) {
  QuitSequence quit;
  UR_EXPECT_TRUE(quit.Begin(/*teardown=*/true));
  // the queued disconnect reaches the daemon first
  const QuitSequence::Step drained = quit.OnDrained();
  UR_EXPECT_TRUE(drained.startTeardown);
  UR_EXPECT_FALSE(drained.answer);
  // the caps after that start nothing more
  UR_EXPECT_FALSE(quit.OnDrainCap().startTeardown);
  const QuitSequence::Step done = quit.OnTeardownDone();
  UR_EXPECT_TRUE(done.answer);
  UR_EXPECT_FALSE(quit.OnAnswerCap().answer);
}

UR_TEST(quitSequenceTearsDownAtTheDrainCapWithACommandHeld) {
  QuitSequence quit;
  quit.Begin(/*teardown=*/true);
  // a command the daemon does not answer: the teardown goes ahead anyway
  const QuitSequence::Step cap = quit.OnDrainCap();
  UR_EXPECT_TRUE(cap.startTeardown);
  UR_EXPECT_FALSE(cap.answer);
  // and the command returning later starts it no second time
  UR_EXPECT_FALSE(quit.OnDrained().startTeardown);
}

UR_TEST(quitSequenceAnswersAtTheCapWithTheTeardownHeld) {
  QuitSequence quit;
  quit.Begin(/*teardown=*/true);
  UR_EXPECT_TRUE(quit.OnDrained().startTeardown);
  // the teardown waits on a held command: the app ends at the cap, once
  UR_EXPECT_TRUE(quit.OnAnswerCap().answer);
  UR_EXPECT_FALSE(quit.OnTeardownDone().answer);
}

UR_TEST(quitSequenceSessionEndOnlyDrains) {
  QuitSequence quit;
  UR_EXPECT_TRUE(quit.Begin(/*teardown=*/false));
  const QuitSequence::Step drained = quit.OnDrained();
  // a logout leaves the daemon as the queued commands left it
  UR_EXPECT_FALSE(drained.startTeardown);
  UR_EXPECT_TRUE(drained.answer);
  UR_EXPECT_FALSE(quit.OnAnswerCap().answer);
}

UR_TEST(quitSequenceBeginsOnce) {
  QuitSequence quit;
  // nothing happens before an ending begins
  UR_EXPECT_FALSE(quit.OnDrained().answer);
  UR_EXPECT_TRUE(quit.Begin(/*teardown=*/false));
  // a Quit during a session ending, or a second signal, is the same ending
  UR_EXPECT_FALSE(quit.Begin(/*teardown=*/true));
  UR_EXPECT_FALSE(quit.OnDrained().startTeardown);
}

UR_TEST(quitSequenceIsWhatEveryEndingRuns) {
  const std::string app = wiring::ReadCode("main.cpp");
  // the tray's Quit and the session's signals both end through the sequence
  UR_EXPECT_TRUE(wiring::Before(app, "tray->on_quit = [&] { endApp(", "true); };"));
  UR_EXPECT_TRUE(wiring::Before(app, "std::function<void()> onSessionEnd = [&] { endApp(", "false); };"));
  UR_EXPECT_TRUE(wiring::Contains(app, "for (const int signum : {SIGTERM, SIGINT, SIGHUP}) {"));
  UR_EXPECT_TRUE(wiring::Contains(app, "&onSessionEnd);"));
  // which refuses connects first (a tray Connect during the ending would start
  // a tunnel behind the teardown), then waits for the queued commands,
  // bounded, and answers at the cap
  UR_EXPECT_TRUE(wiring::Before(app, "host->RefuseConnects();", "host->WhenCommandsDrained("));
  UR_EXPECT_TRUE(wiring::Contains(app, "host->WhenCommandsDrained([&] { applyQuitStep(quit.OnDrained()); });"));
  UR_EXPECT_TRUE(wiring::Contains(app, "urnw::QuitSequence::kDrainMillis);"));
  UR_EXPECT_TRUE(wiring::Contains(app, "urnw::QuitSequence::kAnswerMillis);"));
  // and tears down off the main loop: no Shutdown on it
  UR_EXPECT_TRUE(wiring::Contains(app, "host->ShutdownOffMainLoop("));
  UR_EXPECT_FALSE(wiring::Contains(app, "host->Shutdown()"));
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string off =
      wiring::FunctionBody(host, "void SdkHost::ShutdownOffMainLoop(std::function<void()> done) {");
  UR_EXPECT_TRUE(wiring::Before(off, "shutdownWorker_ = std::thread(", "Shutdown();"));
  UR_EXPECT_TRUE(wiring::Before(wiring::FunctionBody(host, "SdkHost::~SdkHost() {"),
                                "shutdownWorker_.join();", "commands_.Stop();"));
}
