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
// Bittensor is ONE hop: the bridge connects an injected substrate wallet
// (Bittensor Wallet / SubWallet / Talisman / polkadot-js — or a wallet app
// paired over WalletConnect when a project id is configured, see Config.hpp)
// and signs in the same page load. sr25519 signatures are public, so there is
// no encryption envelope and no keypair: the return is PLAIN query params
// (?address=<ss58>&signature=<0xhex>). The signature is verified server side
// against blockchain "TAO". See apple/BITTENSOR.md + apple/NEXTSTEPS2.md.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
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

  // Bittensor: open the browser to sign `message` in one hop (no connect step,
  // no envelope). on_signature fires (sr25519 hex) on the
  // urnetwork://bittensor-sign-message callback, with publicKey() holding the
  // ss58 address the bridge returned alongside it.
  // `purpose` rides along to the bridge (and back in the callback) so the same
  // page can sign a sign-in challenge or a wallet-attach proof ("connect").
  void SignInWithBittensor(const std::string& message, const std::string& purpose = std::string());

  // Google / Apple through the ur.io/sso bridge (SsoBridge.hpp, the login
  // stack's two full-width providers): the browser runs the provider's own
  // sign-in and the identity token comes back on urnetwork://sso. on_sso
  // fires with the RAW return (provider, token, echoed state, error); the
  // host checks the state and the token's nonce against the attempt it
  // minted before anything reaches the api.
  void SignInWithSso(const std::string& provider, const std::string& state,
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
  void HandleBittensorSignMessage(const std::string& query);
  void HandleSso(const std::string& query);

  std::optional<urnet::WalletKeyPair> dappKeyPair_;
  std::optional<std::string> connectedPublicKey_;
  std::optional<std::string> walletEncryptionPublicKey_;
  std::optional<std::string> session_;
  Provider currentProvider_ = Provider::Phantom;
  std::string lastMessage_;
};

}  // namespace urnw
