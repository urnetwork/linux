// The ur.io/sso bridge contract: the url the app opens, the return it accepts,
// and the two checks (echoed state, token nonce) that keep a stray or replayed
// return from starting a login. Mirrors SsoBridge.hpp's header comment.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "SsoBridge.hpp"

using urnw::sso::BridgeUrl;
using urnw::sso::CheckReturn;
using urnw::sso::JwtClaimString;
using urnw::sso::ParseReturn;
using urnw::sso::Return;
using urnw::sso::SimulatedIdentityToken;

UR_TEST(bridgeUrlCarriesProviderRedirectStateAndNonce) {
  const std::string url = BridgeUrl("apple", "st ate", "n&once");
  UR_EXPECT_TRUE_MSG(url, url == "https://ur.io/sso?provider=apple&redirect_link=urnetwork%3A%2F%2Fsso&state=st%20ate&nonce=n%26once");
}

UR_TEST(returnParsesTokenAndStateAndDecodesEscapes) {
  const Return r = ParseReturn("provider=google&auth_jwt=a.b.c&state=x%20y");
  UR_EXPECT_TRUE_MSG(r.provider, r.provider == "google");
  UR_EXPECT_TRUE_MSG(r.authJwt, r.authJwt == "a.b.c");
  UR_EXPECT_TRUE_MSG(r.state, r.state == "x y");
  UR_EXPECT_TRUE_MSG(r.error, r.error.empty());
  const Return e = ParseReturn("provider=apple&error=User%20cancelled&state=x");
  UR_EXPECT_TRUE_MSG(e.error, e.error == "User cancelled");
}

UR_TEST(nonceClaimReadsBackFromTheTokenPayload) {
  const std::string token = SimulatedIdentityToken("the-nonce", "test");
  const auto nonce = JwtClaimString(token, "nonce");
  UR_EXPECT_TRUE_MSG(nonce.value_or("<none>"), nonce && *nonce == "the-nonce");
  UR_EXPECT_TRUE_MSG(std::string("no claim"), !JwtClaimString(token, "missing"));
  UR_EXPECT_TRUE_MSG(std::string("not a jwt"), !JwtClaimString("garbage", "nonce"));
  UR_EXPECT_TRUE_MSG(std::string("bad base64"), !JwtClaimString("a.@@.c", "nonce"));
}

UR_TEST(checkAcceptsOnlyTheAttemptInFlight) {
  const std::string token = SimulatedIdentityToken("n1", "test");
  Return good{"google", token, "s1", ""};
  UR_EXPECT_TRUE_MSG(std::string("good"), CheckReturn(good, "google", "s1", "n1").ok);
  UR_EXPECT_TRUE_MSG(std::string("no attempt"), !CheckReturn(good, "google", "", "n1").ok);
  UR_EXPECT_TRUE_MSG(std::string("wrong state"), !CheckReturn(good, "google", "s2", "n1").ok);
  UR_EXPECT_TRUE_MSG(std::string("wrong provider"), !CheckReturn(good, "apple", "s1", "n1").ok);
  UR_EXPECT_TRUE_MSG(std::string("wrong nonce"), !CheckReturn(good, "google", "s1", "n2").ok);
  Return noToken{"google", "", "s1", ""};
  UR_EXPECT_TRUE_MSG(std::string("no token"), !CheckReturn(noToken, "google", "s1", "n1").ok);
  Return failed{"google", "", "s1", "denied"};
  const auto v = CheckReturn(failed, "google", "s1", "n1");
  UR_EXPECT_TRUE_MSG(v.error, !v.ok && v.error == "denied");
  // a bridge error for ANOTHER attempt is still not this attempt's error
  Return foreignError{"google", "", "s9", "denied"};
  UR_EXPECT_TRUE_MSG(std::string("foreign error"), CheckReturn(foreignError, "google", "s1", "n1").error == "unexpected sign-in return");
}
