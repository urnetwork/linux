// The connect status row's decision table, pinned.
//
// This defect was reported FOUR times and survived three fixes, every one of
// which added a condition to ApplyConnectStatus and none of which was
// exercised by anything. The table below is the review that did not happen:
// every row states an input combination and the exact four channels it must
// produce, so a change that moves one channel without the others fails here
// rather than on the owner's screen.
//
// SPDX-License-Identifier: MPL-2.0
#include "Health.hpp"
#include "TestHarness.hpp"

#include <string>

namespace {

using namespace urnw::health;

// A session that is up: a destination is selected AND the daemon's tunnel is
// still ours. Everything the row can say beyond "Disconnected" needs this.
Signals Session(SdkStatus sdk, int64_t providerCount = 0) {
  Signals s;
  s.sdk = sdk;
  s.destinationSelected = true;
  s.tunnelBound = true;
  s.providerCount = providerCount;
  return s;
}

// Assert all four channels of one row at once. Taking them together is the
// point: three previous fixes moved the headline without the button, or the
// button without the headline.
void ExpectRow(const char* what, const Signals& in, State state, const char* textEnglish, Dot dot,
               Hero hero, Action action) {
  const Reading r = Render(in);
  if (r.state != state) UR_FAIL(std::string(what) + ": wrong state");
  if (std::string(r.textEnglish) != textEnglish) {
    UR_FAIL(std::string(what) + ": headline is \"" + r.textEnglish + "\", expected \"" +
            textEnglish + "\"");
  }
  if (r.dot != dot) UR_FAIL(std::string(what) + ": wrong dot");
  if (r.hero != hero) UR_FAIL(std::string(what) + ": wrong hero pose");
  if (r.action != action) UR_FAIL(std::string(what) + ": wrong button action");
}

}  // namespace

// ---- the vocabulary --------------------------------------------------------

UR_TEST(sdkStatusParsesTheControllersOwnAlphabet) {
  UR_EXPECT_TRUE(ParseSdkStatus("CONNECTED") == SdkStatus::Connected);
  UR_EXPECT_TRUE(ParseSdkStatus("connected") == SdkStatus::Connected);
  UR_EXPECT_TRUE(ParseSdkStatus("CONNECTING") == SdkStatus::Connecting);
  UR_EXPECT_TRUE(ParseSdkStatus("DESTINATION_SET") == SdkStatus::DestinationSet);
  UR_EXPECT_TRUE(ParseSdkStatus("CONNECT_FAILED") == SdkStatus::Failed);
  UR_EXPECT_TRUE(ParseSdkStatus("DISCONNECTED") == SdkStatus::Disconnected);
  // UNKNOWN IS NOT DISCONNECTED. An empty or unrecognised token is the absence
  // of evidence, and reading it as "not connected" is how a carrying tunnel
  // gets a "Disconnected" headline the instant a controller is reopened.
  UR_EXPECT_TRUE(ParseSdkStatus("") == SdkStatus::Unknown);
  UR_EXPECT_TRUE(ParseSdkStatus("WAT") == SdkStatus::Unknown);
}

// ---- THE STATE TABLE -------------------------------------------------------

UR_TEST(stateTableConnected) {
  // The requirement, verbatim: "Connected" appears whenever the tunnel is up
  // AND the SDK has provider sessions.
  ExpectRow("carrying", Session(SdkStatus::Connected, 11), State::Connected, "Connected",
            Dot::Green, Hero::Connected, Action::Disconnect);
  // and the provider window's size is irrelevant once the controller says so
  ExpectRow("carrying, silent grid", Session(SdkStatus::Connected, 0), State::Connected,
            "Connected", Dot::Green, Hero::Connected, Action::Disconnect);
}

UR_TEST(stateTableConnectingAndEvaluating) {
  // "Connecting to providers" ONLY before that, and only over a live session.
  ExpectRow("dialling", Session(SdkStatus::Connecting, 0), State::Connecting,
            "Connecting to providers", Dot::Connecting, Hero::Connecting, Action::Disconnect);
  // providers in the window but none proven: the Windows-parity middle row
  ExpectRow("evaluating", Session(SdkStatus::Connecting, 4), State::Evaluating,
            "Finding providers…", Dot::Connecting, Hero::Connecting, Action::Disconnect);
  // a controller that has not reported yet is still "coming up", not connected
  ExpectRow("no status yet", Session(SdkStatus::Unknown, 0), State::Connecting,
            "Connecting to providers", Dot::Connecting, Hero::Connecting, Action::Disconnect);
}

UR_TEST(stateTableFailedAndDisconnected) {
  // the failed state's one action is Retry: stop, then connect again
  ExpectRow("window failed", Session(SdkStatus::Failed, 0), State::Failed, "Couldn't connect",
            Dot::Coral, Hero::Error, Action::Retry);
  ExpectRow("idle", Signals{}, State::Disconnected, "Disconnected", Dot::Idle, Hero::Disconnected,
            Action::Connect);
  UR_EXPECT_TRUE(Render(Signals{}).showNotProtected);
  UR_EXPECT_TRUE(!Render(Session(SdkStatus::Connected)).showNotProtected);
}

UR_TEST(stateTableDisconnectingAndBalance) {
  // "Disconnecting…" ONLY while a teardown the user asked for is in flight —
  // and it outranks the balance override, because an insufficient balance is
  // not the answer to "what is happening right now".
  Signals tearingDown = Session(SdkStatus::Connected, 11);
  tearingDown.disconnectRequested = true;
  tearingDown.insufficientBalance = true;
  ExpectRow("teardown in flight", tearingDown, State::Disconnecting, "Disconnecting…", Dot::Amber,
            Hero::Processing, Action::Disconnect);

  Signals blocked = Session(SdkStatus::Connected, 11);
  blocked.insufficientBalance = true;
  // The balance row replaces the headline but NEVER the action: an
  // out-of-balance session still has to be disconnectable.
  ExpectRow("out of balance", blocked, State::Blocked,
            "Insufficient balance — add balance or a plan", Dot::Coral, Hero::Error,
            Action::Disconnect);

  // ... and with no session it keeps the action the session warrants, which is
  // Connect. The headline is the thing the user has to act on either way.
  Signals blockedIdle;
  blockedIdle.insufficientBalance = true;
  ExpectRow("out of balance, no session", blockedIdle, State::Blocked,
            "Insufficient balance — add balance or a plan", Dot::Coral, Hero::Error,
            Action::Connect);
}

// A paying account is not told to add balance. Its refusal reads as the data limit,
// the same row in words for the limit, with its Disconnect. A refusal from before
// the purchase that has not cleared says nothing, and the connection reading stands.
UR_TEST(aPayingAccountsRefusalReadsAsItsDataLimit) {
  Signals refused = Session(SdkStatus::Connected, 11);
  refused.insufficientBalance = true;
  ExpectRow("at the limit", WithPlan(refused, /*pro=*/true, /*dataLimit=*/true), State::Blocked,
            "Data limit reached", Dot::Coral, Hero::Error, Action::Disconnect);
  ExpectRow("the refusal from before the purchase", WithPlan(refused, /*pro=*/true, /*dataLimit=*/false),
            State::Connected, "Connected", Dot::Green, Hero::Connected, Action::Disconnect);
  ExpectRow("not Pro", WithPlan(refused, /*pro=*/false, /*dataLimit=*/false), State::Blocked,
            "Insufficient balance — add balance or a plan", Dot::Coral, Hero::Error,
            Action::Disconnect);
  Signals fine = Session(SdkStatus::Connected, 11);
  ExpectRow("no refusal, Pro", WithPlan(fine, /*pro=*/true, /*dataLimit=*/false), State::Connected,
            "Connected", Dot::Green, Hero::Connected, Action::Disconnect);
}

// ---- THE REPORTED DEFECT ---------------------------------------------------

UR_TEST(destinationSetOverACarryingTunnelIsNotConnecting) {
  // THE BUG, four reports and three failed fixes.
  //
  // On this platform DESTINATION_SET is a STEADY-STATE token: it means "a
  // destination is selected", it is true for the entire life of a carrying
  // session, and the feed that produced it could emit nothing else — so the
  // row read "Connecting to providers" with a yellow dot over a tunnel moving
  // megabytes, beside a button reading "Disconnect".
  //
  // A token can no longer decide this on its own. What settles it is whether
  // the controller has said CONNECTED.
  Signals carrying = Session(SdkStatus::Connected, 11);
  carrying.providerCount = 11;
  const Reading connected = Render(carrying);
  UR_EXPECT_TRUE(connected.state == State::Connected);
  UR_EXPECT_TRUE(std::string(connected.textEnglish) == "Connected");
  UR_EXPECT_TRUE(connected.dot == Dot::Green);

  // The same session before the providers attached: still a live session, so
  // still a Disconnect button, but honestly yellow.
  Signals comingUp = Session(SdkStatus::DestinationSet, 0);
  const Reading dialling = Render(comingUp);
  UR_EXPECT_TRUE(dialling.state == State::Connecting);
  UR_EXPECT_TRUE(dialling.action == Action::Disconnect);
}

UR_TEST(aStaleTokenWithNoSessionCannotClaimAnythingIsHappening) {
  // The other half of the same defect: the row must not read "Connecting to
  // providers" — or offer Disconnect — off a token left over from a session
  // that is gone. Every one of these has a status that used to be enough on
  // its own to render Connecting.
  for (const SdkStatus sdk : {SdkStatus::Connecting, SdkStatus::DestinationSet,
                              SdkStatus::Connected, SdkStatus::Failed}) {
    Signals noTunnel;
    noTunnel.sdk = sdk;
    noTunnel.destinationSelected = true;  // the token's own precondition
    noTunnel.tunnelBound = false;         // ... but the daemon's tunnel is gone
    const Reading r = Render(noTunnel);
    UR_EXPECT_TRUE(r.state == State::Disconnected);
    UR_EXPECT_TRUE(r.action == Action::Connect);
    UR_EXPECT_TRUE(r.showNotProtected);

    Signals noDestination;
    noDestination.sdk = sdk;
    noDestination.destinationSelected = false;
    noDestination.tunnelBound = true;
    UR_EXPECT_TRUE(Render(noDestination).state == State::Disconnected);
    UR_EXPECT_TRUE(Render(noDestination).action == Action::Connect);
  }
}

UR_TEST(theButtonAndTheHeadlineComeFromTheSameReading) {
  // THE INVARIANT THE FOUR REPORTS WERE ABOUT. Over every representable
  // combination of the reading's inputs, the button offers Disconnect exactly
  // when the row describes something to disconnect from — never a "Disconnect"
  // beside "Disconnected", never a "Connect" beside "Connected".
  for (int sdkI = 0; sdkI <= static_cast<int>(SdkStatus::Failed); ++sdkI) {
    for (int destination = 0; destination < 2; ++destination) {
      for (int bound = 0; bound < 2; ++bound) {
        for (int providers = 0; providers < 3; providers += 2) {
          for (int balance = 0; balance < 2; ++balance) {
            for (int intent = 0; intent < 2; ++intent) {
              Signals s;
              s.sdk = static_cast<SdkStatus>(sdkI);
              s.destinationSelected = destination != 0;
              s.tunnelBound = bound != 0;
              s.providerCount = providers;
              s.insufficientBalance = balance != 0;
              s.disconnectRequested = intent != 0;
              const Reading r = Render(s);
              // Retry stops the failed session before it connects again
              const bool offersDisconnect = r.action != Action::Connect;
              const bool somethingToStop = SessionUp(s) || s.disconnectRequested;
              if ((r.action == Action::Retry) != (r.state == State::Failed)) {
                UR_FAIL("Retry is offered outside the failed state, or not in it");
              }
              if (offersDisconnect != somethingToStop) {
                UR_FAIL("button action disagrees with the session reading");
              }
              // The settled-idle row is the ONLY one that may claim the user's
              // traffic is unprotected, and it is the only one that offers
              // Connect.
              if (r.showNotProtected != (r.state == State::Disconnected)) {
                UR_FAIL("\"not protected\" appeared on a non-idle row");
              }
              // A row that says "Disconnected" must offer Connect. (Not the
              // converse: the out-of-balance row replaces the headline over a
              // session that may or may not exist, and it keeps whichever
              // action the session warrants.)
              if (r.state == State::Disconnected && offersDisconnect) {
                UR_FAIL("a Disconnected row offered Disconnect");
              }
              // A green dot and the Connected hero are reachable from exactly
              // one state, so no other row can look connected.
              if ((r.dot == Dot::Green) != (r.state == State::Connected)) {
                UR_FAIL("a green dot on a row that is not Connected");
              }
              if ((r.hero == Hero::Connected) != (r.state == State::Connected)) {
                UR_FAIL("the Connected hero pose on a row that is not Connected");
              }
              // "Connected" requires BOTH halves of the requirement.
              if (r.state == State::Connected &&
                  !(SessionUp(s) && s.sdk == SdkStatus::Connected)) {
                UR_FAIL("\"Connected\" without a live session AND provider sessions");
              }
            }
          }
        }
      }
    }
  }
}

// ---- the degrade hold (Windows' Tracker, over CONNECTED) -------------------

UR_TEST(degradeHoldKeepsABlipConnectedAndCallsALossDegraded) {
  DegradeHold hold;
  UR_EXPECT_TRUE(hold.Update(true, false, 0) == ProofLoss::None);  // never proven: nothing held
  UR_EXPECT_TRUE(hold.Update(true, true, 1000) == ProofLoss::None);
  // the controller stops saying CONNECTED: held for the hold, then lost
  UR_EXPECT_TRUE(hold.Update(true, false, 2000) == ProofLoss::Held);
  UR_EXPECT_EQ(2000 + DegradeHold::kDegradeHoldMillis, hold.ReevalAtMillis());
  UR_EXPECT_TRUE(hold.Update(true, false, 2000 + DegradeHold::kDegradeHoldMillis - 1) ==
                 ProofLoss::Held);
  UR_EXPECT_TRUE(hold.Update(true, false, 2000 + DegradeHold::kDegradeHoldMillis) ==
                 ProofLoss::Lost);
  UR_EXPECT_EQ(0, hold.ReevalAtMillis());
  // one CONNECTED recovers at once, and the next loss starts a fresh hold
  UR_EXPECT_TRUE(hold.Update(true, true, 20000) == ProofLoss::None);
  UR_EXPECT_TRUE(hold.Update(true, false, 21000) == ProofLoss::Held);
  UR_EXPECT_EQ(21000 + DegradeHold::kDegradeHoldMillis, hold.ReevalAtMillis());
}

UR_TEST(degradeHoldStartsOverWithTheSessionOrANewAttempt) {
  DegradeHold hold;
  hold.Update(true, true, 0);
  // a session that goes down takes its proof with it
  UR_EXPECT_TRUE(hold.Update(false, false, 1000) == ProofLoss::None);
  UR_EXPECT_TRUE(hold.Update(true, false, 2000) == ProofLoss::None);
  // and so does a deliberate connect: a new location's window is not a loss
  hold.Update(true, true, 3000);
  hold.NoteNewAttempt();
  UR_EXPECT_TRUE(hold.Update(true, false, 4000) == ProofLoss::None);
  UR_EXPECT_TRUE(hold.Update(true, false, 4000 + 10 * DegradeHold::kDegradeHoldMillis) ==
                 ProofLoss::None);
  UR_EXPECT_EQ(0, hold.ReevalAtMillis());
}

UR_TEST(stateTableDegraded) {
  Signals held = Session(SdkStatus::Connecting, 4);
  held.proofLoss = ProofLoss::Held;
  ExpectRow("a blip in the hold", held, State::Connected, "Connected", Dot::Green,
            Hero::Connected, Action::Disconnect);
  Signals lost = Session(SdkStatus::Connecting, 4);
  lost.proofLoss = ProofLoss::Lost;
  ExpectRow("a loss past the hold", lost, State::Degraded, "Connection degraded — reconnecting",
            Dot::Coral, Hero::Connecting, Action::Disconnect);
  UR_EXPECT_TRUE(std::string(Render(lost).textKey) == "conn_degraded");
  // a session that was working and settled on failure since is degraded
  lost.sdk = SdkStatus::Failed;
  ExpectRow("lost, then failed", lost, State::Degraded, "Connection degraded — reconnecting",
            Dot::Coral, Hero::Connecting, Action::Disconnect);
  // the user's Disconnect and a session that is down still outrank it
  lost.disconnectRequested = true;
  UR_EXPECT_TRUE(Render(lost).state == State::Disconnecting);
  Signals down = lost;
  down.disconnectRequested = false;
  down.tunnelBound = false;
  UR_EXPECT_TRUE(Render(down).state == State::Disconnected);
}

UR_TEST(trafficIsHeldOnlyWhileNothingProvenCarriesASession) {
  Signals evaluating = Session(SdkStatus::Connecting, 4);
  UR_EXPECT_TRUE(TrafficHeld(Render(evaluating), evaluating));
  Signals failed = Session(SdkStatus::Failed, 4);
  UR_EXPECT_TRUE(TrafficHeld(Render(failed), failed));
  Signals lost = Session(SdkStatus::Connecting, 4);
  lost.proofLoss = ProofLoss::Lost;
  UR_EXPECT_TRUE(TrafficHeld(Render(lost), lost));
  for (const Signals& s : {Session(SdkStatus::Connected, 4), Session(SdkStatus::Connecting, 0),
                           Signals{}}) {
    UR_EXPECT_FALSE(TrafficHeld(Render(s), s));
  }
  // the held line claims "blocked, not exposed" only with the floor in force
  UR_EXPECT_TRUE(std::string(HeldLineFor(true).key) == "conn_traffic_blocked");
  UR_EXPECT_TRUE(std::string(HeldLineFor(false).key) == "conn_traffic_blocked_unprotected");
}

// The tray's connected icon means proven: a held blip still is, and nothing
// that is building, held, degraded, failed, blocked or going down is.
UR_TEST(onlyAProvenConnectionIsProven) {
  Signals held = Session(SdkStatus::Connecting, 4);
  held.proofLoss = ProofLoss::Held;
  UR_EXPECT_TRUE(Proven(Render(Session(SdkStatus::Connected, 4))));
  UR_EXPECT_TRUE(Proven(Render(held)));
  Signals lost = held;
  lost.proofLoss = ProofLoss::Lost;
  Signals blocked = Session(SdkStatus::Connected, 4);
  blocked.insufficientBalance = true;
  Signals leaving = Session(SdkStatus::Connected, 4);
  leaving.disconnectRequested = true;
  for (const Signals& s : {Session(SdkStatus::Connecting, 0), Session(SdkStatus::Connecting, 4),
                           Session(SdkStatus::Failed, 4), lost, blocked, leaving, Signals{}}) {
    UR_EXPECT_FALSE(Proven(Render(s)));
  }
}

// The tray keeps a session's own claim while no status has been observed for
// it (started or still unproven with the window hidden), icon and words, and
// never upgrades one that was observed.
UR_TEST(theTrayKeepsTheSessionsClaimWithNoStatusObserved) {
  const Signals hidden = Session(SdkStatus::Unknown, 0);
  const Reading claimed = TrayReading(Render(hidden), hidden, /*statusObserved=*/false);
  UR_EXPECT_TRUE(Proven(claimed));
  UR_EXPECT_TRUE(std::string(claimed.textKey) == "connected");
  UR_EXPECT_TRUE(std::string(claimed.textEnglish) == "Connected");
  // an observed Connecting, or one building in the window, is not proven
  const Signals building = Session(SdkStatus::Connecting, 0);
  const Reading observed = TrayReading(Render(building), building, /*statusObserved=*/true);
  UR_EXPECT_FALSE(Proven(observed));
  UR_EXPECT_TRUE(std::string(observed.textEnglish) == "Connecting to providers");
  UR_EXPECT_FALSE(Proven(TrayReading(Render(hidden), hidden, /*statusObserved=*/true)));
  // no session, a teardown and a block read as the window's, evidence or not
  Signals leaving = hidden;
  leaving.disconnectRequested = true;
  Signals blocked = hidden;
  blocked.insufficientBalance = true;
  for (const Signals& s : {Signals{}, leaving, blocked}) {
    const Reading tray = TrayReading(Render(s), s, /*statusObserved=*/false);
    UR_EXPECT_FALSE(Proven(tray));
    UR_EXPECT_TRUE(tray.state == Render(s).state);
  }
  // an observed proof stays proven
  const Signals carrying = Session(SdkStatus::Connected, 4);
  UR_EXPECT_TRUE(Proven(TrayReading(Render(carrying), carrying, /*statusObserved=*/true)));
}

// ---- the reason line (the SDK's window diagnosis) --------------------------

UR_TEST(theReasonLineNamesTheStallWhileTheAttemptIsNotThere) {
  struct Row {
    const char* reason;
    const char* key;
  };
  const Row rows[] = {{"platform-unreachable", "conn_reason_platform"},
                      {"providers-unresponsive", "conn_reason_providers"},
                      {"rate-limited", "conn_reason_rate_limited"},
                      {"auth-failing", "conn_reason_auth"}};
  for (const State state :
       {State::Connecting, State::Evaluating, State::Degraded, State::Failed}) {
    for (const Row& row : rows) {
      const ReasonLine line = ReasonLineFor(state, row.reason);
      if (!line.key || std::string(line.key) != row.key) UR_FAIL(std::string(row.reason));
    }
    // evaluating, an unknown token and an SDK without the field say nothing
    // the headline does not
    for (const char* quiet : {"evaluating", "", "something-new"}) {
      const ReasonLine line = ReasonLineFor(state, quiet);
      const bool expectDetail = state == State::Failed;
      if ((line.key != nullptr) != expectDetail) UR_FAIL(std::string("quiet: ") + quiet);
    }
  }
  // a failure with no reason says what Retry does
  UR_EXPECT_TRUE(std::string(ReasonLineFor(State::Failed, "").key) == "conn_failed_detail");
  // and nothing is said over a connection, an idle page, a teardown or a block
  for (const State state :
       {State::Connected, State::Disconnected, State::Disconnecting, State::Blocked}) {
    UR_EXPECT_TRUE(ReasonLineFor(state, "providers-unresponsive").key == nullptr);
  }
}
