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
  ExpectRow("window failed", Session(SdkStatus::Failed, 0), State::Failed, "Couldn't connect",
            Dot::Coral, Hero::Error, Action::Disconnect);
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
              const bool offersDisconnect = r.action == Action::Disconnect;
              const bool somethingToStop = SessionUp(s) || s.disconnectRequested;
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
