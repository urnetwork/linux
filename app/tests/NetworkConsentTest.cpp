// The network payout consent on the Earnings page (NetworkConsent.hpp,
// CONSENT.md §4, A1, A2, and the apps' shared steps): every state of the
// section (no session, loading, unreadable, no consent, a consent in force or
// from the next epoch, a signing in flight), the flow's steps in their order
// through a fake SDK and a fake wallet (the typed coldkey checked first, the
// challenge, the signature, the submit, the status read again), that nothing
// but the server's message is ever signed or submitted, and each refusal: an
// invalid coldkey, a proof for another message or coldkey, an expired
// challenge, a signature that does not match, a session that is not the
// network owner's, a wallet step the user closed or another wallet flow took
// over, and every other error. The page, the host and the wallet bridge need
// GTK and the SDK, so their wiring is read from the sources and the strings
// from po/en.po.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "BittensorWalletFlow.hpp"
#include "NetworkConsent.hpp"
#include "WiringSource.hpp"

namespace {

namespace consent = urnw::consent;
using consent::Action;
using consent::Challenge;
using consent::Flow;
using consent::Line;
using consent::Notice;
using consent::Phase;
using consent::Signed;
using consent::Status;
using consent::Submitted;
using consent::View;
using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;
using urnw::testing::wiring::ReadRaw;

constexpr const char* kColdkey = "5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY";
constexpr const char* kOtherColdkey = "5FHneW46xGXgs5mUiveU4sbTyGBzmstUspZC92UhjJM694ty";
constexpr const char* kSignature =
    "0x2aeaa98e26062cf65161c68c5cb7aa31ca050cb5bdd07abc80a475d2a2eebc7b"
    "7a9c9546fbdff971b29419ddd9982bf4148c81a49df550154e1674a6b58bac84";
constexpr int64_t kCurrentEpoch = 120;

// A consent message as the server issues it: the prefix line, then canonical
// JSON (quotes, braces), signed byte for byte.
std::string ConsentMessage(const std::string& coldkey) {
  return "Approve URnetwork network wallet mapping\n"
         "{\"schema\":\"urnetwork-network-wallet-mapping-consent-v1\",\"scope\":\"network\","
         "\"domain\":{\"chain_id\":964,\"netuid\":25,\"no_id\":1},\"coldkey\":\"" +
         coldkey +
         "\",\"generation\":2,\"from_epoch\":121,\"through_epoch\":65656,\"expires_at\":"
         "1760367600}";
}

Challenge IssuedChallenge(const std::string& coldkey = kColdkey) {
  Challenge challenge;
  challenge.message = ConsentMessage(coldkey);
  challenge.coldkeySs58 = coldkey;
  challenge.fromEpoch = kCurrentEpoch + 1;
  challenge.throughEpoch = kCurrentEpoch + 1 + 65535;
  challenge.expiresAt = 1760367600;
  return challenge;
}

// the session's proof for `challenge`
Signed ProofFor(const Challenge& challenge) {
  return Signed{true, challenge.coldkeySs58, challenge.message, kSignature, {}};
}

Status NoConsent() { return Status{}; }

// a consent in force since an earlier epoch
Status InForce(const std::string& coldkey = kOtherColdkey) {
  Status status;
  status.active = true;
  status.coldkeySs58 = coldkey;
  status.fromEpoch = kCurrentEpoch - 4;
  status.throughEpoch = kCurrentEpoch - 4 + 65535;
  return status;
}

// a consent that starts next epoch: every new consent is prospective (A2.1)
Status FromNextEpoch(const std::string& coldkey = kColdkey) {
  Status status;
  status.active = true;
  status.coldkeySs58 = coldkey;
  status.fromEpoch = kCurrentEpoch + 1;
  status.throughEpoch = kCurrentEpoch + 1 + 65535;
  return status;
}

// The fake SDK and the fake wallet: every port records what it was asked and
// holds its answer until the test gives it, so each state in between can be
// looked at. Only kColdkey and kOtherColdkey pass the ss58 check.
struct Fake {
  struct SubmitCall {
    std::string coldkey;
    std::string message;
    std::string signature;
  };
  std::set<std::string> valid = {kColdkey, kOtherColdkey};
  std::vector<std::string> checked;  // what the ss58 check was asked
  std::vector<Flow::StatusDone> statusDone;
  std::vector<std::string> challengeColdkeys;
  std::vector<Flow::ChallengeDone> challengeDone;
  std::vector<std::pair<std::string, Challenge>> signRequests;
  std::vector<Flow::SignDone> signDone;
  std::vector<SubmitCall> submits;
  std::vector<Flow::SubmitDone> submitDone;
  std::vector<std::string> errors;  // the page's error surface
  int changes = 0;

  Flow::Ports Ports() {
    Flow::Ports ports;
    ports.status = [this](Flow::StatusDone done) { statusDone.push_back(std::move(done)); };
    ports.validSs58 = [this](const std::string& address) {
      checked.push_back(address);
      return valid.count(address) > 0;
    };
    ports.challenge = [this](const std::string& coldkey, Flow::ChallengeDone done) {
      challengeColdkeys.push_back(coldkey);
      challengeDone.push_back(std::move(done));
    };
    ports.sign = [this](const std::string& walletId, const Challenge& challenge,
                        Flow::SignDone done) {
      signRequests.emplace_back(walletId, challenge);
      signDone.push_back(std::move(done));
    };
    ports.submit = [this](const std::string& coldkey, const std::string& message,
                          const std::string& signature, Flow::SubmitDone done) {
      submits.push_back(SubmitCall{coldkey, message, signature});
      submitDone.push_back(std::move(done));
    };
    return ports;
  }

  void Attach(Flow& flow) {
    flow.on_changed = [this] { ++changes; };
    flow.on_error = [this](const std::string& error) { errors.push_back(error); };
  }

  // the wallet the i-th signature was asked of, or "" when it was not asked
  std::string SignedWith(size_t i) const {
    return i < signRequests.size() ? signRequests[i].first : std::string();
  }

  // the server and the wallet were asked nothing (the status reads aside)
  bool AskedNothingMore() const {
    return challengeDone.empty() && signDone.empty() && submitDone.empty();
  }
};

// Gives the i-th answer `done` holds; a port that was not asked that often is
// a failure, never an out-of-range call.
template <typename Done, typename... Args>
void Answer(std::vector<Done>& done, size_t i, Args&&... args) {
  if (i >= done.size()) {
    UR_FAIL("a port was not asked for answer " + std::to_string(i));
    return;
  }
  done[i](std::forward<Args>(args)...);
}

// The English every T_("key", ...) in `source` passes: the string literals
// after the key, joined (a long one wraps).
std::vector<std::string> FallbacksOf(const std::string& source, const std::string& key) {
  std::vector<std::string> out;
  const std::string open = "T_(\"" + key + "\",";
  for (size_t at = source.find(open); at != std::string::npos; at = source.find(open, at + 1)) {
    std::string text;
    size_t p = at + open.size();
    while (p < source.size()) {
      while (p < source.size() && (source[p] == ' ' || source[p] == '\n')) ++p;
      if (p >= source.size() || source[p] != '"') break;
      for (++p; p < source.size() && source[p] != '"'; ++p) {
        if (source[p] == '\\' && p + 1 < source.size()) ++p;
        text += source[p];
      }
      ++p;
    }
    out.push_back(text);
  }
  return out;
}

// The flow loaded with `status`, its fake attached.
void LoadWith(Flow& flow, Fake& fake, const Status& status) {
  fake.Attach(flow);
  flow.Load();
  Answer(fake.statusDone, fake.statusDone.size() - 1, status, std::string());
}

// Start with Talisman for the challenge's coldkey and walk the steps up to the
// submit's answer.
void SignUpToTheSubmit(Flow& flow, Fake& fake, const Challenge& issued = IssuedChallenge()) {
  flow.Start("talisman", issued.coldkeySs58);
  Answer(fake.challengeDone, fake.challengeDone.size() - 1, issued, std::string());
  Answer(fake.signDone, fake.signDone.size() - 1, ProofFor(issued));
}

View ExpectedNone(Notice notice = Notice::None) {
  View view;
  view.visible = true;
  view.line = Line::None;
  view.body = true;
  view.action = Action::Sign;
  view.notice = notice;
  return view;
}

View ExpectedActive(const std::string& coldkey, Notice notice = Notice::None) {
  View view;
  view.visible = true;
  view.line = Line::Active;
  view.coldkeySs58 = coldkey;
  view.action = Action::Change;
  view.changeNote = true;
  view.notice = notice;
  return view;
}

View ExpectedSigning() {
  View view;
  view.visible = true;
  view.line = Line::Signing;
  return view;
}

// ---- the states -----------------------------------------------------------------

// CONSENT §4: without a session nothing shows and nothing is asked
UR_TEST(Consent_HiddenWithoutASession) {
  Fake fake;
  Flow flow(fake.Ports());
  fake.Attach(flow);
  UR_EXPECT_TRUE(flow.Current() == View{});
  UR_EXPECT_TRUE(flow.phase() == Phase::Hidden);
  flow.Reset();
  UR_EXPECT_TRUE(flow.Current() == View{});
  UR_EXPECT_FALSE(flow.Start("talisman", kColdkey));
  UR_EXPECT_TRUE(fake.statusDone.empty() && fake.AskedNothingMore());
}

// CONSENT §4: a load reads the status and shows loading until it answers
UR_TEST(Consent_LoadShowsLoadingUntilTheStatusAnswers) {
  Fake fake;
  Flow flow(fake.Ports());
  fake.Attach(flow);
  flow.Load();
  UR_EXPECT_EQ(1u, fake.statusDone.size());
  View loading;
  loading.visible = true;
  loading.line = Line::Loading;
  UR_EXPECT_TRUE(flow.Current() == loading);
  UR_EXPECT_TRUE(fake.changes > 0);
}

// CONSENT §4, A2.3: no consent is sn_consent_none with the explanation and
// Sign with wallet, and nothing else
UR_TEST(Consent_NoConsentOffersSignWithWallet) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());
  UR_EXPECT_TRUE(flow.Current() == ExpectedNone());
  UR_EXPECT_TRUE(flow.phase() == Phase::None);
  UR_EXPECT_TRUE(fake.errors.empty());
}

// CONSENT §4: a consent in force shows its coldkey with Change wallet and the
// note, never sn_consent_none
UR_TEST(Consent_AConsentInForceShowsItsColdkeyWithChange) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, InForce());
  UR_EXPECT_TRUE(flow.Current() == ExpectedActive(kOtherColdkey));
  UR_EXPECT_FALSE(flow.Current().body);
}

// A2.3: a consent from the next epoch is active too: sn_consent_active with
// sn_consent_change_note ("takes effect from the next epoch"); its generation
// is not read (A2.2)
UR_TEST(Consent_AConsentFromTheNextEpochShowsActiveWithTheNote) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, FromNextEpoch());
  const View view = flow.Current();
  UR_EXPECT_TRUE(view == ExpectedActive(kColdkey));
  UR_EXPECT_TRUE(view.line == Line::Active && view.changeNote);
}

// CONSENT §4: a status that cannot be read is unavailable, offers nothing and
// does not shout; an active status without its coldkey cannot be read either
UR_TEST(Consent_AnUnreadableStatusIsUnavailable) {
  View unavailable;
  unavailable.visible = true;
  unavailable.line = Line::Unavailable;
  {
    Fake fake;
    Flow flow(fake.Ports());
    fake.Attach(flow);
    flow.Load();
    Answer(fake.statusDone, fake.statusDone.size() - 1, std::nullopt,
           "chain_rpc_unreachable: GET /sn/epoch timed out");
    UR_EXPECT_TRUE(flow.Current() == unavailable);
    UR_EXPECT_TRUE(fake.errors.empty());
    UR_EXPECT_FALSE(flow.Start("talisman", kColdkey));
  }
  {
    Fake fake;
    Flow flow(fake.Ports());
    Status nameless = InForce();
    nameless.coldkeySs58.clear();
    LoadWith(flow, fake, nameless);
    UR_EXPECT_TRUE(flow.Current() == unavailable);
  }
}

// A1: a status read refused for a session that is not the network owner's
// says so
UR_TEST(Consent_AStatusRefusedToANonOwnerSaysSo) {
  Fake fake;
  Flow flow(fake.Ports());
  fake.Attach(flow);
  flow.Load();
  Answer(fake.statusDone, fake.statusDone.size() - 1, std::nullopt,
         "network_owner_only: no network credential");
  UR_EXPECT_TRUE(flow.Current().line == Line::Unavailable);
  UR_EXPECT_TRUE(flow.Current().notice == Notice::OwnerOnly);
  UR_EXPECT_TRUE(fake.errors.empty());
}

// ---- the signing -------------------------------------------------------------------

// CONSENT §4, A2, the apps' steps: with the chosen wallet and the typed
// coldkey, the flow asks the challenge for that coldkey, has the wallet sign
// it, submits it and reads the status again, which shows the new coldkey from
// the next epoch; the section says sn_consent_signing, with no button, all the
// while
UR_TEST(Consent_SignChallengesSignsSubmitsAndReadsTheStatus) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());

  UR_EXPECT_TRUE(flow.Start("talisman", kColdkey));
  UR_EXPECT_TRUE(flow.Busy());
  UR_EXPECT_TRUE(flow.Current() == ExpectedSigning());
  UR_EXPECT_EQ(1u, fake.challengeColdkeys.size());
  UR_EXPECT_TRUE(!fake.challengeColdkeys.empty() && fake.challengeColdkeys[0] == kColdkey);
  UR_EXPECT_TRUE(fake.signRequests.empty());

  Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
  UR_EXPECT_EQ(1u, fake.signRequests.size());
  UR_EXPECT_TRUE(fake.SignedWith(0) == "talisman");  // the wallet chosen
  UR_EXPECT_TRUE(fake.submits.empty());
  UR_EXPECT_TRUE(flow.Current() == ExpectedSigning());

  Answer(fake.signDone, 0, ProofFor(IssuedChallenge()));
  UR_EXPECT_EQ(1u, fake.submits.size());
  UR_EXPECT_TRUE(flow.Current() == ExpectedSigning());

  Answer(fake.submitDone, 0, Submitted{true, {}});
  UR_EXPECT_FALSE(flow.Busy());
  UR_EXPECT_EQ(2u, fake.statusDone.size());  // the status is read again
  UR_EXPECT_TRUE(flow.Current().line == Line::Loading);

  Answer(fake.statusDone, 1, FromNextEpoch(kColdkey), std::string());
  UR_EXPECT_TRUE(flow.Current() == ExpectedActive(kColdkey));
  UR_EXPECT_TRUE(fake.errors.empty());
}

// The apps' steps: the typed coldkey passes the SDK's ss58 check before
// anything is asked; one that fails it, or none, starts nothing
UR_TEST(Consent_TheTypedColdkeyIsCheckedBeforeAnythingIsAsked) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());
  const int changes = fake.changes;
  for (const char* typed : {"5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQZ", "not an address",
                            "0x5a3e0000000000000000000000000000000000a2", ""}) {
    UR_EXPECT_TRUE_MSG(typed, !flow.Start("talisman", typed));
  }
  UR_EXPECT_EQ(4u, fake.checked.size());
  UR_EXPECT_TRUE(fake.AskedNothingMore());
  UR_EXPECT_TRUE(flow.Current() == ExpectedNone());
  UR_EXPECT_EQ(changes, fake.changes);
  UR_EXPECT_TRUE(flow.Start("talisman", kColdkey));
  UR_EXPECT_TRUE(!fake.checked.empty() && fake.checked.back() == kColdkey);
}

// CONSENT §4: the wallet signs the server's message and nothing else: the
// challenge exactly as the SDK returned it, and the submit sends that
// challenge's coldkey and message with the wallet's signature
UR_TEST(Consent_TheWalletSignsOnlyTheServersMessage) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());
  const Challenge issued = IssuedChallenge();
  SignUpToTheSubmit(flow, fake, issued);
  UR_EXPECT_EQ(1u, fake.signRequests.size());
  if (fake.signRequests.empty()) return;
  const Challenge& asked = fake.signRequests[0].second;
  UR_EXPECT_TRUE(asked.message == issued.message);
  UR_EXPECT_TRUE(asked.coldkeySs58 == issued.coldkeySs58);
  UR_EXPECT_TRUE(asked.fromEpoch == issued.fromEpoch && asked.throughEpoch == issued.throughEpoch);
  UR_EXPECT_TRUE(asked.expiresAt == issued.expiresAt);
  UR_EXPECT_EQ(1u, fake.submits.size());
  if (fake.submits.empty()) return;
  UR_EXPECT_TRUE(fake.submits[0].message == issued.message);
  UR_EXPECT_TRUE(fake.submits[0].coldkey == issued.coldkeySs58);
  UR_EXPECT_TRUE(fake.submits[0].signature == kSignature);
}

// The apps' steps: a proof for another message or another coldkey than the
// challenge's is not this consent: it reads sn_consent_failed and is never
// submitted
UR_TEST(Consent_AProofForAnotherMessageOrColdkeyIsNotSubmitted) {
  const Challenge issued = IssuedChallenge();
  Signed otherMessage = ProofFor(issued);
  otherMessage.message = ConsentMessage(kOtherColdkey);
  Signed rewrapped = ProofFor(issued);
  rewrapped.message = "<Bytes>" + issued.message + "</Bytes>";
  Signed otherColdkey = ProofFor(issued);
  otherColdkey.address = kOtherColdkey;
  for (const Signed& proof : {otherMessage, rewrapped, otherColdkey}) {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, issued, std::string());
    Answer(fake.signDone, 0, proof);
    UR_EXPECT_TRUE(fake.submits.empty());
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone(Notice::Failed));
    UR_EXPECT_TRUE(fake.errors.empty());
  }
}

// CONSENT §4: nothing is signed without the server's message: a refused
// challenge, or one without a message, reaches no wallet
UR_TEST(Consent_NothingIsSignedWithoutTheServersMessage) {
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, std::nullopt, "chain_rpc_unreachable: GET /sn/epoch");
    UR_EXPECT_TRUE(fake.signRequests.empty());
    UR_EXPECT_TRUE(fake.submits.empty());
  }
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Challenge empty = IssuedChallenge();
    empty.message.clear();
    Answer(fake.challengeDone, 0, empty, std::string());
    UR_EXPECT_TRUE(fake.signRequests.empty());
    UR_EXPECT_TRUE(fake.submits.empty());
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone());
    UR_EXPECT_EQ(1u, fake.errors.size());
  }
}

// CONSENT §4: nothing is submitted without the wallet's signature
UR_TEST(Consent_NothingIsSubmittedWithoutASignature) {
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
    Answer(fake.signDone, 0, Signed{false, {}, {}, {}, "The request was declined in your wallet."});
    UR_EXPECT_TRUE(fake.submits.empty());
  }
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
    Signed unsigned_ = ProofFor(IssuedChallenge());
    unsigned_.signature.clear();
    Answer(fake.signDone, 0, unsigned_);
    UR_EXPECT_TRUE(fake.submits.empty());
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone());
  }
}

// CONSENT §4: Change wallet reruns the flow, and a failed change keeps the
// consent there is
UR_TEST(Consent_ChangeRerunsTheFlowAndKeepsTheConsentOnFailure) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, InForce());
  UR_EXPECT_TRUE(flow.Current().action == Action::Change);
  UR_EXPECT_TRUE(flow.Start("taocom", kColdkey));
  UR_EXPECT_TRUE(flow.Current() == ExpectedSigning());
  Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
  UR_EXPECT_TRUE(fake.SignedWith(0) == "taocom");
  Answer(fake.signDone, 0, Signed{false, {}, {}, {}, "challenge_expired: "});
  UR_EXPECT_TRUE(flow.Current() == ExpectedActive(kOtherColdkey, Notice::Expired));
  // and a change that lands shows the new coldkey, from the next epoch
  UR_EXPECT_TRUE(flow.Start("taocom", kColdkey));
  UR_EXPECT_TRUE(flow.Current().notice == Notice::None);
  Answer(fake.challengeDone, 1, IssuedChallenge(), std::string());
  Answer(fake.signDone, 1, ProofFor(IssuedChallenge()));
  Answer(fake.submitDone, 0, Submitted{true, {}});
  Answer(fake.statusDone, fake.statusDone.size() - 1, FromNextEpoch(kColdkey), std::string());
  UR_EXPECT_TRUE(flow.Current() == ExpectedActive(kColdkey));
}

// CONSENT §4: a signing starts only from a known status, with a wallet, and
// one at a time
UR_TEST(Consent_StartWaitsForAKnownStatusAndOneSigning) {
  Fake fake;
  Flow flow(fake.Ports());
  fake.Attach(flow);
  flow.Load();  // loading
  UR_EXPECT_FALSE(flow.Start("talisman", kColdkey));
  Answer(fake.statusDone, 0, NoConsent(), std::string());
  UR_EXPECT_FALSE(flow.Start(std::string(), kColdkey));
  UR_EXPECT_TRUE(fake.challengeColdkeys.empty());
  UR_EXPECT_TRUE(flow.Start("talisman", kColdkey));
  UR_EXPECT_FALSE(flow.Start("walletconnect", kOtherColdkey));  // busy
  UR_EXPECT_EQ(1u, fake.challengeColdkeys.size());
}

// CONSENT §4: a new load or a sign-out drops the signing in flight: its late
// answers ask nothing more and change nothing
UR_TEST(Consent_ALoadOrResetDropsTheSigningInFlight) {
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    flow.Load();
    UR_EXPECT_FALSE(flow.Busy());
    Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
    UR_EXPECT_TRUE(fake.signRequests.empty());
    Answer(fake.statusDone, 1, InForce(), std::string());
    UR_EXPECT_TRUE(flow.Current() == ExpectedActive(kOtherColdkey));
  }
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
    flow.Reset();
    Answer(fake.signDone, 0, ProofFor(IssuedChallenge()));
    UR_EXPECT_TRUE(fake.submits.empty());
    UR_EXPECT_TRUE(flow.Current() == View{});
  }
  {
    // a status answer for a load that a sign-out replaced
    Fake fake;
    Flow flow(fake.Ports());
    fake.Attach(flow);
    flow.Load();
    flow.Reset();
    Answer(fake.statusDone, 0, InForce(), std::string());
    UR_EXPECT_TRUE(flow.Current() == View{});
  }
}

// CONSENT §4: a status read again after the submit that cannot be read is
// unavailable, like any other read
UR_TEST(Consent_AnUnreadableStatusAfterTheSubmitIsUnavailable) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());
  SignUpToTheSubmit(flow, fake);
  Answer(fake.submitDone, 0, Submitted{true, {}});
  Answer(fake.statusDone, 1, std::nullopt, "GET /sn/wallet returned no result");
  UR_EXPECT_TRUE(flow.Current().line == Line::Unavailable);
  UR_EXPECT_TRUE(flow.Current().action == Action::None);
}

// ---- the refusals ------------------------------------------------------------------

// CONSENT §4, A1: an expired challenge says sn_consent_expired, however it was
// found: the SDK refusing the challenge, the session's own expiry while the
// wallet signed, or the server refusing the late submit; Sign is offered again
UR_TEST(Consent_AnExpiredChallengeSaysSo) {
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, std::nullopt,
           "challenge_expired: the challenge expired at 1760367600");
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone(Notice::Expired));
    UR_EXPECT_TRUE(fake.errors.empty());
  }
  {
    // the session's expiry, as WalletConnect hands it on (ReportsCode)
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("taocom", kColdkey);
    Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
    Answer(fake.signDone, 0, Signed{false, {}, {}, {}, "challenge_expired: "});
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone(Notice::Expired));
    UR_EXPECT_TRUE(fake.submits.empty());
    UR_EXPECT_TRUE(fake.errors.empty());
  }
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    SignUpToTheSubmit(flow, fake);
    Answer(fake.submitDone, 0,
           Submitted{false, "challenge_expired: the server refused the message"});
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone(Notice::Expired));
    UR_EXPECT_EQ(1u, fake.statusDone.size());  // nothing was stored: no read
    UR_EXPECT_TRUE(fake.errors.empty());
  }
}

// CONSENT §4, A1: a submit the server refused with signature_mismatch says
// sn_consent_failed
UR_TEST(Consent_AMismatchedSignatureSaysSo) {
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());
  SignUpToTheSubmit(flow, fake);
  Answer(fake.submitDone, 0, 
      Submitted{false, "signature_mismatch: the signature does not verify for the coldkey"});
  UR_EXPECT_TRUE(flow.Current() == ExpectedNone(Notice::Failed));
  UR_EXPECT_TRUE(fake.errors.empty());
  // trying again clears it
  UR_EXPECT_TRUE(flow.Start("talisman", kColdkey));
  UR_EXPECT_TRUE(flow.Current().notice == Notice::None);
}

// CONSENT §4, A1, A2.4: a session that is not the network owner's says
// sn_consent_owner_only, from the challenge or the submit
UR_TEST(Consent_NotTheNetworkOwnerSaysSo) {
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, std::nullopt,
           "network_owner_only: the session is not the network owner's");
    UR_EXPECT_TRUE(flow.Current() == ExpectedNone(Notice::OwnerOnly));
    UR_EXPECT_TRUE(fake.signRequests.empty());
    UR_EXPECT_TRUE(fake.errors.empty());
  }
  {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, InForce());
    SignUpToTheSubmit(flow, fake);
    Answer(fake.submitDone, 0, Submitted{false, "network_owner_only: no network credential"});
    UR_EXPECT_TRUE(flow.Current() == ExpectedActive(kOtherColdkey, Notice::OwnerOnly));
    UR_EXPECT_TRUE(fake.errors.empty());
  }
}

// CONSENT §4: every other failure goes to the page's error surface as it came,
// and the section is back where it was
UR_TEST(Consent_OtherErrorsGoToThePagesErrorSurface) {
  const char* const errors[] = {
      "The Talisman extension was not found in this browser. Install it, then try again.",
      "chain_rpc_unreachable: GET /sn/epoch",
      "wallet_blocked: This wallet can't be used with URnetwork.",
      "",
  };
  for (const char* error : errors) {
    // at each step: the challenge, the signature, the submit
    for (int step = 0; step < 3; ++step) {
      Fake fake;
      Flow flow(fake.Ports());
      LoadWith(flow, fake, NoConsent());
      flow.Start("talisman", kColdkey);
      if (step == 0) {
        Answer(fake.challengeDone, 0, std::nullopt, error);
      } else {
        Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
        if (step == 1) {
          Answer(fake.signDone, 0, Signed{false, {}, {}, {}, error});
        } else {
          Answer(fake.signDone, 0, ProofFor(IssuedChallenge()));
          Answer(fake.submitDone, 0, Submitted{false, error});
        }
      }
      const std::string where = std::string("step ") + std::to_string(step) + ": " + error;
      UR_EXPECT_TRUE_MSG(where, flow.Current() == ExpectedNone());
      UR_EXPECT_TRUE_MSG(where, fake.errors.size() == 1 && fake.errors[0] == error);
    }
  }
}

// CONSENT §4: a wallet step the user closed, or one another wallet flow took
// the bridge from, ends quietly: no refusal, no error
UR_TEST(Consent_AClosedOrSupersededWalletStepEndsQuietly) {
  const std::string quiet[] = {std::string(urnw::bittensor::kCancelled),
                               "superseded by a wallet connect request",
                               "superseded by another wallet flow"};
  for (const std::string& error : quiet) {
    for (const Status& status : {NoConsent(), InForce()}) {
      Fake fake;
      Flow flow(fake.Ports());
      LoadWith(flow, fake, status);
      flow.Start("taocom", kColdkey);
      Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
      Answer(fake.signDone, 0, Signed{false, {}, {}, {}, error});
      const View back = status.active ? ExpectedActive(kOtherColdkey) : ExpectedNone();
      UR_EXPECT_TRUE_MSG(error, flow.Current() == back);
      UR_EXPECT_TRUE_MSG(error, fake.errors.empty());
      UR_EXPECT_TRUE_MSG(error, fake.submits.empty());
    }
  }
}

// A3: text a wallet's return carries is never a refusal: the signature step
// words only its own expiry, so a return's "network_owner_only: ..." or
// "signature_mismatch: ..." goes to the page's error surface like any other
// failure (the review's ProbeConsentPageTextSpoofsRefusal)
UR_TEST(Consent_AReturnsTextIsNeverARefusal) {
  for (const char* forged : {"network_owner_only: x", "signature_mismatch: x",
                             "network_owner_only", "signature_mismatch"}) {
    Fake fake;
    Flow flow(fake.Ports());
    LoadWith(flow, fake, NoConsent());
    flow.Start("talisman", kColdkey);
    Answer(fake.challengeDone, 0, IssuedChallenge(), std::string());
    Answer(fake.signDone, 0, Signed{false, {}, {}, {}, forged});
    UR_EXPECT_TRUE_MSG(forged, flow.Current() == ExpectedNone());
    UR_EXPECT_TRUE_MSG(forged, fake.errors.size() == 1 && fake.errors[0] == forged);
    UR_EXPECT_TRUE_MSG(forged, fake.submits.empty());
  }
}

// A1, A3: each step words only the refusals the SDK reports for it: the status
// the owner, the challenge the owner and the expiry, the signature its expiry,
// and the submit all three
UR_TEST(Consent_EachStepWordsOnlyItsOwnRefusals) {
  using consent::NoticeFor;
  using consent::Step;
  const char* const owner = "network_owner_only: x";
  const char* const expired = "challenge_expired: x";
  const char* const mismatch = "signature_mismatch: x";
  UR_EXPECT_TRUE(NoticeFor(Step::Status, owner) == Notice::OwnerOnly);
  UR_EXPECT_TRUE(NoticeFor(Step::Status, expired) == Notice::None);
  UR_EXPECT_TRUE(NoticeFor(Step::Status, mismatch) == Notice::None);
  UR_EXPECT_TRUE(NoticeFor(Step::Challenge, owner) == Notice::OwnerOnly);
  UR_EXPECT_TRUE(NoticeFor(Step::Challenge, expired) == Notice::Expired);
  UR_EXPECT_TRUE(NoticeFor(Step::Challenge, mismatch) == Notice::None);
  UR_EXPECT_TRUE(NoticeFor(Step::Signature, owner) == Notice::None);
  UR_EXPECT_TRUE(NoticeFor(Step::Signature, expired) == Notice::Expired);
  UR_EXPECT_TRUE(NoticeFor(Step::Signature, mismatch) == Notice::None);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, owner) == Notice::OwnerOnly);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, expired) == Notice::Expired);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, mismatch) == Notice::Failed);
  for (const Step step : {Step::Status, Step::Challenge, Step::Signature, Step::Submit}) {
    UR_EXPECT_TRUE(NoticeFor(step, "") == Notice::None);
    UR_EXPECT_TRUE(NoticeFor(step, "wallet_error: user_rejected") == Notice::None);
  }
  // and a challenge refused with a mismatch's code is the page's error surface's
  Fake fake;
  Flow flow(fake.Ports());
  LoadWith(flow, fake, NoConsent());
  flow.Start("talisman", kColdkey);
  Answer(fake.challengeDone, 0, std::nullopt, mismatch);
  UR_EXPECT_TRUE(flow.Current() == ExpectedNone());
  UR_EXPECT_TRUE(fake.errors.size() == 1 && fake.errors[0] == mismatch);
}

// A3: a consent shows no words of a return's own, keeps waiting through a
// return its session did not take, and every other flow is as before
UR_TEST(Consent_AReturnNeverSpeaksForTheConsent) {
  namespace bt = urnw::bittensor;
  UR_EXPECT_FALSE(bt::ShowsPageText(bt::kPurposeConsent));
  for (const std::string_view purpose :
       {bt::kPurposeLogin, bt::kPurposeCreate, bt::kPurposeConnect, bt::kPurposeAdd}) {
    UR_EXPECT_TRUE(bt::ShowsPageText(purpose));
    UR_EXPECT_FALSE(bt::KeepsWaiting(purpose, bt::kStateAwaitingWallet, false));
  }
  UR_EXPECT_TRUE(bt::kStateAwaitingWallet == "awaiting_wallet");
  // a return the session did not take, that left it waiting
  UR_EXPECT_TRUE(bt::KeepsWaiting(bt::kPurposeConsent, "awaiting_wallet", false));
  // one that ended it, or a proof, is the consent's answer
  for (const char* state : {"failed", "signed", "cancelled", "challenge"}) {
    UR_EXPECT_TRUE_MSG(state, !bt::KeepsWaiting(bt::kPurposeConsent, state, false));
  }
  UR_EXPECT_FALSE(bt::KeepsWaiting(bt::kPurposeConsent, "awaiting_wallet", true));
  // a page failure with a code this app does not word reads as the generic
  // failure for a consent, never as the page's text
  UR_EXPECT_TRUE(bt::FailureTextFor("wallet_error", "zzz", false) == bt::FailureText::Generic);

  const std::string wallet = ReadCode("WalletConnect.cpp");
  if (wallet.empty()) {
    UR_FAIL("could not read WalletConnect.cpp");
    return;
  }
  const std::string deliver = FunctionBody(wallet, "void WalletConnect::DeliverBittensorResult(");
  UR_EXPECT_TRUE(Contains(deliver, "const bool pageText = !result.ErrorMessage.empty() && "
                                   "bittensor::ShowsPageText(purpose);"));
  UR_EXPECT_TRUE(Contains(deliver, "bittensor::FailureTextFor(result.ErrorCode, "
                                   "result.BridgeErrorCode, pageText)"));
  // the premise, for every other flow: the page's words reach them as before
  UR_EXPECT_TRUE(Contains(
      deliver, "case bittensor::FailureText::PageText:\n          on_error(result.ErrorMessage);"));
  const std::string ret = FunctionBody(wallet, "void WalletConnect::HandleBittensorReturn(");
  UR_EXPECT_TRUE(Contains(ret, "bittensor::KeepsWaiting(bittensorSession_->purpose(), "
                               "bittensorSession_->state(),"));
  UR_EXPECT_TRUE(Before(ret, "bittensor::KeepsWaiting(", "DeliverBittensorResult(result,"));
}

// A3: sign-out answers and empties the one signature slot a coldkey connect or a
// consent waits in, keeps a challenge still being fetched from the bridge, and
// cancels the session: a waiting request would otherwise take the next
// sign-in's proof first (the review's ProbeConsentSlotSurvivesSignOut)
UR_TEST(ConsentWiring_SignOutReleasesTheSignatureSlot) {
  namespace bridge = urnw::bridge;
  UR_EXPECT_TRUE(bridge::RouteSignature(/*signWaiting=*/true, /*createWaiting=*/false,
                                        /*walletSignInWaiting=*/true) ==
                 bridge::SignatureRoute::AnswerRequest);
  UR_EXPECT_TRUE(bridge::IsSuperseded("superseded by signing out"));
  const std::string host = ReadCode("SdkHost.cpp");
  if (host.empty()) {
    UR_FAIL("could not read SdkHost.cpp");
    return;
  }
  const std::string logout = FunctionBody(host, "void SdkHost::LogoutNow()");
  for (const char* step :
       {"signDone = std::move(walletSignDone_);", "walletSignDone_ = nullptr;",
        "if (signDone) walletFlows_.Begin();", "session->cancel();",
        "out.error = \"superseded by signing out\";", "signDone(std::move(out));"}) {
    UR_EXPECT_TRUE_MSG(step, Contains(logout, step));
  }
  // answered outside mutex_, before the sign-out takes it for the rest
  UR_EXPECT_TRUE(Before(logout, "signDone(std::move(out));", "pendingWalletAuth_.reset();"));
  UR_EXPECT_TRUE(Before(logout, "walletSignDone_ = nullptr;", "signDone(std::move(out));"));
}

// A1: the codes are the SDK's, matched on its "code: message" prefix only
UR_TEST(Consent_CodesMatchOnTheSdksPrefix) {
  UR_EXPECT_TRUE(consent::kNetworkOwnerOnly == "network_owner_only");
  UR_EXPECT_TRUE(consent::kChallengeExpired == "challenge_expired");
  UR_EXPECT_TRUE(consent::kSignatureMismatch == "signature_mismatch");
  UR_EXPECT_TRUE(consent::HasCode("challenge_expired", consent::kChallengeExpired));
  UR_EXPECT_TRUE(consent::HasCode("challenge_expired: ", consent::kChallengeExpired));
  UR_EXPECT_TRUE(consent::HasCode("challenge_expired: at 1760367600", consent::kChallengeExpired));
  UR_EXPECT_FALSE(consent::HasCode("challenge_expired:no space", consent::kChallengeExpired));
  UR_EXPECT_FALSE(consent::HasCode("challenge_expiredx: y", consent::kChallengeExpired));
  UR_EXPECT_FALSE(consent::HasCode("challenge_expire", consent::kChallengeExpired));
  UR_EXPECT_FALSE(consent::HasCode("error: challenge_expired: y", consent::kChallengeExpired));
  UR_EXPECT_FALSE(consent::HasCode("", consent::kChallengeExpired));
  using consent::NoticeFor;
  using consent::Step;
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, "challenge_expired: x") == Notice::Expired);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, "signature_mismatch: x") == Notice::Failed);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, "signature_mismatch") == Notice::Failed);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, "network_owner_only: x") == Notice::OwnerOnly);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, "invalid_ss58_address: x") == Notice::None);
  UR_EXPECT_TRUE(NoticeFor(Step::Submit, "The request was declined in your wallet.") ==
                 Notice::None);
  UR_EXPECT_TRUE(consent::IsQuiet(urnw::bittensor::kCancelled));
  UR_EXPECT_TRUE(consent::IsQuiet("superseded by another sign-in method"));
  UR_EXPECT_FALSE(consent::IsQuiet("challenge_expired: "));
  UR_EXPECT_FALSE(consent::IsQuiet(""));
}

// A1: only a consent session's expiry reaches its flow in the SDK's coded
// form; every other refusal, and the same code in any other flow, reads as
// before
UR_TEST(Consent_OnlyAConsentsExpiryIsHandedOnCoded) {
  namespace bt = urnw::bittensor;
  UR_EXPECT_TRUE(bt::kPurposeConsent == "consent");
  UR_EXPECT_TRUE(bt::ReportsCode(bt::kPurposeConsent, "challenge_expired"));
  for (const std::string_view purpose :
       {bt::kPurposeLogin, bt::kPurposeCreate, bt::kPurposeConnect, bt::kPurposeAdd}) {
    UR_EXPECT_FALSE(bt::ReportsCode(purpose, "challenge_expired"));
  }
  for (const char* code : {"message_mismatch", "address_mismatch", "invalid_signature",
                           "wallet_error", "no_challenge", ""}) {
    UR_EXPECT_TRUE_MSG(code, !bt::ReportsCode(bt::kPurposeConsent, code));
  }
}

// ---- the wiring -----------------------------------------------------------------

// CONSENT §4: the section sits between the Settlement v2 tile and the
// Bittensor wallet block, and draws the flow's view and nothing else
UR_TEST(ConsentWiring_TheSectionDrawsTheFlowsView) {
  const std::string page = ReadCode("EarningsPage.cpp");
  if (page.empty()) {
    UR_FAIL("could not read EarningsPage.cpp");
    return;
  }
  const std::string build = FunctionBody(page, "void EarningsPage::BuildEarningsPane()");
  UR_EXPECT_TRUE(Before(build, "ledgerCard_ = row.root;", "consentCard_ = row.root;"));
  UR_EXPECT_TRUE(Before(build, "consentCard_ = row.root;", "walletStatus_ = group.meta;"));
  UR_EXPECT_TRUE(Contains(build, "T_(\"sn_consent_title\", \"Payout consent\")"));
  UR_EXPECT_EQ(2u, CountOf(build, "sigc::mem_fun(*this, &EarningsPage::OnConsentAction)"));
  const std::string draw = FunctionBody(page, "void EarningsPage::RebuildConsentSection()");
  UR_EXPECT_TRUE(Contains(draw, "const consent::View view = consent_.Current();"));
  for (const char* drawn :
       {"case consent::Line::Loading:", "case consent::Line::Unavailable:",
        "case consent::Line::None:", "case consent::Line::Active:", "case consent::Line::Signing:",
        "case consent::Notice::Expired:", "case consent::Notice::Failed:",
        "case consent::Notice::OwnerOnly:", "consentBody_->set_visible(view.body);",
        "consentChangeNote_->set_visible(view.changeNote);",
        "consentSignButton_->set_visible(view.action == consent::Action::Sign);",
        "consentChangeButton_->set_visible(view.action == consent::Action::Change);",
        "ShortSs58(view.coldkeySs58)"}) {
    UR_EXPECT_TRUE_MSG(drawn, Contains(draw, drawn));
  }
  // each sentence in its key
  UR_EXPECT_TRUE(Before(draw, "case consent::Line::None:", "T_(\"sn_consent_none\""));
  UR_EXPECT_TRUE(Before(draw, "case consent::Line::Active:", "T_(\"sn_consent_active\""));
  UR_EXPECT_TRUE(Before(draw, "case consent::Line::Signing:", "T_(\"sn_consent_signing\""));
  UR_EXPECT_TRUE(Before(draw, "case consent::Notice::Expired:", "T_(\"sn_consent_expired\""));
  UR_EXPECT_TRUE(Before(draw, "case consent::Notice::Failed:", "T_(\"sn_consent_failed\""));
  UR_EXPECT_TRUE(Before(draw, "case consent::Notice::OwnerOnly:", "T_(\"sn_consent_owner_only\""));
  // the flow redraws the section, and its other errors are the page's snackbar
  UR_EXPECT_TRUE(Contains(page, "consent_.on_changed = [this] { RebuildWalletBlock(); };"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::RebuildWalletBlock()"),
                          "RebuildConsentSection();"));
  UR_EXPECT_TRUE(Contains(page, "consent_.on_error = [this](const std::string& error) {"));
  UR_EXPECT_TRUE(Contains(page, "Notify(SnErrorMessage(error, T_(\"something_went_wrong\""));
}

// CONSENT §5: every key the section shows is the store's, its English the
// catalog's msgid (sn_consent_active's {coldkey} is "{}" on linux)
UR_TEST(ConsentWiring_EveryKeyIsTheCatalogsMsgid) {
  const std::string page = ReadRaw("EarningsPage.cpp");
  const std::string catalog = ReadRaw("../po/en.po");
  if (page.empty() || catalog.empty()) {
    UR_FAIL("could not read EarningsPage.cpp or po/en.po");
    return;
  }
  const std::pair<const char*, const char*> keys[] = {
      {"sn_consent_title", "Payout consent"},
      {"sn_consent_none", "No payout wallet yet. Earnings aren't paid until you sign."},
      {"sn_consent_body",
       "Sign once with your Bittensor wallet so this network's earnings are paid to it."},
      {"sn_consent_sign", "Sign with wallet"},
      {"sn_consent_signing", "Waiting for your wallet's signature…"},
      {"sn_consent_active", "Earnings are paid to {}"},
      {"sn_consent_change", "Change wallet"},
      {"sn_consent_change_note", "A new wallet takes effect from the next epoch."},
      {"sn_consent_expired", "The request expired. Try again."},
      {"sn_consent_failed", "The signature didn't match this wallet. Try again."},
      {"sn_consent_owner_only", "Only the network owner can choose the payout wallet."},
  };
  for (const auto& [key, english] : keys) {
    const std::string entry =
        std::string("msgctxt \"") + key + "\"\nmsgid \"" + english + "\"\n";
    UR_EXPECT_TRUE_MSG(std::string(key) + " is in the catalog", Contains(catalog, entry));
    // the page looks it up with the same English
    const std::vector<std::string> fallbacks = FallbacksOf(page, key);
    UR_EXPECT_TRUE_MSG(std::string(key) + " is looked up", !fallbacks.empty());
    for (const std::string& fallback : fallbacks) {
      UR_EXPECT_TRUE_MSG(std::string(key) + ": \"" + fallback + "\" is the catalog's msgid",
                         fallback == english);
    }
  }
}

// CONSENT §9: the boundary calls the SDK's consent API by its final names and
// keeps its errors' "code: message" form, a refused submit's code included
UR_TEST(ConsentWiring_TheBoundaryCallsTheSdksConsentApi) {
  const std::string page = ReadCode("EarningsPage.cpp");
  const std::string status =
      FunctionBody(page, "void FetchConsentStatus(SdkHost& host, ConsentStatusDone done)");
  UR_EXPECT_TRUE(Contains(status, "host.api().snNetworkConsentStatus("));
  for (const char* field :
       {"status.active = result->active;", "status.coldkeySs58 = result->coldkey_ss58;",
        "status.fromEpoch = result->from_epoch;", "status.throughEpoch = result->through_epoch;"}) {
    UR_EXPECT_TRUE_MSG(field, Contains(status, field));
  }
  const std::string challenge = FunctionBody(page, "void RequestConsentChallenge(SdkHost& host,");
  UR_EXPECT_TRUE(Contains(challenge, "host.api().snNetworkConsentChallenge("));
  UR_EXPECT_TRUE(Contains(challenge, "err.value_or(std::string(\"no challenge\"))"));
  for (const char* field :
       {"challenge.message = result->message;", "challenge.coldkeySs58 = result->coldkey_ss58;",
        "challenge.fromEpoch = result->from_epoch;",
        "challenge.throughEpoch = result->through_epoch;",
        "challenge.expiresAt = result->expires_at;"}) {
    UR_EXPECT_TRUE_MSG(field, Contains(challenge, field));
  }
  const std::string toSdk = FunctionBody(
      page,
      "urnet::SnNetworkConsentChallengeResult ToSdkChallenge(const consent::Challenge& challenge)");
  for (const char* field : {"result.message = challenge.message;",
                            "result.coldkey_ss58 = challenge.coldkeySs58;",
                            "result.from_epoch = challenge.fromEpoch;",
                            "result.through_epoch = challenge.throughEpoch;",
                            "result.expires_at = challenge.expiresAt;"}) {
    UR_EXPECT_TRUE_MSG(field, Contains(toSdk, field));
  }
  const std::string submit = FunctionBody(page, "void SubmitConsent(SdkHost& host,");
  UR_EXPECT_TRUE(Contains(submit, "host.api().snNetworkConsentSubmit("));
  UR_EXPECT_TRUE(Contains(submit, "coldkeySs58, message, signature,"));
  UR_EXPECT_TRUE(Contains(submit, "result->error->code.value_or(std::string())"));
  UR_EXPECT_TRUE(Contains(submit, "code + \": \" + result->error->message"));
}

// The apps' steps: Sign with wallet picks the wallet on the shared chooser,
// then asks the coldkey in an empty field whose Continue waits for the SDK's
// ss58 check, and only then starts the flow with both
UR_TEST(ConsentWiring_TheWalletThenTheTypedColdkey) {
  const std::string page = ReadCode("EarningsPage.cpp");
  const std::string action = FunctionBody(page, "void EarningsPage::OnConsentAction()");
  UR_EXPECT_TRUE(Contains(action, "ChooseBittensorWallet([this](const std::string& walletId) { "
                                  "AskConsentColdkey(walletId); });"));
  const std::string chooser = FunctionBody(page, "void EarningsPage::ChooseBittensorWallet(");
  UR_EXPECT_TRUE(Contains(chooser, "NewBittensorWalletChooser("));
  UR_EXPECT_TRUE(Contains(chooser, "if (!walletId.empty()) ctx->chosen(std::string(walletId));"));
  const std::string ask = FunctionBody(page, "void EarningsPage::AskConsentColdkey(");
  // nothing prefilled: the payout coldkey is a deliberate choice
  UR_EXPECT_FALSE(Contains(ask, "field->set_text("));
  UR_EXPECT_TRUE(
      Contains(ask, "T_(\"earnings_address_placeholder\", \"Enter a Bittensor address\")"));
  const std::string enable = "adw_message_dialog_set_response_enabled(ADW_MESSAGE_DIALOG(dialog), ";
  UR_EXPECT_TRUE(Contains(ask, enable + "\"continue\", FALSE);"));
  UR_EXPECT_TRUE(Contains(ask, "const bool valid = sn::ValidateSs58(address);"));
  UR_EXPECT_TRUE(Contains(ask, enable + "\"continue\", valid);"));
  UR_EXPECT_TRUE(Contains(
      ask, "T_(\"invalid_ss58_address\", \"That is not a valid Bittensor address.\")"));
  UR_EXPECT_TRUE(Contains(
      ask, "if (g_strcmp0(response, \"continue\") != 0 || ctx->self->connecting_) return;"));
  UR_EXPECT_TRUE(Contains(ask, "ctx->self->consent_.Start(ctx->walletId, "
                               "TrimWhitespace(ctx->field->get_text().raw()));"));
  // the flow checks it again before it asks anything
  const std::string ports = FunctionBody(page, "consent::Flow::Ports EarningsPage::ConsentPorts()");
  UR_EXPECT_TRUE(Contains(ports, "ports.validSs58 = [](const std::string& address) { return "
                                 "sn::ValidateSs58(address); };"));
}

// CONSENT §4, A1: the signature is the existing connector's: a consent session
// over the challenge as it came, on the same bridge and manual sheet as every
// Bittensor flow, whose proof the flow checks before it submits
UR_TEST(ConsentWiring_TheSignatureIsTheExistingConnectors) {
  const std::string page = ReadCode("EarningsPage.cpp");
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string wallet = ReadCode("WalletConnect.cpp");
  if (page.empty() || host.empty() || wallet.empty()) {
    UR_FAIL("could not read the sources");
    return;
  }
  const std::string ports = FunctionBody(page, "consent::Flow::Ports EarningsPage::ConsentPorts()");
  UR_EXPECT_TRUE(Contains(ports, "host_.SignBittensorConsent("));
  UR_EXPECT_TRUE(Contains(ports, "walletId, sn::ToSdkChallenge(challenge),"));
  UR_EXPECT_TRUE(Contains(
      ports, "done(consent::Signed{signature.ok, signature.address, signature.message,"));
  UR_EXPECT_TRUE(Contains(ports, "sn::FetchConsentStatus(host_,"));
  UR_EXPECT_TRUE(Contains(ports, "sn::RequestConsentChallenge("));
  UR_EXPECT_TRUE(Contains(ports, "sn::SubmitConsent(host_, coldkeySs58, message, signature,"));
  UR_EXPECT_FALSE(Contains(ports, "SignBittensorConnect("));

  const std::string sign = FunctionBody(host, "void SdkHost::SignBittensorConsent(");
  UR_EXPECT_TRUE(Contains(sign, "walletFlows_.Begin()"));
  UR_EXPECT_TRUE(Contains(sign, "walletSignDone_ = std::move(done);"));
  UR_EXPECT_TRUE(Contains(sign, "StartBittensorSession(walletId, std::string("
                                "bittensor::kPurposeConsent), challenge.coldkey_ss58,"));
  UR_EXPECT_TRUE(Contains(sign, "flow, nullptr, nullptr, challenge);"));
  const std::string start = FunctionBody(host, "void SdkHost::StartBittensorSession(");
  // the consent's challenge goes to the session as it came; no
  // /auth/wallet-challenge is asked for it
  UR_EXPECT_TRUE(Contains(
      start, "session->setNetworkConsentChallenge(consent, g_get_real_time() / 1000);"));
  UR_EXPECT_TRUE(Before(start, "if (consent) {\n      ", "session->setNetworkConsentChallenge("));
  UR_EXPECT_TRUE(Before(start, "session->setNetworkConsentChallenge(",
                        "args = session->challengeArgs(expectedAddress);"));
  const size_t posted =
      start.find("if (consent) {\n    PostToMain(std::move(open));\n    return;\n  }");
  const size_t fetched = start.find("api_->authWalletChallenge(");
  UR_EXPECT_TRUE(posted != std::string::npos && fetched != std::string::npos && posted < fetched);
  // the transport opens only while the flow is still the newest
  UR_EXPECT_TRUE(
      Before(start, "if (!WalletFlowIsCurrent(flow)) {", "wallet_.SignWithBittensor(session);"));
  // a consent session's expiry reaches the flow coded, before any other words
  const std::string deliver = FunctionBody(wallet, "void WalletConnect::DeliverBittensorResult(");
  UR_EXPECT_TRUE(Contains(deliver, "bittensor::ReportsCode(purpose, result.ErrorCode)"));
  UR_EXPECT_TRUE(Contains(deliver, "on_error(result.ErrorCode + \": \");"));
  UR_EXPECT_TRUE(Before(deliver, "bittensor::ReportsCode(", "bittensor::FailureTextFor("));
}

// CONSENT §4: every answer reaches the flow on the main loop, only while the
// page's epoch is the one it was asked in, and each step but the read is given
// up on by the consent watchdog, which the page disarms when it goes
UR_TEST(ConsentWiring_EachAnswerIsMarshaledAndWatched) {
  const std::string page = ReadCode("EarningsPage.cpp");
  const std::string ports = FunctionBody(page, "consent::Flow::Ports EarningsPage::ConsentPorts()");
  UR_EXPECT_EQ(4u, CountOf(ports, "PostToMain("));
  UR_EXPECT_EQ(4u, CountOf(ports, "if (*epoch != seen) return;"));
  UR_EXPECT_EQ(3u, CountOf(ports, "BeginFlow("));
  UR_EXPECT_EQ(3u, CountOf(ports, "SettleFlow(consentStep_, generation,"));
  // the signature waits as the coldkey connect does; the calls 20 s
  UR_EXPECT_EQ(1u, CountOf(ports, "BeginFlow(\n        consentStep_, static_cast<int>("
                                  "bittensor::TimeoutMsFor(transport, walletId))"));
  UR_EXPECT_EQ(1u, CountOf(ports, "NotifyContinueInBrowser(walletId);"));
  UR_EXPECT_EQ(2u, CountOf(ports, "BeginFlow(consentStep_, kApiTimeoutMs,"));
  const std::string teardown = FunctionBody(page, "EarningsPage::~EarningsPage()");
  UR_EXPECT_TRUE(Contains(teardown, "consentStep_.timer.disconnect();"));
  UR_EXPECT_TRUE(Contains(teardown, "consent_.on_changed = nullptr;"));
  UR_EXPECT_FALSE(Contains(teardown, "RebuildConsentSection("));
  UR_EXPECT_FALSE(Contains(teardown, "consent_.Reset("));
}

// CONSENT §4: the bridge carries one wallet flow at a time: the consent waits
// for the coldkey connect and the connect for the consent
UR_TEST(ConsentWiring_OneWalletFlowOnTheBridgeAtATime) {
  const std::string page = ReadCode("EarningsPage.cpp");
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::OnConsentAction()"),
                          "if (connecting_ || consent_.Busy()) return;"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::OnConnectWithBridge()"),
                          "if (connecting_ || consent_.Busy()) return;"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::OnConnectManual()"),
                          "if (connecting_ || consent_.Busy()) return;"));
  const std::string block = FunctionBody(page, "void EarningsPage::RebuildWalletBlock()");
  UR_EXPECT_TRUE(Contains(block, "const bool bridgeBusy = connecting_ || consent_.Busy();"));
  UR_EXPECT_TRUE(Contains(block, "connectBridgeButton_->set_sensitive(!bridgeBusy);"));
  UR_EXPECT_TRUE(Contains(block, "connectManualButton_->set_sensitive(checked && !bridgeBusy);"));
  const std::string draw = FunctionBody(page, "void EarningsPage::RebuildConsentSection()");
  UR_EXPECT_TRUE(Contains(draw, "consentSignButton_->set_sensitive(!connecting_);"));
  UR_EXPECT_TRUE(Contains(draw, "consentChangeButton_->set_sensitive(!connecting_);"));
}

// CONSENT §4: the section follows the session: a load reads the status, and
// no session (sign-out, preview) hides it and drops what is in flight
UR_TEST(ConsentWiring_TheSectionFollowsTheSession) {
  const std::string page = ReadCode("EarningsPage.cpp");
  const std::string load = FunctionBody(page, "void EarningsPage::LoadEarnings()");
  UR_EXPECT_TRUE(Before(load, "if (!CanCallApi()) {", "consent_.Load();"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::SettleAllEmpty()"),
                          "consent_.Reset();"));
}

}  // namespace
