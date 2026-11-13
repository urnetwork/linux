// The ur.io wallet bridge's failure codes for a Solana wallet (Phantom or
// Solflare), in this app's words: signing in with the wallet and adding it as
// a sign-in method open the bridge page, and WalletConnect.cpp reads the code
// a failure comes back with through BridgeErrorTextFor. The Solana payout
// wallet this header also decided for (the Earnings card, its connect sheet and
// the USDC waiting for it) was removed on 2026-11-13: USDC provider payouts
// ended on 2026-11-12.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/SolanaBridgeErrorTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::solana {

// The ur.io wallet bridge hands a failure back with a stable code (the sdk's
// urnet::SolanaWalletBridgeError*) next to its English text, on the connect or
// the sign step (WalletConnect.cpp). What this app says for a code: the store
// key and its English (the gettext msgid), with one {} for the wallet's product
// name (Phantom or Solflare) when `takesWalletName`. The codes the page shares
// with the Bittensor bridge page read in the same strings. A null key for any
// other code (the page's invalid_request and wallet_error, a code this app does
// not know, the -1 of pages before the codes): the page's own text is shown
// then.
struct BridgeErrorText {
  const char* key;
  const char* english;
  bool takesWalletName;
};

constexpr BridgeErrorText BridgeErrorTextFor(std::string_view code) noexcept {
  if (code == "extension_not_found") {
    return {"bittensor_error_extension_not_found",
            "The {} extension was not found in this browser. Install it, then try again.", true};
  }
  if (code == "no_account") {
    return {"bittensor_error_no_account",
            "Your {} wallet has no account to sign with. Add or connect an account in the wallet, "
            "then try again.",
            true};
  }
  if (code == "session_not_found") {
    return {"solana_wallet_error_session_not_found",
            "The wallet connection wasn't found in this browser. Start again to reconnect your "
            "wallet.",
            false};
  }
  if (code == "user_rejected") {
    return {"bittensor_error_user_rejected",
            "The request was declined in your wallet. Start again and approve it to continue.",
            false};
  }
  return {nullptr, nullptr, false};
}

}  // namespace urnw::solana
