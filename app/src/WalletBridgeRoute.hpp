// Which waiting flow a wallet-bridge return belongs to. The bridge is a pair of
// process-wide callbacks with no request id, so a return nobody waits for (the
// bridge page's "Return to URnetwork" after its automatic redirect, a tab from
// an abandoned or superseded attempt) is dropped, never turned into a sign-in.
//
// The twin of the windows app's WalletBridgeRoute.h: the same routes, plus the
// create-network signature (a wallet sign-in that found no network finishes by
// signing a second challenge), and without the Seeker signature request or the
// bare connect the Solana payout wallet took its key with (removed with USDC
// payouts on 2026-11-13). Header-only and free of GTK and the SDK
// (tests/WalletBridgeRouteTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace urnw::bridge {

// every reason a superseded wallet flow is answered with starts with this
inline constexpr std::string_view kSupersededPrefix = "superseded by ";

constexpr bool IsSuperseded(std::string_view error) noexcept {
  return error.substr(0, kSupersededPrefix.size()) == kSupersededPrefix;
}

// A connect return (urnetwork://<phantom|solflare>-connect) carries the
// wallet's public key. A wallet being added as a sign-in method (Account >
// Login methods, AddSignInFlow.hpp) chains its challenge like a sign-in, but
// its signature is answered to the sheet and never signs in.
enum class PublicKeyRoute { Drop, SignIn, AddSignIn };

constexpr PublicKeyRoute RoutePublicKey(bool bittensor, bool walletSignInWaiting,
                                        bool walletAddWaiting = false) noexcept {
  if (bittensor) return PublicKeyRoute::Drop;  // no connect hop to chain
  if (walletAddWaiting) return PublicKeyRoute::AddSignIn;
  return walletSignInWaiting ? PublicKeyRoute::SignIn : PublicKeyRoute::Drop;
}

// A signature return carries the wallet's signature over the last message the
// app sent it.
// An add waiting comes before a sign-in: its signature goes to add-auth on the
// signed-in network, never to authLogin.
enum class SignatureRoute { Drop, AnswerRequest, AnswerAdd, FinishCreate, SignIn };

constexpr SignatureRoute RouteSignature(bool signWaiting, bool createWaiting,
                                        bool walletSignInWaiting,
                                        bool walletAddWaiting = false) noexcept {
  if (signWaiting) return SignatureRoute::AnswerRequest;
  if (walletAddWaiting) return SignatureRoute::AnswerAdd;
  if (createWaiting) return SignatureRoute::FinishCreate;
  return walletSignInWaiting ? SignatureRoute::SignIn : SignatureRoute::Drop;
}

// The newest wallet flow owns the bridge: its keypair and its browser tab.
// SdkHost numbers every wallet flow it starts (under its lock), and a step that
// runs later -- a challenge that arrives after its fetch -- opens the bridge
// only while its flow is still the newest. A Bittensor connect superseded by a
// Solana wallet being added as a sign-in method while its challenge was being
// fetched therefore opens no tab, and leaves the Solana keypair (and so its
// return) intact.
class FlowCounter {
 public:
  constexpr uint64_t Begin() noexcept { return ++latest_; }
  constexpr uint64_t Latest() const noexcept { return latest_; }
  constexpr bool IsCurrent(uint64_t flow) const noexcept { return flow != 0 && flow == latest_; }

 private:
  uint64_t latest_ = 0;
};

}  // namespace urnw::bridge
