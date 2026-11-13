// The wallet bridge's return routing: every public key and signature goes to
// the flow waiting for it, and one nobody waits for is dropped rather than
// turned into a wallet sign-in in a signed-in app.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "WalletBridgeRoute.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

using urnw::bridge::FlowCounter;
using urnw::bridge::IsSuperseded;
using urnw::bridge::PublicKeyRoute;
using urnw::bridge::RoutePublicKey;
using urnw::bridge::RouteSignature;
using urnw::bridge::SignatureRoute;

// The bridge page keeps "Return to URnetwork" on screen after its automatic
// redirect and the key pair lives until the next connect, so the same return
// can arrive twice; with nothing waiting it is dropped.
UR_TEST(WalletBridge_AConnectReturnNobodyWaitsForIsDropped) {
  UR_EXPECT_TRUE(RoutePublicKey(false, false) == PublicKeyRoute::Drop);
}

// Signing in with Phantom or Solflare still chains its challenge.
UR_TEST(WalletBridge_AWalletSignInStillSignsIn) {
  UR_EXPECT_TRUE(RoutePublicKey(false, true) == PublicKeyRoute::SignIn);
}

// Bittensor signs in one hop: a public key never chains anything for it.
UR_TEST(WalletBridge_ABittensorConnectReturnChainsNothing) {
  UR_EXPECT_TRUE(RoutePublicKey(true, true) == PublicKeyRoute::Drop);
  UR_EXPECT_TRUE(RoutePublicKey(true, true, true) == PublicKeyRoute::Drop);
}

// A signature from a superseded or abandoned tab never reaches
// AuthLoginWithWallet; a waiting request comes first, then a network being
// created with a wallet, then a wallet sign-in.
UR_TEST(WalletBridge_ASignatureNobodyWaitsForNeverSignsIn) {
  UR_EXPECT_TRUE(RouteSignature(false, false, false) == SignatureRoute::Drop);
  UR_EXPECT_TRUE(RouteSignature(true, true, true) == SignatureRoute::AnswerRequest);
  UR_EXPECT_TRUE(RouteSignature(true, false, false) == SignatureRoute::AnswerRequest);
  UR_EXPECT_TRUE(RouteSignature(false, true, true) == SignatureRoute::FinishCreate);
  UR_EXPECT_TRUE(RouteSignature(false, true, false) == SignatureRoute::FinishCreate);
  UR_EXPECT_TRUE(RouteSignature(false, false, true) == SignatureRoute::SignIn);
}

UR_TEST(WalletBridge_SupersededIsTheHostsOwnWord) {
  UR_EXPECT_TRUE(IsSuperseded("superseded by another sign-in method"));
  UR_EXPECT_FALSE(IsSuperseded("User rejected the request."));
  UR_EXPECT_FALSE(IsSuperseded(""));
  UR_EXPECT_FALSE(IsSuperseded("superseded"));
}

// "Connect Bittensor wallet" fetches its challenge; meanwhile the user adds a
// Solana wallet as a sign-in method and presses Phantom. When the challenge
// lands it must not open a Bittensor tab: that bridge call would also reset the
// Phantom keypair, and the Phantom return could no longer be read.
UR_TEST(WalletBridge_ALateChallengeNeverOpensTheBridgeForAnOlderFlow) {
  FlowCounter flows;
  UR_EXPECT_FALSE(flows.IsCurrent(0));  // before any flow
  const uint64_t bittensor = flows.Begin();
  UR_EXPECT_TRUE(flows.IsCurrent(bittensor));
  const uint64_t solana = flows.Begin();
  UR_EXPECT_FALSE(flows.IsCurrent(bittensor));  // the late challenge opens nothing
  UR_EXPECT_TRUE(flows.IsCurrent(solana));
  UR_EXPECT_TRUE(flows.Latest() == solana);
  UR_EXPECT_FALSE(flows.IsCurrent(0));
  // a sign-in after that takes the bridge from the connect in turn
  const uint64_t signIn = flows.Begin();
  UR_EXPECT_FALSE(flows.IsCurrent(solana));
  UR_EXPECT_TRUE(flows.IsCurrent(signIn));
}

// ---- the call sites ----------------------------------------------------------

namespace {

std::string ReadSource(const std::string& name) {
  const std::string candidates[] = {
      std::string(UR_SRC_DIR) + "/" + name,
      "app/src/" + name,
      "../app/src/" + name,
      "linux/app/src/" + name,
  };
  for (const auto& path : candidates) {
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    std::ostringstream out;
    out << in.rdbuf();
    if (!out.str().empty()) return out.str();
  }
  return std::string();
}

// A function's text from its signature to its closing brace at column 0.
std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

// The counter only protects anything if the host keeps it: every wallet flow
// start takes a number, the Bittensor connect's challenge checks its number
// before it opens the bridge, and a Solana wallet being added answers a waiting
// Bittensor request itself. A counter nobody checks is the defect this exists
// for, so "could not read the source" fails.
UR_TEST(WalletBridge_TheHostChecksItsFlowBeforeALateChallengeOpensTheBridge) {
  const std::string source = ReadSource("SdkHost.cpp");
  if (source.empty()) {
    UR_FAIL("could not read SdkHost.cpp to check the wallet flow counter is wired");
    return;
  }
  for (const char* flow :
       {"void SdkHost::AddSignInWithSolana(", "void SdkHost::SignBittensorConnect(",
        "void SdkHost::SignInWithSolana(", "void SdkHost::SignInWithBittensor(",
        "void SdkHost::SignInWithSso(", "void SdkHost::CreateNetworkWithPendingWallet("}) {
    const std::string body = FunctionBody(source, flow);
    UR_EXPECT_TRUE_MSG(std::string(flow) + " is defined", !body.empty());
    UR_EXPECT_TRUE_MSG(std::string(flow) + " takes a flow number",
                       body.find("walletFlows_.Begin()") != std::string::npos);
  }

  // every Bittensor flow opens its wallet through StartBittensorSession
  const std::string bittensor = FunctionBody(source, "void SdkHost::StartBittensorSession(");
  const size_t opens = bittensor.find("wallet_.SignWithBittensor(session)");
  const size_t checks = bittensor.rfind("WalletFlowIsCurrent(flow)", opens);
  UR_EXPECT_TRUE_MSG("the Bittensor connect opens the bridge", opens != std::string::npos);
  UR_EXPECT_TRUE_MSG("the Bittensor connect checks its flow before it opens the bridge",
                     opens != std::string::npos && checks != std::string::npos);

  const std::string solana = FunctionBody(source, "void SdkHost::AddSignInWithSolana(");
  UR_EXPECT_TRUE_MSG("a Solana wallet being added answers a waiting Bittensor signature request",
                     solana.find("walletSignDone_") != std::string::npos &&
                         solana.find("\"superseded by another sign-in method\"") !=
                             std::string::npos);

  // every superseded reason the host writes carries the prefix IsSuperseded reads
  int reasons = 0;
  for (size_t at = source.find("\"superseded"); at != std::string::npos;
       at = source.find("\"superseded", at + 1)) {
    const size_t close = source.find('"', at + 1);
    if (close == std::string::npos) break;
    const std::string literal = source.substr(at + 1, close - at - 1);
    ++reasons;
    UR_EXPECT_TRUE_MSG("the reason \"" + literal + "\"", IsSuperseded(literal));
  }
  UR_EXPECT_TRUE_MSG("the host writes superseded reasons", reasons > 0);
}

// A superseded Bittensor connect ended by the user's choice: the page settles it
// before it could raise an error snackbar.
UR_TEST(WalletBridge_ThePageSettlesASupersededConnectQuietly) {
  const std::string source = ReadSource("EarningsPage.cpp");
  if (source.empty()) {
    UR_FAIL("could not read EarningsPage.cpp to check a superseded connect is quiet");
    return;
  }
  const std::string body = FunctionBody(source, "void EarningsPage::OnWalletSigned(");
  const size_t quiet = body.find("bridge::IsSuperseded(signature.error)");
  const size_t notify = body.find("Notify(");
  UR_EXPECT_TRUE_MSG("OnWalletSigned settles a superseded answer before any snackbar",
                     quiet != std::string::npos && notify != std::string::npos && quiet < notify);
}
