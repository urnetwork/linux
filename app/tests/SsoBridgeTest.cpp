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
using urnw::sso::AppleAuthorizeUrl;
using urnw::sso::AppleOAuthState;
using urnw::sso::ParseAppleReturn;
using urnw::sso::GoogleAuthorizeUrl;
using urnw::sso::OAuthState;
using urnw::sso::ParseGoogleReturn;
using urnw::sso::OAuthReturnProvider;
using urnw::sso::UrlPath;

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

UR_TEST(appleAuthorizeUrlCarriesClientRedirectStateAndNonce) {
  const std::string url = AppleAuthorizeUrl("https://api.bringyour.com/", "st ate", "n&once");
  UR_EXPECT_TRUE_MSG(url, url == "https://appleid.apple.com/auth/authorize?client_id=network.ur.service"
                              "&redirect_uri=https%3A%2F%2Fapi.bringyour.com%2Fauth%2Fapple%2Fcallback"
                              "&response_type=code%20id_token&response_mode=form_post&scope=name%20email"
                              "&state=st%20ate&nonce=n%26once");
}

UR_TEST(appleStateCarriesThePlatformClaim) {
  const std::string state = AppleOAuthState("tok-1");
  const auto bytes = urnw::sso::detail::Base64UrlDecode(state);
  UR_EXPECT_TRUE_MSG(state, bytes.has_value());
  const std::string json(bytes->begin(), bytes->end());
  UR_EXPECT_TRUE_MSG(json, json == "{\"platform\":\"linux\",\"token\":\"tok-1\"}");
  UR_EXPECT_TRUE_MSG(state, state.find('=') == std::string::npos && state.find('+') == std::string::npos);
}

UR_TEST(appleReturnParsesTheIdTokenAsTheIdentityToken) {
  const Return r = ParseAppleReturn("state=s1&id_token=a.b.c&code=xyz");
  UR_EXPECT_TRUE_MSG(r.provider, r.provider == "apple");
  UR_EXPECT_TRUE_MSG(r.authJwt, r.authJwt == "a.b.c");
  UR_EXPECT_TRUE_MSG(r.state, r.state == "s1");
  UR_EXPECT_TRUE_MSG(r.error, r.error.empty());
  const Return e = ParseAppleReturn("state=s1&error=user_cancelled_authorize");
  UR_EXPECT_TRUE_MSG(e.error, e.error == "user_cancelled_authorize" && e.authJwt.empty());
  // the same checks accept it exactly like a bridge return
  const std::string token = SimulatedIdentityToken("n1", "test");
  const Return good = ParseAppleReturn("state=s1&id_token=" + token);
  UR_EXPECT_TRUE_MSG(std::string("good"), CheckReturn(good, "apple", "s1", "n1").ok);
  UR_EXPECT_TRUE_MSG(std::string("wrong nonce"), !CheckReturn(good, "apple", "s1", "n2").ok);
}

UR_TEST(urlPathOfAReturn) {
  UR_EXPECT_TRUE_MSG(std::string("path"), UrlPath("urnetwork://oauth/apple?state=1") == "/apple");
  UR_EXPECT_TRUE_MSG(std::string("no path"), UrlPath("urnetwork://sso?state=1").empty());
  UR_EXPECT_TRUE_MSG(std::string("bare host"), UrlPath("urnetwork://sso").empty());
}

UR_TEST(googleAuthorizeUrlCarriesClientRedirectStateNonceAndTheCodeFlow) {
  const std::string url = GoogleAuthorizeUrl("https://api.bringyour.com/", "st ate", "n&once");
  UR_EXPECT_TRUE(url.rfind("https://accounts.google.com/o/oauth2/v2/auth?", 0) == 0);
  UR_EXPECT_TRUE(url.find("client_id=338638865390-cg4m0t700mq9073smhn9do81mr640ig1.apps.googleusercontent.com") !=
                 std::string::npos);
  UR_EXPECT_TRUE(url.find("redirect_uri=https%3A%2F%2Fapi.bringyour.com%2Fauth%2Fgoogle%2Fcallback") !=
                 std::string::npos);
  UR_EXPECT_TRUE(url.find("response_type=code") != std::string::npos);
  UR_EXPECT_TRUE(url.find("scope=openid%20email%20profile") != std::string::npos);
  UR_EXPECT_TRUE(url.find("state=st%20ate") != std::string::npos);
  UR_EXPECT_TRUE(url.find("nonce=n%26once") != std::string::npos);
  UR_EXPECT_TRUE(url.find("prompt=select_account") != std::string::npos);
  // never the bridge, never an id_token response the server could not receive
  UR_EXPECT_TRUE(url.find("ur.io/sso") == std::string::npos);
  UR_EXPECT_TRUE(url.find("id_token") == std::string::npos);
}

UR_TEST(oauthStateCarriesThePlatformClaimForBothProviders) {
  const std::string state = OAuthState("tok-2");
  const auto bytes = urnw::sso::detail::Base64UrlDecode(state);
  UR_EXPECT_TRUE(bytes.has_value());
  const std::string json(bytes->begin(), bytes->end());
  UR_EXPECT_TRUE(json.find("\"platform\":\"linux\"") != std::string::npos);
  UR_EXPECT_TRUE(json.find("\"token\":\"tok-2\"") != std::string::npos);
  UR_EXPECT_TRUE(urnw::sso::AppleOAuthState("tok-2") == state);
}

UR_TEST(googleReturnParsesLikeTheAppleOne) {
  const std::string state = OAuthState("tok-3");
  const Return r = ParseGoogleReturn("state=" + state + "&id_token=a.b.c");
  UR_EXPECT_TRUE(r.provider == "google");
  UR_EXPECT_TRUE(r.authJwt == "a.b.c");
  UR_EXPECT_TRUE(r.state == state);
  UR_EXPECT_TRUE(r.error.empty());
  const Return e = ParseGoogleReturn("state=s1&error=access_denied");
  UR_EXPECT_TRUE(e.error == "access_denied");
  UR_EXPECT_TRUE(e.authJwt.empty());
  // the return path names the provider the deep-link handler hands to on_sso
  UR_EXPECT_TRUE(OAuthReturnProvider("/google") == "google");
  UR_EXPECT_TRUE(OAuthReturnProvider("/apple") == "apple");
  UR_EXPECT_TRUE(OAuthReturnProvider("/other").empty());
  UR_EXPECT_TRUE(OAuthReturnProvider("").empty());
  // and the nonce check accepts only a token minted for this attempt
  const Return good = ParseGoogleReturn("state=s1&id_token=" +
                                        urnw::sso::SimulatedIdentityToken("n1", "accounts.google.com"));
  UR_EXPECT_TRUE(CheckReturn(good, "google", "s1", "n1").ok);
  UR_EXPECT_TRUE(!CheckReturn(good, "google", "s1", "n2").ok);
  UR_EXPECT_TRUE(!CheckReturn(good, "apple", "s1", "n1").ok);
}
