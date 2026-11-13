// The Account > Login methods "Add sign-in method" sheet: the options every
// app offers (Apple, Google, a Solana or Bittensor wallet, an email or phone),
// the add-auth request each one sends, and that a browser or wallet return
// started by an add is answered to the add and never signs in. The linux sheet
// offered only the email.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "AddSignInFlow.hpp"
#include "BittensorWalletFlow.hpp"
#include "WalletBridgeRoute.hpp"

namespace as = urnw::addsignin;
using urnw::bridge::PublicKeyRoute;
using urnw::bridge::RoutePublicKey;
using urnw::bridge::RouteSignature;
using urnw::bridge::SignatureRoute;

// ur.io ADD_SIGN_IN_METHODS and the apple sheet's order
UR_TEST(AddSignIn_OffersAppleGoogleWalletAndEmail) {
  const std::vector<as::Method> want = {as::Method::Apple, as::Method::Google,
                                        as::Method::Wallet, as::Method::Email};
  UR_EXPECT_TRUE(as::Methods() == want);
  const std::vector<as::WalletChain> chains = {as::WalletChain::Solana,
                                               as::WalletChain::Bittensor};
  UR_EXPECT_TRUE(as::WalletChains() == chains);
  UR_EXPECT_TRUE(as::kDefaultMethod == as::Method::Email);
}

// the same store keys as ur.io's sheet (METHOD_LABEL_KEYS, CHAIN_LABEL_KEYS)
UR_TEST(AddSignIn_LabelsUseTheSharedKeys) {
  UR_EXPECT_TRUE(std::string("apple") == std::string(as::MethodLabel(as::Method::Apple).key));
  UR_EXPECT_TRUE(std::string("google") == std::string(as::MethodLabel(as::Method::Google).key));
  UR_EXPECT_TRUE(std::string("wallet") == std::string(as::MethodLabel(as::Method::Wallet).key));
  UR_EXPECT_TRUE(std::string("site_app_email") == std::string(as::MethodLabel(as::Method::Email).key));
  UR_EXPECT_TRUE(std::string("solana_wallet") == std::string(as::ChainLabel(as::WalletChain::Solana).key));
  UR_EXPECT_TRUE(std::string("bittensor_wallet") == std::string(as::ChainLabel(as::WalletChain::Bittensor).key));
  UR_EXPECT_TRUE(std::string("wallet_sign_in_method_added") == std::string(as::AddedMessage(as::Method::Wallet).key));
}

// every option sends an add-auth body the server takes
UR_TEST(AddSignIn_EveryMethodSuppliesAnAuthMethod) {
  const as::Args email = as::EmailArgs("a@example.com", "a-long-password");
  UR_EXPECT_TRUE(as::SuppliesMethod(email));
  UR_EXPECT_TRUE(as::NeedsVerification(as::Method::Email));

  const as::Args apple = as::ProviderArgs(as::Method::Apple, "apple-identity-token");
  UR_EXPECT_TRUE(as::SuppliesMethod(apple));
  UR_EXPECT_TRUE(std::string("apple") == apple.authJwtType);
  UR_EXPECT_TRUE(std::string("apple-identity-token") == apple.authJwt);
  const as::Args google = as::ProviderArgs(as::Method::Google, "google-identity-token");
  UR_EXPECT_TRUE(as::SuppliesMethod(google));
  UR_EXPECT_TRUE(std::string("google") == google.authJwtType);

  const as::Args solana =
      as::WalletArgs(as::WalletChain::Solana, "solana-public-key", "c2lnbmF0dXJl", "message");
  UR_EXPECT_TRUE(as::SuppliesMethod(solana));
  UR_EXPECT_TRUE(std::string("solana") == solana.blockchain);
  const as::Args tao =
      as::WalletArgs(as::WalletChain::Bittensor, "5ss58-address", "0xabcd", "message");
  UR_EXPECT_TRUE(as::SuppliesMethod(tao));
  UR_EXPECT_TRUE(std::string("TAO") == tao.blockchain);
  for (as::Method m : {as::Method::Apple, as::Method::Google, as::Method::Wallet}) {
    UR_EXPECT_FALSE(as::NeedsVerification(m));
  }
}

// the server's "no auth method supplied" cases are refused before a round trip
UR_TEST(AddSignIn_IncompleteBodiesSupplyNoMethod) {
  UR_EXPECT_FALSE(as::SuppliesMethod(as::Args{}));
  UR_EXPECT_FALSE(as::SuppliesMethod(as::EmailArgs("a@example.com", "")));
  as::Args jwt;
  jwt.authJwt = "token";
  UR_EXPECT_FALSE(as::SuppliesMethod(jwt));
  UR_EXPECT_FALSE(as::SuppliesMethod(as::WalletArgs(as::WalletChain::Solana, "key", "", "m")));
}

// A verified sso return goes only to the flow that opened it: an add's return
// is added, never signed in with.
UR_TEST(AddSignIn_AnAddSsoReturnNeverSignsIn) {
  UR_EXPECT_TRUE(as::RouteSso(as::Owner::AddSignIn) == as::SsoRoute::AddSignIn);
  UR_EXPECT_TRUE(as::RouteSso(as::Owner::SignIn) == as::SsoRoute::SignIn);
  UR_EXPECT_TRUE(as::RouteSso(as::Owner::None) == as::SsoRoute::Drop);
}

// A wallet being added chains its challenge, and its signature goes to
// add-auth ahead of any sign-in still waiting.
UR_TEST(AddSignIn_AnAddedWalletsSignatureNeverSignsIn) {
  UR_EXPECT_TRUE(RoutePublicKey(false, false, true) == PublicKeyRoute::AddSignIn);
  UR_EXPECT_TRUE(RoutePublicKey(false, true, true) == PublicKeyRoute::AddSignIn);
  // bittensor has no connect hop
  UR_EXPECT_TRUE(RoutePublicKey(true, false, true) == PublicKeyRoute::Drop);

  UR_EXPECT_TRUE(RouteSignature(false, false, false, true) == SignatureRoute::AnswerAdd);
  UR_EXPECT_TRUE(RouteSignature(false, false, true, true) == SignatureRoute::AnswerAdd);
  UR_EXPECT_TRUE(RouteSignature(false, true, true, true) == SignatureRoute::AnswerAdd);
  UR_EXPECT_TRUE(RouteSignature(true, false, false, true) == SignatureRoute::AnswerRequest);
  // no add waiting: unchanged
  UR_EXPECT_TRUE(RouteSignature(false, false, true, false) == SignatureRoute::SignIn);
  UR_EXPECT_TRUE(RouteSignature(false, false, false, false) == SignatureRoute::Drop);
}

// the Bittensor add runs under its own purpose (sdk BittensorWalletPurposeAdd):
// a login session refuses its bridge return, and that refusal is ignored
UR_TEST(AddSignIn_BittensorAddHasItsOwnPurpose) {
  UR_EXPECT_TRUE(std::string("add") == std::string(urnw::bittensor::kPurposeAdd));
  UR_EXPECT_TRUE(urnw::bittensor::kPurposeAdd != urnw::bittensor::kPurposeLogin);
  UR_EXPECT_TRUE(urnw::bittensor::Classify("purpose_mismatch") == urnw::bittensor::Outcome::Ignore);
}
