// Wallet connect via the ur.io/wallet-connect browser bridge — the desktop
// equivalent of the macOS ConnectWalletProviderViewModel. Desktop wallets are
// browser extensions, not URL-scheme apps, so we open
// https://ur.io/wallet-connect?... in the browser; it drives the extension and
// returns via the urnetwork:// scheme.
//
// Solana (Phantom / Solflare) is a TWO-hop flow — connect, then signMessage —
// and carries the SAME NaCl-box envelope the SDK decodes. Crypto is the SDK's
// (generateWalletKeyPair / generateSharedSecret / encrypt/decryptData / base58)
// so it's wire-compatible with Apple's CryptoKit path.
//
// Bittensor runs through the SDK's BittensorWalletSession (sdk
// bittensor_wallet.go), one session per challenge: the session builds the
// bridge url (Talisman, TAO.com: the page uses only that wallet's extension)
// and judges every bridge return, whether a wallet signed it or the user pasted
// it on the page, against the challenge it issued (message, purpose, typed
// address, ss58, signature shape, expiry). Only an accepted proof reaches
// on_signature; what the outcomes mean for the flow is BittensorWalletFlow.hpp.
// sr25519 signatures are public, so there is no envelope and no keypair.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <urnetwork_sdk.hpp>

namespace urnw {

class WalletConnect {
 public:
  enum class Provider { Phantom, Solflare, Bittensor };

  // Solana: open the browser to connect the wallet. on_public_key fires on the
  // urnetwork://<provider>-connect callback.
  void Connect(Provider p);

  // Solana: after a successful Connect, ask the wallet to sign `message`.
  // on_signature fires (base64) on the urnetwork://<provider>-sign-message callback.
  void SignMessage(const std::string& message);

  // Bittensor: hand the wallet the session (its challenge already set): this
  // opens session->bridgeUrl(), and a session that has none (any other transport)
  // answers on_error. Results arrive on on_signature with publicKey() and
  // message() taken from the accepted proof.
  void SignWithBittensor(std::shared_ptr<urnet::BittensorWalletSession> session);

  // The session in flight, if any (the host cancels it with its flow).
  std::shared_ptr<urnet::BittensorWalletSession> bittensorSession() const {
    return bittensorSession_;
  }

  // Sign in with Apple straight against Apple (SsoBridge.hpp, "Sign in with
  // Apple"): the browser opens Apple's authorize page, the api's callback
  // redirects to urnetwork://oauth/apple, and on_sso fires with the RAW return
  // (provider "apple", the id token, the echoed state, error); the host checks
  // the state and the token's nonce against the attempt it minted before
  // anything reaches the api.
  void SignInWithApple(const std::string& apiUrl, const std::string& state,
                       const std::string& nonce);
  // Sign in with Google straight against Google (SsoBridge.hpp, "Sign in with
  // Google"): the browser opens Google's authorize page (code flow), the api's
  // callback exchanges the code and redirects to urnetwork://oauth/google, and
  // on_sso fires with the raw return (provider "google", the id token, the
  // echoed state).
  void SignInWithGoogle(const std::string& apiUrl, const std::string& state,
                        const std::string& nonce);
  std::function<void(std::string provider, std::string authJwt, std::string state,
                     std::string error)>
      on_sso;

  // Route a urnetwork:// callback here. Returns true if it was a wallet or
  // sso callback.
  bool HandleDeepLink(const std::string& url);

  bool connected() const { return connectedPublicKey_.has_value(); }
  const std::string& message() const { return lastMessage_; }
  // The wallet address from the last callback: a base58 public key (solana) or
  // an ss58 address (bittensor).
  std::string publicKey() const { return connectedPublicKey_.value_or(std::string()); }
  Provider provider() const { return currentProvider_; }

  std::function<void(std::string publicKey, Provider)> on_public_key;
  // base64 (solana, NaCl envelope) or 0x-prefixed hex (bittensor, plain params)
  std::function<void(std::string signature)> on_signature;
  std::function<void(std::string error)> on_error;

 private:
  static const char* Host(Provider p);  // "phantom" | "solflare" | "bittensor"
  static std::optional<Provider> ProviderForHost(const std::string& host);
  bool NewKeyPair();
  std::optional<std::string> SharedSecretBase58() const;  // with walletEncryptionPublicKey_
  void OpenUrl(const std::string& url);
  void HandleConnect(Provider p, const std::string& query);
  void HandleSignMessage(Provider p, const std::string& query);
  // urnetwork://bittensor-sign-message?...: judged by the session
  void HandleBittensorReturn(const std::string& url);
  // an accepted proof -> on_signature; a refusal for this flow -> on_error;
  // an answer that is not this flow's -> dropped.
  void DeliverBittensorResult(const urnet::BittensorWalletResult& result);
  // urnetwork://oauth/<apple|google>?state=…&id_token=… (or &error=…)
  void HandleOAuthReturn(const std::string& url);

  std::optional<urnet::WalletKeyPair> dappKeyPair_;
  std::optional<std::string> connectedPublicKey_;
  std::optional<std::string> walletEncryptionPublicKey_;
  std::optional<std::string> session_;
  Provider currentProvider_ = Provider::Phantom;
  std::string lastMessage_;
  std::shared_ptr<urnet::BittensorWalletSession> bittensorSession_;
};

}  // namespace urnw
