// The Account > Login methods "Add sign-in method" sheet: which methods it
// offers, the add-auth request each one sends, and who owns a browser return
// while an add is in flight. Header-only and free of GTK and the SDK
// (tests/AddSignInFlowTest.cpp).
//
// Every app offers the same set (ur.io AddSignInSheet, apple AddAuthSheet), in
// the same order: Apple, Google, Microsoft, Wallet (a Solana or a Bittensor
// wallet), and an email or phone with a password. The linux sheet offered only
// the email.
//
// How each one completes, reusing the login flows (SdkHost):
//   - Apple, Google: the provider's own web flow in the browser (SsoBridge.hpp)
//     returning on urnetwork://oauth/<provider>; Microsoft: the sdk's
//     MicrosoftSignInAttempt through the api's start url, returning on
//     urnetwork://oauth/microsoft. The identity token goes to
//     POST /auth/add-auth { auth_jwt, auth_jwt_type }.
//   - Solana: the ur.io wallet bridge (connect, then sign a fresh
//     /auth/wallet-challenge); Bittensor: the SDK BittensorWalletSession with
//     purpose "add" (a login session refuses its return). Either goes to
//     add-auth { wallet_auth }.
//   - Email or phone: add-auth { user_auth, password }, then a code
//     (GuestConversion), counted as added only once verified.
//
// Adding never signs in: add-auth answers no jwt, so the session's jwt is
// never replaced and the app never becomes the added identity. A browser
// return started by the sheet is answered to the sheet (Owner::AddSignIn) and
// never reaches the login's authLogin, and a login's return never reaches the
// sheet.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace urnw::addsignin {

enum class Method { Apple, Google, Microsoft, Wallet, Email };
enum class WalletChain { Solana, Bittensor };

// The sheet's methods, in picker order (ur.io ADD_SIGN_IN_METHODS).
inline std::vector<Method> Methods() {
  return {Method::Apple, Method::Google, Method::Microsoft, Method::Wallet, Method::Email};
}

// The wallet option's chains (ur.io ADD_SIGN_IN_WALLET_CHAINS).
inline std::vector<WalletChain> WalletChains() {
  return {WalletChain::Solana, WalletChain::Bittensor};
}

// The method the sheet opens on (the apple sheet's default).
inline constexpr Method kDefaultMethod = Method::Email;

// A store key and its English (the gettext msgid).
struct Text {
  std::string_view key;
  std::string_view english;
};

constexpr Text MethodLabel(Method method) noexcept {
  switch (method) {
    case Method::Apple: return {"apple", "Apple"};
    case Method::Google: return {"google", "Google"};
    case Method::Microsoft: return {"microsoft", "Microsoft"};
    case Method::Wallet: return {"wallet", "Wallet"};
    case Method::Email: return {"site_app_email", "Email"};
  }
  return {"", ""};
}

constexpr Text ChainLabel(WalletChain chain) noexcept {
  return chain == WalletChain::Solana ? Text{"solana_wallet", "Solana wallet"}
                                      : Text{"bittensor_wallet", "Bittensor wallet"};
}

constexpr Text ChainHint(WalletChain chain) noexcept {
  return chain == WalletChain::Solana
             ? Text{"connect_solana_wallet_to_add_sign_in_method",
                    "Connect a Solana wallet to add it as a sign-in method."}
             : Text{"connect_bittensor_wallet_to_add_sign_in_method",
                    "Connect a Bittensor wallet to add it as a sign-in method."};
}

// The line shown once a method is added (ur.io addedMessageKey).
constexpr Text AddedMessage(Method method) noexcept {
  switch (method) {
    case Method::Apple: return {"apple_sign_in_method_added", "Apple sign-in method added"};
    case Method::Google: return {"google_sign_in_method_added", "Google sign-in method added"};
    case Method::Microsoft:
      return {"microsoft_sign_in_method_added", "Microsoft sign-in method added"};
    case Method::Wallet: return {"wallet_sign_in_method_added", "Wallet sign-in method added"};
    case Method::Email:
      return {"sign_in_method_added_successfully", "Sign-in method added successfully"};
  }
  return {"", ""};
}

// Only an email or phone has a code to verify.
constexpr bool NeedsVerification(Method method) noexcept { return method == Method::Email; }

// The sso provider id a provider method signs in with ("" for the others),
// also its identity token's auth_jwt_type.
constexpr std::string_view SsoProvider(Method method) noexcept {
  if (method == Method::Apple) return "apple";
  if (method == Method::Google) return "google";
  if (method == Method::Microsoft) return "microsoft";
  return "";
}

// The wallet_auth blockchain of a chain (the SDK's urnet::TAO for Bittensor).
constexpr std::string_view Blockchain(WalletChain chain) noexcept {
  return chain == WalletChain::Solana ? "solana" : "TAO";
}

// The POST /auth/add-auth body, field for field (sdk AddAuthArgs; empty =
// absent). SdkHost copies it into urnet::AddAuthArgs.
struct Args {
  std::string userAuth;
  std::string password;
  std::string authJwt;
  std::string authJwtType;
  std::string walletAddress;
  std::string walletSignature;
  std::string walletMessage;
  std::string blockchain;
};

inline Args EmailArgs(const std::string& userAuth, const std::string& password) {
  Args args;
  args.userAuth = userAuth;
  args.password = password;
  return args;
}

inline Args ProviderArgs(Method method, const std::string& identityToken) {
  Args args;
  args.authJwt = identityToken;
  args.authJwtType = std::string(SsoProvider(method));
  return args;
}

inline Args WalletArgs(WalletChain chain, const std::string& address,
                       const std::string& signature, const std::string& message) {
  Args args;
  args.walletAddress = address;
  args.walletSignature = signature;
  args.walletMessage = message;
  args.blockchain = std::string(Blockchain(chain));
  return args;
}

// Whether `args` name a method the server's AddAuth takes (server
// model/network_user_model.go AddAuth): an email or phone with a password, an
// sso token with its type, or a wallet signature. Anything else is answered
// "no auth method supplied".
inline bool SuppliesMethod(const Args& args) {
  if (!args.userAuth.empty() && !args.password.empty()) return true;
  if (!args.authJwt.empty() && !args.authJwtType.empty()) return true;
  return !args.walletAddress.empty() && !args.walletSignature.empty() &&
         !args.walletMessage.empty() && !args.blockchain.empty();
}

// Who started the browser attempt a return belongs to.
enum class Owner { None, SignIn, AddSignIn };

// What a verified sso return (its state and nonce matched the attempt) does.
enum class SsoRoute { Drop, SignIn, AddSignIn };

constexpr SsoRoute RouteSso(Owner owner) noexcept {
  switch (owner) {
    case Owner::SignIn: return SsoRoute::SignIn;
    case Owner::AddSignIn: return SsoRoute::AddSignIn;
    case Owner::None: return SsoRoute::Drop;
  }
  return SsoRoute::Drop;
}

}  // namespace urnw::addsignin
