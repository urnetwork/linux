// Solana wallet connect (Phantom / Solflare) via the ur.io/wallet-connect browser
// bridge — the desktop equivalent of the macOS ConnectWalletProviderViewModel.
// Desktop wallets are browser extensions, not URL-scheme apps, so we open
// https://ur.io/wallet-connect?... in the browser; it drives the extension and
// returns via the urnetwork:// scheme carrying the SAME NaCl-box envelope the SDK
// decodes. Crypto is the SDK's (generateWalletKeyPair / generateSharedSecret /
// encrypt/decryptData / base58) so it's wire-compatible with Apple's CryptoKit path.
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
  enum class Provider { Phantom, Solflare };

  // Open the browser to connect the wallet. on_public_key fires on the
  // urnetwork://<provider>-connect callback.
  void Connect(Provider p);

  // After a successful Connect, ask the wallet to sign `message`. on_signature
  // fires (base64) on the urnetwork://<provider>-sign-message callback.
  void SignMessage(const std::string& message);

  // Route a urnetwork:// callback here. Returns true if it was a wallet callback.
  bool HandleDeepLink(const std::string& url);

  bool connected() const { return connectedPublicKey_.has_value(); }
  const std::string& message() const { return lastMessage_; }

  std::function<void(std::string publicKey, Provider)> on_public_key;
  std::function<void(std::string base64Signature)> on_signature;
  std::function<void(std::string error)> on_error;

 private:
  static const char* Host(Provider p);            // "phantom" | "solflare"
  static std::optional<Provider> ProviderForHost(const std::string& host);
  bool NewKeyPair();
  std::optional<std::string> SharedSecretBase58() const;  // with walletEncryptionPublicKey_
  void OpenUrl(const std::string& url);
  void HandleConnect(Provider p, const std::string& query);
  void HandleSignMessage(Provider p, const std::string& query);

  std::optional<urnet::WalletKeyPair> dappKeyPair_;
  std::optional<std::string> connectedPublicKey_;
  std::optional<std::string> walletEncryptionPublicKey_;
  std::optional<std::string> session_;
  Provider currentProvider_ = Provider::Phantom;
  std::string lastMessage_;
};

}  // namespace urnw
